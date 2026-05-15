cmake_minimum_required(VERSION 3.16.0)

# Native ESP-IDF build for the Temporal Badge firmware.
#
# Replaces (in parallel with) the legacy `pio run -e echo-dev` flow.
# Drive with:
#   . $IDF_PATH/export.sh
#   idf.py set-target esp32s3
#   idf.py build
#
# Default profile is echo-dev (matches platformio.ini default_envs).
# Sdkconfig layering:  sdkconfig.defaults  +  sdkconfig.defaults.${BADGE_PROFILE}

set(BADGE_PROFILE "echo-dev" CACHE STRING "Badge build profile (echo-dev, echo, charlie, delta)")

set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.${BADGE_PROFILE}"
)

# Everything lives under firmware/src/ — both the legacy pioarduino build
# (framework = arduino, SCons-driven, src_dir = src) and the native IDF /
# pio espidf build use it as the source root. There is no separate
# firmware/main/ directory.
#
# IDF's project.cmake conventionally expects a "main" component at
# ${PROJECT_DIR}/main; we override that by:
#   - Setting EXTRA_COMPONENT_DIRS to include src/, so IDF discovers a
#     component named "src" there.
#   - Re-pointing IDF's main-component lookup at src/ via MAIN_SRCS_DIR
#     (overrides the implicit ${PROJECT_DIR}/main path).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/components"
)
# Tell IDF the main component is at src/ rather than the default main/. Used
# by tools/cmake/project.cmake when computing the main component's path.
set(MAIN_SRCS_DIR "${CMAKE_CURRENT_LIST_DIR}/src")

# Pre-build script outputs (BadgeVersion.generated.h, BuildWifiConfig.generated.h,
# StartupFilesData.h) are produced by `firmware/cmake/prebuild.cmake` and
# referenced from the main component.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/prebuild.cmake")

# arduino-esp32's HWCDC.cpp gates HWCDCSerial on ARDUINO_USB_MODE &&
# ARDUINO_USB_CDC_ON_BOOT, and main.cpp's `Serial` macro expands to
# HWCDCSerial when both are 1. These macros must be visible during the
# arduino component's compile too, so they are added project-wide via
# add_compile_options BEFORE project() so that arduino's CMakeLists picks
# them up when its sources are compiled.
add_compile_options(-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1 -DBOARD_HAS_PSRAM=1 -DNDEBUG=1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firmware C CXX ASM)

# doomgeneric's dummy.c re-defines I_InitTimidityConfig (also defined in
# firmware/src/doom/doom_sound.cpp). Allow duplicate symbols at link time so
# the doom_sound override wins without having to fork dummy.c.
target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE -Wl,--allow-multiple-definition)

# Make the prebuild target a hard prerequisite of the main component build.
add_dependencies(${CMAKE_PROJECT_NAME}.elf badge_prebuild)
