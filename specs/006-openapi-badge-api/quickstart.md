# Quickstart: OpenAPI Badge API Contract Library

## Overview

This feature introduces a two-tier type system for firmware-backend communication:
1. `specs/openapi.json` — versioned backend contract artifact
2. `scripts/gen_badge_api_types.py` — reads the spec, writes `BadgeAPI_types.h`

`BadgeAPI_types.h` is committed to the repo. You only need to regenerate it when
`specs/openapi.json` is updated.

---

## First-time firmware setup (unchanged)

```bash
cd firmware/Firmware-0308-modular/
cp BadgeConfig.h.example BadgeConfig.h   # fill in WiFi SSID, password, server URL
./setup.sh                                # install arduino-cli + libraries (one-time)
```

---

## Build firmware

```bash
cd firmware/Firmware-0308-modular/
./build.sh
```

`BadgeAPI_types.h` is already present and committed. No generator run is needed for
a normal build.

---

## Regenerate BadgeAPI_types.h after an openapi.json update

```bash
# From repo root
python scripts/gen_badge_api_types.py

# Verify the output is deterministic
python scripts/gen_badge_api_types.py && diff firmware/Firmware-0308-modular/BadgeAPI_types.h \
  <(python scripts/gen_badge_api_types.py)
# → no output means identical (deterministic)

# Review the diff
git diff firmware/Firmware-0308-modular/BadgeAPI_types.h

# Build firmware with updated types
cd firmware/Firmware-0308-modular/ && ./build.sh
```

If the firmware **fails to compile** after regeneration, there is a field rename or type
change in the backend spec. Update the call sites in `BadgeAPI.cpp` and all callers.
This is expected — it is the purpose of the contract library.

---

## Sync workflow: backend ships a new spec

1. Backend developer commits a new `openapi.json` to their repo.
2. Firmware developer fetches the new spec:
   ```bash
   # Option A: copy from registrationScanner repo if available locally
   cp ../registrationScanner/openapi.json specs/openapi.json
   # Option B: fetch from staging (if accessible)
   curl -s https://your-backend.example.com/docs/openapi.json > specs/openapi.json
   ```
3. Run the generator and review the diff:
   ```bash
   python scripts/gen_badge_api_types.py
   git diff firmware/Firmware-0308-modular/BadgeAPI_types.h
   ```
4. Update `BadgeAPI.cpp` for any changed fields or new endpoints.
   - Note the spec version at the top of `BadgeAPI.cpp` (see source file comment).
5. Build and test:
   ```bash
   cd firmware/Firmware-0308-modular/ && ./build.sh
   ./flash.sh  # flash to badge for hardware verification
   ```
6. Commit `openapi.json`, `BadgeAPI_types.h`, `BadgeAPI.cpp` together.

---

## Generator usage

```bash
python scripts/gen_badge_api_types.py [--spec PATH] [--out PATH]

# Defaults:
#   --spec  specs/openapi.json
#   --out   firmware/Firmware-0308-modular/BadgeAPI_types.h

# Error exit codes:
#   1 — spec file not found or not valid JSON
#   2 — spec does not contain required firmware endpoints
```

**What the generator does**:
- Reads the `components/schemas` section of `openapi.json`
- For each schema matching a firmware-facing type, emits a C struct
- Skips schemas not in its `FIRMWARE_SCHEMAS` allowlist (warning to stdout)
- Emits a `#pragma once` guard and `// GENERATED — do not edit` header comment
- Output is sorted by schema name for determinism

**What the generator does NOT do**:
- It does not generate HTTP logic, serialization, or function bodies
- It does not touch `BadgeAPI.cpp` — that file is hand-maintained
- It does not auto-run at build time — you run it manually after a spec update

---

## Testing the contract library

### Verify typed structs (SC-003, SC-004)

```bash
# No String in public BadgeAPI.h signatures
grep -n "String" firmware/Firmware-0308-modular/BadgeAPI.h
# → should return zero matches in public function signatures

# No direct HTTPClient in BadgePairing.cpp (SC-002)
grep -n "HTTPClient" firmware/Firmware-0308-modular/BadgePairing.cpp
# → should return zero matches

# All seven functions present in BadgeAPI.h (SC-005)
grep -n "getQRBitmap\|getBadgeInfo\|createBoop\|getBoopStatus\|getBoopPartner\|sendPing\|getPings" \
  firmware/Firmware-0308-modular/BadgeAPI.h
# → should show 7 function declarations
```

### Verify build (SC-001)

```bash
cd firmware/Firmware-0308-modular/ && ./build.sh
# → expect: "Sketch uses ... bytes" — no errors
```

### Verify determinism (SC-004)

```bash
python scripts/gen_badge_api_types.py
python scripts/gen_badge_api_types.py
git diff firmware/Firmware-0308-modular/BadgeAPI_types.h
# → no output (identical output on repeated runs)
```

### Verify behavioral parity (SC-006)

Flash to a badge and exercise:
- [ ] Boot sequence: WiFi connects → QR code displays → activation polling loop
- [ ] QR dismissed by button press → enters main menu
- [ ] After QR scan by staff: badge activates → nametag displays with correct name/role
- [ ] IR pairing: approach another badge → consent dialog → confirmed state
- [ ] Tilt nametag: hold 1.5 s → nametag flip with fade transition
- [ ] Serial monitor: no crashes, no error prints during normal flow

---

## Adding a new BadgeAPI function (future pattern)

1. Add the endpoint to `specs/openapi.json` under `paths` and `components/schemas`.
2. Run `python scripts/gen_badge_api_types.py` — new struct appears in `BadgeAPI_types.h`.
3. Add declaration to `BadgeAPI.h`.
4. Implement in `BadgeAPI.cpp`.
5. Build: `./build.sh`.
