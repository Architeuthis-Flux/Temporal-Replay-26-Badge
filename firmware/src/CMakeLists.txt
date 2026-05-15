#
# ESP-IDF "main" component for the Temporal Badge firmware.
#
# This file lives at firmware/src/ (not firmware/main/) because PlatformIO's
# espidf framework defaults PROJECT_SRC_DIR to ${PROJECT_DIR}/src and looks
# for an `__idf_src` target there. ESP-IDF's idf.py is happy to use any
# directory as the main component as long as it's discoverable; we set
# COMPONENT_DIRS in firmware/CMakeLists.txt accordingly.
#
# Mirrors the [base] / [env:echo-dev] section of platformio.ini:
#   build_src_filter = +<*> -<JumperlOSstuff/> -<modular/> -<ble/>
#                      +<../micropython/usermods/temporalbadge/>
#   board_build.embed_files = ../data/out/bundle.bin
#

set(BADGE_SRC_ROOT "${CMAKE_CURRENT_LIST_DIR}")

# NOTE: cannot use CONFIGURE_DEPENDS — IDF runs this CMakeLists.txt in CMake
# script mode during component-requirements scanning, which forbids it.
file(GLOB_RECURSE _badge_all_sources
    "${BADGE_SRC_ROOT}/*.c"
    "${BADGE_SRC_ROOT}/*.cpp"
    "${BADGE_SRC_ROOT}/*.cc"
    "${BADGE_SRC_ROOT}/*.S"
)

set(_badge_excluded_patterns
    "/JumperlOSstuff/"
    "/modular/"
    "/ble/"
)

set(_badge_sources)
foreach(_src ${_badge_all_sources})
    set(_keep TRUE)
    foreach(_excl ${_badge_excluded_patterns})
        if(_src MATCHES "${_excl}")
            set(_keep FALSE)
            break()
        endif()
    endforeach()
    if(_keep)
        list(APPEND _badge_sources "${_src}")
    endif()
endforeach()

# Bring the temporalbadge MicroPython usermod into this component itself.
# Its `temporalbadge_hal_*` shims call `temporalbadge_runtime_*` functions
# that are implemented in src/micropython/badge_mp_api/* (already in this
# component), so registering the usermod as a separate component creates a
# circular dependency that ESP-IDF's link-group cannot always resolve.
# Compiling them all into the same archive sidesteps the issue.
set(_badge_usermod_root "${CMAKE_CURRENT_LIST_DIR}/../micropython/usermods/temporalbadge")
list(APPEND _badge_sources
    "${_badge_usermod_root}/modtemporalbadge.c"
    "${_badge_usermod_root}/temporalbadge_hal.c"
    "${_badge_usermod_root}/temporalbadge_runtime.cpp"
)

# Every subdirectory under src/ should also act as an include search root,
# because the existing code does e.g. `#include "hardware/HardwareConfig.h"`
# AND `#include "esp32-hal-gpio.h"` (arduino-esp32 header) interchangeably.
# IDF 5.5's bt component does not expose esp_bt.h for esp32s3 through its
# CMake (it omits include/esp32s3/include while pioarduino used to vendor a
# c3-aliased copy). src/hardware/Power.cpp:15 still includes esp_bt.h to call
# esp_bt_mem_release(), so re-add the c3 header path manually — the public API
# the badge uses is identical across c3/s3.
set(_badge_includes
    "$ENV{IDF_PATH}/components/bt/include/esp32c3/include"
    "${BADGE_SRC_ROOT}"
    "${BADGE_SRC_ROOT}/api"
    "${BADGE_SRC_ROOT}/boops"
    "${BADGE_SRC_ROOT}/doom"
    "${BADGE_SRC_ROOT}/hardware"
    "${BADGE_SRC_ROOT}/hardware/battery"
    "${BADGE_SRC_ROOT}/hw"
    "${BADGE_SRC_ROOT}/hw/ir"
    "${BADGE_SRC_ROOT}/identity"
    "${BADGE_SRC_ROOT}/infra"
    "${BADGE_SRC_ROOT}/ir"
    "${BADGE_SRC_ROOT}/led"
    "${BADGE_SRC_ROOT}/messaging"
    "${BADGE_SRC_ROOT}/micropython"
    "${BADGE_SRC_ROOT}/micropython/badge_mp_api"
    "${BADGE_SRC_ROOT}/screens"
    "${BADGE_SRC_ROOT}/ui"
    "${CMAKE_CURRENT_LIST_DIR}/../micropython/usermods/temporalbadge"
)

idf_component_register(
    SRCS ${_badge_sources}
    INCLUDE_DIRS ${_badge_includes}
    REQUIRES
        # arduino-esp32 surface
        arduino
        # vendored Arduino libs
        u8g2
        ArduinoJson
        Adafruit_BusIO
        Adafruit_GFX
        Adafruit_IS31FL3731
        SparkFun_LIS2DH12
        Adafruit_MAX1704X
        # in-tree heavy components
        doomgeneric
        micropython_embed
    PRIV_REQUIRES
        # ESP-IDF subsystems main.cpp / submodules pull in directly
        bt
        nvs_flash
        esp_event
        esp_netif
        esp_wifi
        esp_http_client
        esp_http_server
        esp_https_ota
        esp_psram
        esp_pm
        esp_adc
        esp_partition
        esp_timer
        esp_app_format
        app_update
        driver
        spi_flash
        bootloader_support
        fatfs
        wear_levelling
        mbedtls
        esp-tls
        json
        espressif__qrcode
        espressif__libsodium
        espressif__mdns
)

# Build-flag macros that platformio.ini injected globally for env:echo-dev.
# Kconfig isn't the right home for these (they are not real Kconfig options
# and arduino-esp32 expects them as raw -D defines).
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    HARDWARE_ECHO=1
    BADGE_HAS_DOOM=1
    FEATURE_SOUND=1
    BADGE_DEV_MENU=1
    BADGE_BYPASS_PAIRING=1
    BADGE_ALLOW_BADGE_INFO_EDIT=1
    BADGE_ENABLE_MP_DEV=1
    RELOAD_FILESYSTEM=1
    NDEBUG=1
    ARDUINO_USB_CDC_ON_BOOT=1
    ARDUINO_USB_MODE=1
    BOARD_HAS_PSRAM=1
    "FFCONF_H=\"ffconf.h\""
    "FIRMWARE_VERSION=\"${BADGE_FIRMWARE_VERSION_STR}\""
    "BADGE_FIRMWARE_VERSION=\"${BADGE_FIRMWARE_VERSION_STR}\""
    DOOMGENERIC_RESX=160
    DOOMGENERIC_RESY=100
)

# Embed the data bundle. IDF's BINARY mode generates symbols
# `_binary_bundle_bin_start` / `_binary_bundle_bin_end` — exactly what
# src/api/DataCache.cpp's `extern ... asm("_binary_bundle_bin_start")` expects,
# so the legacy `normalize_bundle_symbols.py` post-action becomes unnecessary.
target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../../data/out/bundle.bin"
    BINARY)

# Tame third-party + heavy-handed warnings the legacy PlatformIO build also
# tolerated (it set Werror only on the application code; arduino-esp32
# enables -Werror=all globally for ESP-IDF builds).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=misleading-indentation
    -Wno-error=narrowing
    -Wno-error=write-strings
    -Wno-error=format
    -Wno-error=sign-compare
    -Wno-error=unused-variable
    -Wno-error=unused-function
    -Wno-error=unused-but-set-variable
    -Wno-error=maybe-uninitialized
    -Wno-error=int-in-bool-context
    -Wno-error=missing-field-initializers
    -Wno-error=format-truncation
    -Wno-error=stringop-truncation
    -Wno-error=comment
    -Wno-error=unknown-pragmas
    -Wno-error=ignored-qualifiers
    -Wno-error=parentheses
    -Wno-error=array-bounds
    -Wno-error=stringop-overflow
    -Wno-error=stringop-overread
    -Wno-error=address
    -Wno-error=type-limits
    -Wno-error=switch-unreachable
    -Wno-error=overloaded-virtual
    -Wno-error=reorder
    -Wno-error=class-memaccess
    -Wno-error=deprecated-enum-enum-conversion
    -Wno-error=deprecated-enum-float-conversion
)
