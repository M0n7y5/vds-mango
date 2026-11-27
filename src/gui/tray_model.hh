// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <functional>
#include <vector>

#include <QString>

#include "control_protocol.hh"

namespace vds::gui {

enum class ServiceState { Checking, Running, Unreachable };

struct TrayEntry {
  enum class Kind {
    /// A controller's state or a line about the service. Opens the window.
    Status,
    /// Sends a test buzz to address.
    Test,
  };
  Kind kind = Kind::Status;
  QString text = {};
  QString address = {};

  bool operator==(const TrayEntry &) const = default;
};

/// The entries above the fixed part of the tray menu, in menu order.
std::vector<TrayEntry>
tray_entries(ServiceState service,
             const std::vector<ControllerStatus> &controllers,
             const std::function<QString(const QString &)> &name_of);

enum class TrayIconState { Connected, Idle, Unreachable };

TrayIconState tray_icon_state(ServiceState service,
                              const std::vector<ControllerStatus> &controllers);

} // namespace vds::gui
