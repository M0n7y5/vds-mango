cmake_minimum_required(VERSION 3.16)

project(vds-mango LANGUAGES CXX)

option(INSTALL_SERVICE "Register vdsd as a system service during install" OFF)
option(BUILD_TESTS "Build unit tests" OFF)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network DBus)

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
add_link_options(-Wl,-z,relro -Wl,-z,now -pie)

set(VDS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(VDS_BUILD_INFO_HEADER ${VDS_GENERATED_DIR}/vds_build_info.hh)
set(VDS_BUILD_INFO_SCRIPT
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_build_info.cmake)

add_custom_target(
  vds_build_info_header ALL
  COMMAND
    ${CMAKE_COMMAND} -DVDS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DVDS_BUILD_INFO_HEADER=${VDS_BUILD_INFO_HEADER}
    -DVDS_SETUP_HELPER=${CMAKE_INSTALL_FULL_DATADIR}/vds-mango/setup-helper.sh
    -DVDS_VERSION=${VDS_VERSION}
    -P ${VDS_BUILD_INFO_SCRIPT}
  BYPRODUCTS ${VDS_BUILD_INFO_HEADER}
  COMMENT "Generating vDS build info header"
  VERBATIM)

add_library(vds_protocol OBJECT src/vds_protocol.cc)
target_include_directories(
  vds_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                      ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_protocol PRIVATE PkgConfig::OPUS)

add_library(vds_steam OBJECT src/vds_steam.cc)
target_include_directories(
  vds_steam PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                   ${CMAKE_CURRENT_SOURCE_DIR}/src)

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

add_library(vds_gui_protocol OBJECT src/gui/control_protocol.cc)
target_include_directories(vds_gui_protocol
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/gui)
target_link_libraries(vds_gui_protocol PUBLIC vds_config Qt6::Core)

add_library(vds_gui_lottie OBJECT src/gui/lottie.cc)
target_include_directories(vds_gui_lottie
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/gui)
target_link_libraries(vds_gui_lottie PUBLIC Qt6::Gui)

add_library(vds_usbip OBJECT src/vds_usbip.cc)
target_include_directories(vds_usbip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                            ${CMAKE_CURRENT_SOURCE_DIR}/src)

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
  # the pad's GET framing. Keep WILL_FAIL until settled on hardware. Do not
  # "fix" the code to satisfy this test without pad evidence.
  set_tests_properties(protocol_feature_set PROPERTIES WILL_FAIL TRUE)

  add_executable(test_protocol_bt_input tests/test_protocol.cc)
  target_compile_definitions(test_protocol_bt_input PRIVATE VDS_TEST_L12)
  target_link_libraries(test_protocol_bt_input PRIVATE vds_test_support
                                                       vds_protocol
                                                       PkgConfig::OPUS)
  add_test(NAME protocol_bt_input COMMAND test_protocol_bt_input
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_gui_protocol tests/test_gui_protocol.cc)
  target_link_libraries(
    test_gui_protocol
    PRIVATE vds_test_support
            jsonl
            vds_common
            vds_config
            vds_gui_protocol
            vds_log
            vdsd_common
            Qt6::Core)
  add_test(NAME gui_protocol COMMAND test_gui_protocol
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_usbip tests/test_usbip.cc)
  target_link_libraries(test_usbip PRIVATE vds_test_support vds_usbip)
  add_test(NAME usbip COMMAND test_usbip
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_lottie tests/test_lottie.cc)
  target_link_libraries(test_lottie PRIVATE vds_test_support vds_gui_lottie)
  set(VDS_STARTUP_ANIMATION
      ${CMAKE_CURRENT_SOURCE_DIR}/data/animations/startup.json)
  target_compile_definitions(
    test_lottie PRIVATE VDS_STARTUP_ANIMATION="${VDS_STARTUP_ANIMATION}")
  add_test(NAME lottie COMMAND test_lottie
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_control_client tests/test_control_client.cc
                                     src/gui/control_client.cc)
  target_include_directories(test_control_client
                             PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gui)
  target_link_libraries(test_control_client PRIVATE vds_test_support
                                                    Qt6::Core Qt6::Network)
  add_test(NAME control_client COMMAND test_control_client
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_single_instance tests/test_single_instance.cc
                                      src/gui/single_instance.cc)
  target_include_directories(test_single_instance
                             PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gui)
  target_link_libraries(test_single_instance PRIVATE vds_test_support
                                                     Qt6::Core Qt6::Network)
  add_test(NAME single_instance COMMAND test_single_instance
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_tray_model tests/test_tray_model.cc
                                 src/gui/tray_model.cc)
  target_include_directories(
    test_tray_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                            ${CMAKE_CURRENT_SOURCE_DIR}/src
                            ${CMAKE_CURRENT_SOURCE_DIR}/src/gui)
  target_link_libraries(test_tray_model PRIVATE vds_test_support Qt6::Core)
  add_test(NAME tray_model COMMAND test_tray_model
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_steam tests/test_steam.cc)
  target_link_libraries(test_steam PRIVATE vds_test_support vds_steam)
  add_test(NAME steam COMMAND test_steam
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

add_library(vds_bt OBJECT src/vds_bt.cc)
target_link_libraries(vds_bt PUBLIC vds_protocol)

add_library(vds_steam_pad OBJECT src/vds_steam_pad.cc)
target_link_libraries(vds_steam_pad PUBLIC vds_steam)

add_library(vds_bluez OBJECT src/vds_bluez.cc)
target_include_directories(vds_bluez PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_bluez PUBLIC vds_config PkgConfig::DBUS)

add_library(vds_udev OBJECT src/vds_udev.cc)
target_include_directories(vds_udev PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_udev PUBLIC vds_config vds_common vds_io
                                      PkgConfig::UDEV)

add_executable(vdsd src/vdsd.cc src/vds_sleep.cc)
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
          vds_steam
          vds_steam_pad
          vds_udev
          vds_usbip
          vdsd_common
          PkgConfig::SYSTEMD)

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

add_executable(
  vds-mango
  src/gui/vds_mango.cc
  src/gui/bluez.cc
  src/gui/control_client.cc
  src/gui/controller_card.cc
  src/gui/controller_dialog.cc
  src/gui/diagnostics_dialog.cc
  src/gui/main_window.cc
  src/gui/setup_helper.cc
  src/gui/setup_wizard.cc
  src/gui/single_instance.cc
  src/gui/startup_animation.cc
  src/gui/tray_model.cc)
add_dependencies(vds-mango vds_build_info_header)
target_include_directories(vds-mango PRIVATE ${VDS_GENERATED_DIR})
target_link_libraries(
  vds-mango
  PRIVATE jsonl
          vds_common
          vds_config
          vds_gui_lottie
          vds_gui_protocol
          Qt6::DBus
          Qt6::Network
          Qt6::Widgets)
qt_add_resources(
  vds-mango "icons" PREFIX "/icons" BASE "data/icons"
  FILES data/icons/vds-mango.svg data/icons/dualsense.svg
        data/icons/dualsense-edge.svg data/icons/headset.svg
        data/icons/microphone.svg data/icons/microphone-muted.svg
        data/icons/steam-controller.svg)
qt_add_resources(
  vds-mango "animations" PREFIX "/animations" BASE "data/animations"
  FILES data/animations/startup.json)

set(VDS_POLKIT_POLICY
    ${CMAKE_CURRENT_BINARY_DIR}/io.github.m0n7y5.vds-mango.policy)
set(VDS_SETUP_HELPER "${CMAKE_INSTALL_FULL_DATADIR}/vds-mango/setup-helper.sh")
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/data/io.github.m0n7y5.vds-mango.policy.in
  ${VDS_POLKIT_POLICY} @ONLY)
set(VDS_INSTALLED_FILES
    "${CMAKE_INSTALL_FULL_BINDIR}/vdsd"
    "${CMAKE_INSTALL_FULL_BINDIR}/vdsctl"
    "${CMAKE_INSTALL_FULL_BINDIR}/vds-mango"
    "${CMAKE_INSTALL_FULL_DATADIR}/applications/vds-mango.desktop"
    "${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/scalable/apps/vds-mango.svg"
    "${CMAKE_INSTALL_FULL_DATADIR}/polkit-1/actions/io.github.m0n7y5.vds-mango.policy"
    "${CMAKE_INSTALL_FULL_DATADIR}/vds-mango/setup-helper.sh"
    "${CMAKE_INSTALL_FULL_DATADIR}/wireplumber/wireplumber.conf.d/99-vds-dualsense-wireplumber.conf"
)
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

install(TARGETS vdsd vdsctl vds-mango
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/vds-mango.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/icons/vds-mango.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ${VDS_POLKIT_POLICY}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
install(PROGRAMS scripts/setup-helper.sh
        DESTINATION ${CMAKE_INSTALL_DATADIR}/vds-mango)
install(FILES data/99-vds-dualsense-wireplumber.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/wireplumber/wireplumber.conf.d)

if(INSTALL_SERVICE)
  install(FILES ${VDS_SYSTEMD_SERVICE_FILE}
          DESTINATION ${VDS_SYSTEMD_UNIT_INSTALL_DIR})
  install(CODE "${VDS_INSTALL_SERVICE_CODE}")
  add_custom_target(
    uninstall
    COMMAND ${VDS_UNINSTALL_SERVICE_SCRIPT} --stop --disable --remove-file
            ${VDS_INSTALLED_SYSTEMD_SERVICE}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${VDS_INSTALLED_FILES})
else()
  add_custom_target(uninstall COMMAND ${CMAKE_COMMAND} -E rm -f
                                      ${VDS_INSTALLED_FILES})
endif()
