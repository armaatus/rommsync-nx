# rommsync-nx — working agreement

On-device RomM sync for a modded Nintendo Switch: a background **sysmodule**
(`sys-rommsync`) plus an Ultrahand/Tesla **overlay** (`ovl-rommsync`). Read
[README.md](README.md) for the product, [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
for the shape, and [docs/WORKFLOW.md](docs/WORKFLOW.md) for how work moves from an
idea to a merged PR — this file is the short form of it.

## Hard rules

1. **No real hardware, ever, until the v1 gate passes.** No real Switch, no
   production RomM, no production library. The gate is issue M8-1. If a task
   seems to need hardware, it is the wrong task — say so instead of doing it.
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

Your worktree provisioned itself when it was created (`orca.yaml` →
`scripts/orca/setup.sh`): it derived isolated ports, seeded ROM fixtures,
configured the build, started its own RomM, opened it in a browser tab already
signed in, and submitted your issue to this agent. Ports live in `.env` — never
hardcode one, and never assume `8080` or `1515`.

```bash
cmake -S . -B build && cmake --build build      # build
ctest --test-dir build --output-on-failure       # test
ctest --test-dir build -R sync --output-on-failure   # one group

# the three Switch targets, built by devkitPro rather than CMake. Nothing built
# here runs anywhere before the M8-1 gate; see switch.mk.
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest \
  bash -lc 'make -C sysmodule && make -C overlay && make -C tlsprobe'

./evals/lint.sh                                  # the agent config still holds
./scripts/orca/env.sh                            # regenerate .env
./scripts/orca/compose.sh up -d                  # start RomM  (logs -f to follow)
./scripts/orca/romm-browser.sh                   # reopen the signed-in tab
./scripts/orca/romm-logs.sh                      # reopen the log tab
./server/testing/seed.sh                         # re-seed ROM fixtures
./.venv/bin/python server/testing/provision.py   # scan the library, mint a fixture token
./scripts/orca/tls-fixture.sh up                 # TLS in front of RomM, for tlsprobe
./scripts/orca/reap.sh                           # RomM stacks whose worktree is gone (--yes removes)
```

There is a browser tab on this worktree's RomM, logged in as the fixture admin.
Use it — a scan result, a platform slug or a rom's real metadata is one glance
away there and several API calls away otherwise. `romm-browser.sh` reopens it.

Removing a worktree from the **Orca UI** runs the teardown hook. Removing it with
`orca worktree rm` does **not** unless you pass `--run-hooks`, and the stack it
leaves behind restarts `unless-stopped` and holds two ports forever. Either pass
the flag or sweep afterwards with `./scripts/orca/reap.sh --yes`.

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

Modes are `status`, `truncate`, `drop` (real TCP reset), `stall`. Faults
auto-disarm after `count` uses (default 1). Full reference: the module docstring
in [server/testing/fault_proxy.py](server/testing/fault_proxy.py).

Never add a commercial ROM to `server/testing/roms.manifest` — it is fetched in
public CI. Homebrew and freely redistributable only.

## Working in parallel

At most **3 worktrees** run at once, and an issue is startable only when it
carries `ready` rather than `blocked` — those labels are maintained by
[`.github/workflows/unblock.yml`](.github/workflows/unblock.yml) from the
`Blocked by #N` lines in each issue body. Do not hand-edit them, and do not
start a `blocked` issue.

One exception the labels cannot express: **a foundation issue lands alone.**
When an issue defines an interface that later issues include — M0-2's
`HttpClient` is the standing example — it merges before anything that depends on
it starts, even if the labels say several things are ready. Three agents each
inventing their own version of a shared header is the one merge conflict worth
serialising to avoid.

## Plan before you edit

Start in plan mode and stay there until the plan is right. Commit it as
`plans/<issue-number>-<slug>.md` with four headings — **Files that change**,
**Order of work**, **Risks**, **Proof**. The bar is that an engineer who has never
seen the conversation could implement the change from it alone. See
[plans/README.md](plans/README.md).

When the implementation departs from the plan, **update the plan in the same
commit**. Departing is normal; departing silently is not, and the review checks
the diff against it.

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

## Finishing a task

1. `ctest --test-dir build --output-on-failure` is green, and your change has a
   test that would have failed before it. Run it and read the output before
   reporting anything complete — "it should pass" is not this. For a bug fix,
   write the failing test first and commit it before the fix.
2. **Run `/code-review` on your own branch** and put the findings in the PR body.
   This is required, not optional — it is what makes a human review tractable.
   [REVIEW.md](REVIEW.md) is the policy it follows: three passes, what counts as
   Important rather than a Nit, and what not to report at all.
3. Any issue your findings invalidated is edited, and the PR body says which and
   why.
4. Open a PR with `Closes #N` for your issue. A workflow uses that line to
   unblock dependent issues, so the wording matters.
5. A human merges. Do not merge your own PR.

## What is watching you

- **Skills** ([`.claude/skills/`](.claude/skills)) load when they become
  relevant: `save-safety` on anything that writes a save, `core-portability` on
  anything reaching for a platform facility inside `core/`, `tracker-is-spec` on
  anything that finds an issue to be wrong. They are advisory.
- **Hooks** ([`.claude/hooks/guard.py`](.claude/hooks/guard.py)) are not. They
  block, with an explanation: merging a PR, force-pushing `main`, writing to
  `server/contract/captures/`, editing secrets, editing `unblock.yml`, and
  editing the hooks and settings themselves — from a shell command as well as
  from an edit. A block is a rule you were about to break, not a bug. It is not
  a sandbox: `guard.py --selftest` is the record of what has been checked, not a
  proof that nothing gets through.
- **Subagents**: [`verifier`](.claude/agents/verifier.md) gives an independent
  build-and-test verdict from a fresh context before you open a PR;
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
| `scripts/orca/` | Per-worktree provisioning hooks. |
| `plans/` | One committed plan per issue, written before the code. |
| `evals/` | Regression tests for the agent configuration itself. |
| `.claude/` | Skills, subagents and hooks — what steers and what blocks. |
| `AGENTS.md` | Symlink to this file, for agent tools that look for that name. |
