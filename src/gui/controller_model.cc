// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <cstddef>
#include <utility>

#include <QStringList>

#include "controller_model.hh"

namespace vds::gui {

namespace {

QString status_text(const ControllerStatus &controller) {
  if (controller.connected) {
    return "Connected";
  }
  if (!controller.error.isEmpty()) {
    return "Failed: " + controller.error;
  }
  return "Press PS to connect";
}

} // namespace

QString profile_label(ControllerProfile profile) {
  switch (profile) {
  case ControllerProfile::Ds5:
    return "DualSense";
  case ControllerProfile::Dse:
    return "DualSense Edge";
  case ControllerProfile::Unspecified:
    break;
  }
  return "Automatic";
}

QString ports_label(std::span<const unsigned> ports) {
  if (ports.empty()) {
    return "Any";
  }
  QStringList labels;
  for (const unsigned port : ports) {
    labels.append(QString::number(port));
  }
  return labels.join(", ");
}

void ControllerModel::set_controllers(
    std::vector<ControllerStatus> controllers) {
  const bool same_rows =
      std::equal(controllers.begin(), controllers.end(), controllers_.begin(),
                 controllers_.end(),
                 [](const ControllerStatus &left, const ControllerStatus &right) {
                   return left.address == right.address;
                 });
  if (!same_rows) {
    beginResetModel();
    controllers_ = std::move(controllers);
    endResetModel();
    return;
  }

  // Updating in place keeps the selection across the periodic refresh.
  controllers_ = std::move(controllers);
  if (!controllers_.empty()) {
    emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
  }
}

void ControllerModel::set_names(QHash<QString, QString> names) {
  names_ = std::move(names);
  if (!controllers_.empty()) {
    emit dataChanged(index(0, NameColumn), index(rowCount() - 1, NameColumn));
  }
}

const ControllerStatus &ControllerModel::controller(int row) const {
  return controllers_.at(static_cast<std::size_t>(row));
}

QString ControllerModel::name(const QString &address) const {
  return names_.value(address, "DualSense controller");
}

int ControllerModel::row_of(const QString &address) const {
  const auto found =
      std::find_if(controllers_.begin(), controllers_.end(),
                   [&](const ControllerStatus &controller) {
                     return controller.address == address;
                   });
  return found == controllers_.end()
             ? -1
             : static_cast<int>(found - controllers_.begin());
}

int ControllerModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : static_cast<int>(controllers_.size());
}

int ControllerModel::columnCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : ColumnCount;
}

QVariant ControllerModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || (role != Qt::DisplayRole && role != Qt::ToolTipRole)) {
    return {};
  }

  const ControllerStatus &row = controller(index.row());
  switch (index.column()) {
  case NameColumn:
    return name(row.address);
  case AddressColumn:
    return row.address;
  case StatusColumn:
    return status_text(row);
  case DeviceColumn:
    return row.path;
  case ProfileColumn:
    return profile_label(row.profile);
  case PortsColumn:
    return ports_label(row.ports);
  }
  return {};
}

QVariant ControllerModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
    return {};
  }
  switch (section) {
  case NameColumn:
    return "Controller";
  case AddressColumn:
    return "Address";
  case StatusColumn:
    return "Status";
  case DeviceColumn:
    return "Virtual device";
  case ProfileColumn:
    return "Appears as";
  case PortsColumn:
    return "Ports";
  }
  return {};
}

} // namespace vds::gui
