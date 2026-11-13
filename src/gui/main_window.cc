// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <iterator>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

#include "controller_dialog.hh"
#include "main_window.hh"

namespace vds::gui {

namespace {

constexpr int kPollIntervalMs = 2000;
constexpr int kStatusMessageMs = 8000;

} // namespace

MainWindow::MainWindow(const QString &socket_path) : client_(socket_path) {
  setWindowTitle("Controllers");
  resize(900, 460);

  auto *warning_icon = new QLabel(this);
  warning_icon->setPixmap(
      style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(24, 24));
  banner_text_ = new QLabel(this);
  banner_text_->setWordWrap(true);
  banner_text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  banner_ = new QWidget(this);
  auto *banner_layout = new QHBoxLayout(banner_);
  banner_layout->addWidget(warning_icon);
  banner_layout->addWidget(banner_text_, 1);
  banner_->hide();

  tabs_ = new QTabWidget(this);
  tabs_->addTab(build_controllers_page(), "Controllers");
  tabs_->addTab(build_diagnostics_page(), "Diagnostics");

  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  layout->addWidget(banner_);
  layout->addWidget(tabs_, 1);
  setCentralWidget(central);

  build_actions();
  update_actions();
  statusBar();

  connect(&poll_timer_, &QTimer::timeout, this,
          [this] { refresh_controllers(); });
  poll_timer_.start(kPollIntervalMs);
  refresh_controllers();
}

QWidget *MainWindow::build_controllers_page() {
  table_ = new QTableView(this);
  table_->setModel(&model_);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setAlternatingRowColors(true);
  table_->setShowGrid(false);
  table_->verticalHeader()->hide();
  table_->horizontalHeader()->setSectionResizeMode(
      QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(
      ControllerModel::StatusColumn, QHeaderView::Stretch);
  connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
          this, [this] { update_actions(); });
  connect(table_, &QTableView::doubleClicked, this,
          [this] { edit_controller(); });

  empty_hint_ = new QLabel(
      "No controllers registered yet. Pair a DualSense over Bluetooth, then "
      "choose Add controller.",
      this);
  empty_hint_->setWordWrap(true);
  empty_hint_->hide();

  auto *page = new QWidget(this);
  auto *layout = new QVBoxLayout(page);
  layout->addWidget(table_, 1);
  layout->addWidget(empty_hint_);
  return page;
}

QWidget *MainWindow::build_diagnostics_page() {
  auto *intro = new QLabel(
      "Turn tracing on, reproduce the problem, then turn it off again. "
      "Trace output goes to /var/log/vdsd.log and stops when vdsd restarts.",
      this);
  intro->setWordWrap(true);

  trace_input_control_ =
      new QCheckBox("Controller input: buttons, sticks, touchpad", this);
  trace_input_audio_ = new QCheckBox("Microphone audio", this);
  trace_output_ =
      new QCheckBox("Output: haptics, speaker, triggers, lights", this);
  const std::pair<QCheckBox *, const char *> scopes[] = {
      {trace_input_control_, "input-control"},
      {trace_input_audio_, "input-audio"},
      {trace_output_, "output"},
  };

  auto *group = new QGroupBox("Trace logging", this);
  auto *group_layout = new QVBoxLayout(group);
  group_layout->addWidget(intro);
  for (const auto &[box, scope] : scopes) {
    group_layout->addWidget(box);
    connect(box, &QCheckBox::clicked, this,
            [this, name = QString(scope)](bool checked) {
              set_trace(name, checked);
            });
  }

  auto *page = new QWidget(this);
  auto *layout = new QVBoxLayout(page);
  layout->addWidget(group);
  layout->addStretch(1);
  return page;
}

void MainWindow::build_actions() {
  add_action_ = new QAction(QIcon::fromTheme("list-add"), "Add controller",
                            this);
  edit_action_ =
      new QAction(QIcon::fromTheme("document-edit"), "Edit", this);
  remove_action_ =
      new QAction(QIcon::fromTheme("list-remove"), "Remove", this);
  auto *refresh_action =
      new QAction(QIcon::fromTheme("view-refresh"), "Refresh", this);
  refresh_action->setShortcut(QKeySequence::Refresh);
  remove_action_->setShortcut(QKeySequence::Delete);

  connect(add_action_, &QAction::triggered, this, [this] { add_controller(); });
  connect(edit_action_, &QAction::triggered, this,
          [this] { edit_controller(); });
  connect(remove_action_, &QAction::triggered, this,
          [this] { remove_controller(); });
  connect(refresh_action, &QAction::triggered, this, [this] {
    refresh_controllers();
    refresh_names();
    refresh_trace();
  });

  auto *toolbar = addToolBar("Controllers");
  toolbar->setMovable(false);
  toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toolbar->addAction(add_action_);
  toolbar->addAction(edit_action_);
  toolbar->addAction(remove_action_);
  toolbar->addSeparator();
  toolbar->addAction(refresh_action);

  auto *help_menu = menuBar()->addMenu("Help");
  help_menu->addAction("About vDS Mango", this, [this] { show_about(); });
}

void MainWindow::refresh_controllers() {
  if (list_pending_) {
    return;
  }
  list_pending_ = true;
  client_.send(
      list_request(),
      [this](const QByteArray &reply) {
        list_pending_ = false;
        std::vector<ControllerStatus> controllers = parse_list_reply(reply);
        show_daemon_error({});
        show_controllers(std::move(controllers));
      },
      [this](const QString &error) {
        list_pending_ = false;
        show_daemon_error(error);
      });
}

void MainWindow::refresh_names() {
  client_.send(
      list_targets_request(),
      [this](const QByteArray &reply) {
        QHash<QString, QString> names;
        for (const PairedController &target : parse_list_targets_reply(reply)) {
          names.insert(target.address, target.name);
        }
        model_.set_names(std::move(names));
      },
      [](const QString &) {});
}

void MainWindow::refresh_trace() {
  client_.send(
      trace_status_request(),
      [this](const QByteArray &reply) { show_trace(parse_trace_reply(reply)); },
      [](const QString &) {});
}

void MainWindow::show_controllers(std::vector<ControllerStatus> controllers) {
  const QString selected = selected_address();
  model_.set_controllers(std::move(controllers));
  if (!selected.isEmpty() && selected_address().isEmpty()) {
    const int row = model_.row_of(selected);
    if (row >= 0) {
      table_->selectRow(row);
    }
  }
  empty_hint_->setVisible(model_.rowCount() == 0);
  update_actions();
}

void MainWindow::show_trace(const TraceScopes &scopes) {
  trace_input_control_->setChecked(scopes.input_control);
  trace_input_audio_->setChecked(scopes.input_audio);
  trace_output_->setChecked(scopes.output);
}

void MainWindow::show_daemon_error(const QString &error) {
  const bool reachable = error.isEmpty();
  banner_text_->setText(error);
  banner_->setVisible(!reachable);
  tabs_->setEnabled(reachable);
  if (!reachable) {
    empty_hint_->hide();
  }
  if (reachable && !daemon_reachable_) {
    daemon_reachable_ = true;
    refresh_names();
    refresh_trace();
  }
  daemon_reachable_ = reachable;
  update_actions();
}

void MainWindow::show_action_error(const QString &title, const QString &error) {
  QMessageBox::warning(this, title, error);
}

void MainWindow::update_actions() {
  const bool selected = !selected_address().isEmpty();
  add_action_->setEnabled(daemon_reachable_);
  edit_action_->setEnabled(daemon_reachable_ && selected);
  remove_action_->setEnabled(daemon_reachable_ && selected);
}

QString MainWindow::selected_address() const {
  const QModelIndexList rows = table_->selectionModel()->selectedRows();
  return rows.isEmpty() ? QString()
                        : model_.controller(rows.first().row()).address;
}

void MainWindow::add_controller() {
  client_.send(
      list_targets_request(),
      [this](const QByteArray &reply) {
        const std::vector<PairedController> targets =
            parse_list_targets_reply(reply);
        std::vector<PairedController> candidates;
        std::copy_if(targets.begin(), targets.end(),
                     std::back_inserter(candidates),
                     [](const PairedController &target) {
                       return !target.registered;
                     });
        if (candidates.empty()) {
          QMessageBox::information(
              this, "Add controller",
              "There is no paired controller left to add.\n\nPair and trust "
              "a DualSense first: hold Create and PS until the light bar "
              "blinks rapidly, then pair it in your Bluetooth settings or "
              "with bluetoothctl.");
          return;
        }
        ControllerDialog dialog(std::move(candidates), this);
        if (dialog.exec() == QDialog::Accepted) {
          apply_assignment(dialog.assignment());
        }
      },
      [this](const QString &error) {
        show_action_error("Add controller", error);
      });
}

void MainWindow::edit_controller() {
  const int row = model_.row_of(selected_address());
  if (row < 0 || !daemon_reachable_) {
    return;
  }
  const ControllerStatus controller = model_.controller(row);
  ControllerDialog dialog(controller, model_.name(controller.address), this);
  if (dialog.exec() == QDialog::Accepted) {
    apply_assignment(dialog.assignment());
  }
}

void MainWindow::remove_controller() {
  const QString address = selected_address();
  if (address.isEmpty()) {
    return;
  }
  QMessageBox confirm(
      QMessageBox::Question, "Remove controller",
      "Remove " + model_.name(address) + " (" + address +
          ")?\n\nIt disconnects from the virtual USB port and stays paired "
          "in Bluetooth.",
      QMessageBox::Cancel, this);
  const QPushButton *remove_button =
      confirm.addButton("Remove", QMessageBox::DestructiveRole);
  confirm.setDefaultButton(QMessageBox::Cancel);
  confirm.exec();
  if (confirm.clickedButton() != remove_button) {
    return;
  }
  client_.send(
      detach_request(address),
      [this, address](const QByteArray &reply) {
        parse_detach_reply(reply);
        statusBar()->showMessage("Removed " + address + ".", kStatusMessageMs);
        refresh_controllers();
      },
      [this](const QString &error) {
        show_action_error("Remove controller", error);
      });
}

void MainWindow::apply_assignment(const ControllerAssignment &assignment) {
  client_.send(
      attach_request(assignment),
      [this, address = assignment.address](const QByteArray &reply) {
        const AttachResult result = parse_attach_reply(reply);
        refresh_controllers();
        refresh_names();
        if (!result.trusted) {
          QMessageBox::warning(
              this, "Controller is not trusted",
              "Saved, but " + address +
                  " is not trusted in BlueZ and will not reconnect on its "
                  "own. Run \"trust " +
                  address + "\" in bluetoothctl.");
          return;
        }
        statusBar()->showMessage(
            result.online
                ? "Saved " + address + "."
                : "Saved " + address +
                      ". Press the PS button on the controller to connect.",
            kStatusMessageMs);
      },
      [this](const QString &error) {
        show_action_error("Save controller", error);
      });
}

void MainWindow::set_trace(const QString &scope, bool enabled) {
  client_.send(
      trace_request(scope, enabled),
      [this](const QByteArray &reply) { show_trace(parse_trace_reply(reply)); },
      [this](const QString &error) {
        show_action_error("Trace logging", error);
        refresh_trace();
      });
}

void MainWindow::show_about() {
  QMessageBox::about(
      this, "About vDS Mango",
      "<b>vDS Mango</b> " + QApplication::applicationVersion() +
          "<p>Bridges DualSense and DualSense Edge controllers from Bluetooth "
          "to a virtual USB device.</p>"
          "<p>A Linux-only fork of "
          "<a href=\"https://github.com/hurryman2212/vds\">vDS</a> by Jihong "
          "Min. MIT license.</p>");
}

} // namespace vds::gui
