// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <exception>
#include <iostream>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QIcon>
#include <QStandardPaths>

#include "main_window.hh"
#include "single_instance.hh"
#include "vds_build_info.hh"

namespace {

constexpr const char *kDefaultControlSocket = "/run/vdsd.sock";

} // namespace

int main(int argc, char **argv) {
  try {
    QApplication app(argc, argv);
    QApplication::setApplicationName("vds-mango");
    QApplication::setApplicationVersion(vds::kVersion);
    QApplication::setDesktopFileName("vds-mango");
    QApplication::setWindowIcon(
        QIcon::fromTheme("vds-mango", QIcon(":/icons/vds-mango.svg")));
    // Plasma's native message dialog ignores the default button, so Return
    // would confirm removing a controller instead of cancelling.
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Manage the DualSense controllers bridged by vdsd.");
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption socket_option(
        "socket", "Control socket of vdsd.", "path", kDefaultControlSocket);
    parser.addOption(socket_option);
    parser.process(app);

    const QString socket_path = parser.value(socket_option);
    // The runtime directory is private to the user.
    vds::gui::SingleInstance instance(
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation),
        socket_path);
    if (!instance.start(qgetenv("XDG_ACTIVATION_TOKEN"))) {
      return 0;
    }
    vds::gui::MainWindow window(socket_path);
    instance.set_activation_handler([&window](const QByteArray &token) {
      window.bring_to_front(token);
    });
    window.show();
    return QApplication::exec();
  } catch (const std::exception &error) {
    std::cerr << "vds-mango: " << error.what() << "\n";
    return 1;
  }
}
