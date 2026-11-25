// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <functional>
#include <vector>

#include <QObject>
#include <QString>

namespace vds::gui {

struct BluetoothDevice {
  QString path;
  QString address;
  QString name;
  bool paired = false;
  bool trusted = false;
  /// A DualSense or DualSense Edge.
  bool supported_controller = false;
};

/// Talks to bluetoothd over the system bus without blocking. Every call ends
/// in exactly one of its two handlers.
class Bluez : public QObject {
public:
  using DevicesHandler = std::function<void(std::vector<BluetoothDevice>)>;
  using DoneHandler = std::function<void()>;
  using ErrorHandler = std::function<void(const QString &)>;

  explicit Bluez(QObject *parent = nullptr);
  ~Bluez() override;

  void list_devices(DevicesHandler on_devices, ErrorHandler on_error);
  void start_discovery();
  void pair_and_trust(const BluetoothDevice &device, DoneHandler on_done,
                      ErrorHandler on_error);

private:
  void trust(const QString &path, DoneHandler on_done, ErrorHandler on_error);

  QString adapter_path_;
  bool discovering_ = false;
};

} // namespace vds::gui
