// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <libudev.h>
#include <sys/stat.h>

#include "vds/ds5_protocol.h"
#include "vds_common.hh"
#include "vds_config.hh"
#include "vds_io.hh"
#include "vds_udev.hh"

namespace {

std::string read_first_line(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}

std::string bluetooth_hid_id(unsigned product) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "HID_ID=0005:%08X:%08X",
                static_cast<unsigned>(VDS_SONY_VENDOR_ID), product);
  return buffer;
}

std::string gamepad_event_node(const std::filesystem::path &hid_device) {
  std::error_code error;
  for (const auto &input :
       std::filesystem::directory_iterator(hid_device / "input", error)) {
    const std::string name = read_first_line(input.path() / "name");
    if (name.empty() || ends_with(name, " Motion Sensors") ||
        ends_with(name, " Touchpad")) {
      continue;
    }
    for (const auto &child :
         std::filesystem::directory_iterator(input.path(), error)) {
      const std::string node = child.path().filename().string();
      if (node.rfind("event", 0) == 0) {
        return "/dev/input/" + node;
      }
    }
  }
  return {};
}

bool is_vds_device_path(std::string_view path) {
  constexpr std::string_view prefix = "/dev/vds";
  return path.rfind(prefix, 0) == 0 &&
         vds::is_decimal_number(path.substr(prefix.size()));
}

std::string require_vds_devnode(udev_device *device) {
  const char *devnode = ::udev_device_get_devnode(device);
  if (devnode == nullptr || !is_vds_device_path(devnode)) {
    return {};
  }
  return devnode;
}

} // namespace

namespace vds {

std::vector<std::string> discover_vds_devices() {
  udev *context = ::udev_new();
  if (context == nullptr) {
    throw std::runtime_error("failed to create udev context");
  }

  udev_enumerate *enumerate = ::udev_enumerate_new(context);
  if (enumerate == nullptr) {
    ::udev_unref(context);
    throw std::runtime_error("failed to create udev enumerate context");
  }

  if (::udev_enumerate_add_match_subsystem(enumerate, "misc") < 0 ||
      ::udev_enumerate_scan_devices(enumerate) < 0) {
    ::udev_enumerate_unref(enumerate);
    ::udev_unref(context);
    throw std::runtime_error("failed to enumerate udev misc devices");
  }

  std::vector<std::string> devices;
  udev_list_entry *entries = ::udev_enumerate_get_list_entry(enumerate);
  udev_list_entry *entry = nullptr;
  udev_list_entry_foreach(entry, entries) {
    const char *syspath = ::udev_list_entry_get_name(entry);
    if (syspath == nullptr) {
      continue;
    }

    udev_device *device = ::udev_device_new_from_syspath(context, syspath);
    if (device == nullptr) {
      continue;
    }
    const std::string devnode = require_vds_devnode(device);
    if (!devnode.empty()) {
      devices.push_back(devnode);
    }
    ::udev_device_unref(device);
  }

  ::udev_enumerate_unref(enumerate);
  ::udev_unref(context);

  std::sort(devices.begin(), devices.end());
  return devices;
}

bool bluetooth_hid_device_present(const std::string &address) {
  const std::string normalized_address = normalize_bluetooth_address(address);
  udev *context = ::udev_new();
  if (context == nullptr) {
    return false;
  }

  udev_enumerate *enumerate = ::udev_enumerate_new(context);
  if (enumerate == nullptr) {
    ::udev_unref(context);
    return false;
  }

  if (::udev_enumerate_add_match_subsystem(enumerate, "hid") < 0 ||
      ::udev_enumerate_scan_devices(enumerate) < 0) {
    ::udev_enumerate_unref(enumerate);
    ::udev_unref(context);
    return false;
  }

  bool present = false;
  udev_list_entry *entries = ::udev_enumerate_get_list_entry(enumerate);
  udev_list_entry *entry = nullptr;
  udev_list_entry_foreach(entry, entries) {
    const char *syspath = ::udev_list_entry_get_name(entry);
    if (syspath == nullptr) {
      continue;
    }

    udev_device *device = ::udev_device_new_from_syspath(context, syspath);
    if (device == nullptr) {
      continue;
    }

    const char *name = ::udev_device_get_sysname(device);
    const char *uniq = ::udev_device_get_sysattr_value(device, "uniq");
    if (name != nullptr && std::string_view(name).rfind("0005:", 0) == 0 &&
        uniq != nullptr) {
      try {
        present = normalize_bluetooth_address(uniq) == normalized_address;
      } catch (const std::exception &) {
        present = false;
      }
    }

    ::udev_device_unref(device);
    if (present) {
      break;
    }
  }

  ::udev_enumerate_unref(enumerate);
  ::udev_unref(context);
  return present;
}

std::optional<BluezHidPad> find_bluez_hid_pad(const std::string &address) {
  const std::string uniq = "HID_UNIQ=" + normalize_bluetooth_address(address);
  const std::string ds5_id = bluetooth_hid_id(VDS_DS5_PRODUCT_ID);
  const std::string dse_id = bluetooth_hid_id(VDS_DSE_PRODUCT_ID);

  std::error_code error;
  for (const auto &entry :
       std::filesystem::directory_iterator("/sys/class/hidraw", error)) {
    std::ifstream uevent(entry.path() / "device" / "uevent");
    bool dualsense = false;
    bool matches = false;
    std::string line;
    while (std::getline(uevent, line)) {
      if (line == ds5_id || line == dse_id) {
        dualsense = true;
      } else if (line.rfind("HID_UNIQ=", 0) == 0) {
        std::transform(line.begin() + 9, line.end(), line.begin() + 9,
                       [](char ch) {
                         return static_cast<char>(
                             std::tolower(static_cast<unsigned char>(ch)));
                       });
        matches = line == uniq;
      }
    }
    if (!dualsense || !matches) {
      continue;
    }

    BluezHidPad pad{
        .hidraw = "/dev/" + entry.path().filename().string(),
        .gamepad_event = gamepad_event_node(entry.path() / "device"),
        .hidden = false,
    };
    if (pad.gamepad_event.empty()) {
      return std::nullopt;
    }
    struct stat status {};
    pad.hidden = ::stat(pad.hidraw.c_str(), &status) == 0 &&
                 (status.st_mode & (S_IRWXG | S_IRWXO)) == 0;
    return pad;
  }
  return std::nullopt;
}

VdsDeviceMonitor::VdsDeviceMonitor() {
  udev_ = ::udev_new();
  if (udev_ == nullptr) {
    throw std::runtime_error("failed to create udev context");
  }

  monitor_ = ::udev_monitor_new_from_netlink(udev_, "udev");
  if (monitor_ == nullptr) {
    ::udev_unref(udev_);
    udev_ = nullptr;
    throw std::runtime_error("failed to create udev monitor");
  }
  if (::udev_monitor_filter_add_match_subsystem_devtype(monitor_, "misc",
                                                        nullptr) < 0 ||
      ::udev_monitor_filter_add_match_subsystem_devtype(monitor_, "input",
                                                        nullptr) < 0) {
    ::udev_monitor_unref(monitor_);
    ::udev_unref(udev_);
    monitor_ = nullptr;
    udev_ = nullptr;
    throw std::runtime_error("failed to configure udev monitor filter");
  }
  if (::udev_monitor_enable_receiving(monitor_) < 0) {
    ::udev_monitor_unref(monitor_);
    ::udev_unref(udev_);
    monitor_ = nullptr;
    udev_ = nullptr;
    throw std::runtime_error("failed to enable udev monitor");
  }
  set_nonblocking(fd(), "udev monitor");
}

VdsDeviceMonitor::~VdsDeviceMonitor() {
  if (monitor_ != nullptr) {
    ::udev_monitor_unref(monitor_);
  }
  if (udev_ != nullptr) {
    ::udev_unref(udev_);
  }
}

VdsDeviceMonitor::VdsDeviceMonitor(VdsDeviceMonitor &&other) noexcept
    : udev_(std::exchange(other.udev_, nullptr)),
      monitor_(std::exchange(other.monitor_, nullptr)) {}

VdsDeviceMonitor &
VdsDeviceMonitor::operator=(VdsDeviceMonitor &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (monitor_ != nullptr) {
    ::udev_monitor_unref(monitor_);
  }
  if (udev_ != nullptr) {
    ::udev_unref(udev_);
  }
  udev_ = std::exchange(other.udev_, nullptr);
  monitor_ = std::exchange(other.monitor_, nullptr);
  return *this;
}

int VdsDeviceMonitor::fd() const { return ::udev_monitor_get_fd(monitor_); }

VdsDeviceMonitor::Events VdsDeviceMonitor::drain() {
  Events events;
  while (true) {
    udev_device *device = ::udev_monitor_receive_device(monitor_);
    if (device == nullptr) {
      return events;
    }
    const char *subsystem = ::udev_device_get_subsystem(device);
    if (subsystem != nullptr && std::string_view(subsystem) == "input") {
      events.input = true;
    } else if (!require_vds_devnode(device).empty()) {
      events.vds_port = true;
    }
    ::udev_device_unref(device);
  }
}

} // namespace vds
