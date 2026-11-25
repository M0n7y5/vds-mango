// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <stdexcept>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>

#include "control_protocol.hh"

namespace vds::gui {

namespace {

QByteArray request_line(const QJsonObject &request) {
  return QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
}

[[noreturn]] void throw_malformed(const QString &detail) {
  throw std::runtime_error(
      ("malformed reply from vdsd: " + detail).toStdString());
}

QString require_string(const QJsonObject &object, const QString &key) {
  const QJsonValue value = object.value(key);
  if (!value.isString()) {
    throw_malformed("missing " + key);
  }
  return value.toString();
}

bool require_bool(const QJsonObject &object, const QString &key) {
  const QJsonValue value = object.value(key);
  if (!value.isBool()) {
    throw_malformed("missing " + key);
  }
  return value.toBool();
}

QJsonArray require_array(const QJsonObject &object, const QString &key) {
  const QJsonValue value = object.value(key);
  if (!value.isArray()) {
    throw_malformed("missing " + key);
  }
  return value.toArray();
}

std::vector<unsigned> require_ports(const QJsonObject &object) {
  const QJsonArray array = require_array(object, "ports");
  std::vector<unsigned> ports;
  ports.reserve(static_cast<std::size_t>(array.size()));
  for (qsizetype i = 0; i < array.size(); ++i) {
    const QJsonValue port = array.at(i);
    if (!port.isDouble() || port.toInt(-1) < 0) {
      throw_malformed("ports must hold port numbers");
    }
    ports.push_back(static_cast<unsigned>(port.toInt()));
  }
  return ports;
}

ControllerProfile require_profile(const QJsonObject &object) {
  return parse_controller_profile(
      require_string(object, "profile").toStdString());
}

std::vector<QJsonObject> reply_objects(const QByteArray &reply) {
  std::vector<QJsonObject> objects;
  const QList<QByteArray> lines = reply.split('\n');
  for (const QByteArray &line : lines) {
    if (line.trimmed().isEmpty()) {
      continue;
    }
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject()) {
      throw_malformed("line is not a JSON object");
    }
    QJsonObject object = document.object();
    const QJsonValue ok = object.value("OK");
    if (ok.isBool() && !ok.toBool()) {
      throw std::runtime_error(require_string(object, "error").toStdString());
    }
    objects.push_back(std::move(object));
  }
  return objects;
}

QJsonObject command_reply(const QByteArray &reply) {
  std::vector<QJsonObject> objects = reply_objects(reply);
  if (objects.size() != 1) {
    throw_malformed("expected exactly one line");
  }
  return std::move(objects.front());
}

} // namespace

bool kernel_fix_missing(const ControllerStatus &controller) {
  return controller.error ==
         QString::fromUtf8(kBluetoothSharingUnsupported.data(),
                           static_cast<qsizetype>(
                               kBluetoothSharingUnsupported.size()));
}

QByteArray list_request() {
  return request_line(QJsonObject{{"command", "list"}});
}

QByteArray list_targets_request() {
  return request_line(QJsonObject{{"command", "list-targets"}});
}

QByteArray attach_request(const ControllerAssignment &assignment) {
  QJsonArray ports;
  for (const unsigned port : assignment.ports) {
    ports.append(static_cast<int>(port));
  }
  QJsonObject request{
      {"command", "attach"},
      {"address", assignment.address},
      {"profile", QString::fromStdString(
                      controller_profile_name(assignment.profile))},
      {"ports", ports},
  };
  if (assignment.audio_buffer) {
    request.insert("audio_buffer", QString::number(*assignment.audio_buffer));
  }
  return request_line(request);
}

QByteArray detach_request(const QString &address) {
  return request_line(QJsonObject{{"command", "detach"}, {"address", address}});
}

QByteArray trace_status_request() {
  return request_line(
      QJsonObject{{"command", "trace"}, {"mode", "status"}, {"scope", "all"}});
}

QByteArray trace_request(const QString &scope, bool enabled) {
  return request_line(QJsonObject{
      {"command", "trace"},
      {"mode", enabled ? "on" : "off"},
      {"scope", scope},
  });
}

QByteArray status_request() {
  return request_line(QJsonObject{{"command", "status"}});
}

QByteArray buzz_request(const QString &address) {
  return request_line(QJsonObject{{"command", "buzz"}, {"address", address}});
}

std::vector<ControllerStatus> parse_list_reply(const QByteArray &reply) {
  std::vector<ControllerStatus> controllers;
  for (const QJsonObject &object : reply_objects(reply)) {
    const QString battery = require_string(object, "battery");
    controllers.push_back(ControllerStatus{
        .address = require_string(object, "address"),
        .connected = require_bool(object, "connected"),
        .path = require_string(object, "path"),
        .error = require_string(object, "error"),
        .profile = require_profile(object),
        .ports = require_ports(object),
        .audio_buffer = require_string(object, "audio_buffer").toUInt(),
        .battery = battery.isEmpty() ? std::nullopt
                                     : std::optional(battery.toUInt()),
        .charging = require_bool(object, "charging"),
        .headset = require_bool(object, "headset"),
        .mic_muted = require_bool(object, "mic_muted"),
        .firmware = require_string(object, "firmware"),
    });
  }
  return controllers;
}

std::vector<PairedController>
parse_list_targets_reply(const QByteArray &reply) {
  std::vector<PairedController> targets;
  for (const QJsonObject &object : reply_objects(reply)) {
    targets.push_back(PairedController{
        .address = require_string(object, "address"),
        .name = require_string(object, "name"),
        .online = require_bool(object, "online"),
        .trusted = require_bool(object, "trusted"),
        .registered = require_bool(object, "registered"),
    });
  }
  return targets;
}

AttachResult parse_attach_reply(const QByteArray &reply) {
  const QJsonObject object = command_reply(reply);
  return AttachResult{
      .online = require_bool(object, "online"),
      .trusted = require_bool(object, "trusted"),
  };
}

void parse_detach_reply(const QByteArray &reply) { (void)command_reply(reply); }

void parse_buzz_reply(const QByteArray &reply) { (void)command_reply(reply); }

TraceScopes parse_trace_reply(const QByteArray &reply) {
  const QJsonArray scopes = require_array(command_reply(reply), "scope");
  TraceScopes result;
  for (qsizetype i = 0; i < scopes.size(); ++i) {
    const QString scope = scopes.at(i).toString();
    if (scope == "input-audio") {
      result.input_audio = true;
    } else if (scope == "input-control") {
      result.input_control = true;
    } else if (scope == "output") {
      result.output = true;
    }
  }
  return result;
}

DaemonStatus parse_status_reply(const QByteArray &reply) {
  const QJsonObject object = command_reply(reply);
  const QString kernel_fix = require_string(object, "kernel_fix");
  return DaemonStatus{
      .version = require_string(object, "version"),
      .kernel_fix = kernel_fix == "unknown"
                        ? std::nullopt
                        : std::optional(kernel_fix == "yes"),
  };
}

} // namespace vds::gui
