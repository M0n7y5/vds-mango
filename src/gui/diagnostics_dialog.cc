// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <utility>

#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "diagnostics_dialog.hh"
#include "spacing.hh"

namespace vds::gui {

DiagnosticsDialog::DiagnosticsDialog(const QString &socket_path,
                                     const QString &system_info,
                                     QWidget *parent)
    : QDialog(parent), client_(socket_path) {
  setWindowTitle("Diagnostics");
  resize(620, 540);

  auto *info = new QPlainTextEdit(system_info, this);
  info->setReadOnly(true);
  auto *copy = new QPushButton(QIcon::fromTheme("edit-copy"),
                               "Copy system info", this);
  connect(copy, &QPushButton::clicked, this, [copy, system_info] {
    QGuiApplication::clipboard()->setText(system_info);
    copy->setText("Copied");
  });
  auto *info_group = new QGroupBox("System information", this);
  auto *info_layout = new QVBoxLayout(info_group);
  info_layout->addWidget(
      new QLabel("Paste this into a bug report. It holds no Bluetooth "
                 "addresses.",
                 this));
  info_layout->addWidget(info, 1);
  info_layout->addWidget(copy, 0, Qt::AlignLeft);

  auto *intro = new QLabel(
      "Turn tracing on, reproduce the problem, then turn it off again. "
      "Trace output goes to /var/log/vdsd.log and stops when vdsd restarts.",
      this);
  intro->setWordWrap(true);
  trace_input_control_ =
      new QCheckBox("Controller input: buttons, sticks, touchpad", this);
  trace_input_audio_ = new QCheckBox("Microphone audio", this);
  trace_output_ =
      new QCheckBox("Output: haptics, speaker, triggers, lights", this);
  const std::pair<QCheckBox *, const char *> scopes[] = {
      {trace_input_control_, "input-control"},
      {trace_input_audio_, "input-audio"},
      {trace_output_, "output"},
  };
  trace_group_ = new QGroupBox("Trace logging", this);
  auto *trace_layout = new QVBoxLayout(trace_group_);
  trace_layout->addWidget(intro);
  for (const auto &[box, scope] : scopes) {
    trace_layout->addWidget(box);
    connect(box, &QCheckBox::clicked, this,
            [this, name = QString(scope)](bool checked) {
              set_trace(name, checked);
            });
  }

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(kWindowMargin, kWindowMargin, kWindowMargin,
                             kWindowMargin);
  layout->setSpacing(kBlockSpacing);
  layout->addWidget(info_group, 1);
  layout->addWidget(trace_group_);
  layout->addWidget(buttons);

  refresh_trace();
}

void DiagnosticsDialog::refresh_trace() {
  client_.send(
      trace_status_request(),
      [this](const QByteArray &reply) { show_trace(parse_trace_reply(reply)); },
      // Without the service there is nothing to trace.
      [this](const QString &) { trace_group_->setEnabled(false); });
}

void DiagnosticsDialog::show_trace(const TraceScopes &scopes) {
  trace_input_control_->setChecked(scopes.input_control);
  trace_input_audio_->setChecked(scopes.input_audio);
  trace_output_->setChecked(scopes.output);
}

void DiagnosticsDialog::set_trace(const QString &scope, bool enabled) {
  client_.send(
      trace_request(scope, enabled),
      [this](const QByteArray &reply) { show_trace(parse_trace_reply(reply)); },
      [this](const QString &error) {
        QMessageBox::warning(this, "Trace logging", error);
        refresh_trace();
      });
}

} // namespace vds::gui
