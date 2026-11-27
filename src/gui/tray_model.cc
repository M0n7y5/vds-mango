// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <utility>

#include "tray_model.hh"

namespace vds::gui {

namespace {

QString controller_line(const ControllerStatus &controller,
                        const QString &name) {
  if (!controller.connected) {
    return name + ": not connected";
  }
  QString line = name + ": connected";
  if (controller.battery) {
    line += ", battery " + QString::number(*controller.battery) + "%";
    if (controller.charging) {
      line += ", charging";
    }
  }
  return line;
}

} // namespace

std::vector<TrayEntry>
tray_entries(ServiceState service,
             const std::vector<ControllerStatus> &controllers,
             const std::function<QString(const QString &)> &name_of) {
  switch (service) {
  case ServiceState::Checking:
    return {{.text = "Checking the service"}};
  case ServiceState::Unreachable:
    return {{.text = "Service not running"}};
  case ServiceState::Running:
    break;
  }
  if (controllers.empty()) {
    return {{.text = "No controllers registered"}};
  }
  std::vector<TrayEntry> entries;
  for (const ControllerStatus &controller : controllers) {
    TrayEntry entry{
        .text = controller_line(controller, name_of(controller.address)),
        .address = controller.address};
    if (controller.connected) {
      entry.actions.push_back(TrayAction::Test);
    }
    entry.actions.push_back(TrayAction::Edit);
    entries.push_back(std::move(entry));
  }
  return entries;
}

bool same_tray_structure(const std::vector<TrayEntry> &before,
                         const std::vector<TrayEntry> &after) {
  return std::equal(before.begin(), before.end(), after.begin(), after.end(),
                    [](const TrayEntry &left, const TrayEntry &right) {
                      return left.address == right.address &&
                             left.actions == right.actions;
                    });
}

TrayIconState
tray_icon_state(ServiceState service,
                const std::vector<ControllerStatus> &controllers) {
  if (service == ServiceState::Unreachable) {
    return TrayIconState::Unreachable;
  }
  const bool any_connected =
      std::any_of(controllers.begin(), controllers.end(),
                  [](const ControllerStatus &c) { return c.connected; });
  return any_connected ? TrayIconState::Connected : TrayIconState::Idle;
}

LowBatteryStep low_battery_step(bool armed, const ControllerStatus &now) {
  if (!now.connected || now.charging) {
    return {.notify = false, .armed = true};
  }
  // An unknown level, such as a charging fault, says nothing either way.
  if (!now.battery) {
    return {.notify = false, .armed = armed};
  }
  if (*now.battery > kLowBatteryPercent) {
    return {.notify = false, .armed = true};
  }
  return {.notify = armed, .armed = false};
}

} // namespace vds::gui
