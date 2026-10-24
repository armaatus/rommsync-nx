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

Every worktree provisions itself with `./scripts/orca/setup.sh` — Orca runs it
on creation (`orca.yaml`), and after a plain `git worktree add` you run it: it
derives isolated ports, seeds ROM fixtures, builds, and starts its own RomM with
the fixture provisioned inside it. Ports live in `.env` — never hardcode one, and
never assume `8080` or `1515`.

```bash
cmake -S . -B build && cmake --build build      # build
ctest --test-dir build --output-on-failure       # test
ctest --test-dir build -R sync --output-on-failure   # one group

# the three Switch targets, built by devkitPro rather than CMake. Nothing built
# here runs anywhere before the M8-1 gate; see switch.mk.
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:20260219 \
  bash -lc 'make -C sysmodule && make -C overlay && make -C tlsprobe'

./scripts/orca/env.sh                            # regenerate .env
./scripts/orca/compose.sh up -d                  # start RomM  (logs -f to follow)
./server/testing/seed.sh                         # re-seed ROM fixtures
./.venv/bin/python server/testing/provision.py   # scan the library, mint a fixture token
./scripts/orca/tls-fixture.sh up                 # TLS in front of RomM, for tlsprobe
./evals/lint.sh                                  # the agent config still holds
./scripts/orca/setup.sh                          # provision this worktree (idempotent)
./scripts/orca/archive.sh                        # tear its RomM down, before removing the worktree
./scripts/orca/reap.sh                           # RomM stacks whose worktree is gone (--yes removes)
```

If `ctest` reports `rig.smoke` as **Skipped**, RomM is not running — start it
rather than working around it.

## How tests work here

There is no mock RomM. Tests run against a **real RomM** in Docker — the
version `ROMM_VERSION` in `.env` names, 5.4.0 by default and the floor since #235
([TESTING.md](docs/TESTING.md#choosing-the-romm-version)) — so a passing test
means the behaviour is genuinely real. Failure modes a healthy RomM
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

At most **2 worktrees** run at once, and an issue is startable only
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
implementation ends up departing from them, the commit message says so under `## Plan`.
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
and say so in your commit message. Never edit them as a side effect of rewording a body.

## Finishing

Your job ends at a commit on your worktree's branch.

1. `ctest --test-dir build --output-on-failure` is green, and your change has a
   test that would have failed before it — run it and read the output before
   reporting complete. For a bug fix, commit the failing test before the fix.
   A change under `sysmodule/`, `overlay/` or `tlsprobe/` is built with devkitPro
   too (the docker line above); `switch-build` is a required check and CI will
   not merge a red one.
2. Any issue your work invalidated is edited; your commit message says which and why.
3. The final commit's message ends with `## Plan` — what the issue asked, where
   you departed, which issues you edited — and `Closes #N` on its own line.
   `unblock.yml` reads that line, once merged, to free dependants.
4. **Stop.** Do not merge, and do not open a pull request. The maintainer's
   session merges your branch into the milestone's integration branch
   (`fleet/mN`) and opens one pull request for it; the maintainer reviews it
   against [REVIEW.md](REVIEW.md) and merges it. `guard.py` refuses
   `gh pr merge` everywhere, and `gh pr review` from a linked worktree.

## What is watching you

- **Skills** ([`.claude/skills/`](.claude/skills)) load when they become
  relevant: `save-safety` on anything that writes a save, `core-portability` on
  anything reaching for a platform facility inside `core/`, `tracker-is-spec` on
  anything that finds an issue to be wrong. They are advisory.
- **Hooks** ([`.claude/hooks/guard.py`](.claude/hooks/guard.py)) are not. They
  block, with an explanation: merging or reviewing your own PR, force-pushing
  `main`, writing to `server/contract/captures/` or a secret — by an edit, or by
  a redirect, `cp`, `mv`, `tee`, `sed -i` and the other shell spellings its
  selftest lists; the probe's `--capture` is the captures' one sanctioned
  writer. The paths and names it protects are
  [`.claude/hooks/guard.json`](.claude/hooks/guard.json). A block is a rule you
  were about to break, not a bug. It is not a sandbox: `guard.py --selftest` is the
  record of what has been checked, not a proof that nothing gets through.
- **Subagents**: [`verifier`](.claude/agents/verifier.md) gives an independent
  build-and-test verdict from a fresh context before you finish;
  [`researcher`](.claude/agents/researcher.md) answers questions about the
  codebase without spending your context on the files it read.
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
| `scripts/orca/` | Per-worktree provisioning: `setup.sh`, `env.sh` (ports and URLs into `.env`), `compose.sh`, `tls-fixture.sh`, `archive.sh`, `reap.sh`, and `issue-command.sh`, the brief `orca.yaml` starts an agent with. |
| `evals/` | Regression tests for the agent configuration itself, and the shell scanners. |
| `.claude/` | Skills, subagents and hooks — what steers and what blocks. |
| `AGENTS.md` | Symlink to this file, for agent tools that look for that name. |
