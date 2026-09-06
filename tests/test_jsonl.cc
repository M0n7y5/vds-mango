// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_check.hh"

#include <cstddef>
#include <string>
#include <string_view>

#include "jsonl.hh"

namespace vds::test {

void test_duplicate_keys_keep_both_fields() {
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

void test_true_literal_parses_as_a_bool() {
  const auto fields = vds::parse_jsonl_object("{\"OK\":true}", "test_jsonl");
  VDS_CHECK_EQ(fields.size(), static_cast<std::size_t>(1));
  VDS_CHECK_EQ(fields[0].value.is_bool, true);
  VDS_CHECK_EQ(fields[0].value.bool_value, true);
  VDS_CHECK_EQ(vds::require_jsonl_bool(fields, "OK", "test_jsonl"), true);
}

void test_false_literal_parses_as_a_bool() {
  const auto fields = vds::parse_jsonl_object("{\"OK\":false}", "test_jsonl");
  VDS_CHECK_EQ(fields.size(), static_cast<std::size_t>(1));
  VDS_CHECK_EQ(fields[0].value.is_bool, true);
  VDS_CHECK_EQ(fields[0].value.bool_value, false);
  VDS_CHECK_EQ(vds::require_jsonl_bool(fields, "OK", "test_jsonl"), false);
}

void test_truncated_true_literal_is_rejected() {
  VDS_CHECK_THROWS(vds::parse_jsonl_object("{\"OK\":tru}", "test_jsonl"));
}

void test_require_bool_rejects_a_quoted_bool() {
  const auto fields =
      vds::parse_jsonl_object("{\"OK\":\"true\"}", "test_jsonl");
  VDS_CHECK_THROWS(vds::require_jsonl_bool(fields, "OK", "test_jsonl"));
}

void run_tests() {
  test_duplicate_keys_keep_both_fields();
  test_true_literal_parses_as_a_bool();
  test_false_literal_parses_as_a_bool();
  test_truncated_true_literal_is_rejected();
  test_require_bool_rejects_a_quoted_bool();
}

} // namespace vds::test
