// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <vector>

#include <QMainWindow>
#include <QString>
#include <QTimer>

#include "bluez.hh"
#include "control_client.hh"
#include "control_protocol.hh"
#include "controller_model.hh"

class QAction;
class QCheckBox;
class QLabel;
class QTabWidget;
class QTableView;

namespace vds::gui {

class MainWindow : public QMainWindow {
public:
  explicit MainWindow(const QString &socket_path);

private:
  QWidget *build_banner();
  QWidget *build_controllers_page();
  QWidget *build_diagnostics_page();
  void build_actions();

  void refresh_controllers();
  void refresh_names();
  void refresh_trace();
  void check_bluetooth();
  void show_controllers(std::vector<ControllerStatus> controllers);
  void show_trace(const TraceScopes &scopes);
  void show_daemon_error(const QString &error);
  void show_bluetooth_problem(const QString &problem);
  void show_action_error(const QString &title, const QString &error);
  void update_banner();
  void update_actions();
  void offer_setup_once();
  QString selected_address() const;

  void open_setup();
  void restore_bluetooth_input();
  void edit_controller();
  void remove_controller();
  void apply_assignment(const ControllerAssignment &assignment);
  void set_trace(const QString &scope, bool enabled);
  void show_about();

  QString socket_path_;
  ControlClient client_;
  Bluez bluez_;
  ControllerModel model_;
  QTimer poll_timer_;
  QString daemon_error_;
  QString bluetooth_problem_;
  bool list_pending_ = false;
  bool daemon_known_ = false;
  bool bluetooth_known_ = false;
  bool setup_offered_ = false;

  QWidget *banner_ = nullptr;
  QLabel *banner_text_ = nullptr;
  QTabWidget *tabs_ = nullptr;
  QTableView *table_ = nullptr;
  QLabel *empty_hint_ = nullptr;
  QAction *edit_action_ = nullptr;
  QAction *remove_action_ = nullptr;
  QCheckBox *trace_input_control_ = nullptr;
  QCheckBox *trace_input_audio_ = nullptr;
  QCheckBox *trace_output_ = nullptr;
};

} // namespace vds::gui
