// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <cstdio>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>

namespace vds::test {

inline int g_failures = 0;

void report(const char *file, int line, const std::string &what);
void run_tests();

template <class T> void append_value(std::ostream &out, const T &value) {
  using U = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<U, bool>) {
    out << (value ? "true" : "false");
  } else if constexpr (std::is_same_v<U, std::uint8_t>) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02x", static_cast<unsigned>(value));
    out << buf;
  } else {
    out << value;
  }
}

template <class A, class B>
void check_eq(const char *file, int line, const char *expr, const A &left,
              const B &right) {
  if (left == right) {
    return;
  }
  std::ostringstream what;
  what << expr << "  left=";
  append_value(what, left);
  what << " right=";
  append_value(what, right);
  report(file, line, what.str());
}

} // namespace vds::test

#define VDS_CHECK(cond)                                                        \
  do {                                                                         \
    if (!(cond)) {                                                             \
      vds::test::report(__FILE__, __LINE__, #cond);                            \
    }                                                                          \
  } while (0)

#define VDS_CHECK_EQ(a, b)                                                     \
  do {                                                                         \
    vds::test::check_eq(__FILE__, __LINE__, #a " == " #b, (a), (b));           \
  } while (0)

#define VDS_CHECK_THROWS(expr)                                                 \
  do {                                                                         \
    try {                                                                      \
      (void)(expr);                                                            \
      vds::test::report(__FILE__, __LINE__, "expected throw: " #expr);         \
    } catch (...) {                                                            \
    }                                                                          \
  } while (0)
