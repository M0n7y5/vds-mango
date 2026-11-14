// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <QObject>
#include <QProcess>

#include "setup_helper.hh"
#include "vds_build_info.hh"

namespace vds::gui {

namespace {

// pkexec exit statuses, from pkexec(1).
constexpr int kAuthorizationDismissed = 126;
constexpr int kAuthorizationFailed = 127;

} // namespace

void run_setup_helper(QObject *context, const QStringList &actions,
                      std::function<void(const QString &)> on_done) {
  auto *process = new QProcess(context);
  QObject::connect(
      process, &QProcess::finished, context,
      [process, on_done](int exit_code, QProcess::ExitStatus status) {
        const QString output =
            QString::fromUtf8(process->readAllStandardError()).trimmed();
        process->deleteLater();
        if (status == QProcess::NormalExit && exit_code == 0) {
          on_done({});
        } else if (exit_code == kAuthorizationDismissed) {
          on_done("Setup was cancelled at the password prompt.");
        } else if (exit_code == kAuthorizationFailed) {
          on_done("Authentication failed.");
        } else {
          on_done(output.isEmpty() ? "Setup failed." : output);
        }
      });
  QObject::connect(process, &QProcess::errorOccurred, context,
                   [process, on_done](QProcess::ProcessError error) {
                     if (error != QProcess::FailedToStart) {
                       return;
                     }
                     process->deleteLater();
                     on_done("Cannot ask for administrator rights: pkexec "
                             "(polkit) is not installed.");
                   });
  process->start("pkexec", QStringList{vds::kSetupHelperPath} + actions);
}

} // namespace vds::gui
