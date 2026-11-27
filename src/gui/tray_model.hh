// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <functional>
#include <vector>

#include <QString>

#include "control_protocol.hh"

namespace vds::gui {

/// At or below this a battery that is not charging counts as low, on the
/// card and in the tray alike.
constexpr unsigned kLowBatteryPercent = 15;

enum class ServiceState { Checking, Running, Unreachable };

enum class TrayAction { Test, Edit };

struct TrayEntry {
  /// A controller's state, or a line about the service or the empty list.
  QString text = {};
  /// Empty for a line that is not about one controller.
  QString address = {};
  /// What the controller's submenu offers, in menu order. A line without
  /// actions is a plain entry that opens the window.
  std::vector<TrayAction> actions = {};

  bool operator==(const TrayEntry &) const = default;
};

/// The entries above the fixed part of the tray menu, in menu order.
std::vector<TrayEntry>
tray_entries(ServiceState service,
             const std::vector<ControllerStatus> &controllers,
             const std::function<QString(const QString &)> &name_of);

/// Whether the menu built for before still fits after once its texts are
/// updated. A rebuild would close a submenu that is open under the pointer.
bool same_tray_structure(const std::vector<TrayEntry> &before,
                         const std::vector<TrayEntry> &after);

enum class TrayIconState { Connected, Idle, Unreachable };

TrayIconState tray_icon_state(ServiceState service,
                              const std::vector<ControllerStatus> &controllers);

struct LowBatteryStep {
  bool notify = false;
  /// Whether the next drop to a low battery notifies.
  bool armed = true;
};

/// Decides on the low-battery notice from the controller's newly polled
/// state and whether its notice was armed.
LowBatteryStep low_battery_step(bool armed, const ControllerStatus &now);

} // namespace vds::gui
