// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <utility>

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QMap>
#include <QStringList>
#include <QVariantMap>

#include "bluez.hh"
#include "vds_profile.hh"

namespace vds::gui {

namespace {

using InterfaceMap = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QDBusObjectPath, InterfaceMap>;

constexpr int kPairTimeoutMs = 60000;
const QString kBluezService = "org.bluez";
const QString kAdapterInterface = "org.bluez.Adapter1";
const QString kDeviceInterface = "org.bluez.Device1";

QString error_text(const QDBusError &error) {
  if (error.type() == QDBusError::ServiceUnknown) {
    return "Bluetooth is not running on this computer.";
  }
  return error.message();
}

template <class OnReply>
void call(QObject *context, const QDBusMessage &message, int timeout_ms,
          OnReply on_reply) {
  auto *watcher = new QDBusPendingCallWatcher(
      QDBusConnection::systemBus().asyncCall(message, timeout_ms), context);
  QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
                   [on_reply](QDBusPendingCallWatcher *finished) {
                     finished->deleteLater();
                     on_reply(*finished);
                   });
}

BluetoothDevice device_from(const QString &path,
                            const QVariantMap &properties) {
  const QString name = properties.value("Name").toString();
  return BluetoothDevice{
      .path = path,
      .address = properties.value("Address").toString(),
      .name = name.isEmpty() ? properties.value("Alias").toString() : name,
      .paired = properties.value("Paired").toBool(),
      .trusted = properties.value("Trusted").toBool(),
      .supported_controller =
          controller_profile_from_modalias(
              properties.value("Modalias").toString().toStdString())
              .has_value(),
  };
}

} // namespace

Bluez::Bluez(QObject *parent) : QObject(parent) {
  qDBusRegisterMetaType<InterfaceMap>();
  qDBusRegisterMetaType<ManagedObjects>();
}

Bluez::~Bluez() {
  if (discovering_) {
    QDBusConnection::systemBus().asyncCall(QDBusMessage::createMethodCall(
        kBluezService, adapter_path_, kAdapterInterface, "StopDiscovery"));
  }
}

void Bluez::list_devices(DevicesHandler on_devices,
                         ProblemHandler on_problem) {
  call(this,
       QDBusMessage::createMethodCall(kBluezService, "/",
                                      "org.freedesktop.DBus.ObjectManager",
                                      "GetManagedObjects"),
       -1, [this, on_devices, on_problem](const QDBusPendingCall &finished) {
         const QDBusPendingReply<ManagedObjects> reply = finished;
         if (reply.isError()) {
           // A bluetoothd that went away took the session with it, and a
           // second StartDiscovery to a live one is harmless.
           discovering_ = false;
           on_problem({.kind = reply.error().type() ==
                                       QDBusError::ServiceUnknown
                                   ? BluetoothProblem::Kind::NotRunning
                                   : BluetoothProblem::Kind::Failed,
                       .text = error_text(reply.error())});
           return;
         }
         std::vector<BluetoothDevice> devices;
         bool powered = false;
         adapter_path_.clear();
         const ManagedObjects objects = reply.value();
         for (auto object = objects.begin(); object != objects.end();
              ++object) {
           const InterfaceMap &interfaces = object.value();
           if (interfaces.contains(kAdapterInterface) && !powered) {
             adapter_path_ = object.key().path();
             powered =
                 interfaces.value(kAdapterInterface).value("Powered").toBool();
           }
           if (interfaces.contains(kDeviceInterface)) {
             devices.push_back(device_from(object.key().path(),
                                           interfaces.value(kDeviceInterface)));
           }
         }
         if (adapter_path_.isEmpty()) {
           discovering_ = false;
           on_problem({.kind = BluetoothProblem::Kind::NoAdapter,
                       .text = "This computer has no Bluetooth adapter."});
           return;
         }
         if (!powered) {
           // Powering off ends every discovery session the adapter had.
           discovering_ = false;
           on_problem({.kind = BluetoothProblem::Kind::SwitchedOff,
                       .text = "Bluetooth is switched off."});
           return;
         }
         on_devices(std::move(devices));
       });
}

void Bluez::start_discovery() {
  if (discovering_ || adapter_path_.isEmpty()) {
    return;
  }
  discovering_ = true;
  QDBusConnection::systemBus().asyncCall(QDBusMessage::createMethodCall(
      kBluezService, adapter_path_, kAdapterInterface, "StartDiscovery"));
}

void Bluez::pair_and_trust(const BluetoothDevice &device, DoneHandler on_done,
                           ErrorHandler on_error) {
  if (device.paired) {
    trust(device.path, std::move(on_done), std::move(on_error));
    return;
  }
  call(this,
       QDBusMessage::createMethodCall(kBluezService, device.path,
                                      kDeviceInterface, "Pair"),
       kPairTimeoutMs,
       [this, path = device.path, on_done,
        on_error](const QDBusPendingCall &finished) {
         const QDBusPendingReply<> reply = finished;
         if (reply.isError() &&
             reply.error().name() != "org.bluez.Error.AlreadyExists") {
           on_error("Pairing failed: " + error_text(reply.error()) +
                    ". Put the controller in pairing mode and try again.");
           return;
         }
         trust(path, on_done, on_error);
       });
}

void Bluez::trust(const QString &path, DoneHandler on_done,
                  ErrorHandler on_error) {
  QDBusMessage message = QDBusMessage::createMethodCall(
      kBluezService, path, "org.freedesktop.DBus.Properties", "Set");
  message << kDeviceInterface << "Trusted"
          << QVariant::fromValue(QDBusVariant(true));
  call(this, message, -1,
       [on_done, on_error](const QDBusPendingCall &finished) {
         const QDBusPendingReply<> reply = finished;
         if (reply.isError()) {
           on_error("Could not mark the controller as trusted: " +
                    error_text(reply.error()));
           return;
         }
         on_done();
       });
}

} // namespace vds::gui
