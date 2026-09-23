# rommsync-nx — working agreement

On-device RomM sync for a modded Nintendo Switch: a background **sysmodule**
(`sys-rommsync`) plus an Ultrahand/Tesla **overlay** (`ovl-rommsync`). Read
[README.md](README.md) for the product, [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
for the shape, and [docs/WORKFLOW.md](docs/WORKFLOW.md) for how work moves from an
idea to a merged PR — this file is the short form of it.

## Hard rules

1. **No real hardware, ever, until the v1 gate passes.** No real Switch, no
   production RomM, no production library. The gate is issue M8-1, and it is the
   command `./scripts/v1-gate.sh`. If a task seems to need hardware, it is the
   wrong task — say so instead of doing it.
2. **Back up before overwriting a save.** Every path that overwrites a save file
   writes a backup *first* and writes atomically. This is the one guarantee
   standing between a bug and a player's destroyed save. It is tested, not
   assumed — see [docs/SYNC_PROTOCOL.md](docs/SYNC_PROTOCOL.md).
3. **The server is the source of truth** for sync conflicts.
4. **Nothing in `core/` may include a host-only or libnx header.** Platform
   details live behind interfaces so the engine stays testable natively.
5. **No secrets in the tree.** `config.ini`, `token.dat`, `device.dat` and
   `.env` are ignored, together with the `.tmp`/`.old` an interrupted commit
   leaves beside them.

## Environment

A fleet worktree provisions itself when it is created (`scripts/fleet/setup.sh`
→ `.autofleet/setup.sh`): it derives isolated ports, seeds ROM fixtures, builds,
and starts its own RomM with the fixture provisioned inside it. Ports live in
`.env` — never hardcode one, and never assume `8080` or `1515`.

```bash
cmake -S . -B build && cmake --build build      # build
ctest --test-dir build --output-on-failure       # test
ctest --test-dir build -R sync --output-on-failure   # one group

# the three Switch targets, built by devkitPro rather than CMake. Nothing built
# here runs anywhere before the M8-1 gate; see switch.mk.
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest \
  bash -lc 'make -C sysmodule && make -C overlay && make -C tlsprobe'

./scripts/orca/env.sh                            # regenerate .env
./scripts/orca/compose.sh up -d                  # start RomM  (logs -f to follow)
./server/testing/seed.sh                         # re-seed ROM fixtures
./.venv/bin/python server/testing/provision.py   # scan the library, mint a fixture token
./scripts/orca/tls-fixture.sh up                 # TLS in front of RomM, for tlsprobe
./evals/lint.sh                                  # the agent config still holds
./scripts/fleet/fleet.sh status                  # what the fleet is running, and what is next
./scripts/fleet/reap.sh                          # RomM stacks whose worktree is gone (--yes removes)
```

If `ctest` reports `rig.smoke` as **Skipped**, RomM is not running — start it
rather than working around it.

## How tests work here

There is no mock RomM. Tests run against a **real RomM 5.2.0** in Docker, so a
passing test means the behaviour is genuinely real. Failure modes a healthy RomM
will not produce on demand (401 mid-sync, truncated body, dropped connection,
stall) are forced with the fault proxy in front of it:

```bash
. ./.env
# make the 3rd call to negotiate fail with 401, once
curl -XPOST "$PROXY_BASE_URL/__fault" \
  -d '{"mode":"status","status":401,"path":"/api/sync/negotiate","after":2}'
```

Modes are `status`, `truncate`, `drop` (real TCP reset), `stall`. Faults auto-disarm
after `count` uses (default 1) and belong to whoever armed them -- untagged, like the
one above, means everybody (#118). Reference: [fault_proxy.py](server/testing/fault_proxy.py).

Never add a commercial ROM to `server/testing/roms.manifest` — it is fetched in
public CI. Homebrew and freely redistributable only.

## Working in parallel

The fleet runs at most **2 worktrees** at once, and an issue is startable only
when it carries `ready` rather than `blocked` — labels [`unblock.yml`](.github/workflows/unblock.yml)
derives from the `Blocked by #N` lines in each issue body, never by hand. Do not
start a `blocked` issue, nor a **`needs-human-step`** one — its last step is the
maintainer's, or no agent can do it at all ([WORKFLOW.md](docs/WORKFLOW.md)).

One exception the labels cannot express: **a foundation issue lands alone.** An issue
defining an interface later issues include — M0-2's `HttpClient` — merges before
anything else starts, even if the labels say several things are ready: two agents
each inventing their own shared header is the one merge conflict worth serialising to
avoid. *Alone* runs both ways (#215).

## The issue is the plan

`/implement` opens with no planning phase: the issue's **Goal / Scope / Design
notes / Acceptance** are the plan, and they are meant to be sufficient. Where the
implementation ends up departing from them, the PR body says so under `## Plan`.
Departing is normal; departing silently is not.

## Code

- C++20. `-Wall -Wextra -Wpedantic -Werror` — warnings are errors, including in
  your branch.
- `core/` sources are globbed, so adding a `.cpp` needs no CMake edit.
- Every network call: timeout, offline-safe, retry with backoff. Never block boot.
- Match the surrounding code's naming and comment density. Comments explain
  *why*, not *what*.

## The tracker is the spec

Every issue carries **Goal / Scope / Design notes / Acceptance** — #5 and #40 are
the standard. Read yours in full before starting; it is meant to be sufficient.

It stays sufficient only if you maintain it. When the work turns up something the
issue did not know — an endpoint that differs from the pinned contract, scope a
closed issue already shipped, a constraint the code imposes — **edit the affected
issue as you find it**, including issues other than your own. Three agents in
three worktrees cannot see each other's findings; these bodies are the only
channel between them, and a stale one makes the next agent re-derive or
contradict a decision already made.

Never hand-edit the `blocked`/`ready` labels — `.github/workflows/unblock.yml`
derives them from the `Blocked by #N` lines below the `<!-- blockers -->` marker.
Those lines *are* editable, and a genuinely missing dependency should be added,
but changing one changes what other agents may start: do it deliberately, alone,
and say so in the PR body. Never edit them as a side effect of rewording a body.

## Finishing

Your job ends at an open pull request.

1. `ctest --test-dir build --output-on-failure` is green, and your change has a
   test that would have failed before it — run it and read the output before
   reporting complete. For a bug fix, commit the failing test before the fix.
   A change under `sysmodule/`, `overlay/` or `tlsprobe/` is built with devkitPro
   too (the docker line above); `switch-build` is a required check and CI will
   not merge a red one.
2. Any issue your work invalidated is edited; the PR body says which and why.
3. Push, and open the PR with `## Plan` — what the issue asked, where you
   departed, which issues you edited — and its `Closes #N` line. `merge-gate`
   requires the closing line, and `unblock.yml` reads it to free dependants.
4. **Stop.** The dispatcher runs the independent review against
   [REVIEW.md](REVIEW.md) and `.autofleet/review.md`; a `request-changes` buys
   exactly one fix session, the re-review of that fix is final, and the rules
   merge an approved PR. `guard.py` refuses `gh pr merge` and `gh pr review`
   from a fleet worktree. A PR touching `.github/`, `.claude/` or `.autofleet/`
   never merges itself; a person merges that one.

## What is watching you

- **Skills** ([`.claude/skills/`](.claude/skills)) load when they become
  relevant: `save-safety` on anything that writes a save, `core-portability` on
  anything reaching for a platform facility inside `core/`, `tracker-is-spec` on
  anything that finds an issue to be wrong. They are advisory.
- **Hooks** ([`.claude/hooks/guard.py`](.claude/hooks/guard.py)) are not. They
  block, with an explanation: merging or reviewing your own PR, force-pushing
  `main`, writing to `server/contract/captures/`, editing secrets — from a shell
  command as well as from an edit. What this project adds to the universal rules
  is [`.autofleet/guard.json`](.autofleet/guard.json). While the fleet is
  stopped, nothing outward leaves a fleet worktree. A block is a rule you were
  about to break, not a bug. It is not a sandbox: `guard.py --selftest` is the
  record of what has been checked, not a proof that nothing gets through.
- **Subagents**: [`verifier`](.claude/agents/verifier.md) gives an independent
  build-and-test verdict from a fresh context before you open a PR;
  [`researcher`](.claude/agents/researcher.md) answers questions about the
  codebase without spending your context on the files it read;
  [`reviewer`](.claude/agents/reviewer.md) is the dispatcher's, not yours.
- `./evals/lint.sh` (also `ctest -R agent.config`) checks that all of the above
  is still well-formed and still enforcing what it claims.

When you get the same correction twice, it belongs in this file or in a skill —
put it there as part of the work, not in a note to yourself.

## Layout

| Path | What |
|---|---|
| `core/` | Portable engine — auth, sync, downloads, config, state. Host-testable. |
| `host/` | Desktop backends for `core/`'s interfaces (libcurl `HttpClient`). Never built for Switch. |
| `sysmodule/` | `sys-rommsync`, the background engine. devkitPro Makefile. |
| `overlay/` | `ovl-rommsync`, the libultrahand overlay. devkitPro Makefile. |
| `tlsprobe/` | The M0-1 TLS spike: a manually-launched `.nro`, never installed. |
| `server/` | Pinned RomM API snapshot, contract probe, and the Docker test fixture. |
| `tests/` | CTest suites. |
| `packaging/` | What the release zip ships beside the two artifacts; `scripts/package.sh` builds it. |
| `scripts/fleet/` | autofleet, vendored: the dispatcher, the brief, the review, the cost report. Nothing in it knows this project. |
| `.autofleet/` | This project's answers to it: ports, compose file, test command, `setup.sh`, `guard.json`, `review.md`. |
| `scripts/orca/` | The project half of provisioning: `env.sh` (the fleet's ports plus our URLs), `compose.sh`, `tls-fixture.sh`. |
| `evals/` | Regression tests for the agent configuration itself, and the payload's own scanners. |
| `.claude/` | Skills, subagents and hooks — what steers and what blocks. Merged by a person, never by the rules. |
| `AGENTS.md` | Symlink to this file, for agent tools that look for that name. |
