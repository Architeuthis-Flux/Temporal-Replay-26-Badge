# Quickstart: Badge Ping Demo (spec-009)

## Prerequisites

- Two (or more) flashed badges with completed boop pairings
- Working WiFi + backend server
- `BadgeConfig.h` configured with `WIFI_SSID`, `WIFI_PASS`, `SERVER_URL`
- `./setup.sh` completed (arduino-cli, libraries, MicroPython embed)

## Build

```bash
cd firmware/Firmware-0308-modular/
./build.sh -n
```

Binary: `build/Firmware-0308-modular.ino.bin`

## Flash

```bash
./flash.py
```

## Seed Test Data

Before testing, ensure at least one boop pairing exists between two badges. Either:
1. Do a live IR boop exchange between two test badges, OR
2. Ask the backend team to seed a `badge_contacts` NVS entry via `BadgeConfig.h` debug build with `BYPASS true`.

## Test: Messages (C Layer)

1. On Badge A: navigate Main Menu → **Messages**
2. Verify: contact list shows previously booped badges (name + company)
3. Select a contact → emoji palette appears (8 emoji)
4. Select emoji → confirm → `"Sending..."` → `"Sent!"`
5. On Badge B: verify emoji + sender name appear in Messages screen (within ~10s)
6. Verify ping record via: `GET <SERVER>/api/v1/pings?requester_badge_uuid=<UUID>&type=emoji`

## Test: Conquest App (Python Layer)

1. On Badge A: Main Menu → **Apps** → **conquest**
2. Verify: army size shown (equals boop count)
3. Select Badge B from contact list → confirm
4. Verify: `"Waiting for <name>..."` appears
5. On Badge B: Main Menu → **Apps** → **conquest** (or badge is already on LIST screen and auto-responds)
6. On Badge A: verify battle result displays with both army sizes
7. Verify winner/loser is correct (higher boop count wins)

## Test: Error Handling

- Disconnect WiFi during send → `"Send failed"` within 10s
- Select contact with expired boop pairing → `"Send failed"  HTTP 403"`
- Conquest with no contacts → `"No contacts. Boop someone first!"`

## Verify Build Success Criterion

```bash
./build.sh -n 2>&1 | tail -5
# Must end with: "Sketch uses ... bytes" — no errors
```

Implementation is NOT complete until this build succeeds.
