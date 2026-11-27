// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <functional>
#include <optional>
#include <span>

#include <QAbstractButton>
#include <QColor>
#include <QFrame>
#include <QPalette>
#include <QString>

#include "bluez.hh"
#include "control_protocol.hh"

class QLabel;
class QToolButton;

namespace vds::gui {

QString profile_label(ControllerProfile profile);
QString ports_label(std::span<const unsigned> ports);

enum class Tone { Plain, Muted, Positive, Warning, Negative };
QColor tone_color(Tone tone, const QPalette &palette);

/// One registered controller: its picture, state, battery and settings.
class ControllerCard : public QFrame {
public:
  /// on_edit and on_remove run inside a button click of this card, so they
  /// must not open a dialog or do anything else that could delete the card.
  /// on_test vibrates the controller.
  ControllerCard(std::function<void()> on_test, std::function<void()> on_edit,
                 std::function<void()> on_remove, QWidget *parent);

  /// bluetooth_problem is unset while Bluetooth is on or not checked yet.
  void show_controller(const ControllerStatus &controller, const QString &name,
                       std::optional<BluetoothProblem::Kind> bluetooth_problem);

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  class Pill;
  class Battery;

  bool connected_ = false;
  int picture_state_ = -1;
  int state_icons_ = -1;
  QLabel *picture_ = nullptr;
  QLabel *name_ = nullptr;
  QLabel *address_ = nullptr;
  Pill *status_ = nullptr;
  Battery *battery_ = nullptr;
  QLabel *battery_text_ = nullptr;
  Pill *profile_ = nullptr;
  Pill *port_ = nullptr;
  /// The haptics delay of a DualSense, the haptics strength of a Steam
  /// Controller.
  Pill *tuning_ = nullptr;
  QLabel *headset_ = nullptr;
  QLabel *mic_ = nullptr;
  QLabel *detail_ = nullptr;
  QToolButton *test_ = nullptr;
};

/// The dashed card that starts adding a controller.
class AddControllerCard : public QAbstractButton {
public:
  explicit AddControllerCard(QWidget *parent);

  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent *event) override;
};

} // namespace vds::gui
