// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <optional>
#include <vector>

#include <QByteArray>
#include <QHash>
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
  /// Live state of a connected controller. No battery level means unknown.
  std::optional<unsigned> battery = std::nullopt;
  bool charging = false;
  bool headset = false;
  bool mic_muted = false;
  /// Both empty while the controller is off or has not told its firmware.
  /// For a DualSense firmware is the version Sony's updater shows, such as
  /// 0630, and firmware_build the internal build, such as 1.16.42. For a
  /// Steam Controller firmware is the build date, such as 2026-11-12, and
  /// firmware_build the build time in hex that names Valve's firmware
  /// files, such as 6AF5F23B.
  QString firmware;
  QString firmware_build;
  /// How a Steam Controller plays haptics, rumble and trackpad clicks.
  SteamTuning steam = {};
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
  /// Unset leaves the saved tuning of a Steam Controller alone.
  std::optional<SteamTuning> steam = std::nullopt;
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

struct DaemonStatus {
  QString version;
  /// Whether the kernel has the DualSense audio fix. Unset until the first
  /// controller has connected, because vdsd tests the driver then.
  std::optional<bool> kernel_fix = std::nullopt;
};

/// The controller cannot connect because this kernel's hid-playstation lacks
/// the Bluetooth audio fix.
bool kernel_fix_missing(const ControllerStatus &controller);

/// The name bluetoothd gave the controller, or one that fits its kind while
/// that is not known.
QString controller_name(const QHash<QString, QString> &names,
                        const QString &address);

/// What the user needs when kernel_fix_missing() holds.
inline const QString kKernelRequirement =
    "vDS Mango needs a kernel with the DualSense audio fix: linux-cachyos "
    "7.3.6 or newer.";

QByteArray list_request();
QByteArray list_targets_request();
/// The targets vdsd finds without asking bluetoothd: Steam Controllers on
/// their Puck. Unlike list_targets_request() it is cheap enough to poll.
/// claim_pucks takes the Pucks from other programs for about 20 s, so a
/// controller Steam holds shows up in the next reply.
QByteArray local_targets_request(bool claim_pucks);
QByteArray attach_request(const ControllerAssignment &assignment);
QByteArray detach_request(const QString &address);
QByteArray trace_status_request();
QByteArray trace_request(const QString &scope, bool enabled);
QByteArray status_request();
/// Asks vdsd to vibrate a connected controller briefly.
QByteArray buzz_request(const QString &address);

/// The parsers throw std::runtime_error carrying the daemon's own error text
/// when the reply is {"OK":false,...}, or a description of the malformed reply.
std::vector<ControllerStatus> parse_list_reply(const QByteArray &reply);
std::vector<PairedController> parse_list_targets_reply(const QByteArray &reply);
AttachResult parse_attach_reply(const QByteArray &reply);
void parse_detach_reply(const QByteArray &reply);
TraceScopes parse_trace_reply(const QByteArray &reply);
DaemonStatus parse_status_reply(const QByteArray &reply);
void parse_buzz_reply(const QByteArray &reply);

} // namespace vds::gui
