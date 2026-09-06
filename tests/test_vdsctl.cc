// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_check.hh"

#include <string>
#include <string_view>

#include "vdsctl_common.hh"

namespace vds::test {

void test_empty_list_reply_names_the_next_command() {
  VDS_CHECK_EQ(vds::format_vdsctl_list_table(""),
               std::string("(no controllers registered; run: vdsctl "
                           "list-targets)\n"));
}

void test_empty_targets_reply_names_the_pairing_tool() {
  VDS_CHECK_EQ(vds::format_vdsctl_targets_table(""),
               std::string("(no attachable controllers; pair one with "
                           "bluetoothctl)\n"));
}

void test_list_table_pads_columns_to_the_widest_cell() {
  constexpr std::string_view kReply =
      "{\"address\":\"aa:bb:cc:dd:ee:01\",\"connected\":true,"
      "\"path\":\"/dev/vds_hcd0\",\"error\":\"\",\"profile\":\"ds5\","
      "\"ports\":[0]}\n"
      "{\"address\":\"11:22:33:44:55:66\",\"connected\":false,"
      "\"path\":\"\",\"error\":\"connect timed out\",\"profile\":\"\","
      "\"ports\":[]}\n";
  constexpr std::string_view kTable =
      "ADDRESS            CONNECTED  PATH           PROFILE  PORTS  ERROR\n"
      "aa:bb:cc:dd:ee:01  yes        /dev/vds_hcd0  ds5      0\n"
      "11:22:33:44:55:66  no                        auto     all    connect "
      "timed out\n";
  VDS_CHECK_EQ(vds::format_vdsctl_list_table(kReply), std::string(kTable));
}

void test_targets_table_pads_columns_to_the_widest_cell() {
  constexpr std::string_view kReply =
      "{\"address\":\"aa:bb:cc:dd:ee:01\",\"name\":\"Wireless Controller\","
      "\"online\":true,\"trusted\":false,\"registered\":true}\n"
      "{\"address\":\"11:22:33:44:55:66\",\"name\":\"DualSense Edge\","
      "\"online\":false,\"trusted\":true,\"registered\":false}\n";
  constexpr std::string_view kTable =
      "ADDRESS            NAME                 ONLINE  TRUSTED  REGISTERED\n"
      "aa:bb:cc:dd:ee:01  Wireless Controller  yes     no       yes\n"
      "11:22:33:44:55:66  DualSense Edge       no      yes      no\n";
  VDS_CHECK_EQ(vds::format_vdsctl_targets_table(kReply), std::string(kTable));
}

void run_tests() {
  test_empty_list_reply_names_the_next_command();
  test_empty_targets_reply_names_the_pairing_tool();
  test_list_table_pads_columns_to_the_widest_cell();
  test_targets_table_pads_columns_to_the_widest_cell();
}

} // namespace vds::test
