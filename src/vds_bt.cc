// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "unique_fd.hh"
#include "vds_bt.hh"
#include "vds_io.hh"
#include "vds_protocol.hh"

namespace vds {

namespace {

constexpr int kBtProtocolL2cap = 0;
constexpr std::uint16_t kHidControlPsm = 0x0011;
constexpr std::uint16_t kHidInterruptPsm = 0x0013;
constexpr std::size_t kBtReadBufferSize = 1024;
constexpr int kBtWritePollTimeoutMs = 1000;
// hidraw carries bare reports; the parsers expect the HIDP header L2CAP has.
constexpr std::uint8_t kHidpInputHeader = 0xa1;
constexpr std::uint8_t kHidpFeatureHeader = 0xa3;

void close_if_open(int &fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

std::string psm_name(std::uint16_t psm) {
  char buffer[7]{};
  std::snprintf(buffer, sizeof(buffer), "0x%04x", psm);
  return buffer;
}

std::string psm_error(std::uint16_t psm, int error) {
  return "Bluetooth L2CAP PSM " + psm_name(psm) + ": " + std::strerror(error);
}

void write_bt_packet(int fd, std::span<const std::uint8_t> packet) {
  try {
    write_full(fd, packet, "Bluetooth L2CAP", kBtWritePollTimeoutMs);
  } catch (const std::runtime_error &error) {
    throw BluetoothLinkError(error.what());
  }
}

bool try_write_packet(int fd, std::span<const std::uint8_t> bytes) {
  while (true) {
    const ssize_t written =
        ::send(fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
    if (written == static_cast<ssize_t>(bytes.size())) {
      return true;
    }
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return false;
      }
      throw BluetoothLinkError("Bluetooth L2CAP write failed: " +
                               std::string(std::strerror(errno)));
    }
    throw BluetoothLinkError("Bluetooth L2CAP short packet write");
  }
}

int open_l2cap_socket() {
  int fd =
      ::socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, kBtProtocolL2cap);
  if (fd < 0 && errno == EINVAL) {
    fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET, kBtProtocolL2cap);
    if (fd >= 0) {
      vds::UniqueFd unique_fd(fd);
      if (::fcntl(unique_fd.get(), F_SETFD, FD_CLOEXEC) < 0) {
        throw BluetoothLinkError("failed to set socket close-on-exec: " +
                                 std::string(std::strerror(errno)));
      }
      return unique_fd.release();
    }
  }
  if (fd < 0) {
    throw BluetoothLinkError("failed to open Bluetooth L2CAP socket: " +
                             std::string(std::strerror(errno)));
  }
  return fd;
}

void fill_l2cap_sockaddr(sockaddr_l2 &sockaddr, std::uint16_t psm,
                         std::span<const std::uint8_t, 6> address) {
  sockaddr.l2_family = AF_BLUETOOTH;
  sockaddr.l2_psm = htobs(psm);
  bdaddr_t bdaddr{};
  std::copy(address.begin(), address.end(), bdaddr.b);
  ::bacpy(&sockaddr.l2_bdaddr, &bdaddr);
}

std::string bluetooth_address_string(const bdaddr_t &address) {
  char buffer[18]{};
  if (::ba2str(&address, buffer) < 0) {
    throw BluetoothLinkError("failed to format Bluetooth address");
  }
  std::string text = buffer;
  std::transform(text.begin(), text.end(), text.begin(), [](char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  });
  return text;
}

UniqueFd listen_l2cap_psm(std::uint16_t psm) {
  UniqueFd fd(open_l2cap_socket());
  set_nonblocking(fd.get(), "socket");

  const int reuse = 1;
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    throw BluetoothLinkError("failed to set " + psm_name(psm) +
                             " listener reuse: " + std::strerror(errno));
  }

  const std::array<std::uint8_t, 6> any_address{};
  sockaddr_l2 sockaddr{};
  fill_l2cap_sockaddr(sockaddr, psm, any_address);
  if (::bind(fd.get(), reinterpret_cast<const struct sockaddr *>(&sockaddr),
             sizeof(sockaddr)) < 0) {
    const int error = errno;
    std::string message = "failed to bind incoming " + psm_error(psm, error);
    if (error == EADDRINUSE) {
      message += "; bluetoothd's input plugin owns the Bluetooth HID channels,"
                 " restart bluetoothd with --noplugin=input"
                 " (override-bluetoothd.sh disable-input --restart)";
    }
    throw BluetoothLinkError(message);
  }
  if (::listen(fd.get(), 1) < 0) {
    const int error = errno;
    throw BluetoothLinkError("failed to listen on incoming " +
                             psm_error(psm, error));
  }
  return fd;
}

std::optional<BtAcceptedChannel> accept_l2cap_psm(int listener_fd,
                                                  std::uint16_t psm) {
  sockaddr_l2 peer{};
  socklen_t peer_size = sizeof(peer);
  UniqueFd fd(::accept(listener_fd, reinterpret_cast<struct sockaddr *>(&peer),
                       &peer_size));
  if (!fd) {
    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) {
      return {};
    }
    throw BluetoothLinkError("failed to accept incoming " +
                             psm_error(psm, error));
  }

  if (peer_size < sizeof(peer)) {
    return {};
  }
  if (::fcntl(fd.get(), F_SETFD, FD_CLOEXEC) < 0) {
    throw BluetoothLinkError("failed to set accepted " + psm_name(psm) +
                             " close-on-exec: " + std::strerror(errno));
  }
  try {
    set_nonblocking(fd.get(), "socket");
  } catch (const std::runtime_error &error) {
    throw BluetoothLinkError(error.what());
  }
  return BtAcceptedChannel{
      .address = bluetooth_address_string(peer.l2_bdaddr),
      .fd = std::move(fd),
  };
}

bool try_write_hidraw(int fd, std::span<const std::uint8_t> report) {
  while (true) {
    const ssize_t written = ::write(fd, report.data(), report.size());
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

} // namespace

BtL2capAcceptor::BtL2capAcceptor()
    : control_listener_fd_(listen_l2cap_psm(kHidControlPsm)),
      interrupt_listener_fd_(listen_l2cap_psm(kHidInterruptPsm)) {}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_control() {
  return accept_l2cap_psm(control_listener_fd_.get(), kHidControlPsm);
}

std::optional<BtAcceptedChannel> BtL2capAcceptor::accept_interrupt() {
  return accept_l2cap_psm(interrupt_listener_fd_.get(), kHidInterruptPsm);
}

BtBackend::BtBackend(std::string address, UniqueFd control_fd,
                     UniqueFd interrupt_fd)
    : address_(std::move(address)), control_fd_(control_fd.release()),
      interrupt_fd_(interrupt_fd.release()) {}

BtBackend::~BtBackend() {
  close_if_open(interrupt_fd_);
  close_if_open(control_fd_);
}

BtBackend BtBackend::from_hidraw(std::string address, UniqueFd hidraw_fd) {
  UniqueFd ready(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!ready) {
    throw BluetoothLinkError("eventfd failed: " +
                             std::string(std::strerror(errno)));
  }
  BtBackend backend(std::move(address), std::move(ready),
                    std::move(hidraw_fd));
  backend.hidraw_ = true;
  return backend;
}

BtBackend::BtBackend(BtBackend &&other) noexcept
    : address_(std::move(other.address_)), control_fd_(other.control_fd_),
      interrupt_fd_(other.interrupt_fd_), hidraw_(other.hidraw_),
      feature_replies_(std::move(other.feature_replies_)) {
  other.control_fd_ = -1;
  other.interrupt_fd_ = -1;
}

BtBackend &BtBackend::operator=(BtBackend &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  close_if_open(interrupt_fd_);
  close_if_open(control_fd_);
  address_ = std::move(other.address_);
  control_fd_ = other.control_fd_;
  interrupt_fd_ = other.interrupt_fd_;
  hidraw_ = other.hidraw_;
  feature_replies_ = std::move(other.feature_replies_);
  other.control_fd_ = -1;
  other.interrupt_fd_ = -1;
  return *this;
}

void BtBackend::send_output_report(std::span<const std::uint8_t> report) {
  if (interrupt_fd_ < 0) {
    return;
  }
  if (hidraw_) {
    if (!try_write_hidraw(interrupt_fd_, report)) {
      throw BluetoothLinkError("Bluetooth hidraw write would block");
    }
    return;
  }
  const auto packet = hidp_output_packet(report);
  write_bt_packet(interrupt_fd_, packet);
}

bool BtBackend::try_send_output_report(std::span<const std::uint8_t> report) {
  if (interrupt_fd_ < 0) {
    return false;
  }
  if (hidraw_) {
    return try_write_hidraw(interrupt_fd_, report);
  }
  const auto packet = hidp_output_packet(report);
  return try_write_packet(interrupt_fd_, packet);
}

void BtBackend::send_feature_get(std::uint8_t report_id) {
  if (control_fd_ < 0) {
    return;
  }
  if (hidraw_) {
    (void)try_send_feature_get(report_id);
    return;
  }
  const auto packet = feature_get_packet(report_id);
  write_bt_packet(control_fd_, packet);
}

bool BtBackend::try_send_feature_get(std::uint8_t report_id) {
  if (control_fd_ < 0) {
    return false;
  }
  if (hidraw_) {
    // hidraw answers a feature request inside the ioctl, which blocks until
    // the controller replies. Keep the reply for read_feature_report() so
    // callers see the same order as on L2CAP.
    std::array<std::uint8_t, 256> buffer{};
    buffer[0] = report_id;
    const int got =
        ::ioctl(interrupt_fd_, HIDIOCGFEATURE(buffer.size()), buffer.data());
    if (got < 0) {
      throw BluetoothLinkError("Bluetooth hidraw feature get failed: " +
                               std::string(std::strerror(errno)));
    }
    std::vector<std::uint8_t> reply{kHidpFeatureHeader};
    reply.insert(reply.end(), buffer.begin(),
                 buffer.begin() + static_cast<std::ptrdiff_t>(got));
    feature_replies_.push_back(std::move(reply));
    (void)::eventfd_write(control_fd_, 1);
    return true;
  }
  const auto packet = feature_get_packet(report_id);
  return try_write_packet(control_fd_, packet);
}

bool BtBackend::try_send_feature_set(std::span<const std::uint8_t> report) {
  if (control_fd_ < 0) {
    return false;
  }
  const auto packet = feature_set_packet(report);
  if (packet.empty()) {
    return true;
  }
  if (hidraw_) {
    // Drop the HIDP header byte; hidraw wants the report itself.
    if (::ioctl(interrupt_fd_, HIDIOCSFEATURE(packet.size() - 1),
                packet.data() + 1) < 0) {
      throw BluetoothLinkError("Bluetooth hidraw feature set failed: " +
                               std::string(std::strerror(errno)));
    }
    return true;
  }
  return try_write_packet(control_fd_, packet);
}

std::optional<std::vector<std::uint8_t>> BtBackend::read_feature_report() {
  if (hidraw_) {
    eventfd_t pending = 0;
    (void)::eventfd_read(control_fd_, &pending);
    if (feature_replies_.empty()) {
      return std::nullopt;
    }
    const auto reply = std::move(feature_replies_.front());
    feature_replies_.pop_front();
    if (!feature_replies_.empty()) {
      (void)::eventfd_write(control_fd_, 1);
    }
    return bt_feature_to_usb_feature_reply(reply);
  }
  std::array<std::uint8_t, kBtReadBufferSize> buffer{};
  const ssize_t got = ::read(control_fd_, buffer.data(), buffer.size());
  if (got < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return std::nullopt;
    }
    throw BluetoothLinkError("Bluetooth L2CAP control read failed: " +
                             std::string(std::strerror(errno)));
  }
  if (got == 0) {
    throw BluetoothLinkError("Bluetooth L2CAP control channel closed");
  }
  return bt_feature_to_usb_feature_reply(
      std::span(buffer.data(), static_cast<std::size_t>(got)));
}

std::optional<std::vector<std::uint8_t>> BtBackend::read_interrupt_packet() {
  std::array<std::uint8_t, kBtReadBufferSize> buffer{};
  const ssize_t got = ::read(interrupt_fd_, buffer.data(), buffer.size());
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
  std::vector<std::uint8_t> packet;
  if (hidraw_) {
    packet.push_back(kHidpInputHeader);
  }
  packet.insert(packet.end(), buffer.begin(),
                buffer.begin() + static_cast<std::ptrdiff_t>(got));
  return packet;
}

} // namespace vds
