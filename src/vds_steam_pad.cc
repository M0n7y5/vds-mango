// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vds_steam_pad.hh"

namespace vds {

namespace {

constexpr std::string_view kDongleHidId = "HID_ID=0003:000028DE:00001304";
constexpr const char *kUsbHidDriver = "/sys/bus/usb/drivers/usbhid";
/* hid-steam's name for the gamepad of a controller behind a dongle. */
constexpr std::string_view kGamepadName = "Wireless Steam Controller";
constexpr std::size_t kReadBufferSize = 128;
constexpr std::uint8_t kStreamStatusReportId = 0x44;
constexpr std::uint8_t kStreamOverrun = 0x01;
constexpr std::uint8_t kStreamStopped = 0x02;
constexpr std::uint8_t kStreamRejected = 0x10;
/* SDL refreshes the settings this often. The controller drops them. */
constexpr auto kSettingsInterval = std::chrono::seconds(3);
/* Rumble stops by itself unless repeated. SDL repeats it this often. */
constexpr auto kRumbleInterval = std::chrono::milliseconds(40);
/* An idle stream costs 129 radio packets per second. */
constexpr auto kStreamIdleTimeout = std::chrono::seconds(2);
constexpr auto kStreamPeriod = std::chrono::microseconds(
    kSteamStreamFrames * 1'000'000 / kSteamStreamSampleRate);
constexpr int kMaxLatePackets = 3;
/*
 * A reply to another command, such as hid-steam's own serial request,
 * waits for the next settings refresh to try again. So does EPIPE, which
 * hid-steam's own feature exchanges retry (steam_send_report_id() and
 * steam_recv_report_id() in drivers/hid/hid-steam.c). Any other failure
 * ends the reads: usbhid waits up to 5 s per transfer, so a controller
 * that does not answer blocks the loop once.
 */
constexpr int kFirmwareReads = 3;

std::string read_first_line(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

bool is_dongle_node(const std::filesystem::path &hid_device) {
  std::ifstream uevent(hid_device / "uevent");
  std::string line;
  while (std::getline(uevent, line)) {
    if (line == kDongleHidId) {
      return true;
    }
  }
  return false;
}

std::string device_node(const std::filesystem::directory_entry &hidraw) {
  return "/dev/" + hidraw.path().filename().string();
}

bool node_is_hidden(const std::string &node) {
  struct stat status {};
  return ::stat(node.c_str(), &status) == 0 &&
         (status.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

/* The USB interface a hidraw node of the dongle sits on. */
std::filesystem::path
usb_interface(const std::filesystem::directory_entry &hidraw) {
  std::error_code error;
  return std::filesystem::canonical(hidraw.path() / "device", error)
      .parent_path();
}

bool write_sysfs(const std::filesystem::path &path, const std::string &text) {
  std::ofstream file(path);
  file << text;
  file.flush();
  return file.good();
}

} // namespace

std::vector<SteamPad> find_steam_pads() {
  std::vector<SteamPad> pads;
  std::error_code error;
  for (const auto &entry :
       std::filesystem::directory_iterator("/sys/class/hidraw", error)) {
    if (!is_dongle_node(entry.path() / "device")) {
      continue;
    }
    /*
     * hid-steam gives the hidraw node and the controller's input devices
     * two HID devices on the same USB interface.
     */
    for (const auto &hid : std::filesystem::directory_iterator(
             usb_interface(entry), error)) {
      for (const auto &input :
           std::filesystem::directory_iterator(hid.path() / "input", error)) {
        const std::string serial = read_first_line(input.path() / "uniq");
        if (read_first_line(input.path() / "name") != kGamepadName ||
            serial.empty()) {
          continue;
        }
        pads.push_back(SteamPad{
            .serial = serial,
            .hidraw = device_node(entry),
            .hidden = node_is_hidden(device_node(entry)),
        });
      }
    }
  }
  return pads;
}

unsigned recreate_steam_dongle_nodes(bool hidden) {
  /* Unbinding removes entries from the directory, so collect them first. */
  std::vector<std::string> interfaces;
  std::error_code error;
  for (const auto &entry :
       std::filesystem::directory_iterator("/sys/class/hidraw", error)) {
    if (is_dongle_node(entry.path() / "device") &&
        node_is_hidden(device_node(entry)) != hidden) {
      interfaces.push_back(usb_interface(entry).filename().string());
    }
  }
  unsigned recreated = 0;
  const std::filesystem::path driver(kUsbHidDriver);
  for (const auto &interface : interfaces) {
    if (write_sysfs(driver / "unbind", interface) &&
        write_sysfs(driver / "bind", interface)) {
      ++recreated;
    }
  }
  return recreated;
}

SteamBackend::SteamBackend(UniqueFd hidraw_fd, Clock::time_point now)
    : hidraw_fd_(std::move(hidraw_fd)), firmware_reads_left_(kFirmwareReads),
      next_settings_(now + kSettingsInterval), next_rumble_(now),
      next_packet_(now), last_signal_(now) {
  send_settings();
  /* A stream left running by a crashed owner would refuse a new start. */
  write_report(steam_stream_config_report(false));
}

SteamBackend::~SteamBackend() {
  if (!hidraw_fd_) {
    return;
  }
  try {
    if (streaming_) {
      write_report(steam_stream_config_report(false));
    }
    if (rumble_) {
      write_report(SteamRumbleReport{(*rumble_)[0]});
    }
  } catch (const SteamLinkError &) {
    /* The controller is gone, and with it what was left running. */
  }
}

void SteamBackend::write_report(std::span<const std::uint8_t> report) {
  while (true) {
    const ssize_t written =
        ::write(hidraw_fd_.get(), report.data(), report.size());
    if (written == static_cast<ssize_t>(report.size())) {
      return;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    throw SteamLinkError("Steam Controller write failed: " +
                         std::string(written < 0 ? std::strerror(errno)
                                                 : "short write"));
  }
}

void SteamBackend::send_settings() {
  SteamSettingsReport report = steam_settings_report();
  if (::ioctl(hidraw_fd_.get(), HIDIOCSFEATURE(report.size()),
              report.data()) < 0) {
    throw SteamLinkError("Steam Controller settings failed: " +
                         std::string(std::strerror(errno)));
  }
}

void SteamBackend::read_firmware() {
  if (firmware_build_time_ || firmware_reads_left_ == 0) {
    return;
  }
  --firmware_reads_left_;
  const SteamAttributesRequest request = steam_attributes_request();
  SteamAttributesRequest reply{request[0]};
  if (::ioctl(hidraw_fd_.get(), HIDIOCSFEATURE(request.size()),
              request.data()) < 0) {
    if (errno != EPIPE) {
      firmware_reads_left_ = 0;
    }
    return;
  }
  const int got =
      ::ioctl(hidraw_fd_.get(), HIDIOCGFEATURE(reply.size()), reply.data());
  if (got < 0) {
    if (errno != EPIPE) {
      firmware_reads_left_ = 0;
    }
    return;
  }
  firmware_build_time_ = steam_firmware_build_time(
      std::span(reply).first(static_cast<std::size_t>(got)));
}

std::optional<UsbInputReport> SteamBackend::read_input() {
  std::array<std::uint8_t, kReadBufferSize> buffer{};
  while (true) {
    const ssize_t got = ::read(hidraw_fd_.get(), buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return std::nullopt;
      }
      if (errno == EINTR) {
        continue;
      }
      throw SteamLinkError("Steam Controller read failed: " +
                           std::string(std::strerror(errno)));
    }
    if (got == 0) {
      throw SteamLinkError("Steam Controller hidraw node closed");
    }
    const std::span report(buffer.data(), static_cast<std::size_t>(got));
    if (steam_controller_switched_off(report)) {
      throw SteamLinkError("the controller switched off");
    }
    if (report[0] == kStreamStatusReportId) {
      note_stream_status(report);
      continue;
    }
    const auto usb = translator_.translate(report);
    if (!usb) {
      continue;
    }
    for (std::size_t pad = 0; pad < pad_clicked_.size(); ++pad) {
      const bool clicked = translator_.pad_clicked(pad);
      if (clicked != pad_clicked_[pad]) {
        pad_clicked_[pad] = clicked;
        const auto click =
            steam_pad_click_report(pad, clicked, tuning_.pad_click);
        if (click) {
          write_report(*click);
        }
      }
    }
    return usb;
  }
}

void SteamBackend::note_stream_status(std::span<const std::uint8_t> report) {
  /* The stop this side asked for is reported the same way. */
  if (!streaming_ || report.size() < 3) {
    return;
  }
  const std::uint8_t status = report[2];
  stream_stats_.status |= status;
  if ((status & (kStreamOverrun | kStreamStopped | kStreamRejected)) != 0) {
    ++stream_stats_.faults;
  }
  stream_stopped_ |= (status & kStreamStopped) != 0;
}

void SteamBackend::apply_usb_output_report(
    std::span<const std::uint8_t> report, Clock::time_point now) {
  const auto rumble = steam_rumble_report(report, tuning_.rumble_strength);
  if (!rumble) {
    return;
  }
  write_report(*rumble);
  if (steam_rumble_active(*rumble)) {
    rumble_ = rumble;
    next_rumble_ = now + kRumbleInterval;
  } else {
    rumble_.reset();
  }
}

void SteamBackend::push_usb_audio(std::span<const std::uint8_t> pcm_bytes,
                                  Clock::time_point now) {
  if (encoder_.push_usb_audio(pcm_bytes)) {
    last_signal_ = now;
    if (!streaming_) {
      start_stream(now);
    }
  } else if (!streaming_) {
    encoder_.clear();
  }
}

bool SteamBackend::wants_audio() const {
  return encoder_.queued_frames() < kSteamStreamFrames;
}

void SteamBackend::start_stream(Clock::time_point now) {
  write_report(steam_stream_config_report(true));
  streaming_ = true;
  stream_stopped_ = false;
  stream_stats_ = {};
  /* The first packet waits for a full packet of audio to arrive. */
  next_packet_ = now + kStreamPeriod;
}

void SteamBackend::stop_stream() {
  write_report(steam_stream_config_report(false));
  streaming_ = false;
  encoder_.clear();
}

SteamBackend::Clock::time_point SteamBackend::service(Clock::time_point now) {
  if (now >= next_settings_) {
    read_firmware();
    send_settings();
    next_settings_ = now + kSettingsInterval;
  }
  if (rumble_ && now >= next_rumble_) {
    write_report(*rumble_);
    next_rumble_ = now + kRumbleInterval;
  }
  if (streaming_ && now - last_signal_ >= kStreamIdleTimeout) {
    stop_stream();
  }
  if (streaming_) {
    if (stream_stopped_) {
      /* An underrun stopped it. A start it does not need is refused. */
      write_report(steam_stream_config_report(true));
      stream_stopped_ = false;
    }
    /* After a stall, resume in step instead of flooding the controller. */
    if (now - next_packet_ > kMaxLatePackets * kStreamPeriod) {
      next_packet_ = now;
    }
    while (now >= next_packet_) {
      const std::size_t queued = encoder_.queued_frames();
      if (queued > 0 && queued < kSteamStreamFrames) {
        ++stream_stats_.starved;
      }
      write_report(encoder_.pop_packet());
      ++stream_stats_.packets;
      next_packet_ += kStreamPeriod;
    }
  }
  return next_due();
}

SteamBackend::Clock::time_point SteamBackend::next_due() const {
  Clock::time_point due = next_settings_;
  if (rumble_) {
    due = std::min(due, next_rumble_);
  }
  if (streaming_) {
    due = std::min(due, next_packet_);
  }
  return due;
}

} // namespace vds
