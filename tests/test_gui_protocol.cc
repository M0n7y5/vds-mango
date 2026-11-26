// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include "vds_check.hh"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include "control_protocol.hh"
#include "vds_log.hh"
#include "vdsd_common.hh"

namespace vds::test {

namespace {

// Runs GUI requests through the daemon's real control handler, so a request
// the strict daemon parser rejects or a reply the GUI cannot read fails here.
class Daemon {
public:
  Daemon()
      : db_path_(directory_.filePath("vdsd.db").toStdString()),
        logger_(directory_.filePath("vdsd.log").toStdString()) {}

  /// What vdsd would report for a controller that is connected right now.
  void connect(vds::VdsdControlControllerStatus status) {
    statuses_.push_back(std::move(status));
  }

  /// The verdict of vdsd's driver test at the first connect.
  void set_kernel_fix(bool present) { kernel_fix_ = present; }

  /// The controller the last buzz command asked vdsd to vibrate.
  const std::string &buzzed() const { return buzzed_; }

  /// What the last request for targets asked of vdsd.
  const vds::TargetQuery &target_query() const { return target_query_; }

  /// The controller database as vdsd wrote it.
  std::string database() const {
    QFile file(QString::fromStdString(db_path_));
    (void)file.open(QIODevice::ReadOnly);
    return file.readAll().toStdString();
  }

  QByteArray handle(const QByteArray &request) {
    static const std::vector<vds::VdsdControlPortStatus> kPorts = {
        {.port = 0,
         .path = "vds0",
         .occupied = false,
         .address = "",
         .device_address = ""},
        {.port = 2,
         .path = "vds2",
         .occupied = false,
         .address = "",
         .device_address = ""},
    };
    bool reload_requested = false;
    const std::string reply = vds::handle_vdsd_control_command(
        vds::trim_command(request.toStdString()), db_path_, statuses_, kPorts,
        [this](const vds::TargetQuery &query) {
          target_query_ = query;
          return std::vector<vds::ControllerTarget>{{
              .address = "aa:bb:cc:dd:ee:01",
              .name = "DualSense Wireless Controller",
              .profile = vds::ControllerProfile::Ds5,
              .online = false,
              .trusted = true,
          }};
        },
        vds::VdsdControlDaemonStatus{.version = "1.2.3",
                                     .kernel_fix = kernel_fix_},
        trace_flags_, reload_requested, buzzed_, logger_);
    return QByteArray::fromStdString(reply);
  }

private:
  QTemporaryDir directory_;
  std::string db_path_;
  vds::Logger logger_;
  std::uint32_t trace_flags_ = 0;
  std::vector<vds::VdsdControlControllerStatus> statuses_;
  std::optional<bool> kernel_fix_;
  std::string buzzed_;
  vds::TargetQuery target_query_;
};

std::string error_of(Daemon &daemon, const QByteArray &request,
                     void (*parse)(const QByteArray &)) {
  try {
    parse(daemon.handle(request));
  } catch (const std::exception &error) {
    return error.what();
  }
  return "";
}

} // namespace

void test_daemon_accepts_the_attach_request_and_gui_reads_the_reply() {
  Daemon daemon;
  const vds::gui::AttachResult result =
      vds::gui::parse_attach_reply(daemon.handle(vds::gui::attach_request({
          .address = "aa:bb:cc:dd:ee:01",
          .profile = vds::ControllerProfile::Dse,
          .ports = {0, 2},
      })));
  VDS_CHECK_EQ(result.online, false);
  VDS_CHECK_EQ(result.trusted, true);
}

void test_listed_controller_keeps_the_attached_profile_and_ports() {
  Daemon daemon;
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Dse,
      .ports = {0, 2},
  }));
  const std::vector<vds::gui::ControllerStatus> controllers =
      vds::gui::parse_list_reply(daemon.handle(vds::gui::list_request()));
  VDS_CHECK_EQ(controllers.size(), static_cast<std::size_t>(1));
  VDS_CHECK_EQ(controllers.at(0).address.toStdString(),
               std::string("aa:bb:cc:dd:ee:01"));
  VDS_CHECK(controllers.at(0).profile == vds::ControllerProfile::Dse);
  VDS_CHECK(controllers.at(0).ports == (std::vector<unsigned>{0, 2}));
  VDS_CHECK_EQ(controllers.at(0).connected, false);
}

void test_listed_target_reports_whether_it_is_registered() {
  Daemon daemon;
  const std::vector<vds::gui::PairedController> before =
      vds::gui::parse_list_targets_reply(
          daemon.handle(vds::gui::list_targets_request()));
  VDS_CHECK_EQ(before.at(0).name.toStdString(),
               std::string("DualSense Wireless Controller"));
  VDS_CHECK_EQ(before.at(0).registered, false);

  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
  }));
  const std::vector<vds::gui::PairedController> after =
      vds::gui::parse_list_targets_reply(
          daemon.handle(vds::gui::list_targets_request()));
  VDS_CHECK_EQ(after.at(0).registered, true);
}

void test_plain_target_request_asks_bluetoothd() {
  Daemon daemon;
  (void)daemon.handle(vds::gui::local_targets_request(false));
  (void)daemon.handle(vds::gui::list_targets_request());
  VDS_CHECK(daemon.target_query().bluetooth);
}

void test_local_target_request_leaves_bluetoothd_alone() {
  Daemon daemon;
  (void)vds::gui::parse_list_targets_reply(
      daemon.handle(vds::gui::local_targets_request(false)));
  VDS_CHECK(!daemon.target_query().bluetooth);
}

void test_local_target_request_leaves_the_pucks_to_other_programs() {
  Daemon daemon;
  (void)daemon.handle(vds::gui::local_targets_request(true));
  (void)daemon.handle(vds::gui::local_targets_request(false));
  VDS_CHECK(!daemon.target_query().claim_pucks);
}

void test_claiming_target_request_asks_for_the_pucks() {
  Daemon daemon;
  (void)vds::gui::parse_list_targets_reply(
      daemon.handle(vds::gui::local_targets_request(true)));
  VDS_CHECK(daemon.target_query().claim_pucks);
}

void test_unpaired_address_surfaces_the_daemon_error_text() {
  Daemon daemon;
  VDS_CHECK_EQ(error_of(daemon,
                        vds::gui::attach_request({
                            .address = "11:22:33:44:55:66",
                            .profile = vds::ControllerProfile::Unspecified,
                            .ports = {},
                        }),
                        [](const QByteArray &reply) {
                          (void)vds::gui::parse_attach_reply(reply);
                        }),
               std::string("address is not paired, not supported, or not "
                           "attachable; run vdsctl list-targets"));
}

void test_detaching_an_unregistered_address_surfaces_the_daemon_error_text() {
  Daemon daemon;
  VDS_CHECK_EQ(error_of(daemon,
                        vds::gui::detach_request("aa:bb:cc:dd:ee:01"),
                        vds::gui::parse_detach_reply),
               std::string("address is not registered"));
}

void test_enabled_trace_scope_is_read_back_from_the_status_reply() {
  Daemon daemon;
  const vds::gui::TraceScopes enabled = vds::gui::parse_trace_reply(
      daemon.handle(vds::gui::trace_request("input-control", true)));
  VDS_CHECK_EQ(enabled.input_control, true);
  VDS_CHECK_EQ(enabled.input_audio, false);
  VDS_CHECK_EQ(enabled.output, false);

  const vds::gui::TraceScopes status = vds::gui::parse_trace_reply(
      daemon.handle(vds::gui::trace_status_request()));
  VDS_CHECK_EQ(status.input_control, true);
}

void test_new_controller_starts_with_the_standard_audio_buffer() {
  Daemon daemon;
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
  }));
  VDS_CHECK_EQ(vds::gui::parse_list_reply(daemon.handle(vds::gui::list_request()))
                   .at(0)
                   .audio_buffer,
               64u);
}

void test_chosen_audio_buffer_is_listed_for_that_controller() {
  Daemon daemon;
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
      .audio_buffer = 24,
  }));
  VDS_CHECK_EQ(vds::gui::parse_list_reply(daemon.handle(vds::gui::list_request()))
                   .at(0)
                   .audio_buffer,
               24u);
}

void test_attach_without_an_audio_buffer_keeps_the_saved_one() {
  Daemon daemon;
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
      .audio_buffer = 24,
  }));
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {1},
  }));
  VDS_CHECK_EQ(vds::gui::parse_list_reply(daemon.handle(vds::gui::list_request()))
                   .at(0)
                   .audio_buffer,
               24u);
}

void test_unmeasured_audio_buffer_surfaces_the_daemon_error_text() {
  Daemon daemon;
  VDS_CHECK_EQ(error_of(daemon,
                        vds::gui::attach_request({
                            .address = "aa:bb:cc:dd:ee:01",
                            .profile = vds::ControllerProfile::Unspecified,
                            .ports = {},
                            .audio_buffer = 12,
                        }),
                        [](const QByteArray &reply) {
                          (void)vds::gui::parse_attach_reply(reply);
                        }),
               std::string("audio buffer must be 16, 24, 32 or 64"));
}

void test_chosen_steam_tuning_is_listed_for_that_controller() {
  Daemon daemon;
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
      .steam = vds::SteamTuning{.haptics_strength = 55,
                                .rumble_strength = 40,
                                .pad_click = vds::PadClick::Soft},
  }));
  const vds::SteamTuning listed =
      vds::gui::parse_list_reply(daemon.handle(vds::gui::list_request()))
          .at(0)
          .steam;
  VDS_CHECK(listed.haptics_strength == 55 && listed.rumble_strength == 40 &&
            listed.pad_click == vds::PadClick::Soft);
}

void test_attach_without_steam_tuning_keeps_the_saved_one() {
  Daemon daemon;
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
      .steam = vds::SteamTuning{.haptics_strength = 55},
  }));
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {1},
  }));
  const vds::SteamTuning listed =
      vds::gui::parse_list_reply(daemon.handle(vds::gui::list_request()))
          .at(0)
          .steam;
  VDS_CHECK_EQ(static_cast<unsigned>(listed.haptics_strength), 55u);
}

void test_untuned_controller_adds_no_key_to_the_database() {
  // A daemon that predates the tuning refuses a record with keys it does not
  // know, so a downgrade must still find an untouched database readable.
  Daemon daemon;
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
      .steam = vds::SteamTuning{},
  }));
  VDS_CHECK_EQ(daemon.database(),
               std::string("{\"address\":\"aa:bb:cc:dd:ee:01\","
                           "\"profile\":\"\",\"ports\":[]}\n"));
}

void test_strength_above_one_hundred_surfaces_the_daemon_error_text() {
  Daemon daemon;
  VDS_CHECK_EQ(
      error_of(daemon,
               vds::gui::attach_request({
                   .address = "aa:bb:cc:dd:ee:01",
                   .profile = vds::ControllerProfile::Unspecified,
                   .ports = {},
                   .steam = vds::SteamTuning{.haptics_strength = 120},
               }),
               [](const QByteArray &reply) {
                 (void)vds::gui::parse_attach_reply(reply);
               }),
      std::string("strength must be a whole number from 0 to 100"));
}

void test_battery_of_a_connected_controller_reaches_the_gui() {
  Daemon daemon;
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
  }));
  daemon.connect({
      .address = "aa:bb:cc:dd:ee:01",
      .connected = true,
      .path = "vds0",
      .error = "",
      .battery_percent = 85,
      .charging = true,
  });
  const auto controller =
      vds::gui::parse_list_reply(daemon.handle(vds::gui::list_request())).at(0);
  VDS_CHECK(controller.battery.has_value());
  VDS_CHECK_EQ(*controller.battery, 85u);
  VDS_CHECK_EQ(controller.charging, true);
}

void test_controller_that_is_off_lists_no_battery_level() {
  Daemon daemon;
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
  }));
  VDS_CHECK(!vds::gui::parse_list_reply(daemon.handle(vds::gui::list_request()))
                 .at(0)
                 .battery.has_value());
}

vds::gui::ControllerStatus listed_with_firmware(std::uint16_t version,
                                                std::uint32_t build) {
  Daemon daemon;
  (void)daemon.handle(vds::gui::attach_request({
      .address = "aa:bb:cc:dd:ee:01",
      .profile = vds::ControllerProfile::Unspecified,
      .ports = {},
  }));
  daemon.connect({
      .address = "aa:bb:cc:dd:ee:01",
      .connected = true,
      .path = "vds0",
      .error = "",
      .firmware = version,
      .firmware_build = build,
  });
  return vds::gui::parse_list_reply(daemon.handle(vds::gui::list_request()))
      .at(0);
}

void test_firmware_keeps_the_leading_zero_sony_prints() {
  VDS_CHECK_EQ(listed_with_firmware(0x0630, 0x0110002a).firmware.toStdString(),
               std::string("0630"));
}

void test_firmware_build_is_listed_as_three_decimal_parts() {
  VDS_CHECK_EQ(
      listed_with_firmware(0x0630, 0x0110002a).firmware_build.toStdString(),
      std::string("1.16.42"));
}

void test_status_reports_the_daemon_version() {
  Daemon daemon;
  VDS_CHECK_EQ(
      vds::gui::parse_status_reply(daemon.handle(vds::gui::status_request()))
          .version.toStdString(),
      std::string("1.2.3"));
}

void test_status_before_any_connect_leaves_the_kernel_fix_unknown() {
  Daemon daemon;
  VDS_CHECK(
      !vds::gui::parse_status_reply(daemon.handle(vds::gui::status_request()))
           .kernel_fix.has_value());
}

void test_status_reports_a_kernel_without_the_fix() {
  Daemon daemon;
  daemon.set_kernel_fix(false);
  const auto kernel_fix =
      vds::gui::parse_status_reply(daemon.handle(vds::gui::status_request()))
          .kernel_fix;
  VDS_CHECK(kernel_fix.has_value());
  VDS_CHECK_EQ(*kernel_fix, false);
}

void test_buzz_request_names_the_connected_controller_to_vdsd() {
  Daemon daemon;
  daemon.connect({
      .address = "aa:bb:cc:dd:ee:01",
      .connected = true,
      .path = "vds0",
      .error = "",
  });
  vds::gui::parse_buzz_reply(
      daemon.handle(vds::gui::buzz_request("AA:BB:CC:DD:EE:01")));
  VDS_CHECK_EQ(daemon.buzzed(), std::string("aa:bb:cc:dd:ee:01"));
}

void test_buzz_for_a_controller_that_is_off_surfaces_the_daemon_error_text() {
  Daemon daemon;
  VDS_CHECK_EQ(
      error_of(daemon, vds::gui::buzz_request("aa:bb:cc:dd:ee:01"),
               vds::gui::parse_buzz_reply),
      std::string("the controller is not connected; press the PS button "
                  "first"));
}

void test_buzz_is_refused_while_a_game_plays_through_the_controller() {
  Daemon daemon;
  daemon.connect({
      .address = "aa:bb:cc:dd:ee:01",
      .connected = true,
      .path = "vds0",
      .error = "",
      .audio_busy = true,
  });
  (void)daemon.handle(vds::gui::buzz_request("aa:bb:cc:dd:ee:01"));
  VDS_CHECK(daemon.buzzed().empty());
}

void run_tests() {
  test_daemon_accepts_the_attach_request_and_gui_reads_the_reply();
  test_listed_controller_keeps_the_attached_profile_and_ports();
  test_listed_target_reports_whether_it_is_registered();
  test_plain_target_request_asks_bluetoothd();
  test_local_target_request_leaves_bluetoothd_alone();
  test_local_target_request_leaves_the_pucks_to_other_programs();
  test_claiming_target_request_asks_for_the_pucks();
  test_unpaired_address_surfaces_the_daemon_error_text();
  test_detaching_an_unregistered_address_surfaces_the_daemon_error_text();
  test_enabled_trace_scope_is_read_back_from_the_status_reply();
  test_new_controller_starts_with_the_standard_audio_buffer();
  test_chosen_audio_buffer_is_listed_for_that_controller();
  test_attach_without_an_audio_buffer_keeps_the_saved_one();
  test_unmeasured_audio_buffer_surfaces_the_daemon_error_text();
  test_chosen_steam_tuning_is_listed_for_that_controller();
  test_attach_without_steam_tuning_keeps_the_saved_one();
  test_untuned_controller_adds_no_key_to_the_database();
  test_strength_above_one_hundred_surfaces_the_daemon_error_text();
  test_battery_of_a_connected_controller_reaches_the_gui();
  test_controller_that_is_off_lists_no_battery_level();
  test_firmware_keeps_the_leading_zero_sony_prints();
  test_firmware_build_is_listed_as_three_decimal_parts();
  test_status_reports_the_daemon_version();
  test_status_before_any_connect_leaves_the_kernel_fix_unknown();
  test_status_reports_a_kernel_without_the_fix();
  test_buzz_request_names_the_connected_controller_to_vdsd();
  test_buzz_for_a_controller_that_is_off_surfaces_the_daemon_error_text();
  test_buzz_is_refused_while_a_game_plays_through_the_controller();
}

} // namespace vds::test
