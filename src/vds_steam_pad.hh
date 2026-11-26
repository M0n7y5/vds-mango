// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "unique_fd.hh"
#include "vds_steam.hh"

namespace vds {

class SteamLinkError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// A Steam Controller that is switched on, reached through its wireless
/// dongle, and still driven by the kernel alone.
struct SteamPad {
  std::string serial;
  std::string hidraw;
  /// Only root may open the hidraw node, so no other program holds it.
  bool hidden = false;
};

/// Reads sysfs only: opening a hidraw node makes hid-steam remove and
/// re-create the controller's input devices.
std::vector<SteamPad> find_steam_pads();

/// Makes the dongles' hidraw nodes anew where their access differs from
/// hidden: 72-vds-dualsense-hide.rules decides the access of a new node, and
/// a program that held the old one loses it. Returns how many it made anew.
unsigned recreate_steam_dongle_nodes(bool hidden);

/// Drives a 2026 Steam Controller through its hidraw node: reads it as a
/// DualSense and plays DualSense rumble and haptics on its grips.
class SteamBackend {
public:
  using Clock = std::chrono::steady_clock;

  struct StreamStats {
    std::uint64_t packets = 0;
    /// Packets that carried audio but had to be filled up with silence.
    std::uint64_t starved = 0;
    /// Overruns, stops and refusals the controller reported.
    std::uint64_t faults = 0;
    /// Every status bit the controller reported: 0x01 overrun, 0x02 stopped,
    /// 0x04 wants data, 0x08 has enough, 0x10 and 0x40 refused, 0x20 accepted.
    std::uint8_t status = 0;
  };

  /// Takes the controller out of its keyboard and mouse emulation.
  SteamBackend(UniqueFd hidraw_fd, Clock::time_point now);
  ~SteamBackend();

  SteamBackend(const SteamBackend &) = delete;
  SteamBackend &operator=(const SteamBackend &) = delete;
  SteamBackend(SteamBackend &&) noexcept = default;
  SteamBackend &operator=(SteamBackend &&) noexcept = default;

  int hidraw_fd() const { return hidraw_fd_.get(); }
  /// Empty when no input waits. Throws SteamLinkError once the controller
  /// switched off.
  std::optional<UsbInputReport> read_input();
  std::optional<BatteryState> battery() const { return translator_.battery(); }
  /// Takes effect with the next sound, rumble command and click.
  void set_tuning(const SteamTuning &tuning) {
    tuning_ = tuning;
    encoder_.set_strength(tuning.haptics_strength);
  }
  void apply_usb_output_report(std::span<const std::uint8_t> report,
                               Clock::time_point now);
  void push_usb_audio(std::span<const std::uint8_t> pcm_bytes,
                      Clock::time_point now);
  /// True while more audio would play without waiting in the queue.
  bool wants_audio() const;
  /// Sends what fell due at now. Returns when it has to run next.
  Clock::time_point service(Clock::time_point now);
  Clock::time_point next_due() const;
  bool streaming() const { return streaming_; }
  /// Counts since the stream last started.
  const StreamStats &stream_stats() const { return stream_stats_; }
  /// The firmware build time from steam_firmware_build_time(). Empty until
  /// the controller answered, and for good once it failed to.
  std::optional<std::uint32_t> firmware_build_time() const {
    return firmware_build_time_;
  }

private:
  void write_report(std::span<const std::uint8_t> report);
  void send_settings();
  void read_firmware();
  void start_stream(Clock::time_point now);
  void stop_stream();
  void note_stream_status(std::span<const std::uint8_t> report);

  UniqueFd hidraw_fd_;
  SteamInputTranslator translator_;
  SteamHapticsEncoder encoder_;
  SteamTuning tuning_;
  std::optional<SteamRumbleReport> rumble_;
  StreamStats stream_stats_;
  std::optional<std::uint32_t> firmware_build_time_;
  int firmware_reads_left_;
  Clock::time_point next_settings_;
  Clock::time_point next_rumble_;
  Clock::time_point next_packet_;
  Clock::time_point last_signal_;
  std::array<bool, 2> pad_clicked_{};
  bool streaming_ = false;
  bool stream_stopped_ = false;
};

} // namespace vds
