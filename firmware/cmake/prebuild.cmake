# Pre-build code generation steps that the legacy platformio.ini ran via
# `extra_scripts = pre:scripts/...`. CMake counterpart: a custom target that
# the main component depends on.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_BADGE_PROJ_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(_BADGE_GEN_DIR "${CMAKE_CURRENT_LIST_DIR}/../src")

# Read VERSION at configure time so the macros end up in the compile database
# even before the prebuild target runs once.
file(READ "${_BADGE_PROJ_DIR}/VERSION" _BADGE_RAW_VERSION)
string(STRIP "${_BADGE_RAW_VERSION}" BADGE_FIRMWARE_VERSION_STR)
if("${BADGE_FIRMWARE_VERSION_STR}" STREQUAL "")
    set(BADGE_FIRMWARE_VERSION_STR "dev")
endif()
message(STATUS "[badge] firmware version = ${BADGE_FIRMWARE_VERSION_STR}")

# Generate WiFi config + startup-files header now (configure-time) so the
# files exist before component scanning. The Python scripts already support
# standalone invocation.
execute_process(
    COMMAND ${Python3_EXECUTABLE}
            "${_BADGE_PROJ_DIR}/scripts/generate_build_wifi_config.py"
    WORKING_DIRECTORY "${_BADGE_PROJ_DIR}"
    RESULT_VARIABLE _wifi_rc
    OUTPUT_VARIABLE _wifi_out
    ERROR_VARIABLE _wifi_err
)
if(NOT _wifi_rc EQUAL 0)
    message(WARNING "[badge] generate_build_wifi_config.py failed (${_wifi_rc}): ${_wifi_err}")
endif()

execute_process(
    COMMAND ${Python3_EXECUTABLE}
            "${_BADGE_PROJ_DIR}/scripts/generate_startup_files.py"
    WORKING_DIRECTORY "${_BADGE_PROJ_DIR}"
    RESULT_VARIABLE _startup_rc
    OUTPUT_VARIABLE _startup_out
    ERROR_VARIABLE _startup_err
)
if(NOT _startup_rc EQUAL 0)
    message(WARNING "[badge] generate_startup_files.py failed (${_startup_rc}): ${_startup_err}")
endif()

# A no-op target that the project depends on so each subsequent `idf.py build`
# re-runs the script (mtime tracking is inside the scripts themselves).
add_custom_target(badge_prebuild ALL
    COMMAND ${Python3_EXECUTABLE}
            "${_BADGE_PROJ_DIR}/scripts/generate_build_wifi_config.py"
    COMMAND ${Python3_EXECUTABLE}
            "${_BADGE_PROJ_DIR}/scripts/generate_startup_files.py"
    WORKING_DIRECTORY "${_BADGE_PROJ_DIR}"
    COMMENT "[badge] regenerating build-time headers"
    VERBATIM
)
