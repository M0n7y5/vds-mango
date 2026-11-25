// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include <linux/hidraw.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "unique_fd.hh"
#include "vds_bt.hh"
#include "vds_protocol.hh"

namespace vds {

namespace {

constexpr std::size_t kBtReadBufferSize = 1024;
// hidraw carries bare reports; the parsers expect the HIDP header in front.
constexpr std::uint8_t kHidpInputHeader = 0xa1;
constexpr std::uint8_t kHidpFeatureHeader = 0xa3;

UniqueFd open_feature_reply_fd() {
  UniqueFd fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!fd) {
    throw BluetoothLinkError("eventfd failed: " +
                             std::string(std::strerror(errno)));
  }
  return fd;
}

} // namespace

BtBackend::BtBackend(std::string address, UniqueFd hidraw_fd)
    : address_(std::move(address)), hidraw_fd_(std::move(hidraw_fd)),
      feature_reply_fd_(open_feature_reply_fd()) {}

void BtBackend::send_output_report(std::span<const std::uint8_t> report) {
  if (!try_send_output_report(report)) {
    throw BluetoothLinkError("Bluetooth hidraw write would block");
  }
}

bool BtBackend::try_send_output_report(std::span<const std::uint8_t> report) {
  while (true) {
    const ssize_t written =
        ::write(hidraw_fd_.get(), report.data(), report.size());
    if (written == static_cast<ssize_t>(report.size())) {
      return true;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return false;
    }
    throw BluetoothLinkError("Bluetooth hidraw write failed: " +
                             std::string(std::strerror(errno)));
  }
}

void BtBackend::send_feature_get(std::uint8_t report_id) {
  // hidraw answers inside the ioctl. Keep the reply for read_feature_report()
  // so callers poll for it like for any other controller event.
  std::array<std::uint8_t, 256> buffer{};
  buffer[0] = report_id;
  const int got =
      ::ioctl(hidraw_fd_.get(), HIDIOCGFEATURE(buffer.size()), buffer.data());
  if (got < 0) {
    throw BluetoothLinkError("Bluetooth hidraw feature get failed: " +
                             std::string(std::strerror(errno)));
  }
  std::vector<std::uint8_t> reply{kHidpFeatureHeader};
  reply.insert(reply.end(), buffer.begin(),
               buffer.begin() + static_cast<std::ptrdiff_t>(got));
  feature_replies_.push_back(std::move(reply));
  (void)::eventfd_write(feature_reply_fd_.get(), 1);
}

bool BtBackend::try_send_feature_set(std::span<const std::uint8_t> report) {
  const auto packet = feature_set_packet(report);
  if (packet.empty()) {
    return true;
  }
  // Drop the HIDP header byte; hidraw wants the report itself.
  if (::ioctl(hidraw_fd_.get(), HIDIOCSFEATURE(packet.size() - 1),
              packet.data() + 1) < 0) {
    throw BluetoothLinkError("Bluetooth hidraw feature set failed: " +
                             std::string(std::strerror(errno)));
  }
  return true;
}

std::optional<std::vector<std::uint8_t>> BtBackend::read_feature_report() {
  eventfd_t pending = 0;
  (void)::eventfd_read(feature_reply_fd_.get(), &pending);
  if (feature_replies_.empty()) {
    return std::nullopt;
  }
  const auto reply = std::move(feature_replies_.front());
  feature_replies_.pop_front();
  if (!feature_replies_.empty()) {
    (void)::eventfd_write(feature_reply_fd_.get(), 1);
  }
  return bt_feature_to_usb_feature_reply(reply);
}

std::optional<std::vector<std::uint8_t>> BtBackend::read_interrupt_packet() {
  std::array<std::uint8_t, kBtReadBufferSize> buffer{};
  const ssize_t got = ::read(hidraw_fd_.get(), buffer.data(), buffer.size());
  if (got < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return std::nullopt;
    }
    throw BluetoothLinkError("Bluetooth interrupt read failed: " +
                             std::string(std::strerror(errno)));
  }
  if (got == 0) {
    throw BluetoothLinkError("Bluetooth interrupt channel closed");
  }
  std::vector<std::uint8_t> packet{kHidpInputHeader};
  packet.insert(packet.end(), buffer.begin(),
                buffer.begin() + static_cast<std::ptrdiff_t>(got));
  return packet;
}

} // namespace vds
