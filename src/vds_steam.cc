// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include "vds_steam.hh"

#include <algorithm>
#include <cstdlib>

namespace vds {

namespace {

/*
 * Steam Controller report layouts as Valve published them in SDL
 * (controller_structs.h, zlib license), offsets counted from the report ID.
 */
constexpr std::uint8_t kStateReportId = 0x42;
constexpr std::uint8_t kBatteryReportId = 0x43;
constexpr std::uint8_t kWirelessStatusReportId = 0x79;
constexpr std::uint8_t kWirelessDisconnected = 1;
constexpr std::size_t kStateButtonsOffset = 2;
constexpr std::size_t kStateTriggersOffset = 6;
constexpr std::size_t kStateSticksOffset = 10;
constexpr std::size_t kStatePadsOffset = 18;
constexpr std::size_t kStatePadSize = 6;
constexpr std::size_t kStateSensorTimeOffset = 30;
constexpr std::size_t kStateAccelOffset = 34;
constexpr std::size_t kStateGyroOffset = 40;
constexpr std::size_t kStateReportSize = 46;
constexpr std::uint8_t kChargeDischarging = 1;
constexpr std::uint8_t kChargeCharging = 2;
constexpr std::uint8_t kChargeDone = 4;

constexpr std::uint32_t kButtonA = 0x00000001;
constexpr std::uint32_t kButtonB = 0x00000002;
constexpr std::uint32_t kButtonX = 0x00000004;
constexpr std::uint32_t kButtonY = 0x00000008;
constexpr std::uint32_t kButtonRightStick = 0x00000020;
/* SDL calls this bit View and 0x4000 Menu; on the controller it is the
 * other way round: the left button, View, sets 0x4000. */
constexpr std::uint32_t kButtonMenu = 0x00000040;
constexpr std::uint32_t kButtonRightBumper = 0x00000200;
constexpr std::uint32_t kButtonDpadDown = 0x00000400;
constexpr std::uint32_t kButtonDpadRight = 0x00000800;
constexpr std::uint32_t kButtonDpadLeft = 0x00001000;
constexpr std::uint32_t kButtonDpadUp = 0x00002000;
constexpr std::uint32_t kButtonView = 0x00004000;
constexpr std::uint32_t kButtonLeftStick = 0x00008000;
constexpr std::uint32_t kButtonSteam = 0x00010000;
constexpr std::uint32_t kButtonLeftBumper = 0x00080000;
constexpr std::uint32_t kButtonRightPadTouch = 0x00200000;
constexpr std::uint32_t kButtonRightPadClick = 0x00400000;
constexpr std::uint32_t kButtonLeftPadTouch = 0x02000000;
constexpr std::uint32_t kButtonLeftPadClick = 0x04000000;

/* DualSense USB input report, as hid-playstation reads it. */
constexpr std::size_t kDsSticksOffset = 1;
constexpr std::size_t kDsTriggersOffset = 5;
constexpr std::size_t kDsSequenceOffset = 7;
constexpr std::size_t kDsButtonsOffset = 8;
constexpr std::size_t kDsGyroOffset = 16;
constexpr std::size_t kDsAccelOffset = 22;
constexpr std::size_t kDsSensorTimeOffset = 28;
constexpr std::size_t kDsTouchOffset = 33;
constexpr std::size_t kDsTouchSize = 4;
constexpr std::size_t kDsBatteryOffset = 53;
constexpr std::uint8_t kDsTouchInactive = 0x80;
constexpr unsigned kDsTouchWidth = 1920;
constexpr unsigned kDsTouchHeight = 1080;
constexpr std::uint8_t kDsHatCentered = 8;
constexpr std::uint8_t kDsBatteryFull = 0x0a;
constexpr std::uint8_t kDsBatteryCharging = 0x10;
constexpr std::uint8_t kDsBatteryCharged = 0x20;

/* The DualSense counts 8192 per g, the Steam Controller 16384. */
constexpr int kAccelDivisor = 2;
constexpr std::int16_t kDsAccelPerG = 8192;
/* Both count 2000 degrees per second over the int16 range. */
constexpr std::int16_t kDsGyroCalibrationSpeed = 540;
constexpr std::int16_t kDsGyroCalibrationCounts = 8847;
/* The DualSense sensor clock ticks three times per microsecond. */
constexpr std::uint32_t kDsSensorTicksPerMicrosecond = 3;

constexpr std::size_t kDsCalibrationReportSize = 41;
constexpr std::size_t kDsPairingReportSize = 20;
constexpr std::size_t kDsFirmwareReportSize = 64;
constexpr std::uint8_t kDsCalibrationReportId = 0x05;
constexpr std::uint8_t kDsPairingReportId = 0x09;
constexpr std::size_t kDsPairingAddressOffset = 1;
constexpr std::size_t kDsAddressSize = 6;

constexpr std::uint8_t kDsOutputReportId = 0x02;
constexpr std::size_t kDsOutputFlags0Offset = 1;
constexpr std::size_t kDsOutputRightMotorOffset = 3;
constexpr std::size_t kDsOutputLeftMotorOffset = 4;
constexpr std::size_t kDsOutputFlags2Offset = 39;
constexpr std::uint8_t kDsOutputFlags0Rumble = 0x03;
constexpr std::uint8_t kDsOutputFlags2Rumble = 0x04;

constexpr std::uint8_t kRumbleReportId = 0x80;
constexpr std::size_t kRumbleLeftOffset = 4;
constexpr std::size_t kRumbleRightOffset = 7;
constexpr std::uint8_t kClickReportId = 0x82;
constexpr std::uint8_t kClick = 1;
constexpr std::uint8_t kClickStrong = 2;
constexpr std::uint8_t kStreamConfigReportId = 0x86;
constexpr std::uint8_t kStreamDataReportId = 0x88;
constexpr std::uint8_t kStreamStop = 1;
constexpr std::uint8_t kStreamStart = 2;
constexpr std::uint8_t kStreamTargetGrips = 2;
constexpr std::uint8_t kStreamMode4kHzMulaw = 9;
constexpr std::uint8_t kMulawSilence = 0xff;
constexpr std::size_t kStreamLeftOffset = 2;
constexpr std::size_t kStreamRightOffset = 33;

constexpr std::uint8_t kFeatureReportId = 0x01;
constexpr std::uint8_t kSetSettingsValues = 0x87;
constexpr std::uint8_t kSettingLizardMode = 9;
constexpr std::uint8_t kSettingImuMode = 48;
constexpr std::uint8_t kImuRawAccelAndGyro = 0x18;

constexpr unsigned kDecimation = VDS_AUDIO_SAMPLE_RATE / kSteamStreamSampleRate;
static_assert(VDS_AUDIO_SAMPLE_RATE % kSteamStreamSampleRate == 0);
constexpr std::size_t kHapticsFirstChannel = 2;
/* Bounds how far the grips can fall behind the game: 23 ms. */
constexpr std::size_t kMaxQueuedFrames = 3 * kSteamStreamFrames;
/*
 * For the same signal the grips shake harder than a DualSense's. At 80% a
 * player found Ratchet & Clank to feel right.
 */
constexpr int kStrengthPercent = 80;

std::int16_t le16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::int16_t>(
      bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void put_le16(std::span<std::uint8_t> bytes, std::size_t offset, int value) {
  const auto clamped = static_cast<std::uint16_t>(std::clamp(
      value, static_cast<int>(INT16_MIN), static_cast<int>(INT16_MAX)));
  bytes[offset] = static_cast<std::uint8_t>(clamped & 0xff);
  bytes[offset + 1] = static_cast<std::uint8_t>(clamped >> 8);
}

void put_le32(std::span<std::uint8_t> bytes, std::size_t offset,
              std::uint32_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

std::uint8_t stick_axis(int value) {
  return static_cast<std::uint8_t>(std::clamp((value + 32768) >> 8, 0, 255));
}

std::uint8_t trigger_axis(int value) {
  return static_cast<std::uint8_t>(std::clamp(value >> 7, 0, 255));
}

std::uint8_t hat(std::uint32_t buttons) {
  /* Indexed by up, right, down, left as bits 0 to 3. */
  constexpr std::uint8_t kNone = kDsHatCentered;
  constexpr std::array<std::uint8_t, 16> kHats = {
      kNone, 0, 2, 1, 4, kNone, 3, kNone, 6, 7, kNone, kNone, 5, kNone,
      kNone, kNone};
  const unsigned index = ((buttons & kButtonDpadUp) != 0 ? 1u : 0u) |
                         ((buttons & kButtonDpadRight) != 0 ? 2u : 0u) |
                         ((buttons & kButtonDpadDown) != 0 ? 4u : 0u) |
                         ((buttons & kButtonDpadLeft) != 0 ? 8u : 0u);
  return kHats[index];
}

std::uint8_t battery_status(const std::optional<BatteryState> &battery) {
  if (!battery) {
    return kDsBatteryFull;
  }
  if (battery->charging && battery->percent >= 100) {
    return kDsBatteryCharged | kDsBatteryFull;
  }
  const auto steps =
      static_cast<std::uint8_t>(std::min(battery->percent / 10, 10u));
  return battery->charging ? kDsBatteryCharging | steps : steps;
}

/* A stable address per controller: hosts tell DualSenses apart by it. */
std::array<std::uint8_t, kDsAddressSize> address_for(std::string_view serial) {
  std::uint64_t hash = 0xcbf29ce484222325ull;
  for (const unsigned char ch : serial) {
    hash = (hash ^ ch) * 0x100000001b3ull;
  }
  std::array<std::uint8_t, kDsAddressSize> address{};
  for (std::size_t i = 0; i < address.size(); ++i) {
    address[i] = static_cast<std::uint8_t>(hash >> (8 * i));
  }
  /* Locally administered and unicast, so it cannot be a real device's. */
  address.back() = static_cast<std::uint8_t>((address.back() | 0x02) & 0xfe);
  return address;
}

} // namespace

std::uint8_t mulaw_encode(std::int16_t sample) {
  constexpr int kBias = 0x84;
  constexpr int kClip = 32635;
  const int magnitude =
      std::min(std::abs(static_cast<int>(sample)), kClip) + kBias;
  int exponent = 7;
  for (int mask = 0x4000; exponent > 0 && (magnitude & mask) == 0;
       mask >>= 1) {
    --exponent;
  }
  const int mantissa = (magnitude >> (exponent + 3)) & 0x0f;
  const int sign = sample < 0 ? 0x80 : 0;
  return static_cast<std::uint8_t>(~(sign | (exponent << 4) | mantissa));
}

std::optional<UsbInputReport>
SteamInputTranslator::translate(std::span<const std::uint8_t> report) {
  if (report.size() >= 3 && report[0] == kBatteryReportId) {
    const unsigned percent = std::min<unsigned>(report[2], 100);
    if (report[1] == kChargeDischarging) {
      battery_ = BatteryState{.percent = percent, .charging = false};
    } else if (report[1] == kChargeCharging) {
      battery_ = BatteryState{.percent = percent, .charging = true};
    } else if (report[1] == kChargeDone) {
      battery_ = BatteryState{.percent = 100, .charging = true};
    }
    return std::nullopt;
  }
  if (report.size() < kStateReportSize || report[0] != kStateReportId) {
    return std::nullopt;
  }

  UsbInputReport usb{};
  usb[0] = VDS_USB_INPUT_REPORT_ID;
  /* Stick up is positive on the Steam Controller and zero on the DualSense. */
  for (std::size_t stick = 0; stick < 2; ++stick) {
    const std::size_t from = kStateSticksOffset + 4 * stick;
    const std::size_t to = kDsSticksOffset + 2 * stick;
    usb[to] = stick_axis(le16(report, from));
    usb[to + 1] = stick_axis(-le16(report, from + 2));
  }
  for (std::size_t trigger = 0; trigger < 2; ++trigger) {
    usb[kDsTriggersOffset + trigger] =
        trigger_axis(le16(report, kStateTriggersOffset + 2 * trigger));
  }
  usb[kDsSequenceOffset] = sequence_++;

  const std::uint32_t buttons = le32(report, kStateButtonsOffset);
  const auto pressed = [&](std::uint32_t button, std::uint8_t bit) {
    return (buttons & button) != 0 ? bit : std::uint8_t{0};
  };
  usb[kDsButtonsOffset] = static_cast<std::uint8_t>(
      hat(buttons) | pressed(kButtonX, 0x10) | pressed(kButtonA, 0x20) |
      pressed(kButtonB, 0x40) | pressed(kButtonY, 0x80));
  usb[kDsButtonsOffset + 1] = static_cast<std::uint8_t>(
      pressed(kButtonLeftBumper, 0x01) | pressed(kButtonRightBumper, 0x02) |
      (usb[kDsTriggersOffset] != 0 ? 0x04 : 0) |
      (usb[kDsTriggersOffset + 1] != 0 ? 0x08 : 0) |
      pressed(kButtonView, 0x10) | pressed(kButtonMenu, 0x20) |
      pressed(kButtonLeftStick, 0x40) | pressed(kButtonRightStick, 0x80));
  usb[kDsButtonsOffset + 2] = static_cast<std::uint8_t>(
      pressed(kButtonSteam, 0x01) |
      pressed(kButtonLeftPadClick | kButtonRightPadClick, 0x02));
  pad_clicked_ = {(buttons & kButtonLeftPadClick) != 0,
                  (buttons & kButtonRightPadClick) != 0};

  /* The DualSense axes are the Steam Controller's X, Z and inverted Y. */
  const int gyro_x = le16(report, kStateGyroOffset);
  const int gyro_y = le16(report, kStateGyroOffset + 2);
  const int gyro_z = le16(report, kStateGyroOffset + 4);
  put_le16(usb, kDsGyroOffset, gyro_x);
  put_le16(usb, kDsGyroOffset + 2, gyro_z);
  put_le16(usb, kDsGyroOffset + 4, -gyro_y);
  const int accel_x = le16(report, kStateAccelOffset);
  const int accel_y = le16(report, kStateAccelOffset + 2);
  const int accel_z = le16(report, kStateAccelOffset + 4);
  put_le16(usb, kDsAccelOffset, accel_x / kAccelDivisor);
  put_le16(usb, kDsAccelOffset + 2, accel_z / kAccelDivisor);
  put_le16(usb, kDsAccelOffset + 4, -accel_y / kAccelDivisor);
  put_le32(usb, kDsSensorTimeOffset,
           le32(report, kStateSensorTimeOffset) *
               kDsSensorTicksPerMicrosecond);

  constexpr std::array<std::uint32_t, 2> kTouchButtons = {
      kButtonLeftPadTouch, kButtonRightPadTouch};
  for (std::size_t pad = 0; pad < touches_.size(); ++pad) {
    Touch &touch = touches_[pad];
    const bool down = (buttons & kTouchButtons[pad]) != 0;
    if (down && !touch.down) {
      touch.id = next_touch_id_;
      next_touch_id_ = (next_touch_id_ + 1) & 0x7f;
    }
    touch.down = down;

    const std::size_t to = kDsTouchOffset + kDsTouchSize * pad;
    usb[to] = static_cast<std::uint8_t>(down ? touch.id
                                             : touch.id | kDsTouchInactive);
    if (!down) {
      continue;
    }
    const std::size_t from = kStatePadsOffset + kStatePadSize * pad;
    const unsigned right = static_cast<unsigned>(le16(report, from) + 32768);
    const unsigned down_y = static_cast<unsigned>(
        std::min(32768 - le16(report, from + 2), 65535));
    const unsigned half = kDsTouchWidth / 2;
    const unsigned x = static_cast<unsigned>(pad) * half + right * half / 65536;
    const unsigned y = down_y * kDsTouchHeight / 65536;
    usb[to + 1] = static_cast<std::uint8_t>(x & 0xff);
    usb[to + 2] = static_cast<std::uint8_t>((x >> 8) | ((y & 0x0f) << 4));
    usb[to + 3] = static_cast<std::uint8_t>(y >> 4);
  }

  usb[kDsBatteryOffset] = battery_status(battery_);
  return usb;
}

bool steam_controller_switched_off(std::span<const std::uint8_t> report) {
  return report.size() >= 2 && report[0] == kWirelessStatusReportId &&
         report[1] == kWirelessDisconnected;
}

std::vector<std::vector<std::uint8_t>>
steam_virtual_feature_reports(std::string_view serial) {
  std::vector<std::uint8_t> calibration(kDsCalibrationReportSize, 0);
  calibration[0] = kDsCalibrationReportId;
  /* Three gyro biases stay zero; then plus and minus for pitch, yaw, roll. */
  for (std::size_t axis = 0; axis < 3; ++axis) {
    put_le16(calibration, 7 + 4 * axis, kDsGyroCalibrationCounts);
    put_le16(calibration, 9 + 4 * axis, -kDsGyroCalibrationCounts);
    put_le16(calibration, 23 + 4 * axis, kDsAccelPerG);
    put_le16(calibration, 25 + 4 * axis, -kDsAccelPerG);
  }
  put_le16(calibration, 19, kDsGyroCalibrationSpeed);
  put_le16(calibration, 21, kDsGyroCalibrationSpeed);

  std::vector<std::uint8_t> pairing(kDsPairingReportSize, 0);
  pairing[0] = kDsPairingReportId;
  const auto address = address_for(serial);
  std::copy(address.begin(), address.end(),
            pairing.begin() + kDsPairingAddressOffset);
  /* Two DualSenses had 08 25 00 here; the host address after it is empty. */
  pairing[kDsPairingAddressOffset + kDsAddressSize] = 0x08;
  pairing[kDsPairingAddressOffset + kDsAddressSize + 1] = 0x25;

  /*
   * As a DualSense on firmware 0630 answers, without the Bluetooth checksum
   * at the end: build date and time, then types, hardware and firmware
   * versions. With this report all zeros and the pairing report empty
   * after the address, Ratchet & Clank never opened the haptics sound card.
   */
  std::vector<std::uint8_t> firmware = {
      kFirmwareInfoReportId,
      'J',  'u',  'l',  ' ',  ' ',  '4',  ' ',  '2',  '0',  '2',  '5',
      '1',  '0',  ':',  '1',  '0',  ':',  '3',  '2',
      0x02, 0x00, 0x04, 0x00, 0x17, 0x06, 0x00, 0x00, 0x2a, 0x00, 0x10, 0x01,
      0x00, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x30, 0x06, 0x00, 0x00, 0x3c, 0x00, 0x01, 0x00, 0x0a, 0x00, 0x02, 0x00,
      0x06, 0x00, 0x00, 0x00};
  firmware.resize(kDsFirmwareReportSize, 0);

  return {std::move(calibration), std::move(pairing), std::move(firmware)};
}

std::optional<SteamRumbleReport>
steam_rumble_report(std::span<const std::uint8_t> usb_output_report) {
  if (usb_output_report.size() <= kDsOutputFlags2Offset ||
      usb_output_report[0] != kDsOutputReportId) {
    return std::nullopt;
  }
  const bool sets_rumble =
      (usb_output_report[kDsOutputFlags0Offset] & kDsOutputFlags0Rumble) != 0 ||
      (usb_output_report[kDsOutputFlags2Offset] & kDsOutputFlags2Rumble) != 0;
  if (!sets_rumble) {
    return std::nullopt;
  }
  SteamRumbleReport report{};
  report[0] = kRumbleReportId;
  /* The motors take 16 bits; repeating the byte maps 255 to 65535. */
  const std::uint8_t left = usb_output_report[kDsOutputLeftMotorOffset];
  const std::uint8_t right = usb_output_report[kDsOutputRightMotorOffset];
  report[kRumbleLeftOffset] = report[kRumbleLeftOffset + 1] = left;
  report[kRumbleRightOffset] = report[kRumbleRightOffset + 1] = right;
  return report;
}

bool steam_rumble_active(const SteamRumbleReport &report) {
  return (report[kRumbleLeftOffset] | report[kRumbleLeftOffset + 1] |
          report[kRumbleRightOffset] | report[kRumbleRightOffset + 1]) != 0;
}

SteamSettingsReport steam_settings_report() {
  SteamSettingsReport report{};
  const std::array<std::uint8_t, 9> settings = {
      kFeatureReportId,   kSetSettingsValues, 6,
      kSettingLizardMode, 0,                  0,
      kSettingImuMode,    kImuRawAccelAndGyro, 0};
  std::copy(settings.begin(), settings.end(), report.begin());
  return report;
}

SteamStreamConfig steam_stream_config_report(bool enable) {
  return {kStreamConfigReportId, enable ? kStreamStart : kStreamStop,
          kStreamTargetGrips,
          enable ? kStreamMode4kHzMulaw : std::uint8_t{0}};
}

SteamClickReport steam_pad_click_report(std::size_t pad, bool pressed) {
  /* Side 0 is the left trackpad, 1 the right; the last byte is 0 dB. */
  return {kClickReportId, static_cast<std::uint8_t>(pad),
          pressed ? kClickStrong : kClick, 0};
}

bool SteamHapticsEncoder::push_usb_audio(
    std::span<const std::uint8_t> pcm_bytes) {
  bool signal = false;
  for (const std::uint8_t byte : pcm_bytes) {
    pending_frame_[pending_frame_pos_++] = byte;
    if (pending_frame_pos_ < pending_frame_.size()) {
      continue;
    }
    pending_frame_pos_ = 0;
    for (std::size_t side = 0; side < sum_.size(); ++side) {
      const std::int16_t sample = le16(
          pending_frame_, (kHapticsFirstChannel + side) * sizeof(std::int16_t));
      signal |= sample != 0;
      sum_[side] += sample;
    }
    if (++summed_ < kDecimation) {
      continue;
    }
    /* Averaging is the low-pass: haptics sit far below the 2 kHz limit. */
    queue_.push_back(Frame{
        .left = mulaw_encode(static_cast<std::int16_t>(
            sum_[0] * kStrengthPercent /
            (100 * static_cast<int>(kDecimation)))),
        .right = mulaw_encode(static_cast<std::int16_t>(
            sum_[1] * kStrengthPercent /
            (100 * static_cast<int>(kDecimation)))),
    });
    sum_ = {};
    summed_ = 0;
    if (queue_.size() > kMaxQueuedFrames) {
      queue_.pop_front();
    }
  }
  return signal;
}

SteamStreamPacket SteamHapticsEncoder::pop_packet() {
  SteamStreamPacket packet;
  packet.fill(kMulawSilence);
  packet[0] = kStreamDataReportId;
  packet[1] = static_cast<std::uint8_t>(kSteamStreamFrames);
  const std::size_t frames = std::min(queue_.size(), kSteamStreamFrames);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    packet[kStreamLeftOffset + frame] = queue_.front().left;
    packet[kStreamRightOffset + frame] = queue_.front().right;
    queue_.pop_front();
  }
  return packet;
}

void SteamHapticsEncoder::clear() {
  queue_.clear();
  sum_ = {};
  summed_ = 0;
  pending_frame_pos_ = 0;
}

} // namespace vds
