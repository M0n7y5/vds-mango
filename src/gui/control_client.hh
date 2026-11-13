// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <functional>

#include <QByteArray>
#include <QObject>
#include <QString>

namespace vds::gui {

class ControlClient : public QObject {
public:
  using ReplyHandler = std::function<void(const QByteArray &)>;
  using ErrorHandler = std::function<void(const QString &)>;

  explicit ControlClient(QString socket_path, QObject *parent = nullptr);

  /// Sends one request line to vdsd without blocking. Exactly one handler
  /// runs: on_reply with the whole reply, or on_error with a message fit for
  /// the user. An exception thrown by on_reply is routed to on_error.
  void send(const QByteArray &request, ReplyHandler on_reply,
            ErrorHandler on_error);

private:
  QString socket_path_;
};

} // namespace vds::gui
