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

/// Drives a controller that BlueZ keeps connected, through its hidraw node.
class BtBackend {
public:
  BtBackend(std::string address, UniqueFd hidraw_fd);

  BtBackend(const BtBackend &) = delete;
  BtBackend &operator=(const BtBackend &) = delete;
  BtBackend(BtBackend &&other) noexcept = default;
  BtBackend &operator=(BtBackend &&other) noexcept = default;

  /// Readable while read_feature_report() has a reply to hand out.
  int feature_reply_fd() const { return feature_reply_fd_.get(); }
  int hidraw_fd() const { return hidraw_fd_.get(); }
  void send_output_report(std::span<const std::uint8_t> report);
  bool try_send_output_report(std::span<const std::uint8_t> report);
  /// Blocks until the controller has answered.
  void send_feature_get(std::uint8_t report_id);
  bool try_send_feature_set(std::span<const std::uint8_t> report);
  std::optional<std::vector<std::uint8_t>> read_feature_report();
  std::optional<std::vector<std::uint8_t>> read_interrupt_packet();

private:
  std::string address_;
  UniqueFd hidraw_fd_;
  UniqueFd feature_reply_fd_;
  std::deque<std::vector<std::uint8_t>> feature_replies_;
};

} // namespace vds
