// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "vds_protocol.hh"

namespace vds {

/// One haptics stream report carries this many samples per grip.
inline constexpr std::size_t kSteamStreamFrames = 31;
/// The highest rate the wireless dongle keeps up with. At 8 kHz the
/// controller reported its stream stopped three times in 800 ms.
inline constexpr unsigned kSteamStreamSampleRate = 4000;

using SteamStreamPacket = std::array<std::uint8_t, 64>;
using SteamStreamConfig = std::array<std::uint8_t, 4>;
using SteamRumbleReport = std::array<std::uint8_t, 10>;
using SteamSettingsReport = std::array<std::uint8_t, 64>;
using SteamClickReport = std::array<std::uint8_t, 4>;

/// G.711 mu-law, the sample format of the haptics stream.
std::uint8_t mulaw_encode(std::int16_t sample);

/// Converts the 2026 Steam Controller's reports into DualSense USB input
/// reports. The left and right trackpads become the two halves of the
/// DualSense touchpad.
class SteamInputTranslator {
public:
  /// A DualSense report for each controller state report. A battery report
  /// only updates the charge that later reports carry.
  std::optional<UsbInputReport> translate(std::span<const std::uint8_t> report);
  /// Empty until the controller has sent a battery report.
  std::optional<BatteryState> battery() const { return battery_; }
  /// Whether the left (0) or right (1) trackpad is pressed down.
  bool pad_clicked(std::size_t pad) const { return pad_clicked_[pad]; }

private:
  struct Touch {
    bool down = false;
    std::uint8_t id = 0;
  };

  std::optional<BatteryState> battery_;
  std::array<Touch, 2> touches_{};
  std::array<bool, 2> pad_clicked_{};
  std::uint8_t next_touch_id_ = 0;
  std::uint8_t sequence_ = 0;
};

/// True for the dongle's report that the controller switched off.
bool steam_controller_switched_off(std::span<const std::uint8_t> report);

/// The feature reports a host reads from a DualSense before it uses one:
/// calibration, pairing and firmware. The calibration states the scale
/// SteamInputTranslator converts the motion sensors to.
std::vector<std::vector<std::uint8_t>>
steam_virtual_feature_reports(std::string_view serial);

/// The rumble command for a DualSense USB output report. Empty when the
/// report leaves the rumble motors alone.
std::optional<SteamRumbleReport>
steam_rumble_report(std::span<const std::uint8_t> usb_output_report);
/// True when the rumble command drives a motor.
bool steam_rumble_active(const SteamRumbleReport &report);

/// The feature report that turns the keyboard and mouse emulation off and
/// the motion sensors on. The controller forgets it without a refresh.
SteamSettingsReport steam_settings_report();

/// Starts or stops the haptics stream on both grips.
SteamStreamConfig steam_stream_config_report(bool enable);

/// The click a trackpad plays when it is pressed down or let go. Outside
/// its keyboard and mouse emulation the controller plays none by itself.
SteamClickReport steam_pad_click_report(std::size_t pad, bool pressed);

/// Turns the haptics pair of the DualSense USB audio format into the Steam
/// Controller's haptics stream.
class SteamHapticsEncoder {
public:
  /// True when the haptics pair carried a signal.
  bool push_usb_audio(std::span<const std::uint8_t> pcm_bytes);
  std::size_t queued_frames() const { return queue_.size(); }
  /// The next stream report, filled up with silence.
  SteamStreamPacket pop_packet();
  void clear();

private:
  struct Frame {
    std::uint8_t left = 0;
    std::uint8_t right = 0;
  };

  std::array<std::uint8_t, VDS_AUDIO_CHANNELS * sizeof(std::int16_t)>
      pending_frame_{};
  std::size_t pending_frame_pos_ = 0;
  std::array<std::int32_t, 2> sum_{};
  unsigned summed_ = 0;
  std::deque<Frame> queue_;
};

} // namespace vds
