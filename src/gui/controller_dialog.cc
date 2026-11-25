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

constexpr std::pair<unsigned, const char *> kDelays[] = {
    {64, "Standard: about 78 ms, safest"},
    {32, "Reduced: about 50 ms"},
    {24, "Low: about 45 ms"},
    {16, "Lowest: about 33 ms, the speaker may crackle"},
};

} // namespace

ControllerDialog::ControllerDialog(const ControllerStatus &controller,
                                   const QString &name, QWidget *parent)
    : QDialog(parent), address_(controller.address) {
  setWindowTitle("Edit controller");

  profile_box_ = new QComboBox(this);
  for (const ControllerProfile profile : kProfiles) {
    profile_box_->addItem(profile_label(profile));
  }
  profile_box_->setCurrentIndex(static_cast<int>(
      std::find(std::begin(kProfiles), std::end(kProfiles),
                controller.profile) -
      std::begin(kProfiles)));
  profile_box_->setToolTip(
      "The controller model games see. Automatic matches the real one.");

  delay_box_ = new QComboBox(this);
  for (const auto &[length, label] : kDelays) {
    delay_box_->addItem(label, length);
  }
  delay_box_->setCurrentIndex(
      std::max(0, delay_box_->findData(controller.audio_buffer)));
  delay_box_->setToolTip(
      "Lower reacts sooner. Too low makes the sound crackle.");

  auto *any_port = new QRadioButton("Any free port", this);
  chosen_ports_ = new QRadioButton("Only:", this);
  any_port->setChecked(controller.ports.empty());
  chosen_ports_->setChecked(!controller.ports.empty());
  auto *ports_row = new QHBoxLayout;
  ports_row->addWidget(any_port);
  ports_row->addSpacing(12);
  ports_row->addWidget(chosen_ports_);
  for (std::size_t port = 0; port < port_boxes_.size(); ++port) {
    port_boxes_[port] = new QCheckBox(QString::number(port), this);
    port_boxes_[port]->setToolTip(QString::fromStdString(
        port_name_for_index(static_cast<unsigned>(port))));
    port_boxes_[port]->setChecked(
        std::find(controller.ports.begin(), controller.ports.end(), port) !=
        controller.ports.end());
    ports_row->addWidget(port_boxes_[port]);
    connect(port_boxes_[port], &QCheckBox::toggled, this,
            [this] { update_accept_button(); });
  }
  ports_row->addStretch();
  connect(chosen_ports_, &QRadioButton::toggled, this,
          [this] { update_accept_button(); });

  auto *form = new QFormLayout;
  form->addRow("Controller:",
               new QLabel(name + " (" + controller.address + ")", this));
  form->addRow("Appears as:", profile_box_);
  form->addRow("Virtual port:", ports_row);
  form->addRow("Haptics and speaker delay:", delay_box_);

  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  accept_button_ = buttons->button(QDialogButtonBox::Ok);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto *layout = new QVBoxLayout(this);
  layout->addLayout(form);
  if (controller.connected) {
    layout->addWidget(new QLabel(
        "Changing how the controller appears reconnects it.", this));
  }
  layout->addWidget(buttons);

  update_accept_button();
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
      .address = address_,
      .profile =
          kProfiles[static_cast<std::size_t>(profile_box_->currentIndex())],
      .ports = {},
      .audio_buffer = delay_box_->currentData().toUInt(),
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
