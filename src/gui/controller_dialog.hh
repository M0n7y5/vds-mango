// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <array>
#include <vector>

#include <QDialog>
#include <QString>

#include "control_protocol.hh"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QRadioButton;

namespace vds::gui {

class ControllerDialog : public QDialog {
public:
  /// Registers one of the paired controllers that vdsd does not know yet.
  ControllerDialog(std::vector<PairedController> candidates, QWidget *parent);
  /// Changes the profile and ports of a registered controller.
  ControllerDialog(const ControllerStatus &controller, const QString &name,
                   QWidget *parent);

  ControllerAssignment assignment() const;

private:
  void build(QWidget *controller_field);
  void show_candidate_hint();
  void update_accept_button();

  std::vector<PairedController> candidates_;
  QString address_;
  QComboBox *candidate_box_ = nullptr;
  QLabel *hint_ = nullptr;
  QComboBox *profile_box_ = nullptr;
  QRadioButton *any_port_ = nullptr;
  QRadioButton *chosen_ports_ = nullptr;
  std::array<QCheckBox *, kMaxPortCount> port_boxes_{};
  QPushButton *accept_button_ = nullptr;
};

} // namespace vds::gui
