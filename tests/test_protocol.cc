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
  // is by design: L2CAP SEQPACKET preserves frame boundaries and short
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

void run_tests() {
#if defined(VDS_TEST_L12)
  test_bt_input_accepts_minimum_frame();
#elif defined(VDS_TEST_L11C)
  test_feature_set_packet_keeps_payload_tail();
#else
  test_feature_set_packet_keeps_payload_tail();
  test_bt_input_accepts_minimum_frame();
#endif
}

} // namespace vds::test
