// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <optional>
#include <utility>
#include <vector>

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
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
  explicit PreparePage(ControlClient &client) : client_(client) {
    setTitle("Prepare this computer");
    setSubTitle("vDS Mango needs its background service.");

    service_row_ = new StatusRow(this);
    problem_ = wrapped_label({}, this);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(service_row_);
    layout->addSpacing(8);
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

  bool isComplete() const override { return !busy_ && service_known_; }

  bool validatePage() override {
    if (service_ok_) {
      return true;
    }
    busy_ = true;
    problem_->setText("Waiting for the password prompt...");
    emit completeChanged();
    run_setup_helper(this, {kStartService}, [this](const QString &error) {
      busy_ = false;
      advance_when_ready_ = error.isEmpty();
      problem_->setText(error);
      emit completeChanged();
      refresh();
    });
    return false;
  }

private:
  void refresh() {
    client_.send(
        list_request(), [this](const QByteArray &) { show_service({}); },
        [this](const QString &error) { show_service(error); });
  }

  void show_service(const QString &error) {
    service_known_ = true;
    service_ok_ = error.isEmpty();
    service_row_->show_state(service_ok_,
                             service_ok_ ? "The background service is running."
                                         : error);
    setButtonText(QWizard::NextButton, service_ok_ ? "Next" : "Set up");
    emit completeChanged();
    if (advance_when_ready_ && service_ok_) {
      advance_when_ready_ = false;
      wizard()->next();
    }
  }

  ControlClient &client_;
  QTimer timer_;
  StatusRow *service_row_ = nullptr;
  QLabel *problem_ = nullptr;
  bool service_known_ = false;
  bool service_ok_ = false;
  bool busy_ = false;
  bool advance_when_ready_ = false;
};

struct Candidate {
  /// The address vdsd registers the controller under.
  QString address;
  QString name;
  /// Unset for a Steam Controller: its Puck needs no pairing.
  std::optional<BluetoothDevice> bluetooth = std::nullopt;
};

class ControllerPage : public QWizardPage {
public:
  ControllerPage(ControlClient &client, Bluez &bluez, QString &added_address)
      : client_(client), bluez_(bluez), added_address_(added_address) {
    setTitle("Add your controller");
    setSubTitle("DualSense: hold Create and PS together until the light bar "
                "blinks rapidly; paired ones are listed too. Steam "
                "Controller: plug in its Puck and switch the controller on.");
    setButtonText(QWizard::NextButton, "Add controller");

    list_ = new QListWidget(this);
    status_ = wrapped_label("Looking for controllers…", this);
    claim_hint_ = wrapped_label("Steam Controller not listed? Steam is "
                                "probably using its Puck.",
                                this);
    claim_ = new QPushButton("Take it from Steam", this);
    auto *claim_row = new QHBoxLayout;
    claim_row->addWidget(claim_hint_, 1);
    claim_row->addWidget(claim_);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(list_, 1);
    layout->addWidget(status_);
    layout->addLayout(claim_row);

    connect(claim_, &QPushButton::clicked, this, [this] {
      claiming_ = true;
      claim_->setEnabled(false);
      claim_hint_->setText("Looking on the Puck. Other programs get it back "
                           "if you close this window without adding a "
                           "controller.");
      refresh();
    });

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
    const Candidate candidate =
        candidates_.at(static_cast<std::size_t>(list_->currentRow()));
    if (!candidate.bluetooth) {
      set_busy(true, "Adding the controller…");
      add(candidate.address);
      return false;
    }
    set_busy(true, candidate.bluetooth->paired
                       ? "Adding the controller…"
                       : "Pairing with the controller…");
    bluez_.pair_and_trust(
        *candidate.bluetooth,
        [this, address = candidate.address] { add(address); },
        [this](const QString &error) { set_busy(false, error); });
    return false;
  }

private:
  void add(const QString &address) {
    client_.send(
        attach_request({
            .address = address,
            .profile = ControllerProfile::Unspecified,
            .ports = {},
        }),
        [this, address](const QByteArray &reply) {
          (void)parse_attach_reply(reply);
          added_address_ = address;
          added_ = true;
          set_busy(false, {});
          wizard()->next();
        },
        [this](const QString &error) { set_busy(false, error); });
  }

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
          client_.send(
              local_targets_request(claiming_),
              [this, registered](const QByteArray &targets) {
                find_bluetooth(registered, switched_on(targets));
              },
              [this](const QString &error) { status_->setText(error); });
        },
        [this](const QString &error) { status_->setText(error); });
  }

  /* The Steam Controllers that are on and not registered yet. */
  static std::vector<Candidate> switched_on(const QByteArray &targets) {
    std::vector<Candidate> found;
    for (const PairedController &target : parse_list_targets_reply(targets)) {
      if (target.online && !target.registered) {
        found.push_back({.address = target.address, .name = target.name});
      }
    }
    return found;
  }

  /* Without Bluetooth a Steam Controller can still be added. */
  void find_bluetooth(const QSet<QString> &registered,
                      const std::vector<Candidate> &found) {
    bluez_.list_devices(
        [this, registered, found](std::vector<BluetoothDevice> devices) {
          std::vector<Candidate> candidates = found;
          for (const BluetoothDevice &device : devices) {
            const QString address = device.address.toLower();
            if (device.supported_controller && !registered.contains(address)) {
              candidates.push_back({.address = address,
                                    .name = device.name,
                                    .bluetooth = device});
            }
          }
          bluez_.start_discovery();
          show_candidates(std::move(candidates), {});
        },
        [this, found](const QString &error) {
          show_candidates(found, error);
        });
  }

  static QString label_of(const Candidate &candidate) {
    if (!candidate.bluetooth) {
      return candidate.name + "  " +
             candidate.address.mid(
                 static_cast<qsizetype>(kSteamAddressPrefix.size())) +
             "  (switched on)";
    }
    return candidate.name + "  " + candidate.bluetooth->address +
           (candidate.bluetooth->paired ? "  (paired)" : "  (ready to pair)");
  }

  void show_candidates(std::vector<Candidate> candidates,
                       const QString &problem) {
    if (!problem.isEmpty()) {
      status_->setText(problem);
    }
    const auto paired = [](const Candidate &candidate) {
      return candidate.bluetooth && candidate.bluetooth->paired;
    };
    const bool same = std::equal(
        candidates.begin(), candidates.end(), candidates_.begin(),
        candidates_.end(), [&](const Candidate &left, const Candidate &right) {
          return left.address == right.address && paired(left) == paired(right);
        });
    if (same) {
      return;
    }
    const QString selected =
        list_->currentRow() >= 0
            ? candidates_.at(static_cast<std::size_t>(list_->currentRow()))
                  .address
            : QString();
    candidates_ = std::move(candidates);
    list_->clear();
    for (const Candidate &candidate : candidates_) {
      list_->addItem(label_of(candidate));
      if (candidate.address == selected) {
        list_->setCurrentRow(list_->count() - 1);
      }
    }
    if (list_->currentRow() < 0 && list_->count() == 1) {
      list_->setCurrentRow(0);
    }
    if (problem.isEmpty()) {
      status_->setText(candidates_.empty() ? "Looking for controllers…"
                                           : QString());
    }
    emit completeChanged();
  }

  ControlClient &client_;
  Bluez &bluez_;
  QString &added_address_;
  QTimer timer_;
  QListWidget *list_ = nullptr;
  QLabel *status_ = nullptr;
  QLabel *claim_hint_ = nullptr;
  QPushButton *claim_ = nullptr;
  std::vector<Candidate> candidates_;
  /// The user asked to take the Pucks from other programs; every refresh
  /// renews that until the page is left.
  bool claiming_ = false;
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
    explanation_ = wrapped_label({}, this);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(row_);
    layout->addSpacing(8);
    layout->addWidget(explanation_);
    layout->addStretch(1);

    connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
  }

  void initializePage() override {
    row_->show_state(false, waiting_text());
    timer_.start(kRefreshIntervalMs);
  }

private:
  bool steam() const {
    return is_steam_address(added_address_.toStdString());
  }

  QString waiting_text() const {
    return steam() ? "Switch the Steam Controller on."
                   : "Press the PS button on the controller.";
  }

  void refresh() {
    client_.send(
        list_request(),
        [this](const QByteArray &reply) {
          for (const ControllerStatus &controller : parse_list_reply(reply)) {
            if (controller.address != added_address_) {
              continue;
            }
            show_controller(controller);
          }
        },
        [this](const QString &error) { row_->show_state(false, error); });
  }

  void show_controller(const ControllerStatus &controller) {
    if (kernel_fix_missing(controller)) {
      row_->show_state(false, "This kernel cannot share the controller with "
                              "Bluetooth.");
      explanation_->setText(kKernelRequirement +
                            " Until then the controller stays an ordinary "
                            "Bluetooth gamepad.");
      return;
    }
    explanation_->clear();
    if (controller.connected) {
      row_->show_state(
          true, steam() ? "Connected. Games now see a USB DualSense; its "
                          "haptics and rumble play on the Steam Controller."
                        : "Connected. Games now see a USB DualSense with "
                          "haptics, adaptive triggers and audio.");
    } else if (!controller.error.isEmpty()) {
      row_->show_state(false, waiting_text() + " The last attempt failed: " +
                                  controller.error);
    } else {
      row_->show_state(false, waiting_text());
    }
  }

  ControlClient &client_;
  const QString &added_address_;
  QTimer timer_;
  StatusRow *row_ = nullptr;
  QLabel *explanation_ = nullptr;
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

  setPage(PreparePageId, new PreparePage(client_));
  setPage(ControllerPageId,
          new ControllerPage(client_, bluez_, added_address_));
  setPage(ConnectPageId, new ConnectPage(client_, added_address_));
  setStartId(prepare_needed ? PreparePageId : ControllerPageId);
}

} // namespace vds::gui
