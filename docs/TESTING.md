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

## There is no mock RomM

Tests run against a **real RomM 5.2.0** in Docker, not a hand-written mock. A
mock only ever encodes what we *believe* RomM does; when a test passes against
the real thing, the behaviour is real, and the whole class of "the mock drifted
from the server" bugs cannot happen.

The cost of that choice is that a healthy RomM will not fail on command — and the
failure paths are where saves get corrupted. So a small **fault-injecting proxy**
sits between the harness and RomM ([`server/testing/fault_proxy.py`](../server/testing/fault_proxy.py)).
It forwards everything untouched until a scenario is armed, then damages exactly
one thing about a genuine response:

| Mode | Effect |
|---|---|
| `status` | Return an arbitrary status (401 mid-sync, 500, …) instead of forwarding |
| `truncate` | Forward the real response but cut the body short, cleanly, with no `Content-Length` to compare against |
| `drop` | Cut the body short, keep the real `Content-Length`, and abort the connection with a TCP reset |
| `stall` | Delay past the client's timeout |

Scenarios can target a path prefix, skip the first N matching requests
(`after`), and apply a fixed number of times before auto-disarming (`count`), so
"the third call to `/api/sync/negotiate` fails with 401" is one line:

```bash
. ./.env
curl -XPOST "$PROXY_BASE_URL/__fault" \
  -d '{"mode":"status","status":401,"path":"/api/sync/negotiate","after":2}'
```

The proxy never synthesises a RomM response of its own — that is what keeps the
fidelity while still making failure deterministic and repeatable in CI.

`truncate` and `drop` differ in exactly one header, and the difference is the
point. A server whose connection dies mid-transfer had already promised a
`Content-Length`, so `drop` keeps it: the client is owed bytes it never gets, and
any competent transport says so. `truncate` drops the header, which is the
harder case — a clean, short, *plausible* response that no transport can fault.
Only a size the caller already knew catches that one, which is why
`http.truncate` asserts on both halves of that behaviour.

## The fixture account

RomM starts empty, so anything authenticated has to create an account first. The
tests do it themselves (`tests/rig.hpp`, `EnsureUser`) rather than a provisioning
script, so a worktree created before those tests existed also just works:
`rommsync` / `rommsync-test-only`, admin, created through RomM's own
first-user bootstrap and then used via `POST /api/token`. Those are not secrets —
this RomM is a throwaway container bound to `127.0.0.1` holding no real data,
the same reasoning that already puts the database password in the compose file.

## The test ladder

Climb it in order. You do not go up a rung until the one below is green.

| Rung | Environment | Proves | Console? |
|---|---|---|---|
| 1 | **Host build + real docker RomM (+ fault proxy)** | Engine logic against a genuine RomM, plus every forced edge case: 401, conflict, partial failure, `Range` resume, multi-file skip. Runs in CI. | No |
| 2 | **Ryujinx NRO + docker RomM** | The real Horizon path (`ssl`/`fs`/`socket`) via a manually-launched NRO built from the same core lib. The only pre-hardware place Horizon code actually runs. | No (emulator) |
| 3 | **Real hardware (M8)** | Final bring-up, on a backup SD/emuMMC, NRO first then a disabled sysmodule. | Yes — last, gated |

Rung 1 is the day-to-day loop. Rung 2 is where the M0-1 `ssl` question gets
answered without hardware. Rung 3 is a single gated milestone, not part of normal
development.

## Rung 1 — host + docker RomM (primary)

```bash
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

- The fixture is [`server/testing/docker-compose.yml`](../server/testing/docker-compose.yml):
  RomM 5.2.0 on a throwaway volume, with metadata providers disabled so runs are
  deterministic and offline.
- [`seed.sh`](../server/testing/seed.sh) populates the library from
  `roms.manifest` — **homebrew and freely redistributable ROMs only**,
  checksum-pinned, since it is fetched in public CI. Two fixtures no real ROM
  provides are generated deterministically instead: a large file for `Range`
  resume, and a multi-file rom directory for the `has_multiple_files` skip.
- Every save-overwrite test asserts a backup is written **first**
  ([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md) hard rule).
- The `http.*` tests cover the native `HttpClient` backend end to end: `get`,
  `status`, `post_json`, `post_form`, `multipart`, `download`, `range`, `drop`,
  `resume`, `resume_no_range`, `truncate`, `stall`, `cancel`. One CTest entry
  each, so a red run names the behaviour rather than "the http tests". They run
  `RUN_SERIAL` because the fault proxy holds one armed scenario for all
  clients. The streaming ones pull RomM's own frontend bundle — the only large
  resource the rig serves that does not first need a library scan, which is
  socket.io-driven rig work belonging to M0-5.
- With Docker stopped, `rig.smoke` reports **Skipped** rather than failing, so a
  local `ctest` is still useful. CI configures with `-DROMMSYNC_REQUIRE_RIG=ON`,
  which turns the same condition into a failure — a green CI run always means the
  tests actually ran.

### Worktree isolation

Several agents work in parallel worktrees, and sync tests mutate saves by design.
Each worktree therefore gets its **own** RomM: `scripts/orca/env.sh` derives a
compose project name and two ports from the worktree path and writes them to
`.env`, so no two worktrees share a port or a database. Only immutable, expensive
things are shared between worktrees — the checksum-pinned ROM downloads and the
content-addressed ccache, declared in [`orca.yaml`](../orca.yaml).

## Rung 2 — Ryujinx NRO

- A standalone **NRO** (manually launched, *not* a sysmodule, *not* on the boot
  path) built from the same core lib, run in Ryujinx against docker RomM.
- Confirms the `ssl`-service backend and libnx `fs`/socket behavior on Horizon.
- If Ryujinx's `ssl`/`bsd` support turns out to be insufficient, the same NRO is
  the fallback experiment on a **backup SD** — still never an auto-boot sysmodule.

## Rung 3 — the v1 gate and real hardware (M8)

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
- **Emulator (rung 2):** the `ssl`-service TLS path and libnx fs/socket calls —
  as far as Ryujinx implements them.
- **Only real hardware (M8):** sysmodule heap behavior under Atmosphère, boot-time
  scheduling, ovl-sysmodules toggling, and the Tesla/Ultrahand overlay UI itself
  (emulators don't load overlays). These are deliberately the *last* things
  touched, and only after everything above is proven.
