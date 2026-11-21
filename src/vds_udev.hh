// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <optional>
#include <string>
#include <vector>

struct udev;
struct udev_monitor;

namespace vds {

std::vector<std::string> discover_vds_devices();
bool bluetooth_hid_device_present(const std::string &address);

/// A DualSense that BlueZ's input plugin connected and hid-playstation drives.
struct BluezHidPad {
  std::string hidraw;
  /// The evdev node of the gamepad, without motion sensors and touchpad.
  std::string gamepad_event;
  /// Only root may open the hidraw node, so no other program holds the pad.
  bool hidden = false;
};

/// Empty until the kernel driver has created the gamepad's input device.
std::optional<BluezHidPad> find_bluez_hid_pad(const std::string &address);

class VdsDeviceMonitor {
public:
  VdsDeviceMonitor();
  ~VdsDeviceMonitor();

  VdsDeviceMonitor(const VdsDeviceMonitor &) = delete;
  VdsDeviceMonitor &operator=(const VdsDeviceMonitor &) = delete;
  VdsDeviceMonitor(VdsDeviceMonitor &&other) noexcept;
  VdsDeviceMonitor &operator=(VdsDeviceMonitor &&other) noexcept;

  struct Events {
    bool vds_port = false;
    bool input = false;
  };

  int fd() const;
  Events drain();

private:
  udev *udev_ = nullptr;
  udev_monitor *monitor_ = nullptr;
};

} // namespace vds
