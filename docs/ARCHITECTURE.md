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
overlay's enable switch). Must be a good background citizen: low idle CPU, back
off when offline, never block boot.

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
- `sdmc:/config/rommsync/state.db` — last-synced hash/mtime per (rom, slot) so the
  client can tell which side changed. A **flat, line-oriented file**: a version
  line, then one JSON object per row (`core/include/rommsync/state_db.hpp`).
  Not SQLite — `core/` may include only standard and `rommsync/` headers, so it
  is not linkable from the portable engine, and a sysmodule heap does not want
  it. It is an optimisation and never a gate: a missing, truncated or corrupt
  file yields an empty baseline and a diagnostic, and the tick hashes
  everything.
- `sdmc:/config/rommsync/queue.json` — pending downloads.
- `sdmc:/config/rommsync/.backup/` — pre-overwrite copies of saves, on a
  conflict *and* on any download that replaced a file.
  `<rom_id>-<slot>-<unix seconds>.<ext>`, written before the overwrite by
  `sync::ExecutePlan` (docs/SYNC_PROTOCOL.md#backups). The directory has to
  exist: `core/` cannot create one, and a missing `.backup/` stops the
  overwrite rather than proceeding without a copy.

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
        noop     → skip
  → POST /api/sync/sessions/{session_id}/complete
  → update state.db
```

## Data flow: a download

```
overlay queues rom_id  → IPC → sysmodule appends to queue.json
worker: GET /api/roms/{id} → resolve fs_name, platform_fs_slug, size, sha1
  → target = folderMap[platform_fs_slug].roms + fs_name
  → GET /api/roms/{id}/content/{fs_name} with Range resume
  → verify sha1 → move into place → report progress over IPC
```

## Explicitly out of scope (v1)

- Multi-file / disc-set roms (`has_multiple_files`) — detect and skip with a
  clear message; revisit later.
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
