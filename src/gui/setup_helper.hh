// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <functional>

#include <QString>
#include <QStringList>

class QObject;

namespace vds::gui {

inline const QString kStartService = "start-service";
inline const QString kDisableBluetoothInput = "disable-bluetooth-input";
inline const QString kEnableBluetoothInput = "enable-bluetooth-input";

/// Runs the root setup helper through polkit, which shows the desktop's
/// password prompt. on_done receives an empty string on success, otherwise a
/// message fit for the user. It is dropped if context is destroyed first.
void run_setup_helper(QObject *context, const QStringList &actions,
                      std::function<void(const QString &)> on_done);

} // namespace vds::gui
