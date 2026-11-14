// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <QString>
#include <QWizard>

#include "bluez.hh"
#include "control_client.hh"

namespace vds::gui {

/// First-run and "add controller" flow: prepares the computer (service and
/// Bluetooth, one password prompt), pairs and registers a controller, then
/// waits for it to connect.
class SetupWizard : public QWizard {
public:
  /// prepare_needed starts at the page that fixes the service and Bluetooth;
  /// otherwise the wizard starts at picking a controller.
  SetupWizard(const QString &socket_path, bool prepare_needed,
              QWidget *parent);

private:
  // Declared before the pages are created so they outlive every pending
  // request that calls back into a page.
  ControlClient client_;
  Bluez bluez_;
  QString added_address_;
};

} // namespace vds::gui
