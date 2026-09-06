// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_check.hh"

#include <string>
#include <string_view>

#include "jsonl.hh"

namespace vds::test {

void run_tests() {
  // Duplicate keys are silently accepted. A later reject-duplicates change
  // must be deliberate; require_jsonl_string keeps the first address.
  constexpr std::string_view kLine =
      "{\"address\":\"aa:bb:cc:dd:ee:01\","
      "\"address\":\"aa:bb:cc:dd:ee:02\","
      "\"profile\":\"\",\"ports\":[]}";
  const auto fields = vds::parse_jsonl_object(kLine, "test_jsonl");
  VDS_CHECK_EQ(fields.size(), static_cast<std::size_t>(4));
  VDS_CHECK_EQ(fields[0].key, std::string("address"));
  VDS_CHECK_EQ(fields[0].value.string_value,
               std::string("aa:bb:cc:dd:ee:01"));
}

} // namespace vds::test
