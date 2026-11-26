// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <linux/input.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <systemd/sd-login.h>
#include <unistd.h>

#include "uapi/vds.h"
#include "unique_fd.hh"
#include "vds/ds5_protocol.h"
#include "vds_bluez.hh"
#include "vds_bt.hh"
#include "vds_build_info.hh"
#include "vds_common.hh"
#include "vds_config.hh"
#include "vds_fs.hh"
#include "vds_io.hh"
#include "vds_log.hh"
#include "vds_profile.hh"
#include "vds_protocol.hh"
#include "vds_sleep.hh"
#include "vds_steam_pad.hh"
#include "vds_udev.hh"
#include "vds_usbip.hh"
#include "vdsd_common.hh"

namespace {

using Clock = std::chrono::steady_clock;
using vds::duration_us;
using vds::hex_bytes;
using vds::hex_u8;

constexpr const char *kDefaultControlSocket = "/run/vdsd.sock";
constexpr const char *kInstanceLockPath = "/run/vdsd.lock";
constexpr const char *kVirtualPortProviderUnavailable =
    "the vhci_hcd kernel module is not loaded (sudo modprobe vhci-hcd)";
constexpr std::size_t kTraceDumpMaxBytes = 96;
constexpr std::size_t kUsbInputButtonsOffset = 8;
constexpr std::size_t kUsbInputButtonsSize = 4;
constexpr std::size_t kUsbInputMuteButtonOffset = 10;
constexpr std::size_t kUsbInputHeadsetOffset = 54;
constexpr std::uint8_t kUsbInputHeadphonesPluggedMask = 0x01;
constexpr std::uint8_t kUsbInputMicPluggedMask = 0x02;
constexpr std::uint8_t kUsbInputMuteButtonMask = 0x04;
constexpr std::uint64_t kInputTraceSummaryInterval = 1000;
constexpr std::uint64_t kOutputTraceSummaryInterval = 250;
constexpr int kMaxPortFramesPerWake = 64;
constexpr int kMaxBtPacketsPerWake = 64;
constexpr int kPendingOutputPollMs = 2;
constexpr auto kInputTraceGapWarn = std::chrono::milliseconds(20);
constexpr auto kInputTraceSlowWriteWarn = std::chrono::milliseconds(5);
constexpr auto kOutputTraceSlowWarn = std::chrono::milliseconds(5);
constexpr auto kOutputTraceFeatureSlowWarn = std::chrono::milliseconds(20);
/*
 * 0x36 carries both a haptics block and a 10 ms Opus speaker block. Pace the
 * combined packet at the speaker frame interval; using the 64-byte haptics
 * duration here lets speaker audio drift into periodic underruns after a few
 * seconds.
 */
constexpr auto kAudioOutputInterval = std::chrono::milliseconds(10);
constexpr auto kHapticsOutputBlockedRetry = std::chrono::milliseconds(2);
constexpr auto kSleepUnplugWait = std::chrono::milliseconds(1000);
constexpr auto kSleepUnplugPoll = std::chrono::milliseconds(5);
constexpr int kInitialFeatureReportPollMs = 5000;
/*
 * What is listed here is hidden from every program but vdsd by
 * 72-vds-dualsense-hide.rules: a Bluetooth controller by its address, and
 * every Steam Controller dongle by kSteamHiddenMarker.
 */
constexpr const char *kHiddenPadDir = "/run/vds/hidden";
constexpr const char *kSteamHiddenMarker = "steam";
/* How long the Pucks stay claimed after the Add wizard last asked. */
constexpr auto kPuckClaimHold = std::chrono::seconds(20);
constexpr std::uint8_t kPairingInfoReportId = 0x09;
constexpr std::size_t kPairingInfoAddressMsbOffset = 6;
constexpr auto kSharingProbeTimeout = std::chrono::milliseconds(400);
constexpr unsigned kSharingProbeAudioReports = 8;
constexpr unsigned kSharingProbeMinAudioReports = 4;
constexpr unsigned kMaxAcceptFailures = 16;
/*
 * The loop is single threaded, so a control client that stalls stalls every
 * pad. Bound the whole exchange instead of trusting the client.
 */
constexpr auto kControlClientTimeout = std::chrono::milliseconds(200);
constexpr std::size_t kControlRequestMaxBytes = 4096;
constexpr std::size_t kMaxPendingAudioChunks = 8;
constexpr std::size_t kFreshPendingAudioChunks = 4;
constexpr std::size_t kMaxPendingControlWrites = 16;
constexpr std::uint8_t kTestCommandReportId = 0x80;
constexpr std::uint8_t kTestCommandResultReportId = 0x81;
constexpr std::uint8_t kTestCommandCompleteStatus = 0x02;
constexpr std::uint8_t kTestCommandAudioDevice = 0x06;
constexpr std::uint8_t kTestCommandWaveoutPrepare = 0x04;
constexpr std::uint8_t kTestCommandWaveoutControl = 0x02;
constexpr std::uint8_t kTestCommandSpeakerParam = 0x08;
constexpr std::uint32_t kSpeakerWaveoutFrequencyHz = 1000;
constexpr std::uint32_t kSpeakerWaveoutPeriodFrames =
    VDS_AUDIO_SAMPLE_RATE / kSpeakerWaveoutFrequencyHz;
constexpr double kTwoPi = 6.28318530717958647692;
/* Arbitrary fixed amplitude used only for synthetic WebHID speaker waveout. */
constexpr std::int16_t kSpeakerWaveoutAmplitude = 12000;
/* The buzz of the control command: strong enough to feel, over in 300 ms. */
constexpr std::uint32_t kTestBuzzFrequencyHz = 150;
constexpr std::uint32_t kTestBuzzPeriodFrames =
    VDS_AUDIO_SAMPLE_RATE / kTestBuzzFrequencyHz;
constexpr std::uint32_t kTestBuzzFrames = VDS_AUDIO_SAMPLE_RATE * 3 / 10;
constexpr std::int16_t kTestBuzzAmplitude = 24000;
constexpr std::array<std::uint8_t, 3> kInitialFeatureReportIds = {0x09, 0x20,
                                                                  0x05};

class PortIoError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct Options {
  std::string socket = kDefaultControlSocket;
  std::string log_path;
  std::string db_path;
};

struct LatencyTraceStats {
  std::uint64_t count = 0;
  std::uint64_t total_us = 0;
  std::uint64_t max_us = 0;
};

struct TraceState {
  std::vector<std::uint8_t> last_hid_out;
  std::uint64_t dropped_usb_frame_count = 0;
  std::uint64_t dropped_audio_haptics_count = 0;
  std::uint64_t queue_dropped_audio_haptics_count = 0;
  std::uint64_t stale_audio_haptics_count = 0;
  std::uint64_t blocked_audio_haptics_count = 0;
  std::uint64_t deferred_bt_state_count = 0;
  std::uint64_t coalesced_bt_state_count = 0;
  std::uint64_t blocked_bt_state_count = 0;
  std::uint64_t audio_usb_frame_count = 0;
  std::uint64_t bt_input_count = 0;
  std::uint64_t bt_mic_packet_count = 0;
  std::uint64_t bt_mic_drop_count = 0;
  std::uint64_t bt_mic_decode_fail_count = 0;
  std::uint64_t audio_in_forward_count = 0;
  LatencyTraceStats output_hid_latency;
  LatencyTraceStats output_feature_latency;
  LatencyTraceStats output_audio_latency;
  LatencyTraceStats output_audio_extract_latency;
  LatencyTraceStats output_audio_send_latency;
  std::uint64_t input_write_count = 0;
  std::uint64_t input_write_total_us = 0;
  std::uint64_t input_write_max_us = 0;
  std::uint64_t input_gap_max_us = 0;
  Clock::time_point last_input_time{};
  bool have_last_input_time = false;
  std::array<std::uint8_t, kUsbInputButtonsSize> last_input_buttons{};
  bool have_last_input_buttons = false;
  bool audio_haptics_nonzero_seen = false;
  bool haptics_burst_active = false;
  std::uint64_t haptics_burst_chunks = 0;
  std::uint64_t audio_encode_fail_count = 0;
  std::uint64_t dropped_control_write_count = 0;
  std::uint64_t coalesced_control_write_count = 0;
};

using vds::kTraceInputAudio;
using vds::kTraceInputControl;
using vds::kTraceOutput;
using vds::trace_enabled;
using vds::trim_command;

/*
 * A feature GET or SET that cannot be written immediately. Queued rather than
 * written with a blocking poll, because the single-threaded loop cannot afford
 * to wait on one pad's congested control channel.
 */
struct PendingControlWrite {
  std::uint8_t report_id = 0;
  bool is_set = false;
  std::vector<std::uint8_t> payload;
};

struct VirtualPort {
  /* Shown to users in place of a device node: the port has none. */
  std::string path;
  std::unique_ptr<vds::UsbipPort> usb;
  vds::PcmAudioExtractor extractor;
  vds::PcmAudioExtractor waveout_extractor;
  vds::MicAudioDecoder mic_decoder;
  vds::HapticsPacketBuilder haptics_builder;
  vds::DsOutputState output_state;
  std::deque<vds::AudioChunk> pending_audio_chunks;
  std::deque<PendingControlWrite> pending_control_writes;
  std::optional<vds::DsState> last_sent_bt_state;
  std::optional<vds::DsState> pending_bt_state;
  std::optional<vds::BtStateReport> pending_bt_state_report;
  Clock::time_point next_haptics_send_time{};
  bool audio_out_stream_active = false;
  bool audio_in_stream_active = false;
  bool mic_muted = false;
  bool mute_button_down = false;
  bool headset_plugged = false;
  bool headset_mic_plugged = false;
  std::optional<vds::BatteryState> battery;
  bool speaker_waveout_selected = true;
  bool speaker_waveout_active = false;
  std::uint32_t speaker_waveout_phase = 0;
  std::uint32_t test_buzz_frames_left = 0;
  std::uint32_t test_buzz_phase = 0;
  std::array<std::vector<std::uint8_t>, 256> feature_cache;
  std::array<bool, 256> feature_cached;
  std::vector<std::uint8_t> pending_feature_reports;
  TraceState trace_state;
};

struct ControllerRuntime {
  vds::ControllerConfig config;
  std::string device;
  std::optional<std::uint32_t> detected_profile;
  std::optional<vds::BtBackend> backend;
  std::optional<vds::SteamBackend> steam;
  bool virtual_connected = false;
  std::string last_error;
  /* BlueZ was asked to drop the pad so it reconnects hidden. */
  bool bluez_kicked = false;
  /* A Steam Controller that failed stays alone until it switches off. */
  bool steam_failed = false;
};

/* Whether BlueZ and vdsd can both drive a pad on this kernel. */
enum class BluetoothSharing { Unknown, Supported, Unsupported };

enum class EventType : std::uint32_t {
  Control = 1,
  Port = 2,
  BtControl = 3,
  BtInterrupt = 4,
  Udev = 7,
  Signal = 8,
  Usb = 9,
  Sleep = 10,
  SteamInput = 11,
};

struct EventSource {
  EventType type;
  std::size_t index;
};

class SocketPathGuard {
public:
  explicit SocketPathGuard(std::string path) : path_(std::move(path)) {}
  ~SocketPathGuard() { ::unlink(path_.c_str()); }

  SocketPathGuard(const SocketPathGuard &) = delete;
  SocketPathGuard &operator=(const SocketPathGuard &) = delete;

private:
  std::string path_;
};

struct SignalRequests {
  bool stop = false;
  bool log_reopen = false;
};

vds::UniqueFd create_signal_fd() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGHUP);
  /* signalfd only reports signals the process blocks. */
  if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
    throw std::runtime_error("sigprocmask failed: " +
                             std::string(std::strerror(errno)));
  }

  vds::UniqueFd fd(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
  if (!fd) {
    throw std::runtime_error("signalfd failed: " +
                             std::string(std::strerror(errno)));
  }
  return fd;
}

void drain_signal_fd(int fd, SignalRequests &requests) {
  while (true) {
    signalfd_siginfo info{};
    const ssize_t got = ::read(fd, &info, sizeof(info));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      throw std::runtime_error("signalfd read failed: " +
                               std::string(std::strerror(errno)));
    }
    if (got != static_cast<ssize_t>(sizeof(info))) {
      throw std::runtime_error("short signalfd read");
    }
    if (info.ssi_signo == SIGHUP) {
      requests.log_reopen = true;
      continue;
    }
    requests.stop = true;
  }
}

Options parse_platform_args(int argc, char **argv) {
  Options options;
  vds::VdsdCommonOptions common = vds::default_vdsd_common_options();
  const std::string platform_options =
      std::string("[--socket ") + kDefaultControlSocket + "]";
  const auto next_value = [&](int &index, std::string_view option) {
    if (index + 1 >= argc) {
      throw std::runtime_error(std::string(option) + " requires a value");
    }
    return std::string(argv[++index]);
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (vds::parse_vdsd_common_option(argc, argv, i, common)) {
      if (common.help_requested) {
        vds::print_vdsd_usage(std::cerr, vds::kVersion, vds::kBuildYear,
                              platform_options);
        std::exit(0);
      }
    } else if (arg == "--socket") {
      options.socket = next_value(i, arg);
    } else {
      throw std::runtime_error("unknown argument: " + std::string(arg));
    }
  }
  options.db_path = common.db_path;
  options.log_path = common.log_path;
  return options;
}

void trace_output_latency(const std::string &device, std::string_view label,
                          LatencyTraceStats &stats, Clock::duration duration,
                          Clock::duration slow_threshold, vds::Logger &logger) {
  const std::uint64_t elapsed_us = duration_us(duration);
  ++stats.count;
  stats.total_us += elapsed_us;
  stats.max_us = std::max(stats.max_us, elapsed_us);

  if (duration >= slow_threshold) {
    logger.log(vds::LogScope::Output, vds::LogLevel::Warn,
               device + " slow " + std::string(label) +
                   " latency count=" + std::to_string(stats.count) +
                   " latency_us=" + std::to_string(elapsed_us));
  }
  if (stats.count == 1 || stats.count % kOutputTraceSummaryInterval == 0) {
    logger.log(vds::LogScope::Output, vds::LogLevel::Debug,
               device + " " + std::string(label) +
                   " latency summary count=" + std::to_string(stats.count) +
                   " avg_us=" + std::to_string(stats.total_us / stats.count) +
                   " max_us=" + std::to_string(stats.max_us));
  }
}

void trace_hid_out_report(const std::string &device,
                          std::span<const std::uint8_t> payload,
                          TraceState &trace_state, vds::Logger &logger) {
  std::ostringstream line;
  line << device << " hid out";
  if (!payload.empty()) {
    line << " report=" << hex_u8(payload[0]);
  }
  if (payload.size() >= 5) {
    /*
     * USB report 0x02 carries SetStateData after the report ID. These fields
     * show whether a plain rumble request exists before the HD haptics audio
     * path is involved.
     */
    line << " flags0=" << hex_u8(payload[1]) << " flags1=" << hex_u8(payload[2])
         << " rumble_r=" << static_cast<unsigned>(payload[3])
         << " rumble_l=" << static_cast<unsigned>(payload[4]);
  }
  if (payload.size() >= 40) {
    line << " light_flags=" << hex_u8(payload[39]);
  }
  logger.log(vds::LogScope::Hid, vds::LogLevel::Debug, line.str());

  const bool hid_out_changed =
      trace_state.last_hid_out.size() != payload.size() ||
      !std::equal(trace_state.last_hid_out.begin(),
                  trace_state.last_hid_out.end(), payload.begin());
  if (!hid_out_changed) {
    return;
  }

  trace_state.last_hid_out.assign(payload.begin(), payload.end());
  std::ostringstream changed;
  changed << device
          << " hid out changed raw=" << hex_bytes(payload, kTraceDumpMaxBytes);
  if (payload.size() >= 33 && payload[0] == VDS_USB_OUTPUT_REPORT_ID) {
    /*
     * SetStateData begins after the report ID. Offsets 10..20 and 21..31 are
     * the right/left adaptive trigger blocks from the DualSense USB report.
     */
    changed << " right_trigger=" << hex_bytes(payload.subspan(11, 11), 11)
            << " left_trigger=" << hex_bytes(payload.subspan(22, 11), 11);
  }
  logger.log(vds::LogScope::Hid, vds::LogLevel::Debug, changed.str());
}

const char *usb_interface_type_name(std::uint8_t type) {
  switch (type) {
  case VDS_USB_INTERFACE_HID:
    return "hid";
  case VDS_USB_INTERFACE_AUDIO_OUT:
    return "audio_out";
  case VDS_USB_INTERFACE_AUDIO_IN:
    return "audio_in";
  default:
    return "unknown";
  }
}

const char *profile_name(std::uint32_t profile) {
  if (profile == VDS_PROFILE_DSE) {
    return "dualsense-edge";
  }
  return "dualsense";
}

std::uint32_t detect_bluetooth_profile(const std::string &address) {
  const auto modalias = vds::bluez_device_modalias(address);
  if (!modalias) {
    throw std::runtime_error("failed to query BlueZ modalias for " + address);
  }
  const auto profile = vds::controller_profile_from_modalias(*modalias);
  if (!profile) {
    throw std::runtime_error("failed to detect Bluetooth device ID for " +
                             address);
  }
  return vds::usb_profile_from_controller_profile(*profile);
}

std::uint32_t resolve_config_profile(const vds::ControllerConfig &controller) {
  switch (controller.profile) {
  case vds::ControllerProfile::Unspecified:
    return detect_bluetooth_profile(controller.address);
  case vds::ControllerProfile::Ds5:
  case vds::ControllerProfile::Dse:
    return vds::usb_profile_from_controller_profile(controller.profile);
  }
  throw std::runtime_error("unknown controller profile");
}

int open_control_socket(const std::string &path) {
  vds::UniqueFd fd(
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!fd) {
    throw std::runtime_error("failed to open control socket: " +
                             std::string(std::strerror(errno)));
  }

  ::unlink(path.c_str());
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    throw std::runtime_error("control socket path is too long: " + path);
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

  if (::bind(fd.get(), reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) < 0) {
    const int error = errno;
    throw std::runtime_error("failed to bind control socket " + path + ": " +
                             std::strerror(error));
  }
  // Any local process may connect; control_peer_allowed decides who is served.
  (void)::chmod(path.c_str(), 0666);

  if (::listen(fd.get(), 4) < 0) {
    const int error = errno;
    throw std::runtime_error("failed to listen on control socket " + path +
                             ": " + std::strerror(error));
  }
  return fd.release();
}

bool write_vds_frame(VirtualPort &port, std::span<const std::uint8_t> bytes,
                     bool trace, vds::Logger &logger) {
  while (true) {
    const ssize_t written =
        ::write(port.usb->frame_fd(), bytes.data(), bytes.size());
    if (written == static_cast<ssize_t>(bytes.size())) {
      return true;
    }
    if (written >= 0) {
      throw PortIoError("short " + port.path + " frame write");
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOSPC) {
      ++port.trace_state.dropped_usb_frame_count;
      if (trace) {
        if (port.trace_state.dropped_usb_frame_count == 1 ||
            port.trace_state.dropped_usb_frame_count % 1000 == 0) {
          logger.log(
              vds::LogScope::Port, vds::LogLevel::Warn,
              port.path + " vDS write queue full count=" +
                  std::to_string(port.trace_state.dropped_usb_frame_count));
        }
      }
      return false;
    }
    throw PortIoError("write failed: " + std::string(std::strerror(errno)));
  }
}

bool write_input_report(VirtualPort &port, const vds::UsbInputReport &report,
                        bool trace, vds::Logger &logger) {
  vds_frame_header header{};
  header.type = VDS_FRAME_USB_HID_IN;
  header.length = static_cast<std::uint32_t>(report.size());

  std::array<std::uint8_t, sizeof(header) + vds::kUsbInputReportSize> frame{};
  std::memcpy(frame.data(), &header, sizeof(header));
  std::memcpy(frame.data() + sizeof(header), report.data(), report.size());
  return write_vds_frame(port, frame, trace, logger);
}

/*
 * hid-playstation refuses a second controller with the address of one it
 * already drives, and it drives the physical pad while BlueZ shares it. The
 * virtual pad then reports the locally administered form of that address.
 */
std::vector<std::uint8_t>
usb_feature_report(std::span<const std::uint8_t> report) {
  std::vector<std::uint8_t> usb_report(report.begin(), report.end());
  if (usb_report.size() > kPairingInfoAddressMsbOffset &&
      usb_report[0] == kPairingInfoReportId) {
    usb_report[kPairingInfoAddressMsbOffset] |= 0x02;
  }
  return usb_report;
}

void cache_feature_report(VirtualPort &port,
                          std::span<const std::uint8_t> report, bool trace,
                          vds::Logger &logger) {
  if (report.empty()) {
    return;
  }

  const std::uint8_t report_id = report[0];
  port.feature_cache[report_id] = usb_feature_report(report);
  port.feature_cached[report_id] = true;

  const auto frame = vds::frame_bytes(VDS_FRAME_USB_FEATURE_REPLY,
                                      port.feature_cache[report_id]);
  (void)write_vds_frame(port, frame, trace, logger);
}

bool try_send_control_write(vds::BtBackend &bt_backend,
                            const PendingControlWrite &write) {
  if (write.is_set) {
    return bt_backend.try_send_feature_set(write.payload);
  }
  bt_backend.send_feature_get(write.report_id);
  return true;
}

bool queue_control_write(VirtualPort &port, PendingControlWrite write,
                         vds::BtBackend &bt_backend,
                         vds::Logger &logger) {
  if (port.pending_control_writes.empty() &&
      try_send_control_write(bt_backend, write)) {
    return true;
  }
  /*
   * Only the last entry is eligible, so a SET can never move across a queued
   * GET for the same report and change what the host reads back.
   */
  if (write.is_set && !port.pending_control_writes.empty() &&
      port.pending_control_writes.back().is_set &&
      port.pending_control_writes.back().report_id == write.report_id) {
    ++port.trace_state.coalesced_control_write_count;
    port.pending_control_writes.back() = std::move(write);
    return true;
  }
  if (port.pending_control_writes.size() >= kMaxPendingControlWrites) {
    /*
     * A refused GET is recoverable: the kernel URB times out and usbhid
     * reissues GET_REPORT. An evicted SET is not, and could leave the pad in
     * a state neither the old nor the new configuration describes.
     */
    ++port.trace_state.dropped_control_write_count;
    if (port.trace_state.dropped_control_write_count == 1 ||
        port.trace_state.dropped_control_write_count % 1000 == 0) {
      logger.log(
          vds::LogScope::Hid, vds::LogLevel::Warn,
          port.path + " dropped queued BT feature write count=" +
              std::to_string(port.trace_state.dropped_control_write_count) +
              " coalesced=" +
              std::to_string(port.trace_state.coalesced_control_write_count));
    }
    return false;
  }
  port.pending_control_writes.push_back(std::move(write));
  return true;
}

bool flush_pending_control_writes(VirtualPort &port,
                                  vds::BtBackend &bt_backend) {
  while (!port.pending_control_writes.empty()) {
    if (!try_send_control_write(bt_backend,
                                port.pending_control_writes.front())) {
      return false;
    }
    port.pending_control_writes.pop_front();
  }
  return true;
}

bool flush_pending_bt_state_report(VirtualPort &port,
                                   vds::BtBackend &bt_backend,
                                   std::uint32_t trace_flags,
                                   vds::Logger &logger) {
  if (!port.pending_bt_state_report || !port.pending_bt_state) {
    return true;
  }

  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  if (bt_backend.try_send_output_report(*port.pending_bt_state_report)) {
    port.last_sent_bt_state = *port.pending_bt_state;
    port.pending_bt_state.reset();
    port.pending_bt_state_report.reset();
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " flushed pending BT 0x31 state report");
    }
    return true;
  }

  ++port.trace_state.blocked_bt_state_count;
  if (output_trace && (port.trace_state.blocked_bt_state_count == 1 ||
                       port.trace_state.blocked_bt_state_count % 1000 == 0)) {
    logger.log(vds::LogScope::Hid, vds::LogLevel::Warn,
               port.path +
                   " pending BT 0x31 state report still blocked "
                   "count=" +
                   std::to_string(port.trace_state.blocked_bt_state_count));
  }
  return false;
}

void forward_bt_state_if_changed(VirtualPort &port,
                                 vds::BtBackend &bt_backend,
                                 std::uint32_t trace_flags, vds::Logger &logger,
                                 std::string_view reason) {
  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);

  (void)flush_pending_bt_state_report(port, bt_backend, trace_flags, logger);

  const vds::DsState state = port.output_state.state();
  if (port.last_sent_bt_state && *port.last_sent_bt_state == state) {
    if (port.pending_bt_state) {
      port.pending_bt_state.reset();
      port.pending_bt_state_report.reset();
      ++port.trace_state.coalesced_bt_state_count;
    }
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " " + std::string(reason) +
                     " skipped: BT state unchanged");
    }
    return;
  }
  if (port.pending_bt_state && *port.pending_bt_state == state) {
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " " + std::string(reason) +
                     " skipped: BT state already pending");
    }
    return;
  }

  const auto packet = port.output_state.build_bt_state_report();
  if (bt_backend.try_send_output_report(packet)) {
    port.last_sent_bt_state = state;
    port.pending_bt_state.reset();
    port.pending_bt_state_report.reset();
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " " + std::string(reason) +
                     " forwarded as BT 0x31 state report");
    }
    return;
  }

  if (port.pending_bt_state) {
    ++port.trace_state.coalesced_bt_state_count;
  }
  ++port.trace_state.deferred_bt_state_count;
  port.pending_bt_state = state;
  port.pending_bt_state_report = packet;
  if (output_trace && (port.trace_state.deferred_bt_state_count == 1 ||
                       port.trace_state.deferred_bt_state_count % 1000 == 0)) {
    logger.log(vds::LogScope::Hid, vds::LogLevel::Warn,
               port.path + " deferred BT 0x31 state report count=" +
                   std::to_string(port.trace_state.deferred_bt_state_count) +
                   " coalesced=" +
                   std::to_string(port.trace_state.coalesced_bt_state_count));
  }
}

void reset_virtual_port(VirtualPort &port) {
  port.extractor = vds::PcmAudioExtractor{};
  port.waveout_extractor = vds::PcmAudioExtractor{};
  port.mic_decoder = vds::MicAudioDecoder{};
  port.haptics_builder = vds::HapticsPacketBuilder{};
  port.output_state = vds::DsOutputState{};
  port.pending_audio_chunks.clear();
  port.pending_control_writes.clear();
  port.last_sent_bt_state.reset();
  port.pending_bt_state.reset();
  port.pending_bt_state_report.reset();
  port.next_haptics_send_time = {};
  port.audio_out_stream_active = false;
  port.audio_in_stream_active = false;
  port.mic_muted = false;
  port.mute_button_down = false;
  port.headset_plugged = false;
  port.headset_mic_plugged = false;
  port.battery.reset();
  port.speaker_waveout_selected = true;
  port.speaker_waveout_active = false;
  port.speaker_waveout_phase = 0;
  port.test_buzz_frames_left = 0;
  port.feature_cache = {};
  port.feature_cached = {};
  port.pending_feature_reports.clear();
  port.trace_state = TraceState{};
}

void disconnect_virtual_port(VirtualPort &port, vds::Logger &logger) {
  port.pending_audio_chunks.clear();
  port.pending_control_writes.clear();
  port.next_haptics_send_time = {};
  port.audio_out_stream_active = false;
  port.audio_in_stream_active = false;
  port.mic_muted = false;
  port.mute_button_down = false;
  port.speaker_waveout_active = false;
  port.speaker_waveout_phase = 0;
  port.test_buzz_frames_left = 0;
  try {
    port.usb->disconnect();
    logger.log(vds::LogScope::Usb, vds::LogLevel::Info,
               port.path + " virtual USB disconnected");
  } catch (const std::exception &error) {
    logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
               port.path + " disconnect failed: " + error.what());
  }
}

void log_audio_encode_failure(VirtualPort &port,
                              const std::runtime_error &error,
                              vds::Logger &logger) {
  ++port.trace_state.audio_encode_fail_count;
  if (port.trace_state.audio_encode_fail_count == 1 ||
      port.trace_state.audio_encode_fail_count % 1000 == 0) {
    logger.log(vds::LogScope::Output, vds::LogLevel::Warn,
               port.path + " audio encode failed count=" +
                   std::to_string(port.trace_state.audio_encode_fail_count) +
                   ": " + error.what());
  }
}

void handle_frame(const vds_frame_header &header,
                  std::span<const std::uint8_t> payload,
                  vds::BtBackend *bt_backend, std::uint32_t trace_flags,
                  VirtualPort &port, vds::Logger &logger) {
  if (header.type == VDS_FRAME_USB_INTERFACE) {
    if (payload.size() != sizeof(vds_usb_interface_event)) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
                 port.path + " malformed USB interface event len=" +
                     std::to_string(payload.size()));
      return;
    }

    vds_usb_interface_event event{};
    std::memcpy(&event, payload.data(), sizeof(event));

    std::ostringstream line;
    line << port.path << " USB set_interface type="
         << usb_interface_type_name(event.interface_type)
         << " number=" << static_cast<unsigned>(event.interface_number)
         << " alt=" << static_cast<unsigned>(event.altsetting);
    if (event.interface_type == VDS_USB_INTERFACE_AUDIO_OUT ||
        event.interface_type == VDS_USB_INTERFACE_AUDIO_IN) {
      line << " stream=" << (event.altsetting != 0 ? "on" : "off");
    }
    logger.log(vds::LogScope::Usb, vds::LogLevel::Info, line.str());
    if (event.interface_type == VDS_USB_INTERFACE_AUDIO_OUT) {
      port.audio_out_stream_active = event.altsetting != 0;
      if (!port.audio_out_stream_active) {
        port.pending_audio_chunks.clear();
        port.extractor = vds::PcmAudioExtractor{};
      } else {
        port.output_state.set_audio_out_stream_active(true,
                                                      port.headset_plugged);
        if (bt_backend) {
          forward_bt_state_if_changed(port, *bt_backend, trace_flags, logger,
                                      "audio out interface");
        }
      }
    } else if (event.interface_type == VDS_USB_INTERFACE_AUDIO_IN) {
      port.audio_in_stream_active = event.altsetting != 0;
      port.mic_decoder = vds::MicAudioDecoder{};
      if (bt_backend) {
        const auto state_report = port.output_state.build_bt_mic_state_report(
            port.audio_in_stream_active, port.mic_muted);
        bt_backend->try_send_output_report(state_report);
        const auto report =
            port.output_state.build_bt_mic_report(port.audio_in_stream_active);
        const bool sent = bt_backend->try_send_output_report(report);
        if (trace_enabled(trace_flags, kTraceInputAudio)) {
          logger.log(
              vds::LogScope::InputAudio, vds::LogLevel::Info,
              port.path + " mic " +
                  std::string(port.audio_in_stream_active ? "open" : "close") +
                  " sent=" + (sent ? "yes" : "no"));
        }
      }
    }
    return;
  }

  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  if (output_trace) {
    std::ostringstream line;
    line << port.path << " frame " << vds::frame_type_name(header.type)
         << " len=" << header.length << " seq=" << header.sequence;

    bool emit_trace = true;
    if (header.type == VDS_FRAME_USB_AUDIO_OUT &&
        payload.size() >= VDS_AUDIO_CHANNELS * sizeof(std::int16_t)) {
      std::array<int, VDS_AUDIO_CHANNELS> peaks{};
      const std::size_t frame_size = VDS_AUDIO_CHANNELS * sizeof(std::int16_t);
      const std::size_t frames = payload.size() / frame_size;
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint8_t *base = payload.data() + frame * frame_size;
        for (std::size_t channel = 0; channel < VDS_AUDIO_CHANNELS; ++channel) {
          const auto low =
              static_cast<std::uint16_t>(base[channel * sizeof(std::int16_t)]);
          const auto high = static_cast<std::uint16_t>(
                                base[channel * sizeof(std::int16_t) + 1])
                            << 8;
          const auto sample = static_cast<std::int16_t>(low | high);
          peaks[channel] =
              std::max(peaks[channel], std::abs(static_cast<int>(sample)));
        }
      }

      ++port.trace_state.audio_usb_frame_count;
      const bool haptics_nonzero = peaks[2] != 0 || peaks[3] != 0;
      emit_trace =
          port.trace_state.audio_usb_frame_count == 1 ||
          port.trace_state.audio_usb_frame_count % 250 == 0 ||
          (haptics_nonzero && !port.trace_state.audio_haptics_nonzero_seen);
      port.trace_state.audio_haptics_nonzero_seen |= haptics_nonzero;
      line << " pcm_peak_ch0=" << peaks[0] << " pcm_peak_ch1=" << peaks[1]
           << " pcm_peak_ch2=" << peaks[2] << " pcm_peak_ch3=" << peaks[3];
    }

    if (emit_trace) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Debug, line.str());
    }
  }

  if (header.type == VDS_FRAME_USB_HID_OUT) {
    const auto start = Clock::now();
    if (output_trace) {
      trace_hid_out_report(port.path, payload, port.trace_state, logger);
    }
    if (!port.output_state.apply_usb_output_report(payload)) {
      if (output_trace) {
        logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                   port.path + " hid out ignored: unsupported output report");
      }
      return;
    }
    if (port.audio_out_stream_active || port.speaker_waveout_active) {
      port.output_state.set_audio_out_stream_active(true, port.headset_plugged);
    }
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " hid out accepted for BT state update");
    }

    if (bt_backend) {
      forward_bt_state_if_changed(port, *bt_backend, trace_flags, logger,
                                  "hid out");
    }

    if (output_trace) {
      trace_output_latency(port.path, "hid_out",
                           port.trace_state.output_hid_latency,
                           Clock::now() - start, kOutputTraceSlowWarn, logger);
    }
    return;
  }

  if (header.type == VDS_FRAME_USB_FEATURE_GET) {
    const auto start = Clock::now();
    if (payload.empty()) {
      return;
    }
    const std::uint8_t report_id = payload[0];
    const unsigned requested_length =
        payload.size() >= 3
            ? static_cast<unsigned>(payload[1] |
                                    (static_cast<unsigned>(payload[2]) << 8))
            : (payload.size() >= 2 ? static_cast<unsigned>(payload[1]) : 0);
    const bool cache_hit = port.feature_cached[report_id];
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " feature get report=" + hex_u8(report_id) +
                     " request_len=" + std::to_string(requested_length) +
                     " cache=" + (cache_hit ? "hit" : "miss") + " forwarded=" +
                     ((bt_backend && !cache_hit) ? "yes" : "no"));
    }
    if (cache_hit) {
      const auto frame = vds::frame_bytes(VDS_FRAME_USB_FEATURE_REPLY,
                                          port.feature_cache[report_id]);
      (void)write_vds_frame(port, frame, output_trace, logger);
      if (output_trace) {
        trace_output_latency(
            port.path, "feature_get", port.trace_state.output_feature_latency,
            Clock::now() - start, kOutputTraceFeatureSlowWarn, logger);
      }
      return;
    }
    if (bt_backend) {
      port.pending_feature_reports.push_back(report_id);
      if (!queue_control_write(port,
                               PendingControlWrite{
                                   .report_id = report_id,
                                   .is_set = false,
                                   .payload = {},
                               },
                               *bt_backend, logger)) {
        port.pending_feature_reports.pop_back();
      }
    }
    if (output_trace) {
      trace_output_latency(
          port.path, "feature_get", port.trace_state.output_feature_latency,
          Clock::now() - start, kOutputTraceFeatureSlowWarn, logger);
    }
    return;
  }

  if (header.type == VDS_FRAME_USB_FEATURE_SET) {
    const auto start = Clock::now();
    if (payload.empty()) {
      return;
    }
    const std::uint8_t report_id = payload[0];
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " feature set report=" + hex_u8(report_id) +
                     " len=" + std::to_string(payload.size()) +
                     " forwarded=" + (bt_backend ? "yes" : "no"));
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " feature set payload=" +
                     hex_bytes(payload, kTraceDumpMaxBytes));
    }
    if (payload.size() >= 3 && payload[0] == kTestCommandReportId) {
      std::size_t command_offset = 1;
      if (payload.size() > command_offset &&
          payload[command_offset] == kTestCommandReportId) {
        ++command_offset;
      }
      if (payload.size() > command_offset + 1) {
        const std::uint8_t command_device = payload[command_offset];
        const std::uint8_t command_action = payload[command_offset + 1];
        const std::size_t command_data_offset = command_offset + 2;

        std::array<std::uint8_t, VDS_USB_INPUT_REPORT_SIZE> test_result{};
        test_result[0] = kTestCommandResultReportId;
        test_result[1] = command_device;
        test_result[2] = command_action;
        test_result[3] = kTestCommandCompleteStatus;
        cache_feature_report(port, test_result, output_trace, logger);
        if (output_trace) {
          logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                     port.path + " cached WebHID test result device=" +
                         hex_u8(command_device) + " action=" +
                         hex_u8(command_action) + " status=complete");
        }

        if (command_device == kTestCommandAudioDevice &&
            command_action == kTestCommandWaveoutPrepare) {
          port.speaker_waveout_selected =
              payload.size() > command_data_offset + 2 &&
              payload[command_data_offset + 2] == kTestCommandSpeakerParam;
          if (output_trace) {
            logger.log(vds::LogScope::Output, vds::LogLevel::Debug,
                       port.path + " WebHID waveout target=" +
                           std::string(port.speaker_waveout_selected
                                           ? "speaker"
                                           : "headphone"));
          }
        } else if (command_device == kTestCommandAudioDevice &&
                   command_action == kTestCommandWaveoutControl &&
                   payload.size() > command_data_offset) {
          const bool enable = payload[command_data_offset] != 0;
          const bool speaker_waveout = enable && port.speaker_waveout_selected;
          port.speaker_waveout_active = speaker_waveout;
          port.speaker_waveout_phase = 0;
          port.waveout_extractor = vds::PcmAudioExtractor{};
          port.output_state.set_audio_out_stream_active(speaker_waveout,
                                                        port.headset_plugged);
          port.audio_out_stream_active = speaker_waveout;
          if (!speaker_waveout) {
            port.pending_audio_chunks.clear();
          }
          if (bt_backend) {
            forward_bt_state_if_changed(port, *bt_backend, trace_flags, logger,
                                        speaker_waveout
                                            ? "WebHID speaker waveout route on"
                                            : "WebHID waveout route off");
          }
          if (output_trace) {
            logger.log(vds::LogScope::Output, vds::LogLevel::Info,
                       port.path + " WebHID waveout " +
                           std::string(enable ? "on" : "off") + " target=" +
                           std::string(port.speaker_waveout_selected
                                           ? "speaker"
                                           : "headphone") +
                           " synthesized=" + (speaker_waveout ? "yes" : "no"));
          }
        }
      }
    }
    if (bt_backend) {
      (void)queue_control_write(port,
                                PendingControlWrite{
                                    .report_id = report_id,
                                    .is_set = true,
                                    .payload = std::vector<std::uint8_t>(
                                        payload.begin(), payload.end()),
                                },
                                *bt_backend, logger);
    }
    if (output_trace) {
      trace_output_latency(
          port.path, "feature_set", port.trace_state.output_feature_latency,
          Clock::now() - start, kOutputTraceFeatureSlowWarn, logger);
    }
    return;
  }

  if (header.type != VDS_FRAME_USB_AUDIO_OUT) {
    return;
  }

  if (!bt_backend) {
    return;
  }

  std::size_t queued_chunks = 0;
  std::size_t dropped_chunks = 0;
  const auto audio_start = Clock::now();
  const auto extract_start = Clock::now();
  std::vector<vds::AudioChunk> chunks;
  try {
    chunks = port.extractor.push_usb_audio(payload);
  } catch (const std::runtime_error &error) {
    /* Audio is lossy by design, so a failed encode drops the frame. */
    log_audio_encode_failure(port, error, logger);
    return;
  }
  const auto extract_duration = Clock::now() - extract_start;
  for (const auto &chunk : chunks) {
    if (output_trace) {
      if (chunk.has_haptics_signal) {
        if (!port.trace_state.haptics_burst_active) {
          logger.log(vds::LogScope::Output, vds::LogLevel::Debug,
                     port.path + " haptics burst start");
          port.trace_state.haptics_burst_active = true;
          port.trace_state.haptics_burst_chunks = 0;
        }
        ++port.trace_state.haptics_burst_chunks;
      } else if (port.trace_state.haptics_burst_active) {
        logger.log(vds::LogScope::Output, vds::LogLevel::Debug,
                   port.path + " haptics burst end chunks=" +
                       std::to_string(port.trace_state.haptics_burst_chunks));
        port.trace_state.haptics_burst_active = false;
        port.trace_state.haptics_burst_chunks = 0;
      }
    }

    /*
     * USB isochronous URBs can arrive in bursts. Queue completed 0x36 audio
     * chunks here and let flush_pending_outputs() send them at the 10 ms
     * speaker frame interval, otherwise speaker/haptics audio turns into
     * audible bursts.
     */
    if (port.pending_audio_chunks.size() >= kMaxPendingAudioChunks) {
      port.pending_audio_chunks.pop_front();
      ++dropped_chunks;
      ++port.trace_state.dropped_audio_haptics_count;
      ++port.trace_state.queue_dropped_audio_haptics_count;
    }

    port.pending_audio_chunks.push_back(chunk);
    ++queued_chunks;
  }
  if (dropped_chunks > 0 &&
      (port.trace_state.dropped_audio_haptics_count == dropped_chunks ||
       port.trace_state.dropped_audio_haptics_count % 1000 == 0)) {
    logger.log(
        vds::LogScope::Output, vds::LogLevel::Warn,
        port.path + " dropped BT 0x36 haptics packets count=" +
            std::to_string(port.trace_state.dropped_audio_haptics_count) +
            " queue=" +
            std::to_string(port.trace_state.queue_dropped_audio_haptics_count) +
            " blocked=" +
            std::to_string(port.trace_state.blocked_audio_haptics_count));
  }
  if (output_trace && queued_chunks > 0) {
    logger.log(
        vds::LogScope::Output, vds::LogLevel::Debug,
        port.path + " audio out queued " + std::to_string(queued_chunks) +
            " BT 0x36 haptics packets pending=" +
            std::to_string(port.pending_audio_chunks.size()) +
            " extract_encode_us=" +
            std::to_string(duration_us(extract_duration)) + " total_us=" +
            std::to_string(duration_us(Clock::now() - audio_start)));
  }
  if (output_trace) {
    trace_output_latency(
        port.path, "audio_out", port.trace_state.output_audio_latency,
        Clock::now() - audio_start, kOutputTraceSlowWarn, logger);
    trace_output_latency(port.path, "audio_extract_encode",
                         port.trace_state.output_audio_extract_latency,
                         extract_duration, kOutputTraceSlowWarn, logger);
  }
}

/*
 * Behind a Steam Controller no DualSense answers, so nothing is forwarded:
 * the feature reports are made up when it connects, and one the host asks
 * for beyond those is answered with zeros. Left unanswered it would stall
 * the host for the five seconds the virtual port waits.
 */
void handle_steam_frame(const vds_frame_header &header,
                        std::span<const std::uint8_t> payload,
                        vds::SteamBackend &steam, VirtualPort &port,
                        vds::Logger &logger) {
  const auto now = Clock::now();
  if (header.type == VDS_FRAME_USB_INTERFACE) {
    vds_usb_interface_event event{};
    if (payload.size() != sizeof(event)) {
      return;
    }
    std::memcpy(&event, payload.data(), sizeof(event));
    if (event.interface_type == VDS_USB_INTERFACE_AUDIO_OUT) {
      port.audio_out_stream_active = event.altsetting != 0;
      logger.log(vds::LogScope::Usb, vds::LogLevel::Info,
                 port.path + " audio out stream=" +
                     (port.audio_out_stream_active ? "on" : "off"));
    }
  } else if (header.type == VDS_FRAME_USB_HID_OUT) {
    steam.apply_usb_output_report(payload, now);
  } else if (header.type == VDS_FRAME_USB_FEATURE_GET) {
    if (payload.empty()) {
      return;
    }
    const std::uint8_t report_id = payload[0];
    if (port.feature_cached[report_id]) {
      const auto frame = vds::frame_bytes(VDS_FRAME_USB_FEATURE_REPLY,
                                          port.feature_cache[report_id]);
      (void)write_vds_frame(port, frame, false, logger);
      return;
    }
    const std::size_t asked =
        payload.size() >= 3
            ? payload[1] | (static_cast<std::size_t>(payload[2]) << 8)
            : vds::kUsbInputReportSize;
    std::vector<std::uint8_t> blank(
        std::clamp<std::size_t>(asked, 1, vds::kUsbInputReportSize), 0);
    blank[0] = report_id;
    logger.log(vds::LogScope::Hid, vds::LogLevel::Warn,
               port.path + " host asked for feature report " +
                   hex_u8(report_id) +
                   ", which a Steam Controller has no answer to; replied "
                   "with zeros");
    cache_feature_report(port, blank, false, logger);
  } else if (header.type == VDS_FRAME_USB_AUDIO_OUT) {
    steam.push_usb_audio(payload, now);
  }
}

bool handle_steam_input(VirtualPort &port, vds::SteamBackend &steam,
                        vds::Logger &logger) {
  const auto report = steam.read_input();
  if (!report) {
    return false;
  }
  port.battery = steam.battery();
  (void)write_input_report(port, *report, false, logger);
  return true;
}

bool handle_vds_frame(VirtualPort &port, vds::BtBackend *bt_backend,
                      vds::SteamBackend *steam, std::uint32_t trace_flags,
                      vds::Logger &logger) {
  std::array<std::uint8_t, sizeof(vds_frame_header) + VDS_FRAME_MAX_PAYLOAD>
      frame;
  const ssize_t got = ::read(port.usb->frame_fd(), frame.data(), frame.size());
  if (got == 0) {
    throw PortIoError(port.path + " closed");
  }
  if (got < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      return false;
    }
    throw PortIoError(port.path +
                      " read failed: " + std::string(std::strerror(errno)));
  }
  if (static_cast<std::size_t>(got) < sizeof(vds_frame_header)) {
    throw PortIoError(port.path + " short VDS frame header");
  }
  vds_frame_header header{};
  std::memcpy(&header, frame.data(), sizeof(header));
  if (header.length > VDS_FRAME_MAX_PAYLOAD) {
    throw PortIoError(port.path + " oversized VDS frame");
  }
  if (static_cast<std::size_t>(got) != sizeof(header) + header.length) {
    throw PortIoError(port.path + " short VDS frame payload");
  }
  const std::span payload(frame.data() + sizeof(header), header.length);
  if (steam) {
    handle_steam_frame(header, payload, *steam, port, logger);
  } else {
    handle_frame(header, payload, bt_backend, trace_flags, port, logger);
  }
  return true;
}

bool handle_bt_input(VirtualPort &port, vds::BtBackend &bt_backend,
                     std::uint32_t trace_flags, vds::Logger &logger) {
  const auto read_time = Clock::now();
  const auto packet = bt_backend.read_interrupt_packet();
  if (!packet) {
    return false;
  }

  if (vds::bt_input_payload_type(*packet) == vds::BtInputPayloadType::Audio) {
    const bool input_audio_trace = trace_enabled(trace_flags, kTraceInputAudio);
    ++port.trace_state.bt_mic_packet_count;
    if (!port.audio_in_stream_active) {
      ++port.trace_state.bt_mic_drop_count;
      if (input_audio_trace &&
          (port.trace_state.bt_mic_drop_count == 1 ||
           port.trace_state.bt_mic_drop_count % 1000 == 0)) {
        logger.log(vds::LogScope::InputAudio, vds::LogLevel::Debug,
                   port.path + " dropped mic packet count=" +
                       std::to_string(port.trace_state.bt_mic_drop_count) +
                       " reason=audio_in_stream_off");
      }
      return true;
    }

    const auto opus_payload = vds::bt_mic_opus_payload(*packet);
    if (!opus_payload) {
      ++port.trace_state.bt_mic_decode_fail_count;
      if (input_audio_trace) {
        logger.log(vds::LogScope::InputAudio, vds::LogLevel::Warn,
                   port.path + " malformed mic packet len=" +
                       std::to_string(packet->size()));
      }
      return true;
    }

    try {
      auto pcm = port.mic_decoder.decode(*opus_payload);
      const auto frame = vds::frame_bytes(VDS_FRAME_USB_AUDIO_IN, pcm);
      const bool wrote =
          write_vds_frame(port, frame, input_audio_trace, logger);
      ++port.trace_state.audio_in_forward_count;
      if (input_audio_trace && (port.trace_state.audio_in_forward_count == 1 ||
                                port.trace_state.audio_in_forward_count %
                                        kInputTraceSummaryInterval ==
                                    0)) {
        logger.log(vds::LogScope::InputAudio, vds::LogLevel::Debug,
                   port.path + " mic forwarded count=" +
                       std::to_string(port.trace_state.audio_in_forward_count) +
                       " len=" + std::to_string(pcm.size()) +
                       " written=" + (wrote ? "yes" : "no"));
      }
    } catch (const std::exception &error) {
      ++port.trace_state.bt_mic_decode_fail_count;
      if (input_audio_trace) {
        logger.log(
            vds::LogScope::InputAudio, vds::LogLevel::Warn,
            port.path + " mic decode failed count=" +
                std::to_string(port.trace_state.bt_mic_decode_fail_count) +
                ": " + error.what());
      }
    }
    return true;
  }

  const auto report = vds::bt_input_to_usb_input(*packet);
  if (!report) {
    return true;
  }

  const bool input_trace = trace_enabled(trace_flags, kTraceInputControl);
  if (input_trace) {
    ++port.trace_state.bt_input_count;
    if (port.trace_state.have_last_input_time) {
      const auto gap = read_time - port.trace_state.last_input_time;
      const auto gap_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(gap).count());
      port.trace_state.input_gap_max_us =
          std::max(port.trace_state.input_gap_max_us, gap_us);
      if (gap >= kInputTraceGapWarn) {
        logger.log(vds::LogScope::InputControl, vds::LogLevel::Warn,
                   port.path + " input gap count=" +
                       std::to_string(port.trace_state.bt_input_count) +
                       " gap_us=" + std::to_string(gap_us));
      }
    }
    port.trace_state.last_input_time = read_time;
    port.trace_state.have_last_input_time = true;

    const std::span buttons(report->data() + kUsbInputButtonsOffset,
                            kUsbInputButtonsSize);
    const bool buttons_changed =
        !port.trace_state.have_last_input_buttons ||
        !std::equal(buttons.begin(), buttons.end(),
                    port.trace_state.last_input_buttons.begin());
    if (buttons_changed) {
      std::copy(buttons.begin(), buttons.end(),
                port.trace_state.last_input_buttons.begin());
      port.trace_state.have_last_input_buttons = true;
      logger.log(vds::LogScope::InputControl, vds::LogLevel::Debug,
                 port.path + " buttons changed raw=" +
                     hex_bytes(buttons, kUsbInputButtonsSize));
    }
  }

  port.battery = vds::usb_input_battery(*report);
  const std::uint8_t headset_status = report->at(kUsbInputHeadsetOffset);
  const bool headset_plugged =
      (headset_status & kUsbInputHeadphonesPluggedMask) != 0;
  const bool headset_mic_plugged =
      (headset_status & kUsbInputMicPluggedMask) != 0;
  const bool mute_button_down =
      (report->at(kUsbInputMuteButtonOffset) & kUsbInputMuteButtonMask) != 0;
  bool headset_mic_changed = false;
  bool mic_mute_changed = false;
  bool output_state_changed = false;
  if (mute_button_down && !port.mute_button_down) {
    port.mic_muted = !port.mic_muted;
    mic_mute_changed = true;
  }
  port.mute_button_down = mute_button_down;
  if (headset_mic_plugged != port.headset_mic_plugged) {
    port.headset_mic_plugged = headset_mic_plugged;
    headset_mic_changed = true;
  }
  if (mic_mute_changed ||
      (headset_mic_changed && port.audio_in_stream_active)) {
    const auto mic_state_report = port.output_state.build_bt_mic_state_report(
        port.audio_in_stream_active, port.mic_muted);
    bt_backend.try_send_output_report(mic_state_report);
    if (input_trace) {
      logger.log(vds::LogScope::InputControl, vds::LogLevel::Info,
                 port.path + " mic " +
                     std::string(port.mic_muted ? "muted" : "unmuted"));
    }
  }
  if (headset_plugged != port.headset_plugged) {
    port.headset_plugged = headset_plugged;
    if (port.audio_out_stream_active || port.speaker_waveout_active) {
      port.output_state.set_audio_out_stream_active(true, port.headset_plugged);
      output_state_changed = true;
    }
    if (input_trace) {
      logger.log(
          vds::LogScope::InputControl, vds::LogLevel::Info,
          port.path + " headset " +
              std::string(port.headset_plugged ? "plugged" : "unplugged"));
    }
  }
  if (output_state_changed) {
    forward_bt_state_if_changed(port, bt_backend, trace_flags, logger,
                                "headset route change");
  }
  if (headset_mic_changed && input_trace) {
    logger.log(
        vds::LogScope::InputControl, vds::LogLevel::Info,
        port.path + " headset mic " +
            std::string(port.headset_mic_plugged ? "plugged" : "unplugged"));
  }

  const auto write_start = Clock::now();
  const bool wrote = write_input_report(port, *report, input_trace, logger);
  const auto write_duration = Clock::now() - write_start;
  if (input_trace) {
    const auto write_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(write_duration)
            .count());
    ++port.trace_state.input_write_count;
    port.trace_state.input_write_total_us += write_us;
    port.trace_state.input_write_max_us =
        std::max(port.trace_state.input_write_max_us, write_us);

    if (write_duration >= kInputTraceSlowWriteWarn) {
      logger.log(vds::LogScope::InputControl, vds::LogLevel::Warn,
                 port.path + " slow input write count=" +
                     std::to_string(port.trace_state.bt_input_count) +
                     " write_us=" + std::to_string(write_us) +
                     " written=" + (wrote ? "yes" : "no"));
    }
    if (port.trace_state.bt_input_count == 1 ||
        port.trace_state.bt_input_count % kInputTraceSummaryInterval == 0) {
      const std::uint64_t avg_write_us =
          port.trace_state.input_write_count == 0
              ? 0
              : port.trace_state.input_write_total_us /
                    port.trace_state.input_write_count;
      logger.log(vds::LogScope::InputControl, vds::LogLevel::Debug,
                 port.path + " input summary count=" +
                     std::to_string(port.trace_state.bt_input_count) +
                     " len=" + std::to_string(report->size()) +
                     " avg_write_us=" + std::to_string(avg_write_us) +
                     " max_write_us=" +
                     std::to_string(port.trace_state.input_write_max_us) +
                     " max_gap_us=" +
                     std::to_string(port.trace_state.input_gap_max_us));
    }
  }
  return true;
}

bool handle_bt_control(VirtualPort &port, vds::BtBackend &bt_backend,
                       std::uint32_t trace_flags, vds::Logger &logger) {
  const auto report = bt_backend.read_feature_report();
  if (!report) {
    return false;
  }
  if (report->empty()) {
    return true;
  }

  const std::uint8_t report_id = (*report)[0];
  port.feature_cache[report_id] = usb_feature_report(*report);
  port.feature_cached[report_id] = true;

  const auto pending = std::find(port.pending_feature_reports.begin(),
                                 port.pending_feature_reports.end(), report_id);
  const bool usb_waiting = pending != port.pending_feature_reports.end();
  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  if (output_trace) {
    logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
               port.path + " bt feature cached report=" + hex_u8(report_id) +
                   " len=" + std::to_string(report->size()) +
                   " usb_waiting=" + (usb_waiting ? "yes" : "no"));
  }
  if (!usb_waiting) {
    return true;
  }

  port.pending_feature_reports.erase(pending);
  const auto frame = vds::frame_bytes(VDS_FRAME_USB_FEATURE_REPLY,
                                      port.feature_cache[report_id]);
  (void)write_vds_frame(port, frame, output_trace, logger);
  return true;
}

void initialize_bt_controller(vds::BtBackend &bt_backend,
                              VirtualPort &port, vds::Logger &logger) {
  /*
   * One budget for the whole handshake. A per-poll timeout restarts on every
   * packet, so a chatty pad could hold the single-threaded loop indefinitely.
   */
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(kInitialFeatureReportPollMs);
  for (const std::uint8_t report_id : kInitialFeatureReportIds) {
    bt_backend.send_feature_get(report_id);

    while (true) {
      const auto remaining = deadline - Clock::now();
      const int wait_ms =
          remaining <= Clock::duration::zero()
              ? 0
              : static_cast<int>(
                    std::chrono::ceil<std::chrono::milliseconds>(remaining)
                        .count());
      pollfd pfd{.fd = bt_backend.feature_reply_fd(),
                 .events = POLLIN,
                 .revents = 0};
      const int ready = ::poll(&pfd, 1, wait_ms);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        throw std::runtime_error("timed out waiting for initial Bluetooth "
                                 "feature report " +
                                 hex_u8(report_id) +
                                 "; the controller dropped during setup, "
                                 "press the PS button to retry");
      }
      if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error("Bluetooth control channel closed during "
                                 "initial feature report fetch");
      }
      if ((pfd.revents & POLLIN) == 0) {
        continue;
      }

      const auto report = bt_backend.read_feature_report();
      if (!report || report->empty()) {
        continue;
      }

      cache_feature_report(port, *report, false, logger);
      if ((*report)[0] == report_id) {
        break;
      }
    }
  }

  logger.log(vds::LogScope::Hid, vds::LogLevel::Info,
             port.path + " primed initial Bluetooth feature cache");
}

std::optional<std::size_t> find_port_index(std::span<const VirtualPort> ports,
                                           const std::string &path) {
  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (ports[i].path == path) {
      return i;
    }
  }
  return std::nullopt;
}

std::vector<unsigned> present_port_indices(std::span<const VirtualPort> ports) {
  std::vector<unsigned> indices;
  indices.reserve(ports.size());
  for (const auto &port : ports) {
    if (const auto index = vds::port_index_from_name(port.path)) {
      indices.push_back(*index);
    }
  }
  return indices;
}

bool controller_uses_port(const ControllerRuntime &controller) {
  return controller.backend || controller.steam ||
         controller.virtual_connected;
}

bool port_used_by_other_controller(
    std::span<const ControllerRuntime> controllers,
    const ControllerRuntime &self, const std::string &device) {
  for (const auto &controller : controllers) {
    if (&controller == &self) {
      continue;
    }
    if (controller.device == device && controller_uses_port(controller)) {
      return true;
    }
  }
  return false;
}

std::optional<std::size_t>
available_port_index(std::span<const VirtualPort> ports,
                     std::span<const ControllerRuntime> controllers,
                     const ControllerRuntime &controller) {
  const std::vector<unsigned> candidate_ports = present_port_indices(ports);
  std::vector<unsigned> occupied_ports;
  occupied_ports.reserve(controllers.size());
  for (const auto &other : controllers) {
    if (&other == &controller || !controller_uses_port(other)) {
      continue;
    }
    if (const auto port = vds::port_index_from_name(other.device)) {
      occupied_ports.push_back(*port);
    }
  }

  const auto selected_port = vds::select_controller_config_port(
      controller.config, candidate_ports, occupied_ports);
  if (!selected_port) {
    return std::nullopt;
  }

  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (vds::port_index_from_name(ports[i].path) == selected_port) {
      return i;
    }
  }
  return std::nullopt;
}

bool controller_has_present_allowed_port(std::span<const VirtualPort> ports,
                                         const vds::ControllerConfig &config) {
  return vds::controller_config_has_candidate_port(config,
                                                   present_port_indices(ports));
}

ControllerRuntime *
controller_for_port(std::vector<ControllerRuntime> &controllers,
                    const std::string &device) {
  for (auto &controller : controllers) {
    if (controller.device == device && controller_uses_port(controller)) {
      return &controller;
    }
  }
  return nullptr;
}

/*
 * The port the controller had while that is still free, else the next one
 * its configuration allows.
 */
std::optional<std::size_t>
claim_port_index(std::span<const VirtualPort> ports,
                 std::span<const ControllerRuntime> controllers,
                 ControllerRuntime &controller) {
  std::optional<std::size_t> port_index;
  if (!controller.device.empty()) {
    port_index = find_port_index(ports, controller.device);
    if (port_index && port_used_by_other_controller(controllers, controller,
                                                    controller.device)) {
      port_index.reset();
    }
  }
  if (!port_index) {
    port_index = available_port_index(ports, controllers, controller);
    if (!port_index) {
      controller.last_error = "no available virtual port";
      return std::nullopt;
    }
    controller.device = ports[*port_index].path;
  }
  return port_index;
}

void drop_backend(ControllerRuntime &controller, VirtualPort &port,
                  const std::string &reason, vds::Logger &logger) {
  if (!controller_uses_port(controller)) {
    return;
  }

  port.pending_bt_state.reset();
  port.pending_bt_state_report.reset();
  logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
             "backend disconnected address=" + controller.config.address +
                 " device=" + controller.device + " reason=" + reason);
  controller.backend.reset();
  controller.steam.reset();
  controller.virtual_connected = false;
  controller.device.clear();
  disconnect_virtual_port(port, logger);
}

std::vector<VirtualPort> create_virtual_ports() {
  std::vector<VirtualPort> ports(vds::kMaxPortCount);
  for (unsigned index = 0; index < ports.size(); ++index) {
    ports[index].path = vds::port_name_for_index(index);
    ports[index].usb = std::make_unique<vds::UsbipPort>();
  }
  return ports;
}

void sync_hidden_pad_markers(std::span<const ControllerRuntime> controllers,
                             bool hide_bluetooth, bool claim_pucks,
                             vds::Logger &logger) {
  std::vector<std::string> wanted;
  if (claim_pucks) {
    wanted.emplace_back(kSteamHiddenMarker);
  }
  for (const auto &controller : controllers) {
    if (vds::is_steam_address(controller.config.address)) {
      wanted.emplace_back(kSteamHiddenMarker);
    } else if (hide_bluetooth) {
      wanted.push_back(controller.config.address);
    }
  }

  std::error_code error;
  std::filesystem::create_directories(kHiddenPadDir, error);
  for (const auto &entry :
       std::filesystem::directory_iterator(kHiddenPadDir, error)) {
    if (std::find(wanted.begin(), wanted.end(),
                  entry.path().filename().string()) == wanted.end()) {
      std::filesystem::remove(entry.path(), error);
    }
  }
  for (const auto &name : wanted) {
    const auto marker = std::filesystem::path(kHiddenPadDir) / name;
    if (!std::filesystem::exists(marker, error) &&
        !std::ofstream(marker).good()) {
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
                 "cannot create " + marker.string() +
                     "; a registered controller stays visible to other "
                     "programs");
    }
  }
}

/*
 * The rule only decides the access of a new node: a program keeps a dongle
 * it opened before the marker existed, and a hidden dongle stays hidden
 * after the marker is gone.
 */
void hide_steam_dongles(bool hidden, vds::Logger &logger) {
  const unsigned nodes = vds::recreate_steam_dongle_nodes(hidden);
  if (nodes > 0) {
    logger.log(vds::LogScope::Hid, vds::LogLevel::Info,
               "made " + std::to_string(nodes) +
                   " Steam Controller dongle nodes anew to " +
                   (hidden ? "hide them from" : "give them back to") +
                   " other programs");
  }
}

/*
 * A passed driver test holds for one build of the kernel and of
 * hid-playstation, so it is kept next to the database: the first line names
 * the kernel, the second the module. A failed one is tested again, because a
 * wrong "no" would stick until the next kernel.
 */
constexpr const char *kKernelFixRecord = "kernel-fix";

std::string kernel_build() {
  struct utsname names {};
  if (::uname(&names) != 0) {
    return {};
  }
  return std::string(names.release) + " " + names.version;
}

/* Empty while hid-playstation is not loaded, and when it is built in. */
std::string playstation_module_build() {
  std::ifstream file("/sys/module/hid_playstation/srcversion");
  std::string build;
  std::getline(file, build);
  return build;
}

/* The module build of a test this kernel passed, else empty. */
std::optional<std::string>
remembered_kernel_fix(const std::filesystem::path &record) {
  std::ifstream file(record);
  std::string kernel;
  std::string module;
  if (!std::getline(file, kernel) || kernel.empty() ||
      kernel != kernel_build()) {
    return std::nullopt;
  }
  std::getline(file, module);
  return module;
}

void remember_kernel_fix(const std::filesystem::path &record,
                         vds::Logger &logger) {
  try {
    vds::write_file_atomic(record, kernel_build() + "\n" +
                                       playstation_module_build() + "\n");
  } catch (const std::exception &error) {
    logger.log(vds::LogScope::Daemon, vds::LogLevel::Warn,
               std::string("cannot remember the kernel check: ") +
                   error.what());
  }
}

/*
 * Tells whether this kernel's hid-playstation parses the controller's
 * Bluetooth audio reports as input. Once audio is enabled the pad sends 100
 * of them per second under the ID and size of its state report. An unfixed
 * driver turns each into about ten key events on the gamepad it exposes, a
 * fixed one into none. Empty when the pad sent too few audio reports to tell.
 */
std::optional<bool>
kernel_misreads_bluetooth_audio(vds::BtBackend &bt_backend, VirtualPort &port,
                                const std::string &gamepad_event,
                                vds::Logger &logger) {
  const vds::UniqueFd gamepad(
      ::open(gamepad_event.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  if (!gamepad) {
    throw std::runtime_error("failed to open " + gamepad_event + ": " +
                             std::strerror(errno));
  }

  vds::PcmAudioExtractor extractor;
  const std::vector<std::uint8_t> silence(
      vds::kPcmWindowFrames * VDS_AUDIO_CHANNELS * sizeof(std::int16_t), 0);
  unsigned audio_reports = 0;
  unsigned key_events = 0;
  const auto deadline = Clock::now() + kSharingProbeTimeout;
  auto next_send = Clock::now();
  while (audio_reports < kSharingProbeAudioReports &&
         Clock::now() < deadline) {
    if (Clock::now() >= next_send) {
      for (const auto &chunk : extractor.push_usb_audio(silence)) {
        (void)bt_backend.try_send_output_report(
            port.haptics_builder.build_packet(
                chunk.haptics, chunk.speaker, port.output_state.state(), true,
                false, vds::kDefaultAudioBufferLength));
      }
      next_send += kAudioOutputInterval;
    }

    std::array<pollfd, 2> fds = {{
        {.fd = bt_backend.hidraw_fd(), .events = POLLIN, .revents = 0},
        {.fd = gamepad.get(), .events = POLLIN, .revents = 0},
    }};
    (void)::poll(fds.data(), fds.size(), kPendingOutputPollMs);
    while (const auto packet = bt_backend.read_interrupt_packet()) {
      if (vds::bt_input_payload_type(*packet) ==
          vds::BtInputPayloadType::Audio) {
        ++audio_reports;
      }
    }
    std::array<input_event, 64> events{};
    while (true) {
      const ssize_t got =
          ::read(gamepad.get(), events.data(), sizeof(events));
      if (got <= 0) {
        break;
      }
      const auto count = static_cast<std::size_t>(got) / sizeof(events[0]);
      key_events += static_cast<unsigned>(
          std::count_if(events.begin(), events.begin() + count,
                        [](const input_event &event) {
                          return event.type == EV_KEY;
                        }));
    }
  }

  /* Stop the microphone stream again; nothing has asked for audio yet. */
  (void)bt_backend.try_send_output_report(
      port.output_state.build_bt_mic_report(false));
  (void)bt_backend.try_send_output_report(
      port.output_state.build_bt_state_report());

  logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
             "hid-playstation check on " + gamepad_event +
                 " audio_reports=" + std::to_string(audio_reports) +
                 " key_events=" + std::to_string(key_events));
  if (audio_reports < kSharingProbeMinAudioReports) {
    return std::nullopt;
  }
  /* Far above what pressing buttons adds, far below what an unfixed driver
   * makes of the same reports. */
  return key_events >= 2 * audio_reports;
}

void complete_shared_controller(ControllerRuntime &controller,
                                VirtualPort &port, const vds::BluezHidPad &pad,
                                BluetoothSharing &sharing,
                                vds::Logger &logger) {
  vds::UniqueFd hidraw_fd(
      ::open(pad.hidraw.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
  if (!hidraw_fd) {
    throw std::runtime_error("failed to open " + pad.hidraw + ": " +
                             std::strerror(errno));
  }
  const std::uint32_t profile = resolve_config_profile(controller.config);
  vds::BtBackend candidate(controller.config.address, std::move(hidraw_fd));

  reset_virtual_port(port);
  port.usb->disconnect();
  initialize_bt_controller(candidate, port, logger);
  candidate.send_output_report(port.output_state.build_bt_init_report());
  if (sharing == BluetoothSharing::Unknown) {
    const auto misreads =
        kernel_misreads_bluetooth_audio(candidate, port, pad.gamepad_event,
                                        logger);
    if (misreads && *misreads) {
      sharing = BluetoothSharing::Unsupported;
      throw std::runtime_error(std::string(vds::kBluetoothSharingUnsupported));
    }
    if (misreads) {
      sharing = BluetoothSharing::Supported;
    }
  }
  port.last_sent_bt_state = port.output_state.state();

  controller.backend = std::move(candidate);
  controller.detected_profile = profile;
  port.usb->connect(profile);
  controller.virtual_connected = true;
  controller.bluez_kicked = false;
  controller.last_error.clear();

  logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
             "hidraw backend connected address=" + controller.config.address +
                 " device=" + port.path + " node=" + pad.hidraw +
                 " profile=" + profile_name(profile));
}

/* Attaches registered pads that BlueZ's input plugin connected. */
void attach_bluez_pads(std::vector<VirtualPort> &ports,
                       std::vector<ControllerRuntime> &controllers,
                       BluetoothSharing &sharing, vds::Logger &logger,
                       bool &epoll_dirty) {
  for (auto &controller : controllers) {
    if (controller_uses_port(controller) ||
        vds::is_steam_address(controller.config.address)) {
      continue;
    }
    const auto pad = vds::find_bluez_hid_pad(controller.config.address);
    if (!pad) {
      continue;
    }
    if (sharing == BluetoothSharing::Unsupported) {
      controller.last_error = vds::kBluetoothSharingUnsupported;
      continue;
    }
    if (!pad->hidden) {
      /*
       * It connected before it was registered, so other programs may hold
       * it open. Only a reconnect closes those handles.
       */
      if (controller.bluez_kicked) {
        constexpr std::string_view kNotHidden =
            "the controller reconnected without being hidden from other "
            "programs; is 72-vds-dualsense-hide.rules installed?";
        if (controller.last_error != kNotHidden) {
          logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Error,
                     "address=" + controller.config.address + " " +
                         std::string(kNotHidden));
        }
        controller.last_error = kNotHidden;
        continue;
      }
      controller.bluez_kicked = true;
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
                 "disconnecting address=" + controller.config.address +
                     " so it reconnects hidden from other programs");
      try {
        (void)vds::disconnect_bluez_device(controller.config.address);
      } catch (const std::exception &error) {
        logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
                   "BlueZ disconnect failed address=" +
                       controller.config.address + " error=" + error.what());
      }
      continue;
    }

    const auto port_index = claim_port_index(ports, controllers, controller);
    if (!port_index) {
      continue;
    }

    try {
      complete_shared_controller(controller, ports[*port_index], *pad,
                                 sharing, logger);
      epoll_dirty = true;
    } catch (const std::exception &error) {
      controller.backend.reset();
      controller.virtual_connected = false;
      controller.device.clear();
      controller.last_error = error.what();
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Error,
                 "connect failed address=" + controller.config.address +
                     " node=" + pad->hidraw + " error=" + error.what());
    }
  }
}

void complete_steam_controller(ControllerRuntime &controller,
                               VirtualPort &port, const vds::SteamPad &pad,
                               vds::Logger &logger) {
  vds::UniqueFd hidraw_fd(
      ::open(pad.hidraw.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
  if (!hidraw_fd) {
    throw std::runtime_error("failed to open " + pad.hidraw + ": " +
                             std::strerror(errno));
  }
  vds::SteamBackend candidate(std::move(hidraw_fd), Clock::now());

  reset_virtual_port(port);
  port.usb->disconnect();
  for (const auto &report : vds::steam_virtual_feature_reports(pad.serial)) {
    cache_feature_report(port, report, false, logger);
  }
  controller.steam.emplace(std::move(candidate));
  controller.steam->set_tuning(controller.config.steam);
  /* The made-up feature reports are those of a plain DualSense. */
  controller.detected_profile = VDS_PROFILE_DS5;
  port.usb->connect(VDS_PROFILE_DS5);
  controller.virtual_connected = true;
  controller.last_error.clear();

  logger.log(vds::LogScope::Hid, vds::LogLevel::Info,
             "Steam Controller connected address=" +
                 controller.config.address + " device=" + port.path +
                 " node=" + pad.hidraw);
}

/* Attaches registered Steam Controllers that are switched on. */
void attach_steam_pads(std::vector<VirtualPort> &ports,
                       std::vector<ControllerRuntime> &controllers,
                       vds::Logger &logger, bool &epoll_dirty) {
  const auto is_steam = [](const ControllerRuntime &controller) {
    return vds::is_steam_address(controller.config.address);
  };
  if (std::none_of(controllers.begin(), controllers.end(), is_steam)) {
    return;
  }
  const std::vector<vds::SteamPad> pads = vds::find_steam_pads();
  for (auto &controller : controllers) {
    if (!is_steam(controller) || controller_uses_port(controller)) {
      continue;
    }
    const std::string_view serial =
        std::string_view(controller.config.address)
            .substr(vds::kSteamAddressPrefix.size());
    const auto pad = std::find_if(pads.begin(), pads.end(),
                                  [&](const vds::SteamPad &candidate) {
                                    return candidate.serial == serial;
                                  });
    if (pad == pads.end()) {
      controller.steam_failed = false;
      continue;
    }
    if (!pad->hidden) {
      constexpr std::string_view kNotHidden =
          "the Steam Controller is not hidden from other programs; is "
          "72-vds-dualsense-hide.rules installed?";
      if (controller.last_error != kNotHidden) {
        logger.log(vds::LogScope::Hid, vds::LogLevel::Error,
                   "address=" + controller.config.address + " " +
                       std::string(kNotHidden));
      }
      controller.last_error = kNotHidden;
      continue;
    }
    if (controller.steam_failed) {
      continue;
    }
    const auto port_index = claim_port_index(ports, controllers, controller);
    if (!port_index) {
      continue;
    }

    try {
      complete_steam_controller(controller, ports[*port_index], *pad, logger);
      epoll_dirty = true;
    } catch (const std::exception &error) {
      controller.steam.reset();
      controller.virtual_connected = false;
      controller.device.clear();
      controller.last_error = error.what();
      /* Closing the node brings the kernel's gamepad back, as a new one. */
      controller.steam_failed = true;
      logger.log(vds::LogScope::Hid, vds::LogLevel::Error,
                 "connect failed address=" + controller.config.address +
                     " node=" + pad->hidraw + " error=" + error.what());
    }
  }
}

/*
 * Every Steam Controller that is registered or switched on and, with
 * bluetooth, the DualSenses BlueZ knows.
 */
std::vector<vds::ControllerTarget>
list_controller_targets(std::span<const ControllerRuntime> controllers,
                        bool bluetooth) {
  std::vector<vds::ControllerTarget> targets;
  if (bluetooth) {
    targets = vds::list_bluez_controller_targets();
  }
  const auto add = [&](std::string address, bool online) {
    const auto known = std::find_if(targets.begin(), targets.end(),
                                    [&](const vds::ControllerTarget &target) {
                                      return target.address == address;
                                    });
    if (known != targets.end()) {
      known->online |= online;
      return;
    }
    targets.push_back(vds::ControllerTarget{
        .address = std::move(address),
        .name = "Steam Controller",
        .profile = vds::ControllerProfile::Unspecified,
        .online = online,
        .trusted = true,
    });
  };
  for (const auto &controller : controllers) {
    if (vds::is_steam_address(controller.config.address)) {
      add(controller.config.address, controller.steam.has_value());
    }
  }
  for (const auto &pad : vds::find_steam_pads()) {
    add(std::string(vds::kSteamAddressPrefix) + pad.serial, true);
  }
  return targets;
}

bool has_pending_bt_output(std::span<const VirtualPort> ports,
                           std::span<const ControllerRuntime> controllers) {
  for (const auto &controller : controllers) {
    if (!controller.backend) {
      continue;
    }
    const auto port_index = find_port_index(ports, controller.device);
    if (port_index && (ports[*port_index].pending_bt_state_report ||
                       !ports[*port_index].pending_audio_chunks.empty() ||
                       !ports[*port_index].pending_control_writes.empty() ||
                       ports[*port_index].speaker_waveout_active ||
                       ports[*port_index].test_buzz_frames_left > 0)) {
      return true;
    }
  }
  return false;
}

int next_wakeup_timeout_ms(std::span<const VirtualPort> ports,
                           std::span<const ControllerRuntime> controllers) {
  const auto now = Clock::now();
  int timeout_ms = 60000;
  if (has_pending_bt_output(ports, controllers)) {
    timeout_ms = std::min(timeout_ms, kPendingOutputPollMs);
  }
  for (const auto &controller : controllers) {
    if (controller.steam) {
      const auto due = controller.steam->next_due();
      if (due <= now) {
        return 0;
      }
      const auto wait =
          std::chrono::ceil<std::chrono::milliseconds>(due - now);
      timeout_ms = std::min(timeout_ms, static_cast<int>(wait.count()));
      continue;
    }
    if (!controller.backend) {
      continue;
    }
    const auto port_index = find_port_index(ports, controller.device);
    if (!port_index || ports[*port_index].pending_audio_chunks.empty()) {
      continue;
    }

    const auto next_time = ports[*port_index].next_haptics_send_time;
    if (next_time == Clock::time_point{} || next_time <= now) {
      return 0;
    }
    const auto audio_wait =
        std::chrono::ceil<std::chrono::milliseconds>(next_time - now);
    timeout_ms = std::min(timeout_ms, static_cast<int>(audio_wait.count()));
  }
  return timeout_ms;
}

bool flush_pending_audio_chunk(VirtualPort &port,
                               vds::BtBackend &bt_backend,
                               std::uint32_t trace_flags,
                               std::uint8_t audio_buffer_length,
                               vds::Logger &logger) {
  if (port.pending_audio_chunks.empty()) {
    return true;
  }

  const auto now = Clock::now();
  if (port.next_haptics_send_time != Clock::time_point{} &&
      now < port.next_haptics_send_time) {
    return true;
  }

  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  std::size_t stale_dropped = 0;
  while (port.pending_audio_chunks.size() > kFreshPendingAudioChunks) {
    port.pending_audio_chunks.pop_front();
    ++stale_dropped;
    ++port.trace_state.dropped_audio_haptics_count;
    ++port.trace_state.queue_dropped_audio_haptics_count;
    ++port.trace_state.stale_audio_haptics_count;
  }
  if (stale_dropped > 0 &&
      (port.trace_state.stale_audio_haptics_count == stale_dropped ||
       port.trace_state.stale_audio_haptics_count % 1000 == 0)) {
    logger.log(
        vds::LogScope::Output, vds::LogLevel::Warn,
        port.path + " dropped stale BT 0x36 audio packets count=" +
            std::to_string(port.trace_state.stale_audio_haptics_count) +
            " pending=" + std::to_string(port.pending_audio_chunks.size()) +
            " dropped=" +
            std::to_string(port.trace_state.dropped_audio_haptics_count) +
            " blocked=" +
            std::to_string(port.trace_state.blocked_audio_haptics_count));
  }

  const auto &chunk = port.pending_audio_chunks.front();
  const auto packet = port.haptics_builder.build_packet(
      chunk.haptics, chunk.speaker, port.output_state.state(), true,
      port.headset_plugged, audio_buffer_length);
  const auto send_start = Clock::now();
  if (!bt_backend.try_send_output_report(packet)) {
    ++port.trace_state.dropped_audio_haptics_count;
    ++port.trace_state.blocked_audio_haptics_count;
    port.pending_audio_chunks.pop_front();
    port.next_haptics_send_time = Clock::now() + kHapticsOutputBlockedRetry;
    if (port.trace_state.blocked_audio_haptics_count == 1 ||
        port.trace_state.blocked_audio_haptics_count % 1000 == 0) {
      logger.log(
          vds::LogScope::Output, vds::LogLevel::Warn,
          port.path +
              " dropped BT 0x36 audio packet after HID queue blocked "
              "count=" +
              std::to_string(port.trace_state.blocked_audio_haptics_count) +
              " pending=" + std::to_string(port.pending_audio_chunks.size()) +
              " dropped=" +
              std::to_string(port.trace_state.dropped_audio_haptics_count) +
              " stale=" +
              std::to_string(port.trace_state.stale_audio_haptics_count));
    }
    return false;
  }

  const auto send_duration = Clock::now() - send_start;
  port.pending_audio_chunks.pop_front();
  port.last_sent_bt_state = port.output_state.state();
  if (port.pending_bt_state &&
      *port.pending_bt_state == *port.last_sent_bt_state) {
    port.pending_bt_state.reset();
    port.pending_bt_state_report.reset();
  }
  port.next_haptics_send_time = now + kAudioOutputInterval;

  if (output_trace) {
    trace_output_latency(port.path, "audio_bt_send",
                         port.trace_state.output_audio_send_latency,
                         send_duration, kOutputTraceSlowWarn, logger);
  }
  return true;
}

constexpr std::size_t kPcmWindowBytes =
    vds::kPcmWindowFrames * VDS_AUDIO_CHANNELS * sizeof(std::int16_t);
using PcmWindow = std::array<std::uint8_t, kPcmWindowBytes>;

/* A sine on one channel pair of the USB audio format, silence on the other. */
PcmWindow sine_window(std::size_t first_channel, std::uint32_t period_frames,
                      std::int16_t amplitude, std::uint32_t &phase) {
  PcmWindow pcm{};
  for (std::size_t frame = 0; frame < vds::kPcmWindowFrames; ++frame) {
    const double angle = kTwoPi * static_cast<double>(phase) /
                         static_cast<double>(period_frames);
    const auto sample = static_cast<std::int16_t>(
        std::sin(angle) * static_cast<double>(amplitude));
    phase = (phase + 1) % period_frames;

    for (std::size_t channel = first_channel;
         channel < first_channel + vds::kSpeakerChannels; ++channel) {
      const std::size_t offset =
          (frame * VDS_AUDIO_CHANNELS + channel) * sizeof(std::int16_t);
      const auto value = static_cast<std::uint16_t>(sample);
      pcm[offset + 0] = static_cast<std::uint8_t>(value & 0xff);
      pcm[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    }
  }
  return pcm;
}

void start_test_buzz(VirtualPort &port, vds::Logger &logger) {
  port.test_buzz_frames_left = kTestBuzzFrames;
  port.test_buzz_phase = 0;
  port.waveout_extractor = vds::PcmAudioExtractor{};
  /* Each audio packet carries this state, as when a game opens the card. */
  port.output_state.set_audio_out_stream_active(true, port.headset_plugged);
  logger.log(vds::LogScope::Output, vds::LogLevel::Info,
             port.path + " test buzz");
}

void enqueue_test_buzz_chunk(VirtualPort &port, vds::Logger &logger) {
  if (port.test_buzz_frames_left == 0 || !port.pending_audio_chunks.empty()) {
    return;
  }

  /* The haptics pair follows the speaker pair. */
  const PcmWindow pcm =
      sine_window(vds::kSpeakerChannels, kTestBuzzPeriodFrames,
                  kTestBuzzAmplitude, port.test_buzz_phase);
  port.test_buzz_frames_left -=
      std::min(port.test_buzz_frames_left,
               static_cast<std::uint32_t>(vds::kPcmWindowFrames));
  try {
    for (const auto &chunk : port.waveout_extractor.push_usb_audio(pcm)) {
      port.pending_audio_chunks.push_back(chunk);
    }
  } catch (const std::runtime_error &error) {
    log_audio_encode_failure(port, error, logger);
  }
}

void enqueue_speaker_waveout_chunk(VirtualPort &port, std::uint32_t trace_flags,
                                   vds::Logger &logger) {
  if (!port.speaker_waveout_active || !port.pending_audio_chunks.empty()) {
    return;
  }

  const PcmWindow pcm =
      sine_window(0, kSpeakerWaveoutPeriodFrames, kSpeakerWaveoutAmplitude,
                  port.speaker_waveout_phase);

  std::vector<vds::AudioChunk> chunks;
  try {
    chunks = port.waveout_extractor.push_usb_audio(pcm);
  } catch (const std::runtime_error &error) {
    log_audio_encode_failure(port, error, logger);
    return;
  }
  for (const auto &chunk : chunks) {
    if (port.pending_audio_chunks.size() >= kMaxPendingAudioChunks) {
      break;
    }
    port.pending_audio_chunks.push_back(chunk);
  }
  if (trace_enabled(trace_flags, kTraceOutput) && !chunks.empty()) {
    logger.log(vds::LogScope::Output, vds::LogLevel::Debug,
               port.path + " queued WebHID speaker waveout chunk pending=" +
                   std::to_string(port.pending_audio_chunks.size()));
  }
}

void service_steam_controller(ControllerRuntime &controller,
                              std::vector<VirtualPort> &ports,
                              vds::Logger &logger, bool &epoll_dirty) {
  const auto port_index = find_port_index(ports, controller.device);
  if (!port_index) {
    controller.steam.reset();
    controller.virtual_connected = false;
    controller.device.clear();
    epoll_dirty = true;
    return;
  }
  auto &port = ports[*port_index];
  vds::SteamBackend &steam = *controller.steam;
  try {
    const auto now = Clock::now();
    if (port.test_buzz_frames_left > 0 && steam.wants_audio()) {
      const PcmWindow pcm =
          sine_window(vds::kSpeakerChannels, kTestBuzzPeriodFrames,
                      kTestBuzzAmplitude, port.test_buzz_phase);
      port.test_buzz_frames_left -=
          std::min(port.test_buzz_frames_left,
                   static_cast<std::uint32_t>(vds::kPcmWindowFrames));
      steam.push_usb_audio(pcm, now);
    }
    const bool was_streaming = steam.streaming();
    (void)steam.service(now);
    if (was_streaming && !steam.streaming()) {
      const auto &stats = steam.stream_stats();
      logger.log(vds::LogScope::Output, vds::LogLevel::Info,
                 port.path + " haptics stream ended packets=" +
                     std::to_string(stats.packets) +
                     " starved=" + std::to_string(stats.starved) +
                     " faults=" + std::to_string(stats.faults) +
                     " status=" + hex_u8(stats.status));
    }
  } catch (const vds::SteamLinkError &error) {
    drop_backend(controller, port, error.what(), logger);
    epoll_dirty = true;
  }
}

void flush_pending_outputs(std::vector<VirtualPort> &ports,
                           std::vector<ControllerRuntime> &controllers,
                           std::uint32_t trace_flags, vds::Logger &logger,
                           bool &epoll_dirty) {
  for (auto &controller : controllers) {
    if (controller.steam) {
      service_steam_controller(controller, ports, logger, epoll_dirty);
      continue;
    }
    if (!controller.backend) {
      continue;
    }
    const auto port_index = find_port_index(ports, controller.device);
    if (!port_index) {
      controller.backend.reset();
      controller.virtual_connected = false;
      controller.device.clear();
      epoll_dirty = true;
      continue;
    }

    try {
      auto &port = ports[*port_index];
      /* Control uses PSM 0x0011; the drains below share PSM 0x0013. */
      (void)flush_pending_control_writes(port, *controller.backend);
      if (!flush_pending_bt_state_report(port, *controller.backend, trace_flags,
                                         logger)) {
        continue;
      }
      enqueue_speaker_waveout_chunk(port, trace_flags, logger);
      enqueue_test_buzz_chunk(port, logger);
      (void)flush_pending_audio_chunk(port, *controller.backend, trace_flags,
                                      controller.config.audio_buffer_length,
                                      logger);
    } catch (const vds::BluetoothLinkError &error) {
      drop_backend(controller, ports[*port_index], error.what(), logger);
      epoll_dirty = true;
    }
  }
}

/*
 * A pad BlueZ shares with vdsd stays connected and hidden when vdsd lets go
 * of it. Only a reconnect gives it back to other programs.
 */
void release_shared_pad(const std::string &address, vds::Logger &logger) {
  if (vds::is_steam_address(address)) {
    return;
  }
  const auto pad = vds::find_bluez_hid_pad(address);
  if (!pad || !pad->hidden) {
    return;
  }
  try {
    (void)vds::disconnect_bluez_device(address);
  } catch (const std::exception &error) {
    logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
               "BlueZ disconnect failed address=" + address +
                   " error=" + error.what());
  }
}

void reconcile_controller_configs(std::vector<VirtualPort> &ports,
                                  std::vector<ControllerRuntime> &controllers,
                                  const std::string &db_path,
                                  vds::Logger &logger) {
  const vds::ConfigDb db = vds::load_config_db(db_path);
  logger.log(vds::LogScope::Config, vds::LogLevel::Info,
             "loaded controller config count=" +
                 std::to_string(db.controllers.size()));

  std::vector<bool> preserved(controllers.size(), false);
  std::vector<std::string> active_devices;
  std::vector<ControllerRuntime> next_controllers;
  next_controllers.reserve(db.controllers.size());

  for (const auto &config : db.controllers) {
    if (!controller_has_present_allowed_port(ports, config)) {
      logger.log(vds::LogScope::Config, vds::LogLevel::Warn,
                 "controller binding unavailable address=" + config.address +
                     " reason=no allowed virtual port present ports=[" +
                     vds::format_ports(config.ports) + "]");
    }
    ControllerRuntime next{
        .config = config,
        .device = {},
        .detected_profile = std::nullopt,
        .backend = std::nullopt,
        .steam = std::nullopt,
        .virtual_connected = false,
        .last_error = {},
        .bluez_kicked = false,
        .steam_failed = false,
    };

    for (std::size_t old_index = 0; old_index < controllers.size();
         ++old_index) {
      auto &old = controllers[old_index];
      if (preserved[old_index] || old.config.address != config.address ||
          old.config.profile != config.profile) {
        continue;
      }
      const bool old_uses_port = controller_uses_port(old);
      const bool old_device_present =
          !old.device.empty() && find_port_index(ports, old.device);
      const bool old_device_allowed =
          old_device_present &&
          vds::controller_config_allows_port_name(config, old.device);
      const bool old_device_available =
          old_device_allowed &&
          std::find(active_devices.begin(), active_devices.end(), old.device) ==
              active_devices.end();

      next.detected_profile = old.detected_profile;
      next.last_error = std::move(old.last_error);
      next.bluez_kicked = old.bluez_kicked;
      next.steam_failed = old.steam_failed;
      if (old_uses_port && old_device_available) {
        next.device = std::move(old.device);
        next.backend = std::move(old.backend);
        next.steam = std::move(old.steam);
        if (next.steam) {
          next.steam->set_tuning(config.steam);
        }
        next.virtual_connected = old.virtual_connected;
        active_devices.push_back(next.device);
      } else if (old_uses_port) {
        if (old.virtual_connected && old_device_present) {
          const auto old_port_index = find_port_index(ports, old.device);
          disconnect_virtual_port(ports[*old_port_index], logger);
        }
        if (old.backend) {
          logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
                     "backend removed address=" + old.config.address +
                         " device=" + old.device);
        }
        old.backend.reset();
        old.steam.reset();
        release_shared_pad(old.config.address, logger);
        old.virtual_connected = false;
        old.device.clear();
      }
      preserved[old_index] = true;
      break;
    }

    logger.log(vds::LogScope::Config, vds::LogLevel::Info,
               "controller registered address=" + config.address +
                   " profile=\"" +
                   vds::controller_profile_name(config.profile) + "\"");
    next_controllers.push_back(std::move(next));
  }

  for (std::size_t i = 0; i < controllers.size(); ++i) {
    if (preserved[i]) {
      continue;
    }
    if (controllers[i].virtual_connected) {
      if (const auto port_index =
              find_port_index(ports, controllers[i].device)) {
        disconnect_virtual_port(ports[*port_index], logger);
      }
    }
    if (controllers[i].backend) {
      logger.log(
          vds::LogScope::Bluetooth, vds::LogLevel::Info,
          "backend removed address=" + controllers[i].config.address +
              " device=" + controllers[i].device);
    }
    release_shared_pad(controllers[i].config.address, logger);
  }

  controllers = std::move(next_controllers);
}

// Root, members of the vds group and users logged in on a local seat may
// control the daemon. SSH-only users and service accounts are refused.
bool control_peer_allowed(int client_fd) {
  ucred peer{};
  socklen_t length = sizeof(peer);
  if (::getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) < 0) {
    return false;
  }
  if (peer.uid == 0 || ::sd_uid_get_seats(peer.uid, 0, nullptr) > 0) {
    return true;
  }

  const group *vds_group = ::getgrnam("vds");
  if (vds_group == nullptr) {
    return false;
  }
  const gid_t vds_gid = vds_group->gr_gid;
  if (peer.gid == vds_gid) {
    return true;
  }
  const passwd *user = ::getpwuid(peer.uid);
  if (user == nullptr) {
    return false;
  }
  int count = 32;
  std::vector<gid_t> groups(static_cast<std::size_t>(count));
  if (::getgrouplist(user->pw_name, user->pw_gid, groups.data(), &count) < 0) {
    groups.resize(static_cast<std::size_t>(count));
    (void)::getgrouplist(user->pw_name, user->pw_gid, groups.data(), &count);
  }
  groups.resize(static_cast<std::size_t>(count));
  return std::find(groups.begin(), groups.end(), vds_gid) != groups.end();
}

void handle_control_client(int control_fd, std::span<VirtualPort> ports,
                           std::span<ControllerRuntime> controllers,
                           const std::string &db_path,
                           std::optional<bool> kernel_fix,
                           std::uint32_t &trace_flags, bool &reload_requested,
                           Clock::time_point &puck_claim_until,
                           vds::Logger &logger, unsigned &accept_failures) {
  vds::UniqueFd client_fd(
      ::accept4(control_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
  if (!client_fd) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return;
    }
    /*
     * An unaccepted connection stays queued and keeps the listener readable,
     * so a permanent failure such as EMFILE would spin the loop. Escalate to
     * fatal once retrying has clearly stopped helping.
     */
    if (++accept_failures >= kMaxAcceptFailures) {
      throw std::runtime_error("accept control client failed: " +
                               std::string(std::strerror(errno)));
    }
    logger.log(vds::LogScope::Control, vds::LogLevel::Error,
               "accept control client failed: " +
                   std::string(std::strerror(errno)));
    return;
  }
  accept_failures = 0;

  const auto deadline = Clock::now() + kControlClientTimeout;
  std::string request;
  while (request.find('\n') == std::string::npos) {
    std::array<char, 256> buffer{};
    const ssize_t got = ::read(client_fd.get(), buffer.data(), buffer.size());
    if (got > 0) {
      if (request.size() + static_cast<std::size_t>(got) >
          kControlRequestMaxBytes) {
        logger.log(vds::LogScope::Control, vds::LogLevel::Warn,
                   "control request too long; dropping client");
        return;
      }
      request.append(buffer.data(), static_cast<std::size_t>(got));
      continue;
    }
    if (got == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      logger.log(vds::LogScope::Control, vds::LogLevel::Warn,
                 "control client read failed: " +
                     std::string(std::strerror(errno)));
      return;
    }

    const auto now = Clock::now();
    if (now >= deadline) {
      logger.log(vds::LogScope::Control, vds::LogLevel::Warn,
                 "control request timed out; dropping client");
      return;
    }
    pollfd pfd{.fd = client_fd.get(), .events = POLLIN, .revents = 0};
    const int ready = ::poll(
        &pfd, 1,
        static_cast<int>(
            std::chrono::ceil<std::chrono::milliseconds>(deadline - now)
                .count()));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      logger.log(vds::LogScope::Control, vds::LogLevel::Warn,
                 "control client poll failed: " +
                     std::string(std::strerror(errno)));
      return;
    }
    if (ready == 0) {
      logger.log(vds::LogScope::Control, vds::LogLevel::Warn,
                 "control request timed out; dropping client");
      return;
    }
  }
  if (request.empty()) {
    return;
  }

  const auto send_reply = [&](const std::string &text) {
    try {
      const auto now = Clock::now();
      const int reply_timeout_ms =
          now >= deadline
              ? 0
              : static_cast<int>(
                    std::chrono::ceil<std::chrono::milliseconds>(deadline - now)
                        .count());
      vds::write_full(client_fd.get(),
                      std::span(reinterpret_cast<const std::uint8_t *>(
                                    text.data()),
                                text.size()),
                      "control reply", reply_timeout_ms);
    } catch (const std::exception &error) {
      logger.log(vds::LogScope::Control, vds::LogLevel::Warn,
                 std::string("control reply failed: ") + error.what());
    }
  };

  if (!control_peer_allowed(client_fd.get())) {
    send_reply(vds::format_control_error_reply(
        "permission denied: controlling vdsd needs root, membership in the "
        "vds group, or a login on this computer's own screen"));
    return;
  }

  std::string reply;
  const std::string command = trim_command(request);
  std::vector<vds::VdsdControlControllerStatus> controller_statuses;
  controller_statuses.reserve(controllers.size());
  for (const auto &controller : controllers) {
    const auto port_index = controller.virtual_connected
                                ? find_port_index(ports, controller.device)
                                : std::nullopt;
    const VirtualPort *port = port_index ? &ports[*port_index] : nullptr;
    /* A Steam Controller's firmware report is made up and says nothing. */
    const auto firmware =
        port && !controller.steam &&
                port->feature_cached[vds::kFirmwareInfoReportId]
            ? vds::usb_firmware_info(
                  port->feature_cache[vds::kFirmwareInfoReportId])
            : std::nullopt;
    controller_statuses.push_back(vds::VdsdControlControllerStatus{
        .address = controller.config.address,
        .connected = controller.virtual_connected,
        .path = controller.virtual_connected ? controller.device : "",
        .error = controller.virtual_connected ? "" : controller.last_error,
        .battery_percent = port && port->battery
                               ? std::optional(port->battery->percent)
                               : std::nullopt,
        .charging = port && port->battery && port->battery->charging,
        .headset = port && port->headset_plugged,
        .mic_muted = port && port->mic_muted,
        .firmware = firmware ? std::optional(firmware->update_version)
                             : std::nullopt,
        .firmware_build =
            firmware ? std::optional(firmware->version) : std::nullopt,
        .audio_busy = port && (port->audio_out_stream_active ||
                               port->speaker_waveout_active),
    });
  }

  std::vector<vds::VdsdControlPortCandidate> port_candidates;
  port_candidates.reserve(ports.size());
  for (const auto &port : ports) {
    const auto port_index = vds::port_index_from_name(port.path);
    if (!port_index) {
      continue;
    }

    port_candidates.push_back(vds::VdsdControlPortCandidate{
        .port = *port_index,
        .path = port.path,
    });
  }

  std::vector<vds::VdsdControlPortBinding> port_bindings;
  port_bindings.reserve(controllers.size());
  for (const auto &controller : controllers) {
    if (!controller_uses_port(controller)) {
      continue;
    }
    const auto port = vds::port_index_from_name(controller.device);
    if (!port) {
      continue;
    }
    port_bindings.push_back(vds::VdsdControlPortBinding{
        .port = *port,
        .address = controller.config.address,
        .device_address = {},
    });
  }

  const std::vector<vds::VdsdControlPortStatus> port_statuses =
      vds::build_vdsd_control_port_statuses(port_candidates, port_bindings);

  std::string buzz_address;
  reply = vds::handle_vdsd_control_command(
      command, db_path, controller_statuses, port_statuses,
      [&](const vds::TargetQuery &query) {
        if (query.claim_pucks) {
          puck_claim_until = Clock::now() + kPuckClaimHold;
        }
        return list_controller_targets(controllers, query.bluetooth);
      },
      vds::VdsdControlDaemonStatus{.version = vds::kVersion,
                                   .kernel_fix = kernel_fix},
      trace_flags, reload_requested, buzz_address, logger);
  for (const auto &controller : controllers) {
    const auto port_index = controller.virtual_connected &&
                                    controller.config.address == buzz_address
                                ? find_port_index(ports, controller.device)
                                : std::nullopt;
    if (port_index) {
      start_test_buzz(ports[*port_index], logger);
    }
  }

  send_reply(reply);
}

std::uint64_t encode_event(EventType type, std::size_t index) {
  return (static_cast<std::uint64_t>(type) << 32) |
         static_cast<std::uint32_t>(index);
}

EventSource decode_event(std::uint64_t data) {
  return EventSource{
      .type = static_cast<EventType>(data >> 32),
      .index = static_cast<std::uint32_t>(data),
  };
}

void add_epoll_fd(int epoll_fd, int fd, EventType type, std::size_t index) {
  epoll_event event{};
  event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
  event.data.u64 = encode_event(type, index);
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
    throw std::runtime_error("epoll add failed: " +
                             std::string(std::strerror(errno)));
  }
}

vds::UniqueFd rebuild_epoll(int control_fd, int signal_fd,
                            const vds::InputDeviceMonitor &input_monitor,
                            int sleep_fd, std::span<VirtualPort> ports,
                            std::span<ControllerRuntime> controllers) {
  vds::UniqueFd epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd) {
    throw std::runtime_error("epoll_create1 failed: " +
                             std::string(std::strerror(errno)));
  }

  add_epoll_fd(epoll_fd.get(), control_fd, EventType::Control, 0);
  add_epoll_fd(epoll_fd.get(), signal_fd, EventType::Signal, 0);
  add_epoll_fd(epoll_fd.get(), input_monitor.fd(), EventType::Udev, 0);
  if (sleep_fd >= 0) {
    add_epoll_fd(epoll_fd.get(), sleep_fd, EventType::Sleep, 0);
  }
  for (std::size_t i = 0; i < ports.size(); ++i) {
    add_epoll_fd(epoll_fd.get(), ports[i].usb->frame_fd(), EventType::Port, i);
    add_epoll_fd(epoll_fd.get(), ports[i].usb->poll_fd(), EventType::Usb, i);
  }
  for (std::size_t i = 0; i < controllers.size(); ++i) {
    if (controllers[i].steam) {
      add_epoll_fd(epoll_fd.get(), controllers[i].steam->hidraw_fd(),
                   EventType::SteamInput, i);
    }
    if (!controllers[i].backend) {
      continue;
    }
    add_epoll_fd(epoll_fd.get(), controllers[i].backend->feature_reply_fd(),
                 EventType::BtControl, i);
    add_epoll_fd(epoll_fd.get(), controllers[i].backend->hidraw_fd(),
                 EventType::BtInterrupt, i);
  }
  return epoll_fd;
}

void disconnect_all(std::vector<VirtualPort> &ports,
                    std::vector<ControllerRuntime> &controllers,
                    vds::Logger &logger) {
  for (auto &controller : controllers) {
    if (!controller.virtual_connected) {
      continue;
    }
    if (const auto port_index = find_port_index(ports, controller.device)) {
      disconnect_virtual_port(ports[*port_index], logger);
    }
    controller.virtual_connected = false;
  }
  controllers.clear();
}

/*
 * vhci_hcd refuses system sleep while a virtual controller is plugged in,
 * and it unplugs asynchronously.
 */
void unplug_for_sleep(std::vector<VirtualPort> &ports,
                      std::vector<ControllerRuntime> &controllers,
                      vds::Logger &logger) {
  for (auto &controller : controllers) {
    if (const auto port_index = find_port_index(ports, controller.device)) {
      drop_backend(controller, ports[*port_index],
                   "the system is going to sleep", logger);
    }
  }
  const auto deadline = Clock::now() + kSleepUnplugWait;
  while (!std::all_of(ports.begin(), ports.end(), [](const VirtualPort &port) {
    return port.usb->unplugged();
  })) {
    if (Clock::now() >= deadline) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
                 "a virtual controller is still plugged in; the system may "
                 "refuse to sleep");
      return;
    }
    std::this_thread::sleep_for(kSleepUnplugPoll);
  }
}

vds::UniqueFd acquire_single_instance_lock(const char *path) {
  vds::UniqueFd fd(::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!fd) {
    throw std::runtime_error("failed to open lock file " + std::string(path) +
                             ": " + std::strerror(errno));
  }
  if (::flock(fd.get(), LOCK_EX | LOCK_NB) < 0) {
    if (errno == EWOULDBLOCK) {
      throw std::runtime_error("another vdsd instance already holds " +
                               std::string(path));
    }
    throw std::runtime_error("failed to lock " + std::string(path) + ": " +
                             std::strerror(errno));
  }
  return fd;
}

int run_daemon(const Options &options, vds::Logger &logger) {
  logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
             "started socket=" + options.socket + " log=" + options.log_path +
                 " db=" + options.db_path);
  if (!vds::usbip_host_available()) {
    throw std::runtime_error(kVirtualPortProviderUnavailable);
  }

  const vds::UniqueFd signal_fd = create_signal_fd();

  vds::UniqueFd control_fd(open_control_socket(options.socket));
  vds::InputDeviceMonitor input_monitor;
  SocketPathGuard control_socket_path(options.socket);
  std::vector<VirtualPort> ports = create_virtual_ports();
  logger.log(vds::LogScope::Port, vds::LogLevel::Info,
             "virtual ports ready provider=vhci_hcd count=" +
                 std::to_string(ports.size()));
  std::optional<vds::SleepGuard> sleep_guard;
  try {
    sleep_guard.emplace();
  } catch (const std::exception &error) {
    logger.log(vds::LogScope::Daemon, vds::LogLevel::Warn,
               std::string("the system will refuse to sleep while a "
                           "controller is connected: ") +
                   error.what());
  }
  std::vector<ControllerRuntime> controllers;
  vds::UniqueFd epoll_fd;
  std::uint32_t trace_flags = 0;
  bool reload_requested = true;
  bool epoll_dirty = true;
  bool first_reload = true;
  unsigned control_accept_failures = 0;
  const std::filesystem::path kernel_fix_record =
      std::filesystem::path(options.db_path).parent_path() / kKernelFixRecord;
  /*
   * The module a remembered test ran against. It is compared once the module
   * is loaded, which a connecting DualSense takes care of.
   */
  std::optional<std::string> unverified_module =
      remembered_kernel_fix(kernel_fix_record);
  bool kernel_fix_remembered = unverified_module.has_value();
  BluetoothSharing sharing = kernel_fix_remembered
                                 ? BluetoothSharing::Supported
                                 : BluetoothSharing::Unknown;
  bool scan_pads = false;
  Clock::time_point puck_claim_until{};
  bool pucks_claimed = false;
  /* Between logind announcing sleep and the resume nothing may plug in. */
  bool asleep = false;
  SignalRequests signals;

  while (!signals.stop) {
    if (signals.log_reopen) {
      signals.log_reopen = false;
      try {
        logger.reopen();
        logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
                   "reopened log file");
      } catch (const std::exception &error) {
        logger.log(vds::LogScope::Daemon, vds::LogLevel::Error,
                   std::string("log reopen failed: ") + error.what());
      }
    }

    /*
     * A Steam Controller another program holds has no input device to be
     * found by, so the Add wizard asks for the Pucks while it looks.
     */
    const bool claim_pucks = Clock::now() < puck_claim_until;
    if (claim_pucks != pucks_claimed) {
      pucks_claimed = claim_pucks;
      reload_requested = true;
    }

    if (reload_requested) {
      try {
        reconcile_controller_configs(ports, controllers, options.db_path,
                                     logger);
        epoll_dirty = true;
        sync_hidden_pad_markers(controllers,
                                sharing != BluetoothSharing::Unsupported,
                                pucks_claimed, logger);
        hide_steam_dongles(
            pucks_claimed ||
                std::any_of(controllers.begin(), controllers.end(),
                            [](const ControllerRuntime &controller) {
                              return vds::is_steam_address(
                                  controller.config.address);
                            }),
            logger);
        scan_pads = true;
      } catch (const std::exception &error) {
        /* Starting with no usable state is a setup failure, not a reload. */
        if (first_reload) {
          throw std::runtime_error(
              "failed to load " + options.db_path + ": " + error.what() +
              "; if this database predates 0.3.0, delete it and re-register "
              "controllers with vdsctl attach");
        }
        logger.log(vds::LogScope::Config, vds::LogLevel::Error,
                   std::string("reload failed: ") + error.what());
      }
      reload_requested = false;
      first_reload = false;
    }

    if (scan_pads && !asleep) {
      scan_pads = false;
      if (unverified_module) {
        const std::string loaded = playstation_module_build();
        if (!loaded.empty()) {
          if (loaded != *unverified_module) {
            sharing = BluetoothSharing::Unknown;
            kernel_fix_remembered = false;
          }
          unverified_module.reset();
        }
      }
      const bool was_supported = sharing != BluetoothSharing::Unsupported;
      attach_bluez_pads(ports, controllers, sharing, logger, epoll_dirty);
      if (sharing == BluetoothSharing::Supported && !kernel_fix_remembered) {
        remember_kernel_fix(kernel_fix_record, logger);
        kernel_fix_remembered = true;
      }
      if (was_supported && sharing == BluetoothSharing::Unsupported) {
        /* Let the pads reconnect as plain Bluetooth gamepads meanwhile. */
        sync_hidden_pad_markers(controllers, false, pucks_claimed, logger);
        for (const auto &controller : controllers) {
          release_shared_pad(controller.config.address, logger);
        }
      }
      attach_steam_pads(ports, controllers, logger, epoll_dirty);
    }

    if (epoll_dirty || !epoll_fd) {
      epoll_fd = rebuild_epoll(control_fd.get(), signal_fd.get(), input_monitor,
                               sleep_guard ? sleep_guard->fd() : -1, ports,
                               controllers);
      epoll_dirty = false;
    }

    std::array<epoll_event, 64> events{};
    int timeout_ms = next_wakeup_timeout_ms(ports, controllers);
    if (pucks_claimed) {
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(
          puck_claim_until - Clock::now());
      timeout_ms = std::clamp(static_cast<int>(left.count()), 0, timeout_ms);
    }
    const int ready =
        ::epoll_wait(epoll_fd.get(), events.data(),
                     static_cast<int>(events.size()), timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("epoll_wait failed: " +
                               std::string(std::strerror(errno)));
    }
    if (ready == 0) {
      flush_pending_outputs(ports, controllers, trace_flags, logger,
                            epoll_dirty);
      continue;
    }

    for (int i = 0; i < ready; ++i) {
      const EventSource source = decode_event(events[i].data.u64);
      const std::uint32_t revents = events[i].events;

      if (source.type == EventType::Signal) {
        if ((revents & EPOLLIN) != 0) {
          drain_signal_fd(signal_fd.get(), signals);
        }
        continue;
      }

      if (source.type == EventType::Control) {
        if ((revents & EPOLLIN) != 0) {
          handle_control_client(
              control_fd.get(), ports, controllers, options.db_path,
              sharing == BluetoothSharing::Unknown
                  ? std::nullopt
                  : std::optional(sharing == BluetoothSharing::Supported),
              trace_flags, reload_requested, puck_claim_until, logger,
              control_accept_failures);
        }
        if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
          throw std::runtime_error("control socket epoll error");
        }
        continue;
      }

      if (source.type == EventType::Udev) {
        if ((revents & EPOLLIN) != 0) {
          scan_pads = input_monitor.drain() || scan_pads;
        }
        if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
          throw std::runtime_error("udev monitor epoll error");
        }
        continue;
      }

      if (source.type == EventType::Sleep) {
        try {
          const auto announced = sleep_guard->drain();
          if (announced == vds::SleepGuard::Event::Sleeping) {
            asleep = true;
            unplug_for_sleep(ports, controllers, logger);
            sleep_guard->allow_sleep();
          } else if (announced == vds::SleepGuard::Event::Resumed) {
            asleep = false;
            logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
                       "resumed from sleep");
            scan_pads = true;
          }
        } catch (const std::exception &error) {
          logger.log(vds::LogScope::Daemon, vds::LogLevel::Error,
                     std::string("sleep handling stopped: ") + error.what());
          sleep_guard.reset();
          asleep = false;
        }
        epoll_dirty = true;
        continue;
      }

      if (source.type == EventType::Usb) {
        if (source.index >= ports.size()) {
          continue;
        }
        auto &port = ports[source.index];
        try {
          port.usb->service();
        } catch (const vds::UsbipLinkError &error) {
          ControllerRuntime *controller =
              controller_for_port(controllers, port.path);
          if (!controller) {
            logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
                       port.path + " " + error.what());
            continue;
          }
          /* Let go of the pad as well, so pressing PS starts over. */
          release_shared_pad(controller->config.address, logger);
          controller->last_error = error.what();
          controller->steam_failed = controller->steam.has_value();
          drop_backend(*controller, port, error.what(), logger);
          epoll_dirty = true;
        }
        continue;
      }

      if (source.type == EventType::Port) {
        if (source.index >= ports.size()) {
          continue;
        }
        auto &port = ports[source.index];
        ControllerRuntime *controller =
            controller_for_port(controllers, port.path);
        vds::BtBackend *backend = controller && controller->backend
                                           ? &*controller->backend
                                           : nullptr;
        vds::SteamBackend *steam = controller && controller->steam
                                       ? &*controller->steam
                                       : nullptr;
        if ((revents & EPOLLIN) != 0) {
          for (int frame = 0; frame < kMaxPortFramesPerWake; ++frame) {
            try {
              if (!handle_vds_frame(port, backend, steam, trace_flags,
                                    logger)) {
                break;
              }
            } catch (const vds::BluetoothLinkError &error) {
              if (!controller) {
                throw;
              }
              drop_backend(*controller, port, error.what(), logger);
              epoll_dirty = true;
              break;
            } catch (const vds::SteamLinkError &error) {
              drop_backend(*controller, port, error.what(), logger);
              epoll_dirty = true;
              break;
            } catch (const PortIoError &error) {
              logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                         std::string(error.what()) + "; reloading ports");
              reload_requested = true;
              epoll_dirty = true;
              break;
            }
          }
        }
        if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
          logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                     port.path + " epoll error; reloading ports");
          reload_requested = true;
          epoll_dirty = true;
        }
        continue;
      }

      if (source.type == EventType::SteamInput) {
        if (source.index >= controllers.size() ||
            !controllers[source.index].steam) {
          continue;
        }
        auto &controller = controllers[source.index];
        const auto port_index = find_port_index(ports, controller.device);
        if (!port_index) {
          continue;
        }
        auto &port = ports[*port_index];
        try {
          for (int packet = 0; packet < kMaxBtPacketsPerWake; ++packet) {
            if (!handle_steam_input(port, *controller.steam, logger)) {
              break;
            }
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            drop_backend(controller, port, "Steam Controller dongle unplugged",
                         logger);
            epoll_dirty = true;
          }
        } catch (const vds::SteamLinkError &error) {
          drop_backend(controller, port, error.what(), logger);
          epoll_dirty = true;
        } catch (const PortIoError &error) {
          logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                     std::string(error.what()) + "; reloading ports");
          reload_requested = true;
          epoll_dirty = true;
        }
        continue;
      }

      if (source.index >= controllers.size() ||
          !controllers[source.index].backend) {
        continue;
      }
      auto &controller = controllers[source.index];
      const auto port_index = find_port_index(ports, controller.device);
      if (!port_index) {
        controller.backend.reset();
        controller.virtual_connected = false;
        epoll_dirty = true;
        continue;
      }
      auto &port = ports[*port_index];

      if (source.type == EventType::BtControl) {
        if ((revents & EPOLLIN) != 0) {
          for (int packet = 0; packet < kMaxBtPacketsPerWake; ++packet) {
            try {
              if (!handle_bt_control(port, *controller.backend, trace_flags,
                                     logger)) {
                break;
              }
            } catch (const vds::BluetoothLinkError &error) {
              drop_backend(controller, port, error.what(), logger);
              epoll_dirty = true;
              break;
            } catch (const PortIoError &error) {
              logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                         std::string(error.what()) + "; reloading ports");
              reload_requested = true;
              epoll_dirty = true;
              break;
            }
          }
        }
        if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
          drop_backend(controller, port,
                       "Bluetooth feature reply epoll error", logger);
          epoll_dirty = true;
        }
        continue;
      }

      if (source.type == EventType::BtInterrupt) {
        if ((revents & EPOLLIN) != 0) {
          for (int packet = 0; packet < kMaxBtPacketsPerWake; ++packet) {
            try {
              if (!handle_bt_input(port, *controller.backend, trace_flags,
                                   logger)) {
                break;
              }
            } catch (const vds::BluetoothLinkError &error) {
              drop_backend(controller, port, error.what(), logger);
              epoll_dirty = true;
              break;
            } catch (const PortIoError &error) {
              logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                         std::string(error.what()) + "; reloading ports");
              reload_requested = true;
              epoll_dirty = true;
              break;
            }
          }
        }
        if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
          drop_backend(controller, port,
                       "Bluetooth interrupt channel epoll error", logger);
          epoll_dirty = true;
        }
      }
    }

    flush_pending_outputs(ports, controllers, trace_flags, logger,
                          epoll_dirty);
  }

  logger.log(vds::LogScope::Daemon, vds::LogLevel::Info, "stopping");
  sync_hidden_pad_markers({}, false, false, logger);
  for (const auto &controller : controllers) {
    release_shared_pad(controller.config.address, logger);
  }
  disconnect_all(ports, controllers, logger);
  hide_steam_dongles(false, logger);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    ::signal(SIGPIPE, SIG_IGN);

    const Options options = parse_platform_args(argc, argv);
    const vds::UniqueFd instance_lock =
        acquire_single_instance_lock(kInstanceLockPath);
    vds::Logger logger(options.log_path);
    try {
      return run_daemon(options, logger);
    } catch (const std::exception &error) {
      logger.log(vds::LogScope::Daemon, vds::LogLevel::Error,
                 std::string("fatal: ") + error.what());
      std::cerr << "vdsd: " << error.what() << "\n";
      return 1;
    }
  } catch (const std::exception &error) {
    std::cerr << "vdsd: " << error.what() << "\n";
    return 1;
  }
}
