# RomM API contract (pinned to 5.2.0)

Everything the Switch client talks to. **Verified against a live RomM `5.2.0`**:
`server/contract/romm-openapi-5.2.0.json` is the full OpenAPI snapshot, and
`server/contract/captures/` holds the real responses this page quotes, captured
from the docker fixture by `server/probe_contract.py` (issue M0-4). If you
upgrade RomM, re-capture and update this file — `ctest -R contract` fails when
the captures stop matching the server.

> The snapshot says what RomM *declares*; the captures say what it *does*. Where
> this page marks something **verified**, it came from a captured response, not
> from the schema — the schema does not tell you that a save's `content_hash` is
> an MD5, that an upload comes back under a different file name, or that
> `expires_at` is null.

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

Real `init` response (`captures/auth-device-init.json`):
```json
{
  "device_code": "<64 lowercase hex characters>",
  "user_code": "<8 uppercase alphanumerics, no dash>",
  "verification_path": "/pair/device",
  "verification_path_complete": "/pair/device?user_code=<user_code>",
  "expires_in": 600,
  "interval": 5
}
```
`init` answers **`201 Created`**, not `200`. Both verification fields are
**paths, not URLs** — the server is origin-agnostic, so the client joins them
with the origin it was configured with.

Real `token` response (`captures/auth-device-token.json`):
```json
{
  "access_token": "rmm_<64 lowercase hex characters>",
  "device_id": "9fdce844-779b-4216-a6e2-597a2f3e7027",
  "scopes": ["assets.read", "assets.write", "collections.read", "devices.read",
             "devices.write", "me.read", "roms.read", "roms.user.read",
             "roms.user.write"],
  "expires_at": null
}
```
**Verified:** there is no `token_type`, no `refresh_token`, and no `expires_in`.
`expires_at` comes back `null` — the token does not expire on its own, so a
`401` means revoked, not stale, and there is nothing to refresh
([AUTH.md](AUTH.md#re-pairing--revocation)). `scopes` is what was *approved*,
sorted, which is not necessarily what was requested — check it rather than
assuming. The response already carries a `device_id`, so pairing alone is enough
to start syncing; see below before calling `POST /api/devices`.

A poll that is not yet approved answers **`400`** with FastAPI's `detail`, not
RFC 8628's `error`: `authorization_pending`, `slow_down`, `access_denied` and
`expired_token` all share that one status, and only the string separates "keep
polling" from "this pairing is dead". Over-polling from one IP answers `429`
with an English sentence instead of a code. None of this is in the OpenAPI
snapshot, which declares `200` and `422` and no error body; the full table is in
[AUTH.md](AUTH.md#polling-the-token-endpoint) and the shape is captured in
`captures/auth-device-token-pending.json`.

Client-token pairing (alternative / management) also exists:
`POST /api/client-tokens`, `POST /api/client-tokens/exchange {code}`,
`GET /api/client-tokens/pair/{code}/status`.

### Scopes to request

```
me.read roms.read roms.user.read roms.user.write
assets.read assets.write devices.read devices.write collections.read
me.write            # only if recording play sessions
```

This block is not prose: the `auth.scopes` test parses it and compares it to
`MinimumScopes()`, so editing one without the other goes red. Anything qualified
with a `#` comment is documented and *not* requested.

All authed requests send `Authorization: Bearer <token>`.

## Device registration

| Method | Path | Body → Response |
|---|---|---|
| GET | `/api/devices` | list this user's devices → `DeviceSchema[]` |
| POST | `/api/devices` | `DeviceCreatePayload` → `DeviceCreateResponse` |
| GET/PUT/DELETE | `/api/devices/{device_id}` | manage one → `DeviceSchema` |

**Pairing is what registers the console. This client never calls
`POST /api/devices`.**

RomM creates the device row from the `client_device_identifier` in
`POST /api/auth/device/init`, and the token response hands back its `device_id`
([AUTH.md](AUTH.md)). Pair again with the same identifier and RomM answers with
the *same* `device_id` — that is the mechanism the whole thing rests on, and
`device.repair` checks it by counting the rows on the server.

### Why `POST /api/devices` is the wrong call

**Verified — it never matches on `client_device_identifier`.** RomM matches an
existing device on `mac_address` *or* `hostname` and on nothing else. `name`,
`platform`, `client` and `client_device_identifier` play no part, `allow_existing`
on its own deduplicates nothing, and calling it with the device-bound client
token *of the very device being described* does not change that. One console,
six calls, five extra rows:

| Call, as the paired console | Result |
|---|---|
| `{name, platform, client, allow_existing: true}` | **201**, a new `device_id`, every single time |
| `{…, allow_existing: false}` | **201** as well — the flag is not a dedup switch |
| `{…, hostname}` first call | **201**, a new `device_id` |
| `{…, hostname}` again | **200**, the *same* `device_id` (even under a different `name`) |
| `{…, hostname, allow_duplicate: true}` | **201** — a new row, matching suppressed |
| `{…, mac_address}` | same as `hostname`, keyed on the MAC |

The status code is the signal: `201` created, `200` matched. But none of the
rows it creates carries a `client_device_identifier`, so none of them is the row
pairing made and none of them can be found again by this console. And a new
device has no sync history, which makes every save look like a first encounter
([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md)).

`DeviceCreateResponse` — note the id field is **`device_id`**, not `id`:

```json
{
  "device_id": "e5b50c67-6118-4da4-8122-a7acec65f8cf",
  "name": "probe device",
  "created_at": "2026-09-04T11:12:27.445281+00:00"
}
```

`DeviceCreatePayload` (key fields): `name, platform, client, client_version,
hostname, mac_address, sync_mode, sync_config, allow_existing, allow_duplicate,
reset_syncs`.

`device.never_post` holds this finding in the test suite, because a document
cannot go red.

### Reading the device back

`GET /api/devices/{device_id}` returns `DeviceSchema`
(`captures/devices-get.json`), where the id field is **`id`** — the same value
the token response calls `device_id`:

```json
{
  "id": "3e175584-2641-44bd-913b-e42d8fc64f85",
  "user_id": 1,
  "name": "probe_contract.py",
  "platform": "switch",
  "client": "rommsync-nx-probe",
  "client_version": "0.0.0",
  "ip_address": null, "mac_address": null, "hostname": null,
  "client_device_identifier": "probe-contract-script",
  "sync_mode": "api",
  "sync_enabled": true,
  "sync_config": null,
  "last_seen": "…", "created_at": "…", "updated_at": "…"
}
```

Three of these decide whether the console can sync at all, and all three are
worth reading at boot rather than meeting mid-sync:

| Observed | Meaning |
|---|---|
| `404 {"detail": "Device with ID … not found"}` | deleted in the web UI. The token still works and `GET /api/devices` still lists — only this device is gone. `POST /api/sync/negotiate` answers the same 404. |
| `sync_enabled: false` | the user's own switch. `POST /api/sync/negotiate` answers `400 {"detail": "Sync is disabled for this device"}`. |
| `client_device_identifier` | the only field that ties a row to a console. `null` on every device RomM did not pair, so it is also the only way to search the list for one. |

A `401` here is the token revoked, not expired ([AUTH.md](AUTH.md#re-pairing--revocation)).

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
device_id: string?   # optional only with a device-bound client token
saves: array   *required   # ClientSaveState, one per local save the client found
```

`ClientSaveState` — **verified field names**, required ones marked:
```
rom_id: integer          *   # matched locally, see SYNC_PROTOCOL.md step 0
file_name: string        *
updated_at: date-time    *   # the client file's mtime, UTC
file_size_bytes: integer *
slot: string?                # saves pair on (rom_id, slot); null means "archival"
emulator: string?
content_hash: string?        # MD5 of the file bytes, NOT SHA1
```
`content_hash` is an **MD5** — roms carry `sha1_hash`/`md5_hash`/`crc_hash`, but
saves are compared on MD5 alone, so a SHA1 here makes every unchanged save look
changed. A `null` slot is never paired with a slotted server save, so a
null-slot client save always negotiates as `upload`.

`SyncNegotiateResponse` (`captures/sync-negotiate-upload.json`):
```json
{
  "session_id": 56,
  "operations": [
    {
      "action": "upload",
      "rom_id": 4,
      "save_id": null,
      "file_name": "probe.srm",
      "slot": "probe-6f164f17-a",
      "emulator": "probe-emulator",
      "reason": "Save exists on client but not on server",
      "server_updated_at": null,
      "server_content_hash": null
    }
  ],
  "total_upload": 1,
  "total_download": 0,
  "total_conflict": 0,
  "total_no_op": 0
}
```

`SyncOperationSchema`:
```
action: "upload" | "download" | "conflict" | "no_op"   # no_op, with the underscore
rom_id: integer
save_id: integer?          # the server save (null for a save the server lacks)
file_name: string          # the SERVER's name once a save exists; see below
slot: string?
emulator: string?
reason: string
server_updated_at: string?
server_content_hash: string?
```

The `reason` strings are the server's arbitration. This is the **complete** set
5.2.0 can emit — read off `endpoints/sync.py` and `handler/sync/comparison.py`,
because only the five marked ✔ appear in the captures and are therefore guarded
by `ctest -R contract`. Treat an unlisted reason as a server that moved:

| `action` | `reason` | When | |
|---|---|---|---|
| `upload` | `Save exists on client but not on server` | no server save for that `(rom_id, slot)` | ✔ |
| `upload` | `Client save is newer (no sync history)` | no sync record, client timestamp is later | |
| `upload` | `Client save is newer than last sync` | only the client moved past the sync record | |
| `download` | `Save exists on server but not on client` | the client did not report that pair, and never synced it | ✔ |
| `download` | `Server save is newer (no sync history)` | no sync record, server timestamp is later | |
| `download` | `Server save is newer than last sync` | only the server moved past the sync record | |
| `download` | `Server save updated since last sync, not present on client` | the client dropped a pair that has since changed | |
| `conflict` | `Both sides changed since last sync` | both moved past the sync record | ✔ |
| `conflict` | `Same timestamp but different content` | no sync record, equal timestamps, different hashes | ✔ |
| `no_op` | `Content is identical` | hashes match — checked before anything else | ✔ |
| `no_op` | `No changes since last sync` | neither side moved past the sync record | |
| `no_op` | `Saves appear identical` | no sync record, equal timestamps, hashes not comparable | |
| `no_op` | `Save is untracked on this device` | the device's sync row is marked untracked |  |

Both `conflict` reasons matter to the client: hashes are compared first, so a
`conflict` is always two genuinely different files, and the second reason needs
no sync history at all — RomM's timestamps are second-granular, so a local write
in the same second as the server's copy reaches it
([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#conflicts)).

**Verified — negotiate is how the client discovers server-only saves.** Every
save the device has no sync history for comes back as a `download`, including
for roms the client never mentioned; an empty `saves` array is a legitimate
"tell me everything I am missing". It does *not* report saves the device is
already in sync with, so an empty request from a fully-synced device returns an
empty `operations` array.

`SyncCompletePayload`: `operations_completed, operations_failed, play_sessions[]`
→ `SyncCompleteResponse { session, play_session_ingest }`
(`captures/sync-complete.json`):
```json
{
  "session": {
    "id": 22,
    "device_id": "55a783e8-d1f4-49db-9bab-d7b49274b4fe",
    "user_id": 1,
    "status": "COMPLETED",
    "initiated_at": "2026-09-04T11:12:27+00:00",
    "completed_at": "2026-09-04T11:12:27+00:00",
    "operations_planned": 0,
    "operations_completed": 1,
    "operations_failed": 0,
    "error_message": null,
    "created_at": "2026-09-04T11:12:27+00:00",
    "updated_at": "2026-09-04T11:12:27+00:00"
  },
  "play_session_ingest": null
}
```
`status` is upper-case. `operations_planned` counts the operations that need
*work*, so a plan of nothing but `no_op` is planned `0` — do not treat
`operations_completed > operations_planned` as an error.

## Save & state I/O (used while executing a plan)

| Method | Path | Notes |
|---|---|---|
| GET | `/api/saves` | list; filter `rom_id, platform_id, device_id, slot` |
| POST | `/api/saves` | upload; query `rom_id*, emulator, slot, device_id, session_id, overwrite, autocleanup, autocleanup_limit`; multipart field **`saveFile`** (+ optional `screenshotFile`) |
| PUT | `/api/saves/{id}` | replace one save's bytes in place; query `device_id` |
| GET | `/api/saves/{id}/content` | download raw save bytes |
| GET | `/api/saves/identifiers` | lightweight id/hash listing |
| POST | `/api/saves/{id}/downloaded` | `{device_id}` — record that this device now holds this save |
| POST | `/api/saves/delete` | `{saves: [id, …]}` |
| GET/POST | `/api/states` (+ `/{id}/content`) | same shape for save states; field **`stateFile`** |

`SaveSchema`, the real response to an upload (`captures/saves-post.json`):
```json
{
  "id": 9,
  "rom_id": 4,
  "user_id": 1,
  "file_name": "probe [2026-09-04_11-12-27].srm",
  "file_name_no_tags": "probe",
  "file_name_no_ext": "probe [2026-09-04_11-12-27]",
  "file_extension": "srm",
  "file_path": "users/557365723a31/saves/nes/4/probe-emulator",
  "file_size_bytes": 23,
  "full_path": "users/557365723a31/saves/nes/4/probe-emulator/probe [2026-09-04_11-12-27].srm",
  "download_path": "/api/saves/9/content?timestamp=2026-09-04 11:12:27.553771+00:00",
  "missing_from_fs": false,
  "created_at": "2026-09-04T11:12:27.553770+00:00",
  "updated_at": "2026-09-04T11:12:27.553771+00:00",
  "emulator": "probe-emulator",
  "slot": "probe-e7d68684-a",
  "content_hash": "abd8fff93894e8112c7dd17386e54a5f",
  "is_public": false,
  "screenshot": null,
  "origin_device_id": "55a783e8-d1f4-49db-9bab-d7b49274b4fe",
  "device_syncs": [
    {
      "device_id": "55a783e8-d1f4-49db-9bab-d7b49274b4fe",
      "device_name": "probe-e7d68684-client",
      "last_synced_at": "2026-09-04T11:12:27+00:00",
      "is_untracked": false,
      "is_current": false
    }
  ]
}
```

Three things to write code against, all verified:

- **RomM renames on ingest.** `probe.srm` went up; `probe [2026-09-04_11-12-27].srm`
  came back. Every later operation echoes the *server's* name, so never match a
  local file by the `file_name` an operation carries — match on `(rom_id, slot)`
  and keep your own name.
- **`content_hash` is an MD5** (32 hex), unlike a rom's `sha1_hash`.
- **`download_path` is not a safe URL.** It carries a raw timestamp with a space
  and a `+` in the query string. Percent-encode it, or ignore it and build
  `/api/saves/{id}/content` yourself.

`device_syncs[]` is the per-device sync history negotiate arbitrates against:
`{device_id, device_name, last_synced_at, is_untracked, is_current}`. A row is
written by **both** sides of the loop, as long as `device_id` is passed: an
upload writes one at the save's new `updated_at` (visible in the capture above),
and `POST /api/saves/{id}/downloaded` writes one for a save this device pulled.
Skip either and the device stays in negotiate's no-sync-history branch forever.

**An upload needs `overwrite=true`.** With a `device_id` and a `slot`, `POST
/api/saves` answers `409 {"detail": "Slot has a newer save since your last
sync"}` when this device has no sync row for the slot's current save — which is
exactly the state negotiate calls `Client save is newer (no sync history)` and
tells the client to upload. See
[SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#step-2--execute-the-plan).

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

`GET /api/roms` returns an envelope, not a bare array
(`captures/roms-list.json`): `{items, total, limit, offset, char_index,
rom_id_index, filter_values}`.

Key rom fields for the client: `id, platform_fs_slug, platform_slug,
platform_display_name, fs_name, fs_name_no_ext, fs_name_no_tags, fs_extension,
fs_path, fs_size_bytes, sha1_hash, md5_hash, crc_hash, has_multiple_files,
has_simple_single_file, has_nested_single_file, files, name, missing_from_fs`.
An unscanned or metadata-less library leaves most of the ~70 other fields
`null` — the capture is from a fixture with no metadata providers, which is the
worst case the client must survive.

### Platform → folder mapping

`platform_fs_slug` is the on-disk slug (e.g. `snes`, `gba`, `psx`, `n64`,
`genesis`, `nds`, `psp`, `dreamcast`). The client maps each to a Tico /
RetroArch SD folder; default map + override scheme in [CONFIG.md](CONFIG.md).

## Resume & integrity

- Range requests are supported on `content` endpoints (SwitchRomM already relies
  on `Range` resume) — use them for large-rom downloads on flaky links.
- Verify **rom** downloads against `sha1_hash`/`md5_hash` from the rom schema,
  and **save** downloads against the `content_hash` MD5.
- Saves are matched to roms by `fs_name_no_ext` (exact) with `fs_name_no_tags`
  fallback, scoped by platform when known — see [SYNC_PROTOCOL.md](SYNC_PROTOCOL.md).
