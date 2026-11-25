// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <vector>

#include <QHash>
#include <QMainWindow>
#include <QString>
#include <QTimer>

#include "bluez.hh"
#include "control_client.hh"
#include "control_protocol.hh"

class QGridLayout;
class QLabel;
class QPushButton;
class QStackedWidget;

namespace vds::gui {

class AddControllerCard;
class ControllerCard;

class MainWindow : public QMainWindow {
public:
  explicit MainWindow(const QString &socket_path);

private:
  QWidget *build_banner();
  QWidget *build_controllers_page();
  void build_menus();
  void build_health_strip();

  void refresh_controllers();
  void refresh_status();
  void refresh_bluetooth();
  void refresh_names();
  void show_controllers(std::vector<ControllerStatus> controllers);
  void rebuild_cards();
  void update_cards();
  void show_daemon_error(const QString &error);
  void show_action_error(const QString &title, const QString &error);
  /// The three checks along the bottom, and the banner while one fails.
  void update_health();
  void offer_setup_once();
  QString name_of(const QString &address) const;
  QString system_info() const;

  void open_setup();
  void open_diagnostics();
  void edit_controller(const QString &address);
  void remove_controller(const QString &address);
  void apply_assignment(const ControllerAssignment &assignment);
  void show_about();

  QString socket_path_;
  ControlClient client_;
  Bluez bluez_;
  std::vector<ControllerStatus> controllers_;
  QHash<QString, QString> names_;
  QTimer poll_timer_;
  QString daemon_error_;
  DaemonStatus daemon_status_;
  QString bluetooth_problem_;
  bool list_pending_ = false;
  bool bluetooth_pending_ = false;
  bool daemon_known_ = false;
  bool bluetooth_known_ = false;
  bool setup_offered_ = false;

  QWidget *banner_ = nullptr;
  QLabel *banner_text_ = nullptr;
  QPushButton *banner_setup_ = nullptr;
  /// The empty state, or the cards once a controller is registered.
  QStackedWidget *controller_pages_ = nullptr;
  QWidget *card_area_ = nullptr;
  QGridLayout *card_grid_ = nullptr;
  std::vector<ControllerCard *> cards_;
  AddControllerCard *add_card_ = nullptr;
  QLabel *service_check_ = nullptr;
  QLabel *kernel_check_ = nullptr;
  QLabel *bluetooth_check_ = nullptr;
};

} // namespace vds::gui
