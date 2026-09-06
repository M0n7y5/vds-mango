// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_check.hh"

#include <exception>
#include <string_view>

#include "vds_config.hh"

namespace vds::test {

void run_tests() {
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

} // namespace vds::test
