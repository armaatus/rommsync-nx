# rommsync-nx's own review rules

What REVIEW.md cannot know about this project. Read after it; the passes and the
Important/Nit bar are REVIEW.md's, these are the failure paths this codebase
cares about most.

## Correctness

- **The save guarantee.** Any path that overwrites a save file backs it up first
  and writes atomically. An interruption anywhere in the sequence leaves either
  the old file or the new one. A failed backup aborts the write. This is
  CLAUDE.md hard rule 2, and a breach is always Important.
- **Network calls.** Every one has a timeout, is safe offline, retries with
  backoff, and never blocks boot. A call missing a timeout is Important.
- **Conflict resolution.** The server is the source of truth. Local-wins
  behaviour that is not explicitly specified in the issue is a bug.
- **Partial state.** What is left on disk when the process dies here? A
  half-written state.db, a `.tmp` beside a save, or a token file with no
  matching device record is Important.
- **Integer and buffer handling** in anything that parses a server response.

## Portability and platform rules

- Nothing in `core/` includes a host-only or libnx header (hard rule 4). CI
  catches the mechanical form; report the ones it cannot -- a platform
  assumption smuggled in as a type, a path separator, an endianness assumption,
  a `long` that is not the same width on aarch64.
- Nothing new is on the boot path.
- No real hardware and no production RomM is touched (hard rule 1). A test or
  script that reaches a non-loopback address is Important --
  `policy.loopback_only` exists for this and a finding here means it was worked
  around.
- No secrets in the tree (hard rule 5): `config.ini`, `token.dat`, `device.dat`,
  `.env`, `server/testing/fixture-auth.env`.

## Compliance with the spec

- A test that would have failed before the change, named. Its absence is
  Important regardless of how green the suite is.
- The Switch targets are built by CI (`switch-build`), not by `ctest`; a change
  under `sysmodule/`, `overlay/` or `tlsprobe/` that the PR body does not say
  was built with devkitPro is a finding.
- Did the work invalidate an issue -- any issue -- that has not been edited?
  Those bodies are the only channel between parallel worktrees.

## Do not report

- Anything CI already enforces: compiler warnings, `core/` include hygiene,
  shell scripts that do not parse, an unformatted Python file, artifact shape.
- Comment density or naming that matches the surrounding code.
- Generated or vendored trees: `build/`, `.venv/`, `server/testing/library/`,
  `.cache/`.
- `server/contract/captures/` content. It is a recorded snapshot of a real
  server and is not written by hand. A *change* to it, on the other hand, is
  Important and belongs in the spec pass.
