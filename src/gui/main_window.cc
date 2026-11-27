// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <utility>

#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QVBoxLayout>

#include "controller_card.hh"
#include "controller_dialog.hh"
#include "diagnostics_dialog.hh"
#include "lottie.hh"
#include "main_window.hh"
#include "setup_wizard.hh"
#include "spacing.hh"
#include "startup_animation.hh"

namespace vds::gui {

namespace {

constexpr int kPollIntervalMs = 2000;
constexpr int kStatusMessageMs = 8000;
constexpr int kCardColumns = 2;
constexpr int kLogoSize = 96;
constexpr int kTrayIconSizes[] = {16, 22, 24, 32, 48, 64};

void show_check(QLabel *check, Tone tone, const QString &text,
                const QString &problem = {}) {
  check->setText(QString("<span style=\"color:%1\">●</span> %2")
                     .arg(tone_color(tone, check->palette()).name(), text));
  check->setToolTip(problem);
}

void show_bluetooth_problem(QLabel *check, const BluetoothProblem &problem) {
  const QString needed = "A DualSense needs Bluetooth to connect, a Steam "
                         "Controller on its Puck does not.";
  switch (problem.kind) {
  case BluetoothProblem::Kind::SwitchedOff:
    show_check(check, Tone::Muted, "Bluetooth off", needed);
    break;
  case BluetoothProblem::Kind::NoAdapter:
    show_check(check, Tone::Muted, "No Bluetooth adapter", needed);
    break;
  case BluetoothProblem::Kind::NotRunning:
    show_check(check, Tone::Muted, "Bluetooth not running", needed);
    break;
  case BluetoothProblem::Kind::Failed:
    show_check(check, Tone::Warning, "Bluetooth unavailable",
               problem.text + "\n" + needed);
    break;
  }
}

std::optional<BluetoothProblem::Kind>
kind_of(const std::optional<BluetoothProblem> &problem) {
  return problem ? std::optional(problem->kind) : std::nullopt;
}

QImage tray_image(TrayIconState state, int size) {
  QImage image = QIcon(":/icons/vds-mango.svg")
                     .pixmap(QSize(size, size), 1.0)
                     .toImage()
                     .convertToFormat(QImage::Format_ARGB32);
  if (state == TrayIconState::Idle) {
    for (int y = 0; y < image.height(); ++y) {
      auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
      for (int x = 0; x < image.width(); ++x) {
        const int gray = qGray(line[x]);
        line[x] = qRgba(gray, gray, gray, qAlpha(line[x]) * 3 / 4);
      }
    }
  } else if (state == TrayIconState::Unreachable) {
    // Breeze's negative red. The white ring keeps the mark apart from the
    // orange mango and from a dark panel.
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal ring = std::max(1.0, size / 16.0);
    const qreal diameter = size * 0.6;
    const QRectF mark(size - diameter, size - diameter, diameter, diameter);
    painter.setPen(QPen(Qt::white, ring));
    painter.setBrush(QColor(0xda, 0x44, 0x53));
    painter.drawEllipse(mark.adjusted(ring / 2, ring / 2, -ring / 2,
                                      -ring / 2));
    const qreal bar = std::max(1.5, diameter / 6);
    const QPointF center = mark.center();
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawRect(QRectF(center.x() - bar / 2, mark.top() + diameter * 0.2,
                            bar, diameter * 0.36));
    painter.drawRect(QRectF(center.x() - bar / 2,
                            mark.bottom() - diameter * 0.2 - bar, bar, bar));
  }
  return image;
}

QIcon tray_icon(TrayIconState state) {
  QIcon icon;
  for (const int size : kTrayIconSizes) {
    icon.addPixmap(QPixmap::fromImage(tray_image(state, size)));
  }
  return icon;
}

} // namespace

MainWindow::MainWindow(const QString &socket_path)
    : socket_path_(socket_path), client_(socket_path) {
  setWindowTitle("vDS Mango");
  resize(900, 520);

  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  layout->setContentsMargins(kWindowMargin, kWindowMargin, kWindowMargin,
                             kWindowMargin);
  layout->setSpacing(kBlockSpacing);
  layout->addWidget(build_banner());
  layout->addWidget(build_controllers_page(), 1);
  layout->addWidget(build_health_strip());
  setCentralWidget(central);

  build_menus();
  build_tray();
  update_health();
  message_timer_.setSingleShot(true);
  connect(&message_timer_, &QTimer::timeout, message_, &QLabel::clear);

  connect(&poll_timer_, &QTimer::timeout, this, [this] {
    refresh_controllers();
    refresh_bluetooth();
  });
  poll_timer_.start(kPollIntervalMs);
  refresh_controllers();
  refresh_bluetooth();
  play_startup_animation();
}

void MainWindow::bring_to_front(const QByteArray &activation_token) {
  // Qt's xdg-shell plugin activates a window with this variable and unsets
  // it, which is also how Qt passes on the token of a tray icon click.
  if (!activation_token.isEmpty() &&
      QGuiApplication::platformName().startsWith("wayland")) {
    qputenv("XDG_ACTIVATION_TOKEN", activation_token);
  }
  setWindowState(windowState() & ~Qt::WindowMinimized);
  show();
  QWidget *front = QApplication::activeModalWidget();
  if (front == nullptr) {
    front = this;
  }
  front->raise();
  front->activateWindow();
}

void MainWindow::closeEvent(QCloseEvent *event) {
  // Quit and logout close the window as well, and those must end the app.
  if (tray_ == nullptr || !event->spontaneous()) {
    QMainWindow::closeEvent(event);
    return;
  }
  event->ignore();
  hide();
  if (!tray_hint_shown_) {
    tray_hint_shown_ = true;
    tray_->showMessage("vDS Mango",
                       "vDS Mango keeps running in the tray. Quit it from "
                       "the tray menu.");
  }
}

QWidget *MainWindow::build_banner() {
  auto *warning_icon = new QLabel(this);
  warning_icon->setPixmap(
      style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(24, 24));
  banner_text_ = new QLabel(this);
  banner_text_->setWordWrap(true);
  banner_setup_ = new QPushButton("Set up", this);
  connect(banner_setup_, &QPushButton::clicked, this, [this] { open_setup(); });

  banner_ = new QWidget(this);
  auto *layout = new QHBoxLayout(banner_);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(warning_icon);
  layout->addWidget(banner_text_, 1);
  layout->addWidget(banner_setup_);
  banner_->hide();
  return banner_;
}

QWidget *MainWindow::build_controllers_page() {
  auto *logo = new QLabel(this);
  logo->setPixmap(
      QIcon(":/icons/vds-mango.svg")
          .pixmap(QSize(kLogoSize, kLogoSize), devicePixelRatioF()));
  logo->setAlignment(Qt::AlignCenter);
  auto *title = new QLabel("No controllers yet", this);
  QFont title_font = title->font();
  title_font.setBold(true);
  title_font.setPointSizeF(title_font.pointSizeF() * 1.3);
  title->setFont(title_font);
  title->setAlignment(Qt::AlignCenter);
  auto *hint = new QLabel("Add a DualSense and games see it as a USB "
                          "controller, with haptics, adaptive triggers and "
                          "audio. A Steam Controller on its Puck becomes a "
                          "DualSense with haptics.",
                          this);
  hint->setWordWrap(true);
  hint->setAlignment(Qt::AlignCenter);
  auto *add_button =
      new QPushButton(QIcon::fromTheme("list-add"), "Add controller", this);
  connect(add_button, &QPushButton::clicked, this, [this] { open_setup(); });

  auto *empty_page = new QWidget(this);
  auto *empty_layout = new QVBoxLayout(empty_page);
  empty_layout->setSpacing(kBlockSpacing);
  empty_layout->addStretch(1);
  empty_layout->addWidget(logo);
  empty_layout->addWidget(title);
  empty_layout->addWidget(hint);
  empty_layout->addWidget(add_button, 0, Qt::AlignCenter);
  empty_layout->addStretch(2);

  add_card_ = new AddControllerCard(this);
  connect(add_card_, &QAbstractButton::clicked, this, [this] { open_setup(); });
  card_area_ = new QWidget(this);
  card_grid_ = new QGridLayout;
  card_grid_->setSpacing(kBlockSpacing);
  for (int column = 0; column < kCardColumns; ++column) {
    card_grid_->setColumnStretch(column, 1);
  }
  auto *card_layout = new QVBoxLayout(card_area_);
  card_layout->setContentsMargins(0, 0, 0, 0);
  card_layout->addLayout(card_grid_);
  card_layout->addStretch(1);
  auto *scroll = new QScrollArea(this);
  scroll->setWidget(card_area_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);

  controller_pages_ = new QStackedWidget(this);
  controller_pages_->addWidget(empty_page);
  controller_pages_->addWidget(scroll);
  rebuild_cards();
  return controller_pages_;
}

QWidget *MainWindow::build_health_strip() {
  service_check_ = new QLabel(this);
  kernel_check_ = new QLabel(this);
  bluetooth_check_ = new QLabel(this);
  message_ = new QLabel(this);
  message_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  // A long message is cut off instead of widening the window.
  message_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  // A link, not a button: text lines up with the card edges, padding does not.
  auto *diagnostics = new QLabel(
      QString("<a href=\"#\" style=\"color:%1; text-decoration:none\">"
              "Diagnostics</a>")
          .arg(palette().color(QPalette::Highlight).name()),
      this);
  diagnostics->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                       Qt::LinksAccessibleByKeyboard);
  connect(diagnostics, &QLabel::linkActivated, this,
          [this] { open_diagnostics(); });

  auto *strip = new QWidget(this);
  auto *layout = new QHBoxLayout(strip);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(kWindowMargin);
  layout->addWidget(service_check_);
  layout->addWidget(kernel_check_);
  layout->addWidget(bluetooth_check_);
  layout->addWidget(message_, 1);
  layout->addWidget(diagnostics);
  return strip;
}

void MainWindow::show_message(const QString &text) {
  message_->setText(text);
  message_timer_.start(kStatusMessageMs);
}

void MainWindow::build_menus() {
  auto *setup_menu = menuBar()->addMenu("Setup");
  setup_menu->addAction("Run setup…", this, [this] { open_setup(); });
  setup_menu->addAction(QIcon::fromTheme("view-refresh"), "Refresh",
                        QKeySequence::Refresh, this, [this] {
                          refresh_controllers();
                          refresh_bluetooth();
                          refresh_names();
                        });
  setup_menu->addSeparator();
  setup_menu->addAction(QIcon::fromTheme("application-exit"), "Quit",
                        QKeySequence::Quit, qApp, &QApplication::quit);
  auto *help_menu = menuBar()->addMenu("Help");
  help_menu->addAction("About vDS Mango", this, [this] { show_about(); });
}

void MainWindow::build_tray() {
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    return;
  }
  tray_menu_ = new QMenu(this);
  tray_entries_end_ = tray_menu_->addSeparator();
  tray_menu_->addAction(QIcon::fromTheme("list-add"), "Add controller…", this,
                        [this] { open_setup_from_tray(); });
  tray_menu_->addAction("Open vDS Mango", this, [this] { bring_to_front({}); });
  tray_menu_->addSeparator();
  tray_menu_->addAction(QIcon::fromTheme("application-exit"), "Quit", qApp,
                        &QApplication::quit);
  tray_ = new QSystemTrayIcon(this);
  tray_->setContextMenu(tray_menu_);
  connect(tray_, &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason != QSystemTrayIcon::Trigger) {
              return;
            }
            if (isVisible() && !isMinimized() &&
                QApplication::activeModalWidget() == nullptr) {
              hide();
            } else {
              bring_to_front({});
            }
          });
  QApplication::setQuitOnLastWindowClosed(false);
  update_tray();
  tray_->show();
}

void MainWindow::update_tray() {
  if (tray_ == nullptr) {
    return;
  }
  QStringList lines{"vDS Mango"};
  if (!daemon_error_.isEmpty()) {
    lines << "Service not running";
  } else {
    for (const ControllerStatus &controller : controllers_) {
      QString line = name_of(controller.address) + ": " +
                     (controller.connected ? "connected" : "not connected");
      if (controller.battery) {
        line += ", battery " + QString::number(*controller.battery) + "%";
      }
      lines << line;
    }
  }
  tray_->setToolTip(lines.join('\n'));
  update_tray_menu();
  const TrayIconState icon_state =
      tray_icon_state(service_state(), controllers_);
  if (icon_state != tray_icon_state_) {
    tray_icon_state_ = icon_state;
    tray_->setIcon(tray_icon(icon_state));
  }
}

void MainWindow::update_tray_menu() {
  std::vector<TrayEntry> entries =
      tray_entries(service_state(), controllers_,
                   [this](const QString &address) { return name_of(address); });
  if (entries == tray_entries_) {
    return;
  }
  for (QAction *action : tray_entry_actions_) {
    tray_menu_->removeAction(action);
    action->deleteLater();
  }
  tray_entry_actions_.clear();
  for (const TrayEntry &entry : entries) {
    auto *action = new QAction(entry.text, tray_menu_);
    if (entry.kind == TrayEntry::Kind::Test) {
      connect(action, &QAction::triggered, this,
              [this, address = entry.address] { test_controller(address); });
    } else {
      connect(action, &QAction::triggered, this,
              [this] { bring_to_front({}); });
    }
    tray_menu_->insertAction(tray_entries_end_, action);
    tray_entry_actions_.push_back(action);
  }
  tray_entries_ = std::move(entries);
}

ServiceState MainWindow::service_state() const {
  if (!daemon_known_) {
    return ServiceState::Checking;
  }
  return daemon_error_.isEmpty() ? ServiceState::Running
                                 : ServiceState::Unreachable;
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
        refresh_status();
      },
      [this](const QString &error) {
        list_pending_ = false;
        daemon_status_ = {};
        show_daemon_error(error);
      });
}

void MainWindow::refresh_status() {
  client_.send(
      status_request(),
      [this](const QByteArray &reply) {
        daemon_status_ = parse_status_reply(reply);
        update_health();
      },
      [](const QString &) {});
}

void MainWindow::refresh_bluetooth() {
  if (bluetooth_pending_) {
    return;
  }
  bluetooth_pending_ = true;
  const auto show = [this](std::optional<BluetoothProblem> problem) {
    bluetooth_pending_ = false;
    bluetooth_known_ = true;
    const bool cards_change = kind_of(problem) != kind_of(bluetooth_problem_);
    bluetooth_problem_ = std::move(problem);
    update_health();
    if (cards_change) {
      update_cards();
    }
  };
  bluez_.list_devices(
      [show](const std::vector<BluetoothDevice> &) { show(std::nullopt); },
      show);
}

void MainWindow::refresh_names() {
  client_.send(
      list_targets_request(),
      [this](const QByteArray &reply) {
        names_.clear();
        for (const PairedController &target : parse_list_targets_reply(reply)) {
          names_.insert(target.address, target.name);
        }
        update_cards();
      },
      [](const QString &) {});
}

void MainWindow::show_controllers(std::vector<ControllerStatus> controllers) {
  const auto same_address = [](const ControllerStatus &left,
                               const ControllerStatus &right) {
    return left.address == right.address;
  };
  const bool same_cards =
      std::equal(controllers.begin(), controllers.end(), controllers_.begin(),
                 controllers_.end(), same_address);
  controllers_ = std::move(controllers);
  if (!same_cards) {
    rebuild_cards();
  }
  update_cards();
  controller_pages_->setCurrentIndex(controllers_.empty() ? 0 : 1);
  notify_low_batteries();
}

void MainWindow::notify_low_batteries() {
  if (tray_ == nullptr) {
    return;
  }
  QHash<QString, bool> armed;
  for (const ControllerStatus &controller : controllers_) {
    const LowBatteryStep step = low_battery_step(
        low_battery_armed_.value(controller.address, true), controller);
    armed.insert(controller.address, step.armed);
    if (step.notify) {
      tray_->showMessage("Low battery",
                         name_of(controller.address) + " is at " +
                             QString::number(*controller.battery) +
                             "%. Charge it soon.",
                         QSystemTrayIcon::Warning);
    }
  }
  low_battery_armed_ = std::move(armed);
}

void MainWindow::rebuild_cards() {
  for (ControllerCard *card : cards_) {
    card_grid_->removeWidget(card);
    card->deleteLater();
  }
  cards_.clear();
  card_grid_->removeWidget(add_card_);

  const auto place = [this](QWidget *widget) {
    const int index = card_grid_->count();
    card_grid_->addWidget(widget, index / kCardColumns, index % kCardColumns,
                          Qt::AlignTop);
  };
  for (const ControllerStatus &controller : controllers_) {
    const QString address = controller.address;
    auto *card = new ControllerCard(
        [this, address] { test_controller(address); },
        [this, address] {
          QTimer::singleShot(0, this,
                             [this, address] { edit_controller(address); });
        },
        [this, address] {
          QTimer::singleShot(0, this,
                             [this, address] { remove_controller(address); });
        },
        card_area_);
    place(card);
    cards_.push_back(card);
  }
  place(add_card_);
}

void MainWindow::update_cards() {
  for (std::size_t index = 0; index < cards_.size(); ++index) {
    cards_[index]->show_controller(controllers_[index],
                                   name_of(controllers_[index].address),
                                   kind_of(bluetooth_problem_));
  }
  update_tray();
}

void MainWindow::show_daemon_error(const QString &error) {
  const bool became_reachable =
      error.isEmpty() && (!daemon_known_ || !daemon_error_.isEmpty());
  daemon_known_ = true;
  daemon_error_ = error;
  controller_pages_->setEnabled(error.isEmpty());
  if (became_reachable) {
    refresh_names();
  }
  update_health();
  offer_setup_once();
  update_tray();
}

void MainWindow::show_action_error(const QString &title, const QString &error) {
  QMessageBox::warning(this, title, error);
}

void MainWindow::update_health() {
  const bool service_ok = daemon_error_.isEmpty();
  if (!daemon_known_) {
    show_check(service_check_, Tone::Muted, "Checking the service");
  } else if (service_ok) {
    show_check(service_check_, Tone::Positive, "Service running");
  } else {
    show_check(service_check_, Tone::Negative, "Service not running",
               daemon_error_);
  }

  /*
   * vdsd tests the driver when the first DualSense connects on a kernel and
   * remembers a pass, so until then there is nothing to report.
   */
  QString kernel_problem;
  const bool kernel_known = service_ok && daemon_status_.kernel_fix;
  kernel_check_->setVisible(kernel_known);
  if (kernel_known && *daemon_status_.kernel_fix) {
    show_check(kernel_check_, Tone::Positive, "Kernel has the DualSense fix");
  } else if (kernel_known) {
    kernel_problem = "This kernel cannot share controllers with Bluetooth. " +
                     kKernelRequirement;
    show_check(kernel_check_, Tone::Negative, "Kernel lacks the DualSense fix",
               kernel_problem);
  }

  if (!bluetooth_known_) {
    show_check(bluetooth_check_, Tone::Muted, "Checking Bluetooth");
  } else if (!bluetooth_problem_) {
    show_check(bluetooth_check_, Tone::Positive, "Bluetooth on");
  } else {
    show_bluetooth_problem(bluetooth_check_, *bluetooth_problem_);
  }

  const QString &text = !service_ok ? daemon_error_ : kernel_problem;
  banner_text_->setText(text);
  // Setup can start the service. It cannot change the kernel.
  banner_setup_->setVisible(!service_ok);
  banner_->setVisible(!text.isEmpty());
}

void MainWindow::offer_setup_once() {
  // The wizard must not open on top of the logo.
  if (setup_offered_ || !daemon_known_ || starting_up_) {
    return;
  }
  setup_offered_ = true;
  if (!daemon_error_.isEmpty() || controllers_.empty()) {
    // Queued so the dialog's event loop does not run inside a reply handler.
    QTimer::singleShot(0, this, [this] { open_setup(); });
  }
}

void MainWindow::play_startup_animation() {
  // Plasma answers 0 while its animation speed is set to instant.
  if (style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr,
                         this) == 0) {
    return;
  }
  QFile file(":/animations/startup.json");
  (void)file.open(QIODevice::ReadOnly);
  starting_up_ = true;
  new StartupAnimation(
      LottieAnimation(file.readAll()),
      [this] {
        starting_up_ = false;
        offer_setup_once();
      },
      this);
}

QString MainWindow::name_of(const QString &address) const {
  return controller_name(names_, address);
}

QString MainWindow::system_info() const {
  QStringList lines;
  lines << "vDS Mango " + QApplication::applicationVersion() + ", service " +
               (daemon_status_.version.isEmpty() ? "unknown"
                                                 : daemon_status_.version);
  lines << "System: " + QSysInfo::prettyProductName() + ", kernel " +
               QSysInfo::kernelVersion() + ", desktop " +
               qEnvironmentVariable("XDG_CURRENT_DESKTOP", "unknown");
  lines << "Service: " +
               (daemon_error_.isEmpty() ? "running" : daemon_error_);
  lines << QString("DualSense audio fix in the kernel: ") +
               (!daemon_status_.kernel_fix    ? "not checked yet"
                : *daemon_status_.kernel_fix ? "yes"
                                             : "no");
  lines << "Bluetooth: " + (!bluetooth_known_     ? "not checked yet"
                            : !bluetooth_problem_ ? "on"
                                                  : bluetooth_problem_->text);
  if (controllers_.empty()) {
    lines << "Controllers: none registered";
  }
  for (const ControllerStatus &controller : controllers_) {
    QStringList facts;
    facts << name_of(controller.address);
    facts << (controller.connected       ? "connected on " + controller.path
              : controller.error.isEmpty() ? "not connected"
                                           : "error: " + controller.error);
    if (!controller.firmware.isEmpty()) {
      facts << "firmware " + controller.firmware + " (build " +
                   controller.firmware_build + ")";
    }
    if (controller.battery) {
      facts << "battery " + QString::number(*controller.battery) + "%";
    }
    const bool steam = is_steam_address(controller.address.toStdString());
    if (!steam) {
      facts << "appears as " + profile_label(controller.profile);
    }
    facts << "port " + ports_label(controller.ports);
    if (steam) {
      facts << QString("haptics %1%, rumble %2%, trackpad click %3")
                   .arg(unsigned{controller.steam.haptics_strength})
                   .arg(unsigned{controller.steam.rumble_strength})
                   .arg(QString::fromStdString(
                       pad_click_name(controller.steam.pad_click)));
    } else {
      facts << "delay " + QString::number(controller.audio_buffer);
    }
    lines << "Controller: " + facts.join(", ");
  }
  return lines.join('\n');
}

void MainWindow::open_setup() {
  SetupWizard wizard(socket_path_, !daemon_error_.isEmpty(), this);
  wizard.exec();
  refresh_controllers();
  refresh_names();
}

void MainWindow::open_setup_from_tray() {
  bring_to_front({});
  // Queued so the wizard's event loop does not hold up the D-Bus call that
  // delivered the click. The check runs then, so a second click while the
  // first is queued only raises the wizard.
  QTimer::singleShot(0, this, [this] {
    if (QApplication::activeModalWidget() == nullptr) {
      open_setup();
    }
  });
}

void MainWindow::open_diagnostics() {
  DiagnosticsDialog dialog(socket_path_, system_info(), this);
  dialog.exec();
}

void MainWindow::edit_controller(const QString &address) {
  const auto found = std::find_if(controllers_.begin(), controllers_.end(),
                                  [&](const ControllerStatus &candidate) {
                                    return candidate.address == address;
                                  });
  if (found == controllers_.end() || !daemon_error_.isEmpty()) {
    return;
  }
  const ControllerStatus controller = *found;
  ControllerDialog dialog(controller, name_of(address), this);
  if (dialog.exec() == QDialog::Accepted) {
    apply_assignment(dialog.assignment());
  }
}

void MainWindow::remove_controller(const QString &address) {
  QMessageBox confirm(
      QMessageBox::Question, "Remove controller",
      "Remove " + name_of(address) + " (" + address +
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
        show_message("Removed " + address + ".");
        refresh_controllers();
      },
      [this](const QString &error) {
        show_action_error("Remove controller", error);
      });
}

void MainWindow::test_controller(const QString &address) {
  client_.send(
      buzz_request(address),
      [this, address](const QByteArray &reply) {
        parse_buzz_reply(reply);
        show_message("Sent a test buzz to " + name_of(address) + ".");
      },
      [this](const QString &error) {
        // A message box on a hidden window would show up without context.
        if (tray_ != nullptr && !isVisible()) {
          tray_->showMessage("Test haptics", error,
                             QSystemTrayIcon::Warning);
        } else {
          show_action_error("Test haptics", error);
        }
      });
}

void MainWindow::apply_assignment(const ControllerAssignment &assignment) {
  client_.send(
      attach_request(assignment),
      [this, address = assignment.address](const QByteArray &reply) {
        (void)parse_attach_reply(reply);
        refresh_controllers();
        show_message("Saved " + address + ".");
      },
      [this](const QString &error) {
        show_action_error("Save controller", error);
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
