// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include "vds_check.hh"

#include <string>
#include <vector>

#include <QString>

#include "tray_model.hh"

namespace vds::test {

namespace {

using vds::gui::ControllerStatus;
using vds::gui::ServiceState;
using vds::gui::TrayEntry;

constexpr const char *kPad = "aa:bb:cc:dd:ee:01";
constexpr const char *kSteam = "steam:FXA1234567B89";

QString name_of(const QString &address) {
  return address == kPad ? "DualSense" : "Steam Controller";
}

ControllerStatus connected(const char *address) {
  ControllerStatus controller;
  controller.address = address;
  controller.connected = true;
  return controller;
}

ControllerStatus off(const char *address) {
  ControllerStatus controller;
  controller.address = address;
  return controller;
}

/// One line per entry: kind, text and address.
std::string menu(ServiceState service,
                 const std::vector<ControllerStatus> &controllers) {
  std::string lines;
  for (const TrayEntry &entry :
       vds::gui::tray_entries(service, controllers, name_of)) {
    lines += entry.kind == TrayEntry::Kind::Test ? "test|" : "status|";
    lines += entry.text.toStdString() + "|" + entry.address.toStdString();
    lines += "\n";
  }
  return lines;
}

void test_connected_controller_line_shows_its_battery() {
  ControllerStatus pad = connected(kPad);
  pad.battery = 55;
  VDS_CHECK_EQ(menu(ServiceState::Running, {pad}),
               "status|DualSense: connected, battery 55%|aa:bb:cc:dd:ee:01\n"
               "test|Test DualSense|aa:bb:cc:dd:ee:01\n");
}

void test_charging_controller_line_says_it_charges() {
  ControllerStatus pad = connected(kPad);
  pad.battery = 25;
  pad.charging = true;
  VDS_CHECK_EQ(menu(ServiceState::Running, {pad}),
               "status|DualSense: connected, battery 25%, charging|"
               "aa:bb:cc:dd:ee:01\n"
               "test|Test DualSense|aa:bb:cc:dd:ee:01\n");
}

void test_controller_with_unknown_battery_shows_no_level() {
  VDS_CHECK_EQ(menu(ServiceState::Running, {connected(kSteam)}),
               "status|Steam Controller: connected|steam:FXA1234567B89\n"
               "test|Test Steam Controller|steam:FXA1234567B89\n");
}

void test_controller_that_is_off_gets_no_test_entry() {
  VDS_CHECK_EQ(menu(ServiceState::Running, {off(kSteam)}),
               "status|Steam Controller: not connected|"
               "steam:FXA1234567B89\n");
}

void test_test_entry_follows_its_own_controller() {
  VDS_CHECK_EQ(menu(ServiceState::Running, {off(kSteam), connected(kPad)}),
               "status|Steam Controller: not connected|"
               "steam:FXA1234567B89\n"
               "status|DualSense: connected|aa:bb:cc:dd:ee:01\n"
               "test|Test DualSense|aa:bb:cc:dd:ee:01\n");
}

void test_no_registered_controller_shows_one_line_saying_so() {
  VDS_CHECK_EQ(menu(ServiceState::Running, {}),
               "status|No controllers registered|\n");
}

void test_unreachable_service_replaces_the_stale_list() {
  VDS_CHECK_EQ(menu(ServiceState::Unreachable, {connected(kPad)}),
               "status|Service not running|\n");
}

void test_first_poll_pending_does_not_claim_no_controllers() {
  VDS_CHECK_EQ(menu(ServiceState::Checking, {}),
               "status|Checking the service|\n");
}

} // namespace

void run_tests() {
  test_connected_controller_line_shows_its_battery();
  test_charging_controller_line_says_it_charges();
  test_controller_with_unknown_battery_shows_no_level();
  test_controller_that_is_off_gets_no_test_entry();
  test_test_entry_follows_its_own_controller();
  test_no_registered_controller_shows_one_line_saying_so();
  test_unreachable_service_replaces_the_stale_list();
  test_first_poll_pending_does_not_claim_no_controllers();
}

} // namespace vds::test
