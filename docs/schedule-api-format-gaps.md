# Schedule API Format Gaps

**TL;DR:** Yes — the live schedule API is the bottleneck. The firmware
parser, the static fallback, and `data/build-data.py` all support richer
event data (room UIDs, speaker UIDs, descriptions). The live endpoint at
`https://brooklyn.party/api/v1/schedule?format=badge` only emits the
minimum 7 fields per event, drops descriptions, and uses free-form
room-name strings instead of the canonical UIDs from `floors.md`.

Audit captured 2026-05-01 against production
(`https://brooklyn.party`) on the `echo` build.

---

## What the live API returns today

Endpoint: `GET /api/v1/schedule?format=badge`
Required header: `Accept: application/msgpack` (without it, server
returns 32 KB JSON; with it, 7.3 KB msgpack).

Top-level shape — matches the firmware parser:

```
[
  version: int = 1,
  "",                      # reserved metadata slot
  "",                      # reserved metadata slot
  events: [ <event>, ... ] # 78 entries on production
]
```

Per-event shape — **all 78 events are exactly 7 fields**:

```
[
  date_str,        # "2026-05-05"
  start_time_str,  # "07:30"
  end_time_str,    # "09:00"
  type_int,        # 0=talk, 1=workshop, 2=other (matches TYPE_MAP)
  title_str,       # "Keynote"
  room_str,        # "The Hangar"   ← string, not a UID
  speakers_str     # "Samar Abbas, Amjad Masad, Venkat Venkataramani"
]                  #                ← flattened, not a UID list
```

Descriptions are **never sent** (no event has 8 fields).

---

## What the firmware can already consume

`firmware/src/screens/ScheduleData.cpp::parseScheduleMsgPack` accepts:

- 7-field events (current live behaviour) — works.
- 8-field events with a trailing `desc` string — works, but the server
  never emits this. `SCHED_DESC_MAX = 192` per event already reserved.
- Anything beyond field 7 (or 8) is silently `skip()`ped, so adding
  fields to the wire format is a **non-breaking change** for already
  shipped firmware.

`data/build-data.py` (source of truth for the offline bundle) already
emits both `loc` (room UID, `floor_idx*100 + section_idx*10`, with `99`
suffix for whole-floor refs and `9xx` for off-site) and `speakers`
(integer ID list keyed off `speakers.md` row order). Floors share the
same UID space, so a numeric match is sufficient to bind events to map
sections.

---

## Concrete consequences in the UI

1. **Schedule detail modal description is blank for live data.** Static
   fallback rows that ship in `SCHED_DAYS[]` have descriptions; the 78
   live rows do not. As soon as the live fetch succeeds, every modal
   loses its body copy.
2. **Map ↔ schedule binding is fragile.** Live room strings include
   variants the canonical floor list does not — e.g.
   `"Lobby / 2nd Floor"`, `"Lobby / Downstairs"`,
   `"Java [sold out]"` — so `MapData` papers over the gap with
   hand-maintained sub-aliases inside `f1_subs[]` / `f3_subs[]`. Any new
   server-side room name silently breaks "Locate" until those tables are
   hand-edited.
3. **No bundle endpoint in production.** `GET /api/v1/data` (declared in
   `BadgeAPI.h` and consumed by `DataCache`) returns 404. The only live
   data path is the schedule endpoint above; the rich
   bundle-with-UIDs only reaches the badge via the embed fallback in
   flash, which means stale until reflash.

---

## Server-side action items (priority order)

### 1. Append `desc` as field 8 of every event — high value, no breaking risk

Output schema becomes:

```
[ date, start, end, type, title, room, speakers, desc ]
                                                  ^
                                                  new
```

`desc` is a string; empty string is fine for events without copy. Old
firmware skips it; new firmware (already deployed) populates the modal
body. Source of truth: the existing `desc` field in
`data/in/schedule.yaml`.

### 2. Append `loc` as field 9 — unblocks UID-based map binding

```
[ date, start, end, type, title, room, speakers, desc, loc ]
                                                        ^
                                                        new int
```

Use the same code scheme as `data/build-data.py` (`floor_idx*100 +
section_idx*10`, `*99` for whole-floor refs, `9xx` for off-site). Once
this lands, the firmware can drop the room-name fuzzy match in
`MapData` and use a single integer compare against `floors.msgpack`
section codes.

### 3. Replace `speakers_str` with `speaker_ids` list — or add as field 10

Either:

- Replace field 7 (`speakers_str`) with `speaker_ids: [int, int, ...]`
  keyed off the `speakers.md` row order — cleaner, but breaks any
  consumer that's parsing the comma-joined string today.
- Or keep field 7, add field 10 = `speaker_ids: [int, int, ...]` —
  non-breaking, lets the badge cross-reference into
  `speakers.msgpack` for company / title.

### 4. Canonicalise room strings to match `floors.md`

Even with `loc` in place, the human-readable room string should match
the canonical `floors.md` entries (`"Lobby"`, not
`"Lobby / 2nd Floor"`; `"Level 02"`, not `"Java [sold out]"`). This
removes the hand-maintained alias tables in `MapData.cpp`
(`f1_subs[]`, `f3_subs[]`).

### 5. Stand up `GET /api/v1/data` — the full bundle endpoint

Serve the output of `data/build-data.py` (the
`[ header | schedule.msgpack | speakers.msgpack | floors.msgpack ]`
TBDS bundle) at `/api/v1/data` with `application/msgpack` and an ETag
header. The badge's `DataCache` is already plumbed for this path
(3-tier loader: FatFS → re-download → embedded fallback). Once live,
even if items 1-4 stall, the bundle covers all of them in one shot.

---

## Firmware-side action items (optional, can ship ahead of server)

These tolerate the missing fields and degrade gracefully:

- **Capture `loc` when present.** Extend `SchedEvent` with
  `uint16_t loc` (default 0). Update `parseScheduleMsgPack` to read
  field 9 when `fieldCount >= 9`. Use it in
  `MapFloorScreen::collectEventsForRoom` as the primary key, falling
  back to room-string match when `loc == 0`.
- **Capture `speaker_ids` when present.** Extend the parser to accept
  either field-7-string or field-10-list. Cache the ID list on the
  event so the modal can pull rich speaker data from
  `speakers.msgpack`.
- **Surface a debug dump.** Add a serial command (`dump:schedule`) that
  prints field counts per event and counts of empty desc/loc/speakers,
  so the next API rev can be verified without tearing down a badge.

None of these change wire compatibility; they just unlock richer UI
the moment the server starts emitting the fields.

---

## Verification recipe

```bash
# Fetch live msgpack and confirm shape
curl -sS -H "Accept: application/msgpack" \
  "https://brooklyn.party/api/v1/schedule?format=badge" \
  -o /tmp/schedule.msgpack

python3 -c '
import msgpack
data = msgpack.unpackb(open("/tmp/schedule.msgpack","rb").read(), raw=False)
events = data[3]
print("events:", len(events))
print("field-count histogram:",
      {n: sum(1 for e in events if len(e)==n) for n in range(5,12)})
print("events missing desc:",
      sum(1 for e in events if len(e) < 8 or not e[7]))
print("unique rooms:", len({e[5] for e in events}))
'
```

Expected after server fix #1: every event has `len(e) >= 8` and most
events have non-empty `e[7]`.
Expected after server fix #2: every event has `len(e) >= 9` and `e[8]`
is an integer in the floors.msgpack `loc` space.
