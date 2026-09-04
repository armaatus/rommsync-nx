# Save sync protocol (client side)

How `sys-rommsync` implements RomM 5.2.0's negotiate → execute → complete loop.
Endpoints and schemas: [API_CONTRACT.md](API_CONTRACT.md). The payload and
response shapes below are the ones captured from a live 5.2.0 under
`server/contract/captures/` (issue M0-4), not a reading of the OpenAPI snapshot.

## Principle: the server is the source of truth

The client never decides a conflict on its own. It reports what it has (hashes +
timestamps), the server returns a per-save action, and the client obeys. This is
what keeps *every* device and the server converged.

## Step 0 — matching local files to roms

For each file under the configured save/state dirs:

1. Strip the save/state suffix to get the base name (`Game (USA).srm` → `Game (USA)`).
2. Match to a rom by `fs_name_no_ext` (exact), falling back to `fs_name_no_tags`.
3. Scope by platform when the path implies one (Tico's per-system folders do;
   RetroArch's flat `saves/` doesn't — match by name across the library then).
4. Ambiguous match (same base name on two platforms, no platform hint) → skip and
   log; never guess.

Cache the rom index (`GET /api/roms`) per tick; it's also needed by downloads.

## Step 1 — negotiate

Build `SyncNegotiatePayload`. Each entry is a `ClientSaveState`; the field names
are verified, and three of them are not what an obvious guess produces —
`content_hash` (not `hash`), `file_size_bytes` (not `size`), and an **MD5**, not
the SHA1 the rom schema uses:

```jsonc
{
  "device_id": "<the device_id from the token, or one cached from /api/devices>",
  "saves": [
    {
      "rom_id": 1234,                          // required
      "file_name": "Game (USA).srm",           // required
      "updated_at": "2026-09-03T12:00:00Z",    // required — FS mtime, UTC
      "file_size_bytes": 32768,                // required
      "slot": "autosave",                      // optional, but see below
      "emulator": "retroarch",                 // or "tico"
      "content_hash": "<MD5 of the file bytes>"
    }
    // ... one per local save
  ]
}
```

**Saves pair on `(rom_id, slot)`.** A stable slot name is what keeps one save in
sync across ticks; a `null` slot means "archival, manual upload" to RomM and is
never paired with a slotted server save, so it negotiates as `upload` every time
even when the identical bytes are already there. Pick a slot and keep it.

That entry is `rommsync::sync::ClientSaveState`
([`core/include/rommsync/sync.hpp`](../core/include/rommsync/sync.hpp)), and
`EncodeNegotiateRequest` is the only thing that writes this body. It refuses,
with the field named, a save it cannot send faithfully — a SHA1 or an uppercase
digest in `content_hash`, a blank `slot` (which is neither a slot nor archival),
an `updated_at` at the epoch. Every one of those is a save RomM would accept and
then arbitrate as something the client did not mean; see
[API_CONTRACT.md](API_CONTRACT.md#save-sync--negotiate--execute--complete) for
what the server does and does not complain about.

`updated_at` is the file's mtime in UTC, whole seconds, `…Z`. RomM stores these
at second granularity and compares with a strict `>`, so sub-second precision is
dropped *downwards* — rounding 11:36:27.9 up to :28 would claim a file is newer
than it is and win an arbitration it should have lost. A timestamp at or before
the epoch is refused rather than sent: that is a console whose clock never got
set, and the server would read it as "very old" and plan a `download` over what
may be the only copy of the save.

`POST /api/sync/negotiate` → `SyncNegotiateResponse { session_id, operations[],
total_upload, total_download, total_conflict, total_no_op }`.

The plan also covers saves the client did **not** report: any server save this
device has no sync history for comes back as a `download`, for any rom. That is
how the client learns a save exists at all — there is no separate "what's new"
call to make. An empty `saves` array is a legitimate "tell me what I'm missing".

A server save the device *has* synced and that has not changed since is read as
"the client deleted it on purpose" and produces **no operation at all** — so a
save the client loses locally is not silently restored. Untracked saves
(`is_untracked` on the device's sync row) come back as `no_op` /
`Save is untracked on this device`.

## Step 2 — execute the plan

For each `SyncOperationSchema`. The action is `no_op` — with the underscore:

| action | do |
|---|---|
| `no_op` | nothing |
| `upload` | `POST /api/saves?rom_id=&emulator=&slot=&session_id=&device_id=&overwrite=true`, multipart `saveFile` = local bytes |
| `download` | `GET /api/saves/{save_id}/content` → **back up existing local file** → write to the file's SD path → `POST /api/saves/{save_id}/downloaded {device_id}` |
| `conflict` | resolve by policy (below) — **always back up the loser first** |

**`overwrite=true` is not optional on an upload.** Given a `device_id` and a
`slot`, RomM refuses the post with `409 {"detail": "Slot has a newer save since
your last sync"}` whenever this device has no sync row for the slot's current
save, or one older than it. That is precisely the plan's own
`upload` / `Client save is newer (no sync history)` case — the first upload after
a re-pair, or from a device registered this boot. Executing a plan the server
just issued must not be rejected by the server, so send `overwrite=true` and let
negotiate be the arbiter.

Three traps in the operation itself:

- **`file_name` is the server's name, not yours.** RomM renames a save on ingest:
  `probe.srm` is stored as `probe [2026-09-04_11-12-27].srm`, and every later
  operation echoes that. Match the operation back to your local file on
  `(rom_id, slot)` and keep your own path; writing the server's name to the SD
  produces a file no emulator will load.
- **That rename is also why a second upload is a second save.** The datetime tag
  is part of the stored name, so `POST /api/saves` for a slot that already has a
  save creates a *new row* a second later — `overwrite=true` included — and the
  new row has no sync history for this device. The next negotiation therefore
  falls into the no-history branch and answers
  `upload / Client save is newer (no sync history)` rather than comparing against
  the last sync. Only `PUT /api/saves/{id}` moves an existing row forward.
  `harness.conflict` depends on that distinction, and says so.
- **`save_id` is null for an `upload`** when the server has nothing yet, and set
  when it has an older copy. Don't dereference it unconditionally.

Pass `session_id` on uploads so the server ties them to this session, and
`device_id` so the sync history that the *next* negotiation arbitrates against is
written. Track `operations_completed` / `operations_failed`.

Backups go to `sdmc:/config/rommsync/.backup/<rom_id>-<ts>.<ext>`. Never destroy
a save without a backup — this is a hard rule.

That name carries neither the slot nor the save's own name, so **two saves of one
rom backed up in the same second are the same file**, and the second backup
destroys the first — one rom with two slots, or a save and its state, is enough.
Whatever M2-5 writes has to disambiguate them; `harness.backup` currently steps
around the collision rather than hiding it (`Sandbox::BackupPathFor`).

### Conflicts

RomM does *not* send a resolution: there is no `server_wins` / `keep_both` field
to obey. The client picks a policy, and the safe default is **keep both**: back
up the local file, write the server copy to the save path, and leave the backup
where the overlay can surface it (M7-1). `server_content_hash` and
`server_updated_at` are there to show the user what they are choosing between.

**A `conflict` arrives with one of two reasons, and a client must handle both:**

- `Both sides changed since last sync` — the expected case, when this device has
  a sync record and each side moved past it.
- `Same timestamp but different content` — **no sync history**, the two
  timestamps are equal, and the hashes are not. RomM compares these at second
  granularity, so this is not exotic: a save written in the same second as the
  server's copy, seen by a device that has not synced it, lands here. Switching
  only on the first reason drops this one into whatever the default branch does,
  and on a conflict the default branch is the one that can overwrite a save.

The arbitration is per `(device, save)`. Sync history is written by an upload
carrying `device_id` and by `POST /api/saves/{id}/downloaded` — a download the
client never confirms leaves the device looking like it has never seen the save,
which puts every later comparison in the no-sync-history branch above.

## Step 3 — complete

`POST /api/sync/sessions/{session_id}/complete`:
```jsonc
{ "operations_completed": 7, "operations_failed": 0, "play_sessions": [] }
```
→ `{ "session": SyncSessionSchema, "play_session_ingest": null }`. The session's
`operations_planned` counts operations that need *work*, so a plan of nothing but
`no_op` reports `0` planned against however many you completed; that is not an
error.

Then persist the new per-save `{content_hash, mtime, server_updated_at,
server_content_hash}` to `state.db`. That stored baseline is how the *next* tick
skips work — the server arbitrates, but the client still has to know which local
files are worth re-hashing.

## Change detection between ticks

The negotiate call already computes the plan server-side, so the client can send
all saves every tick and let the server decide. `state.db` is an optimization:
skip re-hashing unchanged files (mtime+size match the stored baseline) to keep
ticks cheap on large libraries.

## Save states

Same flow via `/api/states` (field `stateFile`). **Off by default** — states are
core- and version-specific; only enable when your devices run matching cores.
Config: `sync.states = true`.

## Failure & safety rules

- Offline / unreachable server → abort the tick cleanly, change nothing, retry
  next schedule. Missed ticks are harmless.
- Partial plan failure → complete with the accurate `operations_failed`; leave
  unsynced files for next tick. Never leave a half-written save (write to temp,
  fsync, rename).
- One backup per overwrite, always, before writing.
- Verify a downloaded save against the MD5 in `content_hash` /
  `server_content_hash`, and its length against `file_size_bytes`.
