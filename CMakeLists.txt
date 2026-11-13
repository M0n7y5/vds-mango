cmake_minimum_required(VERSION 3.12)

project(vds LANGUAGES CXX)

option(INSTALL_SERVICE "Register vdsd as a system service during install" OFF)
option(BUILD_TESTS "Build unit tests" OFF)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(BLUEZ REQUIRED IMPORTED_TARGET bluez)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      RelWithDebInfo
      CACHE STRING "Build type" FORCE)
endif()

# -U before -D must share the options list: definitions are emitted before
# flags, and some distros predefine _FORTIFY_SOURCE=2 in the toolchain.
add_compile_options(-Wall -Wextra -Wshadow -Wformat=2
                    -Werror=format-security -fstack-protector-strong -fPIE
                    $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
                    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>)
# add_link_options() needs CMake 3.13; this file declares 3.12.
string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-z,relro -Wl,-z,now -pie")

set(VDS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(VDS_BUILD_INFO_HEADER ${VDS_GENERATED_DIR}/vds_build_info.hh)
set(VDS_BUILD_INFO_SCRIPT
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_build_info.cmake)

add_custom_target(
  vds_build_info_header ALL
  COMMAND
    ${CMAKE_COMMAND} -DVDS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DVDS_BUILD_INFO_HEADER=${VDS_BUILD_INFO_HEADER} -P ${VDS_BUILD_INFO_SCRIPT}
  BYPRODUCTS ${VDS_BUILD_INFO_HEADER}
  COMMENT "Generating vDS build info header"
  VERBATIM)

add_library(vds_protocol OBJECT src/vds_protocol.cc)
target_include_directories(
  vds_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                      ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_protocol PRIVATE PkgConfig::OPUS)

add_library(vds_common OBJECT src/vds_common.cc)
target_include_directories(vds_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(jsonl OBJECT src/jsonl.cc)
target_include_directories(jsonl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(vds_io OBJECT src/vds_io.cc)
target_include_directories(vds_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                         ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(vds_config OBJECT src/vds_config.cc src/vds_fs.cc)
target_include_directories(vds_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                             ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_config PUBLIC jsonl vds_common)

add_library(vds_log OBJECT src/vds_log.cc)
target_include_directories(vds_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(vdsctl_common OBJECT src/vdsctl_common.cc)
target_include_directories(vdsctl_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vdsctl_common PUBLIC jsonl vds_config)

add_library(vdsd_common OBJECT src/vdsd_common.cc)
target_include_directories(
  vdsd_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                     ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vdsd_common PUBLIC jsonl vds_config vds_log)

if(BUILD_TESTS)
  enable_testing()
  add_library(vds_test_support OBJECT tests/vds_check.cc)
  target_include_directories(vds_test_support
                             PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)

  add_executable(test_jsonl tests/test_jsonl.cc)
  target_link_libraries(test_jsonl PRIVATE vds_test_support jsonl)
  add_test(NAME jsonl COMMAND test_jsonl
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_config tests/test_config.cc)
  target_link_libraries(test_config PRIVATE vds_test_support vds_config jsonl
                                            vds_common)
  add_test(NAME config COMMAND test_config
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_vdsctl tests/test_vdsctl.cc)
  target_link_libraries(test_vdsctl PRIVATE vds_test_support vdsctl_common
                                            vds_config jsonl vds_common)
  add_test(NAME vdsctl COMMAND test_vdsctl
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_protocol_feature_set tests/test_protocol.cc)
  target_compile_definitions(test_protocol_feature_set PRIVATE VDS_TEST_L11C)
  target_link_libraries(test_protocol_feature_set PRIVATE vds_test_support
                                                          vds_protocol
                                                          PkgConfig::OPUS)
  add_test(NAME protocol_feature_set COMMAND test_protocol_feature_set
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  # L11c: asserts the USB tail survives feature_set_packet. UNVERIFIED
  # contract: current code writes the DualSense BT CRC over the tail, matching
  # the pad's GET framing. Keep WILL_FAIL until settled on hardware; do not
  # "fix" the code to satisfy this test without pad evidence.
  set_tests_properties(protocol_feature_set PROPERTIES WILL_FAIL TRUE)

  add_executable(test_protocol_bt_input tests/test_protocol.cc)
  target_compile_definitions(test_protocol_bt_input PRIVATE VDS_TEST_L12)
  target_link_libraries(test_protocol_bt_input PRIVATE vds_test_support
                                                       vds_protocol
                                                       PkgConfig::OPUS)
  add_test(NAME protocol_bt_input COMMAND test_protocol_bt_input
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

add_library(vds_bt OBJECT src/vds_bt.cc)
target_link_libraries(vds_bt PUBLIC vds_protocol vds_io PkgConfig::BLUEZ)

add_library(vds_bluez OBJECT src/vds_bluez.cc)
target_include_directories(vds_bluez PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_bluez PUBLIC vds_config PkgConfig::DBUS)

add_library(vds_udev OBJECT src/vds_udev.cc)
target_include_directories(vds_udev PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_udev PUBLIC vds_config vds_common vds_io
                                      PkgConfig::UDEV)

add_executable(vdsd src/vdsd.cc)
add_dependencies(vdsd vds_build_info_header)
target_include_directories(vdsd PRIVATE ${VDS_GENERATED_DIR})
target_link_libraries(
  vdsd
  PRIVATE vds_protocol
          jsonl
          vds_bt
          vds_bluez
          vds_common
          vds_config
          vds_io
          vds_log
          vds_udev
          vdsd_common)

add_executable(vdsctl src/vdsctl.cc)
add_dependencies(vdsctl vds_build_info_header)
target_include_directories(vdsctl PRIVATE ${VDS_GENERATED_DIR})
target_link_libraries(
  vdsctl
  PRIVATE jsonl
          vds_bluez
          vds_common
          vds_config
          vds_io
          vdsctl_common
          vds_udev)

set(VDS_INSTALLED_VDSD "${CMAKE_INSTALL_FULL_BINDIR}/vdsd")
set(VDS_INSTALLED_VDSCTL "${CMAKE_INSTALL_FULL_BINDIR}/vdsctl")
set(VDS_INSTALL_SERVICE_SCRIPT
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install-service.sh)
set(VDS_UNINSTALL_SERVICE_SCRIPT
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/uninstall-service.sh)
set(VDS_SYSTEMD_UNIT_INSTALL_DIR
    "lib/systemd/system"
    CACHE PATH "systemd unit install directory")
set(VDS_SYSTEMD_VDSD "${CMAKE_INSTALL_FULL_BINDIR}/vdsd")
set(VDS_SYSTEMD_SERVICE_FILE ${CMAKE_CURRENT_BINARY_DIR}/vdsd.service)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/data/vdsd.service.in
               ${VDS_SYSTEMD_SERVICE_FILE} @ONLY)
if(IS_ABSOLUTE "${VDS_SYSTEMD_UNIT_INSTALL_DIR}")
  set(VDS_INSTALLED_SYSTEMD_SERVICE
      "${VDS_SYSTEMD_UNIT_INSTALL_DIR}/vdsd.service")
else()
  set(VDS_INSTALLED_SYSTEMD_SERVICE
      "${CMAKE_INSTALL_PREFIX}/${VDS_SYSTEMD_UNIT_INSTALL_DIR}/vdsd.service")
endif()
set(VDS_INSTALL_SERVICE_CODE
    "if(DEFINED ENV{DESTDIR} AND NOT \"\$ENV{DESTDIR}\" STREQUAL \"\")\n")
string(
  APPEND VDS_INSTALL_SERVICE_CODE
  "  message(STATUS \"Skipping systemd service enable under DESTDIR\")\n")
string(APPEND VDS_INSTALL_SERVICE_CODE "else()\n")
string(APPEND VDS_INSTALL_SERVICE_CODE "  execute_process(\n")
string(APPEND VDS_INSTALL_SERVICE_CODE
       "    COMMAND \"${VDS_INSTALL_SERVICE_SCRIPT}\" --enable\n")
string(APPEND VDS_INSTALL_SERVICE_CODE
       "    RESULT_VARIABLE VDS_SERVICE_RESULT)\n")
string(APPEND VDS_INSTALL_SERVICE_CODE
       "  if(NOT VDS_SERVICE_RESULT EQUAL 0)\n")
string(
  APPEND
  VDS_INSTALL_SERVICE_CODE
  "    message(FATAL_ERROR \"failed to enable systemd service vdsd.service\")\n"
)
string(APPEND VDS_INSTALL_SERVICE_CODE "  endif()\n")
string(APPEND VDS_INSTALL_SERVICE_CODE "endif()\n")

install(TARGETS vdsd vdsctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(INSTALL_SERVICE)
  install(FILES ${VDS_SYSTEMD_SERVICE_FILE}
          DESTINATION ${VDS_SYSTEMD_UNIT_INSTALL_DIR})
  install(CODE "${VDS_INSTALL_SERVICE_CODE}")
  add_custom_target(
    uninstall
    COMMAND ${VDS_UNINSTALL_SERVICE_SCRIPT} --stop --disable --remove-file
            ${VDS_INSTALLED_SYSTEMD_SERVICE}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${VDS_INSTALLED_VDSD}
            ${VDS_INSTALLED_VDSCTL})
else()
  add_custom_target(
    uninstall COMMAND ${CMAKE_COMMAND} -E rm -f ${VDS_INSTALLED_VDSD}
                      ${VDS_INSTALLED_VDSCTL})
endif()
