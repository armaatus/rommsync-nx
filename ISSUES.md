# Backlog

Milestones and issues for rommsync-nx. `scripts/create_issues.sh` creates these
on GitHub (labels, milestones, issues) via `gh`. Issue IDs (M0-1 …) are
referenced from the docs; keep them stable.

Labels: `sysmodule`, `overlay`, `server`, `auth`, `sync`, `download`, `ipc`,
`config`, `packaging`, `docs`, `risk`, `test-harness`, `good-first-issue`,
`blocked`, `ready`.

## Dependencies

GitHub has no native blocking relation, so an issue that cannot start yet carries
`Blocked by #N` lines in its body and the `blocked` label; when every blocker is
closed it becomes `ready`. Labels are maintained by
[`.github/workflows/unblock.yml`](.github/workflows/unblock.yml) on merge — do
not hand-edit them.

## Testing policy (hard rule)

**Nothing touches a real Switch or a production RomM until v1 is proven
off-console.** All of M1–M5 is developed and tested on the host harness against a
throwaway **real** RomM in docker, with a fault-injecting proxy forcing the
failure paths (see [TESTING.md](docs/TESTING.md)). There is no mock RomM. Real
hardware is a single, gated milestone at the end (**M8**).

---

## M0 — Foundations & de-risking

Everything here runs off-console. The goal is a test harness that can prove the
whole engine on a laptop/CI before a single line runs on real hardware.

- **M0-0** `test-harness` `packaging` `docs` **Project scaffolding — make the
  repo buildable, testable and agent-ready.** CMake+CTest host build, the real
  docker RomM fixture + seed script, the fault-injecting proxy, `orca.yaml`
  per-worktree provisioning, `CLAUDE.md`/`AGENTS.md`, a CI that can genuinely go
  red, and the blocked/ready workflow. Everything else in M0 depends on this.
- **M0-1** `risk` `sysmodule` `test-harness` **Sysmodule TLS feasibility via the
  `ssl` service — Ryujinx-first, off the boot path.** De-risking spike (no longer
  a blocker): a standalone, manually-launched **NRO** (not a sysmodule) does a TLS
  GET over the `ssl` service, run **in Ryujinx** against docker RomM; only if
  Ryujinx can't exercise `ssl`, run the same NRO on a backup SD. Measure heap;
  go/no-go vs. fallbacks. See [DEVELOPMENT.md](docs/DEVELOPMENT.md#tls-in-a-sysmodule).
- **M0-2** `sysmodule` `test-harness` `docs` **`HttpClient` interface + native
  (libcurl) backend.** GET/POST/multipart, streaming download-to-file, timeouts,
  cancellation, `Range`. Swappable TLS backend; ship the **native backend** so all
  networked logic is testable off-console. This is the interface every later
  milestone includes, so it lands alone before any fan-out.
- **M0-3** `packaging` `test-harness` **CI builds host harness (runs tests) +
  Switch skeleton (artifacts).** Native build runs the test suite against the
  docker RomM rig; devkitpro build compiles the sysmodule + overlay to artifacts
  (not run). Scaffolded in M0-0; this issue completes the devkitpro job once the
  Makefiles exist.
- **M0-4** `server` `test-harness` `docs` **Capture real RomM auth/sync response
  shapes** by running `server/probe_contract.py --auth --negotiate` against a
  **docker RomM (never production)**; paste real JSON into
  [API_CONTRACT.md](docs/API_CONTRACT.md) / [AUTH.md](docs/AUTH.md) and pin them
  as typed structs.
- **M0-5** `server` `test-harness` `sync` **Host test harness + fault-injection
  scenarios.** Wire the core engine to the real docker RomM (M0-6) through the
  fault proxy, and cover every edge case: 401, conflict, partial failure,
  `Range` resume, multi-file skip. Backbone of "prove v1 before hardware."
- **M0-6** `server` `test-harness` **docker-compose RomM fixture** — the primary
  test rig. One-command real RomM 5.2.0 on a throwaway volume, seeded with
  homebrew ROMs, used to capture shapes and to back the Ryujinx tier. Scaffolded
  in M0-0; this issue seeds a curated collection and a scoped client token.
- **M0-7** `docs` `risk` `test-harness` **"No real hardware / no production data
  until proven v1" policy + M0 exit gate.** Write [TESTING.md](docs/TESTING.md),
  update DEVELOPMENT.md, define the v1 gate M8 depends on.

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

## M8 — Hardware bring-up (after proven v1)

First contact with a real modded Switch. **Gated:** nothing here starts until the
v1 gate passes. Everything below the thin Horizon glue has already been proven on
host + docker RomM + Ryujinx.

- **M8-1** `risk` **v1 gate — do not touch hardware until this passes.** Checklist:
  sync/downloads/auth/config+IPC all green on host + docker; `ssl` backend proven
  in Ryujinx NRO; backups verified by tests; tagged v1 build; NAND/SD backup ready.
- **M8-2** `sysmodule` `risk` **First real-console smoke test** on a backup
  SD/emuMMC: manually-launched NRO first, then sysmodule installed **disabled**,
  first sync against a **disposable** collection with `.backup/` populated. Never
  the production library until all pass.
