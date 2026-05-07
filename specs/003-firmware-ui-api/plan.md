# Implementation Plan: Firmware UI & API Integration

**Branch**: `003-firmware-ui-api` | **Date**: 2026-03-10 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/003-firmware-ui-api/spec.md`

## Summary

Five user stories spanning flash tool reliability, hardware UI verification, WiFi boot path, badge API client, and live nametag display. The largest new artifact is `badge_api.py` — a self-contained HTTP client module with a single transport function and named endpoint functions. Supporting changes: `flash.sh` interrupt and confirmation fixes, `boot.py` refactored to use `badge_api`, `boot.py` sleep loops broken into Ctrl+C-interruptible chunks, and a firmware rebuild to bake in the `_boot.py` auto-format fix.

## Technical Context

**Language/Version**: MicroPython v1.24.0 (Python 3.4 subset)
**Primary Dependencies**: `ssd1306` (frozen), `urequests` (frozen), `machine` (built-in), `esp32` (built-in), `mpremote` (dev toolchain)
**Storage**: VFS FAT partition on 8 MB QIO flash (4 MB factory app + 4 MB VFS)
**Testing**: Manual only — flash → REPL → observe boot → exercise inputs; no automated runner
**Target Platform**: ESP32-S3-MINI-1 running MicroPython v1.24.0, board `TEMPORAL_BADGE_DELTA`
**Project Type**: Embedded firmware (MicroPython VFS application)
**Performance Goals**: Display refresh ≤ 50 ms per frame; WiFi connect within `WIFI_TIMEOUT_MS` (5 s default); API calls complete within reasonable wall time on conference WiFi
**Constraints**: ≤ 240 KB available heap on device; no threads for HTTP (single-threaded urequests); flash.sh --vfs-only must complete in < 60 s
**Scale/Scope**: ~2,000 units; 5 VFS Python files modified; 1 new file (badge_api.py); 1 firmware rebuild

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Check | Notes |
|-----------|-------|-------|
| **I. MicroPython-First** | ✅ PASS | All new code is Python. No new C required. |
| **II. Firmware-0306 Parity** | ✅ PASS | Boot sequence (splash → WiFi → QR → nametag → main loop) preserved. Nametag source changes from raw server XBM to JSON-parsed text — this is a bugfix not a deviation (the /info endpoint returns JSON, not XBM). IR pairing unchanged. |
| **III. Credentials-at-Build** | ✅ PASS | `badge_api.py` reads `SERVER_URL` from `creds` module (VFS prototyping path). No credentials hardcoded in module. |
| **IV. Backend Contract Compliance** | ✅ PASS | All five endpoint functions match the API table in spec. QR generated on-device via `uQR.py` (no `/qr.xbm` call). HMAC insertion point designed into transport layer. |
| **V. Reproducible Build** | ✅ PASS | Firmware rebuild uses existing `build.sh` with pinned MicroPython v1.24.0 + ESP-IDF v5.2.3. No version changes. |
| **VI. Hardware Safety** | ✅ PASS | No GPIO changes. `pins.csv` unchanged. No eFuse operations in this spec. |

**Gate result: PASS — proceed to Phase 0.**

## Project Structure

### Documentation (this feature)

```text
specs/003-firmware-ui-api/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/
│   └── badge_api.md     # Phase 1 output — badge_api.py public interface
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
firmware/micropython-build/
├── badge_api.py         # NEW — HTTP client, transport layer, 5 endpoint functions
├── boot.py              # MODIFY — use badge_api; break sleeps into loops; JSON nametag
├── flash.sh             # MODIFY — fix echo-before-copy; remove VFS mount exec; per-file confirm
├── config.py            # MODIFY if needed — remove BYPASS/BYPASS_DELAY_MS now config is cleaned
├── graphics.py          # MODIFY if needed — display functions for nametag layout
├── main.py              # MODIFY if needed — fix any hardware UI issues found during verification
├── badge_sdk.py         # MODIFY if needed — align uid property naming, wire to badge_api
├── ir_nec.py            # MODIFY if needed
└── uQR.py               # MODIFY if needed
```

**Structure Decision**: Single embedded firmware directory. No backend changes (deferred). No test subdirectory (manual-only verification).

## Complexity Tracking

> No constitution violations — section intentionally empty.
