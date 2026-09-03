# Testing strategy

**Hard rule: nothing touches a real Switch or a production RomM until v1 is
proven off-console.** The whole engine is built so its logic runs on a laptop and
in CI, with only a thin Horizon glue layer (the `ssl` service, `fs`, `tipc`) left
as the single hardware-ish unknown — and even that is exercised in an emulator
before any console.

## Why this is possible

All networking sits behind the [`HttpClient`](DEVELOPMENT.md#tls-in-a-sysmodule)
interface. On the host we implement it with libcurl; on the Switch with the
Horizon `ssl` service. The sync engine, matcher, download worker, config parser,
`state.db`, and the IPC protocol don't know or care which backend is underneath —
so they are fully testable natively.

## The test ladder

Climb it in order. You do not go up a rung until the one below is green.

| Rung | Environment | Proves | Console? |
|---|---|---|---|
| 1 | **Host build + mock RomM** | Engine logic + every edge case (401, conflict, partial failure, `Range` resume, multi-file skip). Fast, offline, runs in CI. | No |
| 2 | **Host build + docker RomM** | Fidelity — the mock matched a real RomM 5.2.0. | No |
| 3 | **Ryujinx NRO + docker RomM** | The real Horizon path (`ssl`/`fs`/`socket`) via a manually-launched NRO built from the same core lib. The only pre-hardware place Horizon code actually runs. | No (emulator) |
| 4 | **Real hardware (M8)** | Final bring-up, on a backup SD/emuMMC, NRO first then a disabled sysmodule. | Yes — last, gated |

Rungs 1–2 are the day-to-day loop. Rung 3 is where the M0-1 `ssl` question gets
answered without hardware. Rung 4 is a single gated milestone, not part of normal
development.

## Rung 1 — host harness + mock RomM (primary)

- `make test` builds the core library + native `HttpClient` and runs the suite
  against the **mock RomM** (`server/testing/` — see M0-5). No external services.
- The mock is scriptable: it can return any negotiate plan and force 401s,
  conflicts (`server_wins` / `device_wins` / `keep_both`), mid-plan failures, and
  dropped-connection resume.
- Every save-overwrite test asserts a backup is written **first**
  ([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md) hard rule).

## Rung 2 — docker RomM (fidelity)

- `server/testing/docker-compose.yml` (M0-6) brings up a real RomM 5.2.0 on a
  throwaway volume, seeded with a small `Handheld` collection and a sample save.
- Used to capture real response shapes (`probe_contract.py`, M0-4) and to re-run
  the harness scenarios against the genuine article.
- **Never** point this at a production RomM database.

## Rung 3 — Ryujinx NRO

- A standalone **NRO** (manually launched, *not* a sysmodule, *not* on the boot
  path) built from the same core lib, run in Ryujinx against docker RomM.
- Confirms the `ssl`-service backend and libnx `fs`/socket behavior on Horizon.
- If Ryujinx's `ssl`/`bsd` support turns out to be insufficient, the same NRO is
  the fallback experiment on a **backup SD** — still never an auto-boot sysmodule.

## Rung 4 — the v1 gate and real hardware (M8)

Do not begin M8 until **M8-1** is fully checked:

- Sync (M2), downloads (M3), auth (M1), config+IPC (M5) all green on host +
  docker RomM.
- `ssl` backend proven on Ryujinx (or isolated NRO on backup SD).
- Backups verified by tests; a tagged v1 build (M6); a known-good NAND/SD backup.

Then **M8-2**: on a spare/backup SD or emuMMC, run the NRO first, install the
sysmodule **disabled**, and only sync against a **disposable** collection with
`.backup/` populated before ever pointing at the real library.

## What can and can't be tested off-console — honest limits

- **Fully off-console:** auth flow, sync negotiate/execute/complete, conflict
  handling, downloads + resume + hash verify, config parsing, `state.db`, backup
  guarantees, the IPC command/response protocol.
- **Emulator (rung 3):** the `ssl`-service TLS path and libnx fs/socket calls —
  as far as Ryujinx implements them.
- **Only real hardware (M8):** sysmodule heap behavior under Atmosphère, boot-time
  scheduling, ovl-sysmodules toggling, and the Tesla/Ultrahand overlay UI itself
  (emulators don't load overlays). These are deliberately the *last* things
  touched, and only after everything above is proven.
