// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include "vds_check.hh"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vds_steam.hh"

namespace vds::test {

namespace {

using StateReport = std::array<std::uint8_t, 54>;
using Byte = std::uint8_t;

constexpr std::size_t kButtons = 2;
constexpr std::size_t kLeftTrigger = 6;
constexpr std::size_t kLeftStickX = 10;
constexpr std::size_t kLeftStickY = 12;
constexpr std::uint32_t kA = 0x00000001;
constexpr std::uint32_t kMenu = 0x00000040;
constexpr std::uint32_t kDpadRight = 0x00000800;
constexpr std::uint32_t kDpadUp = 0x00002000;
constexpr std::uint32_t kView = 0x00004000;
constexpr std::uint32_t kRightPadTouch = 0x00200000;
constexpr std::uint32_t kLeftPadTouch = 0x02000000;
constexpr std::uint32_t kRightPadClick = 0x00400000;
constexpr std::uint32_t kLeftPadClick = 0x04000000;

StateReport state_report(std::uint32_t buttons = 0) {
  StateReport report{};
  report[0] = 0x42;
  for (std::size_t i = 0; i < sizeof(buttons); ++i) {
    report[kButtons + i] = static_cast<Byte>(buttons >> (8 * i));
  }
  return report;
}

void put16(StateReport &report, std::size_t offset, int value) {
  report[offset] = static_cast<Byte>(value & 0xff);
  report[offset + 1] = static_cast<Byte>((value >> 8) & 0xff);
}

UsbInputReport translated(const StateReport &report) {
  SteamInputTranslator translator;
  const auto usb = translator.translate(report);
  VDS_CHECK(usb.has_value());
  return usb.value_or(UsbInputReport{});
}

/* Frames of the DualSense USB audio format: speaker pair, haptics pair. */
std::vector<Byte> pcm(std::size_t frames, std::array<int, 4> sample) {
  std::vector<Byte> bytes;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    for (const int value : sample) {
      bytes.push_back(static_cast<Byte>(value & 0xff));
      bytes.push_back(static_cast<Byte>((value >> 8) & 0xff));
    }
  }
  return bytes;
}

std::vector<Byte> output_report(Byte flags0, Byte flags2, Byte right,
                                Byte left) {
  std::vector<Byte> report(48, 0);
  report[0] = 0x02;
  report[1] = flags0;
  report[3] = right;
  report[4] = left;
  report[39] = flags2;
  return report;
}

} // namespace

void test_mulaw_encodes_silence_as_ff() {
  VDS_CHECK_EQ(mulaw_encode(0), Byte{0xff});
}

void test_mulaw_encodes_positive_full_scale_as_80() {
  VDS_CHECK_EQ(mulaw_encode(32767), Byte{0x80});
}

void test_mulaw_encodes_negative_full_scale_as_00() {
  VDS_CHECK_EQ(mulaw_encode(-32768), Byte{0x00});
}

void test_mulaw_encodes_a_mid_level_sample() {
  // 1000 + bias 132 = 1132: segment 3, mantissa 1.
  VDS_CHECK_EQ(mulaw_encode(1000), Byte{0xce});
}

void test_state_of_a_resting_controller_becomes_a_resting_dualsense() {
  // Captured from a Steam Controller lying on a table.
  const StateReport report = {
      0x42, 0x65, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x68,
      0x02, 0x62, 0x00, 0x3b, 0x01, 0x49, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x14, 0x60,
      0x3c, 0x4e, 0x00, 0xe8, 0xf5, 0x72, 0x3f, 0x00, 0x00, 0x00, 0x00,
      0xff, 0xff, 0x66, 0x7d, 0x43, 0xf6, 0xbf, 0xfd, 0x9f, 0x17};
  const UsbInputReport usb = translated(report);
  const std::array<Byte, 12> head = {0x01, 130,  127,  129,  127,  0x00,
                                     0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
  VDS_CHECK(std::equal(head.begin(), head.end(), usb.begin()));
  // Gyro 0, 0, -1 and gravity on Z become 0, -1, 0 and gravity on Y.
  const std::array<Byte, 16> motion = {0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
                                       0x27, 0x00, 0xb9, 0x1f, 0x0c, 0x05,
                                       0x80, 0x3d, 0x20, 0xb5};
  VDS_CHECK(std::equal(motion.begin(), motion.end(), usb.begin() + 16));
}

void test_a_button_becomes_cross() {
  VDS_CHECK_EQ(translated(state_report(kA))[8], Byte{0x28});
}

void test_stick_pushed_up_reads_zero() {
  StateReport report = state_report();
  put16(report, kLeftStickY, 32767);
  VDS_CHECK_EQ(translated(report)[2], Byte{0x00});
}

void test_stick_pushed_right_reads_full_scale() {
  StateReport report = state_report();
  put16(report, kLeftStickX, 32767);
  VDS_CHECK_EQ(translated(report)[1], Byte{0xff});
}

void test_pulled_trigger_sets_its_axis() {
  StateReport report = state_report();
  put16(report, kLeftTrigger, 32767);
  VDS_CHECK_EQ(translated(report)[5], Byte{0xff});
}

void test_pulled_trigger_presses_its_button() {
  StateReport report = state_report();
  put16(report, kLeftTrigger, 32767);
  VDS_CHECK_EQ(translated(report)[9], Byte{0x04});
}

void test_dpad_up_and_right_is_the_north_east_hat() {
  VDS_CHECK_EQ(translated(state_report(kDpadUp | kDpadRight))[8], Byte{0x01});
}

void test_view_button_on_the_left_becomes_create() {
  VDS_CHECK_EQ(translated(state_report(kView))[9], Byte{0x10});
}

void test_menu_button_on_the_right_becomes_options() {
  VDS_CHECK_EQ(translated(state_report(kMenu))[9], Byte{0x20});
}

void test_untouched_pads_report_no_touch() {
  const UsbInputReport usb = translated(state_report());
  VDS_CHECK_EQ(usb[33] & 0x80, 0x80);
  VDS_CHECK_EQ(usb[37] & 0x80, 0x80);
}

void test_left_pad_centre_is_the_centre_of_the_left_half() {
  // x 480, y 540 of 1920 by 1080, as 12 bits each.
  const UsbInputReport usb = translated(state_report(kLeftPadTouch));
  const std::array<Byte, 4> point = {0x00, 0xe0, 0xc1, 0x21};
  VDS_CHECK(std::equal(point.begin(), point.end(), usb.begin() + 33));
}

void test_right_pad_centre_is_the_centre_of_the_right_half() {
  // x 1440, y 540.
  const UsbInputReport usb = translated(state_report(kRightPadTouch));
  const std::array<Byte, 4> point = {0x00, 0xa0, 0xc5, 0x21};
  VDS_CHECK(std::equal(point.begin(), point.end(), usb.begin() + 37));
}

void test_a_new_touch_gets_a_new_id() {
  SteamInputTranslator translator;
  (void)translator.translate(state_report(kLeftPadTouch));
  (void)translator.translate(state_report());
  const auto usb = translator.translate(state_report(kLeftPadTouch));
  VDS_CHECK(usb && (*usb)[33] == 0x01);
}

void test_pad_click_presses_the_touchpad_button() {
  VDS_CHECK_EQ(translated(state_report(kLeftPadClick))[10], Byte{0x02});
}

void test_battery_report_is_not_an_input_report() {
  SteamInputTranslator translator;
  const std::array<Byte, 3> battery = {0x43, 0x01, 85};
  VDS_CHECK(!translator.translate(battery));
}

void test_battery_report_sets_the_reported_charge() {
  SteamInputTranslator translator;
  const std::array<Byte, 3> battery = {0x43, 0x01, 85};
  (void)translator.translate(battery);
  VDS_CHECK(translator.battery() && translator.battery()->percent == 85 &&
            !translator.battery()->charging);
}

void test_battery_charge_reaches_the_dualsense_report_in_ten_percent_steps() {
  SteamInputTranslator translator;
  const std::array<Byte, 3> battery = {0x43, 0x01, 85};
  (void)translator.translate(battery);
  const auto usb = translator.translate(state_report());
  VDS_CHECK(usb && (*usb)[53] == 0x08);
}

void test_charging_battery_sets_the_dualsense_charging_state() {
  SteamInputTranslator translator;
  const std::array<Byte, 3> battery = {0x43, 0x02, 40};
  (void)translator.translate(battery);
  const auto usb = translator.translate(state_report());
  VDS_CHECK(usb && (*usb)[53] == 0x14);
}

void test_truncated_state_report_is_ignored() {
  SteamInputTranslator translator;
  const StateReport report = state_report();
  VDS_CHECK(!translator.translate(std::span(report).first(45)));
}

void test_wireless_disconnect_means_switched_off() {
  const std::array<Byte, 2> report = {0x79, 0x01};
  VDS_CHECK(steam_controller_switched_off(report));
}

void test_wireless_connect_does_not_mean_switched_off() {
  const std::array<Byte, 2> report = {0x79, 0x02};
  VDS_CHECK(!steam_controller_switched_off(report));
}

void test_calibration_states_the_scale_of_the_translated_sensors() {
  const auto calibration = steam_virtual_feature_reports("FXA1")[0];
  VDS_CHECK_EQ(calibration.size(), std::size_t{41});
  if (calibration.size() != 41) {
    return;
  }
  // Report 0x05, no gyro bias, +-8847 counts at 540 deg/s, +-8192 per g.
  const std::array<Byte, 35> expected = {
      0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8f, 0x22, 0x71, 0xdd, 0x8f,
      0x22, 0x71, 0xdd, 0x8f, 0x22, 0x71, 0xdd, 0x1c, 0x02, 0x1c, 0x02, 0x00,
      0x20, 0x00, 0xe0, 0x00, 0x20, 0x00, 0xe0, 0x00, 0x20, 0x00, 0xe0};
  VDS_CHECK(std::equal(expected.begin(), expected.end(), calibration.begin()));
}

void test_pairing_report_has_the_size_hid_playstation_expects() {
  VDS_CHECK_EQ(steam_virtual_feature_reports("FXA1")[1].size(),
               std::size_t{20});
}

void test_pairing_address_is_locally_administered() {
  const auto pairing = steam_virtual_feature_reports("FXA1")[1];
  VDS_CHECK(pairing.size() == 20 && (pairing[6] & 0x03) == 0x02);
}

void test_pairing_address_differs_between_controllers() {
  VDS_CHECK(steam_virtual_feature_reports("FXA1")[1] !=
            steam_virtual_feature_reports("FXA2")[1]);
}

void test_pairing_address_is_stable_for_one_controller() {
  VDS_CHECK(steam_virtual_feature_reports("FXA1")[1] ==
            steam_virtual_feature_reports("FXA1")[1]);
}

void test_firmware_report_has_the_size_hid_playstation_expects() {
  VDS_CHECK_EQ(steam_virtual_feature_reports("FXA1")[2].size(),
               std::size_t{64});
}

void test_firmware_report_states_a_current_dualsense_firmware() {
  // Update version 0x0630 where hid-playstation and the daemon read it.
  const auto firmware = steam_virtual_feature_reports("FXA1")[2];
  VDS_CHECK(firmware.size() == 64 && firmware[44] == 0x30 &&
            firmware[45] == 0x06);
}

void test_pairing_report_carries_the_bytes_a_dualsense_sends_after_it() {
  const auto pairing = steam_virtual_feature_reports("FXA1")[1];
  VDS_CHECK(pairing.size() == 20 && pairing[7] == 0x08 &&
            pairing[8] == 0x25 && pairing[9] == 0x00);
}

void test_rumble_scales_the_motors_to_sixteen_bits() {
  const auto rumble =
      steam_rumble_report(output_report(0x03, 0, 0x40, 0xff), 100);
  const SteamRumbleReport expected = {0x80, 0x00, 0x00, 0x00, 0xff,
                                      0xff, 0x00, 0x40, 0x40, 0x00};
  VDS_CHECK(rumble && *rumble == expected);
}

void test_rumble_follows_the_newer_vibration_flag() {
  VDS_CHECK(steam_rumble_report(output_report(0x00, 0x04, 0x40, 0xff), 100));
}

void test_output_report_without_rumble_flags_leaves_the_motors_alone() {
  VDS_CHECK(!steam_rumble_report(output_report(0x00, 0x00, 0x40, 0xff), 100));
}

void test_zero_rumble_is_not_active() {
  const auto rumble = steam_rumble_report(output_report(0x03, 0, 0, 0), 100);
  VDS_CHECK(rumble && !steam_rumble_active(*rumble));
}

void test_rumble_at_half_strength_halves_the_motor_speeds() {
  // 0xff is 65535 at full strength and 0x40 is 16448: half is 0x7fff and
  // 0x2020.
  const auto rumble =
      steam_rumble_report(output_report(0x03, 0, 0x40, 0xff), 50);
  const SteamRumbleReport expected = {0x80, 0x00, 0x00, 0x00, 0xff,
                                      0x7f, 0x00, 0x20, 0x20, 0x00};
  VDS_CHECK(rumble && *rumble == expected);
}

void test_rumble_turned_off_is_not_active() {
  const auto rumble =
      steam_rumble_report(output_report(0x03, 0, 0x40, 0xff), 0);
  VDS_CHECK(rumble && !steam_rumble_active(*rumble));
}

void test_settings_turn_lizard_mode_off_and_the_motion_sensors_on() {
  const auto report = steam_settings_report();
  const std::array<Byte, 10> expected = {0x01, 0x87, 0x06, 0x09, 0x00,
                                         0x00, 0x30, 0x18, 0x00, 0x00};
  VDS_CHECK(std::equal(expected.begin(), expected.end(), report.begin()));
}

void test_stream_starts_on_the_grips_at_four_kilohertz_mulaw() {
  const SteamStreamConfig expected = {0x86, 0x02, 0x02, 0x09};
  VDS_CHECK(steam_stream_config_report(true) == expected);
}

void test_stream_stop_names_the_grips() {
  const SteamStreamConfig expected = {0x86, 0x01, 0x02, 0x00};
  VDS_CHECK(steam_stream_config_report(false) == expected);
}

void test_pressing_the_left_pad_plays_a_strong_click_on_it() {
  const SteamClickReport expected = {0x82, 0x00, 0x02, 0x00};
  VDS_CHECK(steam_pad_click_report(0, true, PadClick::Strong) == expected);
}

void test_letting_go_of_the_right_pad_plays_a_light_click_on_it() {
  const SteamClickReport expected = {0x82, 0x01, 0x01, 0x00};
  VDS_CHECK(steam_pad_click_report(1, false, PadClick::Strong) == expected);
}

void test_soft_click_plays_the_click_twelve_decibels_quieter() {
  // -12 as a signed byte is 0xf4.
  const SteamClickReport expected = {0x82, 0x00, 0x01, 0xf4};
  VDS_CHECK(steam_pad_click_report(0, true, PadClick::Soft) == expected);
}

void test_click_turned_off_plays_nothing() {
  VDS_CHECK(!steam_pad_click_report(0, true, PadClick::Off));
}

void test_translator_reports_the_pressed_pad() {
  SteamInputTranslator translator;
  (void)translator.translate(state_report(kRightPadClick));
  VDS_CHECK(!translator.pad_clicked(0) && translator.pad_clicked(1));
}

void test_translator_reports_the_pad_let_go() {
  SteamInputTranslator translator;
  (void)translator.translate(state_report(kRightPadClick));
  (void)translator.translate(state_report());
  VDS_CHECK(!translator.pad_clicked(1));
}

void test_twelve_usb_frames_make_one_stream_sample() {
  SteamHapticsEncoder encoder;
  (void)encoder.push_usb_audio(pcm(12, {0, 0, 1000, -1000}));
  VDS_CHECK_EQ(encoder.queued_frames(), std::size_t{1});
}

void test_stream_packet_carries_left_then_right_grip() {
  // The grips play at 80%: 1250 becomes 1000, which is 0xce in mu-law.
  SteamHapticsEncoder encoder;
  (void)encoder.push_usb_audio(pcm(12, {0, 0, 1250, -1250}));
  const SteamStreamPacket packet = encoder.pop_packet();
  VDS_CHECK(packet[0] == 0x88 && packet[1] == 31 && packet[2] == 0xce &&
            packet[33] == 0x4e);
}

void test_stream_packet_is_filled_up_with_silence() {
  SteamHapticsEncoder encoder;
  (void)encoder.push_usb_audio(pcm(12, {0, 0, 1000, -1000}));
  const SteamStreamPacket packet = encoder.pop_packet();
  VDS_CHECK(packet[3] == 0xff && packet[32] == 0xff && packet[34] == 0xff &&
            packet[63] == 0xff);
}

void test_stream_sample_is_the_average_of_its_usb_frames() {
  SteamHapticsEncoder encoder;
  (void)encoder.push_usb_audio(pcm(6, {0, 0, 2500, 2500}));
  (void)encoder.push_usb_audio(pcm(6, {0, 0, 0, 0}));
  VDS_CHECK_EQ(encoder.pop_packet()[2], Byte{0xce});
}

void test_speaker_sound_is_not_a_haptics_signal() {
  SteamHapticsEncoder encoder;
  VDS_CHECK(!encoder.push_usb_audio(pcm(12, {9000, -9000, 0, 0})));
}

void test_haptics_sound_is_a_haptics_signal() {
  SteamHapticsEncoder encoder;
  VDS_CHECK(encoder.push_usb_audio(pcm(12, {0, 0, 0, 1})));
}

void test_usb_frame_split_across_two_pushes_is_reassembled() {
  SteamHapticsEncoder encoder;
  const std::vector<Byte> bytes = pcm(12, {0, 0, 1250, -1250});
  (void)encoder.push_usb_audio(std::span(bytes).first(5));
  (void)encoder.push_usb_audio(std::span(bytes).subspan(5));
  VDS_CHECK_EQ(encoder.pop_packet()[2], Byte{0xce});
}

void test_full_scale_haptics_reach_the_grips_at_eighty_percent() {
  // 32767 * 0.8 = 26213: top segment, mantissa 9.
  SteamHapticsEncoder encoder;
  (void)encoder.push_usb_audio(pcm(12, {0, 0, 32767, 32767}));
  VDS_CHECK_EQ(encoder.pop_packet()[2], Byte{0x86});
}

void test_queue_keeps_at_most_three_packets() {
  SteamHapticsEncoder encoder;
  (void)encoder.push_usb_audio(pcm(12 * 100, {0, 0, 1000, 1000}));
  VDS_CHECK_EQ(encoder.queued_frames(), std::size_t{93});
}

void test_haptics_at_half_strength_halve_the_sample() {
  // 2000 becomes 1000, which is 0xce in mu-law.
  SteamHapticsEncoder encoder;
  encoder.set_strength(50);
  (void)encoder.push_usb_audio(pcm(12, {0, 0, 2000, 2000}));
  VDS_CHECK_EQ(encoder.pop_packet()[2], Byte{0xce});
}

void test_haptics_turned_off_are_no_signal() {
  SteamHapticsEncoder encoder;
  encoder.set_strength(0);
  VDS_CHECK(!encoder.push_usb_audio(pcm(12, {0, 0, 1000, 1000})));
}

void run_tests() {
  test_mulaw_encodes_silence_as_ff();
  test_mulaw_encodes_positive_full_scale_as_80();
  test_mulaw_encodes_negative_full_scale_as_00();
  test_mulaw_encodes_a_mid_level_sample();
  test_state_of_a_resting_controller_becomes_a_resting_dualsense();
  test_a_button_becomes_cross();
  test_stick_pushed_up_reads_zero();
  test_stick_pushed_right_reads_full_scale();
  test_pulled_trigger_sets_its_axis();
  test_pulled_trigger_presses_its_button();
  test_dpad_up_and_right_is_the_north_east_hat();
  test_view_button_on_the_left_becomes_create();
  test_menu_button_on_the_right_becomes_options();
  test_untouched_pads_report_no_touch();
  test_left_pad_centre_is_the_centre_of_the_left_half();
  test_right_pad_centre_is_the_centre_of_the_right_half();
  test_a_new_touch_gets_a_new_id();
  test_pad_click_presses_the_touchpad_button();
  test_battery_report_is_not_an_input_report();
  test_battery_report_sets_the_reported_charge();
  test_battery_charge_reaches_the_dualsense_report_in_ten_percent_steps();
  test_charging_battery_sets_the_dualsense_charging_state();
  test_truncated_state_report_is_ignored();
  test_wireless_disconnect_means_switched_off();
  test_wireless_connect_does_not_mean_switched_off();
  test_calibration_states_the_scale_of_the_translated_sensors();
  test_pairing_report_has_the_size_hid_playstation_expects();
  test_pairing_address_is_locally_administered();
  test_pairing_address_differs_between_controllers();
  test_pairing_address_is_stable_for_one_controller();
  test_firmware_report_has_the_size_hid_playstation_expects();
  test_firmware_report_states_a_current_dualsense_firmware();
  test_pairing_report_carries_the_bytes_a_dualsense_sends_after_it();
  test_rumble_scales_the_motors_to_sixteen_bits();
  test_rumble_follows_the_newer_vibration_flag();
  test_output_report_without_rumble_flags_leaves_the_motors_alone();
  test_zero_rumble_is_not_active();
  test_rumble_at_half_strength_halves_the_motor_speeds();
  test_rumble_turned_off_is_not_active();
  test_settings_turn_lizard_mode_off_and_the_motion_sensors_on();
  test_stream_starts_on_the_grips_at_four_kilohertz_mulaw();
  test_stream_stop_names_the_grips();
  test_pressing_the_left_pad_plays_a_strong_click_on_it();
  test_letting_go_of_the_right_pad_plays_a_light_click_on_it();
  test_soft_click_plays_the_click_twelve_decibels_quieter();
  test_click_turned_off_plays_nothing();
  test_translator_reports_the_pressed_pad();
  test_translator_reports_the_pad_let_go();
  test_twelve_usb_frames_make_one_stream_sample();
  test_stream_packet_carries_left_then_right_grip();
  test_stream_packet_is_filled_up_with_silence();
  test_stream_sample_is_the_average_of_its_usb_frames();
  test_speaker_sound_is_not_a_haptics_signal();
  test_haptics_sound_is_a_haptics_signal();
  test_usb_frame_split_across_two_pushes_is_reassembled();
  test_full_scale_haptics_reach_the_grips_at_eighty_percent();
  test_queue_keeps_at_most_three_packets();
  test_haptics_at_half_strength_halve_the_sample();
  test_haptics_turned_off_are_no_signal();
}

} // namespace vds::test
