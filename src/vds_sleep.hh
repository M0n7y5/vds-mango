// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <optional>
#include <vector>

#include "unique_fd.hh"

struct sd_bus;

namespace vds {

/// Holds system sleep back, through logind, until the caller allows it.
/// vhci_hcd refuses to suspend while a virtual device is plugged in, and the
/// daemon is already frozen by the time the kernel asks.
class SleepGuard {
public:
  enum class Event { Sleeping, Resumed };

  SleepGuard();
  ~SleepGuard();

  SleepGuard(const SleepGuard &) = delete;
  SleepGuard &operator=(const SleepGuard &) = delete;

  int fd() const;
  /// The latest thing logind announced since the last call.
  std::optional<Event> drain();
  /// Lets the announced sleep proceed. The guard holds again after resume.
  void allow_sleep();

private:
  void hold();

  sd_bus *bus_ = nullptr;
  UniqueFd delay_lock_;
  std::vector<Event> events_;
};

} // namespace vds
