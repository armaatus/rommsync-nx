# Review policy

What `/code-review` looks for on this repo, what counts as **Important** rather
than a **Nit**, and what it should not report at all.

CLAUDE.md makes `/code-review` on your own branch a required step before opening
a PR, with the findings in the PR body. This file is what makes those findings
comparable between agents and between PRs: without it, three worktrees produce
three different reviews of three different things, and a human cannot tell a
serious finding from a preference.

Read this before reviewing. If you are the author, read it before you finish --
a finding you can predict is one you can avoid.

## The passes

Run all three. Report them separately.

### 1. Correctness

Bugs, logic errors, and the failure paths this project cares about most:

- **The save guarantee.** Any path that overwrites a save file backs it up first
  and writes atomically. An interruption anywhere in the sequence leaves either
  the old file or the new one. A failed backup aborts the write. This is hard
  rule 2, and a breach is always Important.
- **Network calls.** Every one has a timeout, is safe offline, retries with
  backoff, and never blocks boot. A call missing a timeout is Important.
- **Conflict resolution.** The server is the source of truth. Local-wins
  behaviour that is not explicitly specified in the issue is a bug.
- **Partial state.** What is left on disk when the process dies here? A
  half-written state.db, a `.tmp` beside a save, or a token file with no
  matching device record is Important.
- **Integer and buffer handling** in anything that parses a server response.

### 2. Portability and platform rules

- Nothing in `core/` includes a host-only or libnx header (hard rule 4). CI
  catches the mechanical form; report the ones it cannot -- a platform
  assumption smuggled in as a type, a path separator, an endianness assumption,
  a `long` that is not the same width on aarch64.
- Nothing new is on the boot path.
- No real hardware and no production RomM is touched (hard rule 1). A test or
  script that reaches a non-loopback address is Important -- `policy.loopback_only`
  exists for this and a finding here means it was worked around.
- No secrets in the tree (hard rule 5).

### 3. Compliance with the spec

- Against the issue: does the diff do what **Scope** asked, and does it satisfy
  **Acceptance**? Name anything in the diff that is outside Scope, and anything
  in Acceptance the diff does not cover.
- Against `plans/<issue>-<slug>.md`: where the implementation departed from the
  committed plan, is the plan updated in the same PR? An undocumented departure
  is Important; a documented one is fine.
- Is there a test that would have failed before this change? Name it. Its
  absence is Important regardless of how green the suite is.
- Did the work invalidate an issue -- any issue -- that has not been edited? That
  is Important: those bodies are the only channel between parallel worktrees.

## Important vs Nit

**Important** is reserved for a finding that would break behaviour, destroy or
corrupt a save, leak a secret, breach a hard rule, break the build on either
target, or leave the tracker saying something untrue.

Everything else is a **Nit**: naming, comment wording, ordering, a clearer
formulation of something already correct.

Report at most **five nits**, and summarise the rest as a count. A review whose
signal is buried in twenty preferences costs more attention than it saves.

## Do not report

- Anything CI already enforces: compiler warnings, `core/` include hygiene,
  shell scripts that do not parse, an unformatted Python file, artifact shape.
  CI going red says it better and does not need a human to read it.
- Comment density or naming that matches the surrounding code. CLAUDE.md asks
  for consistency with what is there, not for a house style this file does not
  define.
- Generated or vendored trees: `build/`, `.venv/`, `server/testing/library/`,
  `.cache/`.
- `server/contract/captures/` content. It is a recorded snapshot of a real
  server; it is not written by hand and reviewing its style is meaningless.
  A *change* to it, on the other hand, is Important and belongs in pass 3.

## What findings do and do not do

Findings inform the human who merges. They do not approve and they do not block:
a PR still needs a human, and no agent merges its own work (CLAUDE.md,
"Finishing a task").

When a review flags the same mistake twice across PRs, the correction goes into
CLAUDE.md as part of that review. That is how this stops being a review finding
and starts being something the next session already knows.
