// BadgeVersion.h — Firmware version string
//
// FIRMWARE_VERSION is normally injected by build.sh via:
//   arduino-cli compile --build-property "compiler.cpp.extra_flags=-DFIRMWARE_VERSION=\"...\""
// The fallback below is used when building outside of build.sh (e.g. Arduino IDE).

#pragma once

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "0.1.2"
#endif
