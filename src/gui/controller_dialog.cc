// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QVBoxLayout>

#include "controller_card.hh"
#include "controller_dialog.hh"
#include "spacing.hh"

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

constexpr std::pair<PadClick, const char *> kPadClicks[] = {
    {PadClick::Off, "Off"},
    {PadClick::Soft, "Soft"},
    {PadClick::Strong, "Strong"},
};

constexpr int kSliderWidth = 220;

/* A slider shows no number, so its value stands next to it. */
QSlider *add_strength_row(QFormLayout *form, const QString &label,
                          unsigned percent, const QString &tip,
                          QWidget *parent) {
  auto *slider = new QSlider(Qt::Horizontal, parent);
  slider->setRange(0, 100);
  slider->setSingleStep(5);
  slider->setPageStep(10);
  slider->setValue(static_cast<int>(percent));
  slider->setMinimumWidth(kSliderWidth);
  slider->setToolTip(tip);

  auto *value = new QLabel(parent);
  value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  value->setMinimumWidth(value->fontMetrics().horizontalAdvance("100%"));
  const auto show_value = [value](int chosen) {
    value->setText(chosen == 0 ? QString("Off")
                               : QString::number(chosen) + "%");
  };
  QObject::connect(slider, &QSlider::valueChanged, value, show_value);
  show_value(slider->value());

  auto *row = new QHBoxLayout;
  row->addWidget(slider, 1);
  row->addWidget(value);
  form->addRow(label, row);
  return slider;
}

} // namespace

ControllerDialog::ControllerDialog(const ControllerStatus &controller,
                                   const QString &name, QWidget *parent)
    : QDialog(parent), address_(controller.address) {
  setWindowTitle("Edit controller");

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
  const bool steam = is_steam_address(controller.address.toStdString());
  if (steam) {
    form->addRow("Virtual port:", ports_row);
    haptics_ = add_strength_row(
        form, "Haptics strength:", controller.steam.haptics_strength,
        "How hard the grips shake for a game's haptics. 80% is closest to a "
        "DualSense.",
        this);
    rumble_ = add_strength_row(
        form, "Rumble strength:", controller.steam.rumble_strength,
        "How hard the grips shake for a game's rumble.", this);
    click_box_ = new QComboBox(this);
    for (const auto &[click, label] : kPadClicks) {
      click_box_->addItem(label, static_cast<int>(click));
    }
    click_box_->setCurrentIndex(std::max(
        0, click_box_->findData(static_cast<int>(controller.steam.pad_click))));
    click_box_->setToolTip(
        "The click you feel when you press a trackpad down.");
    form->addRow("Trackpad click:", click_box_);
  } else {
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

    form->addRow("Appears as:", profile_box_);
    form->addRow("Virtual port:", ports_row);
    form->addRow("Haptics and speaker delay:", delay_box_);
  }

  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  accept_button_ = buttons->button(QDialogButtonBox::Ok);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(kWindowMargin, kWindowMargin, kWindowMargin,
                             kWindowMargin);
  layout->setSpacing(kBlockSpacing);
  layout->addLayout(form);
  if (controller.connected && !steam) {
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
      .profile = ControllerProfile::Unspecified,
      .ports = {},
  };
  if (haptics_ != nullptr) {
    result.steam = SteamTuning{
        .haptics_strength = static_cast<std::uint8_t>(haptics_->value()),
        .rumble_strength = static_cast<std::uint8_t>(rumble_->value()),
        .pad_click = static_cast<PadClick>(click_box_->currentData().toInt()),
    };
  } else {
    result.profile =
        kProfiles[static_cast<std::size_t>(profile_box_->currentIndex())];
    result.audio_buffer = delay_box_->currentData().toUInt();
  }
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
