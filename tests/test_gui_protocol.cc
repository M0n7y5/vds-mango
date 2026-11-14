// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include "vds_check.hh"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include <QByteArray>
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

  QByteArray handle(const QByteArray &request) {
    static const std::vector<vds::VdsdControlPortStatus> kPorts = {
        {.port = 0,
         .path = "/dev/vds0",
         .occupied = false,
         .address = "",
         .device_address = ""},
        {.port = 2,
         .path = "/dev/vds2",
         .occupied = false,
         .address = "",
         .device_address = ""},
    };
    bool reload_requested = false;
    const std::string reply = vds::handle_vdsd_control_command(
        vds::trim_command(request.toStdString()), db_path_, {}, kPorts,
        [] {
          return std::vector<vds::ControllerTarget>{{
              .address = "aa:bb:cc:dd:ee:01",
              .name = "DualSense Wireless Controller",
              .profile = vds::ControllerProfile::Ds5,
              .online = false,
              .trusted = true,
          }};
        },
        trace_flags_, audio_buffer_length_, reload_requested, logger_);
    return QByteArray::fromStdString(reply);
  }

private:
  QTemporaryDir directory_;
  std::string db_path_;
  vds::Logger logger_;
  std::uint32_t trace_flags_ = 0;
  std::uint8_t audio_buffer_length_ = vds::kDefaultAudioBufferLength;
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

void test_chosen_audio_buffer_is_read_back_from_the_status_reply() {
  Daemon daemon;
  VDS_CHECK_EQ(vds::gui::parse_audio_buffer_reply(
                   daemon.handle(vds::gui::audio_buffer_request(32))),
               32u);
  VDS_CHECK_EQ(vds::gui::parse_audio_buffer_reply(
                   daemon.handle(vds::gui::audio_buffer_status_request())),
               32u);
}

void test_unmeasured_audio_buffer_surfaces_the_daemon_error_text() {
  Daemon daemon;
  VDS_CHECK_EQ(error_of(daemon, vds::gui::audio_buffer_request(12),
                        [](const QByteArray &reply) {
                          (void)vds::gui::parse_audio_buffer_reply(reply);
                        }),
               std::string("audio buffer must be 16, 24, 32 or 64"));
}

void run_tests() {
  test_daemon_accepts_the_attach_request_and_gui_reads_the_reply();
  test_listed_controller_keeps_the_attached_profile_and_ports();
  test_listed_target_reports_whether_it_is_registered();
  test_unpaired_address_surfaces_the_daemon_error_text();
  test_detaching_an_unregistered_address_surfaces_the_daemon_error_text();
  test_enabled_trace_scope_is_read_back_from_the_status_reply();
  test_chosen_audio_buffer_is_read_back_from_the_status_reply();
  test_unmeasured_audio_buffer_surfaces_the_daemon_error_text();
}

} // namespace vds::test
