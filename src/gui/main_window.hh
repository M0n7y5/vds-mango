// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QHash>
#include <QMainWindow>
#include <QString>
#include <QTimer>

#include "bluez.hh"
#include "control_client.hh"
#include "control_protocol.hh"
#include "tray_model.hh"

class QAction;
class QGridLayout;
class QLabel;
class QMenu;
class QPushButton;
class QStackedWidget;
class QSystemTrayIcon;

namespace vds::gui {

class AddControllerCard;
class ControllerCard;

class MainWindow : public QMainWindow {
public:
  explicit MainWindow(const QString &socket_path);

  /// Shows, un-minimises, raises and activates the window. On Wayland the
  /// compositor only lets it take focus with a valid xdg-activation token.
  void bring_to_front(const QByteArray &activation_token);

protected:
  /// With a tray icon the close button hides the window to the tray.
  void closeEvent(QCloseEvent *event) override;

private:
  QWidget *build_banner();
  QWidget *build_controllers_page();
  void build_menus();
  QWidget *build_health_strip();
  /// Adds the tray icon, if the desktop has a tray.
  void build_tray();
  void update_tray();
  /// The controller entries at the top of the tray menu.
  void update_tray_menu();
  void fill_tray_submenu(QMenu *submenu, const TrayEntry &entry);
  ServiceState service_state() const;

  void refresh_controllers();
  void refresh_status();
  void refresh_bluetooth();
  void refresh_names();
  void show_controllers(std::vector<ControllerStatus> controllers);
  /// Shows the tray's low-battery notice where low_battery_step() says so.
  void notify_low_batteries();
  void rebuild_cards();
  void update_cards();
  void show_daemon_error(const QString &error);
  void show_action_error(const QString &title, const QString &error);
  /// Shows text beside the checks for a few seconds.
  void show_message(const QString &text);
  /// The three checks along the bottom, and the banner while the service or
  /// kernel check fails.
  void update_health();
  void offer_setup_once();
  /// Plays the logo over the window, unless the desktop has animations
  /// turned off.
  void play_startup_animation();
  QString name_of(const QString &address) const;
  QString system_info() const;

  void open_setup();
  /// Brings the window to the front and opens a dialog with open_dialog,
  /// unless a dialog is open already.
  void open_from_tray(const std::function<void()> &open_dialog);
  void open_diagnostics();
  void test_controller(const QString &address);
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
  QTimer message_timer_;
  QString daemon_error_;
  DaemonStatus daemon_status_;
  std::optional<BluetoothProblem> bluetooth_problem_;
  bool list_pending_ = false;
  bool bluetooth_pending_ = false;
  bool daemon_known_ = false;
  bool bluetooth_known_ = false;
  bool setup_offered_ = false;
  bool starting_up_ = false;
  bool tray_hint_shown_ = false;

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
  QLabel *message_ = nullptr;
  /// Null without a tray.
  QSystemTrayIcon *tray_ = nullptr;
  QMenu *tray_menu_ = nullptr;
  /// The first action after the controller entries.
  QAction *tray_entries_end_ = nullptr;
  std::vector<TrayEntry> tray_entries_;
  std::vector<QAction *> tray_entry_actions_;
  std::optional<TrayIconState> tray_icon_state_;
  /// Missing means armed.
  QHash<QString, bool> low_battery_armed_;
};

} // namespace vds::gui
