// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include "vds_check.hh"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>

#include "uapi/vds.h"
#include "unique_fd.hh"
#include "vds_usbip.hh"

namespace vds::test {

namespace {

using Clock = vds::UsbipDevice::Clock;
using Bytes = std::vector<std::uint8_t>;
using std::chrono::milliseconds;

constexpr Clock::time_point kStart{std::chrono::seconds(100)};
constexpr std::uint32_t kHidInEndpoint = 4;
constexpr std::uint32_t kAudioOutEndpoint = 1;
constexpr std::uint32_t kAudioInEndpoint = 2;
constexpr std::uint16_t kHidInterface = 3;

struct Reply {
  std::uint32_t command = 0;
  std::uint32_t seqnum = 0;
  std::int32_t status = 0;
  std::uint32_t length = 0;
  Bytes data;
};

struct Frame {
  std::uint16_t type = 0;
  Bytes payload;
};

// Plays vhci_hcd on one socket and vdsd on the other.
class Host {
public:
  explicit Host(std::uint32_t profile = VDS_PROFILE_DS5) {
    int host[2];
    int frames[2];
    VDS_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, host) == 0);
    VDS_CHECK(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, frames) == 0);
    host_ = vds::UniqueFd(host[0]);
    vdsd_ = vds::UniqueFd(frames[0]);
    device_frames_ = vds::UniqueFd(frames[1]);
    device_.emplace(profile, vds::UniqueFd(host[1]), device_frames_.get());
  }

  std::optional<Clock::time_point> service(Clock::duration after = {}) {
    return device_->service(kStart + after);
  }

  void write_host(const Bytes &bytes) {
    VDS_CHECK(::send(host_.get(), bytes.data(), bytes.size(), 0) ==
              static_cast<ssize_t>(bytes.size()));
  }

  Bytes submit_bytes(std::uint32_t seqnum, std::uint32_t endpoint, bool in,
                     const std::array<std::uint8_t, 8> &setup,
                     std::uint32_t length, const Bytes &out,
                     const std::vector<std::uint32_t> &packets) {
    in_[seqnum] = in;
    const std::uint32_t words[10] = {
        1,      seqnum, 0x10002, in ? 1u : 0u, endpoint, 0,
        length, 0,      static_cast<std::uint32_t>(packets.size()),
        0,
    };
    Bytes bytes;
    for (const std::uint32_t word : words) {
      append_be32(bytes, word);
    }
    bytes.insert(bytes.end(), setup.begin(), setup.end());
    bytes.insert(bytes.end(), out.begin(), out.end());
    std::uint32_t offset = 0;
    for (const std::uint32_t packet : packets) {
      append_be32(bytes, offset);
      append_be32(bytes, packet);
      append_be32(bytes, 0);
      append_be32(bytes, 0);
      offset += packet;
    }
    return bytes;
  }

  void control_in(std::uint32_t seqnum, std::uint8_t type, std::uint8_t request,
                  std::uint16_t value, std::uint16_t index,
                  std::uint16_t length) {
    const std::array<std::uint8_t, 8> setup{
        type,
        request,
        static_cast<std::uint8_t>(value & 0xff),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(index & 0xff),
        static_cast<std::uint8_t>(index >> 8),
        static_cast<std::uint8_t>(length & 0xff),
        static_cast<std::uint8_t>(length >> 8),
    };
    write_host(submit_bytes(seqnum, 0, true, setup, length, {}, {}));
  }

  void get_device_descriptor(std::uint32_t seqnum) {
    control_in(seqnum, 0x80, 0x06, 0x0100, 0, 18);
  }

  void get_feature_report(std::uint32_t seqnum, std::uint8_t report_id) {
    control_in(seqnum, 0xa1, 0x01,
               static_cast<std::uint16_t>(0x0300 | report_id), kHidInterface,
               64);
  }

  void poll_input(std::uint32_t seqnum) {
    write_host(submit_bytes(seqnum, kHidInEndpoint, true, {}, 64, {}, {}));
  }

  void play(std::uint32_t seqnum, const Bytes &pcm,
            const std::vector<std::uint32_t> &packets) {
    write_host(submit_bytes(seqnum, kAudioOutEndpoint, false, {},
                            static_cast<std::uint32_t>(pcm.size()), pcm,
                            packets));
  }

  void capture(std::uint32_t seqnum,
               const std::vector<std::uint32_t> &packets) {
    std::uint32_t length = 0;
    for (const std::uint32_t packet : packets) {
      length += packet;
    }
    write_host(
        submit_bytes(seqnum, kAudioInEndpoint, true, {}, length, {}, packets));
  }

  void unlink(std::uint32_t seqnum, std::uint32_t target) {
    Bytes bytes;
    for (const std::uint32_t word : {2u, seqnum, 0x10002u, 0u, 0u, target}) {
      append_be32(bytes, word);
    }
    bytes.resize(48, 0);
    write_host(bytes);
  }

  void close_host() { host_.reset(); }

  std::optional<Reply> reply() {
    std::uint8_t header[48];
    const ssize_t got =
        ::recv(host_.get(), header, sizeof(header), MSG_DONTWAIT);
    if (got != static_cast<ssize_t>(sizeof(header))) {
      return std::nullopt;
    }
    Reply reply;
    reply.command = be32(header + 0);
    reply.seqnum = be32(header + 4);
    reply.status = static_cast<std::int32_t>(be32(header + 20));
    reply.length = be32(header + 24);
    const std::uint32_t packets = be32(header + 32);
    if (reply.command == 3 && in_[reply.seqnum] && reply.status == 0) {
      reply.data = read_host(reply.length);
    }
    (void)read_host(packets * 16);
    return reply;
  }

  void send_frame(std::uint16_t type, const Bytes &payload) {
    vds_frame_header header{};
    header.type = type;
    header.length = static_cast<std::uint32_t>(payload.size());
    Bytes bytes(sizeof(header));
    std::memcpy(bytes.data(), &header, sizeof(header));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    VDS_CHECK(::send(vdsd_.get(), bytes.data(), bytes.size(), 0) ==
              static_cast<ssize_t>(bytes.size()));
  }

  std::optional<Frame> frame() {
    std::array<std::uint8_t, sizeof(vds_frame_header) + VDS_FRAME_MAX_PAYLOAD>
        bytes{};
    const ssize_t got =
        ::recv(vdsd_.get(), bytes.data(), bytes.size(), MSG_DONTWAIT);
    if (got < static_cast<ssize_t>(sizeof(vds_frame_header))) {
      return std::nullopt;
    }
    vds_frame_header header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    return Frame{header.type, Bytes(bytes.begin() + sizeof(header),
                                    bytes.begin() + got)};
  }

private:
  static void append_be32(Bytes &bytes, std::uint32_t value) {
    const std::uint32_t wire = htonl(value);
    const auto *raw = reinterpret_cast<const std::uint8_t *>(&wire);
    bytes.insert(bytes.end(), raw, raw + sizeof(wire));
  }

  static std::uint32_t be32(const std::uint8_t *bytes) {
    std::uint32_t wire = 0;
    std::memcpy(&wire, bytes, sizeof(wire));
    return ntohl(wire);
  }

  Bytes read_host(std::size_t size) {
    Bytes bytes(size);
    // A zero-length read on an empty stream socket reports EAGAIN.
    if (size > 0) {
      VDS_CHECK(::recv(host_.get(), bytes.data(), size, MSG_DONTWAIT) ==
                static_cast<ssize_t>(size));
    }
    return bytes;
  }

  vds::UniqueFd host_;
  vds::UniqueFd vdsd_;
  vds::UniqueFd device_frames_;
  std::optional<vds::UsbipDevice> device_;
  std::map<std::uint32_t, bool> in_;
};

Bytes input_report(std::uint8_t marker) {
  Bytes report(64, 0);
  report[0] = 0x01;
  report[1] = marker;
  return report;
}

} // namespace

void test_device_descriptor_carries_the_dualsense_usb_ids() {
  Host host;
  host.get_device_descriptor(1);
  host.service();
  const auto reply = host.reply();
  VDS_CHECK(reply.has_value());
  // hid-playstation binds by these: vendor 054c, product 0ce6.
  VDS_CHECK(Bytes(reply->data.begin() + 8, reply->data.begin() + 12) ==
            (Bytes{0x4c, 0x05, 0xe6, 0x0c}));
}

void test_edge_profile_carries_the_dualsense_edge_product_id() {
  Host host(VDS_PROFILE_DSE);
  host.get_device_descriptor(1);
  host.service();
  const auto reply = host.reply();
  VDS_CHECK(reply.has_value());
  VDS_CHECK(Bytes(reply->data.begin() + 10, reply->data.begin() + 12) ==
            (Bytes{0xf2, 0x0d}));
}

void test_played_audio_reaches_vdsd_when_it_is_submitted() {
  Host host;
  host.play(1, Bytes(768, 0x22), {384, 384});
  host.service();
  const auto frame = host.frame();
  VDS_CHECK(frame.has_value());
  VDS_CHECK_EQ(frame->type,
               static_cast<std::uint16_t>(VDS_FRAME_USB_AUDIO_OUT));
  VDS_CHECK(frame->payload == Bytes(768, 0x22));
}

void test_played_audio_completes_after_its_own_duration() {
  Host host;
  // Two packets of 1 ms each: 384 bytes are 96 frames of 4-channel S16.
  host.play(1, Bytes(768, 0x22), {384, 384});
  const auto due = host.service();
  VDS_CHECK(!host.reply().has_value());
  VDS_CHECK(due == kStart + milliseconds(2));
  host.service(milliseconds(2));
  const auto reply = host.reply();
  VDS_CHECK(reply.has_value());
  VDS_CHECK_EQ(reply->length, 768u);
}

void test_input_report_completes_one_polling_interval_after_it_arrives() {
  Host host;
  host.poll_input(1);
  host.send_frame(VDS_FRAME_USB_HID_IN, input_report(0x10));
  const auto due = host.service();
  VDS_CHECK(!host.reply().has_value());
  VDS_CHECK(due == kStart + milliseconds(4));
  host.service(milliseconds(4));
  const auto reply = host.reply();
  VDS_CHECK(reply.has_value());
  VDS_CHECK(reply->data == input_report(0x10));
}

void test_newest_input_report_wins_while_the_host_is_not_polling() {
  Host host;
  host.send_frame(VDS_FRAME_USB_HID_IN, input_report(0x10));
  host.send_frame(VDS_FRAME_USB_HID_IN, input_report(0x20));
  host.service();
  host.poll_input(1);
  host.service();
  host.service(milliseconds(4));
  const auto reply = host.reply();
  VDS_CHECK(reply.has_value());
  VDS_CHECK(reply->data == input_report(0x20));
}

void test_uncached_feature_report_is_requested_from_vdsd() {
  Host host;
  host.get_feature_report(1, 0x05);
  host.service();
  const auto frame = host.frame();
  VDS_CHECK(frame.has_value());
  VDS_CHECK_EQ(frame->type,
               static_cast<std::uint16_t>(VDS_FRAME_USB_FEATURE_GET));
  VDS_CHECK(frame->payload == (Bytes{0x05, 64, 0}));
}

void test_feature_report_from_vdsd_answers_the_waiting_request() {
  Host host;
  host.get_feature_report(1, 0x05);
  host.service();
  host.send_frame(VDS_FRAME_USB_FEATURE_REPLY, Bytes{0x05, 0xaa, 0xbb});
  host.service();
  const auto reply = host.reply();
  VDS_CHECK(reply.has_value());
  VDS_CHECK(reply->data == (Bytes{0x05, 0xaa, 0xbb}));
}

void test_feature_request_stalls_when_vdsd_never_answers() {
  Host host;
  host.get_feature_report(1, 0x05);
  host.service();
  host.service(std::chrono::seconds(5));
  const auto reply = host.reply();
  VDS_CHECK(reply.has_value());
  VDS_CHECK_EQ(reply->status, -EPIPE);
}

void test_unlinking_a_waiting_input_poll_reports_it_cancelled() {
  Host host;
  host.poll_input(1);
  host.unlink(2, 1);
  host.service();
  const auto reply = host.reply();
  VDS_CHECK(reply.has_value());
  VDS_CHECK_EQ(reply->seqnum, 2u);
  VDS_CHECK_EQ(reply->status, -ECONNRESET);
}

void test_request_split_across_two_reads_is_answered_once_complete() {
  Host host;
  Bytes request = host.submit_bytes(
      1, 0, true, {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0}, 18, {}, {});
  host.write_host(Bytes(request.begin(), request.begin() + 20));
  host.service();
  VDS_CHECK(!host.reply().has_value());
  host.write_host(Bytes(request.begin() + 20, request.end()));
  host.service();
  VDS_CHECK(host.reply().has_value());
}

void test_closed_host_connection_is_reported() {
  Host host;
  host.close_host();
  bool lost = false;
  try {
    host.service();
  } catch (const vds::UsbipLinkError &) {
    lost = true;
  }
  VDS_CHECK(lost);
}

void test_microphone_audio_from_vdsd_fills_the_capture_packet() {
  Host host;
  host.send_frame(VDS_FRAME_USB_AUDIO_IN, Bytes(192, 0x11));
  host.capture(1, {192});
  host.service();
  host.service(milliseconds(1));
  const auto reply = host.reply();
  VDS_CHECK(reply.has_value());
  VDS_CHECK(reply->data == Bytes(192, 0x11));
}

void test_capture_packet_is_silence_without_microphone_audio() {
  Host host;
  host.capture(1, {192});
  host.service();
  host.service(milliseconds(1));
  const auto reply = host.reply();
  VDS_CHECK(reply.has_value());
  VDS_CHECK(reply->data == Bytes(192, 0x00));
}

void run_tests() {
  test_device_descriptor_carries_the_dualsense_usb_ids();
  test_edge_profile_carries_the_dualsense_edge_product_id();
  test_played_audio_reaches_vdsd_when_it_is_submitted();
  test_played_audio_completes_after_its_own_duration();
  test_input_report_completes_one_polling_interval_after_it_arrives();
  test_newest_input_report_wins_while_the_host_is_not_polling();
  test_uncached_feature_report_is_requested_from_vdsd();
  test_feature_report_from_vdsd_answers_the_waiting_request();
  test_feature_request_stalls_when_vdsd_never_answers();
  test_unlinking_a_waiting_input_poll_reports_it_cancelled();
  test_request_split_across_two_reads_is_answered_once_complete();
  test_closed_host_connection_is_reported();
  test_microphone_audio_from_vdsd_fills_the_capture_packet();
  test_capture_packet_is_silence_without_microphone_audio();
}

} // namespace vds::test
