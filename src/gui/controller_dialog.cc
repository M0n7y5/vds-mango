// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include "controller_dialog.hh"
#include "controller_model.hh"

namespace vds::gui {

namespace {

constexpr ControllerProfile kProfiles[] = {
    ControllerProfile::Unspecified,
    ControllerProfile::Ds5,
    ControllerProfile::Dse,
};

} // namespace

ControllerDialog::ControllerDialog(std::vector<PairedController> candidates,
                                   QWidget *parent)
    : QDialog(parent), candidates_(std::move(candidates)) {
  setWindowTitle("Add controller");

  candidate_box_ = new QComboBox(this);
  for (const PairedController &candidate : candidates_) {
    candidate_box_->addItem(candidate.name + " (" + candidate.address + ")");
  }
  build(candidate_box_);

  connect(candidate_box_, &QComboBox::currentIndexChanged, this,
          [this] { show_candidate_hint(); });
  show_candidate_hint();
}

ControllerDialog::ControllerDialog(const ControllerStatus &controller,
                                   const QString &name, QWidget *parent)
    : QDialog(parent), address_(controller.address) {
  setWindowTitle("Edit controller");
  build(new QLabel(name + " (" + controller.address + ")", this));

  profile_box_->setCurrentIndex(static_cast<int>(
      std::find(std::begin(kProfiles), std::end(kProfiles),
                controller.profile) -
      std::begin(kProfiles)));
  chosen_ports_->setChecked(!controller.ports.empty());
  for (const unsigned port : controller.ports) {
    if (port < port_boxes_.size()) {
      port_boxes_[port]->setChecked(true);
    }
  }
  if (controller.connected) {
    hint_->setText("Changing how the controller appears reconnects it.");
    hint_->show();
  }
  update_accept_button();
}

void ControllerDialog::build(QWidget *controller_field) {
  hint_ = new QLabel(this);
  hint_->setWordWrap(true);
  hint_->hide();

  profile_box_ = new QComboBox(this);
  for (const ControllerProfile profile : kProfiles) {
    profile_box_->addItem(profile_label(profile));
  }
  profile_box_->setToolTip(
      "Which USB controller games see. Automatic matches the physical "
      "controller. Pick a model when a game supports only one of the two.");

  any_port_ = new QRadioButton("Any free port", this);
  any_port_->setChecked(true);
  chosen_ports_ = new QRadioButton("Only:", this);
  auto *ports_row = new QHBoxLayout;
  ports_row->addWidget(any_port_);
  ports_row->addSpacing(12);
  ports_row->addWidget(chosen_ports_);
  for (std::size_t port = 0; port < port_boxes_.size(); ++port) {
    port_boxes_[port] = new QCheckBox(QString::number(port), this);
    port_boxes_[port]->setToolTip(
        QString::fromStdString(port_path_for_index(static_cast<unsigned>(port))));
    ports_row->addWidget(port_boxes_[port]);
    connect(port_boxes_[port], &QCheckBox::toggled, this,
            [this] { update_accept_button(); });
  }
  ports_row->addStretch();
  connect(chosen_ports_, &QRadioButton::toggled, this,
          [this] { update_accept_button(); });

  auto *form = new QFormLayout;
  form->addRow("Controller:", controller_field);
  form->addRow("Appears as:", profile_box_);
  form->addRow("Virtual port:", ports_row);

  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  accept_button_ = buttons->button(QDialogButtonBox::Ok);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto *layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(hint_);
  layout->addWidget(buttons);

  update_accept_button();
}

void ControllerDialog::show_candidate_hint() {
  const PairedController &candidate =
      candidates_.at(static_cast<std::size_t>(candidate_box_->currentIndex()));
  if (candidate.trusted) {
    hint_->hide();
    return;
  }
  hint_->setText("This controller is not trusted in BlueZ, so it will not "
                 "reconnect on its own. Run \"trust " +
                 candidate.address + "\" in bluetoothctl.");
  hint_->show();
}

void ControllerDialog::update_accept_button() {
  const bool chosen = chosen_ports_->isChecked();
  bool any_checked = false;
  for (QCheckBox *box : port_boxes_) {
    box->setEnabled(chosen);
    any_checked = any_checked || box->isChecked();
  }
  accept_button_->setEnabled(!chosen || any_checked);
}

ControllerAssignment ControllerDialog::assignment() const {
  ControllerAssignment result{
      .address = candidate_box_
                     ? candidates_
                           .at(static_cast<std::size_t>(
                               candidate_box_->currentIndex()))
                           .address
                     : address_,
      .profile =
          kProfiles[static_cast<std::size_t>(profile_box_->currentIndex())],
      .ports = {},
  };
  if (chosen_ports_->isChecked()) {
    for (std::size_t port = 0; port < port_boxes_.size(); ++port) {
      if (port_boxes_[port]->isChecked()) {
        result.ports.push_back(static_cast<unsigned>(port));
      }
    }
  }
  return result;
}

} // namespace vds::gui
