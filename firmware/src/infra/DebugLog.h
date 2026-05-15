// DebugLog.h — per-category Serial log gates.
//
// High-frequency event prints that spam the serial console (IR
// self-echo drops, boop beacon retx, etc.) are wrapped with the
// macros below so they can be silenced
// from `settings.txt` without recompiling.
//
// Usage:
//   #include "DebugLog.h"
//   LOG_IR("[BadgeIR] self-echo dropped (type=0x%02x w=%u)\n", t, n);
//   DBG("[WiFi] connected ip=%s\n", ip);
//
// Categories cover the noisy modules:
//
//   LOG_IR     → BadgeIR frame-level:
//                self-echo, TX, ACK, NEED, MANIFEST, DATA chunks)
//   LOG_BOOP   → BadgeBoops + BoopFeedback (pairing protocol:
//                beacon lock, retx meta, field TX/RX, recording)
//   LOG_NOTIFY → reserved legacy notification category
//   LOG_ZIGMOJI → reserved legacy zigmoji category
//   LOG_IMU     → IMU samples, orientation thresholds, flip transitions
//   DBG        → general-purpose; suppressed when raw REPL is active
//
// All macros (including per-category LOG_*) are suppressed when the
// MicroPython raw REPL is active. This prevents debug output from
// corrupting the binary framing that JumperIDE / mpremote rely on.
//
// Boot banners, init messages, and error / failure paths keep
// using plain `Serial.printf` — they're rare and always useful,
// and they fire before any IDE connects.
//
// Each macro expands to a `do { if (gate) Serial.printf(...); } while(0)`
// so it's safe to use in `if`/`else` without extra braces, and the
// gate check is one `LDR + CBZ` — effectively free when disabled.

#pragma once

#include <Arduino.h>
#include "BadgeConfig.h"

extern "C" bool mpy_raw_repl_active( void ) __attribute__((weak));

// Master gate: suppress ALL debug output when raw REPL is active so
// we don't corrupt the OK/\x04/\x04 framing that IDEs rely on.
#define _DBG_GATE() (!mpy_raw_repl_active || !mpy_raw_repl_active())

#define DBG(...)         do { if (_DBG_GATE()) Serial.printf(__VA_ARGS__); } while (0)
#define LOG_IR(...)      do { if (_DBG_GATE() && badgeConfig.get(kLogIr))     Serial.printf(__VA_ARGS__); } while (0)
#define LOG_BOOP(...)    do { if (_DBG_GATE() && badgeConfig.get(kLogBoop))   Serial.printf(__VA_ARGS__); } while (0)
#define LOG_NOTIFY(...)  do { if (_DBG_GATE() && badgeConfig.get(kLogNotify)) Serial.printf(__VA_ARGS__); } while (0)
#define LOG_ZIGMOJI(...) do { if (_DBG_GATE() && badgeConfig.get(kLogZigmoji)) Serial.printf(__VA_ARGS__); } while (0)
#define LOG_IMU(...)     do { if (_DBG_GATE() && badgeConfig.get(kLogImu))    Serial.printf(__VA_ARGS__); } while (0)
