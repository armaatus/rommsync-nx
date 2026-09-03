# Backlog

Milestones and issues for rommsync-nx. `scripts/create_issues.sh` creates these
on GitHub (labels, milestones, issues) via `gh`. Issue IDs (M0-1 …) are
referenced from the docs; keep them stable.

Labels: `sysmodule`, `overlay`, `server`, `auth`, `sync`, `download`, `ipc`,
`config`, `packaging`, `docs`, `risk`, `good-first-issue`.

---

## M0 — Foundations & de-risking

- **M0-1** `risk` `sysmodule` **Prototype HTTPS from a sysmodule via the `ssl`
  service.** Standalone spike: from a minimal sysmodule, do a TLS GET to the RomM
  `/openapi.json` over the Horizon `ssl` service (+ `bsd`/socket) and print the
  status. Measure heap use. Decide go/no-go vs. fallbacks in
  [DEVELOPMENT.md](docs/DEVELOPMENT.md#tls-in-a-sysmodule). **Blocks everything
  networked.**
- **M0-2** `docs` **Define the `HttpClient` interface** (methods, streaming to
  file, timeouts, cancellation) so TLS backend is swappable.
- **M0-3** `packaging` **Repo skeleton builds in CI** (devkitpro docker image;
  empty sysmodule + overlay compile and produce artifacts).
- **M0-4** `server` **Confirm auth/sync response shapes** by running
  `server/probe_contract.py --auth --negotiate` against live RomM; paste the real
  response JSON into [API_CONTRACT.md](docs/API_CONTRACT.md) / [AUTH.md](docs/AUTH.md).

## M1 — Authentication

- **M1-1** `auth` `sysmodule` Implement device-code `init` → display code via IPC
  → poll `token`. Persist token + expiry.
- **M1-2** `auth` `docs` Verify and document the **init/token response fields**
  (from M0-4) and code against them.
- **M1-3** `auth` `sysmodule` Register device (`POST /api/devices`), cache
  `device_id`. Handle `allow_existing`.
- **M1-4** `auth` Token refresh + `401` handling → mark unauthenticated, signal
  overlay to re-pair.
- **M1-5** `auth` `config` Stable `client_device_identifier` derivation + secure
  token store ([SECURITY.md](docs/SECURITY.md)).

## M2 — Save sync engine

- **M2-1** `sync` `docs` Pin the exact `SyncNegotiatePayload.saves[]` entry
  fields from the snapshot; encode as a typed struct.
- **M2-2** `sync` Local scan + rom matching (fs_name_no_ext, platform scoping,
  ambiguity handling) per [SYNC_PROTOCOL.md](docs/SYNC_PROTOCOL.md#step-0--matching-local-files-to-roms).
- **M2-3** `sync` SHA1 hashing + `state.db` baseline (skip unchanged files).
- **M2-4** `sync` `negotiate` call + parse operations plan.
- **M2-5** `sync` Execute plan: upload / download / conflict / noop, with
  **mandatory pre-overwrite backup** and atomic writes.
- **M2-6** `sync` `complete` call + persist new baseline.
- **M2-7** `sync` Offline / partial-failure safety (abort clean, accurate
  `operations_failed`, retry next tick).
- **M2-8** `sync` Save **states** support behind `sync.states` flag.

## M3 — Downloads

- **M3-1** `download` `config` Platform→folder map (defaults + `config.ini`
  overrides) per [CONFIG.md](docs/CONFIG.md).
- **M3-2** `download` Queue model (`queue.json`) + worker loop.
- **M3-3** `download` Single-file rom download with **Range resume** + SHA1
  verify; stream to file (no whole-rom RAM buffering).
- **M3-4** `download` Skip/flag multi-file roms (`has_multiple_files`) cleanly.
- **M3-5** `download` Progress reporting over IPC.

## M4 — Overlay (ovl-rommsync)

- **M4-1** `overlay` `ipc` Minimal libultrahand overlay that connects to the
  sysmodule IPC and shows **Status**. Ships `.ovl` with `ULTR` signature.
- **M4-2** `overlay` Enable/disable auto-sync + "Sync now".
- **M4-3** `overlay` `download` Library browser (paged) → enqueue downloads →
  progress.
- **M4-4** `overlay` `config` Settings screen (server URL, interval, states,
  folder overrides, re-pair).
- **M4-5** `overlay` `auth` Pairing screen: show user_code + verification URL,
  live pair state.

## M5 — Config & IPC

- **M5-1** `config` `config.ini` parser + defaults + validation ([CONFIG.md](docs/CONFIG.md)).
- **M5-2** `ipc` Define IPC service (commands in
  [DEVELOPMENT.md](docs/DEVELOPMENT.md#ipc)); share the interface between
  components.
- **M5-3** `ipc` `config` Live config edits from overlay → sysmodule persists.
- **M5-4** `ipc` Paged list streaming (platforms, roms, queue) without large
  payloads.

## M6 — Packaging & CI

- **M6-1** `packaging` Install layout: sysmodule under
  `/atmosphere/contents/<TID>/`, overlay under `/switch/.overlays/`; a release
  zip that drops onto the SD.
- **M6-2** `packaging` `ovl-sysmodules` compatibility (toggle on/off, boot flag).
- **M6-3** `packaging` Versioning + GitHub Releases from CI tags.
- **M6-4** `docs` End-user install + first-run pairing guide.

## M7 — Polish

- **M7-1** `sync` Conflict UX in overlay (list, keep-both surfacing).
- **M7-2** Robust backoff/retry + battery-friendly scheduling (don't sync in
  sleep, resume on wake/network-up).
- **M7-3** `docs` Troubleshooting guide (401s, folder mismatches, Tico paths).
- **M7-4** Optional: play-session recording (`me.write`, `/api/play-sessions`).
