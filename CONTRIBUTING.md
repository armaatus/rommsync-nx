# Contributing

## Where things get built

- `sysmodule/` and `overlay/` are **Switch homebrew** — built on a devkitPro dev
  machine or in CI (`devkitpro/devkita64` docker image). See
  [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). **Do not build these on the RomM
  server.**
- `server/` is the only part that runs on the RomM host (a snapshot + a Python
  probe script).

## Workflow

[docs/WORKFLOW.md](docs/WORKFLOW.md) is the full loop — how an idea becomes an
issue, an issue becomes a worktree, and a worktree becomes a merged PR, including
the parts that run without a human. The short form:

1. Pick an issue carrying `ready` (`good-first-issue` where labelled; the
   oldest open milestone first). Do not start a `blocked` one — those labels
   are derived, not hand-set.
2. Branch from `main`: `feature/M2-2-rom-matching`.
3. **Plan before you edit.** Files that change, order of work, risks, proof —
   it goes in the PR body under `## Plan`.
4. Keep changes scoped to the issue. Reference it in commits (`M2-2: ...`).
5. `ctest --test-dir build --output-on-failure` green, with a test that would
   have failed before your change.
6. Run `/code-review` on your own branch and put the findings in the PR body.
   [REVIEW.md](REVIEW.md) is the policy it follows.
7. CI must pass (warnings-as-errors). Open a PR whose body carries `Closes #N`,
   and let a human merge it.

Have an idea rather than a task? File it with the **Intent** issue template — it
does not have to be a spec yet.

## Order of work

The client exists: M0-M10 built the engine, the sysmodule and the overlay, and
what is open now is launch readiness -- the milestones still open on the
tracker, gated by the v1 gate (M8-1, `./scripts/v1-gate.sh`) before anything
touches a real console. The `ready`/`blocked` labels already encode the order
within them; follow those rather than a list here, which would go stale.

## Ground rules

- No secrets in the tree. `config.ini` and `token.dat` are git-ignored.
- Never overwrite a save without a backup (see
  [docs/SYNC_PROTOCOL.md](docs/SYNC_PROTOCOL.md)).
- Every network path: timeout, offline-safe, backoff. Never block boot.
- Verify any RomM schema against `server/contract/` before coding to it; if the
  server drifts, re-snapshot and update `docs/API_CONTRACT.md`.

## Commit attribution

See the repo's commit history for the established trailer format.

## Conduct & security

- By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).
- Found a vulnerability? Don't open a public issue — follow
  [SECURITY.md](SECURITY.md).
