// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "unique_fd.hh"

namespace vds {

class BluetoothLinkError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct BtAcceptedChannel {
  std::string address;
  UniqueFd fd;
};

class BtL2capAcceptor {
public:
  BtL2capAcceptor();

  BtL2capAcceptor(const BtL2capAcceptor &) = delete;
  BtL2capAcceptor &operator=(const BtL2capAcceptor &) = delete;
  BtL2capAcceptor(BtL2capAcceptor &&other) noexcept = default;
  BtL2capAcceptor &operator=(BtL2capAcceptor &&other) noexcept = default;

  int control_listener_fd() const { return control_listener_fd_.get(); }
  int interrupt_listener_fd() const { return interrupt_listener_fd_.get(); }
  std::optional<BtAcceptedChannel> accept_control();
  std::optional<BtAcceptedChannel> accept_interrupt();

private:
  UniqueFd control_listener_fd_;
  UniqueFd interrupt_listener_fd_;
};

class BtBackend {
public:
  /// Owns the two raw HID channels of a controller that connected to vdsd.
  BtBackend(std::string address, UniqueFd control_fd, UniqueFd interrupt_fd);
  /// Drives a controller that BlueZ keeps connected, through its hidraw node.
  /// control_fd() then signals queued feature replies instead of a channel.
  static BtBackend from_hidraw(std::string address, UniqueFd hidraw_fd);
  ~BtBackend();

  BtBackend(const BtBackend &) = delete;
  BtBackend &operator=(const BtBackend &) = delete;
  BtBackend(BtBackend &&other) noexcept;
  BtBackend &operator=(BtBackend &&other) noexcept;

  int control_fd() const { return control_fd_; }
  int interrupt_fd() const { return interrupt_fd_; }
  void send_output_report(std::span<const std::uint8_t> report);
  bool try_send_output_report(std::span<const std::uint8_t> report);
  void send_feature_get(std::uint8_t report_id);
  bool try_send_feature_get(std::uint8_t report_id);
  bool try_send_feature_set(std::span<const std::uint8_t> report);
  std::optional<std::vector<std::uint8_t>> read_feature_report();
  std::optional<std::vector<std::uint8_t>> read_interrupt_packet();

private:
  std::string address_;
  int control_fd_ = -1;
  int interrupt_fd_ = -1;
  bool hidraw_ = false;
  std::deque<std::vector<std::uint8_t>> feature_replies_;
};

} // namespace vds
