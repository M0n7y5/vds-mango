// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

#include "unique_fd.hh"

namespace vds {

/// The host controller dropped the virtual device or sent something invalid.
class UsbipLinkError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// A virtual DualSense speaking the device side of USB/IP on host_fd. It
/// exchanges the frames of include/uapi/vds.h with vdsd on frame_fd, one per
/// message.
class UsbipDevice {
public:
  using Clock = std::chrono::steady_clock;

  UsbipDevice(std::uint32_t profile, UniqueFd host_fd, int frame_fd);
  ~UsbipDevice();

  UsbipDevice(const UsbipDevice &) = delete;
  UsbipDevice &operator=(const UsbipDevice &) = delete;

  /// Handles what the host and vdsd have sent and sends the replies due at
  /// now. Returns when the next reply falls due.
  std::optional<Clock::time_point> service(Clock::time_point now);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// One virtual USB port on the in-tree USB/IP host controller (vhci_hcd).
class UsbipPort {
public:
  UsbipPort();
  ~UsbipPort();

  UsbipPort(const UsbipPort &) = delete;
  UsbipPort &operator=(const UsbipPort &) = delete;

  /// vdsd's end of the frame channel.
  int frame_fd() const { return daemon_fd_.get(); }
  /// Readable whenever service() has work to do.
  int poll_fd() const { return epoll_fd_.get(); }
  /// Plugs a controller of this profile into a free vhci_hcd port.
  void connect(std::uint32_t profile);
  void disconnect();
  /// vhci_hcd unplugs asynchronously and refuses system sleep until then.
  bool unplugged() const;
  /// Unplugs the controller before it throws UsbipLinkError.
  void service();

private:
  void arm_timer(std::optional<UsbipDevice::Clock::time_point> due);
  void discard_frames();

  UniqueFd daemon_fd_;
  UniqueFd device_frame_fd_;
  UniqueFd timer_fd_;
  UniqueFd epoll_fd_;
  std::unique_ptr<UsbipDevice> device_;
  std::optional<unsigned> vhci_port_;
};

/// False while the vhci_hcd kernel module is not loaded.
bool usbip_host_available();

} // namespace vds
