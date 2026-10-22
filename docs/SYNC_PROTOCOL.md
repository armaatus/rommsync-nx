# Save sync protocol (client side)

How `sys-rommsync` implements RomM 5.2.0's negotiate → execute → complete loop.
Endpoints and schemas: [API_CONTRACT.md](API_CONTRACT.md). The payload and
response shapes below are the ones captured from a live 5.2.0 under
`server/contract/captures/` (issue M0-4), not a reading of the OpenAPI snapshot.

## Principle: the server is the source of truth

The client never decides a conflict on its own. It reports what it has (hashes +
timestamps), the server returns a per-save action, and the client obeys. This is
what keeps *every* device and the server converged.

## Before step 0 — which RomM this is

A tick asks the server which release it runs before it asks it anything else:
the unauthenticated `GET /api/heartbeat`, whose `SYSTEM.VERSION` every RomM
sends (M10-2, #228). RomM has no capability endpoint, so every behaviour that
differs between releases is inferred from that one string, in one place —
`server::CapabilitiesFor` in
[`server_capabilities.hpp`](../core/include/rommsync/server_capabilities.hpp) —
and handed to the tick as `TickOptions::server`.

**Below RomM 5.4.0 the tick stops there** (`TickOutcome::kUnsupported`): no
library fetch, no negotiation, nothing offered or written. A 5.2 or 5.3 cannot
answer `delete`, filter by emulator or keep per-device baselines, and without
those another device's save is superseded silently — which is worse than not
syncing (D1 in #226). A pre-release reads as its release (`5.4.0-beta.1` is
5.4.0), and a version that is missing or does not parse reads as the newest, the
rule `server/testing/provision.py` follows too. The answer is read once a boot
and again after a pairing; a refused server is asked again every tick, so an
upgrade is noticed without a reboot.

The heartbeat is the server's whole configuration and grows with its library,
so it is walked rather than parsed: only `SYSTEM` reaches the JSON parser,
bounded at `server::kMaxSystemBytes` (`alloc.heartbeat` measures it).

## Step 0 — matching local files to roms

For each file under the configured save/state dirs:

0. **Which dirs.** RetroArch's come first, and from `sdmc:/retroarch/retroarch.cfg`
   rather than from the map (#230, [CONFIG.md](CONFIG.md#retroarch-read-from-retroarchcfg)):
   its `savefile_directory`, and with sort-by-core on — the Switch build's
   default — each `<CoreFolder>/` one level under it instead, where a loose file
   is skipped as *outside layout* because RetroArch does not read it. First, so
   that the copy RetroArch reads wins its slot over an older one the map's flat
   `/retroarch/saves` still holds. While RetroArch's own Cloud Sync is on, no
   RetroArch folder is read at all (`[sources] retroarch = auto`): two writers
   to one save is the failure to avoid. Then the map's folders, as written —
   except that a Tico folder `<root>/<folder>` is read where Tico 0.8 keeps it,
   `<root>/users/<account>/<folder>`, once `<root>/users/` exists (#231,
   [CONFIG.md](CONFIG.md#tico-per-account-saves)). Several accounts and no
   `[tico] account` naming one is one skip for the whole `users/`, never a
   guess; a file left in the old folder is *outside layout*. In every folder,
   `*.tmp`, `*.png` and `*.restart` are not saves and are passed over, and no
   subfolder — Tico's `backups/` — is read. A subfolder of Tico's psp folder is
   where PPSSPP keeps a game's save, so it is a *save folder* skip by name rather
   than silence: PSP saves are not synced until the `PSP-<folder>.zip` adapter
   (#238, after #44).
1. Strip the save/state suffix to get the base name (`Game (USA).srm` → `Game (USA)`).
2. Match to a rom by `fs_name_no_ext` (exact), falling back to `fs_name_no_tags`.
   In Tico's psx folder a `.mcd` that is not exactly one rom's name is read as
   DuckStation names its cards (#238): `<name>_2.mcd` is card 2 of `<name>`,
   and one card serves every disc of a game, so `<name>.mcd` may be the card of
   the rom `<name> (Disc 1)`. Card 2 is a second save of the rom, in its own
   slot `autosave-mcd-card2` and uploaded as `<name>_2.mcd`
   (`scan::MemoryCardName` is the same rule the other way, which `place::Placer`
   places a card by).
3. Scope by platform when the path implies one (Tico's per-system folders do;
   RetroArch's flat `saves/` doesn't — match by name across the library then).
4. Ambiguous match (same base name on two platforms, no platform hint) → skip and
   log; never guess.
5. Derive the `slot`: the shared one, `[sync] slot` (`autosave` by default,
   [CONFIG.md](CONFIG.md#slot-the-shared-slot)), for a `.srm`, and
   `<slot>-<extension>` for anything else (`autosave-rtc`, `autosave-mcd`) —
   `scan::SlotFor` (#236). `autosave` is the slot RetroArch's Cloud Sync, the
   web player, Argosy and Grout file a game's save RAM under, so a save here is
   the save they see. It pairs with `rom_id` on the server, so it is **derived,
   not chosen** per file: a slot that changes between ticks makes the same file a
   new save every tick. One slot holds one save per rom — RomM keeps the newest
   row of a slot as the head every client pairs against — which is why a
   companion such as Snes9x's `.rtc` gets a slot of its own (RomM's RetroArch path
   keeps it out of the slot too), and why a rom's RetroArch `.srm` and Tico `.srm`
   on one card are one slot: the one found first (RetroArch's) is synced and the
   other is skipped as a *duplicate slot*. The name the server is told is the
   rom's own stem, `<fs_name_no_ext><ext>`, which is the name RomM serves
   RetroArch the save under.
6. Name the `emulator` that wrote it — the id RomM pairs lineages on and other
   clients look saves up by, not the source's name (`scan::ResolveSource`,
   [`save_source.hpp`](../core/include/rommsync/save_source.hpp)). A RetroArch
   sort-by-core folder RomM's table knows names its core (`saves/mGBA/` →
   `mgba`); otherwise `[platform.<slug>] emulator` in `config.ini` decides;
   otherwise the built-in table: the libretro core id where the bytes are
   RetroArch-compatible (`snes9x`, `gambatte`, `genesis_plus_gx`), a `tico-*`
   id where they are not (`tico-duckstation`, `tico-ppsspp`), and `null` for a
   system neither table knows. The same id goes on a state's upload. A `tico-*`
   id is an **isolated format**: it syncs Switch to Switch and never with a
   RetroArch core — see [Formats only one emulator reads](#formats-only-one-emulator-reads).
7. Validate the record before emitting it. A save the encoder would refuse — an
   mtime of 0 from a console with an unset clock, a name the server would read
   as a path — is skipped with a reason, because `EncodeNegotiateRequest` stops
   at the first bad entry and would otherwise cost the tick every other save.
8. Fill `content_hash` from `state::ContentHashFor` — the baseline's digest when
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
      "emulator": "snes9x",                    // the core that wrote it; step 0
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

**`emulators` filters the lineages (5.4, #236).** The body also carries
`"emulators": [...]`, the ids this console can load — every scanned save's, and
the one each mapped platform's folders resolve to, each with RetroArch's folder
name for it (`scan::EmulatorFilter`). RomM then pairs and offers only server
saves one of them wrote, so a PlayStation core's save in a rom's `autosave` is
never downloaded over a Tico DuckStation card. It is left out — not sent as `[]`,
which matches nothing — when a scanned save has no emulator: RomM cannot list
`null`, and that save's own uploads would drop out of the pairing and be
uploaded again every tick. The slot guard is not filtered: an upload into a slot
whose newest save is a lineage the filter hides still draws the 409 below — so
the 409's second negotiation goes out **without** the filter, to see the save
the guard saw. Filtered, it would answer with this console's own previous
version, and every later upload would be refused and "resolved" the same way.

### `rom_ids`: only the roms on the card (#241)

Every negotiation also carries `"rom_ids": [...]`. These are the roms the card
holds: a file named after the rom, or a folder for a multi-file rom, in one of
its platform's mapped `roms` folders (`sync::FindRomsOnCard`). The roms of the
saves the request reports are added. RomM then offers downloads for those roms
only. Without the scope, a device with no history is offered every save in the
library, and #237 would place them beside games the console does not have. The
scope is read-only: a rom left out is outside the negotiation, never deleted.
So a scope that is too narrow costs a first-time restore, never a save. A rom
kept in a folder the map does not name gets no first-time restore until that
folder is mapped (CONFIG.md).

RomM takes at most 500 ids a request, so a larger card is negotiated in chunks
(`sync::ChunkScope`). The rules:

- Each rom is in exactly one chunk.
- Each save is sent in the chunk that names its rom. RomM widens a chunk's
  scope by the roms of the saves it carries, so a save sent elsewhere would
  leave its own rom's chunk offering the server's copy over it.
- The chunks are one tick. Their operations run as one plan, the baseline is
  written once, and each session is completed with its own share
  (`TickResult::other_sessions`).
- A chunk that fails to negotiate fails the tick before anything is written.
  The sessions the earlier chunks opened are left to RomM's cleanup.

`rom_ids` costs at most `kMaxRomScopeBytes` (about 10 KiB) of request body. The
response only shrinks.

**A rom just downloaded is asked about on its own, with `restore_unlisted`.**
When a download finishes, the engine asks for a tick at once
(`Scheduler::RunSoon`). That tick sends the downloaded roms in a separate
negotiation with `"restore_unlisted": true`, and the save is placed through
#237 before the game is launched. The flag matters for a device that synced
the save before the rom was deleted: RomM otherwise reads the missing save as
one the user deleted and offers nothing. The flag applies to the whole request,
so it goes only on a request scoped to the downloaded roms; nothing else this
console deleted comes back. Those roms are kept until a negotiation has asked
for them, so an offline tick keeps them for the next tick. They are not kept
across a reboot. After a reboot the rom is an ordinary rom on the card, and its
save still comes unless this device had synced it before.

### Upgrading from rc1: the old slots (#236)

v1.0.0-rc1 synced each save in a slot of its own, `<source>-<extension>`
(`tico-srm`, `retroarch-srm`). The first tick after the upgrade does **not**
re-upload those: every save is negotiated in the shared slot like any other, and
the server arbitrates the first meeting.

- `state.db` is version 2 on that card. It is read, not discarded (state_db.hpp):
  each `tico-srm` row moves to the slot its file is synced in now, keeping the
  file's digest, mtime and size — so nothing is hashed again — and **dropping its
  server half**. This device has never synced the shared slot, and the old
  slot's digest would have the 409 rule below take any newer head as a plain
  download of "unchanged" bytes.
- An empty shared slot gets the save, once.
- A shared slot holding the same bytes is a `no_op`.
- One holding a newer save (RetroArch's, say) is a `download`, the card's bytes
  backed up first.
- One holding an *older* one is a `conflict`: the guard refuses the upload, and a
  device that has never synced the slot cannot show its bytes unchanged. The
  head lands as for any conflict and the conflicts screen offers the card's copy
  back; with #240 the losing side is also kept on the server.

**rc1's rows are left on the server**, in their old slots, untouched. RomM's web
UI shows them as named slots (`tico-srm`) beside `autosave`; they are the copy
of each save as rc1 last synced it, and deleting them is the user's call. With
the `emulators` filter on they are never offered back to this console (rc1 sent
`emulator=tico`).

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
total_upload, total_download, total_conflict, total_no_op, total_delete }` —
`total_delete` since 5.4.0, read as zero when absent.

`sync::Negotiate` makes that call and `ParseNegotiateResponse` turns the answer
into a `SyncPlan` (M2-4). Both live in the same header as the request side. Three
things about the answer are worth stating here rather than leaving to the code:

- **The plan is read whole or not at all.** One unreadable operation refuses the
  response, because a plan with a save missing from it looks exactly like a plan
  for a device that is already in sync — and the save that got dropped is the one
  nobody hears about again. A truncated body is that failure in its quietest
  form, which is why `sync.truncated` forces one. A plan of more than
  `sync::kMaxPlanOperations` operations — `state::kMaxRecords`, 128 — is refused
  the same way and for a sibling reason: it is read an operation at a time but
  held for the whole tick, and past that bound it is more than the sysmodule's
  heap was sized for (M9-20, #246).
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
  backoff. A session a retried negotiation abandoned was cancelled by 5.2.0 at
  the next negotiate; 5.4.0 leaves it open until its 24-hour cleanup fails it
  ([API_CONTRACT.md](API_CONTRACT.md#save-sync--negotiate--execute--complete)).

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
| `upload` | `POST /api/saves?rom_id=&emulator=&slot=&session_id=&device_id=&content_hash=&overwrite=false`, multipart `saveFile` = local bytes; a **409** is resolved below |
| `download` | `GET /api/saves/{save_id}/content` → **back up existing local file** → write to the file's SD path → `POST /api/saves/{save_id}/downloaded {device_id, content_hash}` |
| `conflict` | resolve by policy (below) — **always back up the loser first** |
| `delete` | 5.4.0: **back up the local file**, then remove it — or leave it, under `[sync] on_server_delete = keep` ([below](#a-save-deleted-on-the-server)) |

**Both carry `content_hash`** (#234): the MD5 of the file hashed as it is
uploaded, and of the verified bytes a download put on the card. RomM 5.4 keeps
it as this device's baseline on that save row, and a later report that matches
it proves this side did not change -- so an mtime-only touch (Tico's atomic
rewrites) against a row that moved in place negotiates as `download`, not
`conflict` (`tick.baseline_moved`, `tick.baseline_downloaded`). A baseline can
only ever remove a conflict, never create one, and the one way it could remove a
*real* one is a digest of bytes the server does not hold; hashing just before
the upload streams the file, and confirming only the verified digest, is what
keeps that from happening (`harness::BaselineAudit`). It is the bytes' own MD5 even for a ZIP, whose
server digest hashes the entries instead: RomM holds a device's baseline only
against that device's later reports, so the different function costs the
suppression and nothing else. A baseline lives on one row, and another device's upload is a new head with no
history for this device -- that case is #232's 409 (`tick.baseline_other_row`).

Three traps in the operation itself:

- **`file_name` is the server's name, not yours.** RomM renames a save on ingest:
  `probe.srm` is stored as `probe [2026-09-04_11-12-27].srm`, and every later
  operation echoes that. Match the operation back to your local file on
  `(rom_id, slot)` and keep your own path; writing the server's name to the SD
  produces a file no emulator will load.
- **`overwrite` decides the 409 and nothing else. It does not stop a slot
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

### The slot guard, and the 409 (#232)

**An upload goes out with `overwrite=false`.** Given a `device_id` and a `slot`,
RomM then refuses the post with `409 {"detail": "Slot has a newer save since
your last sync"}` whenever the slot's newest save — written by any device or
emulator — is newer than this device's last sync of it. That is the protection a
shared slot needs: without it, a RetroArch or Argosy upload landing between this
tick's negotiate and its upload would be silently superseded. So the guard is
kept, and only a player's explicit **Keep local** for one save sends
`overwrite=true` (`ExecuteOptions::keep_local`; #240 split the overlay choice
that fills it into a later PR, so nothing fills it yet).

On 5.4.0 the 409 is not only a race. Every other device's upload is a new row
and becomes the head, this device has no sync row against it, and negotiate
falls back to timestamps and plans `upload` / `Client save is newer (no sync
history)` — even for bytes this device sent itself (#227). So a 409 is what a
device meets after any other device writes the slot, and the tick resolves it
rather than counting it failed forever (`sync::ResolveRefusedUploads`,
[`sync_resolve.hpp`](../core/include/rommsync/sync_resolve.hpp)):

1. **Negotiate once more**, for the refused saves only, scoped with `rom_ids`.
   The first plan may name a head that has since been replaced, and downloading
   that row would fetch — and confirm — a superseded save. Never more than one
   second negotiation per tick, and none while a game is running: the engine's
   own check (#242) is before the plan, and this is a round trip after it, so
   it asks again (`TickOptions::hold_resolution`) and a held resolution leaves
   the 409 for the next tick.
2. **Decide** with Grout's rule (#226 R3): local bytes whose MD5 equals the
   server digest `state.db` recorded at the last sync take the head as a
   **download**; anything else — changed bytes, no row, no digest — is a
   **conflict**. A conflict the server names stays one; a `no_op` stays one.
   **Never another upload**, and never a file step 0 did not find: a save the
   scan left out (a Tico account it could not decide, #231) is not reported,
   so it is never uploaded, and a resolution without a matched target is not
   run rather than placed.
3. **Execute** that through `ExecutePlan`, so it is fetched, verified, backed up
   and committed exactly as a planned download is, and merge it into the tick's
   plan and report at the refused operation's index — the baseline, the conflict
   history and `complete`'s counts then describe what happened to the save. The
   second session is completed with its own counts.

The server stays the source of truth: the 409 *is* its verdict, and both answers
put its head on the card. What the client decides is only whether the local
bytes it replaces were progress never synced — and those are recorded as a
conflict, with the guard's own sentence as the reason. `tick.newer_download` and
`tick.newer_conflict` have a second device upload between negotiate and upload
against the real RomM, under the crash replay.

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
supply: the server's `file_name` carries the ingest tag and the folder depends
on which emulator plays the system. `ExecuteOptions::place` is where that policy
is injected, and the engine's is `place::Placer` (#237):

- **The emulator** is `[platform.<slug>] play_with`, Tico by default (#226 D5) —
  never the map's first folder, which is `/retroarch/saves` for every system.
- **The folder** is that emulator's as the card is laid out this tick: Tico's
  through `tico::Resolve` (and nothing while several accounts are undecided),
  RetroArch's through `retroarch.cfg` (and nothing while it syncs itself).
- **The name** is the rom's `fs_name_no_ext` with the emulator's extension.
- **It must come back as itself.** The placed file has to be one the next scan
  matches to the same rom in the same slot, holding bytes the target emulator
  reads; otherwise it would be a second save of the game, uploaded on the next
  tick while the row it came from is offered again. Placing nothing is a named
  failure with the reason.
- **A placement only creates.** A file already at the path is one the scan did
  not pair with this row — skipped, or another slot's — and the operation fails
  with `kNoLocalSave` without touching it. A missing folder is made. The bytes
  arrive through the same stage-verify-commit as any download, and
  `execute.placed` replays a power cut at every call of it: no file, or all of
  it.

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

**The guarantee is an ordering of persistence, not of calls** (M9-14, #203).
After a power cut at any instant, and the boot sweep that follows it
(`sync::RecoverStaging`), the save holds its old bytes or its new ones — and if
it holds the new ones, a backup holding the old ones in full is on the card.
Neither FAT32 nor exFAT journals, so "the backup call returned before the
overwrite call started" does not imply that; what does is each step being
durable before the next one can be:

1. The save is copied to `.backup/<name>.tmp`, closed, and **synced**.
2. It is renamed to `.backup/<name>`, and **`.backup/` is synced**, so the name
   is on the card and not only the bytes. A card that will not confirm this is a
   failed backup (`kBackupFailed`): the copy is removed and the overwrite does
   not happen. The overwrite is in a different directory, and without this a
   power cut could keep it and lose the backup's name — the one gap the call
   order hid, found by the replay below.
3. The new bytes are staged beside the save and **synced** — by
   `io::WriteAtomically`/`CopyAtomically` for a restore, by the HTTP backend
   before it renames its `.part` for a download.
4. `io::CommitStaged`: the save to `<save>.old`, the staged file onto the save,
   `<save>.old` removed, and the save's directory synced. A cut between the two
   renames leaves `.old` and no save, which the boot sweep renames back.

Every sync is `io::Sync`, through the same `io::Disk` seam every write, rename
and remove goes through (#202), which asks the platform's `io::FileSync` hook:
`host/src/file_sync.cpp` `fsync`s by path, and `sysmodule/source/main.cpp`
calls `fsdevCommitDevice("sdmc:")`, which commits the whole card and ignores the
path — which is why the hook takes a path rather than an open handle. A hook
that refuses fails the step it guards. With none installed the older, weaker
promise stands: no reader ever sees half a file, and nothing about power cuts.

**What proves it.** `tests/crash_disk.hpp` records every call an overwrite makes
through `io::Disk`, cuts the record at every call, and enumerates what a card
that does not journal may keep: per directory, any prefix of the names changed
since its last sync; per file, the data written since its last sync lost,
truncated, torn, short, out of order, or complete. Each card is swept as the next boot would sweep it, and
judged against the property above. `execute.crash` (a download),
`states.crash` (a state) and `conflicts.crash` (a restore) are the three paths
that land on a save; the first and last also run with `.backup/` unconfirmed
(the state path shares `BackUpFirst`, so it shares that answer);
`execute.crash_model` shows the replay catches an overwrite written in place,
one with no backup, and a rename of bytes nothing synced. The `backup` row of
`scripts/v1-gate.sh` counts them. What the model assumes — a sync that says yes
did persist, a directory's own updates land in order, no flying writes — is
listed at the top of that header; a kernel-level injector such as lazyfs is the
check on those assumptions, and is not in the suite. Every replay drives the
host backend; the Horizon one (`http_wire.cpp`) syncs its `.part` through the
same `io::Sync`, and nothing off a console runs it under the replay (#44).

**`state.db` keeps the same rename protocol, and gets no A/B slots — decided,
not deferred.** A flying write (a sector written whole, to the wrong place) can
corrupt a file nothing opened, and A/B copies with a generation counter and a
CRC32C would detect that. They are not worth it here, because of what
`state.db` is: an optimisation and never a gate (`state_db.hpp`). A file that
is garbage, truncated, or fails the strict per-row JSON read costs a re-hash and
a renegotiation; a row that a stray write left *parseable but wrong* — the only
case a checksum would add — costs a wrong arbitration, and every arbitration
that replaces local bytes backs them up first under the property above. So the
worst a corrupted baseline can do is put the server's copy on the card with the
local one under `.backup/` — a conflict a person can undo (M7-1), never a lost
save. Save files themselves keep the rename protocol too: doubling every save
write is the cost the issue ruled out. If `state.db` ever becomes a gate —
something a tick refuses to run without — this decision has to be reopened.

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

#### The losing side stays on the server too (#240)

`.backup/` is on one SD card, where no other device can recover it. So keep
both also uploads the local bytes as an **archival** save, following RomM's own
"keep both" guidance (#226 R4):

1. fetch the server copy to a staged file and verify it;
2. back up the local file under `.backup/`;
3. `POST /api/saves` with the backup's bytes, **no `slot`**, the local emulator
   (what step 0 reported), `device_id`, `content_hash` and `overwrite=false`,
   under `<stem> [switch-conflict <YYYY-MM-DD_HH-MM-SS>].<ext>`;
4. commit the server copy onto the save, and confirm it.

RomM never pairs a `null`-slot row in negotiate (it reads `slot_not_null` rows
only) and never prunes one, so the copy cannot come back into sync, and the
server copy still wins on the card. The name carries the conflict's second
because RomM tags no `null`-slot upload: it keeps the name as sent, and a second
upload of a name it already holds replaces that row's bytes. No `session_id`:
RomM counts every upload naming one as a completed operation of the session.

The upload follows the backup rather than preceding it, so a card that refuses
the backup — every tick, until someone fixes it — refuses before anything is
sent, and a stuck conflict does not leave an archival row per tick. It precedes
the commit, so the server holds the copy before the card loses it. A conflict
that lands on a path nothing was at (a placement) replaced nothing and uploads
nothing. The 409's conflict (#232) runs through the same branch.

**A failed upload does not stop the conflict.** The local bytes are in
`.backup/` exactly as before; the result is `ArchiveOutcome::kFailed`, the tick
logs that the bytes are only on this card, and the conflicts screen says the
server holds no copy (`conflicts::LocalCopy::kCardOnly`), on the row and in the
detail. An entry recorded before #240 says nothing either way.

### Formats only one emulator reads

Six of Tico's systems write saves no libretro core reads — DuckStation's `.mcd`
card, Mupen64Plus's split N64 files, PPSSPP's folders, and whatever Flycast,
DraStic and YabaSanshiro write [#44 verifies]. They sync under a `tico-*`
emulator id (step 0, item 6) and **only with the same module on another
Switch** (#226 D3). Adapters that would rewrite one format into the other are
filed after #44 has captured real files; until one is proven byte for byte,
isolation is what keeps a progress-destroying guess off the card.

The id alone does not do it. RomM pairs saves on `(rom_id, slot)` whatever
wrote them (`sync-negotiate-pairs-across-emulators.json`), and the `emulators`
filter (#236) is one list per request: a console that reads RetroArch's psx
core beside DuckStation names both, so a plan can still offer a Beetle `.srm`
for DuckStation's card. So `ExecutePlan` asks before it fetches anything
(`scan::Interchangeable`): **a `tico-*` save is never written where anything
else reads, and nothing else is ever written over one.** A matched save is read
as the emulator step 0 reported for it; a first-time restore as the emulator
its placement was made for (`place::Placement::emulator` — `place::Placer`
already places only where `place::Compatible` allows, #237, and this is the
executor's own check behind it), else as what the scan would report for the
path (`ExecuteOptions::reads_as`, `scan::EmulatorAt`). With nothing to say,
a `tico-*` save is not placed at all, and the check comes before the
placement's folder is made. A refusal is `OperationError::kOtherEmulator`: nothing fetched, nothing
backed up, nothing touched, and counted failed — the plan asked for something
this client will not do, and it will ask again until one of the two saves
moves. The same holds inside #232's resolution of a 409, which executes through
the same path. Two libretro cores in one slot are not this rule's: that is RomM's
pairing and #236's filter.

| System | Tico module | Emulator id | Syncs with | Status in M10 |
|---|---|---|---|---|
| NES | FCEUmm | `fceumm` | RetroArch `fceumm`, the web player | full [verify bytes] |
| SNES | Snes9x | `snes9x` | RetroArch `snes9x`, the web player | full (`.rtc` in a slot of its own) |
| GB / GBC | Gambatte | `gambatte` | RetroArch `gambatte` | full [verify RTC] |
| GBA | mGBA | `mgba` | RetroArch `mgba`, the web player | full [verify] |
| Genesis / SMS / GG / Sega CD | Genesis Plus GX | `genesis_plus_gx` | RetroArch `genesis_plus_gx` | full [verify `.brm`] |
| Arcade | FinalBurn Neo | `fbneo` | RetroArch `fbneo` | full [verify] |
| PS1 | DuckStation | `tico-duckstation` | DuckStation on another Switch | Switch-only: `<game>.mcd`, card 2 `<game>_2.mcd` (own slot), one card for every disc, kept under disc 1's rom; a first-time restore is placed under those names, and a card kept under a later disc's rom is not placed |
| N64 | Mupen64Plus | `tico-mupen64plus` | Mupen64Plus on another Switch | Switch-only: `.eep`, `.sra`, `.fla`, `.mpk` each a save in its own slot (`autosave-eep` …) |
| Dreamcast | Flycast | `tico-flycast` | Flycast on another Switch | Switch-only, for a file named after its rom [verify on #44: Flycast may name VMUs by game id] |
| NDS | DraStic | `tico-drastic` | DraStic on another Switch | Switch-only (`.dsv` likely) [verify on #44] |
| Saturn | YabaSanshiro | `tico-yabasanshiro` | YabaSanshiro on another Switch | Switch-only [verify the file name on #44] |
| PSP | PPSSPP | `tico-ppsspp` | — | not synced: its saves are folders, a *save folder* skip; the `PSP-<folder>.zip` adapter needs #44 and RomM's per-entry ZIP hash (#234) |

"Full" needs the other device on the same core for that system. A RetroArch save
for an isolated system — Beetle PSX, `mupen64plus_next`, RetroArch's `ppsspp` —
still syncs between RetroArch installs, the Switch's own included; it is only
never written into Tico's folder, nor Tico's into RetroArch's.

### A save deleted on the server

5.4.0 remembers what each slot lost (`deleted_assets`: up to 100 content hashes
per `(rom_id, slot)`, each with when it was lost). A client that reports a slot
the server has **emptied**, holding one of those versions written no later than
its removal, is answered `delete` / `Save was deleted on the server`, with
`save_id: null` — and must not upload it again. RetroArch deleting a `.srm`
removes every version of it, so a deletion arrives from other devices as well as
from the web UI (M10-7, #233; `compare_missing_server_save` in
`handler/sync/comparison.py`).

The client obeys it, because the server is the source of truth, and obeys it the
way it obeys an overwrite, because the file is just as gone:

1. **Only a save this tick reported.** The operation is matched on
   `(rom_id, slot)` to the scan's own targets and never to a path invented for
   it (`ExecuteOptions::place` is a download's). A save the scan did not report
   — an undecided Tico account (#231), a folder it could not read — cannot be
   removed.
2. **Backed up first**, through the same `sync::BackUpFirst`, under the same
   name rules a download's backup gets — so the backup is durable, name and all,
   before anything is removed.
3. **The backup is checked against the digest the tick reported.** The ruling
   was about those bytes; a save the player wrote to after the scan is progress
   it never saw, so it stays and its stray copy is removed. With no digest to
   hold it to, the delete is refused before anything is copied.
4. **Then the file is removed**, and its directory synced. A power cut at any
   call leaves the save or its backup, never neither (`execute.crash_delete`).

The removal is recorded in the conflict history as `deleted` (M7-1), so the
overlay lists it with its backup and can restore it. A restored file is newer
than the removal, so the next negotiation plans its upload: progress on the
device, which RomM then holds again.

`[sync] on_server_delete = keep` leaves the file where it is instead
([CONFIG.md](CONFIG.md#on_server_delete-a-save-deleted-in-romm)), and the
**client** keeps the promise not to upload it: the slot's `state.db` row becomes
the reported bytes marked `deleted_on_server`, and `sync::RunTick` leaves a save
out of the negotiation while its digest is that row's. RomM's own memory is not
enough on its own — it is bounded, and it reads the same bytes with a later mtime
as progress, so an emulator that writes its save back on closing would have it
uploaded straight away (`tick.deleted_keep` shows exactly that with the mark
disabled). New bytes are offered as usual, and the first sync of the slot clears
the mark. Both a removed and a kept delete count as completed in `complete`.

A `delete` is obeyed only where `server::ServerCapabilities::delete_action` says
the server means one (#228); elsewhere it is counted as not understood, like an
action this build does not know. Every server a tick reaches is 5.4.0 or later,
so in practice that is everywhere.

#### Finding the backup again, and putting it back

A backup is only a safety net if a human can find one without an SD reader, so
every overwrite is recorded in `conflicts.db` beside `state.db` as it happens —
a conflict, a download that replaced a file, a save removed because RomM deleted
it (#233), and the first two on the states side
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
| `kRemoved` | **erased**: the row describes a file that is gone. |
| `kKept` | **rewritten** from the reported facts with no server half and `deleted_on_server` set, so the next tick withholds those bytes ([above](#a-save-deleted-on-the-server)). Any later advance clears the mark. |
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
1.4 MiB, so a memo per directory is unbounded in the one dimension that is
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
- neither does one the client has no previous observation of — "new since the
  last tick" cannot be told from "its `state.db` row went away", and the rows go
  away for reasons that are the client's *own* writes (a download whose bytes
  could not be read back, an M7-1 restore, a baseline a yanked card left
  unreadable). It costs the first write to a genuinely new save; the alternative
  is the whole library reported as played the day `state.db` will not parse;
- the first tick after a boot with no buffer produces nothing, and must: with no
  window there is no lower bound at all;
- and a window longer than `play::kMaxWindowSeconds` (twelve hours) produces
  nothing either. The arithmetic is still right — the play did happen in there —
  but the half of the window the client controls is "when did I last look", and a
  console that was off for a week did not look for a week. Twelve rather than
  something tight because a console suspended overnight fires one tick on wake
  and the session it derives spans the sleep; refusing *that* would cost play
  that really happened. Every path that gives up on a tick still moves the window
  forward (`SdEngine::StampPlayWindow`), so the bound is a backstop rather than
  the usual case.

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

An answer this build cannot read is a **warning**, not an error: it leaves
`play_session_ingest` absent, which already means "keep them buffered", and the
tick completes normally. A malformed play-session answer failing a completion the
server actually performed would be play time costing a sync tick, which is the
one thing this feature may not do.

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
| id X    | id X, moved     | unchanged | download, **only if the row's `emulator` is this state's core** — else keep both |
| id X    | id X, moved     | changed   | **keep both** |

A server state no local file claimed is *placed* — and a placement may only
**create**. Where it goes is the caller's (`sync::StateSyncOptions::place`),
because the folder depends on the rom's platform and on which emulator the state
belongs to; nothing said is a named failure rather than a guessed path. The
engine's is the save rule above (`place::Placer::State`), with one difference:
the state keeps the server's name, since `(rom_id, file_name)` is what pairs it
— Tico's slots excepted, below.

**A state moves only between devices running its core** (#239). A state is a
core's memory, so `place::LoadsState` is stricter than a save's
`place::Compatible`: the same id, or two ids RomM files under one RetroArch core
folder, and never a state that names no emulator. Since a POST moves a row's
`emulator`, a row this console paired with can come back holding another core's
state, which is kept apart rather than downloaded. And since states have no
negotiate to filter by `emulators`, the client filters the listing itself: a row
the placer calls `foreign` — another core's, or in a slot the frontend the
platform is played in has no file for — is counted in `filtered` and passed over
silently, the way a state for a rom this console lacks is. DuckStation's
`tico-duckstation` states therefore meet only DuckStation on another Switch, and
a RetroArch PSX core's never land in Tico's folder.

**Tico's slots, under RetroArch's names** (`[sync] map_state_slots`, off by
default; #226 D4). Tico writes `<game>.state0`–`.state4` for menu slots 1–5 and
`.state5` for Auto; RetroArch writes `<game>.state{k}` for slot k,
`.state` for slot 0 and `.state.auto` for Auto. With the mapping on, the scan
reports a Tico state of a libretro module under RetroArch's name — the key,
the upload and `state.db` all use it, the card keeps Tico's — and the placer
writes RetroArch's back under Tico's (`scan::RetroArchStateName`,
`scan::TicoStateName`, `core/include/rommsync/state_names.hpp`):

| Slot | Tico | RetroArch |
|---|---|---|
| menu slot k (1–5) | `.state{k-1}` | `.state{k}` |
| Auto | `.state5` | `.state.auto` |
| RetroArch slot 0 | — | `.state` |

Turning the mapping on or off renames the keys, not the files, so a key can meet
a `state.db` row taken of a *different* file — the card's `Game.state0`, now
`Game.state1`, meets the row its own `Game.state1` was synced under. The table
reads that as "this console played on" and would upload menu slot 1 over slot
2's row. So an upload over a row is kept both instead while another scanned
state of the rom still matches that row exactly (`RowDescribesAnother`).

Either way a placement takes only a name its frontend loads: `.state0`–`.state5`
in Tico's folder, `.state`, `.state<N>` (N ≥ 1) or `.state.auto` in RetroArch's.
RetroArch's `.state.auto` is matched to its rom by the name before `.state`, like
every other state. The `<state>.png` beside a state is uploaded as its
`screenshotFile` under the server's name plus `.png`, which is how RomM binds a
screenshot to a state; it is never itself scanned.

If there
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
be recorded. `kSuperseded` is every `Session is already <status>` but
`COMPLETED`: on 5.4.0, a session closed on purpose (#235). In all three the
transfers already happened and the baseline is on the card, so the answer is the
next tick's own negotiation.

### What the tick says it did

`TickOutcome` is split on what the scheduler would *do*: `kCompleted`,
`kPartial` (something did not happen — an operation, or the baseline reaching the
card; the next negotiation plans it again), `kUnreported` (the transfers landed,
the accounting did not, and the baseline *is* on the card — the two are asked
about in that order, because a bad minute loses both and only one of them is
expensive), `kOffline` (nothing was written — back off), `kRefused` (an answer
that does not change on a second attempt), `kUnauthorized`, `kRescanNeeded`,
`kCanceled`, and `kUnsupported` (a server below RomM 5.4.0: nothing was asked of
it — see [Before step 0](#before-step-0--which-romm-this-is)).
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
- **No tick runs while a game is in the foreground** (M10-16, #242). A game
  holds its save in memory and writes it when it closes — Tico does on exit —
  so a save downloaded under it is overwritten with the old copy, which the next
  tick uploads as the newest, and a save uploaded under it is the one from
  before the session. A tick that falls due is held and the sysmodule asks
  again every few seconds (`pmdmntGetApplicationProcessId`); one tick runs once
  the game has closed, however many fell due. "Sync now" during a game is that
  same tick (`SyncOutcome::kDeferred`). The download queue is not held — a rom is
  not a save. `engine.foreground` holds this against the real RomM.
- Partial plan failure → complete with the accurate `operations_failed`; leave
  unsynced files for next tick. Never leave a half-written save (write to temp,
  fsync, rename). An `action` this build does not recognise is counted with
  `operations_failed` and not with `operations_completed`: the server planned
  work that did not happen, and calling it completed tells RomM the client did
  something it cannot name.
- One backup per overwrite, always, before writing — and per removal, before
  removing (#233).
- A card that refuses -- full, failing, or gone -- costs the incoming save and
  never the one on the card: a refused transfer leaves the save as it was, a
  refused backup stops the overwrite, and neither advances the baseline. The
  operation's message says which (`the card is full`, `the card refused the
  write`). `tick.download_full`, `tick.download_eio` and `tick.backup_full` hold
  this with the refusal forced through `io::Disk` (#202).
- Verify a downloaded save against the MD5 in `content_hash` /
  `server_content_hash`, and its length against `file_size_bytes`.
