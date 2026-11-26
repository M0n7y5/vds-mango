// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "jsonl.hh"
#include "vdsctl_common.hh"

namespace vds {

namespace {

constexpr std::string_view kControlReplyContext = "control reply";
constexpr std::string_view kListContext = "list reply";
constexpr std::string_view kTargetsContext = "list-targets reply";
constexpr std::string_view kNoControllersHint =
    "(no controllers registered. Run: vdsctl list-targets)";
constexpr std::string_view kNoTargetsHint =
    "(no attachable controllers. Pair one with bluetoothctl)";
constexpr std::string_view kOfflineHint =
    "note: controller is not connected. Press the PS button to connect\n";

std::string attach_request(const ControllerConfig &config) {
  std::string request = "{";
  request += jsonl_string_field("command", "attach");
  request += ',';
  request += jsonl_string_field("address", config.address);
  request += ',';
  request +=
      jsonl_string_field("profile", controller_profile_name(config.profile));
  request += ',';
  request += jsonl_unsigned_array_field("ports", config.ports);
  request += "}\n";
  return request;
}

std::string detach_request(std::string_view address) {
  std::string request = "{";
  request += jsonl_string_field("command", "detach");
  request += ',';
  request += jsonl_string_field("address", address);
  request += "}\n";
  return request;
}

std::string list_request() { return "{\"command\":\"list\"}\n"; }

std::string list_targets_request() {
  return "{\"command\":\"list-targets\"}\n";
}

std::string trace_request(const VdsctlTraceCommand &trace) {
  std::string request = "{";
  request += jsonl_string_field("command", "trace");
  request += ',';
  request += jsonl_string_field("mode", trace.mode);
  request += ',';
  request += jsonl_string_field("scope", trace.scope);
  request += "}\n";
  return request;
}

bool is_help_flag(std::string_view arg) {
  return arg == "-h" || arg == "--help";
}

std::string_view control_reply_line(std::string_view reply) {
  if (!reply.empty() && reply.back() == '\n') {
    reply.remove_suffix(1);
  }
  return reply;
}

std::optional<std::vector<JsonlField>>
accept_control_reply(const std::string &reply) {
  std::vector<JsonlField> fields =
      parse_jsonl_object(control_reply_line(reply), kControlReplyContext);
  if (!require_jsonl_bool(fields, "OK", kControlReplyContext)) {
    std::cerr << "vdsctl: "
              << require_jsonl_string(fields, "error", kControlReplyContext)
              << "\n";
    return std::nullopt;
  }
  std::cout << reply;
  return fields;
}

std::string untrusted_hint(std::string_view address) {
  std::string hint = "note: controller is not trusted in BlueZ. Run 'trust ";
  hint += address;
  hint += "' in bluetoothctl or it will not reconnect\n";
  return hint;
}

int run_attach_command(int argc, char **argv, const VdsctlPlatform &platform) {
  const std::optional<std::vector<JsonlField>> fields = accept_control_reply(
      run_vdsctl_attach(argc, argv, platform.request_control));
  if (!fields) {
    return 1;
  }

  if (!require_jsonl_bool(*fields, "online", kControlReplyContext)) {
    std::cerr << kOfflineHint;
  }
  if (!require_jsonl_bool(*fields, "trusted", kControlReplyContext)) {
    std::cerr << untrusted_hint(
        require_jsonl_string(*fields, "address", kControlReplyContext));
  }
  return 0;
}

std::string yes_no(bool value) { return value ? "yes" : "no"; }

std::string ports_cell(std::span<const unsigned> ports) {
  if (ports.empty()) {
    return "all";
  }
  std::string text;
  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (i > 0) {
      text += ',';
    }
    text += std::to_string(ports[i]);
  }
  return text;
}

std::string profile_cell(std::string profile) {
  return profile.empty() ? "auto" : profile;
}

template <class Row>
std::vector<std::vector<std::string>>
parse_table_rows(std::string_view jsonl, std::string_view context, Row to_row) {
  std::vector<std::vector<std::string>> rows;
  std::size_t offset = 0;
  while (offset < jsonl.size()) {
    const std::size_t end = jsonl.find('\n', offset);
    const std::size_t length =
        end == std::string_view::npos ? jsonl.size() - offset : end - offset;
    const std::string_view line = jsonl.substr(offset, length);
    offset += length + 1;
    if (line.empty()) {
      continue;
    }
    rows.push_back(to_row(parse_jsonl_object(line, context)));
  }
  return rows;
}

std::string format_table(std::span<const std::string_view> headers,
                         const std::vector<std::vector<std::string>> &rows,
                         std::string_view placeholder) {
  if (rows.empty()) {
    std::string text(placeholder);
    text += '\n';
    return text;
  }

  std::vector<std::size_t> widths;
  widths.reserve(headers.size());
  for (const std::string_view header : headers) {
    widths.push_back(header.size());
  }
  for (const auto &row : rows) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }

  std::string text;
  const auto append_line = [&](const auto &cells) {
    std::string line;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (i > 0) {
        line += "  ";
      }
      line += cells[i];
      line.append(widths[i] - cells[i].size(), ' ');
    }
    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
    text += line;
    text += '\n';
  };

  append_line(headers);
  for (const auto &row : rows) {
    append_line(row);
  }
  return text;
}

std::optional<std::string> refusal_of(std::string_view reply) {
  const std::string_view line = reply.substr(0, reply.find('\n'));
  if (line.empty()) {
    return std::nullopt;
  }
  const std::vector<JsonlField> fields =
      parse_jsonl_object(line, kControlReplyContext);
  for (const JsonlField &field : fields) {
    if (field.key == "OK" && field.value.is_bool && !field.value.bool_value) {
      return require_jsonl_string(fields, "error", kControlReplyContext);
    }
  }
  return std::nullopt;
}

int print_listing(const std::string &reply, bool raw,
                  std::string (*format_table)(std::string_view)) {
  if (const std::optional<std::string> error = refusal_of(reply)) {
    std::cerr << "vdsctl: " << *error << "\n";
    return 1;
  }
  std::cout << (raw ? reply : format_table(reply));
  return 0;
}

int run_list_command(int argc, char **argv, const VdsctlPlatform &platform) {
  const bool json = parse_vdsctl_json_flag(argc, argv);
  return print_listing(run_vdsctl_list(platform.request_control),
                       json || !platform.is_tty(), format_vdsctl_list_table);
}

int run_list_targets_command(int argc, char **argv,
                             const VdsctlPlatform &platform) {
  const bool json = parse_vdsctl_json_flag(argc, argv);
  return print_listing(run_vdsctl_list_targets(platform.request_control),
                       json || !platform.is_tty(),
                       format_vdsctl_targets_table);
}

} // namespace

std::string vdsctl_usage(std::string_view version,
                         std::string_view build_year) {
  std::string text = "vdsctl (";
  text += version;
  text += "): vDS Mango management utility - Copyright (C) ";
  text += build_year;
  text += " Jihong Min\n"
          "usage:\n"
          "  vdsctl attach <address> [--ports 0[,1...]|\"\"] "
          "[--profile ds5|dse|\"\"]\n"
          "  vdsctl detach <address>\n"
          "  vdsctl list [--json]\n"
          "  vdsctl list-targets [--json]\n"
          "  vdsctl trace on|off [--scope <scope>[,<scope>...]]\n"
          "  vdsctl trace status\n"
          "\n"
          "address:\n"
          "  a Bluetooth address, or steam:<serial> for a Steam Controller "
          "on its\n"
          "  Puck. vdsctl list-targets shows both\n"
          "\n"
          "trace scopes:\n"
          "  all, input, input-audio, input-control, output\n"
          "\n"
          "exit status:\n"
          "  0 success, 1 error\n";
  return text;
}

std::optional<VdsctlCommand> parse_vdsctl_command(std::string_view command) {
  if (command == "attach") {
    return VdsctlCommand::Attach;
  }
  if (command == "detach") {
    return VdsctlCommand::Detach;
  }
  if (command == "list") {
    return VdsctlCommand::List;
  }
  if (command == "list-targets") {
    return VdsctlCommand::ListTargets;
  }
  if (command == "trace") {
    return VdsctlCommand::Trace;
  }
  return std::nullopt;
}

int run_vdsctl_app(int argc, char **argv, std::string_view version,
                   std::string_view build_year,
                   const VdsctlPlatform &platform) {
  try {
    if (argc < 2) {
      std::cerr << vdsctl_usage(version, build_year);
      return 1;
    }

    const std::string_view name = argv[1];
    if (is_help_flag(name)) {
      std::cout << vdsctl_usage(version, build_year);
      return 0;
    }
    if (name == "--version" || name == "version") {
      std::cout << "vdsctl " << version << "\n";
      return 0;
    }

    const std::optional<VdsctlCommand> command = parse_vdsctl_command(name);
    if (!command) {
      std::cerr << vdsctl_usage(version, build_year);
      return 1;
    }
    if (argc > 2 && is_help_flag(argv[2])) {
      std::cout << vdsctl_usage(version, build_year);
      return 0;
    }

    switch (*command) {
    case VdsctlCommand::Attach:
      return run_attach_command(argc, argv, platform);
    case VdsctlCommand::Detach:
      return accept_control_reply(
                 run_vdsctl_detach(argc, argv, platform.request_control))
                 ? 0
                 : 1;
    case VdsctlCommand::List:
      return run_list_command(argc, argv, platform);
    case VdsctlCommand::ListTargets:
      return run_list_targets_command(argc, argv, platform);
    case VdsctlCommand::Trace:
      return accept_control_reply(
                 run_vdsctl_trace(argc, argv, platform.request_control))
                 ? 0
                 : 1;
    }
    throw std::runtime_error("unhandled command: " + std::string(name));
  } catch (const std::exception &error) {
    std::cerr << "vdsctl: " << error.what() << "\n";
    return 1;
  }
}

void require_vdsctl_arg_count(int argc, int expected) {
  if (argc != expected) {
    throw std::runtime_error("unexpected argument");
  }
}

bool parse_vdsctl_json_flag(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[2]) == "--json") {
    return true;
  }
  require_vdsctl_arg_count(argc, 2);
  return false;
}

ControllerConfig parse_vdsctl_attach(int argc, char **argv) {
  if (argc < 3) {
    throw std::runtime_error("attach requires an address");
  }

  ControllerConfig config{
      .address = normalize_controller_address(argv[2]),
      .profile = ControllerProfile::Unspecified,
      .ports = {},
  };

  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next_arg_is_value = [&] {
      return i + 1 < argc && std::string_view(argv[i + 1]).rfind("--", 0) != 0;
    };
    if (arg == "--profile") {
      config.profile =
          parse_controller_profile(next_arg_is_value() ? argv[++i] : "");
    } else if (arg == "--ports") {
      config.ports = parse_ports(next_arg_is_value() ? argv[++i] : "");
    } else {
      throw std::runtime_error("unknown attach option: " + std::string(arg));
    }
  }

  return config;
}

VdsctlTraceCommand parse_vdsctl_trace(int argc, char **argv) {
  if (argc != 3 && argc != 5) {
    throw std::runtime_error(
        "trace requires on, off, or status with optional --scope");
  }

  const std::string_view mode = argv[2];
  if (mode != "on" && mode != "off" && mode != "status") {
    throw std::runtime_error("trace requires on, off, or status");
  }

  VdsctlTraceCommand command{
      .mode = std::string(mode),
      .scope = "all",
  };
  if (argc == 5) {
    if (mode == "status") {
      throw std::runtime_error("trace status takes no --scope");
    }
    if (std::string_view(argv[3]) != "--scope") {
      throw std::runtime_error("trace option must be --scope");
    }
    command.scope = argv[4];
  }

  return command;
}

std::string run_vdsctl_attach(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  const ControllerConfig config = parse_vdsctl_attach(argc, argv);
  return request_control(attach_request(config));
}

std::string run_vdsctl_detach(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  require_vdsctl_arg_count(argc, 3);
  return request_control(
      detach_request(normalize_controller_address(argv[2])));
}

std::string run_vdsctl_list(
    const std::function<std::string(const std::string &)> &request_control) {
  return request_control(list_request());
}

std::string run_vdsctl_list_targets(
    const std::function<std::string(const std::string &)> &request_control) {
  return request_control(list_targets_request());
}

std::string run_vdsctl_trace(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  return request_control(trace_request(parse_vdsctl_trace(argc, argv)));
}

std::string format_vdsctl_list_table(std::string_view jsonl) {
  static constexpr std::array<std::string_view, 6> kHeaders = {
      "ADDRESS", "CONNECTED", "PATH", "PROFILE", "PORTS", "ERROR"};
  const auto rows = parse_table_rows(
      jsonl, kListContext, [](std::span<const JsonlField> fields) {
        return std::vector<std::string>{
            require_jsonl_string(fields, "address", kListContext),
            yes_no(require_jsonl_bool(fields, "connected", kListContext)),
            require_jsonl_string(fields, "path", kListContext),
            profile_cell(require_jsonl_string(fields, "profile", kListContext)),
            ports_cell(
                require_jsonl_unsigned_array(fields, "ports", kListContext)),
            require_jsonl_string(fields, "error", kListContext),
        };
      });
  return format_table(kHeaders, rows, kNoControllersHint);
}

std::string format_vdsctl_targets_table(std::string_view jsonl) {
  static constexpr std::array<std::string_view, 5> kHeaders = {
      "ADDRESS", "NAME", "ONLINE", "TRUSTED", "REGISTERED"};
  const auto rows = parse_table_rows(
      jsonl, kTargetsContext, [](std::span<const JsonlField> fields) {
        return std::vector<std::string>{
            require_jsonl_string(fields, "address", kTargetsContext),
            require_jsonl_string(fields, "name", kTargetsContext),
            yes_no(require_jsonl_bool(fields, "online", kTargetsContext)),
            yes_no(require_jsonl_bool(fields, "trusted", kTargetsContext)),
            yes_no(require_jsonl_bool(fields, "registered", kTargetsContext)),
        };
      });
  return format_table(kHeaders, rows, kNoTargetsHint);
}

} // namespace vds
