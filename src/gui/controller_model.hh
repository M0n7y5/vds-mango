// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <span>
#include <vector>

#include <QAbstractTableModel>
#include <QHash>
#include <QString>

#include "control_protocol.hh"

namespace vds::gui {

QString profile_label(ControllerProfile profile);
QString ports_label(std::span<const unsigned> ports);

class ControllerModel : public QAbstractTableModel {
public:
  enum Column {
    NameColumn,
    AddressColumn,
    StatusColumn,
    DeviceColumn,
    ProfileColumn,
    PortsColumn,
    ColumnCount,
  };

  using QAbstractTableModel::QAbstractTableModel;

  void set_controllers(std::vector<ControllerStatus> controllers);
  void set_names(QHash<QString, QString> names);
  const ControllerStatus &controller(int row) const;
  QString name(const QString &address) const;
  int row_of(const QString &address) const;

  int rowCount(const QModelIndex &parent = {}) const override;
  int columnCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override;

private:
  std::vector<ControllerStatus> controllers_;
  QHash<QString, QString> names_;
};

} // namespace vds::gui
