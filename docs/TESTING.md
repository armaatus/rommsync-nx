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
- The `pair.*` tests cover the device-code flow end to end (M1-1): `happy`,
  `starting`, `mid_poll`, `denied`, `expired`, `retry`, `unauthorized`,
  `rejection_streak`, `stall`, `drop`, `lost_grant`, `payload`. `starting` is
  the only one that needs two threads — it drives `Begin()` on one and
  `status()` on the other, which is the threading contract the overlay depends
  on. Every code is one a real RomM issued, every approval
  goes through RomM's own `/api/auth/device/approve` and every denial through
  `/api/auth/device/deny` — the endpoints that let a test be the human the grant
  assumes. They wait out real poll intervals rather than faking a clock, because
  RomM answers `slow_down` to a client that undercuts the `interval` it asked
  for, and that is the one rule the loop has to obey. `RUN_SERIAL`, like the
  `http.*` tests, for the same fault-proxy reason.
- One rig constraint worth knowing before it looks like a flake: RomM rate
  limits `POST /api/auth/device/init` to **ten a minute, per IP**. A console
  pairs once; the suite opens a dozen codes from one address, so `pair.*` waits
  a rate-limited init out rather than failing on it. The wait is bounded, so an
  init broken for any other reason still goes red.
- `core.token_store` covers `token.dat`: the atomic write, and specifically what
  survives a write that cannot complete. The interesting one is the process
  being **killed mid-write** — forced with `fork()` and `RLIMIT_FSIZE`, so a
  child asked to write a record far larger than its file-size limit is killed by
  `SIGXFSZ` inside the write, at a byte offset the kernel picks, with no cleanup
  on the way out. That is a power cut, deterministically, and without the timing
  race a sleep-then-kill would have. It also asserts that no token, refresh
  token or device code appears in any message this code can produce, by running
  every failure path with a distinctive needle in the record and searching the
  output for it.
- `core.device_identity` covers `device.dat` and the `client_device_identifier`
  under it: the SHA-256 against FIPS 180-4's published vectors and against the
  block-boundary lengths the padding gets wrong, and then the property that
  actually matters — the identifier is the *same* identifier across a restart, a
  re-pair, a different seed, an interrupted commit and a corrupt record.
- `auth.scopes` reads the scope list out of
  [API_CONTRACT.md](API_CONTRACT.md#scopes-to-request) and compares it to
  `MinimumScopes()`. Editing the document without editing the code, or the other
  way round, fails here — including adding a scope marked "only if…" to the set
  the client actually requests.
- None of those three touches the network, so they never skip.
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

That is only half the guarantee. `contract.captures` pins the captures to the
server; `auth.shapes` pins the **structs** to the captures, by parsing the
committed files through `rommsync::auth` rather than restating them as literals.
A field RomM renames therefore fails twice — once as drift, once as a struct
that can no longer read its own capture — and a struct that quietly guessed a
name cannot pass. Neither `auth.shapes` nor `core.json` (the JSON reader itself,
mostly a list of bodies that must be *refused*) touches the network, nor does
`core.token_store`, so none of them ever skips: a body that should have been rejected is a bug with or without a
server to have sent it.

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

### The romm log tab

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

### The RomM browser tab

The log tab tells you the server is alive. It does not tell you what the scan
decided: which platform RomM inferred each seeded folder is, what a rom's
metadata actually looks like, whether the `Handheld` collection ended up with
anything in it. Those are the questions that come up while implementing against
the API, and they are one glance away in RomM's own web UI — if you are logged
in, on the right port, out of three.

So `setup.sh` finishes by running `scripts/orca/romm-browser.sh`, which opens an
Orca browser tab on this worktree's RomM and authenticates that tab's session
with the fixture credentials. It runs after provisioning because provisioning is
what creates the admin it signs in as.

It signs in from inside the page rather than by driving the login form: one
`fetch` to `/api/login` with the same Basic header and `X-CSRFToken` echo that
`provision.py` uses, leaving the tab holding a genuine `romm_session` cookie. The
form is a Vue app whose markup is RomM's to change; `/api/login` is the pinned
contract in [API_CONTRACT.md](API_CONTRACT.md).

Reopen or re-authenticate it at any time:

```bash
./scripts/orca/romm-browser.sh
```

It always exits 0 — a browser tab must never fail a worktree — so it reports
success through `.orca/romm-browser.state` instead, and `setup.sh` only promises
a signed-in tab in its summary when that file says one exists.

It is idempotent — a tab belonging to **this** worktree and already pointing at
**this** worktree's RomM is reused rather than duplicated. Both halves of that
matter: `orca tab list` reports every tab on the machine, so matching on the port
alone would hand the script another worktree's tab to log in and navigate, and
matching on the worktree alone would adopt a tab someone opened on unrelated
documentation. Like the log tab, it never fails setup and no-ops on a plain clone
or in CI where there is no Orca CLI.

### The agent's prompt is drafted, not sent

Creating a worktree from a linked issue puts the resolved spec — what
`issue-command.sh` produced — into the agent's composer as a **draft**, and does
not submit it. That is Orca's own behaviour for issue-linked workspaces and it is
not reachable from `orca.yaml`: the app picks `promptDelivery: draft` whenever a
work item is linked. The result is a fully provisioned worktree whose agent has
read nothing, waiting for someone to walk over and press Return.

`scripts/orca/agent-autostart.sh` presses it. Everything about how it is written
comes from one asymmetry: the keypress it replaces is a small annoyance, while
submitting a *human's* half-typed prompt for them would be a real one. So it
presses Return on Orca's paste and on nothing else, and it takes three
independent conditions to get there.

- **A linked issue.** `--watch` refuses to run unless `orca worktree current`
  reports one. Without a linked issue Orca drafts nothing, so any text in that
  composer was typed by a person and there is nothing here to do.
- **Arriving with the agent.** Orca delivers the draft as part of launching the
  agent — sometimes as a launch argument, sometimes pasted once the agent is
  ready — so there is a short grace window (30s) for it to appear. Once that
  window closes on an empty composer, text showing up later is someone typing,
  and watching stops there. The window is the one place where the guarantee is
  a judgement rather than a proof: a person who opens a brand-new issue-linked
  agent tab, types, and pauses, all within those 30 seconds, would have it
  submitted. `AGENT_AUTOSTART_GRACE_SECONDS` shortens it.
- **Read twice, identically.** A paste caught part way through is a shorter
  string, and submitting there would hand the agent a truncated issue.

`orca terminal read --json` is what makes any of this possible: it reports unsent
composer text as `draft`, separately from the terminal's output, so there is
something specific to look for rather than a keystroke fired on a timer.

`setup.sh` starts it detached, in watching mode, as its last act. The order
forces that: `setupAgentStartupPolicy: wait-for-setup` holds the agent's tab
until setup returns, so while setup is running the draft does not exist yet. The
watcher gives up after two minutes — what it waits for is part of the agent's
launch, not something that can turn up later.

Run by hand it takes a single shot and is *not* gated on a linked issue: running
it is itself the deliberate act, and "submit whatever is drafted" is then what
was asked for. Set `ROMMSYNC_AGENT_AUTOSTART=0` to keep the manual Return.

Removing a worktree signals the watcher on the way out (`archive.sh`). It
identifies the process before signalling it, because a pidfile outlives a
`kill -9` and a reboot, and the number in it is then whatever the system reused.

Removing a worktree runs `scripts/orca/archive.sh`, which takes that worktree's
stack and volumes down with it. It derives the project name from the worktree
path rather than reading `.env` back, so a worktree whose `.env` never got
written still tears down cleanly, and it checks afterwards that docker holds
nothing under that project instead of assuming `down` worked.

Only the Orca UI runs that hook by itself. `orca worktree rm` **skips**
`orca.yaml` archive hooks unless `--run-hooks` is passed, so removing a worktree
from the CLI without it leaves the whole stack running with no worktree left to
find it from. Pass the flag, or sweep afterwards with `reap.sh --yes`.

Stacks can still outlive their worktree — one deleted with `rm -rf`, removed with
`orca worktree rm` and no `--run-hooks`, or removed while Docker was stopped. The fixture restarts `unless-stopped`, so those come
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
