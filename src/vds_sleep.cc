// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <systemd/sd-bus.h>

#include "vds_sleep.hh"

namespace vds {

namespace {

constexpr const char *kLogind = "org.freedesktop.login1";
constexpr const char *kLogindPath = "/org/freedesktop/login1";
constexpr const char *kLogindManager = "org.freedesktop.login1.Manager";
/*
 * sd-bus defaults to 25 s, which would stall the single-threaded event loop
 * for that long when logind is wedged.
 */
constexpr std::uint64_t kCallTimeoutUs = 2000000;

[[noreturn]] void fail(const std::string &what, int error) {
  throw std::runtime_error(what + ": " + std::strerror(-error));
}

int on_prepare_for_sleep(sd_bus_message *message, void *userdata,
                         sd_bus_error *) {
  int sleeping = 0;
  if (::sd_bus_message_read(message, "b", &sleeping) >= 0) {
    static_cast<std::vector<SleepGuard::Event> *>(userdata)->push_back(
        sleeping ? SleepGuard::Event::Sleeping : SleepGuard::Event::Resumed);
  }
  return 0;
}

} // namespace

SleepGuard::SleepGuard() {
  int result = ::sd_bus_open_system(&bus_);
  if (result < 0) {
    fail("failed to connect to the system bus", result);
  }
  (void)::sd_bus_set_method_call_timeout(bus_, kCallTimeoutUs);
  result = ::sd_bus_match_signal(bus_, nullptr, kLogind, kLogindPath,
                                 kLogindManager, "PrepareForSleep",
                                 on_prepare_for_sleep, &events_);
  if (result < 0) {
    ::sd_bus_unref(bus_);
    fail("failed to subscribe to logind sleep announcements", result);
  }
  try {
    hold();
  } catch (...) {
    ::sd_bus_unref(bus_);
    throw;
  }
}

SleepGuard::~SleepGuard() { ::sd_bus_unref(bus_); }

int SleepGuard::fd() const { return ::sd_bus_get_fd(bus_); }

std::optional<SleepGuard::Event> SleepGuard::drain() {
  while (true) {
    const int result = ::sd_bus_process(bus_, nullptr);
    if (result < 0) {
      fail("system bus connection lost", result);
    }
    if (result == 0) {
      break;
    }
  }
  if (events_.empty()) {
    return std::nullopt;
  }
  const Event latest = events_.back();
  events_.clear();
  if (latest == Event::Resumed) {
    hold();
  }
  return latest;
}

void SleepGuard::allow_sleep() { delay_lock_.reset(); }

void SleepGuard::hold() {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = nullptr;
  const int result = ::sd_bus_call_method(
      bus_, kLogind, kLogindPath, kLogindManager, "Inhibit", &error, &reply,
      "ssss", "sleep", "vDS Mango", "Unplugging virtual controllers", "delay");
  if (result < 0) {
    const std::string detail = error.message ? error.message : "";
    ::sd_bus_error_free(&error);
    fail("logind refused the sleep delay lock " + detail, result);
  }
  int lock = -1;
  const int read = ::sd_bus_message_read(reply, "h", &lock);
  // The descriptor belongs to the message.
  delay_lock_ = UniqueFd(read >= 0 ? ::fcntl(lock, F_DUPFD_CLOEXEC, 3) : -1);
  ::sd_bus_message_unref(reply);
  if (!delay_lock_) {
    throw std::runtime_error("logind returned no sleep delay lock");
  }
}

} // namespace vds
