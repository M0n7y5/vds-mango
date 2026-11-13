// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <exception>
#include <iostream>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QIcon>

#include "main_window.hh"
#include "vds_build_info.hh"

namespace {

constexpr const char *kDefaultControlSocket = "/run/vdsd.sock";

} // namespace

int main(int argc, char **argv) {
  try {
    QApplication app(argc, argv);
    QApplication::setApplicationName("vds-mango");
    QApplication::setApplicationDisplayName("vDS Mango");
    QApplication::setApplicationVersion(vds::kVersion);
    QApplication::setDesktopFileName("vds-mango");
    QApplication::setWindowIcon(QIcon::fromTheme("input-gaming"));
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

    vds::gui::MainWindow window(parser.value(socket_option));
    window.show();
    return QApplication::exec();
  } catch (const std::exception &error) {
    std::cerr << "vds-mango: " << error.what() << "\n";
    return 1;
  }
}
