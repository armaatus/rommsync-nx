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
5. Derive the `slot` from the emulator the folder belongs to and the save's own
   extension (`retroarch-srm`). It pairs with `rom_id` on the server, so it has
   to be **derived, not chosen**: a slot that changes between ticks makes the
   same file a new save every tick. The emulator is in it because RomM pairs on
   `(rom_id, slot)` alone — a rom whose RetroArch `.srm` and Tico `.srm` both
   mapped to `srm` would have the two files overwrite each other through the
   server, forever.
6. Validate the record before emitting it. A save the encoder would refuse — an
   mtime of 0 from a console with an unset clock, a name the server would read
   as a path — is skipped with a reason, because `EncodeNegotiateRequest` stops
   at the first bad entry and would otherwise cost the tick every other save.
7. Fill `content_hash` from `state::ContentHashFor` — the baseline's digest when
   the file's mtime **and** size still match, a fresh read otherwise. **Never
   leave it null to save a read**: a save reported without a digest is compared
   on timestamps and planned as an `upload` the server already has, on this tick
   and every tick after it. The one legitimate null is a file whose bytes could
   not be read at all, which is still reported — less precisely — and counted.

The scanner names files by their SD-root path (`/retroarch/saves/Game.srm`).
Opening one needs the platform prefix, so it is `fs::FileSystem::Resolve` that
turns it into something `io::ReadFile` and `state::HashFile` can open —
`sdmc:…` on Horizon, a path under the card's root on the host. Nothing above
that interface learns which.

Cache the rom index (`GET /api/roms`) per tick; it's also needed by downloads.
**It answers an envelope, `{items, total, limit, offset}`, not a bare array, so
it has to be paged** — a client that reads `items` and stops has whatever page
size RomM felt like giving it, and a rom on the last page is exactly the one a
save is going to need.

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

`sync::Negotiate` makes that call and `ParseNegotiateResponse` turns the answer
into a `SyncPlan` (M2-4). Both live in the same header as the request side. Three
things about the answer are worth stating here rather than leaving to the code:

- **The plan is read whole or not at all.** One unreadable operation refuses the
  response, because a plan with a save missing from it looks exactly like a plan
  for a device that is already in sync — and the save that got dropped is the one
  nobody hears about again. A truncated body is that failure in its quietest
  form, which is why `sync.truncated` forces one.
- **An `action` this client does not recognise becomes `no_op`, and is logged.**
  On a save, the default branch is the one that can overwrite it. The same goes
  for an unrecognised `reason`, except that the action there is still obeyed: the
  reason is the server's explanation, not its decision.
- **Four failures that are not "the network".** A `404` carrying RomM's
  `Device with ID … not found` is this device deleted in the web UI, a
  `400 Sync is disabled for this device` is the user's own switch, a `401` is the
  token revoked (`expires_at` is null, so there is nothing to refresh), and a
  `403` is a scope the user did not approve — which is a working pairing, not a
  dead one, and is why the two are separate errors. None of the four gets better
  by retrying. Everything else — no response, a `5xx`, a `429` — retries with
  backoff, and RomM cancels the session a retried negotiation superseded, so the
  abandoned ones do not pile up ([API_CONTRACT.md](API_CONTRACT.md#save-sync--negotiate--execute--complete)).

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
- **`overwrite=true` clears the 409 and nothing else. It does not stop a slot
  accreting a row per upload.** This page has now had this wrong twice, in both
  directions, so here is the mechanism rather than the conclusion: RomM stamps a
  slot upload with a **second-granularity** datetime tag and then looks the
  existing row up *by that tagged name* (`_apply_datetime_tag` and
  `get_save_by_filename`, `endpoints/saves.py` in 5.2.0). Two uploads therefore
  land on the same row only when they share a base file name **and the same
  wall-clock second**; one second later the name RomM computes matches nothing
  and it writes a **second** row, flag or no flag. Verified directly against the
  live 5.2.0, and `execute.occupied` pins the deterministic half.
  The revision that said `overwrite=true` "replaces the row in place, tag and id
  included" was reading a same-second run — which is what made that assertion
  pass on a fast machine and fail in CI (issue #85).
  `PUT /api/saves/{id}` is the call that really does move one row forward, and is
  what `harness.conflict` uses to arrange "the server's copy changed" without
  touching this device's history at all.
- **`save_id` is null for an `upload`** when the server has nothing yet, and set
  when it has an older copy. Don't dereference it unconditionally.

Pass `session_id` on uploads so the server ties them to this session, and
`device_id` so the sync history that the *next* negotiation arbitrates against is
written. Track `operations_completed` / `operations_failed`.

### The order every overwrite goes through

`sync::ExecutePlan`
([`core/include/rommsync/sync_execute.hpp`](../core/include/rommsync/sync_execute.hpp))
is the only thing that acts on a plan, and a `download` or a `conflict` is four
steps in this order and no other:

1. **fetch to `io::TempPathFor(<save>)`, never to the save.**
   `http::DownloadTarget` renames its `.part` onto the destination the moment
   the body ends — which is before anything has checked that the bytes are the
   save the plan meant. Pointing it at the save file would replace it with an
   unverified body, and the `.part` mechanism would not have caught it, because
   the body was complete. It was just the wrong save, or a body a proxy
   shortened without saying so.
2. **verify** the staged bytes' MD5 against `server_content_hash`. A mismatch
   discards them and leaves the local file untouched — a *successful* refusal.
3. **back up** the save's current bytes, streamed with `io::CopyAtomically`
   (`io::WriteAtomically` takes a `string_view`, and a save state does not fit
   in the sysmodule's heap).
4. **commit** with `io::CommitStaged`, which is `WriteAtomically`'s two-rename
   commit for a file that arrived on disk rather than as a string.

Step 4 cannot start until step 3 has succeeded, and every step before it can
fail without the save changing at all. A missing `.backup/` therefore *stops*
the overwrite: `core/` cannot create a directory with only standard headers —
that is the platform layer's job — and no backup means no overwrite.

**A negotiate operation carries no size**, so `expected_size` comes from a
preflight `GET /api/saves/{id}`. Without one, a body that ends cleanly and early
is indistinguishable from a complete one; that is exactly what the fault proxy's
`truncate` mode produces, and `execute.truncate` is the scenario.

**There is no retry inside a tick**, and there are now two reasons rather than
one. A re-posted upload really does duplicate the save: a retry a second later
gets a fresh datetime tag and therefore a new row (above), which is the reason an
earlier revision gave, then withdrew, and which turns out to have been right. And
the plan describes a state the server may have moved on from, so the arbiter of
that is a fresh negotiation rather than this client. A failed operation is
counted, left alone, and picked up by the next negotiation. `sync::RunTick`
owns the order of one tick and M7-2 owns the schedule between ticks.

**A save the client has no local file for** (`download` /
`Save exists on server but not on client`) needs a destination the plan cannot
supply: the server's `file_name` carries the ingest tag and the directory
depends on the rom's platform. `ExecuteOptions::place` is where that policy is
injected, and it needs the rom index plus the platform→folder map (M3-1); with
none supplied the operation fails by name rather than guessing at a path.

### Backups

Backups go to `sdmc:/config/rommsync/.backup/<rom_id>-<slot>-<ts>.<ext>`. Never
destroy a save without a backup — this is a hard rule.

**The slot is in the name because the first scheme was unsafe.**
`<rom_id>-<ts>.<ext>` carries neither the slot nor the save's own name, so two
saves of one rom backed up in the same second were the same file and the second
backup destroyed the first — one rom with two slots, or a save and its state,
was enough. `(rom_id, slot)` is the pair RomM keys a save on, so it is the pair
that separates two of them. A `null` slot spells itself `archival`, every byte
outside `[A-Za-z0-9._-]` becomes `_` so a slot cannot carry a separator out of
`.backup/`, and a name that is somehow still taken gets `-1`, `-2`, … rather
than being written over. `sync::BackupFileName` is the one spelling of that,
and `harness::Sandbox::BackupPathFor` calls it so the audit and the code under
test cannot disagree.

**A state has no slot**, so its middle segment is built from its own name
instead — `sync::StateBackupDiscriminator`, giving
`<rom_id>-state-<name>-<ts>.<ext>`. Passing a null slot for one would spell
`archival` for every state of a rom, which is the collision this scheme exists to
prevent, one asset kind over
([Save states](#save-states)).

**The backup promises ordering, and — once the platform layer has said how —
durability too.** The copy is flushed, closed, made durable and only then renamed
into place, before the save is touched: no reader ever sees half a backup, no
overwrite happens without one, and a console that loses power between the copy
and the overwrite does not leave a backup whose name landed and whose data blocks
never did.

That last part is `io::FileSync`, a hook the platform layer installs because
neither `fsync` nor Horizon's `fsFsCommit` is reachable from `core/`.
`host/src/file_sync.cpp` syncs one descriptor; `sysmodule/source/main.cpp` calls
`fsdevCommitDevice("sdmc:")`, which is the only primitive devkitA64 has — its
newlib exports no `fsync` at all, which is why the hook takes a path rather than
an open handle. A hook that refuses fails the copy, so a backup that could not be
put on the card stops the overwrite rather than being counted as one. With none
installed the older, weaker promise stands.

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

#### Finding the backup again, and putting it back

A backup is only a safety net if a human can find one without an SD reader, so
every overwrite is recorded in `conflicts.db` beside `state.db` as it happens —
a conflict, a download that replaced a file, and the same two on the states side
(`core/include/rommsync/conflict_log.hpp`, M7-1). The entry stores **the exact
path the backup was written to**, never a name derived a second time: a
`BackUpFirst` that had to step past an occupied name would make the two
spellings differ, and a restore from the wrong path is the failure backups exist
to prevent.

`conflicts::Restore` puts those bytes back, and it is **a save overwrite in its
own right**: it copies the file it is about to replace under `.backup/` first,
through the same `sync::BackUpFirst`, and then commits with
`io::CopyAtomically`. A restore that ate the server's copy would be this rule
broken one level up.

It writes the local file and nothing else. **The server stays the source of
truth**: the next negotiation arbitrates, and will most likely plan an `upload`.
The user is choosing which bytes to offer, not overruling RomM, and the overlay
says so before the press.

`[sync] conflict_show` hides the overlay's conflicts screen. It never stops the
recording — a console that had it off for a month lists every conflict, and
keeps every backup, when it goes back on.

## Step 3 — complete

`POST /api/sync/sessions/{session_id}/complete`:
```jsonc
{ "operations_completed": 7, "operations_failed": 0, "play_sessions": [] }
```
→ `{ "session": SyncSessionSchema, "play_session_ingest": null }`. `[]` is what a
tick that recorded no play time sends, and it is sent explicitly rather than
omitted; when there *is* play time it goes in that array and the ingest comes
back non-null — see [Play time](#play-time) below. The session's
`operations_planned` counts operations that need *work*, so a plan of nothing but
`no_op` reports `0` planned against however many you completed; that is not an
error.

Then persist the new per-save `{content_hash, mtime, file_size_bytes,
server_updated_at, server_content_hash}` to `state.db`. That stored baseline is
how the *next* tick skips work — the server arbitrates, but the client still has
to know which local files are worth re-hashing. The format, the reader and the
writer are `core/include/rommsync/state_db.hpp`: a version line and one JSON
object per `(rom_id, slot)`, written with `io::WriteAtomically` by
`state::SaveBaseline`.

**Persist first, report second.** Complete is accounting, not the commit point:
the uploads and downloads already landed on the server, and RomM has already
written the sync rows the next negotiation arbitrates against. A `complete` that
fails must therefore not take the baseline with it — do that and the next tick
re-uploads saves the server already has, and RomM stamps each one as a new row.
`sync::FinishTick` performs the two in that order so it cannot be forgotten, and
reports both halves; a failed completion is a failed *tick*, retried next
schedule.

Which operations move a row:

| Outcome | Row |
|---|---|
| `kUploaded` | advances. The local file was not touched, so the facts the tick reported are still the facts on the card. `server_content_hash` becomes the digest that was just sent — the plan's described the copy the upload replaced. |
| `kDownloaded`, `kKeptBoth` | advances, **from the card**. The bytes changed, so the mtime, the size and the digest are all re-read; a row built from the reported ones would claim a digest for bytes nothing hashed. |
| `kNoOp` | advances. Nothing happened and nothing stopped being true. |
| `kFailed`, `kNotUnderstood`, `kCanceled` | keeps its previous row, so the next tick retries the save. |

A save whose hash failed is not moved forward — `SaveBaseline` refuses a row
with an empty `content_hash` anyway. Whether the row it already had survives
depends on whether the file did: an `upload` and a `no_op` leave the card alone,
so the stored row still describes it and is kept; a `download` whose new bytes
could not be read back replaced them, so the stored row describes bytes that are
gone and is **erased**, and the save is hashed again next tick.

A save with no operation at all is not advanced, and does not need to be: a save
the client reports and is already in sync with is answered with an explicit
`no_op` / `Content is identical`, not with an absence
([API_CONTRACT.md](API_CONTRACT.md#save-sync--negotiate--execute--complete)).
What negotiate leaves out of a plan is a save the client never mentioned.

An advancing row starts from the row that is already there, so a field the plan
did not restate does not silently erase what the last sync knew. Which side may
erase differs by direction: an `upload` clears `server_updated_at` because the
stored one describes the copy it just replaced, a `download` clears what the plan
could not supply because the bytes on the card are newer than the stored value
describes, and a `no_op` moved nothing and keeps everything it does not restate.

`core/` has no single-file stat, so the re-read for a download goes through
`fs::FileSystem::List` of the save's directory. Only the most recent listing is
kept — a listing may hold up to `fs::kMaxDirectoryEntries` names on a heap of
512 KiB, so a memo per directory is unbounded in the one dimension that is
scarce, on the tick that already downloaded the most. Keeping the last one still
collapses the case that matters, which is several saves in one folder.

## Play time

Optional, and last in every sense (M7-4, #39): it writes no save, touches no
baseline, and **a play-session failure never fails a tick**. If it is in the way,
it is the thing to cut.

**The console cannot see which rom is running.** Horizon will name the
foreground *application*, but on a modded Switch that application is RetroArch or
Tico — never the rom — and no API says which file an emulator loaded. So the
sessions are derived from the saves instead: a save whose mtime moved between two
ticks means that rom was played, and the two ticks bound the window.

That makes `duration_ms` an **upper bound rather than a measurement**, and the
client says so rather than pretending otherwise:

- the window's *end* is the save's own mtime — a real observation, the moment the
  emulator wrote — not the tick that noticed it;
- its *start* is the later of the previous tick and the save's previous mtime,
  the tightest lower bound the client actually has;
- a save whose mtime did not move produces nothing at all;
- the first tick after a boot with no buffer produces nothing, and must: with no
  previous observation every save on the card would come back as one enormous
  session.

Wall clock only. A console whose clock was never set, or that has been corrected
backwards, records nothing for that tick rather than sending a timestamp RomM
would file under 1970 forever — the bound is `play::kEarliestPlausibleSeconds`,
deliberately tighter than the one a save's `updated_at` is held to.

Unsent sessions wait in `sdmc:/config/rommsync/play.db`, which is `conflicts.db`'s
format: a header line carrying the next id and the moment the last tick looked,
then one JSON object per session, oldest first, written with
`io::WriteAtomically`. It is bounded and the **oldest fall off the front**, so an
offline console fills it and stops rather than growing a file forever; a reboot
does not lose it.

They ride out on the completion the tick is making anyway, so a console on
battery spends no extra request. `POST /api/play-sessions` is the other route,
for flushing without a sync (`play::Flush`).

A session is dropped from the buffer only against an answer that names it, and
`duplicate` counts as an answer — which is what makes a retried flush safe. An
entry RomM answers `error` is dropped too, with a line saying so: the body was
already validated on the way out, so a refusal is deterministic, and a session
kept at the head of the buffer would be re-sent and refused forever while holding
a slot RomM would have taken.

## Change detection between ticks

The negotiate call already computes the plan server-side, so the client can send
all saves every tick and let the server decide. `state.db` is an optimization:
skip re-hashing unchanged files (mtime+size match the stored baseline) to keep
ticks cheap on large libraries.

Both, not either — `state::ContentHashFor` reuses a stored digest only when the
mtime *and* the size still match. A same-size overwrite inside one second is
caught by neither alone, and an emulator that restores mtimes is caught only by
the size.

**Skipping the hash is not skipping the report.** A save whose file was not
re-read still carries its stored `content_hash` into the payload. A save reported
without one is a save the server compares on timestamps alone, which plans an
upload for bytes it already has — every tick, forever.

A `state.db` that is missing, truncated or corrupt is an empty baseline plus a
diagnostic, and the tick hashes everything. It is never a reason to refuse to
sync: a lost baseline costs time, not correctness.

## Save states

**Not the same flow.** This section used to say "same flow via `/api/states`",
and that was wrong in the way that matters: there is no negotiation for a state,
so the server decides nothing and the client has to. Verified against a live
5.2.0 and pinned by `server/contract/captures/states-post.json` and
`states-list.json` — see [API_CONTRACT.md](API_CONTRACT.md#save-states) for the
endpoints.

What 5.2.0 actually offers:

- `SyncNegotiatePayload` carries **only** `saves`. A state never appears in a
  negotiate request or in a plan.
- `StateSchema` has no `slot`, no `content_hash`, no `origin_device_id` and no
  `device_syncs`. There is no per-device sync history for a state and no digest
  to verify a download against.
- There is no `POST /api/states/{id}/downloaded`.
- `POST /api/states` takes `rom_id` and `emulator` and a multipart `stateFile`
  (plus an optional `screenshotFile`). No `slot`, no `device_id`, no
  `session_id`, no `overwrite`.

...and two behaviours that shape everything the client does:

- **`POST /api/states` is an upsert keyed on `(rom_id, file_name)` alone.** A
  second POST under a name the rom already has replaces that row's bytes in
  place, keeps its `id`, and *moves* its `emulator` to whatever the new request
  said. It does not create a second row the way `POST /api/saves` does. So a POST
  **is an overwrite of somebody's state**, and the emulator is not part of the
  key.
- **RomM does not rename a state on ingest.** A save sent as `probe.srm` is
  stored as `probe [2026-09-04_11-12-27].srm`; a state sent as `probe.state` is
  stored as `probe.state`. The server's name and the client's are the same
  string, which is what makes `(rom_id, file_name)` a usable pairing key on both
  sides.

### The policy

Hard rule 3 — the server is the source of truth — has nothing to be the source
of truth *with* here, so the client arbitrates, conservatively:

**Upload freely, overwrite only on an unambiguous baseline match, keep both on
anything else, and never delete a state.** Losing a duplicate costs disk; losing
a state costs the session.

For one local state, keyed `(rom_id, file_name)`:

| `state.db` row | server row under that name | local file | → |
|---|---|---|---|
| none    | none            | —         | upload |
| none    | present         | —         | **keep both** |
| id X    | none            | —         | upload (this is how a state deleted elsewhere comes back) |
| id X    | id ≠ X          | —         | **keep both** |
| id X    | id X, unmoved   | unchanged | no-op |
| id X    | id X, unmoved   | changed   | upload |
| id X    | id X, moved     | unchanged | download |
| id X    | id X, moved     | changed   | **keep both** |

A server state no local file claimed is *placed* — and a placement may only
**create**. Where it goes is the caller's (`sync::StateSyncOptions::place`),
because the folder depends on the rom's platform and on which emulator the state
belongs to; nothing said is a named failure rather than a guessed path. If there
is already a file at the path it names, that is keep-both too: "no local state
claimed this row" is not "there is nothing at that path" — a state the scan
skipped, as an ambiguity or as the loser of a duplicate name, claims nothing and
still sits on the card, and writing over it on the strength of a row this console
has no history for is exactly what the policy refuses.

"Keep both" means exactly that: nothing is transferred, the local file is not
touched, the server row is not written, and the run says so in a warning. It is
counted apart from both `completed` and `failed`, because it is the policy
working rather than a failure.

Two local states of one rom that share a file name are **one** state to RomM, so
the scan reports the first and skips the second with `duplicate name` — the same
reasoning that put the emulator in a save's slot, applied to a key the server
enforces without one.

### Backups and verification

A state that is replaced goes through the same four steps as a save, using the
same two `io` primitives: fetch to `io::TempPathFor(<state>)`, check it, back it
up with `io::CopyAtomically` into `.backup/`, then `io::CommitStaged` it into
place. The `harness::Sandbox` teardown audit covers a state exactly as it covers
a `.srm`.

The backup name carries a discriminator built from the state's own name —
`<rom_id>-state-<name>-<unix seconds>[-<n>].<ext>` — because a state has no slot.
Passing a null slot instead would spell `archival` for every state of one rom,
leaving only the uniquifier to separate them (see
[Backups](#backups)).

**A downloaded state can be checked against `file_size_bytes` and nothing else.**
RomM computes no digest for a state, so there is no MD5 to compare — a length
match is *not* an integrity check, and every state download says so in a warning
rather than letting a reader assume the save path's verification is in play.

### Off by default means silent

`sync.states` is `false` unless the user sets it. With it off, the state
directories are never walked and `/api/states` is never called —
`sync::SyncStates` returns before either. States are core- and
version-specific: a state synced onto a console running a different build is how
a player loses a session they thought was safe, so it is opt-in.
Config: `sync.states = true`.

`state.db` holds a state row beside a save row, discriminated by `"kind"`, in
format version **2**. A version 1 file is discarded and re-hashed once — the
designed cost of the version line, not a migration
([state_db.hpp](../core/include/rommsync/state_db.hpp)).

## One tick, and what interrupting it may leave

`sync::RunTick` ([`core/include/rommsync/sync_tick.hpp`](../core/include/rommsync/sync_tick.hpp))
is the whole loop in one call: recover, negotiate, execute, finish. It owns
neither end — the scan is step 0's and the schedule between ticks is M7-2's — and
it re-implements no retry, because `sync::CallPolicy` already times out, retries
and backs off on both API calls.

**The rule it exists to keep**: the state left behind after any interruption is
either the state before the tick, or a strictly completed subset of it. There is
no third legal outcome.

### On entry: what a crash left behind

A *handled* failure clears its own staging. This is the sweep for the case where
no cleanup got to run — `sync::RecoverStaging`, over the save folders and
`.backup/`:

| left behind | by | what happens to it |
|---|---|---|
| `<save>.tmp.part` | a download the process died during | removed. `DownloadTarget::resume` is false for a save, so nothing in it is worth keeping and the next tick refetches |
| `<save>.tmp` | a download whose body ended and whose commit did not | **removed, not committed.** See below |
| `<save>.old` | an interrupted `io::CommitStaged` | renamed back when `<save>` is missing — it is then the only copy of the save. When `<save>` *is* there the commit finished, and the file is counted and **left alone**: a `Game.srm.old` a human made by hand has exactly that shape, and this sweep does not delete files it did not write |
| `.backup/<rom_id>-<slot>-<ts>.<ext>` | any overwrite | never touched. A backup is never garbage: it is the copy M7-1 restores from, including one written for an overwrite that then failed |

**A `<save>.tmp` is discarded rather than finished, and an earlier revision of
this page and of issue #16 said the opposite.** The reasoning was that a `.tmp`
holds complete, *verified* bytes that never landed. It holds complete bytes; it
does not hold verified ones. `ExecutePlan` stages a download at
`io::TempPathFor(<save>)`, and `http::DownloadTarget` renames its `.part` onto
that path the moment the body ends — which is *before* the MD5 check and before
the previous bytes are backed up. So a `.tmp` a crash left may be the wrong save,
with no backup beside it, and nothing at recovery time can tell: the digest that
would settle it lived in a plan that is gone. Committing it would overwrite a
save with unverified bytes and no backup, which is hard rule 2 broken in the one
place it matters. Discarding costs one refetch.

**A bare `<name>.part` is left alone.** Only `<name>.tmp.part` is swept. A `.part`
beside a rom is a M3-3 range-resume in progress, and a sweep that took it would
throw away a gigabyte of a transfer that was going to finish.

**Not `/config/rommsync` itself.** The records there — `token.dat`, `device.dat`,
`config.ini`, `state.db` — already recover from their own `.old` when they are
read, so a sweep adds nothing; and the overlay writes `config.ini` from another
thread (M5-3), where removing a `.tmp` between that write and its rename would
cost a setting the user had just changed. `RecoverStaging` takes the list of
directories rather than deciding for itself precisely so a caller can keep to the
ones nothing else writes.

The sweep runs *before* the network, and it is right to run it on a tick that
turns out to be offline: it only removes litter and puts back a save an
interrupted commit parked. It writes no backup, no save and no `state.db` — which
is the reading "with RomM unreachable, a tick writes nothing at all" gets here,
and `tick.offline` checks both halves of it: a card with nothing to recover is
byte-for-byte identical afterwards, and a card with leftovers has them dealt with
and still gains no backup and no rewritten baseline.

### A save the sweep put back is a save the scan never saw

Restoring a `<save>.old` is the one thing the sweep does that the rest of the
tick cannot simply carry on past. Step 0 ran before `RunTick` — the scan's output
*is* its argument — so the restored save is missing from `reported`, and RomM
answers a save it is not told about by planning a download of its own copy over
it. The local bytes would be backed up and replaced without the conflict
arbitration they were owed, which is the client deciding a conflict by omission.

So the tick stops, before the network, with `TickOutcome::kRescanNeeded`, and the
caller scans again and runs another. That costs a tick, which is harmless, and it
only happens after a crash mid-commit. A caller that sweeps *before* it scans
never sees it.

### After the negotiation: the directory `core/` cannot create

`sdmc:/config/rommsync/.backup/` missing is an `OperationError::kBackupFailed`
*before* the save is touched — no backup, no overwrite — so on a first boot every
download failed until something made the folder.
`fs::FileSystem::CreateDirectory` is that platform facility, behind the interface
that already owns the card. `RunTick` calls it once the negotiation has answered
and not before: nothing needs the directory until an operation does, and a tick
that never reached the server must not have written anything at all. A directory
that is already there is success.

### Cancellation

`TickOptions::cancel` is **one** `http::CancelToken`, copied onto the negotiate
policy, the execute options and the complete policy, and checked between the
stages. A shutdown or an overlay "stop" therefore ends the tick at an operation
boundary rather than mid-write, and without spending three timeouts plus two
backoffs on the accounting call afterwards — on the link whose loss is usually
why the shutdown happened. A token already fired costs no request at all. Who
fires it is the scheduler's (M7-2) and the overlay's (M4-2).

### A session that is not this tick's to close

`CompleteError::kAlreadyCompleted`, `kSuperseded` and `kNoSuchSession` all set
`TickResult::session_gone`, and none of them is a reason to re-run the plan. The
first means the accounting succeeded — a retry landing after the first attempt
got through looks exactly like it — and the other two mean the counts will never
be recorded. In all three the transfers already happened and the baseline is on
the card, so the answer is the next tick's own negotiation.

### What the tick says it did

`TickOutcome` is split on what the scheduler would *do*: `kCompleted`,
`kPartial` (something did not happen — an operation, or the baseline reaching the
card; the next negotiation plans it again), `kUnreported` (the transfers landed,
the accounting did not, and the baseline *is* on the card — the two are asked
about in that order, because a bad minute loses both and only one of them is
expensive), `kOffline` (nothing was written — back off), `kRefused` (an answer
that does not change on a second attempt), `kUnauthorized`, `kRescanNeeded` and
`kCanceled`.
`TickResult::answer` is separate and is what `auth::Gate::Observe` takes: it is
the **last** word on the credentials rather than the worst one, because one 401
is not a verdict and a completion the same token was accepted for is proof it
still works (M1-4). It falls back through the completion, the last operation and
finally the negotiation, which on any tick that got a plan is itself proof the
server read the token — leave that last step out and a tick that negotiated fine
and then lost the link twice reports silence, and `auth::Gate`'s consecutive
count survives a 200 that should have cleared it.

The two kinds share one budget, because `state::kMaxRecords` bounds the *file*.
`scan::kMaxStates` is a quarter of it, and a run trims its own state rows before
a save ever loses one — a save is what hard rule 2 protects. **A card whose saves
alone reach the bound therefore records no states**: there is no room beside
them, so every state is kept on both sides rather than synced. That settles
instead of churning (the next tick finds no row and sends nothing) and the run
says so, naming the remedy — turn `sync.states` off, or raise the client's
bounds.

## Failure & safety rules

- Offline / unreachable server → abort the tick cleanly, change nothing, retry
  next schedule. Missed ticks are harmless.
- Partial plan failure → complete with the accurate `operations_failed`; leave
  unsynced files for next tick. Never leave a half-written save (write to temp,
  fsync, rename). An `action` this build does not recognise is counted with
  `operations_failed` and not with `operations_completed`: the server planned
  work that did not happen, and calling it completed tells RomM the client did
  something it cannot name.
- One backup per overwrite, always, before writing.
- Verify a downloaded save against the MD5 in `content_hash` /
  `server_content_hash`, and its length against `file_size_bytes`.
