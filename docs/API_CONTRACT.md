# RomM API contract (pinned to 5.2.0)

Everything the Switch client talks to. **Verified against a live RomM `5.2.0`
instance** (see `server/contract/romm-openapi-5.2.0.json` for the full snapshot).
If you upgrade RomM, re-run `server/probe_contract.py` and update this file.

Base URL: your RomM origin, e.g. `https://romm.example.com` (see
[SECURITY.md](SECURITY.md) for exposing it safely). All API paths are under
`/api`.

## Authentication — OAuth 2.0 Device Authorization Grant (headless)

Perfect for a console with no keyboard: the sysmodule shows a short code, you
approve it once in RomM's web UI, and the sysmodule receives a long-lived client
token. Full walkthrough in [AUTH.md](AUTH.md).

| Method | Path | Body → Response |
|---|---|---|
| POST | `/api/auth/device/init` | `DeviceAuthInitPayload` → device_code + user_code + verification info |
| GET | `/api/auth/device/pending/{user_code}` | (web UI approval lookup) |
| POST | `/api/auth/device/approve` | approve a pending code (web UI) |
| POST | `/api/auth/device/deny` | deny a pending code (web UI) |
| POST | `/api/auth/device/token` | `DeviceAuthTokenPayload {device_code}` → access token (poll until approved) |

`DeviceAuthInitPayload`:
```
client_device_identifier: string   (stable per-console id, e.g. hash of serial)
name: string                       ("My Switch")
client: string                     ("rommsync-nx")
platform: string?                  ("switch")
client_version: string?
requested_scopes: array            (see scopes below)
```

Client-token pairing (alternative / management) also exists:
`POST /api/client-tokens`, `POST /api/client-tokens/exchange {code}`,
`GET /api/client-tokens/pair/{code}/status`.

### Scopes to request

```
me.read roms.read roms.user.read roms.user.write
assets.read assets.write devices.read devices.write collections.read
me.write            # only if recording play sessions
```

All authed requests send `Authorization: Bearer <token>`.

## Device registration

| Method | Path | Body → Response |
|---|---|---|
| GET | `/api/devices` | list this user's devices |
| POST | `/api/devices` | `DeviceCreatePayload` → `{device_id, name, created_at}` — the id field is **`device_id`**, not `id` (verified live, 5.2.0) |
| GET/PUT/DELETE | `/api/devices/{device_id}` | manage one |

`DeviceCreatePayload` (key fields): `name, platform, client, client_version,
hostname, mac_address, sync_mode, sync_config, allow_existing, allow_duplicate`.
Cache the returned `device_id` — it's a UUID string, and it's what every sync call is scoped by.

## Save sync — negotiate → execute → complete

This is the core loop. The client hashes its local saves, asks the server for a
plan, executes it, and reports back. **The server is the arbiter** of conflicts.
Detailed semantics in [SYNC_PROTOCOL.md](SYNC_PROTOCOL.md).

| Method | Path | Body → Response |
|---|---|---|
| POST | `/api/sync/negotiate` | `SyncNegotiatePayload` → `SyncNegotiateResponse` |
| POST | `/api/sync/sessions/{session_id}/complete` | `SyncCompletePayload` → `SyncCompleteResponse` |
| GET | `/api/sync/sessions` / `/{session_id}` | inspect sessions (`SyncSessionSchema`) |
| POST | `/api/sync/devices/{device_id}/push-pull` | one-shot convenience → `SyncSessionSchema` |

`SyncNegotiatePayload`:
```
device_id: string?
saves: array   *required   # one entry per local save the client found
```
Each save entry carries at least: `rom_id` (matched locally), `file_name`,
`slot`, `emulator`, content `hash` (SHA1), `updated_at`/mtime, `size`. Confirm
the exact `SyncSaveEntry` field names from the snapshot before coding (issue #M2-1).

`SyncNegotiateResponse`:
```
session_id: integer
operations: array of SyncOperationSchema
total_upload / total_download / total_conflict / total_no_op: integer
```

`SyncOperationSchema`:
```
action: "upload" | "download" | "conflict" | "noop"
rom_id: integer
save_id: integer?          # server save to fetch (download/conflict)
file_name: string
slot: string?
emulator: string?
reason: string
server_updated_at: string?
server_content_hash: string?
```

`SyncCompletePayload`: `operations_completed, operations_failed, play_sessions[]`
→ `SyncCompleteResponse { session, play_session_ingest }`.

## Save & state I/O (used while executing a plan)

| Method | Path | Notes |
|---|---|---|
| GET | `/api/saves` | list; filter `rom_id, platform_id, device_id, slot` |
| POST | `/api/saves` | upload; query `rom_id*, emulator, slot, device_id, session_id, overwrite, autocleanup, autocleanup_limit`; multipart field **`saveFile`** (+ optional `screenshotFile`) |
| GET | `/api/saves/{id}/content` | download raw save bytes |
| GET | `/api/saves/identifiers` | lightweight id/hash listing |
| POST | `/api/saves/{id}/downloaded` | mark a save downloaded by this device |
| GET/POST | `/api/states` (+ `/{id}/content`) | same shape for save states; field **`stateFile`** |

`SaveSchema` (response) key fields: `id, rom_id, user_id, file_name,
file_extension, file_size_bytes, download_path, content_hash, emulator, slot,
updated_at, origin_device_id`.

## Library & downloads (the "browse + get games" side)

| Method | Path | Notes |
|---|---|---|
| GET | `/api/roms` | paged; filters incl. `collection_id`, `platform_ids`, `search_term`, `limit`, `offset`, `order_by` |
| GET | `/api/roms/{id}` | `DetailedRomSchema` |
| GET | `/api/roms/{id}/content/{file_name}` | download a single-file rom (`fs_name`) |
| GET | `/api/roms/{id}/files` | list files of a multi-file rom |
| GET | `/api/roms/{id}/files/content/{file_name}` | download one file of a multi-file rom |
| GET | `/api/collections` | user collections (curate a "Switch" collection to mirror) |
| HEAD | `/api/roms/{id}/content/{file_name}` | size / resume checks |

Key `SimpleRomSchema` fields for the client: `id, platform_fs_slug,
platform_slug, fs_name, fs_name_no_ext, fs_name_no_tags, fs_size_bytes,
sha1_hash, md5_hash, crc_hash, has_multiple_files, name`.

### Platform → folder mapping

`platform_fs_slug` is the on-disk slug (e.g. `snes`, `gba`, `psx`, `n64`,
`genesis`, `nds`, `psp`, `dreamcast`). The client maps each to a Tico /
RetroArch SD folder; default map + override scheme in [CONFIG.md](CONFIG.md).

## Resume & integrity

- Range requests are supported on `content` endpoints (SwitchRomM already relies
  on `Range` resume) — use them for large-rom downloads on flaky links.
- Verify downloads against `sha1_hash`/`md5_hash` from the rom schema.
- Saves are matched to roms by `fs_name_no_ext` (exact) with `fs_name_no_tags`
  fallback, scoped by platform when known — see [SYNC_PROTOCOL.md](SYNC_PROTOCOL.md).
