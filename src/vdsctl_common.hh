// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "vds_config.hh"

namespace vds {

enum class VdsctlCommand {
  Attach,
  Detach,
  List,
  ListTargets,
  Trace,
};

struct VdsctlTraceCommand {
  std::string mode;
  std::string scope;
};

struct VdsctlPlatform {
  std::function<std::string(const std::string &)> request_control;
  std::function<bool()> is_tty = [] { return false; };
};

std::string vdsctl_usage(std::string_view version, std::string_view build_year);
int run_vdsctl_app(int argc, char **argv, std::string_view version,
                   std::string_view build_year, const VdsctlPlatform &platform);
std::optional<VdsctlCommand> parse_vdsctl_command(std::string_view command);
void require_vdsctl_arg_count(int argc, int expected);
bool parse_vdsctl_json_flag(int argc, char **argv);
ControllerConfig parse_vdsctl_attach(int argc, char **argv);
VdsctlTraceCommand parse_vdsctl_trace(int argc, char **argv);
std::string run_vdsctl_attach(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control);
std::string run_vdsctl_detach(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control);
std::string run_vdsctl_list(
    const std::function<std::string(const std::string &)> &request_control);
std::string run_vdsctl_list_targets(
    const std::function<std::string(const std::string &)> &request_control);
std::string run_vdsctl_trace(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control);
std::string format_vdsctl_list_table(std::string_view jsonl);
std::string format_vdsctl_targets_table(std::string_view jsonl);

} // namespace vds
