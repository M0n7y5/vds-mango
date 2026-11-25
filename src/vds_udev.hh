// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <optional>
#include <string>

struct udev;
struct udev_monitor;

namespace vds {

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

/// Watches input devices appear and disappear.
class InputDeviceMonitor {
public:
  InputDeviceMonitor();
  ~InputDeviceMonitor();

  InputDeviceMonitor(const InputDeviceMonitor &) = delete;
  InputDeviceMonitor &operator=(const InputDeviceMonitor &) = delete;
  InputDeviceMonitor(InputDeviceMonitor &&other) noexcept;
  InputDeviceMonitor &operator=(InputDeviceMonitor &&other) noexcept;

  int fd() const;
  /// True when something changed since the last call.
  bool drain();

private:
  udev *udev_ = nullptr;
  udev_monitor *monitor_ = nullptr;
};

} // namespace vds
