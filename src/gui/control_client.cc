// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <exception>
#include <utility>

#include <QLocalSocket>
#include <QTimer>

#include "control_client.hh"

namespace vds::gui {

namespace {

constexpr int kControlTimeoutMs = 5000;

QString socket_error_message(QLocalSocket::LocalSocketError error,
                             const QString &path, const QString &detail) {
  switch (error) {
  case QLocalSocket::ServerNotFoundError:
  case QLocalSocket::ConnectionRefusedError:
    return "The background service (vdsd) is not running.";
  case QLocalSocket::SocketAccessError:
    return "The background service (vdsd) refused the connection on " + path +
           ".";
  default:
    return "Cannot reach vdsd at " + path + ": " + detail;
  }
}

} // namespace

ControlClient::ControlClient(QString socket_path, QObject *parent)
    : QObject(parent), socket_path_(std::move(socket_path)) {}

void ControlClient::send(const QByteArray &request, ReplyHandler on_reply,
                         ErrorHandler on_error) {
  auto *socket = new QLocalSocket(this);
  auto *timer = new QTimer(socket);
  timer->setSingleShot(true);

  const auto finish = [socket, timer] {
    timer->stop();
    socket->disconnect();
    socket->abort();
    socket->deleteLater();
  };

  connect(socket, &QLocalSocket::connected, socket,
          [socket, request] { socket->write(request); });
  connect(socket, &QLocalSocket::disconnected, socket,
          [socket, finish, on_reply, on_error] {
            const QByteArray reply = socket->readAll();
            finish();
            try {
              on_reply(reply);
            } catch (const std::exception &error) {
              on_error(QString::fromUtf8(error.what()));
            }
          });
  connect(socket, &QLocalSocket::errorOccurred, socket,
          [socket, finish, on_error,
           path = socket_path_](QLocalSocket::LocalSocketError error) {
            // vdsd closes the connection after its reply, which Qt reports as
            // an error just before disconnected().
            if (error == QLocalSocket::PeerClosedError) {
              return;
            }
            const QString message =
                socket_error_message(error, path, socket->errorString());
            finish();
            on_error(message);
          });
  connect(timer, &QTimer::timeout, socket,
          [finish, on_error, path = socket_path_] {
            finish();
            on_error("vdsd is not responding at " + path);
          });

  timer->start(kControlTimeoutMs);
  socket->connectToServer(socket_path_);
}

} // namespace vds::gui
