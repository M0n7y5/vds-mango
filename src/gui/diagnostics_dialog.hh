// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <QDialog>
#include <QString>

#include "control_client.hh"
#include "control_protocol.hh"

class QCheckBox;
class QGroupBox;

namespace vds::gui {

/// Trace logging switches, and a summary of the setup to paste into a bug
/// report.
class DiagnosticsDialog : public QDialog {
public:
  DiagnosticsDialog(const QString &socket_path, const QString &system_info,
                    QWidget *parent);

private:
  void refresh_trace();
  void show_trace(const TraceScopes &scopes);
  void set_trace(const QString &scope, bool enabled);

  // Its own client, so no reply arrives after the dialog is gone.
  ControlClient client_;
  QGroupBox *trace_group_ = nullptr;
  QCheckBox *trace_input_control_ = nullptr;
  QCheckBox *trace_input_audio_ = nullptr;
  QCheckBox *trace_output_ = nullptr;
};

} // namespace vds::gui
