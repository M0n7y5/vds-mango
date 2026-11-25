// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>

#include "vds_config.hh"

namespace vds::gui {

struct ControllerStatus {
  QString address;
  bool connected = false;
  QString path;
  QString error;
  ControllerProfile profile = ControllerProfile::Unspecified;
  std::vector<unsigned> ports;
  unsigned audio_buffer = 0;
};

struct PairedController {
  QString address;
  QString name;
  bool online = false;
  bool trusted = false;
  bool registered = false;
};

struct ControllerAssignment {
  QString address;
  ControllerProfile profile = ControllerProfile::Unspecified;
  std::vector<unsigned> ports;
  /// Unset leaves the controller's saved haptics and speaker delay alone.
  std::optional<unsigned> audio_buffer = std::nullopt;
};

struct AttachResult {
  bool online = false;
  bool trusted = false;
};

struct TraceScopes {
  bool input_audio = false;
  bool input_control = false;
  bool output = false;
};

/// The controller cannot connect because this kernel's hid-playstation lacks
/// the Bluetooth audio fix.
bool kernel_fix_missing(const ControllerStatus &controller);

/// What the user needs when kernel_fix_missing() holds.
inline const QString kKernelRequirement =
    "vDS Mango needs a kernel with the DualSense audio fix: linux-cachyos "
    "7.3.6 or newer.";

QByteArray list_request();
QByteArray list_targets_request();
QByteArray attach_request(const ControllerAssignment &assignment);
QByteArray detach_request(const QString &address);
QByteArray trace_status_request();
QByteArray trace_request(const QString &scope, bool enabled);

/// The parsers throw std::runtime_error carrying the daemon's own error text
/// when the reply is {"OK":false,...}, or a description of the malformed reply.
std::vector<ControllerStatus> parse_list_reply(const QByteArray &reply);
std::vector<PairedController> parse_list_targets_reply(const QByteArray &reply);
AttachResult parse_attach_reply(const QByteArray &reply);
void parse_detach_reply(const QByteArray &reply);
TraceScopes parse_trace_reply(const QByteArray &reply);

} // namespace vds::gui
