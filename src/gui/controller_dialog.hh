// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <array>

#include <QDialog>
#include <QString>

#include "control_protocol.hh"

class QCheckBox;
class QComboBox;
class QPushButton;
class QRadioButton;
class QSlider;

namespace vds::gui {

/// Changes which virtual ports a registered controller may use and, for a
/// DualSense, which model games see and its haptics and speaker delay. A
/// Steam Controller gets its haptics, rumble and trackpad click instead.
class ControllerDialog : public QDialog {
public:
  ControllerDialog(const ControllerStatus &controller, const QString &name,
                   QWidget *parent);

  ControllerAssignment assignment() const;

private:
  void update_accept_button();

  QString address_;
  /// Set for a DualSense only.
  QComboBox *profile_box_ = nullptr;
  QComboBox *delay_box_ = nullptr;
  /// Set for a Steam Controller only.
  QSlider *haptics_ = nullptr;
  QSlider *rumble_ = nullptr;
  QComboBox *click_box_ = nullptr;
  QRadioButton *chosen_ports_ = nullptr;
  std::array<QCheckBox *, kMaxPortCount> port_boxes_{};
  QPushButton *accept_button_ = nullptr;
};

} // namespace vds::gui
