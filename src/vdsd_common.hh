// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vds_config.hh"

namespace vds {

class Logger;

inline constexpr std::uint32_t kTraceInputAudio = 1u << 0;
inline constexpr std::uint32_t kTraceInputControl = 1u << 1;
inline constexpr std::uint32_t kTraceInput =
    kTraceInputAudio | kTraceInputControl;
inline constexpr std::uint32_t kTraceOutput = 1u << 2;
inline constexpr std::uint32_t kTraceAll = kTraceInput | kTraceOutput;

struct VdsdCommonOptions {
  std::string db_path;
  std::string log_path;
  bool help_requested = false;
};

struct VdsdControlControllerStatus {
  std::string address;
  bool connected = false;
  std::string path;
  std::string error;
  /// Live state of a connected controller. No battery level means unknown.
  std::optional<unsigned> battery_percent = std::nullopt;
  bool charging = false;
  bool headset = false;
  bool mic_muted = false;
  /// As hid-playstation prints it, for bug reports.
  std::optional<std::uint32_t> firmware = std::nullopt;
  /// A game or app is playing through the controller's sound card.
  bool audio_busy = false;
};

/// What the daemon knows about itself, for the status command.
struct VdsdControlDaemonStatus {
  std::string_view version;
  /// Whether this kernel's hid-playstation ignores Bluetooth audio reports.
  /// Unset until the first controller has connected.
  std::optional<bool> kernel_fix = std::nullopt;
};

struct VdsdControlPortStatus {
  unsigned port = 0;
  std::string path;
  bool occupied = false;
  std::string address;
  std::string device_address;
};

struct VdsdControlPortCandidate {
  unsigned port = 0;
  std::string path;
};

struct VdsdControlPortBinding {
  unsigned port = 0;
  std::string address;
  std::string device_address;
};

VdsdCommonOptions default_vdsd_common_options();
std::string vdsd_usage(std::string_view version, std::string_view build_year,
                       std::string_view platform_options);
void print_vdsd_usage(std::ostream &out, std::string_view version,
                      std::string_view build_year,
                      std::string_view platform_options);
bool parse_vdsd_common_option(int argc, char **argv, int &index,
                              VdsdCommonOptions &options);
std::string trim_command(std::string command);
std::uint32_t parse_trace_scope(std::string_view scope);
std::string trace_scope_name(std::uint32_t scope);
std::string active_trace_name(std::uint32_t trace_flags);
bool trace_enabled(std::uint32_t trace_flags, std::uint32_t target);
std::vector<VdsdControlPortStatus> build_vdsd_control_port_statuses(
    std::span<const VdsdControlPortCandidate> candidates,
    std::span<const VdsdControlPortBinding> bindings);
std::string format_control_error_reply(std::string_view error);
/// buzz_address receives the controller a buzz command wants to vibrate.
std::string handle_vdsd_control_command(
    std::string_view request, const std::string &db_path,
    std::span<const VdsdControlControllerStatus> controllers,
    std::span<const VdsdControlPortStatus> ports,
    const std::function<std::vector<ControllerTarget>()> &list_targets,
    const VdsdControlDaemonStatus &daemon, std::uint32_t &trace_flags,
    bool &reload_requested, std::string &buzz_address, Logger &logger);

} // namespace vds
