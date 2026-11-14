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

namespace vds::gui {

/// Changes which controller model games see and which virtual ports a
/// registered controller may use.
class ControllerDialog : public QDialog {
public:
  ControllerDialog(const ControllerStatus &controller, const QString &name,
                   QWidget *parent);

  ControllerAssignment assignment() const;

private:
  void update_accept_button();

  QString address_;
  QComboBox *profile_box_ = nullptr;
  QRadioButton *chosen_ports_ = nullptr;
  std::array<QCheckBox *, kMaxPortCount> port_boxes_{};
  QPushButton *accept_button_ = nullptr;
};

} // namespace vds::gui
