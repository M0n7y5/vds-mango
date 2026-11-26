// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QThread>

#include "single_instance.hh"

namespace vds::gui {

namespace {

constexpr int kStartTimeoutMs = 5000;
constexpr int kRequestTimeoutMs = 2000;
constexpr unsigned long kRetryMs = 20;
constexpr qint64 kMaxRequestBytes = 4096;

std::runtime_error system_error(const std::string &what,
                                const QString &path) {
  return std::runtime_error(what + " " + path.toStdString() + ": " +
                            std::strerror(errno));
}

} // namespace

SingleInstance::SingleInstance(const QString &directory,
                               const QString &control_socket) {
  const QString key = QString::fromLatin1(
      QCryptographicHash::hash(
          QFileInfo(control_socket).absoluteFilePath().toUtf8(),
          QCryptographicHash::Sha256)
          .toHex()
          .left(16));
  const QDir dir(directory);
  lock_path_ = dir.filePath("vds-mango-" + key + ".lock");
  server_path_ = dir.filePath("vds-mango-" + key + ".sock");
}

SingleInstance::~SingleInstance() {
  if (lock_fd_ >= 0) {
    ::close(lock_fd_);
  }
}

bool SingleInstance::start(const QByteArray &activation_token) {
  const QDeadlineTimer deadline(kStartTimeoutMs);
  for (;;) {
    if (try_lock()) {
      listen();
      return true;
    }
    if (request_activation(activation_token)) {
      return false;
    }
    if (deadline.hasExpired()) {
      throw std::runtime_error("another vds-mango holds " +
                               lock_path_.toStdString() +
                               " and does not answer");
    }
    // The holder is between taking the lock and listening, or it is exiting
    // and the lock is about to be free.
    QThread::msleep(kRetryMs);
  }
}

void SingleInstance::set_activation_handler(ActivationHandler handler) {
  on_activation_ = std::move(handler);
}

bool SingleInstance::try_lock() {
  if (lock_fd_ < 0) {
    // CLOEXEC keeps the setup helper and other children from holding the
    // lock after the app exits.
    lock_fd_ = ::open(QFile::encodeName(lock_path_).constData(),
                      O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd_ < 0) {
      throw system_error("cannot open", lock_path_);
    }
  }
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) == 0) {
    return true;
  }
  if (errno == EWOULDBLOCK) {
    return false;
  }
  throw system_error("cannot lock", lock_path_);
}

void SingleInstance::listen() {
  // Only the lock holder gets here, so a socket file is a crashed one's.
  QLocalServer::removeServer(server_path_);
  if (!server_.listen(server_path_)) {
    throw std::runtime_error("cannot listen on " +
                             server_path_.toStdString() + ": " +
                             server_.errorString().toStdString());
  }
  connect(&server_, &QLocalServer::newConnection, this, [this] {
    while (QLocalSocket *socket = server_.nextPendingConnection()) {
      connect(socket, &QLocalSocket::disconnected, socket,
              &QObject::deleteLater);
      connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
        if (!socket->canReadLine()) {
          if (socket->bytesAvailable() > kMaxRequestBytes) {
            socket->abort();
          }
          return;
        }
        const QByteArray token = socket->readLine().trimmed();
        if (on_activation_) {
          on_activation_(token);
        }
        socket->disconnectFromServer();
      });
    }
  });
}

bool SingleInstance::request_activation(const QByteArray &activation_token) {
  QLocalSocket socket;
  socket.connectToServer(server_path_);
  if (!socket.waitForConnected(kRequestTimeoutMs)) {
    return false;
  }
  socket.write(activation_token + '\n');
  if (!socket.waitForBytesWritten(kRequestTimeoutMs)) {
    return false;
  }
  // The running instance hangs up once it has handled the request.
  return socket.state() == QLocalSocket::UnconnectedState ||
         socket.waitForDisconnected(kRequestTimeoutMs);
}

} // namespace vds::gui
