# Research: Badge Core Primitives Validation & Integration

**Feature**: `008-badge-core-primitives`
**Date**: 2026-03-18
**Status**: Complete — all NEEDS CLARIFICATION resolved

---

## Decision 1 — `badge.ping()` maps to existing `probeBadgeExistence`, not to `/api/v1/pings`

**Decision**: `badge.ping()` is implemented as a Python wrapper around the existing C++
`BadgeAPI::probeBadgeExistence(uid)` function, which issues a `GET /api/v1/badge/{uid}/info`
request and returns `ok` (bool) + `httpCode`. The Python binding returns `True` on HTTP 200,
`False` on any other result.

**Rationale**: The spec and assumptions explicitly define ping as "a non-mutating HTTP probe
to the backend that confirms network reachability — not an ICMP ping or IR broadcast." The
constitution's `/api/v1/pings` endpoint is the **social interaction ping** feature (directed
async message via POST), which is a separate product feature. `probeBadgeExistence` already
performs exactly the right connectivity probe against an auth-exempt endpoint — no new backend
work needed.

**Alternatives considered**:
- Map to `POST /api/v1/pings` — rejected: requires HMAC auth, is a stateful social feature,
  not a connectivity probe.
- Add a dedicated `/health` or `/ping` backend endpoint — rejected: unnecessary; the badge
  info endpoint already serves as a lightweight reachability check with no side effects.
- Use `badge.http_get(badge.server_url() + "/api/v1/badge/" + badge.uid() + "/info")` from
  Python — rejected: app developers shouldn't construct internal URLs; the primitive should
  be opaque.

---

## Decision 2 — Tilt-to-nametag requires an explicit `MODE_MENU` guard (documented Principle II deviation)

**Decision**: `pollTilt()` in `BadgeInput.cpp` must gate tilt activation behind
`renderMode == MODE_MENU`. Currently, `pollTilt()` fires on every `loop()` iteration when
`!modalActive`, meaning it can activate the nametag during MODE_INPUT_TEST or MODE_QR.

**Evidence**: `pollTilt()` has no `renderMode` check. The Python app case is incidentally safe
(loop() is blocked during `BadgePython::execApp()`), but input test and QR modes are not.

**Deviation from Principle II (Firmware-0308 Behavioral Source)**: Firmware-0308 has the
same ungated tilt behavior because it had no Python runtime and no input-test mode. The
guard is a documented intentional improvement for spec-008. This deviation is justified —
it prevents nametag flicker during active non-menu screens — and must be noted in the
commit message per constitution governance.

**Alternatives considered**:
- Accept ungated tilt — rejected: spec FR-004 is explicit ("MUST NOT activate while a Python
  app or any other non-menu screen is active").
- Gate only on Python active — rejected: doesn't cover input-test or QR modes; spec covers
  all non-menu states.

---

## Decision 3 — QR enrollment backend dependency: deferred, existing flow is correct

**Decision**: The QR→enrollment→paired transition is already correctly implemented in
firmware. `pollQRPairing()` polls `GET /api/v1/badge/{uid}/info` every `POLL_INTERVAL_MS`
via a FreeRTOS probe task on Core 0. HTTP 200 triggers `fetchBadgeXBM()` and transitions
to `BADGE_PAIRED`. No firmware changes needed for this flow.

The backend enrollment endpoint (`POST /api/v1/badges/enroll` or equivalent) is a backend
dependency out of scope for firmware work. Per project policy, backend changes wait until
firmware is building and tested. The existing `POST /api/v1/link-user-to-badge` endpoint
in `registrationScanner/routes/registration.py` is the likely implementation point.

**What needs to be added to backend (out of scope for this spec)**:
- Either `POST /api/v1/badges/enroll` as a new endpoint, or confirm that
  `POST /api/v1/link-user-to-badge` is the enrollment trigger
- Once a badge UUID is linked, `GET /api/v1/badge/{uid}/info` must return HTTP 200

**Alternatives considered**:
- Add a new enrollment endpoint in firmware scope — rejected: user preference is firmware first.
- Change polling to use a different endpoint — rejected: existing logic is correct and complete.

---

## Decision 4 — IR boop primitives: no firmware changes needed

**Decision**: The IR boop flow is fully implemented in `BadgeIR.cpp` with a 10-state
phase machine (`IDLE → LISTENING → SENDING → WAITING → INCOMING → PAIR_CONSENT →
PAIRED_OK / PAIR_FAILED / PAIR_IGNORED / PAIR_CANCELLED`). HTTP POST to `/api/v1/boops`
is implemented in `BadgeAPI::createBoop()`. Partner name display is handled in BadgeIR.cpp.

The Python `badge.ir_send()` and `badge.ir_available()` / `badge.ir_read()` are already
present; `ir_available()` and `ir_read()` remain stubs (v1 limitation — IR frames are not
queued to Python). This is acceptable for spec-008 because the boop use case is
firmware-level, not Python-app-level.

**Alternatives considered**:
- Queue IR frames to Python — deferred to a dedicated launcher/IR spec; out of scope.

---

## Decision 5 — Dead code scope: scoped to `firmware/Firmware-0308-modular/` only

**Decision**: Dead code removal is scoped to the active firmware directory per spec
assumption. No changes to `firmware/Firmware-0308/` (behavioral reference), `micropython-build/`
(archived), or `registrationScanner/`.

Candidates for removal/audit during implementation:
- Any `BadgeTest.cpp` stubs for removed functionality
- Stale comments referencing `flash.sh` (replaced by `flash.py`)
- Any unused `#include` or `#define` in headers

---

## Decision 6 — `badge.ping()` C bridge pattern: add to `badge_http_mp.c` + `BadgePython_bridges.cpp`

**Decision**: Follow the established bridge pattern:
1. `BadgePython_bridges.h` — declare `extern "C" bool BadgePython_probe_backend()`
2. `BadgePython_bridges.cpp` — implement: calls `BadgeAPI::probeBadgeExistence(uid_hex)`,
   returns `r.ok` (true = HTTP 200)
3. `badge_mp/badge_http_mp.c` — add `badge_ping_fn()` MicroPython C function, returns
   `mp_obj_t` True/False
4. `badge_mp/badge_module.c` — register `badge_ping_fn` in the module globals table as
   `"ping"`

**Alternatives considered**:
- New `badge_ping_mp.c` file — rejected: single function doesn't justify a new file;
  ping is HTTP-layer behavior, fits in `badge_http_mp.c`.
- Expose `probeBadgeExistence` directly with `httpCode` return — rejected: app developers
  don't need HTTP codes; True/False is the right abstraction for a ping.

---

## Decision 7 — Documentation update scope: README.md files only

**Decision**: Update `firmware/Firmware-0308-modular/README.md` and `apps/README.md`
(the Python app guide). No new documentation files. Root `README.md` may need minor
verification but is expected to be current.

Key content to verify/add:
- `apps/README.md`: add `badge.ping()` primitive with example
- Firmware README: confirm quickstart steps match current `BadgeConfig.h.example`,
  `setup.sh`, `build.sh -n`, `flash.py` workflow
- Remove any stale `flash.sh` references

---

---

## Decision 8 — Root cause of IR boop failure: structural collision, not timing

**Decision**: The original simultaneous-TX PING protocol is architecturally unsolvable.
Both badges call `sendUID()` simultaneously. `sendUID()` stops IrReceiver for ~800ms while
transmitting. T002/T003 (random 500–2000ms pre-TX delay + 1200ms window) were implemented
and tested on hardware. Serial logs showed A's 1200ms listen window expired before B's UID
send started (~1.7s gap measured). The timing gap is structural: random delay delays BOTH
badges proportionally; the window never reliably contains the peer's UID burst.

**Evidence**: Dual serial log analysis (BADGE-A.log + BADGE-B.log) showing:
- Both badges enter PING cycle simultaneously despite T002 jitter
- A's listen window closes before B transmits; B mirrors the same failure
- No successful 6-byte UID exchange observed across multiple boop attempts

**Alternatives considered**:
- Increase window further — rejected: logs showed structural not timing gap; window > 2000ms
  exceeds UX budget and still doesn't guarantee peer has started TX.
- Per-byte ACK scheme on PING protocol — rejected: PING protocol has no sequencing;
  would require a full redesign anyway; better to do TCP-IR properly.

---

## Decision 9 — TCP-IR half-duplex protocol: architecture

**Decision**: Replace PING protocol with a TCP-inspired three-way handshake over NEC IR,
followed by sequential per-byte ACK'd UID exchange. Half-duplex: only one badge TX at a
time after the handshake establishes roles. This architecturally prevents simultaneous TX.

**Protocol summary**:
- SYN/SYN-ACK/ACK handshake → role determination (INITIATOR / RESPONDER)
- INITIATOR sends all 6 UID bytes first, each ACK'd by RESPONDER
- RESPONDER sends all 6 UID bytes, each ACK'd by INITIATOR
- Both have peer UID → `boopPairingRequested` → HTTP POST

**Role determination**:
- Normal (one UP press, one passive listener): UP presser = INITIATOR
- Simultaneous SYN: both see each other's cookies via SYN_ACK; lower cookie = INITIATOR
- Tie-break: lower `uid_hex[0]`

**Rationale**: TCP's key insight — establish roles before data exchange — directly solves
the collision. After handshake, only INITIATOR transmits first; RESPONDER is in pure RX
mode. No simultaneous TX possible.

**Alternatives considered**:
- CSMA/CD-style collision detection — rejected: ESP32 IR hardware cannot detect collisions
  mid-frame; no equivalent to Ethernet CD.
- Token passing — rejected: requires a shared token, which is itself a bootstrap collision
  problem.
- Time-division (fixed slot assignment) — rejected: requires clock synchronization;
  no shared time reference on badges out of the box.

---

## Decision 10 — NEC IR packet encoding for TCP-IR protocol

**Decision**: Encode protocol type and payload into the 8-bit addr + 8-bit cmd NEC frame.

```
Control packets (addr = type, cmd = payload):
  SYN     0xC0  cmd = my_cookie (random 1–255)
  SYN_ACK 0xC1  cmd = my_cookie
  ACK     0xC2  cmd = seq being acknowledged (0–5)
  NACK    0xC4  cmd = seq expected
  FIN     0xC5  cmd = 0x00
  RST     0xC6  cmd = 0x00

Data packets (addr encodes type + seq):
  DATA    addr = 0xD0 | seq (0xD0–0xD5), cmd = UID byte value
```

The 0xC0–0xC6 range is in the reserved/private NEC address space; DATA uses 0xD0–0xD5.
All 6 packet types fit without collision. DATA embeds seq in the lower nibble, allowing
duplicate detection (retransmit same frame → receiver sees same addr+cmd, ignores if
already ACK'd).

**Rationale**: NEC addr (8-bit) + cmd (8-bit) gives 16 bits per frame. Splitting into type
(addr) + payload (cmd) cleanly maps to all required control signals and UID data bytes.
The encoding is deterministic and requires no parsing beyond comparing addr range.

**Alternatives considered**:
- Use single byte (addr only, cmd = addr complement) — rejected: NEC complement is for
  error detection only; using cmd as complement wastes the payload channel.
- Pack both role and seq into addr — rejected: 8 bits is sufficient for type+seq together
  only if we use nibbles; the DATA approach (0xD0|seq) achieves this cleanly.

---

## Decision 11 — IR hardware lifecycle: enabled only on Boop screen

**Decision**: IrSender and IrReceiver are started when the badge enters `MODE_MAIN` (the
Boop screen) and stopped when it leaves. A `volatile bool irHardwareEnabled` flag is written
by Core 1 (`loop()`) on screen transitions and read by Core 0 (`irTask()`).

**Why `volatile bool` (not mutex/queue)**:
- Single writer (Core 1), single reader (Core 0); no compound read-modify-write needed
- `volatile` prevents compiler from caching the value in a register across iterations
- Sufficient for this use case; a `QueueHandle_t` would be overkill for a one-bit signal

**IR enable/disable sequence**:
- `→ MODE_MAIN`: `irHardwareEnabled = true` → irTask calls `IrReceiver.begin()` +
  `IrSender.begin()` on next loop; state reset to `IR_IDLE`
- `← MODE_MAIN`: `irHardwareEnabled = false` → irTask calls `IrReceiver.end()`, powers off
  sender, resets to `IR_IDLE`; any in-flight exchange is abandoned (RST not sent — peer
  will timeout)

**Battery rationale**: NEC IR receiver draws ~1mA continuously when enabled. At 2,000
badges running 8-hour conference days, leaving IR on during QR/menu/app screens would waste
~16Wh conference-wide. Battery is the primary constraint for wearable badges.

**Alternatives considered**:
- Always-on IR — rejected: unnecessary battery draw; user clarification confirmed
  battery conservation preferred (Clarification Q1, Answer A).
- Disable only during non-Boop menus — rejected: more complex state tracking; simpler to
  enable only on the one screen that uses IR.

---

## Summary: What Needs to Be Built

| Item | Status | Work Needed |
|------|--------|-------------|
| `badge.ping()` Python primitive | ❌ Missing | Add bridge + binding + registration |
| Tilt guard (menu-only) | ⚠️ Ungated | Add `renderMode == MODE_MENU` check in `pollTilt()` |
| QR enrollment firmware | ✅ Complete | Verify end-to-end; backend dep deferred |
| IR boop firmware (old protocol) | ❌ Broken | Full rewrite to TCP-IR (Decisions 8–11) |
| TCP-IR protocol (`BadgeIR.cpp`) | ❌ Missing | Rewrite `irTask()` with SYN/ACK state machine |
| TCP-IR protocol (`BadgeIR.h`) | ❌ Missing | New `IrPhase` enum + `IrStatus` struct |
| IR hardware lifecycle | ❌ Missing | `irHardwareEnabled` flag + enable/disable in irTask |
| `boopEngaged` edge trigger | ❌ Missing | Change BadgeInput.cpp to fire once on UP press |
| BadgeDisplay TCP-IR status | ❌ Missing | renderMain() phases + live packet status display |
| Python display/input/HTTP/UID | ✅ Complete | No changes |
| Python exception recovery | ✅ Complete | Verified in `BadgePython.cpp` |
| `apps/README.md` — ping docs | ✅ Done (T010) | `badge.ping()` section added |
| Firmware README accuracy | ✅ Done (T009) | Updated for TCP-IR phases |
| Dead code removal | ✅ Done (T006/T007) | Dead IrPhase values removed |
