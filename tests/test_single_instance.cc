// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include "vds_check.hh"

#include <cstring>
#include <exception>
#include <optional>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include "single_instance.hh"

namespace vds::test {

namespace {

using vds::gui::SingleInstance;

constexpr int kLaunchTimeoutMs = 10000;
const QString kControlSocket = "/run/vdsd.sock";

/*
 * A later launch blocks until the running instance answers, and the running
 * instance answers from this thread's event loop, so the launch gets its own
 * thread. Empty when start() threw or did not return in time.
 */
std::optional<bool> launch_again(const QString &directory,
                                 const QString &control_socket,
                                 const QByteArray &token) {
  QEventLoop loop;
  std::optional<bool> started;
  std::thread launch([&] {
    std::optional<bool> result;
    try {
      SingleInstance later(directory, control_socket);
      result = later.start(token);
    } catch (const std::exception &) {
    }
    QMetaObject::invokeMethod(
        &loop,
        [&, result] {
          started = result;
          loop.quit();
        },
        Qt::QueuedConnection);
  });
  QTimer::singleShot(kLaunchTimeoutMs, &loop, &QEventLoop::quit);
  loop.exec();
  launch.join();
  return started;
}

std::optional<bool> start_here(SingleInstance &instance) {
  try {
    return instance.start({});
  } catch (const std::exception &error) {
    vds::test::report(__FILE__, __LINE__, error.what());
    return std::nullopt;
  }
}

// What a killed instance leaves: a socket file that nobody listens on.
void leave_dead_socket(const QString &path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const QByteArray name = QFile::encodeName(path);
  std::memcpy(address.sun_path, name.constData(), name.size());
  VDS_CHECK(::bind(fd, reinterpret_cast<const sockaddr *>(&address),
                   sizeof(address)) == 0);
  ::close(fd);
}

} // namespace

// Without the lock a second launch opens a second window on the same daemon.
void test_second_launch_hands_its_token_to_the_running_instance() {
  QTemporaryDir directory;
  SingleInstance running(directory.path(), kControlSocket);
  VDS_CHECK(start_here(running) == true);
  QByteArray received;
  running.set_activation_handler(
      [&received](const QByteArray &token) { received = token; });

  VDS_CHECK(launch_again(directory.path(), kControlSocket, "token-1") ==
            false);
  VDS_CHECK_EQ(received.toStdString(), std::string("token-1"));
}

// Screenshot tooling runs the app on a fake socket next to the real one.
void test_other_control_socket_starts_its_own_instance() {
  QTemporaryDir directory;
  SingleInstance running(directory.path(), kControlSocket);
  VDS_CHECK(start_here(running) == true);

  VDS_CHECK(launch_again(directory.path(), "/tmp/fake.sock", {}) == true);
}

// A killed instance cannot delete its socket file, and listen() refuses a
// path that exists.
void test_killed_instance_leftovers_do_not_block_a_start() {
  QTemporaryDir directory;
  QStringList leftovers;
  {
    SingleInstance killed(directory.path(), kControlSocket);
    VDS_CHECK(start_here(killed) == true);
    leftovers = QDir(directory.path()).entryList(QDir::Files | QDir::System);
  }
  int dead_sockets = 0;
  for (const QString &name : leftovers) {
    const QString path = directory.filePath(name);
    if (!QFile::exists(path)) {
      leave_dead_socket(path);
      ++dead_sockets;
    }
  }
  VDS_CHECK_EQ(dead_sockets, 1);

  SingleInstance restarted(directory.path(), kControlSocket);
  VDS_CHECK(start_here(restarted) == true);
}

void run_tests() {
  int argc = 1;
  char name[] = "test_single_instance";
  char *argv[] = {name, nullptr};
  const QCoreApplication app(argc, argv);
  test_second_launch_hands_its_token_to_the_running_instance();
  test_other_control_socket_starts_its_own_instance();
  test_killed_instance_leftovers_do_not_block_a_start();
}

} // namespace vds::test
