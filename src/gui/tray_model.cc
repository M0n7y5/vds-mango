// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

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
  using Kind = TrayEntry::Kind;
  switch (service) {
  case ServiceState::Checking:
    return {{.kind = Kind::Status, .text = "Checking the service"}};
  case ServiceState::Unreachable:
    return {{.kind = Kind::Status, .text = "Service not running"}};
  case ServiceState::Running:
    break;
  }
  if (controllers.empty()) {
    return {{.kind = Kind::Status, .text = "No controllers registered"}};
  }
  std::vector<TrayEntry> entries;
  for (const ControllerStatus &controller : controllers) {
    const QString name = name_of(controller.address);
    entries.push_back({.kind = Kind::Status,
                       .text = controller_line(controller, name),
                       .address = controller.address});
    if (controller.connected) {
      entries.push_back({.kind = Kind::Test,
                         .text = "Test " + name,
                         .address = controller.address});
    }
  }
  return entries;
}

} // namespace vds::gui
