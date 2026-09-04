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

### Provisioning the fixture

`compose up` gives you a RomM that is *running*. It does not give you one that is
*usable*, and the difference is easy to miss: `seed.sh` stages ROMs on disk, but
nothing imports them, so `/api/heartbeat` stays green while `/api/roms` reports
an empty library — and whichever test needed a ROM fails looking like a bug in
its own code.

`server/testing/provision.py` closes that gap, and `setup.sh` runs it after the
stack is up:

```bash
./.venv/bin/python server/testing/provision.py --base-url "$ROMM_BASE_URL"
```

It is idempotent, so re-running it is how you repair a fixture rather than
rebuild it. In order it:

1. creates the fixture admin — `POST /api/users` is unauthenticated *only* while
   no user exists, so a second run gets a 403 and treats it as "already done"
2. registers every platform folder on disk
3. **scans the library, and waits for the scan to finish.** The scan is not a
   REST call: `POST /api/tasks/run/scan_library` refuses, because the task is
   declared `manual_run: False`. It is driven over socket.io (`scan` →
   `scan:done`), and authorization comes from the session cookie captured at
   connect, not from the payload
4. creates the curated `Handheld` collection
5. mints a client token **through the real device-code flow, with no human**

Step 5 is the one that matters beyond this issue. The device-code grant is built
around a person approving a code in a browser, which CI and an agent cannot do —
but `/api/auth/device/approve` is an ordinary authenticated endpoint, so the
provisioner approves its own code and the flow closes itself. `probe_contract.py
--auth --negotiate` now runs unattended for the same reason.

Credentials land in `server/testing/fixture-auth.env` (gitignored):
`ROMM_FIXTURE_TOKEN`, `ROMM_FIXTURE_DEVICE_ID`, `ROMM_FIXTURE_COLLECTION_ID`,
`ROMM_FIXTURE_ROM_COUNT`. The account matches `rig::kUser`/`kPassword` in
`tests/rig.hpp`, which creates the same user itself — whichever runs first wins,
and they only agree because the constants are kept identical.

`rig.smoke` asks whether RomM is up; `rig.provisioned` asks whether it is usable.
Both are needed: the first passed throughout the entire period the library was
empty.

### Keeping the captured contract honest

`contract.captures` re-runs `probe_contract.py --auth --negotiate
--sync-scenarios` against the fixture and compares the result with the committed
`server/contract/captures/` — the responses [API_CONTRACT.md](API_CONTRACT.md),
[AUTH.md](AUTH.md) and [SYNC_PROTOCOL.md](SYNC_PROTOCOL.md) quote and that M1/M2
are coded against. It compares field names, nesting and JSON types, plus the
`action`/`reason` pairs each scenario is named for; ids, timestamps and the
per-run slot names differ every run and are ignored.

The scenarios upload throwaway saves — an empty-save negotiate returns an empty
`operations` array, so `upload`, `download`, `no_op` and `conflict` cannot be
observed without state on the server. Every save they create is deleted again,
and the probe refuses a non-loopback URL. `RUN_SERIAL`, because they mutate the
library other tests read.

A red run here means RomM changed under the docs, not that a test is flaky:
re-capture, read the diff, and fix whatever the docs claimed.

The Python tooling lives in a per-worktree `.venv` built from
`server/requirements.txt` by `setup.sh`. The C++ build needs none of it.

### Worktree isolation

Several agents work in parallel worktrees, and sync tests mutate saves by design.
Each worktree therefore gets its **own** RomM: `scripts/orca/env.sh` derives a
compose project name and two ports from the worktree path and writes them to
`.env`, so no two worktrees share a port or a database. Only immutable, expensive
things are shared between worktrees — the checksum-pinned ROM downloads and the
content-addressed ccache, declared in [`orca.yaml`](../orca.yaml).

`.env` has more than one writer: `compose.sh` generates a missing one before it
runs, so the `romm` tab writes it at the same moment `setup.sh` does. Each run
renames its own `mktemp` file into place, which makes that safe in both
directions — no reader sees a half-written file, and no writer has its temp file
carried off by another's rename. The derivation is a pure function of the
worktree path, so concurrent writers publish identical bytes.

### The romm tab

`defaultTabs` in [`orca.yaml`](../orca.yaml) asks Orca for a tab following this
worktree's RomM. It is a request, and Orca does not always grant it: worktrees
have come up with the tabs created, untitled, and running none of their commands
while the fixture was live and serving. A running fixture nothing is showing
looks exactly like one that failed to start.

So `setup.sh` runs `scripts/orca/ensure-romm-tab.sh` — before it brings the stack
up, because a failure there is exactly when you need the tab and `set -e` would
otherwise skip the check. `romm-logs.sh` publishes a pidfile while it follows; if
one is live, Orca honoured the request and the script does nothing, and if not,
it asks the `orca` CLI for the tab itself and waits for a follower to actually
appear before claiming success. It never fails setup, and it no-ops on a plain
clone or in CI where there is no CLI and no tabs to create.

Do not expect the tab to be called `romm`. Orca titles a tab after the command it
was given and re-derives that title from the running process, so a rename races
it and lands only sometimes. The tab is therefore created with exactly the
command `orca.yaml` asks for, so that when the rename loses, the title still
reads `./scripts/orca/romm-logs.sh`.

Removing a worktree runs `scripts/orca/archive.sh`, which takes that worktree's
stack and volumes down with it. It derives the project name from the worktree
path rather than reading `.env` back, so a worktree whose `.env` never got
written still tears down cleanly, and it checks afterwards that docker holds
nothing under that project instead of assuming `down` worked.

Stacks can still outlive their worktree — one deleted with `rm -rf`, or removed
while Docker was stopped. The fixture restarts `unless-stopped`, so those come
back on every docker start and hold two ports each. Sweep them up with:

```bash
./scripts/orca/reap.sh          # list stacks with no worktree, change nothing
./scripts/orca/reap.sh --yes    # remove them, volumes included
```

It protects two sets of stacks: those belonging to a live worktree of this repo,
whose project names it derives the same way `env.sh` did, and those whose
containers still point at a directory that exists — which covers a separate
clone of this repo that `git worktree list` cannot see. Anything it cannot
positively establish as stale is left alone, and it refuses to sweep at all
rather than run with an incomplete idea of what is live, so a stack it cannot
account for survives instead of being deleted.

The gap that remains: a *separate clone* whose stack has been reduced to volumes
alone leaves nothing pointing at its directory, so it looks stale. Run the
dry-run first if more than one clone of this repo is in play.

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
