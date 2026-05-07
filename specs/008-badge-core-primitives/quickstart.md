# Quickstart Documentation Plan

**Feature**: `008-badge-core-primitives`
**Scope**: Accuracy audit and targeted updates to existing README files only.
No new documentation files are created (per spec assumption).

---

## Files to Update

### 1. `firmware/Firmware-0308-modular/README.md`

**Audit checklist** — verify each section reflects current reality:

- [ ] Quick start section references `setup.sh`, `build.sh -n`, `flash.py` — no mention of `flash.sh`
- [ ] `BadgeConfig.h` setup instructions match current `.example` fields
- [ ] Module map includes `BadgePython`, `BadgeMenu`, all current modules
- [ ] Serial commands section covers `run:`, `list`, `test`, `test:c`, `test:py`
- [ ] Boot sequence description matches current `osRun()` → `osUnpairedFlow()` / `osPairedFlow()` flow
- [ ] IR pairing state machine description matches current phase list
- [ ] Tilt nametag behavior documented: 1500ms hold, menu-only activation (new for spec-008)
- [ ] MicroPython section mentions `setup.sh` generates `micropython_embed/`

**Expected changes**:
- Add note that tilt-to-nametag only activates while on the menu screen (not during apps or input test)
- Remove any stale `flash.sh` references if present

---

### 2. `firmware/Firmware-0308-modular/apps/README.md`

**Audit checklist**:

- [ ] All v1 `badge` module functions documented
- [ ] `badge.ping()` added with description and example (new for spec-008)
- [ ] `badge.tilt_read()` return value documented (bool: HIGH=upright, LOW=flat)
- [ ] `badge.ir_available()` / `badge.ir_read()` stubs noted as v1 limitations
- [ ] Deployment instructions: `apps/` directory → rebuild + reflash
- [ ] Escape chord documented (BTN_UP + BTN_DOWN to exit)
- [ ] Exception handling behavior documented (traceback displayed, returns to menu)

**Expected additions**:
```markdown
## badge.ping()

```python
badge.ping() -> bool
```

Returns `True` if the badge can reach the backend API (HTTP 200 from badge info endpoint).
Returns `False` if WiFi is disconnected, the server is unreachable, or the badge is not
enrolled. Blocks for up to 8 seconds on timeout.

```python
# Example: display connectivity status
if badge.ping():
    badge.display.text("online", 0, 32)
else:
    badge.display.text("offline", 0, 32)
badge.display.show()
```
```

---

### 3. Root `README.md` — verify only

No content changes expected. Verify the repo structure section and quick start pointer
to the firmware README are accurate.

---

## Acceptance Criteria for Documentation

Per spec SC-005 and SC-006:
- A developer with hardware can follow the firmware README quickstart and flash successfully
  within 15 minutes, with zero undocumented steps
- A developer using only `apps/README.md` can write a working app that uses `badge.ping()`,
  `badge.display.*`, and `badge.tilt_read()` without reading C source
