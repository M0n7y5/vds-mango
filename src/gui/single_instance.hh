// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <functional>

#include <QByteArray>
#include <QLocalServer>
#include <QObject>
#include <QString>

namespace vds::gui {

/// Keeps one app per user and control socket. The first process holds an
/// flock and listens on a local socket. A later launch passes its activation
/// request to it and exits.
class SingleInstance : public QObject {
public:
  /// Runs in the running instance once per later launch, with the
  /// xdg-activation token that launch was given, possibly empty.
  using ActivationHandler = std::function<void(const QByteArray &token)>;

  /// directory must be private to the user, like the runtime directory.
  SingleInstance(const QString &directory, const QString &control_socket);
  ~SingleInstance() override;

  /// Returns true when this process became the running instance, and false
  /// once the running one has taken the activation request. Blocks for at
  /// most a few seconds and throws when neither happens.
  bool start(const QByteArray &activation_token);
  void set_activation_handler(ActivationHandler handler);

private:
  bool try_lock();
  void listen();
  bool request_activation(const QByteArray &activation_token);

  QString lock_path_;
  QString server_path_;
  int lock_fd_ = -1;
  QLocalServer server_;
  ActivationHandler on_activation_;
};

} // namespace vds::gui
