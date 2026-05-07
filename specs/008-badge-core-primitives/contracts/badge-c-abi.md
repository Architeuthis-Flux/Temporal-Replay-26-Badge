# Contract: Badge C ABI Changes (spec-008)

**Header**: `BadgePython_bridges.h`
**Caller**: `badge_mp/badge_http_mp.c` (C, MicroPython binding layer)
**Implementer**: `BadgePython_bridges.cpp` (C++)
**Per constitution Principle VII**: All `badge_mp/*.c` files must call through the
`extern "C"` bridge layer; they must not re-implement HTTP logic.

---

## New Bridge Function: `BadgePython_probe_backend`

**Declaration** (add to `BadgePython_bridges.h`):
```c
extern "C" bool BadgePython_probe_backend(void);
```

**Implementation** (add to `BadgePython_bridges.cpp`):
```cpp
bool BadgePython_probe_backend() {
    extern char uid_hex[];
    BadgeAPI::ProbeResult r = BadgeAPI::probeBadgeExistence(uid_hex);
    return r.ok;
}
```

**Contract**:
- Returns `true` if `BadgeAPI::probeBadgeExistence` returns `ok == true` (HTTP 200)
- Returns `false` on any failure (connection error, timeout, non-200)
- Never throws; error handling is inside `BadgeAPI::probeBadgeExistence` (returns ok=false)
- Called from Core 1 (MicroPython runs on Core 1); `HTTPClient` is safe to call from any
  core, but is synchronous — will block for up to 8 seconds on timeout

**Threading note**: `probeBadgeExistence` uses `HTTPClient` (synchronous). Calling this
from a Python app on Core 1 while the IR task on Core 0 is also making HTTP calls could
cause resource contention. This is an existing v1 limitation; no mutex added in spec-008.

---

## Existing C ABI: No Changes

No existing `BadgePython_bridges.h` declarations are modified. All changes are additive.

| Bridge function | Status |
|-----------------|--------|
| `BadgePython_display_clear()` | ✅ Unchanged |
| `BadgePython_display_text()` | ✅ Unchanged |
| `BadgePython_display_show()` | ✅ Unchanged |
| `BadgePython_button_pressed()` | ✅ Unchanged |
| `BadgePython_tilt_read()` | ✅ Unchanged |
| `BadgePython_http_get()` | ✅ Unchanged |
| `BadgePython_http_post()` | ✅ Unchanged |
| `BadgePython_uid()` | ✅ Unchanged |
| `BadgePython_server_url()` | ✅ Unchanged |
| `BadgePython_ir_send()` | ✅ Unchanged |

---

## `badge_module.c` Registration Addition

Add to the module globals table in `badge_mp/badge_module.c`:
```c
{ MP_ROM_QSTR(MP_QSTR_ping), MP_ROM_PTR(&badge_ping_fn_obj) },
```

And define the function object in `badge_http_mp.c`:
```c
STATIC mp_obj_t badge_ping_fn(void) {
    return mp_obj_new_bool(BadgePython_probe_backend());
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(badge_ping_fn_obj, badge_ping_fn);
```

---

## Backend Enrollment Contract (dependency — out of scope for firmware)

The QR enrollment detection relies on `GET /api/v1/badge/{uid}/info` returning HTTP 200
after a badge is linked to an attendee. The backend must ensure:

1. Before enrollment: `GET /api/v1/badge/{uid}/info` returns HTTP 404 or non-200
2. After enrollment: `GET /api/v1/badge/{uid}/info` returns HTTP 200 with:
   ```json
   {
     "name": "string",
     "title": "string",
     "company": "string",
     "attendee_type": "Attendee|Staff|Vendor|Speaker",
     "bitmap": [int, ...]
   }
   ```

The firmware's `pollQRPairing()` will detect the transition automatically. No firmware
changes are required for this flow — only the backend enrollment endpoint is needed.
