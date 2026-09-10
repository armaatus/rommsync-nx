# Architecture

Two on-console components plus shared state on the SD card. The server is stock
RomM 5.2.0 — we add nothing server-side except (optionally) an HTTPS reverse
proxy and a dedicated user/token (see [SECURITY.md](SECURITY.md)).

## Components

### 1. `sys-rommsync` — background sysmodule

A Horizon **sysmodule** (title under `/atmosphere/contents/<TID>/`) loaded at
boot by Atmosphère. No UI. Responsibilities:

- **Auth manager** — performs the device-code flow once, persists the resulting
  bearer token securely on the SD, refreshes as needed. ([AUTH.md](AUTH.md))
- **HTTP client over TLS** — uses the **Horizon `ssl` system service** via libnx
  (`sslCreateContext`, `ssl` + `bsd`/`socket`) rather than bundling mbedTLS, to
  keep the sysmodule's memory footprint viable. This is the single biggest
  technical risk; it is isolated behind an `HttpClient` interface so it can be
  swapped. ([DEVELOPMENT.md](DEVELOPMENT.md#tls-in-a-sysmodule))
- **SD enumeration** — reading a directory is the second thing after HTTP that
  Horizon and the host do differently (`fsdev`/`readdir` over `sdmc:` versus
  `<filesystem>`), so it sits behind the `fs::FileSystem` interface in
  `core/include/rommsync/file_system.hpp`. It also owns the one mapping the rest
  of the engine cannot do for itself — `Resolve` turns an SD-root path into the
  one `io::ReadFile` and `state::HashFile` can open. The host backend is
  `host/native_file_system.cpp`; **the Horizon one is not written yet** and is
  what the save scanner needs on the console.
- **Sync engine** — the negotiate → execute → complete loop.
  ([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md))
- **Download worker** — drains a queue of rom ids, downloads (with `Range`
  resume + hash verify) into the mapped emulator folder.
- **Scheduler** — triggers a sync on: boot (after network is up), a configurable
  interval, and on explicit request from the overlay.
- **IPC service** — a small `tipc`/`cmif` service the overlay connects to for
  status, config, toggle, queue, and "sync now".

Runs paused/idle when disabled (toggled via ovl-sysmodules boot flag, or the
overlay's enable switch). Those are **two** switches, not two spellings of one:
the boot flag decides whether the process exists at all, and `[sync] enabled`
decides whether a resident one syncs. The table, and the four states the overlay
draws off them, are
[DEVELOPMENT.md#the-two-switches](DEVELOPMENT.md#the-two-switches).

Must be a good background citizen: low idle CPU, back off when offline, never
block boot.

### 2. `ovl-rommsync` — Ultrahand / Tesla overlay

An `.ovl` built on [libultrahand](https://github.com/ppkantorski/libultrahand)
(append the `ULTR` signature). Pure front-end — it holds no sync logic, it just
drives the sysmodule over IPC. Screens:

- **Status** — connection, last sync time, counts (uploaded/downloaded/conflict),
  current download + queue depth.
- **Library / queue** — browse platforms → roms (paged from RomM *via the
  sysmodule*), add to download queue, see progress.
- **Sync** — "Sync now", enable/disable auto-sync, per-emulator toggles.
- **Settings** — server URL, re-run pairing, folder-map overrides, interval,
  states-sync on/off, conflict policy display.

Model the toggle + IPC pattern on
[ovl-sysmodules](https://github.com/ppkantorski/ovl-sysmodules) and the sys-clk
overlay.

### 3. Shared state on SD

- `sdmc:/config/rommsync/config.ini` — user config ([CONFIG.md](CONFIG.md)).
- `sdmc:/config/rommsync/token.dat` — bearer token + device_id. In the clear:
  Horizon's FAT32 has no permission bits, so the mitigation is minimum scopes
  and revocability, not secrecy ([SECURITY.md](SECURITY.md)).
- `sdmc:/config/rommsync/device.dat` — the `client_device_identifier`, derived
  once and kept for the life of the SD. Separate from `token.dat` because it has
  to survive a re-pair ([AUTH.md](AUTH.md#client-identifier)).
- `sdmc:/config/rommsync/auth.json` — the server's standing verdict on the
  token, to be written once `auth::Gate` has counted enough consecutive `401`s
  or `403`s to give up on the pairing ([AUTH.md](AUTH.md#re-pairing--revocation)).
  Read at boot and written by `SdEngine::ObserveAnswer`, which every call the
  worker makes reports into (M7-2, #37) — see AUTH.md.
  One JSON object, `{"format":"rommsync-auth","version":1,"block":"revoked"}`,
  and it **exists only while the console is blocked** — so the overlay's re-pair
  prompt is up on the first poll after a boot rather than after the engine has
  spent the same budget of requests again. Never a gate on boot: an unreadable or
  unrecognised one is no verdict, and the console finds out by asking. Beside
  `token.dat` rather than inside it, because a verdict re-derivable in three
  requests has no business being written into the one file that cannot be
  re-fetched without a human at a browser.
- `sdmc:/config/rommsync/state.db` — last-synced hash/mtime per (rom, slot) so the
  client can tell which side changed. A **flat, line-oriented file**: a version
  line, then one JSON object per row (`core/include/rommsync/state_db.hpp`).
  Not SQLite — `core/` may include only standard and `rommsync/` headers, so it
  is not linkable from the portable engine, and a sysmodule heap does not want
  it. It is an optimisation and never a gate: a missing, truncated or corrupt
  file yields an empty baseline and a diagnostic, and the tick hashes
  everything.
- `sdmc:/config/rommsync/queue.json` — pending downloads. One JSON object,
  `{"format":"rommsync-queue","version":1,"entries":[…]}`, written with
  `io::WriteAtomically` after **every** state transition
  (`core/include/rommsync/download.hpp`): that is what lets an entry left
  `active` by a power cut be resumed from its `.part` rather than restarted or
  forgotten. Like `state.db` it never blocks boot — a corrupt or oversized file
  yields an empty queue and a diagnostic — but unlike it, a queue entry is
  something the user asked for, so the *writer* refuses rather than silently
  dropping an entry it cannot store. Finished entries stay in the file: the
  overlay's queue screen is served from here with the server down.
- `sdmc:/config/rommsync/.backup/` — pre-overwrite copies of saves, on a
  conflict *and* on any download that replaced a file.
  `<rom_id>-<slot>-<unix seconds>.<ext>`, written before the overwrite by
  `sync::ExecutePlan` (docs/SYNC_PROTOCOL.md#backups). A save state's is
  `<rom_id>-state-<name>-<unix seconds>.<ext>`, written by `sync::SyncStates`
  into the same directory. The directory has to exist: `core/` cannot create
  one, and a missing `.backup/` stops the overwrite rather than proceeding
  without a copy. **Nothing ever deletes a file from here** — not the history
  bound below, not a restore, not the recovery sweep, which removes only an
  interrupted `.tmp`.
- `sdmc:/config/rommsync/conflicts.db` — the index that makes `.backup/`
  legible (M7-1). A header line and one JSON object per overwrite, newest
  first, written with `io::WriteAtomically` after **every** entry:
  `{rom_id, rom name, file name, slot, emulator, when, the server's reason,
  local size/MD5/mtime, server_content_hash, server_updated_at, backup path}`.
  Bounded at `conflicts::kMaxEntries`; the oldest entry falls off the end and
  its backup stays on the card. The overlay reads it over `ListConflicts` and
  asks the sysmodule to put bytes back over `RestoreBackup`
  (`core/include/rommsync/conflict_log.hpp`). `[sync] conflict_show` hides the
  *screen* and never the recording.
- `sdmc:/config/rommsync/play.db` — play time this console recorded and has not
  yet handed to RomM (M7-4). `conflicts.db`'s format: a header line carrying the
  next id **and the moment the last tick looked at the saves**, then one JSON
  object per session, oldest first. Bounded at `play::kMaxSessions`; the oldest
  fall off the *front*, because this is a queue the server drains rather than a
  list a person reads. Nothing on the IPC surface exposes it — play time is not
  on any screen (`core/include/rommsync/play_sessions.hpp`).

- `sdmc:/config/rommsync/rommsync.log` — what went wrong, in the words
  docs/TROUBLESHOOTING.md is written against (M7-3). One line per event:
  `<ordinal> <level> <event> <detail>`, where the event is one of a closed set
  (`core/include/rommsync/log.hpp`) and the ordinal, not a clock, is what orders
  the file — Horizon has no usable time until `timeInitialize`, and a console
  whose clock never comes up is a supported state. **Bounded**: the live file is
  capped at `log::kMaxFileBytes` and rotates to one `rommsync.log.old`, so the
  pair is twice that and never more. **Never carries a secret**: `log::Redact`
  runs inside the writer, not at the call sites, so a bearer token, a
  `device_code` and a `user:password@` cannot reach the card from a call site
  nobody reviewed. Written by `SdEngine`, which is the half that holds a whole
  tick's outcome; `core/` builds the sentences and hands them up. Not durable,
  deliberately — `io::FileSync` commits the whole card and a log is not worth
  that, so a power cut costs the tail. Served to the overlay by `GetLog` from an
  in-memory copy, so reading it needs neither an SD reader nor a card read.

The overlay and sysmodule both read config; the **sysmodule owns writes** to
token/state to avoid races — the overlay asks it to change things via IPC.

## Data flow: a sync tick

```
scheduler fires
  → engine scans SD save/state dirs (per CONFIG folder map)
  → match each file to a rom_id (fs_name_no_ext, platform-scoped)
  → hash (MD5) each, reusing state.db's digest when mtime+size match
  → build SyncNegotiatePayload.saves[]
  → POST /api/sync/negotiate  → {session_id, operations[]}
  → for each op:
        upload   → POST /api/saves?...&overwrite=true (multipart saveFile)
        download → GET /api/saves/{id} for the size
                 → GET /api/saves/{id}/content → stage → verify MD5
                 → back up the local file → commit → POST .../downloaded
        conflict → the same, keep-both: RomM sends NO resolution, so the
                   server's copy lands and the local bytes stay in .backup/
  → record every overwrite in conflicts.db, so a human can find the backup
    (conflicts::RecordSaves / RecordStates, M7-1)
        noop     → skip
  → update state.db  (sync::FinishTick, and in this order: complete is
                      accounting, so a failed one must not cost the baseline)
  → POST /api/sync/sessions/{session_id}/complete
    carrying play_sessions[] derived from the mtimes that moved since the last
    tick (play::DeriveSessions, M7-4) -- optional, and never able to fail a tick
```

## Data flow: a download

```
overlay queues rom_id  → IPC → sysmodule appends to queue.json
worker: GET /api/roms/{id} → resolve fs_name, platform_fs_slug, size, sha1
  → target = folderMap[platform_fs_slug].roms + fs_name
  → GET /api/roms/{id}/content/{fs_name} with Range resume
  → verify sha1 → move into place → report progress over IPC
```

## Sleep and wake: the PSC contract

The console suspends. Horizon tells a process so through **PSC** (`psc:m`, the
power state controller), and a process that does not subscribe is one the
transition happens *around*: its `fsp-srv` sessions and its sockets are still in
use when the services behind them go down. For other projects that is a crash --
sys-autopilot's reports were `omm` aborting with `2165-1001` beside `bsdsocket`
aborts, on a console that hard-restarts on wake. For this one it is worse: the
worker can be mid-`sync::Execute` when the card goes away, which is a save write
cut in half and hard rule 2 defeated by timing.

It is not an edge case reached once a week. The console wakes itself
periodically with the display off to talk to the network, and sysmodules run in
those windows too.

```
psc:m  --SleepReady-->  power::Watcher  --Quiesce-->  SdEngine
                              |                          | cancel the drain
                              |                          | cancel the tick
                              |                          | wait: worker parked
                              |                          | wait: no save write
                              |<-------------------------+
                              +--Acknowledge--> psc:m
   ...console sleeps, and this process issues nothing at all...
psc:m  --MinimumAwake-->  power::Watcher  --Resume-->  SdEngine
                              +--Acknowledge--> psc:m
```

Four rules, and each of them is a different broken console:

1. **Every request is acknowledged, exactly once.** PSC waits for the
   acknowledgement before it moves the console on, so a module that does not
   answer is a whole system frozen with no fatal and no crash report (sys-con#155
   is exactly that, and sys-clk#85 is a console that then never wakes).
2. **The acknowledgement goes out only once nothing is in flight** — no
   `download::Drain`, no `sync::RunTick`, and no save write from a restore. The
   acknowledgement *is* this process telling PSC that the card and the sockets
   are free of it, so sending it early is the lie hard rule 2 cannot survive.
3. **...and it goes out anyway, within `kQuiesceBudget`.** Rules 1 and 2 pull in
   opposite directions and the budget is where they meet: three seconds, after
   which the acknowledgement goes out with a `warn` in the log saying what was
   still busy. Waiting longer is rule 1 broken.
4. **Nothing is issued between `SleepReady` and the next `MinimumAwake`.** Not
   `EssentialServicesAwake`, which says the critical services are back and
   nothing about `fsp-srv` — Atmosphere's own `erpt` turns its filesystem access
   back on at `MinimumAwake` and not before, and this follows it. Sockets do not
   survive sleep at all.

The wake does **not** fire a backlog. `sync::Scheduler` states its interval on
the wall clock rather than on `steady_clock`, so an eleven-hour suspend is one
interval elapsed and one tick, not twenty-two.

The module registers with `PscPmModuleId_Fs` as its dependency, which is what
decides *where in the order* it is told: early on the way down, late on the way
back up, which is what a process that writes to the card needs. Its own
`PscPmModuleId` is arbitrary — there is no registry, and third-party sysmodules
pick unused values with nothing managing the collisions (`power_psc.cpp` says
which one and why).

The seam is `sysmodule/source/power.hpp`: `power::Module` is the platform half
(`psc:m`, in `power_psc.cpp`, compiled only by devkitPro), `power::Sink` is what
a transition does to this process (`SdEngine`), and `power::Watcher` is the loop
between them. Everything but the first is host-testable, which is what `power.*`
and `engine.sleeps` are.

## Explicitly out of scope (v1)

- Multi-file / disc-set roms (`has_multiple_files`) — **detect and skip**, with
  a message the overlay can render; never download the zip.
  **The worker is what refuses it**, and there is no refusal at the door: the
  sysmodule's engine holds no `roms::RomIndex` — a tick fetches one and does not
  keep it, deliberately (`sysmodule::SdEngine::Enqueue`) — so `Enqueue` records
  the id, and the drain settles the entry `kSkipped` with a reason the queue
  screen draws. `download::EnqueueRom` is the door-side check for a caller that
  *does* hold an index, and answers `ipc::Error::kMultiFile` there; nothing on
  the console calls it today. This paragraph said the opposite until M9-5 (#197)
  wired the worker and found the two disagreeing.

  The reason is not effort. `GET /content` on a disc set serves a zip RomM
  builds on the fly with **no `Content-Length`**, and the rom-level `sha1_hash`
  is the digest of *neither* disc — so there is nothing to check the archive
  against, and an unverified file would land on the card under the rom's name
  looking like a rom.

  A v2 downloads the discs one at a time with
  `GET /api/roms/{id}/content/{file_name}?file_ids=<one files[].id>` — raw
  bytes, a real length, `Range` resume, and a per-file `sha1_hash` to verify
  against — and then has the part that is not HTTP to do: an `.m3u`, a
  per-emulator folder layout, and per-file verification. Route and traps:
  [API_CONTRACT.md](API_CONTRACT.md#multi-file-roms-and-the-three-things-that-are-not-what-they-look-like).

  `has_nested_single_file` — a directory holding exactly **one** file — is *not*
  this case. It is an ordinary download and the skip must not fire on it
  (`download.nested`). It lands under `fs_name` with the **extension of the one
  file inside the directory** on the end — `fs_name` alone carries none for an
  emulator to pick a core from, and the inner file's own name is neither unique
  across roms nor what an emulator names the save after (#92,
  [API_CONTRACT.md](API_CONTRACT.md#has_nested_single_file-is-not-a-disc-set)).
- Save **states** sync on by default (fragile across cores) — supported but
  opt-in.
- Installing NSP/XCI Switch titles — this project is emulator content only;
  ownfoil/DBI already cover that.

## Why this shape

- **Sysmodule, not NRO:** downloads and save sync must happen without you sitting
  in an app; a background service is the only way to get "auto-sync after I
  play."
- **Overlay, not full GUI:** Ultrahand is already in your button-combo muscle
  memory; a `.ovl` gives toggle + config + status with almost no UI surface to
  maintain.
- **Client calls out over HTTPS:** works from any network, and nothing listens
  inbound on the Switch — the correct security posture ([SECURITY.md](SECURITY.md)).
