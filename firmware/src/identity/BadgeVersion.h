// BadgeVersion.h — Firmware version string
//
// FIRMWARE_VERSION is normally injected by build.sh via:
//   arduino-cli compile --build-property "compiler.cpp.extra_flags=-DFIRMWARE_VERSION=\"...\""
// The fallback below is used when building outside of build.sh (e.g. Arduino IDE).

#pragma once

#ifndef FIRMWARE_VERSION
  // Tagged baseline. Bump this manually when cutting a flash batch
  // (or override at build time via -DFIRMWARE_VERSION="..." once the
  // build script wires it up to git describe). Visible on the boot
  // splash bottom-right, the diagnostics screen, and any place that
  // prints fw build info, so a wrong number here is noisy.
  #define FIRMWARE_VERSION "v0.0.0"
#endif
