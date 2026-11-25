// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_check.hh"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "vds_protocol.hh"

namespace vds::test {

void test_feature_set_packet_keeps_payload_tail() {
  // L11c: asserts the USB tail survives feature_set_packet. UNVERIFIED
  // contract: current code writes the DualSense BT CRC over the tail,
  // matching the pad's GET framing. Keep WILL_FAIL until settled on
  // hardware; do not "fix" the code to satisfy this test without pad
  // evidence.
  const std::array<std::uint8_t, 41> report = {
      0x05, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
      0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
      0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
      0x21, 0x22, 0x23, 0x24, 0xaa, 0xaa, 0xaa, 0xaa};
  const auto packet = vds::feature_set_packet(report);
  VDS_CHECK_EQ(packet.size(), static_cast<std::size_t>(42));
  if (packet.size() < 42) {
    return;
  }
  vds::test::check_eq(__FILE__, __LINE__, "L11c: packet[38] == 0xaa",
                      packet[38], static_cast<std::uint8_t>(0xaa));
  vds::test::check_eq(__FILE__, __LINE__, "L11c: packet[39] == 0xaa",
                      packet[39], static_cast<std::uint8_t>(0xaa));
  vds::test::check_eq(__FILE__, __LINE__, "L11c: packet[40] == 0xaa",
                      packet[40], static_cast<std::uint8_t>(0xaa));
  vds::test::check_eq(__FILE__, __LINE__, "L11c: packet[41] == 0xaa",
                      packet[41], static_cast<std::uint8_t>(0xaa));
}

void test_bt_input_accepts_minimum_frame() {
  // L12: 66 bytes carries the complete 63-byte USB payload, so acceptance
  // is by design: hidraw hands out whole reports and short
  // frames only come from nonconforming pads. The trailing counter/CRC of a
  // 79-byte frame is never read.
  const std::array<std::uint8_t, 66> packet = {
      0xa1, 0x31, 0x01, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22,
      0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
      0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
      0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43,
      0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e};
  const std::optional<vds::UsbInputReport> converted =
      vds::bt_input_to_usb_input(packet);
  const bool present = converted.has_value();
  vds::test::check_eq(__FILE__, __LINE__,
                      "L12: converted.has_value() == true", present, true);
  if (!converted.has_value()) {
    return;
  }
  vds::test::check_eq(__FILE__, __LINE__, "L12: usb[0] == 0x01",
                      (*converted)[0], static_cast<std::uint8_t>(0x01));
  vds::test::check_eq(__FILE__, __LINE__, "L12: usb[1] == packet[3]",
                      (*converted)[1], static_cast<std::uint8_t>(0x10));
  vds::test::check_eq(__FILE__, __LINE__, "L12: usb[63] == packet[65]",
                      (*converted)[63], static_cast<std::uint8_t>(0x4e));
}

UsbInputReport input_report_with_battery_byte(std::uint8_t status) {
  UsbInputReport report{};
  report[0] = 0x01;
  report[53] = status;
  return report;
}

void test_battery_level_counts_ten_percent_steps() {
  const auto battery = usb_input_battery(input_report_with_battery_byte(0x08));
  VDS_CHECK(battery.has_value());
  VDS_CHECK_EQ(battery->percent, 85u);
  VDS_CHECK_EQ(battery->charging, false);
}

void test_battery_on_the_cable_reports_charging() {
  const auto battery = usb_input_battery(input_report_with_battery_byte(0x13));
  VDS_CHECK(battery.has_value());
  VDS_CHECK_EQ(battery->percent, 35u);
  VDS_CHECK_EQ(battery->charging, true);
}

void test_battery_charging_fault_reports_no_level() {
  VDS_CHECK(!usb_input_battery(input_report_with_battery_byte(0xa4)));
}

// The first 46 bytes of report 0x20 as a DualSense on update 0630 sends it.
std::array<std::uint8_t, 46> firmware_info_report() {
  std::array<std::uint8_t, 46> report{};
  report[0] = 0x20;
  report[28] = 0x2a;
  report[29] = 0x00;
  report[30] = 0x10;
  report[31] = 0x01;
  report[44] = 0x30;
  report[45] = 0x06;
  return report;
}

void test_firmware_info_reads_the_version_the_kernel_driver_reads() {
  // hid-playstation: fw_version = get_unaligned_le32(&buf[28]) of report 0x20.
  const auto firmware = usb_firmware_info(firmware_info_report());
  VDS_CHECK(firmware.has_value());
  VDS_CHECK_EQ(firmware->version, 0x0110002au);
}

void test_firmware_info_reads_the_update_version_sony_publishes() {
  // hid-playstation: update_version = get_unaligned_le16(&buf[44]).
  const auto firmware = usb_firmware_info(firmware_info_report());
  VDS_CHECK(firmware.has_value());
  VDS_CHECK_EQ(firmware->update_version, static_cast<std::uint16_t>(0x0630));
}

void test_firmware_info_of_a_truncated_report_is_unknown() {
  const auto report = firmware_info_report();
  VDS_CHECK(!usb_firmware_info(std::span(report).first(45)));
}

void run_tests() {
#if defined(VDS_TEST_L12)
  test_bt_input_accepts_minimum_frame();
  test_battery_level_counts_ten_percent_steps();
  test_battery_on_the_cable_reports_charging();
  test_battery_charging_fault_reports_no_level();
  test_firmware_info_reads_the_version_the_kernel_driver_reads();
  test_firmware_info_reads_the_update_version_sony_publishes();
  test_firmware_info_of_a_truncated_report_is_unknown();
#elif defined(VDS_TEST_L11C)
  test_feature_set_packet_keeps_payload_tail();
#else
  test_feature_set_packet_keeps_payload_tail();
  test_bt_input_accepts_minimum_frame();
  test_battery_level_counts_ten_percent_steps();
  test_battery_on_the_cable_reports_charging();
  test_battery_charging_fault_reports_no_level();
  test_firmware_info_reads_the_version_the_kernel_driver_reads();
  test_firmware_info_reads_the_update_version_sony_publishes();
  test_firmware_info_of_a_truncated_report_is_unknown();
#endif
}

} // namespace vds::test
