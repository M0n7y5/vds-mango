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
using vds::gui::LowBatteryStep;
using vds::gui::ServiceState;
using vds::gui::TrayAction;
using vds::gui::TrayEntry;
using vds::gui::TrayIconState;

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

std::vector<TrayEntry> entries(ServiceState service,
                               const std::vector<ControllerStatus> &list) {
  return vds::gui::tray_entries(service, list, name_of);
}

/// The actions of a submenu, such as "test,edit".
std::string actions(const TrayEntry &entry) {
  std::string names;
  for (const TrayAction action : entry.actions) {
    names += names.empty() ? "" : ",";
    names += action == TrayAction::Test ? "test" : "edit";
  }
  return names;
}

/// One line per entry: title, address and actions.
std::string menu(ServiceState service,
                 const std::vector<ControllerStatus> &controllers) {
  std::string lines;
  for (const TrayEntry &entry : entries(service, controllers)) {
    lines += entry.text.toStdString() + "|" + entry.address.toStdString() +
             "|" + actions(entry) + "\n";
  }
  return lines;
}

void test_connected_controller_title_shows_its_battery() {
  ControllerStatus pad = connected(kPad);
  pad.battery = 55;
  VDS_CHECK_EQ(entries(ServiceState::Running, {pad}).at(0).text.toStdString(),
               "DualSense: connected, battery 55%");
}

void test_charging_controller_title_says_it_charges() {
  ControllerStatus pad = connected(kPad);
  pad.battery = 25;
  pad.charging = true;
  VDS_CHECK_EQ(entries(ServiceState::Running, {pad}).at(0).text.toStdString(),
               "DualSense: connected, battery 25%, charging");
}

void test_controller_with_unknown_battery_shows_no_level() {
  VDS_CHECK_EQ(entries(ServiceState::Running, {connected(kSteam)})
                   .at(0)
                   .text.toStdString(),
               "Steam Controller: connected");
}

void test_controller_that_is_off_says_so_in_its_title() {
  VDS_CHECK_EQ(
      entries(ServiceState::Running, {off(kSteam)}).at(0).text.toStdString(),
      "Steam Controller: not connected");
}

void test_connected_controller_offers_test_and_edit() {
  VDS_CHECK_EQ(actions(entries(ServiceState::Running, {connected(kPad)}).at(0)),
               "test,edit");
}

void test_controller_that_is_off_offers_only_edit() {
  VDS_CHECK_EQ(actions(entries(ServiceState::Running, {off(kPad)}).at(0)),
               "edit");
}

void test_each_controller_gets_its_own_submenu_in_list_order() {
  VDS_CHECK_EQ(menu(ServiceState::Running, {off(kSteam), connected(kPad)}),
               "Steam Controller: not connected|steam:FXA1234567B89|edit\n"
               "DualSense: connected|aa:bb:cc:dd:ee:01|test,edit\n");
}

void test_no_registered_controller_shows_one_line_saying_so() {
  VDS_CHECK_EQ(menu(ServiceState::Running, {}),
               "No controllers registered||\n");
}

void test_unreachable_service_replaces_the_stale_list() {
  VDS_CHECK_EQ(menu(ServiceState::Unreachable, {connected(kPad)}),
               "Service not running||\n");
}

void test_first_poll_pending_does_not_claim_no_controllers() {
  VDS_CHECK_EQ(menu(ServiceState::Checking, {}), "Checking the service||\n");
}

void test_status_lines_have_no_actions() {
  for (const ServiceState service :
       {ServiceState::Checking, ServiceState::Unreachable}) {
    VDS_CHECK(entries(service, {connected(kPad)}).at(0).actions.empty());
  }
  VDS_CHECK(entries(ServiceState::Running, {}).at(0).actions.empty());
}

void test_battery_change_keeps_the_open_submenu() {
  ControllerStatus before = connected(kPad);
  before.battery = 55;
  ControllerStatus after = connected(kPad);
  after.battery = 45;
  VDS_CHECK(vds::gui::same_tray_controllers(
      entries(ServiceState::Running, {before}),
      entries(ServiceState::Running, {after})));
}

void test_connecting_keeps_the_neighbours_open_submenu() {
  VDS_CHECK(vds::gui::same_tray_controllers(
      entries(ServiceState::Running, {off(kPad), off(kSteam)}),
      entries(ServiceState::Running, {connected(kPad), off(kSteam)})));
}

void test_added_controller_rebuilds_the_menu() {
  VDS_CHECK(!vds::gui::same_tray_controllers(
      entries(ServiceState::Running, {off(kPad)}),
      entries(ServiceState::Running, {off(kPad), off(kSteam)})));
}

void test_replaced_controller_rebuilds_its_submenu_for_the_new_address() {
  VDS_CHECK(!vds::gui::same_tray_controllers(
      entries(ServiceState::Running, {off(kPad)}),
      entries(ServiceState::Running, {off(kSteam)})));
}

void test_first_controller_replaces_the_plain_status_line() {
  VDS_CHECK(!vds::gui::same_tray_controllers(
      entries(ServiceState::Running, {}),
      entries(ServiceState::Running, {off(kPad)})));
}

void test_one_connected_controller_shows_the_normal_icon() {
  VDS_CHECK(vds::gui::tray_icon_state(ServiceState::Running,
                                      {off(kSteam), connected(kPad)}) ==
            TrayIconState::Connected);
}

void test_no_connected_controller_dims_the_icon() {
  VDS_CHECK(vds::gui::tray_icon_state(ServiceState::Running, {off(kPad)}) ==
            TrayIconState::Idle);
}

void test_unreachable_service_warns_despite_a_stale_connected_list() {
  VDS_CHECK(vds::gui::tray_icon_state(ServiceState::Unreachable,
                                      {connected(kPad)}) ==
            TrayIconState::Unreachable);
}

ControllerStatus on_battery(unsigned percent) {
  ControllerStatus pad = connected(kPad);
  pad.battery = percent;
  return pad;
}

void test_battery_dropping_to_the_card_threshold_notifies() {
  VDS_CHECK(vds::gui::low_battery_step(true, on_battery(15)).notify);
}

void test_battery_above_the_card_threshold_does_not_notify() {
  VDS_CHECK(!vds::gui::low_battery_step(true, on_battery(25)).notify);
}

void test_low_battery_does_not_repeat_on_the_next_polls() {
  const LowBatteryStep first = vds::gui::low_battery_step(true, on_battery(15));
  const LowBatteryStep second =
      vds::gui::low_battery_step(first.armed, on_battery(5));
  const LowBatteryStep third =
      vds::gui::low_battery_step(second.armed, on_battery(5));
  VDS_CHECK(!second.notify);
  VDS_CHECK(!third.notify);
}

void test_charging_controller_with_a_low_battery_does_not_notify() {
  ControllerStatus pad = on_battery(5);
  pad.charging = true;
  VDS_CHECK(!vds::gui::low_battery_step(true, pad).notify);
}

void test_charging_rearms_the_notice() {
  ControllerStatus pad = on_battery(5);
  pad.charging = true;
  VDS_CHECK(vds::gui::low_battery_step(false, pad).armed);
}

void test_battery_rising_above_the_threshold_rearms_the_notice() {
  VDS_CHECK(vds::gui::low_battery_step(false, on_battery(25)).armed);
}

void test_disconnecting_rearms_the_notice() {
  VDS_CHECK(vds::gui::low_battery_step(false, off(kPad)).armed);
}

void test_unknown_battery_does_not_notify() {
  VDS_CHECK(!vds::gui::low_battery_step(true, connected(kPad)).notify);
}

void test_unknown_battery_does_not_rearm_a_notice_already_shown() {
  VDS_CHECK(!vds::gui::low_battery_step(false, connected(kPad)).armed);
}

} // namespace

void run_tests() {
  test_connected_controller_title_shows_its_battery();
  test_charging_controller_title_says_it_charges();
  test_controller_with_unknown_battery_shows_no_level();
  test_controller_that_is_off_says_so_in_its_title();
  test_connected_controller_offers_test_and_edit();
  test_controller_that_is_off_offers_only_edit();
  test_each_controller_gets_its_own_submenu_in_list_order();
  test_no_registered_controller_shows_one_line_saying_so();
  test_unreachable_service_replaces_the_stale_list();
  test_first_poll_pending_does_not_claim_no_controllers();
  test_status_lines_have_no_actions();
  test_battery_change_keeps_the_open_submenu();
  test_connecting_keeps_the_neighbours_open_submenu();
  test_added_controller_rebuilds_the_menu();
  test_replaced_controller_rebuilds_its_submenu_for_the_new_address();
  test_first_controller_replaces_the_plain_status_line();
  test_one_connected_controller_shows_the_normal_icon();
  test_no_connected_controller_dims_the_icon();
  test_unreachable_service_warns_despite_a_stale_connected_list();
  test_battery_dropping_to_the_card_threshold_notifies();
  test_battery_above_the_card_threshold_does_not_notify();
  test_low_battery_does_not_repeat_on_the_next_polls();
  test_charging_controller_with_a_low_battery_does_not_notify();
  test_charging_rearms_the_notice();
  test_battery_rising_above_the_threshold_rearms_the_notice();
  test_disconnecting_rearms_the_notice();
  test_unknown_battery_does_not_notify();
  test_unknown_battery_does_not_rearm_a_notice_already_shown();
}

} // namespace vds::test
