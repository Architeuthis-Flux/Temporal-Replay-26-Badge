# Contract: badge_api.py

**Module**: `firmware/micropython-build/badge_api.py`
**Consumer**: `boot.py`, `badge_sdk.py` (future), `main.py` (future)
**Stability**: Internal — subject to change before conference deployment

---

## Module Interface

### Initialization

No class instantiation required. Module reads `SERVER_URL` from the `creds` module lazily on first call. If `creds` is absent (BYPASS mode), all functions return `None` immediately and log a warning.

```python
import badge_api
# No setup call needed. SERVER_URL sourced from: import creds; creds.SERVER_URL
```

---

## Public Functions

### `get_badge_info(badge_uuid: str) -> dict | None`

Fetch attendee registration data for this badge.

```
GET {SERVER_URL}/api/v1/badge/{badge_uuid}/info
```

**Returns**:
```python
{"name": str, "title": str, "company": str, "attendee_type": str}
```
Returns `None` on HTTP 404 (badge not registered) or any network/HTTP error.

**Guarantees**: HTTP connection closed before return.

---

### `create_boop(uuid1: str, uuid2: str) -> dict | None`

Initiate a mutual pairing workflow between two badges.

```
POST {SERVER_URL}/api/v1/boops
Body: {"badge_uuids": [uuid1, uuid2]}
```

**Returns**:
```python
{"workflow_id": str, "status": "pending" | "confirmed", ...}
```
Returns `None` on network error or non-2xx response.

**Note**: HTTP 200 = second badge confirming (status "confirmed"). HTTP 202 = first badge (status "pending"). Both are handled.

---

### `get_boop_status(workflow_id: str, badge_uuid: str) -> dict | None`

Poll the status of a pending pairing workflow.

```
GET {SERVER_URL}/api/v1/boops/status/{workflow_id}
```

**Returns**:
```python
{"status": "pending" | "confirmed" | "cancelled", ...}
```
Returns `None` on network error or not-found.

---

### `list_boops(badge_uuid: str) -> list | None`

List all active boop pairings for this badge.

```
GET {SERVER_URL}/api/v1/boops
Query params: badge_uuid={badge_uuid}  (or however the server expects filtering)
```

**Returns**: List of boop dicts, or `[]` if none, or `None` on error.

---

### `cancel_pending_boop(badge_uuid: str) -> bool | None`

Cancel any pending (unconfirmed) boop for this badge.

```
DELETE {SERVER_URL}/api/v1/boops/pending
```

**Returns**: `True` on success, `None` on error.

---

## Internal Transport

### `_request(method, path, body=None) -> dict | list | bool | None`

*Private — not part of the public interface.*

Single function that all public endpoint functions delegate to. Responsibilities:
1. Build full URL from `SERVER_URL + path`
2. Set `Content-Type: application/json` header
3. **[FUTURE]** Insert HMAC auth headers: `X-Badge-ID`, `X-Timestamp`, `X-Signature`
4. Call `urequests.request(method, url, headers=..., data=ujson.dumps(body) if body else None)`
5. Parse response: 2xx → `.json()`, 404 → `None`, other non-2xx → `None`
6. **Always** call `r.close()` in a `finally` block
7. Catch `OSError` and other exceptions → return `None`, print error

---

## Error Behavior Contract

| Scenario | Return Value | Side Effects |
|----------|-------------|--------------|
| HTTP 2xx, valid JSON | parsed dict/list | connection closed |
| HTTP 404 | `None` | connection closed |
| HTTP other non-2xx | `None` | connection closed, status code logged |
| Network error (OSError) | `None` | exception logged |
| `creds` module absent | `None` | warning logged |
| Invalid response JSON | `None` | connection closed, exception logged |

**Invariant**: No raw `urequests` response object is ever returned or held alive after the function returns.

---

## Future Auth Hook

When HMAC authentication is added (future spec), only `_request` changes:

```python
def _request(method, path, body=None):
    import utime
    ts = str(utime.time())
    sig = badge_crypto.sign_request(_BADGE_UUID + ts)  # C module
    headers = {
        "Content-Type": "application/json",
        "X-Badge-ID": _BADGE_UUID,
        "X-Timestamp": ts,
        "X-Signature": sig,
    }
    ...
```

All callers (`boot.py`, `badge_sdk.py`, `main.py`) gain authentication with zero code changes.

---

## Callers (Post-Implementation)

| File | Functions Used |
|------|---------------|
| `boot.py` | `get_badge_info` |
| `badge_sdk.py` (future) | `create_boop`, `get_boop_status`, `list_boops`, `cancel_pending_boop` |
| `main.py` (future) | `create_boop` (after IR pairing receives partner UID) |
