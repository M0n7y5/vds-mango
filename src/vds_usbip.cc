// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
// Copyright (C) 2026 Antonio <bleader93@gmail.com>
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "uapi/vds.h"
#include "vds/ds5_usb.h"
#include "vds_usbip.hh"

namespace vds {

namespace {

using Clock = UsbipDevice::Clock;
using std::chrono::microseconds;

constexpr const char *kVhci = "/sys/devices/platform/vhci_hcd.0";
constexpr std::uint32_t kCmdSubmit = 1;
constexpr std::uint32_t kCmdUnlink = 2;
constexpr std::uint32_t kRetSubmit = 3;
constexpr std::uint32_t kRetUnlink = 4;
constexpr std::uint32_t kSpeedHigh = 3;
constexpr unsigned kPortFree = 4;
constexpr int kStall = -EPIPE;
constexpr std::int32_t kMaxTransferBytes = 1024 * 1024;
constexpr std::int32_t kMaxIsoPackets = 1024;
constexpr std::size_t kHidPacketSize = 64;
constexpr std::size_t kAudioOutBytesPerMs = 384;
constexpr std::size_t kAudioInBytesPerMs = 192;
constexpr std::size_t kAudioInQueueMaxBytes = 100 * kAudioInBytesPerMs;
constexpr std::int16_t kVolumeMin = -100 * 256;
constexpr auto kFeatureGetTimeout = std::chrono::seconds(5);
constexpr int kMaxFramesPerService = 64;
constexpr int kMaxHostReadsPerService = 16;

#pragma pack(push, 1)
struct UsbipHeader {
  std::uint32_t command;
  std::uint32_t seqnum;
  std::uint32_t devid;
  std::uint32_t direction;
  std::uint32_t ep;
  // CMD_SUBMIT: flags, buffer length, start frame, packets, interval.
  // RET_SUBMIT: status, actual length, start frame, packets, errors.
  // CMD_UNLINK: seqnum to unlink. RET_UNLINK: status.
  std::uint32_t field[5];
  std::uint8_t setup[8];
};
struct UsbipIsoDesc {
  std::uint32_t offset;
  std::uint32_t length;
  std::uint32_t actual_length;
  std::uint32_t status;
};
#pragma pack(pop)
static_assert(sizeof(UsbipHeader) == 48);
static_assert(sizeof(UsbipIsoDesc) == 16);

struct Profile {
  std::uint16_t product_id;
  std::string_view product;
  std::span<const std::uint8_t> configuration;
  std::span<const std::uint8_t> hid;
  std::span<const std::uint8_t> hid_report;
  unsigned hid_in_interval_us;
};

Profile profile_for(std::uint32_t profile) {
  if (profile == VDS_PROFILE_DSE) {
    return Profile{VDS_DSE_PRODUCT_ID,
                   VDS_DSE_USB_PRODUCT_STRING,
                   vds_dse_usb_configuration_descriptor,
                   vds_dse_usb_hid_descriptor,
                   vds_dse_usb_hid_report_descriptor,
                   VDS_DSE_USB_HID_IN_INTERVAL_US};
  }
  return Profile{VDS_DS5_PRODUCT_ID,
                 VDS_DS5_USB_PRODUCT_STRING,
                 vds_ds5_usb_configuration_descriptor,
                 vds_ds5_usb_hid_descriptor,
                 vds_ds5_usb_hid_report_descriptor,
                 VDS_DS5_USB_HID_IN_INTERVAL_US};
}

std::vector<std::uint8_t> string_descriptor(std::string_view text) {
  std::vector<std::uint8_t> bytes{0, 0x03};
  for (const char c : text) {
    bytes.push_back(static_cast<std::uint8_t>(c));
    bytes.push_back(0);
  }
  bytes[0] = static_cast<std::uint8_t>(bytes.size());
  return bytes;
}

std::vector<std::uint8_t> le16_bytes(std::int16_t value) {
  const auto raw = static_cast<std::uint16_t>(value);
  return {static_cast<std::uint8_t>(raw & 0xff),
          static_cast<std::uint8_t>(raw >> 8)};
}

/// The state of a high-speed vhci_hcd port, by port number.
std::vector<std::pair<unsigned, unsigned>> vhci_port_states() {
  std::ifstream status(std::string(kVhci) + "/status");
  if (!status) {
    throw std::runtime_error("vhci_hcd is not loaded (sudo modprobe vhci-hcd)");
  }
  std::vector<std::pair<unsigned, unsigned>> states;
  std::string line;
  while (std::getline(status, line)) {
    std::istringstream fields(line);
    std::string hub;
    unsigned port = 0;
    unsigned state = 0;
    if (fields >> hub >> port >> state && hub == "hs") {
      states.emplace_back(port, state);
    }
  }
  return states;
}

void write_vhci(const std::string &name, const std::string &text) {
  const std::string path = std::string(kVhci) + "/" + name;
  const UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CLOEXEC));
  if (!fd) {
    throw std::runtime_error(path + ": " + std::strerror(errno));
  }
  if (::write(fd.get(), text.data(), text.size()) < 0) {
    throw std::runtime_error(path + ": " + std::strerror(errno));
  }
}

void watch(int epoll_fd, int fd) {
  epoll_event event{};
  event.events = EPOLLIN;
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
    throw std::runtime_error(std::string("USB/IP port epoll add failed: ") +
                             std::strerror(errno));
  }
}

struct Reply {
  std::uint32_t seqnum;
  Clock::time_point due;
  std::vector<std::uint8_t> bytes;
};

struct FeatureGet {
  std::uint32_t seqnum;
  std::uint8_t report_id;
  std::uint16_t length;
  Clock::time_point deadline;
};

} // namespace

struct UsbipDevice::Impl {
  Impl(std::uint32_t profile, UniqueFd host_fd, int frame_fd)
      : profile_(profile_for(profile)), host_fd_(std::move(host_fd)),
        frame_fd_(frame_fd) {}

  std::optional<Clock::time_point> service(Clock::time_point now) {
    now_ = now;
    read_frames();
    read_host();
    return send_due();
  }

  void read_frames() {
    std::array<std::uint8_t, sizeof(vds_frame_header) + VDS_FRAME_MAX_PAYLOAD>
        frame{};
    for (int count = 0; count < kMaxFramesPerService; ++count) {
      const ssize_t got =
          ::recv(frame_fd_, frame.data(), frame.size(), MSG_DONTWAIT);
      if (got <= 0) {
        return;
      }
      vds_frame_header header{};
      if (static_cast<std::size_t>(got) < sizeof(header)) {
        continue;
      }
      std::memcpy(&header, frame.data(), sizeof(header));
      if (sizeof(header) + header.length != static_cast<std::size_t>(got)) {
        continue;
      }
      handle_frame(header.type, {frame.data() + sizeof(header), header.length});
    }
  }

  void handle_frame(std::uint16_t type, std::span<const std::uint8_t> payload) {
    switch (type) {
    case VDS_FRAME_USB_HID_IN:
      if (payload.empty() || payload.size() > kHidPacketSize) {
        break;
      }
      if (!hid_in_pending_.empty()) {
        const std::uint32_t seqnum = hid_in_pending_.front();
        hid_in_pending_.pop_front();
        schedule_hid_in(seqnum, payload);
      } else {
        // A state snapshot: keep the newest while the host is not polling.
        input_packet_.emplace(payload.begin(), payload.end());
      }
      break;
    case VDS_FRAME_USB_FEATURE_REPLY:
      if (payload.empty() || payload.size() > kHidPacketSize) {
        break;
      }
      feature_cache_[payload[0]].assign(payload.begin(), payload.end());
      for (auto it = feature_gets_.begin(); it != feature_gets_.end();) {
        if (it->report_id == payload[0]) {
          reply(it->seqnum, 0,
                payload.first(
                    std::min<std::size_t>(payload.size(), it->length)),
                0, {});
          it = feature_gets_.erase(it);
        } else {
          ++it;
        }
      }
      break;
    case VDS_FRAME_USB_AUDIO_IN:
      audio_in_.insert(audio_in_.end(), payload.begin(), payload.end());
      if (audio_in_.size() > kAudioInQueueMaxBytes) {
        audio_in_.erase(audio_in_.begin(),
                        audio_in_.begin() +
                            static_cast<std::ptrdiff_t>(audio_in_.size() -
                                                        kAudioInQueueMaxBytes));
      }
      break;
    default:
      break;
    }
  }

  void emit(std::uint16_t type, std::span<const std::uint8_t> payload) {
    std::array<std::uint8_t, sizeof(vds_frame_header) + VDS_FRAME_MAX_PAYLOAD>
        frame{};
    if (payload.size() > VDS_FRAME_MAX_PAYLOAD) {
      return;
    }
    vds_frame_header header{};
    header.type = type;
    header.length = static_cast<std::uint32_t>(payload.size());
    header.sequence = frame_sequence_++;
    std::memcpy(frame.data(), &header, sizeof(header));
    std::copy(payload.begin(), payload.end(), frame.begin() + sizeof(header));
    // Lossy by design: audio and HID OUT are realtime or state snapshots.
    (void)::send(frame_fd_, frame.data(), sizeof(header) + payload.size(),
                 MSG_DONTWAIT | MSG_NOSIGNAL);
  }

  void read_host() {
    std::array<std::uint8_t, 16384> chunk{};
    for (int count = 0; count < kMaxHostReadsPerService; ++count) {
      const ssize_t got =
          ::recv(host_fd_.get(), chunk.data(), chunk.size(), MSG_DONTWAIT);
      if (got == 0) {
        throw UsbipLinkError("vhci_hcd closed the connection");
      }
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        throw UsbipLinkError(std::string("vhci_hcd read failed: ") +
                             std::strerror(errno));
      }
      received_.insert(received_.end(), chunk.begin(), chunk.begin() + got);
      if (static_cast<std::size_t>(got) < chunk.size()) {
        break;
      }
    }

    std::size_t used = 0;
    while (const std::size_t size =
               handle_message(std::span(received_).subspan(used))) {
      used += size;
    }
    received_.erase(received_.begin(),
                    received_.begin() + static_cast<std::ptrdiff_t>(used));
  }

  void write_host(std::span<const std::uint8_t> data) {
    ssize_t sent = -1;
    do {
      sent = ::send(host_fd_.get(), data.data(), data.size(),
                    MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
      throw UsbipLinkError(std::string("vhci_hcd write failed: ") +
                           std::strerror(errno));
    }
    // A partial message would desynchronise the stream for good.
    if (static_cast<std::size_t>(sent) != data.size()) {
      throw UsbipLinkError("vhci_hcd stopped reading");
    }
  }

  /// Returns the size of the message it handled, 0 while it is incomplete.
  std::size_t handle_message(std::span<const std::uint8_t> bytes) {
    UsbipHeader header{};
    if (bytes.size() < sizeof(header)) {
      return 0;
    }
    std::memcpy(&header, bytes.data(), sizeof(header));
    const std::uint32_t command = ntohl(header.command);
    const std::uint32_t seqnum = ntohl(header.seqnum);
    if (command == kCmdUnlink) {
      unlink(seqnum, ntohl(header.field[0]));
      return sizeof(header);
    }
    if (command != kCmdSubmit) {
      throw UsbipLinkError("unexpected USB/IP command " +
                           std::to_string(command));
    }

    const bool out = ntohl(header.direction) == 0;
    const std::uint32_t ep = ntohl(header.ep);
    const auto length = static_cast<std::int32_t>(ntohl(header.field[1]));
    const auto packets = static_cast<std::int32_t>(ntohl(header.field[3]));
    if (length < 0 || length > kMaxTransferBytes || packets > kMaxIsoPackets) {
      throw UsbipLinkError("oversized USB/IP transfer");
    }
    const std::size_t data_size = out ? static_cast<std::size_t>(length) : 0;
    // Transfers other than isochronous ones carry no packet descriptors.
    const std::size_t iso_count =
        packets > 0 ? static_cast<std::size_t>(packets) : 0;
    const std::size_t size =
        sizeof(header) + data_size + iso_count * sizeof(UsbipIsoDesc);
    if (bytes.size() < size) {
      return 0;
    }
    const auto data = bytes.subspan(sizeof(header), data_size);
    std::vector<UsbipIsoDesc> iso(iso_count);
    if (iso_count > 0) {
      std::memcpy(iso.data(), bytes.data() + sizeof(header) + data_size,
                  iso_count * sizeof(UsbipIsoDesc));
    }
    for (auto &desc : iso) {
      desc.offset = ntohl(desc.offset);
      desc.length = ntohl(desc.length);
    }

    if (ep == 0) {
      control(seqnum, header.setup, data, out ? length : 0);
    } else if (ep == (VDS_USB_HID_IN_ENDPOINT & 0x0f) && !out) {
      if (input_packet_) {
        schedule_hid_in(seqnum, *input_packet_);
        input_packet_.reset();
      } else {
        hid_in_pending_.push_back(seqnum);
      }
    } else if (ep == VDS_USB_HID_OUT_ENDPOINT && out) {
      if (!data.empty() && data.size() <= kHidPacketSize) {
        emit(VDS_FRAME_USB_HID_OUT, data);
      }
      reply(seqnum, data.size() > kHidPacketSize ? kStall : 0, {}, length, {});
    } else if (ep == VDS_USB_AUDIO_OUT_ENDPOINT && out) {
      audio_out(seqnum, data, iso);
    } else if (ep == (VDS_USB_AUDIO_IN_ENDPOINT & 0x0f) && !out) {
      audio_in(seqnum, iso);
    } else {
      reply(seqnum, kStall, {}, 0, {});
    }
    return size;
  }

  std::optional<Clock::time_point> send_due() {
    std::optional<Clock::time_point> next;
    const auto later = [&](Clock::time_point when) {
      next = next ? std::min(*next, when) : when;
    };
    for (auto it = replies_.begin(); it != replies_.end();) {
      if (it->due <= now_) {
        write_host(it->bytes);
        it = replies_.erase(it);
      } else {
        later(it->due);
        ++it;
      }
    }
    for (auto it = feature_gets_.begin(); it != feature_gets_.end();) {
      if (it->deadline <= now_) {
        reply(it->seqnum, kStall, {}, 0, {});
        it = feature_gets_.erase(it);
      } else {
        later(it->deadline);
        ++it;
      }
    }
    return next;
  }

  std::vector<std::uint8_t> build(std::uint32_t seqnum, int status,
                                  std::span<const std::uint8_t> in_data,
                                  std::int32_t out_length,
                                  std::span<const UsbipIsoDesc> iso) {
    UsbipHeader ret{};
    ret.command = htonl(kRetSubmit);
    ret.seqnum = htonl(seqnum);
    std::uint32_t actual =
        status == 0 ? static_cast<std::uint32_t>(
                          in_data.empty() ? out_length : in_data.size())
                    : 0;
    std::vector<UsbipIsoDesc> wire(iso.begin(), iso.end());
    if (!wire.empty()) {
      // vhci_hcd drops the connection unless the header's length is the sum
      // of the per-packet lengths.
      actual = 0;
      for (auto &desc : wire) {
        actual += desc.actual_length;
        desc.offset = htonl(desc.offset);
        desc.length = htonl(desc.length);
        desc.actual_length = htonl(desc.actual_length);
        desc.status = 0;
      }
    }
    ret.field[0] = htonl(static_cast<std::uint32_t>(status));
    ret.field[1] = htonl(actual);
    ret.field[3] = htonl(static_cast<std::uint32_t>(wire.size()));

    std::vector<std::uint8_t> bytes(sizeof(ret));
    std::memcpy(bytes.data(), &ret, sizeof(ret));
    if (status == 0) {
      bytes.insert(bytes.end(), in_data.begin(), in_data.end());
    }
    const auto *raw = reinterpret_cast<const std::uint8_t *>(wire.data());
    bytes.insert(bytes.end(), raw, raw + wire.size() * sizeof(UsbipIsoDesc));
    return bytes;
  }

  void reply(std::uint32_t seqnum, int status,
             std::span<const std::uint8_t> in_data, std::int32_t out_length,
             std::span<const UsbipIsoDesc> iso) {
    write_host(build(seqnum, status, in_data, out_length, iso));
  }

  void schedule(std::uint32_t seqnum, std::span<const std::uint8_t> in_data,
                std::span<const UsbipIsoDesc> iso, Clock::time_point due) {
    replies_.push_back(Reply{seqnum, due, build(seqnum, 0, in_data, 0, iso)});
  }

  void schedule_hid_in(std::uint32_t seqnum,
                       std::span<const std::uint8_t> report) {
    // Games detect the polling rate, so reports leave at the real one.
    next_hid_in_ = std::max(next_hid_in_, now_) +
                   microseconds(profile_.hid_in_interval_us);
    schedule(seqnum, report, {}, next_hid_in_);
  }

  void unlink(std::uint32_t seqnum, std::uint32_t target) {
    bool found = false;
    const auto scheduled =
        std::find_if(replies_.begin(), replies_.end(),
                     [&](const Reply &item) { return item.seqnum == target; });
    if (scheduled != replies_.end()) {
      replies_.erase(scheduled);
      found = true;
    }
    const auto pending =
        std::find(hid_in_pending_.begin(), hid_in_pending_.end(), target);
    if (pending != hid_in_pending_.end()) {
      hid_in_pending_.erase(pending);
      found = true;
    }
    const auto feature = std::find_if(
        feature_gets_.begin(), feature_gets_.end(),
        [&](const FeatureGet &item) { return item.seqnum == target; });
    if (feature != feature_gets_.end()) {
      feature_gets_.erase(feature);
      found = true;
    }
    UsbipHeader ret{};
    ret.command = htonl(kRetUnlink);
    ret.seqnum = htonl(seqnum);
    // Zero tells vhci_hcd the transfer had already completed.
    ret.field[0] = htonl(static_cast<std::uint32_t>(found ? -ECONNRESET : 0));
    write_host({reinterpret_cast<const std::uint8_t *>(&ret), sizeof(ret)});
  }

  void audio_out(std::uint32_t seqnum, std::span<const std::uint8_t> data,
                 std::vector<UsbipIsoDesc> &iso) {
    std::size_t chunk_begin = 0;
    std::size_t chunk_length = 0;
    std::size_t total = 0;
    const auto flush = [&] {
      if (chunk_length > 0) {
        emit(VDS_FRAME_USB_AUDIO_OUT, data.subspan(chunk_begin, chunk_length));
      }
    };
    for (auto &desc : iso) {
      if (desc.offset > data.size() ||
          desc.length > data.size() - desc.offset) {
        desc.actual_length = 0;
        continue;
      }
      desc.actual_length = desc.length;
      total += desc.length;
      if (desc.length == 0) {
        continue;
      }
      if (chunk_length > 0 && desc.offset == chunk_begin + chunk_length &&
          chunk_length + desc.length <= VDS_FRAME_MAX_PAYLOAD) {
        chunk_length += desc.length;
        continue;
      }
      flush();
      chunk_begin = desc.offset;
      chunk_length = desc.length;
    }
    // snd-usb-audio keeps about 15 ms of URBs queued. The PCM goes to vdsd
    // now and only the completion is paced by its duration, so ALSA still
    // sees a realtime sink.
    flush();
    schedule(seqnum, {}, iso, pace(next_iso_out_, total, kAudioOutBytesPerMs));
  }

  void audio_in(std::uint32_t seqnum, std::vector<UsbipIsoDesc> &iso) {
    std::vector<std::uint8_t> data;
    for (auto &desc : iso) {
      const std::size_t bytes =
          std::min<std::size_t>(desc.length, kAudioInBytesPerMs);
      const std::size_t queued = std::min(bytes, audio_in_.size());
      desc.actual_length = static_cast<std::uint32_t>(bytes);
      data.insert(data.end(), audio_in_.begin(),
                  audio_in_.begin() + static_cast<std::ptrdiff_t>(queued));
      audio_in_.erase(audio_in_.begin(),
                      audio_in_.begin() + static_cast<std::ptrdiff_t>(queued));
      data.insert(data.end(), bytes - queued, 0);
    }
    schedule(seqnum, data, iso,
             pace(next_iso_in_, data.size(), kAudioInBytesPerMs));
  }

  Clock::time_point pace(Clock::time_point &next, std::size_t bytes,
                         std::size_t bytes_per_ms) {
    const auto duration = microseconds(std::max<std::size_t>(
        (bytes * 1000 + bytes_per_ms - 1) / bytes_per_ms, 1000));
    next = std::max(next, now_) + duration;
    return next;
  }

  void control(std::uint32_t seqnum, const std::uint8_t *setup,
               std::span<const std::uint8_t> out, std::int32_t out_length) {
    const std::uint8_t type = setup[0];
    const std::uint8_t request = setup[1];
    const auto value = static_cast<std::uint16_t>(setup[2] | (setup[3] << 8));
    const auto index = static_cast<std::uint16_t>(setup[4] | (setup[5] << 8));
    const auto length = static_cast<std::uint16_t>(setup[6] | (setup[7] << 8));
    const std::uint8_t interface = index & 0xff;
    std::vector<std::uint8_t> in_data;
    int status = kStall;

    if ((type & 0x60) == 0x00) {
      status = standard_control(request, value, index, in_data);
    } else if ((type & 0x7f) == 0x21 && interface == VDS_USB_HID_INTERFACE) {
      const bool feature = (value >> 8) == 0x03;
      const std::uint8_t report_id = value & 0xff;
      if (request == 0x01 && feature && feature_cache_[report_id].empty()) {
        // Only the physical controller can answer a cache miss.
        feature_gets_.push_back(
            FeatureGet{seqnum, report_id, length, now_ + kFeatureGetTimeout});
        const std::array<std::uint8_t, 3> get{
            report_id, static_cast<std::uint8_t>(length & 0xff),
            static_cast<std::uint8_t>(length >> 8)};
        emit(VDS_FRAME_USB_FEATURE_GET, get);
        return;
      }
      status = hid_control(request, value, out, in_data);
    } else if ((type & 0x7f) == 0x21 &&
               interface == VDS_USB_AUDIO_CONTROL_INTERFACE) {
      status = audio_control((type & 0x80) != 0, request, value, index, out,
                             in_data);
    }
    if (in_data.size() > length) {
      in_data.resize(length);
    }
    reply(seqnum, status, in_data, out_length, {});
  }

  int standard_control(std::uint8_t request, std::uint16_t value,
                       std::uint16_t index,
                       std::vector<std::uint8_t> &in_data) {
    const std::uint8_t interface = index & 0xff;
    switch (request) {
    case 0x06:
      switch (value >> 8) {
      case 0x01:
        in_data = {18,
                   0x01,
                   0x00,
                   0x02,
                   VDS_USB_DEVICE_CLASS,
                   VDS_USB_DEVICE_SUBCLASS,
                   VDS_USB_DEVICE_PROTOCOL,
                   VDS_USB_MAX_PACKET_SIZE0,
                   VDS_SONY_VENDOR_ID & 0xff,
                   VDS_SONY_VENDOR_ID >> 8,
                   static_cast<std::uint8_t>(profile_.product_id & 0xff),
                   static_cast<std::uint8_t>(profile_.product_id >> 8),
                   VDS_USB_DEVICE_BCD & 0xff,
                   VDS_USB_DEVICE_BCD >> 8,
                   1,
                   2,
                   0,
                   VDS_USB_NUM_CONFIGURATIONS};
        return 0;
      case 0x02:
        in_data.assign(profile_.configuration.begin(),
                       profile_.configuration.end());
        return 0;
      case 0x03:
        if ((value & 0xff) == 0) {
          in_data.assign(std::begin(vds_usb_language_descriptor),
                         std::end(vds_usb_language_descriptor));
        } else if ((value & 0xff) == 1) {
          in_data = string_descriptor(VDS_USB_MANUFACTURER_STRING);
        } else if ((value & 0xff) == 2) {
          in_data = string_descriptor(profile_.product);
        } else {
          return kStall;
        }
        return 0;
      case 0x06:
        in_data = {10, 0x06, 0x00, 0x02, 0, 0, 0, 64, 1, 0};
        return 0;
      case 0x21:
        in_data.assign(profile_.hid.begin(), profile_.hid.end());
        return interface == VDS_USB_HID_INTERFACE ? 0 : kStall;
      case 0x22:
        in_data.assign(profile_.hid_report.begin(), profile_.hid_report.end());
        return interface == VDS_USB_HID_INTERFACE ? 0 : kStall;
      default:
        return kStall;
      }
    case 0x00:
      in_data = {0, 0};
      return 0;
    case 0x01:
    case 0x03:
    case 0x05:
      return 0;
    case 0x09:
      altsetting_ = {};
      return 0;
    case 0x08:
      in_data = {VDS_USB_CONFIGURATION_VALUE};
      return 0;
    case 0x0a:
      if (interface >= altsetting_.size()) {
        return kStall;
      }
      in_data = {altsetting_[interface]};
      return 0;
    case 0x0b: {
      std::uint8_t interface_type = VDS_USB_INTERFACE_HID;
      if (interface == VDS_USB_AUDIO_OUT_INTERFACE) {
        interface_type = VDS_USB_INTERFACE_AUDIO_OUT;
      } else if (interface == VDS_USB_AUDIO_IN_INTERFACE) {
        interface_type = VDS_USB_INTERFACE_AUDIO_IN;
      } else if (interface != VDS_USB_HID_INTERFACE) {
        return kStall;
      }
      if (value > (interface == VDS_USB_HID_INTERFACE ? 0 : 1)) {
        return kStall;
      }
      altsetting_[interface] = static_cast<std::uint8_t>(value);
      const vds_usb_interface_event event{
          .interface_number = interface,
          .altsetting = static_cast<std::uint8_t>(value),
          .interface_type = interface_type,
          .reserved = 0,
      };
      emit(VDS_FRAME_USB_INTERFACE,
           {reinterpret_cast<const std::uint8_t *>(&event), sizeof(event)});
      return 0;
    }
    default:
      return kStall;
    }
  }

  int hid_control(std::uint8_t request, std::uint16_t value,
                  std::span<const std::uint8_t> out,
                  std::vector<std::uint8_t> &in_data) {
    const bool feature = (value >> 8) == 0x03;
    const std::uint8_t report_id = value & 0xff;
    switch (request) {
    case 0x01:
      if (feature) {
        in_data = feature_cache_[report_id];
      } else {
        in_data.assign(kHidPacketSize, 0);
        in_data[0] = report_id;
      }
      return 0;
    case 0x02:
    case 0x03:
      in_data = {0};
      return 0;
    case 0x09: {
      if (!feature || out.size() > kHidPacketSize) {
        return kStall;
      }
      if (!out.empty()) {
        std::vector<std::uint8_t> set{report_id};
        set.insert(set.end(), out.begin(), out.end());
        emit(VDS_FRAME_USB_FEATURE_SET, set);
      }
      return 0;
    }
    case 0x0a:
    case 0x0b:
      return 0;
    default:
      return kStall;
    }
  }

  int audio_control(bool in, std::uint8_t request, std::uint16_t value,
                    std::uint16_t index, std::span<const std::uint8_t> out,
                    std::vector<std::uint8_t> &in_data) {
    const std::uint8_t entity = index >> 8;
    const std::uint8_t selector = value >> 8;
    std::size_t unit = 0;
    if (entity == VDS_USB_MIC_FEATURE_UNIT) {
      unit = 1;
    } else if (entity != VDS_USB_SPEAKER_FEATURE_UNIT) {
      return kStall;
    }
    if (!in) {
      if (request != 0x01) {
        return kStall;
      }
      if (selector == 0x01 && !out.empty()) {
        mute_[unit] = out[0];
        return 0;
      }
      if (selector == 0x02 && out.size() >= 2) {
        const auto raw = static_cast<std::int16_t>(out[0] | (out[1] << 8));
        volume_[unit] = std::clamp<std::int16_t>(raw, kVolumeMin, 0);
        return 0;
      }
      return kStall;
    }
    if (selector == 0x01 && request == 0x81) {
      in_data = {mute_[unit]};
      return 0;
    }
    if (selector != 0x02) {
      return kStall;
    }
    // Signed 8.8 dB.
    switch (request) {
    case 0x81:
      in_data = le16_bytes(volume_[unit]);
      return 0;
    case 0x82:
      in_data = le16_bytes(kVolumeMin);
      return 0;
    case 0x83:
      in_data = le16_bytes(0);
      return 0;
    case 0x84:
      in_data = le16_bytes(0x0100);
      return 0;
    default:
      return kStall;
    }
  }

  Profile profile_;
  UniqueFd host_fd_;
  int frame_fd_;
  Clock::time_point now_{};
  std::uint64_t frame_sequence_ = 0;
  std::vector<std::uint8_t> received_;
  std::vector<Reply> replies_;
  std::deque<std::uint32_t> hid_in_pending_;
  std::vector<FeatureGet> feature_gets_;
  std::optional<std::vector<std::uint8_t>> input_packet_;
  std::deque<std::uint8_t> audio_in_;
  Clock::time_point next_hid_in_{};
  Clock::time_point next_iso_out_{};
  Clock::time_point next_iso_in_{};
  std::array<std::uint8_t, 4> altsetting_{};
  std::array<std::uint8_t, 2> mute_{};
  std::array<std::int16_t, 2> volume_{kVolumeMin, kVolumeMin};
  std::array<std::vector<std::uint8_t>, 256> feature_cache_;
};

UsbipDevice::UsbipDevice(std::uint32_t profile, UniqueFd host_fd, int frame_fd)
    : impl_(std::make_unique<Impl>(profile, std::move(host_fd), frame_fd)) {}

UsbipDevice::~UsbipDevice() = default;

std::optional<UsbipDevice::Clock::time_point>
UsbipDevice::service(Clock::time_point now) {
  return impl_->service(now);
}

UsbipPort::UsbipPort() {
  int sockets[2];
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                   sockets) < 0) {
    throw std::runtime_error(std::string("socketpair failed: ") +
                             std::strerror(errno));
  }
  daemon_fd_ = UniqueFd(sockets[0]);
  device_frame_fd_ = UniqueFd(sockets[1]);
  timer_fd_ =
      UniqueFd(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));
  epoll_fd_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
  if (!timer_fd_ || !epoll_fd_) {
    throw std::runtime_error(std::string("USB/IP port setup failed: ") +
                             std::strerror(errno));
  }
  watch(epoll_fd_.get(), device_frame_fd_.get());
  watch(epoll_fd_.get(), timer_fd_.get());
}

UsbipPort::~UsbipPort() { disconnect(); }

void UsbipPort::connect(std::uint32_t profile) {
  disconnect();
  const auto states = vhci_port_states();
  const auto free_port =
      std::find_if(states.begin(), states.end(),
                   [](const auto &port) { return port.second == kPortFree; });
  if (free_port == states.end()) {
    throw std::runtime_error("no free vhci_hcd port");
  }
  int sockets[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
    throw std::runtime_error(std::string("socketpair failed: ") +
                             std::strerror(errno));
  }
  // vhci_hcd takes its own reference to this end.
  const UniqueFd host_end(sockets[0]);
  UniqueFd device_end(sockets[1]);
  const unsigned port = free_port->first;
  // The device id only has to be nonzero.
  write_vhci("attach", std::to_string(port) + " " +
                           std::to_string(host_end.get()) + " " +
                           std::to_string((1u << 16) | (port + 2)) + " " +
                           std::to_string(kSpeedHigh));
  watch(epoll_fd_.get(), device_end.get());
  vhci_port_ = port;
  device_ = std::make_unique<UsbipDevice>(profile, std::move(device_end),
                                          device_frame_fd_.get());
}

void UsbipPort::disconnect() {
  if (device_) {
    try {
      write_vhci("detach", std::to_string(*vhci_port_));
    } catch (const std::exception &) {
      // vhci_hcd has already dropped the device. Closing the socket below
      // covers every other case.
    }
    device_.reset();
    arm_timer(std::nullopt);
  }
  discard_frames();
}

bool UsbipPort::unplugged() const {
  if (!vhci_port_) {
    return true;
  }
  const auto states = vhci_port_states();
  return std::none_of(states.begin(), states.end(), [&](const auto &port) {
    return port.first == *vhci_port_ && port.second != kPortFree;
  });
}

void UsbipPort::service() {
  std::uint64_t expirations = 0;
  (void)!::read(timer_fd_.get(), &expirations, sizeof(expirations));
  if (!device_) {
    discard_frames();
    return;
  }
  try {
    arm_timer(device_->service(UsbipDevice::Clock::now()));
  } catch (const UsbipLinkError &) {
    disconnect();
    throw;
  }
}

void UsbipPort::arm_timer(std::optional<UsbipDevice::Clock::time_point> due) {
  itimerspec spec{};
  if (due) {
    const auto since_boot =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            due->time_since_epoch())
            .count();
    spec.it_value.tv_sec = since_boot / 1000000000;
    // All zero would disarm the timer.
    spec.it_value.tv_nsec = std::max<long>(since_boot % 1000000000, 1);
  }
  if (::timerfd_settime(timer_fd_.get(), TFD_TIMER_ABSTIME, &spec, nullptr) <
      0) {
    throw std::runtime_error(std::string("timerfd_settime failed: ") +
                             std::strerror(errno));
  }
}

void UsbipPort::discard_frames() {
  std::array<std::uint8_t, 64> frame{};
  while (::recv(device_frame_fd_.get(), frame.data(), frame.size(),
                MSG_DONTWAIT) > 0) {
  }
}

bool usbip_host_available() {
  return ::access((std::string(kVhci) + "/attach").c_str(), F_OK) == 0;
}

} // namespace vds
