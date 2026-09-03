# Save sync protocol (client side)

How `sys-rommsync` implements RomM 5.2.0's negotiate → execute → complete loop.
Endpoints and schemas: [API_CONTRACT.md](API_CONTRACT.md).

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

Build `SyncNegotiatePayload`:
```jsonc
{
  "device_id": "<cached device id>",
  "saves": [
    {
      "rom_id": 1234,
      "file_name": "Game (USA).srm",
      "slot": null,
      "emulator": "retroarch",      // or "tico"
      "hash": "<sha1 of file bytes>",
      "updated_at": "2026-09-03T12:00:00Z",  // from FS mtime, UTC
      "size": 32768
    }
    // ... one per local save
  ]
}
```
> Confirm exact entry field names against the snapshot (`SyncSaveEntry` /
> negotiate payload `saves` item) before coding — tracked as issue **M2-1**.

`POST /api/sync/negotiate` → `SyncNegotiateResponse { session_id, operations[],
total_* }`.

## Step 2 — execute the plan

For each `SyncOperationSchema`:

| action | do |
|---|---|
| `noop` | nothing (hashes already match) |
| `upload` | `POST /api/saves?rom_id=&emulator=&slot=&session_id=&overwrite=true`, multipart `saveFile` = local bytes |
| `download` | `GET /api/saves/{save_id}/content` → **back up existing local file** → write to the file's SD path |
| `conflict` | server sets resolution in `reason`/policy: `server_wins` (download), `device_wins` (upload), `keep_both` (write server copy alongside, keep local) — **always back up the loser first** |

Pass `session_id` on uploads so the server ties them to this session. Track
`operations_completed` / `operations_failed`.

Backups go to `sdmc:/config/rommsync/.backup/<rom_id>-<ts>.<ext>`. Never destroy
a save without a backup — this is a hard rule.

## Step 3 — complete

`POST /api/sync/sessions/{session_id}/complete`:
```jsonc
{ "operations_completed": 7, "operations_failed": 0, "play_sessions": [] }
```
Then persist the new per-save `{hash, mtime, server_updated_at, server_hash}` to
`state.db`. That stored baseline is how the *next* tick knows which side changed
(local mtime/hash differs → local changed; server hash differs → server changed).

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
- Verify downloaded save size/hash where the server provides `content_hash`.
