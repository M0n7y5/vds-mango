// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_check.hh"

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

#include "vds_config.hh"

namespace vds::test {

void test_one_bad_line_drops_every_controller() {
  // One malformed line zeroes the whole controller list: parse throws and
  // the caller is left with no controllers.
  constexpr std::string_view kText =
      "{\"address\":\"aa:bb:cc:dd:ee:01\",\"profile\":\"\",\"ports\":[]}\n"
      "{\"address\":\"aa:bb:cc:dd:ee:02\",\"profile\":\"\",\"ports\":[],}\n";
  vds::ConfigDb db;
  bool threw = false;
  try {
    db = vds::parse_config_db(kText);
  } catch (const std::exception &) {
    threw = true;
  }
  VDS_CHECK(threw);
  VDS_CHECK_EQ(db.controllers.size(), static_cast<std::size_t>(0));
}

void test_dash_separated_address_normalizes_to_colon_form() {
  VDS_CHECK_EQ(vds::normalize_bluetooth_address("AA-BB-CC-DD-EE-01"),
               std::string("aa:bb:cc:dd:ee:01"));
}

void test_bare_hex_address_normalizes_to_colon_form() {
  VDS_CHECK_EQ(vds::normalize_bluetooth_address("AABBCCDDEE01"),
               std::string("aa:bb:cc:dd:ee:01"));
}

void test_eleven_hex_digits_are_rejected() {
  VDS_CHECK_THROWS(vds::normalize_bluetooth_address("aabbccddee0"));
}

void test_record_without_audio_buffer_uses_the_standard_length() {
  const vds::ConfigDb db = vds::parse_config_db(
      "{\"address\":\"aa:bb:cc:dd:ee:01\",\"profile\":\"\",\"ports\":[]}\n");
  VDS_CHECK_EQ(static_cast<unsigned>(db.controllers.at(0).audio_buffer_length),
               64u);
}

void test_unmeasured_audio_buffer_in_a_record_is_rejected() {
  VDS_CHECK_THROWS(vds::parse_config_db(
      "{\"address\":\"aa:bb:cc:dd:ee:01\",\"profile\":\"\",\"ports\":[],"
      "\"audio_buffer\":\"12\"}\n"));
}

void test_steam_serial_normalizes_to_upper_case() {
  VDS_CHECK_EQ(vds::normalize_controller_address("steam:fxa1234567b89"),
               std::string("steam:FXA1234567B89"));
}

void test_steam_serial_with_a_path_separator_is_rejected() {
  VDS_CHECK_THROWS(vds::normalize_controller_address("steam:../etc"));
}

void test_empty_steam_serial_is_rejected() {
  VDS_CHECK_THROWS(vds::normalize_controller_address("steam:"));
}

void test_record_of_a_steam_controller_is_read() {
  const vds::ConfigDb db = vds::parse_config_db(
      "{\"address\":\"steam:FXA1234567B89\",\"profile\":\"\",\"ports\":[]}\n");
  VDS_CHECK_EQ(db.controllers.at(0).address,
               std::string("steam:FXA1234567B89"));
}

void run_tests() {
  test_one_bad_line_drops_every_controller();
  test_dash_separated_address_normalizes_to_colon_form();
  test_bare_hex_address_normalizes_to_colon_form();
  test_eleven_hex_digits_are_rejected();
  test_record_without_audio_buffer_uses_the_standard_length();
  test_unmeasured_audio_buffer_in_a_record_is_rejected();
  test_steam_serial_normalizes_to_upper_case();
  test_steam_serial_with_a_path_separator_is_rejected();
  test_empty_steam_serial_is_rejected();
  test_record_of_a_steam_controller_is_read();
}

} // namespace vds::test
