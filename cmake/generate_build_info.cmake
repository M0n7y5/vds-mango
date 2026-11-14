if(NOT DEFINED VDS_SOURCE_DIR
   OR NOT DEFINED VDS_BUILD_INFO_HEADER
   OR NOT DEFINED VDS_SETUP_HELPER)
  message(FATAL_ERROR "missing vDS build info arguments")
endif()

execute_process(
  COMMAND "${VDS_SOURCE_DIR}/scripts/generate-version.sh"
  OUTPUT_VARIABLE VDS_VERSION
  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(VDS_VERSION STREQUAL "")
  set(VDS_VERSION unknown)
endif()

string(TIMESTAMP VDS_BUILD_YEAR "%Y")
get_filename_component(VDS_BUILD_INFO_DIR "${VDS_BUILD_INFO_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${VDS_BUILD_INFO_DIR}")

set(VDS_BUILD_INFO_TMP "${VDS_BUILD_INFO_HEADER}.tmp")
file(
  WRITE "${VDS_BUILD_INFO_TMP}"
  "#pragma once\n\n"
  "namespace vds {\n\n"
  "inline constexpr const char *kVersion = \"${VDS_VERSION}\";\n"
  "inline constexpr const char *kBuildYear = \"${VDS_BUILD_YEAR}\";\n"
  "inline constexpr const char *kSetupHelperPath = \"${VDS_SETUP_HELPER}\";\n\n"
  "} // namespace vds\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${VDS_BUILD_INFO_TMP}" "${VDS_BUILD_INFO_HEADER}")
file(REMOVE "${VDS_BUILD_INFO_TMP}")
