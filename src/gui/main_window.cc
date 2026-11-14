// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <utility>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
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
#include <QRadioButton>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

#include "controller_dialog.hh"
#include "main_window.hh"
#include "setup_helper.hh"
#include "setup_wizard.hh"

namespace vds::gui {

namespace {

constexpr int kPollIntervalMs = 2000;
constexpr int kStatusMessageMs = 8000;

} // namespace

MainWindow::MainWindow(const QString &socket_path)
    : socket_path_(socket_path), client_(socket_path) {
  setWindowTitle("vDS Mango");
  resize(900, 460);

  tabs_ = new QTabWidget(this);
  tabs_->addTab(build_controllers_page(), "Controllers");
  tabs_->addTab(build_diagnostics_page(), "Diagnostics");
  tabs_->addTab(build_advanced_page(), "Advanced");

  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  layout->addWidget(build_banner());
  layout->addWidget(tabs_, 1);
  setCentralWidget(central);

  build_menus();
  update_actions();
  statusBar();

  connect(&poll_timer_, &QTimer::timeout, this,
          [this] { refresh_controllers(); });
  poll_timer_.start(kPollIntervalMs);
  refresh_controllers();
  check_bluetooth();
}

QWidget *MainWindow::build_banner() {
  auto *warning_icon = new QLabel(this);
  warning_icon->setPixmap(
      style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(24, 24));
  banner_text_ = new QLabel(this);
  banner_text_->setWordWrap(true);
  auto *setup_button = new QPushButton("Set up", this);
  connect(setup_button, &QPushButton::clicked, this, [this] { open_setup(); });

  banner_ = new QWidget(this);
  auto *layout = new QHBoxLayout(banner_);
  layout->addWidget(warning_icon);
  layout->addWidget(banner_text_, 1);
  layout->addWidget(setup_button);
  banner_->hide();
  return banner_;
}

QWidget *MainWindow::build_controllers_page() {
  auto *add_action =
      new QAction(QIcon::fromTheme("list-add"), "Add controller", this);
  edit_action_ = new QAction(QIcon::fromTheme("document-edit"), "Edit", this);
  remove_action_ = new QAction(QIcon::fromTheme("list-remove"), "Remove", this);
  auto *refresh_action =
      new QAction(QIcon::fromTheme("view-refresh"), "Refresh", this);
  refresh_action->setShortcut(QKeySequence::Refresh);
  remove_action_->setShortcut(QKeySequence::Delete);

  connect(add_action, &QAction::triggered, this, [this] { open_setup(); });
  connect(edit_action_, &QAction::triggered, this,
          [this] { edit_controller(); });
  connect(remove_action_, &QAction::triggered, this,
          [this] { remove_controller(); });
  connect(refresh_action, &QAction::triggered, this, [this] {
    refresh_controllers();
    refresh_names();
    refresh_trace();
    refresh_audio_buffer();
    check_bluetooth();
  });

  auto *toolbar = new QToolBar(this);
  toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toolbar->addAction(add_action);
  toolbar->addAction(edit_action_);
  toolbar->addAction(remove_action_);
  toolbar->addSeparator();
  toolbar->addAction(refresh_action);

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
      "No controllers yet. Choose Add controller to pair and add a DualSense.",
      this);
  empty_hint_->setWordWrap(true);
  empty_hint_->hide();

  auto *page = new QWidget(this);
  auto *layout = new QVBoxLayout(page);
  layout->addWidget(toolbar);
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

QWidget *MainWindow::build_advanced_page() {
  auto *intro = new QLabel(
      "How much audio the controller queues before it plays haptics and "
      "speaker sound. Lower values react sooner, but too little makes the "
      "sound crackle, especially farther from the computer. A change applies "
      "at once and goes back to Standard when vdsd restarts. The delays were "
      "measured on one DualSense lying next to the computer.",
      this);
  intro->setWordWrap(true);

  const std::pair<unsigned, const char *> choices[] = {
      {64, "Standard: about 78 ms, safest"},
      {32, "Reduced: about 50 ms"},
      {24, "Low: about 45 ms"},
      {16, "Lowest: about 33 ms, the speaker may crackle"},
  };

  auto *group = new QGroupBox("Haptics and speaker delay", this);
  auto *group_layout = new QVBoxLayout(group);
  group_layout->addWidget(intro);
  audio_buffer_choices_ = new QButtonGroup(this);
  for (const auto &[length, label] : choices) {
    auto *choice = new QRadioButton(label, this);
    audio_buffer_choices_->addButton(choice, static_cast<int>(length));
    group_layout->addWidget(choice);
  }
  connect(audio_buffer_choices_, &QButtonGroup::idClicked, this,
          [this](int length) {
            client_.send(
                audio_buffer_request(static_cast<unsigned>(length)),
                [this](const QByteArray &reply) {
                  show_audio_buffer(parse_audio_buffer_reply(reply));
                },
                [this](const QString &error) {
                  show_action_error("Haptics and speaker delay", error);
                  refresh_audio_buffer();
                });
          });

  auto *page = new QWidget(this);
  auto *layout = new QVBoxLayout(page);
  layout->addWidget(group);
  layout->addStretch(1);
  return page;
}

void MainWindow::build_menus() {
  auto *setup_menu = menuBar()->addMenu("Setup");
  setup_menu->addAction("Run setup…", this, [this] { open_setup(); });
  setup_menu->addAction("Restore Bluetooth input…", this,
                        [this] { restore_bluetooth_input(); });
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
        show_controllers(parse_list_reply(reply));
        show_daemon_error({});
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

void MainWindow::refresh_audio_buffer() {
  client_.send(
      audio_buffer_status_request(),
      [this](const QByteArray &reply) {
        show_audio_buffer(parse_audio_buffer_reply(reply));
      },
      [](const QString &) {});
}

void MainWindow::check_bluetooth() {
  bluez_.query_input_plugin(
      [this](bool enabled) {
        show_bluetooth_problem(
            enabled ? "Bluetooth still connects controllers as plain "
                      "gamepads, so they cannot reach vDS Mango."
                    : QString());
      },
      [this](const QString &error) { show_bluetooth_problem(error); });
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
}

void MainWindow::show_trace(const TraceScopes &scopes) {
  trace_input_control_->setChecked(scopes.input_control);
  trace_input_audio_->setChecked(scopes.input_audio);
  trace_output_->setChecked(scopes.output);
}

void MainWindow::show_audio_buffer(unsigned length) {
  if (QAbstractButton *choice =
          audio_buffer_choices_->button(static_cast<int>(length))) {
    choice->setChecked(true);
  }
}

void MainWindow::show_daemon_error(const QString &error) {
  const bool became_reachable =
      error.isEmpty() && (!daemon_known_ || !daemon_error_.isEmpty());
  daemon_known_ = true;
  daemon_error_ = error;
  tabs_->setEnabled(error.isEmpty());
  if (!error.isEmpty()) {
    empty_hint_->hide();
  }
  if (became_reachable) {
    refresh_names();
    refresh_trace();
    refresh_audio_buffer();
  }
  update_banner();
  update_actions();
  offer_setup_once();
}

void MainWindow::show_bluetooth_problem(const QString &problem) {
  bluetooth_known_ = true;
  bluetooth_problem_ = problem;
  update_banner();
  offer_setup_once();
}

void MainWindow::show_action_error(const QString &title, const QString &error) {
  QMessageBox::warning(this, title, error);
}

void MainWindow::update_banner() {
  const QString &text =
      daemon_error_.isEmpty() ? bluetooth_problem_ : daemon_error_;
  banner_text_->setText(text);
  banner_->setVisible(!text.isEmpty());
}

void MainWindow::update_actions() {
  const bool editable = daemon_error_.isEmpty() && !selected_address().isEmpty();
  edit_action_->setEnabled(editable);
  remove_action_->setEnabled(editable);
}

void MainWindow::offer_setup_once() {
  if (setup_offered_ || !daemon_known_ || !bluetooth_known_) {
    return;
  }
  setup_offered_ = true;
  if (!daemon_error_.isEmpty() || !bluetooth_problem_.isEmpty() ||
      model_.rowCount() == 0) {
    // Queued so the dialog's event loop does not run inside a reply handler.
    QTimer::singleShot(0, this, [this] { open_setup(); });
  }
}

QString MainWindow::selected_address() const {
  const QModelIndexList rows = table_->selectionModel()->selectedRows();
  return rows.isEmpty() ? QString()
                        : model_.controller(rows.first().row()).address;
}

void MainWindow::open_setup() {
  SetupWizard wizard(
      socket_path_, !daemon_error_.isEmpty() || !bluetooth_problem_.isEmpty(),
      this);
  wizard.exec();
  refresh_controllers();
  refresh_names();
  check_bluetooth();
}

void MainWindow::restore_bluetooth_input() {
  QMessageBox confirm(
      QMessageBox::Question, "Restore Bluetooth input",
      "Bluetooth will handle classic keyboards, mice and gamepads itself "
      "again.\n\nDualSense controllers then connect as plain Bluetooth "
      "gamepads and no longer reach vDS Mango until you run setup again. "
      "Bluetooth restarts, so connected devices drop for a few seconds.",
      QMessageBox::Cancel, this);
  const QPushButton *restore_button =
      confirm.addButton("Restore", QMessageBox::AcceptRole);
  confirm.setDefaultButton(QMessageBox::Cancel);
  confirm.exec();
  if (confirm.clickedButton() != restore_button) {
    return;
  }
  run_setup_helper(this, {kEnableBluetoothInput}, [this](const QString &error) {
    if (!error.isEmpty()) {
      show_action_error("Restore Bluetooth input", error);
    }
    check_bluetooth();
  });
}

void MainWindow::edit_controller() {
  const int row = model_.row_of(selected_address());
  if (row < 0 || !daemon_error_.isEmpty()) {
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
        (void)parse_attach_reply(reply);
        refresh_controllers();
        statusBar()->showMessage("Saved " + address + ".", kStatusMessageMs);
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
