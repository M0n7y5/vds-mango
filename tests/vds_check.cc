// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_check.hh"

#include <cstdio>

namespace vds::test {

void report(const char *file, int line, const std::string &what) {
  ++g_failures;
  std::fprintf(stderr, "%s:%d: %s\n", file, line, what.c_str());
}

} // namespace vds::test

int main() {
  vds::test::run_tests();
  return vds::test::g_failures != 0;
}
