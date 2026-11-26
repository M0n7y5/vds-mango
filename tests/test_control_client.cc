// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include "vds_check.hh"

#include <memory>

#include <QByteArray>
#include <QCoreApplication>
#include <QLocalServer>
#include <QString>
#include <QTemporaryDir>

#include "control_client.hh"

namespace vds::test {

namespace {

constexpr int kConnectTimeoutMs = 2000;

} // namespace

// A window owns its client and the state its handlers touch, so a handler
// that runs while the client is destroyed works on freed members.
void test_destroying_the_client_drops_a_request_in_flight() {
  QTemporaryDir directory;
  const QString path = directory.filePath("vdsd.sock");
  QLocalServer server;
  VDS_CHECK(server.listen(path));

  bool answered = false;
  auto client = std::make_unique<vds::gui::ControlClient>(path);
  client->send(
      "{\"command\":\"list\"}\n",
      [&answered](const QByteArray &) { answered = true; },
      [&answered](const QString &) { answered = true; });
  VDS_CHECK(server.waitForNewConnection(kConnectTimeoutMs));
  client.reset();
  VDS_CHECK(!answered);
}

void run_tests() {
  int argc = 1;
  char name[] = "test_control_client";
  char *argv[] = {name, nullptr};
  const QCoreApplication app(argc, argv);
  test_destroying_the_client_drops_a_request_in_flight();
}

} // namespace vds::test
