// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <utility>
#include <vector>

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QSet>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWizardPage>

#include "control_protocol.hh"
#include "setup_helper.hh"
#include "setup_wizard.hh"

namespace vds::gui {

namespace {

enum PageId { PreparePageId, ControllerPageId, ConnectPageId };

constexpr int kRefreshIntervalMs = 1000;

QLabel *wrapped_label(const QString &text, QWidget *parent) {
  auto *label = new QLabel(text, parent);
  label->setWordWrap(true);
  return label;
}

class StatusRow : public QWidget {
public:
  explicit StatusRow(QWidget *parent) : QWidget(parent) {
    icon_ = new QLabel(this);
    text_ = wrapped_label({}, this);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(icon_);
    layout->addWidget(text_, 1);
  }

  void show_state(bool ok, const QString &text) {
    icon_->setPixmap(style()
                         ->standardIcon(ok ? QStyle::SP_DialogApplyButton
                                           : QStyle::SP_MessageBoxWarning)
                         .pixmap(20, 20));
    text_->setText(text);
  }

private:
  QLabel *icon_ = nullptr;
  QLabel *text_ = nullptr;
};

class PreparePage : public QWizardPage {
public:
  PreparePage(ControlClient &client, Bluez &bluez)
      : client_(client), bluez_(bluez) {
    setTitle("Prepare this computer");
    setSubTitle("vDS Mango needs its background service and has to receive "
                "DualSense controllers from Bluetooth.");

    service_row_ = new StatusRow(this);
    bluetooth_row_ = new StatusRow(this);
    explanation_ = wrapped_label({}, this);
    problem_ = wrapped_label({}, this);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(service_row_);
    layout->addWidget(bluetooth_row_);
    layout->addSpacing(8);
    layout->addWidget(explanation_);
    layout->addWidget(problem_);
    layout->addStretch(1);

    connect(&timer_, &QTimer::timeout, this, [this] {
      if (wizard() != nullptr && wizard()->currentPage() == this && !busy_) {
        refresh();
      }
    });
  }

  void initializePage() override {
    timer_.start(kRefreshIntervalMs);
    refresh();
  }

  bool isComplete() const override {
    return !busy_ && service_known_ && bluetooth_known_ &&
           bluetooth_problem_.isEmpty();
  }

  bool validatePage() override {
    if (ready()) {
      return true;
    }
    QStringList actions;
    if (bluetooth_input_enabled_) {
      actions.append(kDisableBluetoothInput);
    }
    if (!service_ok_) {
      actions.append(kStartService);
    }
    busy_ = true;
    problem_->setText("Waiting for the password prompt…");
    emit completeChanged();
    run_setup_helper(this, actions, [this](const QString &error) {
      busy_ = false;
      advance_when_ready_ = error.isEmpty();
      problem_->setText(error);
      emit completeChanged();
      refresh();
    });
    return false;
  }

private:
  bool ready() const {
    return service_ok_ && bluetooth_known_ && !bluetooth_input_enabled_ &&
           bluetooth_problem_.isEmpty();
  }

  void refresh() {
    client_.send(
        list_request(),
        [this](const QByteArray &reply) {
          (void)parse_list_reply(reply);
          show_service({});
        },
        [this](const QString &error) { show_service(error); });
    bluez_.query_input_plugin(
        [this](bool enabled) {
          bluetooth_known_ = true;
          bluetooth_input_enabled_ = enabled;
          bluetooth_problem_.clear();
          bluez_.list_devices(
              [this](const std::vector<BluetoothDevice> &devices) {
                show_bluetooth(devices);
              },
              [this](const QString &error) {
                bluetooth_problem_ = error;
                show_bluetooth({});
              });
        },
        [this](const QString &error) {
          bluetooth_known_ = true;
          bluetooth_problem_ = error;
          show_bluetooth({});
        });
  }

  void show_service(const QString &error) {
    service_known_ = true;
    service_ok_ = error.isEmpty();
    service_row_->show_state(
        service_ok_, service_ok_ ? "The background service is running." : error);
    update_page();
  }

  void show_bluetooth(const std::vector<BluetoothDevice> &devices) {
    if (!bluetooth_problem_.isEmpty()) {
      bluetooth_row_->show_state(false, bluetooth_problem_);
      explanation_->clear();
      update_page();
      return;
    }
    if (!bluetooth_input_enabled_) {
      bluetooth_row_->show_state(
          true, "Bluetooth hands DualSense controllers to vDS Mango.");
      explanation_->clear();
      update_page();
      return;
    }

    QStringList affected;
    for (const BluetoothDevice &device : devices) {
      if (device.paired && device.classic_input &&
          !device.supported_controller) {
        affected.append(device.name);
      }
    }
    bluetooth_row_->show_state(
        false, "Bluetooth still connects controllers as plain gamepads.");
    explanation_->setText(
        "Setup switches off the standard Bluetooth support for classic "
        "keyboards, mice and gamepads, because only one program can receive "
        "them. Bluetooth audio is not affected. Bluetooth restarts, so "
        "connected devices drop for a few seconds.\n\n" +
        (affected.isEmpty()
             ? QString("None of your paired devices is affected.")
             : "These paired devices stop working until you undo this under "
               "Setup > Restore Bluetooth input: " +
                   affected.join(", ") + "."));
    update_page();
  }

  void update_page() {
    setButtonText(QWizard::NextButton, ready() ? "Next" : "Set up");
    emit completeChanged();
    if (advance_when_ready_ && ready()) {
      advance_when_ready_ = false;
      wizard()->next();
    }
  }

  ControlClient &client_;
  Bluez &bluez_;
  QTimer timer_;
  StatusRow *service_row_ = nullptr;
  StatusRow *bluetooth_row_ = nullptr;
  QLabel *explanation_ = nullptr;
  QLabel *problem_ = nullptr;
  QString bluetooth_problem_;
  bool service_known_ = false;
  bool service_ok_ = false;
  bool bluetooth_known_ = false;
  bool bluetooth_input_enabled_ = false;
  bool busy_ = false;
  bool advance_when_ready_ = false;
};

class ControllerPage : public QWizardPage {
public:
  ControllerPage(ControlClient &client, Bluez &bluez, QString &added_address)
      : client_(client), bluez_(bluez), added_address_(added_address) {
    setTitle("Add your controller");
    setSubTitle("Hold Create and PS together until the light bar blinks "
                "rapidly. Controllers that are already paired are listed "
                "too.");
    setButtonText(QWizard::NextButton, "Add controller");

    list_ = new QListWidget(this);
    status_ = wrapped_label("Looking for controllers…", this);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(list_, 1);
    layout->addWidget(status_);

    connect(list_, &QListWidget::currentRowChanged, this,
            [this] { emit completeChanged(); });
    connect(&timer_, &QTimer::timeout, this, [this] {
      if (wizard() != nullptr && wizard()->currentPage() == this && !busy_) {
        refresh();
      }
    });
  }

  void initializePage() override {
    timer_.start(kRefreshIntervalMs);
    refresh();
  }

  bool isComplete() const override {
    return !busy_ && list_->currentRow() >= 0;
  }

  bool validatePage() override {
    if (added_) {
      return true;
    }
    const BluetoothDevice device =
        candidates_.at(static_cast<std::size_t>(list_->currentRow()));
    set_busy(true, device.paired ? "Adding the controller…"
                                 : "Pairing with the controller…");
    bluez_.pair_and_trust(
        device,
        [this, address = device.address] {
          client_.send(
              attach_request({
                  .address = address,
                  .profile = ControllerProfile::Unspecified,
                  .ports = {},
              }),
              [this, address](const QByteArray &reply) {
                (void)parse_attach_reply(reply);
                added_address_ = address.toLower();
                added_ = true;
                set_busy(false, {});
                wizard()->next();
              },
              [this](const QString &error) { set_busy(false, error); });
        },
        [this](const QString &error) { set_busy(false, error); });
    return false;
  }

private:
  void set_busy(bool busy, const QString &status) {
    busy_ = busy;
    list_->setEnabled(!busy);
    status_->setText(status);
    emit completeChanged();
  }

  void refresh() {
    client_.send(
        list_request(),
        [this](const QByteArray &reply) {
          QSet<QString> registered;
          for (const ControllerStatus &controller : parse_list_reply(reply)) {
            registered.insert(controller.address);
          }
          bluez_.list_devices(
              [this, registered](std::vector<BluetoothDevice> devices) {
                std::erase_if(devices, [&](const BluetoothDevice &device) {
                  return !device.supported_controller ||
                         registered.contains(device.address.toLower());
                });
                show_candidates(std::move(devices));
              },
              [this](const QString &error) { status_->setText(error); });
        },
        [this](const QString &error) { status_->setText(error); });
  }

  void show_candidates(std::vector<BluetoothDevice> devices) {
    bluez_.start_discovery();
    const bool same = std::equal(
        devices.begin(), devices.end(), candidates_.begin(), candidates_.end(),
        [](const BluetoothDevice &left, const BluetoothDevice &right) {
          return left.path == right.path && left.paired == right.paired;
        });
    if (same) {
      return;
    }
    const QString selected =
        list_->currentRow() >= 0
            ? candidates_.at(static_cast<std::size_t>(list_->currentRow())).path
            : QString();
    candidates_ = std::move(devices);
    list_->clear();
    for (const BluetoothDevice &device : candidates_) {
      list_->addItem(device.name + "  " + device.address +
                     (device.paired ? "  (paired)" : "  (ready to pair)"));
      if (device.path == selected) {
        list_->setCurrentRow(list_->count() - 1);
      }
    }
    if (list_->currentRow() < 0 && list_->count() == 1) {
      list_->setCurrentRow(0);
    }
    status_->setText(candidates_.empty() ? "Looking for controllers…"
                                         : QString());
    emit completeChanged();
  }

  ControlClient &client_;
  Bluez &bluez_;
  QString &added_address_;
  QTimer timer_;
  QListWidget *list_ = nullptr;
  QLabel *status_ = nullptr;
  std::vector<BluetoothDevice> candidates_;
  bool busy_ = false;
  bool added_ = false;
};

class ConnectPage : public QWizardPage {
public:
  ConnectPage(ControlClient &client, const QString &added_address)
      : client_(client), added_address_(added_address) {
    setTitle("Connect the controller");
    setFinalPage(true);

    row_ = new StatusRow(this);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(row_);
    layout->addStretch(1);

    connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
  }

  void initializePage() override {
    row_->show_state(false, "Press the PS button on the controller.");
    timer_.start(kRefreshIntervalMs);
  }

private:
  void refresh() {
    client_.send(
        list_request(),
        [this](const QByteArray &reply) {
          for (const ControllerStatus &controller : parse_list_reply(reply)) {
            if (controller.address != added_address_) {
              continue;
            }
            if (controller.connected) {
              row_->show_state(true,
                               "Connected. Games now see a USB DualSense "
                               "with haptics, adaptive triggers and audio.");
            } else if (!controller.error.isEmpty()) {
              row_->show_state(false, "Press the PS button on the controller. "
                                      "The last attempt failed: " +
                                          controller.error);
            } else {
              row_->show_state(false,
                               "Press the PS button on the controller.");
            }
          }
        },
        [this](const QString &error) { row_->show_state(false, error); });
  }

  ControlClient &client_;
  const QString &added_address_;
  QTimer timer_;
  StatusRow *row_ = nullptr;
};

} // namespace

SetupWizard::SetupWizard(const QString &socket_path, bool prepare_needed,
                         QWidget *parent)
    : QWizard(parent), client_(socket_path) {
  setWindowTitle("Set up vDS Mango");
  setWizardStyle(QWizard::ModernStyle);
  setOption(QWizard::NoBackButtonOnStartPage);
  setOption(QWizard::NoBackButtonOnLastPage);
  setOption(QWizard::NoCancelButtonOnLastPage);
  resize(620, 420);

  setPage(PreparePageId, new PreparePage(client_, bluez_));
  setPage(ControllerPageId,
          new ControllerPage(client_, bluez_, added_address_));
  setPage(ConnectPageId, new ConnectPage(client_, added_address_));
  setStartId(prepare_needed ? PreparePageId : ControllerPageId);
}

} // namespace vds::gui
