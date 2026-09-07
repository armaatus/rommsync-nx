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
platforms.read
me.write            # guards PUT /api/users/{id}, /api/client-tokens and device approve/deny
```

This block is not prose: the `auth.scopes` test parses it and compares it to
`MinimumScopes()`, so editing one without the other goes red. Anything qualified
with a `#` comment is documented and *not* requested.

`me.write` is the one that is documented and deliberately not requested, and the
reason it is written down at all is that an obvious guess gets it wrong: it is
**not** the scope for recording play sessions. In 5.2.0's snapshot it guards
`PUT /api/users/{id}`, the `/api/client-tokens` family and
`/api/auth/device/{approve,deny}` — none of which this client calls.
`POST /api/play-sessions` is guarded by `roms.user.write`, which is on the
unconditional list already, so M7-4 (#39) needed no new scope at all.

`platforms.read` is the one that is not obvious, and it was found by being
refused. `GET /api/platforms` declares it in 5.2.0's OpenAPI and answers
`403 {"detail":"Forbidden"}` to a token granted every other scope on this list —
so the overlay's library browser (M4-3, #25) could not have listed a single
platform without it. Nothing else on this API can stand in: `GET /api/roms`
carries a `filter_values.platforms`, and it holds bare ids with no slug, no name
and no rom count.

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

**Verified — it never matches on `client_device_identifier`.** RomM deduplicates
on what it calls a device's *fingerprint*, which is `hostname` **or**
`mac_address` and nothing else. `name`, `platform`, `client` and
`client_device_identifier` are not part of it, and calling this with the
device-bound client token *of the very device being described* does not change
that. Every row below was run against a live 5.2.0 as the paired console:

| Call, as the paired console | Result |
|---|---|
| `{name, platform, client}`, any `allow_existing` | **201**, a new `device_id`, every single time — with no fingerprint there is nothing to match, so the flag has nothing to act on |
| `{…, hostname}` / `{…, mac_address}` first call | **201**, a new `device_id` |
| …again | **200**, the *same* `device_id` — even under a different `name` |
| …again with `allow_existing: false` | **409** `{"detail": {"error": "device_exists", "message": "A device with this fingerprint already exists", "device_id": "…"}}` |
| …again with `allow_duplicate: true` | **201** — a new row, matching suppressed |

The status code is the signal: `201` created, `200` matched, `409` matched and
refused. So the mechanism works — it is simply keyed on the wrong thing. A
Switch sysmodule has no stable hostname or MAC to offer, and the identifier it
*does* have is not consulted, so every call creates a row that carries no
`client_device_identifier`: not the row pairing made, and not one this console
can ever find again. A new device also has no sync history, which makes every
save look like a first encounter ([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md)). Even
RomM's own dedup, working exactly as designed, cannot reach the paired device —
`device.never_post` asserts that against the `409`.

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

The client does not build this body by hand: `core/include/rommsync/sync.hpp`
types it (M2-1), and `sync.payload` compares that struct against the
`ClientSaveState` schema in the committed snapshot on every run, in both
directions — a field the snapshot grows and a field the encoder invents are both
red.

**Verified — only half a typo is loud.** Rename a *required* field and RomM
answers **422** naming it (`sync.required` sends `size` for `file_size_bytes`).
Rename an *optional* one and RomM answers **200**: the unknown key is ignored,
the field defaults to `null`, and only the plan changes. `sync.understood` sends
one identical save twice, differing in nothing but `content_hash` vs `hash`:

| sent as | plan |
|---|---|
| `content_hash` | `no_op` — `Content is identical` |
| `hash` | `upload` — `Client save is newer than last sync` |

Nothing in the response says a field was dropped, so a client that guesses this
name re-uploads every save on every tick and never learns why. `updated_at`
tolerates the same kind of near-miss: an offsetless `2026-09-04 11:36:27` is
accepted rather than refused, and what the server then takes it to mean is its
business, not the client's — so send the offset. `FormatTimestamp` writes `Z`.

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

Only `action`, `rom_id`, `file_name` and `reason` are in the schema's `required`
list. That is a pydantic default rather than a promise that the other five may be
absent: 5.2.0 emits **all nine** on every operation, as every capture under
`server/contract/captures/` shows. `ParseNegotiateResponse` therefore reads all
nine, the five nullable ones as `T | null`, so a server that genuinely stopped
sending one is a named error rather than a field that silently defaulted — and
`sync.payload` pins that reading against the snapshot in both directions.

The response is `rommsync::sync::SyncPlan`
([`core/include/rommsync/sync.hpp`](../core/include/rommsync/sync.hpp)) and
`sync::Negotiate` is the only thing that makes this call (M2-4). It sends
`device_id` even though the snapshot marks it optional, refuses a record that
names no device rather than negotiating as nobody, and separates the three
failures a bare "network error" would hide: `404` (deleted in the web UI), `400
Sync is disabled for this device`, and `401` (revoked — there is nothing to
refresh). A `403` is kept apart from that `401`: RomM approves what the *user*
ticked, so a 403 here is a scope missing from a pairing that otherwise works,
and reporting it as a revocation sends the user looking for something that did
not happen. Read `scopes` back off the token instead of meeting it here.

**Verified — negotiating twice cancels the first session.** Each
`POST /api/sync/negotiate` opens a session, and RomM cancels the device's
previous `IN_PROGRESS` one when it does: two negotiations in a row leave the
first `CANCELLED` with a `completed_at`, and exactly one row is ever in progress
per device. So a retried negotiation — a stall the client gave up on, a tick that
died before `complete` — does not accumulate open sessions, and only the last
`session_id` is worth posting uploads and `complete` against.

**The `server_content_hash` is whatever some client uploaded.** RomM stores the
digest it is given, so a save another tool put there can come back carrying a
SHA1 or an uppercase digest — which compares equal to nothing, forever, with no
symptom except that save negotiating as changed on every tick. It is the same
failure `ClientSaveState.content_hash` is validated against, arriving from the
other direction; `ParseNegotiateResponse` reports it rather than refusing the
plan, because it is one save's problem and not the plan's.

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

An `action` or a `reason` that is not in the tables above is a server that moved.
`ClassifyAction` pins the unknown action to `no_op` and reports it, because on a
save the default branch is the one that can overwrite it; an unrecognised
`reason` leaves the action obeyed and is reported the same way. `sync.plan` holds
both tables to this document.

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
→ `SyncCompleteResponse { session, play_session_ingest }`. The capture below is
a completion that carried **no** play sessions, which is why the ingest is
`null`; what a non-null one looks like is under
[Play sessions](#play-sessions) below.
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

**Verified — completing a session twice is a `400`, not a no-op.** The second
`POST .../complete` for the same id answers
`400 {"detail":"Session is already COMPLETED"}`. That is on the ordinary path
rather than an exotic one: every network call retries with backoff, so a first
attempt that reached RomM before the connection died makes the *second* attempt
land here — and it means the accounting succeeded, which is the opposite of what
a bare "the server refused the body" would say. `sync::CompleteError` therefore
carries `kAlreadyCompleted` for it, separately from `kRejected`.

**Verified — completing a session another negotiation cancelled is also a
`400`, with a different detail: `{"detail":"Session is already CANCELLED"}`.**
This is the *reachable* half of the pair. Each negotiate cancels the device's
previous `IN_PROGRESS` session (above), so a tick that gave up on one
negotiation and made another is completing a session RomM has already ended. It
says the opposite of the `COMPLETED` detail about the accounting — those counts
were never recorded and never will be — so `sync::CompleteError` carries
`kSuperseded` for it, apart from both `kAlreadyCompleted` and `kRejected`.
Nothing about the body is wrong; the answer is the next tick's own negotiation.

**Verified — an unknown session id is a `404`
`{"detail":"Sync session with ID N not found"}`.** A session RomM *cancelled*
still exists and answers the `400` above, so reaching this means an id that was
never a session — or a server that is not RomM. The client gates on the detail
rather than on the status for that second reason, the same one the device `404`
is gated on: a `server_url` pointing at something which is not this RomM answers
`404` too, and FastAPI's own is `{"detail":"Not Found"}`.

**`SyncSessionSchema` has twelve fields, not the six a completion is about.**
`user_id`, `initiated_at`, `completed_at`, `created_at` and `updated_at` are on
it as well. Ten of the twelve are declared required; `completed_at` and
`error_message` are not, and the client reads them strictly anyway — the same
stance it takes on the five optional fields of a negotiate operation, because
5.2.0 emits all twelve on every response and a server that stopped would be a
change worth a named error. `completed_at` is the one that says the session
actually closed.

**Verified — a save the client reports and is already in sync with is answered
with an explicit `no_op`, not with an absence.** Reporting a save whose digest
matches the server's comes back as an operation with
`reason: "Content is identical"`, which is what `captures/sync-negotiate-no-op.json`
holds. What negotiate leaves out of a plan is a save the client did not mention
in `saves[]` — so "it does not report saves the device is already in sync with"
above is about an *empty* request, not about a save that was reported. A client
committing a baseline can therefore rely on every reported save carrying an
operation.

## Play sessions

How long a rom was played, so a library's play time reflects the console and not
just the desktop (M7-4, #39). Optional in the strongest sense: nothing depends on
it and a failure here must never fail a sync tick.

| Method | Path | Scope | Notes |
|---|---|---|---|
| POST | `/api/play-sessions` | `roms.user.write` | ingest; `PlaySessionIngestPayload` → **201** `PlaySessionIngestResponse` |
| GET | `/api/play-sessions` | `roms.user.read` | read back; query `rom_id, device_id, start_after, end_before, limit, offset` |
| DELETE | `/api/play-sessions/{session_id}` | — | not used by this client |

**`roms.user.write`, not `me.write`.** Both scopes are already covered above; the
mistake is worth stating twice because the wrong one is a plausible guess and is
a scope this client refuses to request.

`PlaySessionEntry` (the ingest) and `SyncPlaySessionEntry` (the completion) are
**field-for-field identical** in the snapshot, which is why the client spells the
entry once (`sync::PlaySession`):

```jsonc
{ "rom_id": 4, "save_slot": "retroarch-srm",   // both nullable
  "start_time": "2026-09-04T11:12:27Z",        // required, date-time
  "end_time":   "2026-09-04T11:36:27Z",        // required, date-time
  "duration_ms": 1440000 }                     // required, >= 0
```

So there are two routes and the client prefers the cheaper one: the sync tick is
already making a request, and `SyncCompletePayload.play_sessions[]` rides on it.
`POST /api/play-sessions` is for flushing without a sync.

The answer is the same shape either way — `play_session_ingest` on a completion is
a whole `PlaySessionIngestResponse` or `null`:

```json
{
  "results": [
    { "index": 0, "status": "created", "id": 9, "detail": null },
    { "index": 1, "status": "duplicate", "id": null, "detail": null }
  ],
  "created_count": 1,
  "skipped_count": 1
}
```

**Verified — a session that ends in the future is answered `error`, not `422`.**
The snapshot bounds nothing but `duration_ms >= 0`; the live 5.2.0 also refuses a
future `end_time`, per entry, with
`{"index":0,"status":"error","id":null,"detail":"end_time is too far in the future"}`
while the request as a whole is still a **201**. So a console whose clock runs
ahead of the server loses those sessions rather than the request — which is why
`play::DeriveSessions` refuses an mtime later than the tick's own clock before it
ever gets here, and why the rig scenarios anchor their windows to the clock
rather than to a fixed instant.

**Ingest is not all-or-nothing, and `duplicate` is success.** `status` is one of
`created`, `duplicate`, `error`, per entry, indexed by position in what was sent.
That is what makes a retried flush safe: an attempt that reached RomM before the
connection died has already written the rows, and the retry is answered
`duplicate` for every one of them. A client that read that as a failure would
re-send its whole buffer forever.

## Save & state I/O (used while executing a plan)

| Method | Path | Notes |
|---|---|---|
| GET | `/api/saves` | list; filter `rom_id, platform_id, device_id, slot` |
| POST | `/api/saves` | upload; query `rom_id*, emulator, slot, device_id, session_id, overwrite, autocleanup, autocleanup_limit`; multipart field **`saveFile`** (+ optional `screenshotFile`) |
| PUT | `/api/saves/{id}` | replace one save's bytes in place; query `device_id` |
| GET | `/api/saves/{id}` | one save → `SaveSchema`; query `device_id` — see `device_syncs` below |
| GET | `/api/saves/{id}/content` | download raw save bytes; query `device_id`, `session_id` — **do not send them**, see below |
| GET | `/api/saves/identifiers` | lightweight id/hash listing |
| POST | `/api/saves/{id}/downloaded` | `{device_id}` — record that this device now holds this save |
| POST | `/api/saves/delete` | `{saves: [id, …]}` |
| GET | `/api/states` | list; filter `rom_id, platform_id` — **no `device_id`, no `slot`** |
| POST | `/api/states` | upload; query `rom_id*, emulator`; multipart field **`stateFile`** (+ optional `screenshotFile`) |
| PUT | `/api/states/{id}` | replace one state's bytes in place |
| GET | `/api/states/{id}` | one state → `StateSchema` |
| GET | `/api/states/{id}/content` | download raw state bytes |
| POST | `/api/states/delete` | `{states: [id, …]}` — the client never calls this |

**A state is not a save with a different field name.** See
[Save states](#save-states) below; the row in this table is deliberately not the
save row with `state` substituted.

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

**Verified — reading a save back only shows `device_syncs` if the read names a
device.** `GET /api/saves/{id}` and `GET /api/saves` answer `"device_syncs": []`
unless the query carries `device_id`, whatever rows exist; with it, the row comes
back. The `id` is unchanged by asking, so it is a filter and not a write — the
`last_synced_at` it answers with is the one the upload wrote. This matters
because an empty array reads exactly like *"this device has never synced this
save"*, which is the state the whole arbitration hangs on: a client (or a test)
that checks its own history without passing `device_id` concludes that every
upload it just made wrote nothing. `execute.upload` and `execute.download` assert
on it, and pass `device_id`.

**`GET /api/saves/{id}/content` takes `device_id` and `session_id` too, and the
client must not use them.** Passing `device_id` there records the download at the
moment the bytes are *requested*, before anything has verified them, so a
truncated body or one that fails its MD5 would still leave the device claiming it
holds the save. `sync::ExecutePlan` fetches the content unadorned and calls
`POST /api/saves/{id}/downloaded` only after the verified bytes are on the card
(docs/SYNC_PROTOCOL.md#the-order-every-overwrite-goes-through).

**A negotiate operation carries no size, so an executing client needs
`GET /api/saves/{id}`.** `SyncOperationSchema` has nine fields and
`file_size_bytes` is not one of them, and without a size
`http::DownloadTarget::expected_size` is zero — which means a body that ends
cleanly and early cannot be told from a complete one, because RomM's own
`Content-Length` is what the shortening removed. That is the shape
`server/testing/fault_proxy.py`'s `truncate` mode produces on purpose.

**An upload needs `overwrite=true`, and the flag does exactly one thing.** With
a `device_id` and a `slot`, `POST /api/saves` answers `409 {"detail": "Slot has a
newer save since your last sync"}` when this device has no sync row for the
slot's current save — which is exactly the state negotiate calls `Client save is
newer (no sync history)` and tells the client to upload. The flag clears that
check and nothing else; it is read well before the row is looked up.
`execute.occupied` arranges the state and asserts the 409 on the flagless
request, then asserts the same upload succeeds with the flag. A previous
revision said the flag did a *second* thing — see below; it does not.

**Verified — whether an upload replaces a row or adds one is decided by the
clock, not by the flag.** RomM stamps a slot upload with a second-granularity
datetime tag and looks the existing row up by that tagged name
(`_apply_datetime_tag`, `get_save_by_filename`; `endpoints/saves.py` in 5.2.0),
so a POST lands on the existing row only when it shares the base file name **and
the wall-clock second** of the ingest that created it. One second later it
creates a **second** row, `overwrite=true` included. Measured directly: two
uploads of the same file in one second answered the same `id`; the next, two
seconds later, answered a new one.

A previous revision of this section claimed the flag "replaces the row in place,
tag and id included" and that this is what keeps a slot from accreting a row per
tick. That was a same-second run read as a rule, and it is what made
`execute.occupied` pass locally and fail in CI (issue #85). **A slot does accrete
a row per upload**, which is what `autocleanup` / `autocleanup_limit` on this
endpoint exist for, and which is a reason not to re-post an upload inside a tick.
`PUT /api/saves/{id}` also moves a row forward, and unlike either POST it takes
no `slot`, so it is what a test uses to change the server's copy without
touching a device's history.

## Save states

`captures/states-post.json` and `captures/states-list.json` are the real
responses; `contract.captures` re-checks both against a live server on every
`ctest`. What follows is what 5.2.0 does, verified, and it differs from
`/api/saves` in every way that matters to a client.

**`StateSchema` carries no `slot`, no `content_hash`, no `origin_device_id` and
no `device_syncs`.** Its fields are `id, rom_id, user_id, file_name,
file_name_no_tags, file_name_no_ext, file_extension, file_path, file_size_bytes,
full_path, download_path, missing_from_fs, created_at, updated_at, emulator,
is_public, screenshot`. There is no digest to verify a downloaded state against
and no per-device sync history to arbitrate one with.

**There is no `POST /api/states/{id}/downloaded`.** Nothing records that a device
holds a state, so the only record of it is the client's own `state.db`.

**`SyncNegotiatePayload` has only `saves`.** A state never appears in a negotiate
request, in a plan, or in the counts `POST /api/sync/sessions/{id}/complete`
carries. There is no server-side arbitration for a state at all.

**Verified — `POST /api/states` is an upsert keyed on `(rom_id, file_name)`
alone.** A second POST under a name the rom already has replaces that row's bytes
**in place**: the `id` is unchanged, `file_size_bytes` and `updated_at` move, and
the row's `emulator` is *moved* to whatever the second request said — so the
emulator is not part of the key, and two local states of one rom sharing a name
are one row on the server however different their folders are. Measured
directly, and `states.upsert` asserts it against the live fixture; the probe
fails loudly if it ever stops being true.

This is the opposite of `POST /api/saves`, which accretes a row per upload (see
above), and it is what makes an unconditional POST dangerous: it destroys
whatever was under that name, and RomM answers `200`. The client therefore only
ever POSTs to a name the rom does not have, or to the row its own baseline names
and has found unmoved
([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#save-states)).

**Verified — RomM does not rename a state on ingest.** A save sent as
`probe.srm` is stored as `probe [2026-09-04_11-12-27].srm`; a state sent as
`probe-6694c595.state` is stored under exactly that name (`captures/states-post.json`).
The server's `file_name` and the client's are the same string, which is what
makes `(rom_id, file_name)` usable as a pairing key on both sides — and it is why
nothing in the state path needs the tag-stripping a save's `file_name` demands.

**Scopes.** `assets.read` for the listing and the content, `assets.write` for the
upload — both already in [Scopes to request](#scopes-to-request), so states need
no re-pairing. A pairing that was granted the read scopes and not the write one
gets a `403` on every upload, which the client reports as a missing scope rather
than as a revocation.

## Library & downloads (the "browse + get games" side)

| Method | Path | Notes |
|---|---|---|
| GET | `/api/roms` | paged; filters incl. `collection_id`, `platform_ids`, `search_term`, `limit`, `offset`, `order_by` |
| GET | `/api/roms/{id}` | `DetailedRomSchema` |
| GET | `/api/roms/{id}/content/{file_name}` | download a single-file rom (`fs_name`) — a **zip** for a multi-file one, see below; `?file_ids=` selects files, and a single id serves that file raw |
| GET | `/api/roms/{id}/files` | list files of a multi-file rom |
| GET | `/api/roms/{id}/files/content/{file_name}` | download one file — `{id}` is the **RomFile** id, not the rom's |
| GET | `/api/collections` | user collections (curate a "Switch" collection to mirror) |
| HEAD | `/api/roms/{id}/content/{file_name}` | size / resume checks |
| GET | `/api/platforms` | **unpaged bare array** of `PlatformSchema`; needs `platforms.read` |

`GET /api/roms` returns an envelope, not a bare array
(`captures/roms-list.json`): `{items, total, limit, offset, char_index,
rom_id_index, filter_values}`.

`GET /api/platforms` is the opposite shape and the one exception on this API:
**no envelope, no `limit`, no `offset`** — 5.2.0 answers the whole list as a bare
array of `PlatformSchema` (`captures/platforms-list.json`). A client that wants
platforms a page at a time therefore has to page them on its own side, which is
what `lists::Service` does (M5-4, #31). Key platform fields for the client:
`id, slug, fs_slug, name, display_name, custom_name, rom_count, fs_size_bytes,
missing_from_fs`. `display_name` is `custom_name` when the user set one and
`name` otherwise, so it is the string to draw; `fs_slug` is what
`config::Config::platforms` is keyed by, and `id` is what `GET /api/roms`'s
`platform_ids` takes.

Key rom fields for the client: `id, platform_fs_slug, platform_slug,
platform_display_name, fs_name, fs_name_no_ext, fs_name_no_tags, fs_extension,
fs_path, fs_size_bytes, sha1_hash, md5_hash, crc_hash, has_multiple_files,
has_simple_single_file, has_nested_single_file, files, name, missing_from_fs`.
An unscanned or metadata-less library leaves most of the ~70 other fields
`null` — the capture is from a fixture with no metadata providers, which is the
worst case the client must survive.

### Multi-file roms, and the three things that are not what they look like

**The v1 decision: detect and skip.** A rom with `has_multiple_files` is never
downloaded. `download::EnqueueRom` refuses it with `ipc::Error::kMultiFile` from
the rom index the engine already holds, and the worker is the backstop for an
entry that reached the queue anyway — it settles `kSkipped` with a sentence
naming the disc set, so the overlay can say why nothing arrived. Verified
against a live 5.2.0 by `harness.multifile`, `download.multifile` and
`download.nested`; [ARCHITECTURE.md](ARCHITECTURE.md#explicitly-out-of-scope-v1)
records the same decision.

Three things here are silent, and together they are the reason:

- `GET /api/roms/{id}/content/{file_name}` on a rom with `has_multiple_files`
  does **not** serve a rom. It serves a zip RomM builds on the fly, with
  `Content-Type: application/zip` and **no `Content-Length`** — so a client that
  treats it like any other rom writes an archive to the SD under the rom's name
  and has nothing to verify its length against.
- **The rom-level digest is not the digest of anything you can download.** On
  the seeded two-disc fixture the rom reports `sha1_hash: 3aa785a3…` while its
  two discs report `4ee43964…` and `9b5aaf68…`; the per-file digests do match
  the bytes each `files[].id` serves, and the rom's matches neither. So the
  archive above cannot be checked against anything either. This, not effort, is
  why v1 skips: the alternative is an unverifiable file on the card under a
  rom's name, and the failure is silent — the client would report success.
- In `GET /api/roms/{id}/files/content/{file_name}` the `{id}` is the
  **`files[].id`**, not the rom id, and the `{file_name}` segment selects
  nothing at all — it only decorates the download name. Building that URL from a
  rom id and a file name returns `200` and the bytes of whatever file happens to
  carry that id: the wrong disc, with no error to notice. Take the id from
  `files[]`.

`has_multiple_files` is on the **list** schema as well as the detail one, so a
client can decide to skip without a second call per rom. `files[]` is not: it is
present and always empty on the list, and only `GET /api/roms/{id}` fills it.

#### The route a v2 would take

Recorded rather than built, and pinned by `harness.multifile` so that "recorded"
keeps meaning "still true". Two per-file paths exist on 5.2.0; both are keyed on
a **`files[].id`**, and they are not equally safe:

| Path | Notes |
|---|---|
| `GET /api/roms/{id}/content/{file_name}?file_ids=<one files[].id>` | **The one to use.** `{id}` is the **rom** id. With a single id it serves that file *raw*: `application/octet-stream`, a real `Content-Length`, `Accept-Ranges: bytes` (a `Range` request comes back `206`), and a `content-disposition` naming that file. Verify against that entry's `sha1_hash`. A `file_ids` value that is not this rom's file is a **404** — the id is scoped to the rom |
| `GET /api/roms/{id}/content/{file_name}?file_ids=a,b` | Two or more ids is the on-the-fly **zip** again, so `file_ids` is not a way around it: a v2 makes one call per file |
| `GET /api/roms/{id}/files/content/{file_name}` | Same bytes, but `{id}` is a **`files[].id`** and nothing scopes it to a rom, so a wrong id is somebody else's file with a `200`. Prefer the row above |

The HTTP is the easy half. What is actually missing for a disc set is on the
card: an `.m3u` listing the discs, a per-emulator folder layout to put them in,
and per-file verification. `config::ValidRomFileName()` is the check to reuse
for `files[].file_name` — those names come off the server's filesystem exactly
as `fs_name` does, and nothing else validates them.

#### `has_nested_single_file` is not a disc set

A rom is exactly one of `has_simple_single_file`, `has_nested_single_file` and
`has_multiple_files`. The middle one is a **directory holding exactly one
file**, and it is an ordinary download — the skip must not fire on it. Pinned by
`harness.multifile` and `download.nested` against the seeded
`Synthetic Nested Game`:

- the whole-rom `content` endpoint serves **the file itself** —
  `application/octet-stream`, a real `Content-Length` equal to
  `fs_size_bytes`, and `Accept-Ranges: bytes`, so Range resume works as usual;
- the rom-level `sha1_hash` **is** that file's digest, so the download verifies
  the way any other rom's does.

One consequence, and it is a real one: `fs_name` is the **directory's** name and
`fs_extension` is `""`, so the rom lands on the card *without* the inner file's
extension — `Synthetic Nested Game`, not `Synthetic Nested Game.bin`. The bytes
are right and the digest verifies them, so the entry settles `kDone` while
RetroArch and hbmenu, which pick a core by extension, will not load it. M3-4
pinned this with `download.nested` rather than changing it — the name is also
what `AlreadyOnTheCard` looks for and what the overlay renders — and **#92**
carries the decision. The name a fix would use is `files[0].file_name` from
`GET /api/roms/{id}`.

### Platform → folder mapping

`platform_fs_slug` is the on-disk slug (e.g. `snes`, `gba`, `psx`, `n64`,
`genesis`, `nds`, `psp`, `dreamcast`). The client maps each to a Tico /
RetroArch SD folder; default map + override scheme in [CONFIG.md](CONFIG.md).

## Resume & integrity

- Range requests are supported on `content` endpoints (SwitchRomM already relies
  on `Range` resume) — use them for large-rom downloads on flaky links. Proven
  against a live 5.2.0 by `harness.resume`, and consumed by the download worker:
  `download.resume` asserts the resumed request comes back **206** carrying only
  the bytes that were missing.
- Verify **rom** downloads against `sha1_hash`/`md5_hash` from the rom schema,
  and **save** downloads against the `content_hash` MD5. Both rom digests are
  `string | null` and both are on the detail *and* the list schema; an unscanned
  library leaves them null, and M3-3 records such a download as unverified rather
  than passing it off as checked (`download.fallback`).
- `truncate` — a body that ends early with no `Content-Length` at all — is caught
  only by the client's own expected size, so a rom download always sets
  `DownloadTarget::expected_size` from `fs_size_bytes` (`download.truncate`).
- Saves are matched to roms by `fs_name_no_ext` (exact) with `fs_name_no_tags`
  fallback, scoped by platform when known — see [SYNC_PROTOCOL.md](SYNC_PROTOCOL.md).
