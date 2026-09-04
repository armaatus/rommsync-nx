# rommsync-nx — working agreement

On-device RomM sync for a modded Nintendo Switch: a background **sysmodule**
(`sys-rommsync`) plus an Ultrahand/Tesla **overlay** (`ovl-rommsync`). Read
[README.md](README.md) for the product, [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
for the shape.

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

./scripts/orca/env.sh                            # regenerate .env
./scripts/orca/romm-browser.sh                   # open RomM signed in, if the tab is gone
./scripts/orca/romm-logs.sh                      # follow RomM, if the tab is missing
./server/testing/seed.sh                         # re-seed ROM fixtures
./.venv/bin/python server/testing/provision.py   # scan the library, mint a fixture token
./scripts/orca/compose.sh up -d                  # start RomM
./scripts/orca/compose.sh logs -f                # follow it; tab 2 shows this
./scripts/orca/reap.sh                           # list RomM stacks whose worktree is gone (--yes removes)
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

## Code

- C++20. `-Wall -Wextra -Wpedantic -Werror` — warnings are errors, including in
  your branch.
- `core/` sources are globbed, so adding a `.cpp` needs no CMake edit.
- Every network call: timeout, offline-safe, retry with backoff. Never block boot.
- Match the surrounding code's naming and comment density. Comments explain
  *why*, not *what*.

## Finishing a task

1. `ctest --test-dir build --output-on-failure` is green, and your change has a
   test that would have failed before it.
2. **Run `/code-review` on your own branch** and put the findings in the PR body.
   This is required, not optional — it is what makes a human review tractable.
3. Open a PR with `Closes #N` for your issue. A workflow uses that line to
   unblock dependent issues, so the wording matters.
4. A human merges. Do not merge your own PR.

## Layout

| Path | What |
|---|---|
| `core/` | Portable engine — auth, sync, downloads, config, state. Host-testable. |
| `host/` | Desktop backends for `core/`'s interfaces (libcurl `HttpClient`). Never built for Switch. |
| `sysmodule/` | `sys-rommsync`, the background engine. devkitPro Makefile. |
| `overlay/` | `ovl-rommsync`, the libultrahand overlay. devkitPro Makefile. |
| `server/` | Pinned RomM API snapshot, contract probe, and the Docker test fixture. |
| `tests/` | CTest suites. |
| `scripts/orca/` | Per-worktree provisioning hooks. |
