# Review policy

What a review of a change here looks for, which findings **block a merge** and
which do not, and what it should not report at all.

Work reaches `main` as one pull request per milestone: each ticket's branch is
merged into the integration branch (`fleet/mN`), and the maintainer reviews and
merges that. Before it opens, each branch can be reviewed from a context that
has not seen the conversation which produced it — `/code-review` and
`/mattpocock-skills:code-review` — because an author reviewing its own work
shares its blind spots. Whoever reviews, this is the standard.

Read this file before reviewing. If you are the author, read it before you
finish — a finding you can predict is one you can avoid.

## The dimensions

Evaluate all seven. Report only what you found; a dimension with nothing in it
gets one line or none.

1. **Architecture and design** — separation of concerns, modularity, coupling,
   whether abstractions are at a consistent level, whether the design holds at
   the size this will actually reach.
2. **Code quality and maintainability** — readability by someone with no AI
   assistance and no context, naming, unnecessary complexity, duplication that
   should be extracted, error handling, and whether comments explain *why*.
3. **Impact and breaking changes** — every usage of a changed interface found
   and updated, backward compatibility, migrations that are safe and reversible,
   consumers of a changed API accounted for, downstream effects considered.
4. **Testing** — critical paths and edge cases covered, tests meaningful and
   independent, failure cases tested. **Is there a test that would have failed
   before this change?** Name it. Its absence is Important regardless of how
   green the suite is.
5. **Performance** — algorithmic complexity at the expected data size, query
   patterns and N+1s, resource usage, whether long-running work blocks something
   it should not.
6. **Security** — input validation, authentication and authorisation, exposure
   of sensitive data, injection, and dependencies.
7. **Project standards and spec** — the hard rules in `CLAUDE.md`; the issue's
   **Scope** and **Acceptance**, naming anything in the diff outside Scope and
   anything in Acceptance the diff does not cover; the `## Plan` section, where
   an undocumented departure from it is Important and a documented one is fine;
   and whether the work invalidated an issue — any issue — that has not been
   edited, which is Important because those bodies are the only channel between
   parallel worktrees.

### ...and this project's own

The failure paths this codebase cares about most.

**Correctness**

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

**Portability and platform rules**

- Nothing in `core/` includes a host-only or libnx header (hard rule 4). CI
  catches the mechanical form; report the ones it cannot — a platform
  assumption smuggled in as a type, a path separator, an endianness assumption,
  a `long` that is not the same width on aarch64.
- Nothing new is on the boot path.
- No real hardware and no production RomM is touched (hard rule 1). A test or
  script that reaches a non-loopback address is Important —
  `policy.loopback_only` exists for this and a finding here means it was worked
  around.
- No secrets in the tree (hard rule 5): `config.ini`, `token.dat`, `device.dat`,
  `.env`, `server/testing/fixture-auth.env`.

**Compliance with the spec**

- The Switch targets are built by CI (`switch-build`), not by `ctest`; a change
  under `sysmodule/`, `overlay/` or `tlsprobe/` whose commit message does not say
  it was built with devkitPro is a finding.

## Critical, Important, Suggestion

**Critical** — security, data loss or corruption, a breaking change with no
migration, or a production failure. **A Critical finding is fixed, never argued
away.** There is no second reviewer behind this one to take the argument to.

**Important** — a real defect or a breach of a hard rule: wrong behaviour on a
path that matters, a missing test that would have caught the bug, a broken build
on either target, or the tracker left saying something untrue. Fixed, or
disputed in the commit message with a reason the re-review accepts.

**Suggestion** — naming, comment wording, ordering, a clearer formulation of
something already correct. **A Suggestion blocks nothing** and nobody has to
answer it, so labelling one thing as another is the single mistake here that
moves a merge. The asymmetry is deliberate: a nit that held a branch cost a
whole loop to change a comment.

**A Suggestion never becomes its own issue.** One that does costs a whole loop —
brief, implementation, review, fix — to change a comment, and a review that
mints work every round is the most expensive thing in this system. Anything
worth keeping goes on the repository's standing nit issue, if it keeps one.

Report at most **five Suggestions**, and summarise the rest as a count. A review
whose signal is buried in twenty preferences costs more attention than it saves.

## What not to report

- **The comment density**, where a project's own rules ask for it. A diff that
  matches the code around it is correct.
- **A preference restated as a defect.** If the existing code is correct and you
  would have written it differently, that is a Suggestion at most, and it counts
  against the cap of five.
- **Anything you cannot cite.** A behaviour claim needs a `file:line` in the
  actual source, not an inference from a name. If you are unsure a finding is
  real, drop it or say you are unsure — a wrong Important finding spends a fix
  on nothing.
- **Anything CI already enforces**: compiler warnings, `core/` include hygiene,
  shell scripts that do not parse, an unformatted Python file, artifact shape.
- **Generated or vendored trees**: `build/`, `.venv/`, `server/testing/library/`,
  `.cache/`, `overlay/lib/`.
- **`server/contract/captures/` content.** It is a recorded snapshot of a real
  server and is not written by hand. A *change* to it, on the other hand, is
  Important.
