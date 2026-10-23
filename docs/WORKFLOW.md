# How work happens here

This project is built by agents working in parallel, one per git worktree, with
one person deciding what the rules are and merging what lands. **This page is
the maintainer's**, read once and not per issue: the loop end to end — what each
stage produces, what starts the next, where a person is required, and why each
rule is the shape it is. An agent's own instructions are [CLAUDE.md](../CLAUDE.md)
and the issue it was handed; it comes here for the conventions behind a rule —
the `<!-- blockers -->` marker in [Stage 2](#stage-2--spec) chief among them —
and not for what to run.

The idea it turns on: **each stage ends by committing an artifact, and the next
stage begins by reading it.** The issue, the branch, the commit message and the
pull request are the audit trail — who asked for what, what the agent produced,
what approved it. No handoff meetings; only files.

---

## The loop

| Stage | Artifact | Starts the next stage | Who decides |
|---|---|---|---|
| 1. Intent | an `intent`-labelled issue | giving it Goal/Scope/Acceptance | anyone files |
| 2. Spec | the issue body, `ready` | a session dispatching it | the maintainer |
| 3. Build | a branch: the diff, its tests, a `## Plan` commit | the suite green | the agent, in its worktree |
| 4. Integrate | the integration branch `fleet/mN`, and one pull request | every branch merged, the suite green | the coordinating session |
| 5. Review and merge | the merged pull request | `unblock.yml` relabelling | the maintainer |
| 6. Maintain | a new issue, or an eval case | back to stage 1 | the maintainer |

### Stage 1 — Intent

An idea, a bug, a rough observation. File it with the **Intent** template
([`.github/ISSUE_TEMPLATE/intent.yml`](../.github/ISSUE_TEMPLATE/intent.yml)):
problem, proposed outcome, affected systems, constraints, open questions, in your
own words. An idea should not have to wait until someone has time to write a
proper issue.

### Stage 2 — Spec

An issue is workable when it carries the four sections every issue here carries —
#5 and #40 are the standard: **Goal**, **Scope** (including what is *not* in),
**Design notes** (the decisions already made, and why), **Acceptance** (each item
something a test or a command can demonstrate).

Below a `<!-- blockers -->` marker, `Blocked by #N` lines name its dependencies.
[`unblock.yml`](../.github/workflows/unblock.yml) derives `blocked`/`ready` from
those lines, and only a `ready` issue is dispatched. **Never hand-edit those
labels.** The lines are editable, but changing one changes what other agents may
start — do it deliberately, alone, and say so in the commit message.

It runs on a merged pull request, and on an issue `opened`, `edited`, `closed` or
`reopened` — not only on a merge. Two consequences worth holding on to, because
CLAUDE.md tells agents to edit issue bodies as they work:

- **An edit relabels the whole backlog**, not just the issue touched: every run
  recomputes every open issue. That is deliberate and idempotent, and the runs
  are serialised by a `concurrency` group on the job, newest wins. The stale
  label is removed *before* the new one is added, and the add is gated on the
  removal succeeding, so an issue ends with the single label it should have or
  the one it had — never both. Neither label is the fail-open state: nothing
  starts an issue that does not carry `ready`, and the next run finishes the job.
- **Only a line below the marker counts — when the body has one.** `Blocked by
  #N` has to begin a line, below the **first** `<!-- blockers -->` marker;
  everything under the earliest one is read, so pasting a second marker lower
  down adds to the section above it rather than superseding it. Above the
  marker, prose in Goal, Scope or Design notes blocks nothing — including a
  sentence like *"no longer blocked by #7"*, which used to register as a blocker
  (#47).

  **A body with no marker is read whole**, and that is the case to be careful
  in: any line that *begins* with `Blocked by #N`, or with a list bullet and then
  `Blocked by #N`, counts wherever it appears. **If you are adding blocker lines
  to an issue that has no marker, add the marker too** — that is what makes the
  rest of the body inert.

  Three boundaries either way. The anchor sees the start of a line and nothing
  before it, so a negation that *wrapped* onto the previous line still counts —
  keep a blocker line, and any sentence about one, on one line. The prefix accepts
  `-`, `*`, `+`, `>`, `1.`, `1)`, `- [ ]` and `**bold**`, so a bulleted mention is
  a blocker as surely as a bare one. And **the reader does not know what a fenced
  code block is** — a `<!-- blockers -->` line inside one is a marker like any
  other, and being first it wins. Indenting it does not help, and neither does
  splitting it across two lines; leaving anything else on the line does — write
  it inline in backticks the way this page does, or put a note after it.

One rule the labels cannot express: **a foundation issue lands alone.** An issue
that defines an interface later issues include (M0-2's `HttpClient` is the
standing example) merges before anything depending on it starts — two agents each
inventing their own shared header is the one merge conflict worth serialising to
avoid. *Alone* runs both ways (#215): nothing else is dispatched beside it.

And one the labels express badly: **an issue no agent can close.** Label it
**`needs-human-step`**. It means either:

- **The last step is a person's.** Tagging a release (#148), touching a real
  console (#44) — outward, irreversible, and the maintainer's call. `ready`
  cannot say this: every blocker really is closed. Where there is agent work in
  front of that step, it belongs in **its own issue with its own `Closes`
  line** — #142 prepared the release, #148 *is* the release.
- **No agent can start at all** — its scope is something the guard refuses, or
  something only the maintainer can reach.

Both mean the same thing to whoever dispatches: never give it a worktree. Which
one you have is in the issue's own **Scope**, not in the label.

### Stage 3 — Build, in the worktree

A Claude Code session — the coordinator — dispatches each `ready` ticket to an
Agent subagent in **its own git worktree**, branched from the milestone's
integration branch `fleet/mN`. Orca can do the same from a linked issue: it
creates the worktree, runs `orca.yaml`'s setup, and starts the agent with the
brief `scripts/orca/issue-command.sh <n>` prints. Either way the worktree is
provisioned before any test runs — [`scripts/orca/setup.sh`](../scripts/orca/setup.sh):
isolated ports into `.env`, submodules, seeded ROM fixtures, a full build, the
venv, its own RomM with the fixture provisioned inside it. Orca holds the
agent's tab until it finishes (`setupAgentStartupPolicy: wait-for-setup`); a
session making its own worktree runs it first. See
[Provisioning a worktree](#provisioning-a-worktree).

**There is no planning phase.** The agent's opening prompt is `/implement`, and
the issue is the plan: Goal, Scope, Design notes and Acceptance were settled at
Stage 2, on the tracker, which is where the deciding belongs. `/implement` is a
skill from the mattpocock plugin and ships `disable-model-invocation: true`, so
it is reachable only as the run's prompt, or by reading its file.

`## Plan` — **what the issue asked for, and where the implementation departed
from it and why**, plus the issues it edited — ends the branch's final commit
message, followed by `Closes #N` on a line of its own. Departing is normal;
departing silently is not, and the review checks that section against the diff.

Then it builds, with four things that are not negotiable:

- **There is no mock for the service under test.** Tests run against a real
  RomM in Docker, per worktree, on its own port. Failure modes a healthy server
  will not produce on demand — 401 mid-sync, a truncated body, a dropped
  connection, a stall — are forced with the fault proxy. See
  [TESTING.md](TESTING.md).
- **Every change carries a test that would have failed before it.** Not "the
  suite still passes". For a bug fix, write the failing test first, watch it fail
  for the reason you expect, commit it, and only then fix the code —
  `/mattpocock-skills:tdd` is that loop.
- **Verification is part of "done".** Run the full suite and read the output. If
  `rig.smoke` reports **Skipped**, the fixture is not running and most of the
  suite is meaningless.
- **When the integration branch moves under you, rebase onto it — never merge it
  in.** `scripts/release-notes.sh` builds the notes with `git log --no-merges`,
  so a merge commit at the head of a branch is a commit the notes cannot see:
  `release.notes` goes red saying the notes do not list the commit at HEAD.
  Whatever you resolve, diff the result against a tree you have actually run.

The agent does not merge, push to `main`, or open a pull request; its work ends
at the commit.

### Stage 4 — The integration branch, and one pull request

The coordinating session merges each finished branch into `fleet/mN`, reruns the
suite there, and opens **one** pull request for the milestone. Its body collects
each branch's `## Plan` and every `Closes #N` line — GitHub closes all of them
on the merge, and `unblock.yml` frees their dependants from them.

One pull request rather than one per ticket because the review is the scarce
resource, not CI: the maintainer reads one coherent diff per milestone instead
of a queue of small ones that each conflict with the next.

### Stage 5 — Review and merge

**The maintainer reviews and merges.** Nothing merges itself: `guard.py` refuses
`gh pr merge` from an agent, `--auto` included — with no gate among the required
checks, arming auto-merge would be merging on green.

[REVIEW.md](../REVIEW.md) is the standard, whoever reviews. Before the pull
request opens, the coordinator can review each branch from a fresh context —
`/code-review` and `/mattpocock-skills:code-review` (standards, and spec against
diff) — and fold the findings into a fix on that branch. The verdict on the pull
request is the maintainer's.

### Stage 6 — Maintain

Once the pull request merges, each worktree is torn down —
`./scripts/orca/archive.sh` and then `git worktree remove`, or Orca's own removal,
which runs the archive hook — and the next milestone's integration branch is cut
from `main`.

What comes back from the change re-enters at stage 1:

- A review finding that appears **twice** stops being a review finding: the
  correction goes into [CLAUDE.md](../CLAUDE.md) as part of that review.
  `/mattpocock-skills:writing-for-agents` is the skill for editing it.
- Anything that reached the default branch and had to be reverted earns a rule
  with an assertion behind it — a `--selftest` row, or a suite phase — written by
  whoever handled it. A rule with nothing running it is a rule that has already
  stopped holding.
- Anything the work invalidated in the tracker is edited as it is found —
  including issues that are not yours.

---

## Provisioning a worktree

Everything per-worktree lives in [`scripts/orca/`](../scripts/orca), and every
script there works the same whether Orca or a person runs it:

| Script | What |
|---|---|
| `setup.sh` | the whole provisioning, idempotent: Python check, `env.sh`, submodules, fixtures, build, venv, RomM, provision. `orca.yaml`'s `setup` |
| `env.sh` | derives the compose project `rmx-<slug>-<offset>` and three ports from the worktree's absolute path, into `.env` |
| `compose.sh` | `docker compose` against *this* worktree's stack |
| `archive.sh` | takes this worktree's stack and volumes down. `orca.yaml`'s `archive`; run it yourself before a plain `git worktree remove` |
| `reap.sh` | lists stacks whose worktree is gone; `--yes` removes them |
| `issue-command.sh` | the brief `orca.yaml`'s `issueCommand` starts an agent with |

```bash
git worktree add ../rommsync-nx-277 -b 277-thing fleet/m13
(cd ../rommsync-nx-277 && ./scripts/orca/setup.sh)
# ... work, commit ...
(cd ../rommsync-nx-277 && ./scripts/orca/archive.sh)
git worktree remove ../rommsync-nx-277
```

The stack name and ports are a pure function of the worktree path, so they are
stable across restarts and never collide; teardown re-derives them rather than
trusting `.env`. A worktree deleted with `rm -rf`, or removed while Docker was
down, leaves its stack behind — `./scripts/orca/reap.sh --yes` sweeps those. See
[TESTING.md](TESTING.md#worktree-isolation).

## Guardrails

Two layers, in increasing order of how hard they are to ignore.

**[CLAUDE.md](../CLAUDE.md)** — read in full at the start of every session, so
its size is paid on every task in every worktree. `evals/lint.sh` caps it at 200
lines.

**Hooks** ([`.claude/hooks/guard.py`](../.claude/hooks/guard.py)) —
deterministic. Four rules, each with a `--selftest` row:

| It blocks | Why |
|---|---|
| `gh pr merge` — `--auto` included — and its REST (`gh api …/pulls/N/merge`) and GraphQL (`mergePullRequest`) spellings | the maintainer merges; the agent that wrote it does not |
| force-pushing the default branch | the commit chain is the audit trail. The branch name comes from `.claude/hooks/guard.json` |
| editing `.env` and whatever `.claude/hooks/guard.json` names as a secret or a pinned path | hard rule 5, and the pinned RomM contract under `server/contract/captures/` |
| `gh pr review`, its REST (`gh api …/pulls/N/reviews`) and GraphQL (`addPullRequestReview`, `submitPullRequestReview`) spellings, and a `gh api` body read from a file, **in a linked worktree** | every pull request here goes up from the maintainer's account, so an agent's review is a review of its own work |

The review rule holds **only in a linked worktree** — where agents work — and
not in the main checkout, where the maintainer is the control and a guard that
argues with a person doing manual work is one people route around.

**What the hook is not: a sandbox.** It reads a command and decides; it does not
confine one. A session that means to get past it can — an interpreter one-liner
that opens a file, a path assembled from a variable. What it holds is the
*routine* line: the shapes an agent reaches for while solving the problem in
front of it rather than working around a rule. Past that, the backstops are the
diff and the maintainer's merge.

Three properties are deliberate:

- **It does not fail open.** An unreadable payload or an unparseable command
  blocks, and so does a `guard.json` that does not parse.
- **It tokenises with `shlex`**, splits compound commands on `;`, `&&`, `||` and
  `|`, recurses into `bash -c`, and skips heredoc *bodies* — so
  `true && gh pr merge 3` is caught while a document quoting that line is not.
- **`--selftest` counts what it ran.** It is the record of what has been checked
  — **not** a proof that nothing else gets through.

The guards do not protect themselves: `.claude/hooks/` is editable from any
worktree, because a guard nobody can improve is a guard that rots. The control
is the diff, which the maintainer merges.

## The lint

Every way the configuration breaks is quiet. A provisioning script that stops
parsing fails in the next worktree, during setup. A guard whose pattern stopped
matching stops blocking. Neither shows in a diff review.

[`./evals/lint.sh`](../evals/lint.sh) (`ctest -R agent.config`, and the
`agent config` workflow) checks the skills, subagents, hooks, `orca.yaml` and the
workflows, and runs [`./evals/run.sh`](../evals/run.sh): `bash -n` and
`shellcheck --severity=error` over every script, the three scanners for what
shellcheck does not know, and the `--selftest`s.

## Repository settings this depends on

The default branch is protected, with `host-tests`, `switch-build` and the lint
as required checks and no required approving review — the maintainer merges.
`merge-gate` belonged to the retired dispatcher and was deleted with it (#277);
if branch protection still lists it as required, take it off (a settings step),
or every pull request waits on a check that never reports.

## Working in parallel

At most **two** worktrees at once. The ceiling is not machine capacity — each
runs its own RomM and its own build — so much as how many streams one person can
review properly. Each is isolated by its own ports, its own compose project and
its own `.env`; only the immutable caches under `.cache/` are shared.

## When something stalls

| Symptom | Cause | Fix |
|---|---|---|
| `rig.smoke` **Skipped**, or tests hang or 401 as if the code broke | this worktree's RomM is not running — often a stack torn down under it | `docker ps`; then `./scripts/orca/setup.sh` again |
| setup stops on Python | the PATH Orca hands hooks has no Python >= 3.10 | install one; it names what it tried |
| `gh pr merge` refused | the maintainer merges | nothing — stop at the commit |
| `gh pr review` refused | an agent's worktree does not review | report findings in your output |
| stacks with no worktree in `docker ps` | a worktree removed without `archive.sh` | `./scripts/orca/reap.sh --yes` |
| an issue you expected is not `ready` | a `Blocked by #N` line names an open issue, or the body has no marker and a line elsewhere reads as one | read the body; see [Stage 2](#stage-2--spec) |
