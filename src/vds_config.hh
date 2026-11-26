// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "jsonl.hh"
#include "uapi/vds.h"

namespace vds {

inline constexpr unsigned kMinPortCount = VDS_MIN_PORT_COUNT;
inline constexpr unsigned kMaxPortCount = VDS_MAX_PORT_COUNT;
inline constexpr std::uint8_t kDefaultAudioBufferLength = 64;
/// The connect error of a controller on a kernel whose hid-playstation parses
/// Bluetooth audio reports as input. The GUI compares against it to show the
/// requirement where the controller would connect.
inline constexpr std::string_view kBluetoothSharingUnsupported =
    "this kernel reads the controller's Bluetooth audio as button presses. "
    "vDS Mango needs a kernel with the DualSense audio fix, such as "
    "linux-cachyos 7.3.6 or newer";
/// Registers a Steam Controller by its serial number: steam:FXA1234567B89.
inline constexpr std::string_view kSteamAddressPrefix = "steam:";

enum class ControllerProfile {
  Unspecified,
  Ds5,
  Dse,
};

/// The click a Steam Controller's trackpad plays when it is pressed down.
enum class PadClick {
  Off,
  Soft,
  Strong,
};

/// How a Steam Controller plays what a game sends to a DualSense.
struct SteamTuning {
  /// Percent of the game's haptics signal. For the same signal the grips
  /// shake harder than a DualSense's. At 80 a player found Ratchet & Clank
  /// to feel right.
  std::uint8_t haptics_strength = 80;
  /// Percent of the game's rumble motor values.
  std::uint8_t rumble_strength = 100;
  PadClick pad_click = PadClick::Strong;
};

struct ControllerConfig {
  std::string address;
  ControllerProfile profile = ControllerProfile::Unspecified;
  std::vector<unsigned> ports;
  /// How much audio the controller queues before it plays haptics and
  /// speaker sound.
  std::uint8_t audio_buffer_length = kDefaultAudioBufferLength;
  /// Used by a Steam Controller only.
  SteamTuning steam = {};
};

struct ControllerTarget {
  std::string address;
  std::string name;
  ControllerProfile profile = ControllerProfile::Unspecified;
  bool online = false;
  bool trusted = true;
};

struct ConfigDb {
  std::vector<ControllerConfig> controllers;
};

std::string normalize_bluetooth_address(std::string_view address);
/// A Bluetooth address, or kSteamAddressPrefix and a serial number.
std::string normalize_controller_address(std::string_view address);
bool is_steam_address(std::string_view address);
ControllerProfile parse_controller_profile(std::string_view profile);
std::string controller_profile_name(ControllerProfile profile);
/// Accepts only the lengths confirmed to play on a DualSense: 16, 24, 32, 64.
/// Other values garbled or muted the output (12, 0).
std::uint8_t parse_audio_buffer_length(std::string_view text);
PadClick parse_pad_click(std::string_view text);
std::string pad_click_name(PadClick click);
/// The tuning keys among fields replace the values of tuning. An absent key
/// keeps its value.
SteamTuning read_steam_tuning(std::span<const JsonlField> fields,
                              std::string_view context, SteamTuning tuning);
/// The tuning as JSONL fields, each led by a comma. only_changed leaves out
/// the values a controller starts with.
std::string steam_tuning_fields(const SteamTuning &tuning, bool only_changed);
std::vector<unsigned> parse_ports(std::string_view text);
std::string format_ports(std::span<const unsigned> ports);
/// Virtual ports have no device node. This name identifies one to users.
std::string port_name_for_index(unsigned port);
std::optional<unsigned> port_index_from_name(std::string_view name);
bool controller_config_allows_port(const ControllerConfig &config,
                                   unsigned port);
bool controller_config_allows_port_name(const ControllerConfig &config,
                                        std::string_view name);
std::vector<unsigned>
controller_config_port_targets(const ControllerConfig &config);
bool controller_config_has_candidate_port(
    const ControllerConfig &config, std::span<const unsigned> candidate_ports);
std::optional<unsigned>
select_controller_config_port(const ControllerConfig &config,
                              std::span<const unsigned> candidate_ports,
                              std::span<const unsigned> occupied_ports = {});
const ControllerConfig *
find_controller_config_by_address(const ConfigDb &db, std::string_view address);
ConfigDb load_config_db(const std::string &path);
ConfigDb parse_config_db(std::string_view text);
ConfigDb update_config_db(const std::string &path,
                          const std::function<void(ConfigDb &)> &update);
void upsert_controller_config(ConfigDb &db, ControllerConfig config);
bool remove_controller_config(ConfigDb &db, std::string_view address);
void validate_config_assignments(const ConfigDb &db,
                                 std::span<const std::string> devices);

} // namespace vds
