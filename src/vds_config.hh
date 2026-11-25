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

#include "uapi/vds.h"

namespace vds {

inline constexpr unsigned kMinPortCount = VDS_MIN_PORT_COUNT;
inline constexpr unsigned kMaxPortCount = VDS_MAX_PORT_COUNT;
inline constexpr std::uint8_t kDefaultAudioBufferLength = 64;
/// The connect error of a controller that BlueZ keeps connected on a kernel
/// whose hid-playstation parses Bluetooth audio reports as input. The GUI
/// compares against it to offer taking Bluetooth input over instead.
inline constexpr std::string_view kBluetoothSharingUnsupported =
    "this kernel cannot share the controller with Bluetooth input; let vDS "
    "Mango take over Bluetooth input instead (vDS Mango setup, or "
    "override-bluetoothd.sh disable-input --restart)";

enum class ControllerProfile {
  Unspecified,
  Ds5,
  Dse,
};

struct ControllerConfig {
  std::string address;
  ControllerProfile profile = ControllerProfile::Unspecified;
  std::vector<unsigned> ports;
  /// How much audio the controller queues before it plays haptics and
  /// speaker sound.
  std::uint8_t audio_buffer_length = kDefaultAudioBufferLength;
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
ControllerProfile parse_controller_profile(std::string_view profile);
std::string controller_profile_name(ControllerProfile profile);
/// Accepts only the lengths confirmed to play on a DualSense: 16, 24, 32, 64.
/// Other values garbled or muted the output (12, 0).
std::uint8_t parse_audio_buffer_length(std::string_view text);
std::vector<unsigned> parse_ports(std::string_view text);
std::string format_ports(std::span<const unsigned> ports);
/// Virtual ports have no device node; this name identifies one to users.
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
