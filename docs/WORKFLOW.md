# How work happens here

This project is built by agents working in parallel, one per Orca worktree, with
one human deciding what the rules are. This file is that loop end to end: what
each stage produces, what starts the next, where a person is required, and how to
stop the whole thing.

New here? Read [CLAUDE.md](../CLAUDE.md) first — it is the working agreement and
it is short. This file is the longer explanation behind it.

The shape is adapted from Anthropic's
[AI-native SDLC playbook](https://claude.com/blog/the-ai-native-sdlc-playbook).
The idea it turns on: **each stage ends by committing an artifact, and the next
stage begins by reading it.** The chain of commits is the audit trail — who asked
for what, what the agent produced, what approved it. No handoff meetings; only
files.

Three things shape every decision below.

**The machine is free.** This Mac has effectively unlimited time and the fixtures
already on disk, so the heavy work — building, the full suite against a real
RomM, both review passes — happens locally. GitHub Actions is the independent
second opinion and the gate, and at roughly 5.5 minutes per PR it is not the
scarce resource.

**A PR arrives reviewed, or it does not arrive.** By the time a pull request
exists it has been through `/code-review` and `/mattpocock-skills:code-review`
locally and the findings are in its body. The push is gated on a local marker
and the merge is gated on those findings being in the body — see
[the merge gate](#the-merge-gate).

**Nothing merges on trust.** The agent never merges and never approves. It asks
GitHub to merge, and one required check — [`merge-gate`](#the-merge-gate) —
decides whether that is allowed.

---

## Start it

Run the dispatcher in an Orca terminal, so it is as visible as the work it
starts:

```bash
orca terminal create --worktree active --title fleet \
  --command "./scripts/orca/fleet.sh run --auto"
```

Or directly:

```bash
./scripts/orca/fleet.sh run 11 12 13              # work exactly these issues
./scripts/orca/fleet.sh run --auto                # keep taking `ready` issues
./scripts/orca/fleet.sh run --auto --until 08:00  # ...and stop then
./scripts/orca/fleet.sh run --auto --for 6h --max-prs 5
./scripts/orca/fleet.sh status                    # what is running, what is next
./scripts/orca/stop.sh                            # stop. See below — this always works.
```

`fleet.sh` is deterministic shell. No model runs in it. Its whole job is to decide
*which* issue gets a worktree and *when*.

**What it picks:** anything `ready` and not already in flight, ordered by how many
open issues name it in a `Blocked by #N` line. The work that frees the most other
work goes first, which is the fastest way to turn a mostly-blocked backlog into a
wide one. Milestones do not order it — `ready` already means every blocker is
closed, and a milestone number is not a claim about what can be built *now*.
`--auto` never pauses; it stops when the queue empties, at `--until`/`--for`, or
after `--max-prs`, and says which.

`ready` on its own is not enough, incidentally: the label stays until the PR
merges, so `fleet.sh` also excludes any issue that already has an open PR
carrying `Closes #N`.

**What it will not do:** it does not merge, and it never touches a worktree it did
not create.

## Stop it

```bash
./scripts/orca/stop.sh          # drain: no new worktrees, running agents finish
./scripts/orca/stop.sh --now    # ...and interrupt the fleet's agents too
./scripts/orca/stop.sh --all    # ...and every other agent Orca knows about
./scripts/orca/fleet.sh resume  # carry on
```

The stop is a **file** — `~/.rommsync-fleet/STOP` — not a signal, and it lives
outside every worktree. That is deliberate: a signal only reaches a process that
is still healthy, and the moment you most need a stop is the one where something
is not.

Three things read it, so it holds even when nothing cooperates:

- `fleet.sh` checks it before every decision and opens nothing new.
- `await-review.sh` checks it between polls and returns exit 3.
- **`guard.py` refuses `git push`, `gh pr create`, every `gh … comment/edit`
  and any `gh api` with a write method while it exists.** Reads stay open, so a
  stopped agent can still find out what it was in the middle of. So a stopped fleet produces no outward effects even from an
  agent that is mid-thought and has not read the news. Reading, building and
  testing stay open — the point is to stop work reaching anyone, not to freeze
  the machine.

Nothing removes that file except `fleet.sh resume`.

---

## The loop

| Stage | Artifact | Starts the next stage | Who decides |
|---|---|---|---|
| 1. Intent | an `intent`-labelled issue | giving it Goal/Scope/Acceptance | anyone files |
| 2. Spec | the issue body, `ready` | `fleet.sh` opening a worktree | the maintainer |
| 3. Build | the diff and its tests | `ctest` green | enforced by the hook |
| 4. Local review | `.orca/reviewed-<sha>` + the PR body | the push gate lifting | enforced by the hook |
| 5. Independent review | a GitHub review | `merge-gate` going green | the rules you set |
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
those lines on every merge, and `fleet.sh` reads the same lines to order its
queue. **Never hand-edit those labels.** The lines are editable, but changing one
changes what other agents may start — do it deliberately, alone, and say so in
the PR body.

One rule the labels cannot express: **a foundation issue lands alone.** An issue
that defines an interface later issues include (M0-2's `HttpClient` is the
standing example) merges before anything depending on it starts. Label it
`foundation` and `fleet.sh` holds the fan-out for it.

### Stage 3 — Build, in the worktree

`fleet.sh` creates the worktree with the issue linked and the brief already sent;
`orca.yaml`'s setup hook provisions it — isolated ports, seeded ROM fixtures, a
full build, its own RomM, a scanned library, a browser tab signed in as the
fixture admin. `setupAgentStartupPolicy: wait-for-setup` holds the agent's tab
until that finishes, so its first `ctest` means something.

The agent plans before it edits — **Files that change / Order of work / Risks /
Proof**, in the PR body under `## Plan`, at the bar that someone who never saw the
conversation could implement it from the plan alone. Departing from a plan is
normal; departing silently is not.

Then it builds, with three things that are not negotiable:

- **There is no mock RomM.** Tests run against a real RomM 5.2.0 in Docker, per
  worktree, on its own port. Failure modes a healthy server will not produce on
  demand — 401 mid-sync, a truncated body, a dropped connection, a stall — are
  forced with the fault proxy. See [TESTING.md](TESTING.md).
- **Every change carries a test that would have failed before it.** Not "the
  suite still passes". For a bug fix, write the failing test first, watch it fail
  for the reason you expect, commit it, and only then fix the code —
  `/mattpocock-skills:tdd` is that loop.
- **Verification is part of "done".** Run `ctest` and read the output. If
  `rig.smoke` reports **Skipped**, RomM is not running and most of the suite is
  meaningless.
- **When `main` moves under you, rebase onto it — never merge it in.**
  `scripts/release-notes.sh` builds the notes with `git log --no-merges`, because
  a squash-merge repo has no merge commits worth listing, so a merge commit at
  the head of a branch is a commit the notes cannot see: `release.notes` goes
  red saying the notes do not list the commit at HEAD, and the branch's own work
  is missing from them. The failure names the symptom and not the cause, which
  is why it is written here. Rebasing costs resolving the same region once per
  commit; do that. Whatever you resolve, diff the result against a tree you have
  actually run — `git add -A` on a round where a second file was also conflicted
  is how conflict markers reach a commit that still builds.

The [`verifier`](../.claude/agents/verifier.md) subagent is the packaged final
check: a fresh context that builds, runs the suite, hunts for the test that would
have failed, and answers `READY` or `NOT READY`. It fixes nothing, which is why
its verdict is worth having.

An issue gets **three hours**. On expiry the fleet interrupts the agent, comments
on the issue saying so, and **leaves the worktree standing** — a stuck task is
exactly the one worth looking at, and its fixture and build state are the
evidence. You get a notification.

### Stage 4 — Local review, before anything leaves

Two passes, because they look for different things and this machine has the time:

```bash
/code-review high                  # defects: correctness, efficiency, reuse
/mattpocock-skills:code-review     # conformance: standards, and spec-vs-diff
```

[REVIEW.md](../REVIEW.md) is the policy both follow. Fix what is real, re-run the
tests, then record it:

```bash
./scripts/orca/record-review.sh findings.md
```

That writes `.orca/reviewed-<sha>`, and **`guard.py` refuses `git push` and
`gh pr create` from a fleet-owned worktree without it.** The marker records what
it is given; it cannot tell whether a review really happened, so it is a
checklist gate. What actually enforces the two passes is `merge-gate`, which
reads the PR body — and that a human can read too. The marker is
per-commit, so amending or adding a commit needs the review re-run — which is the
point. A worktree you opened by hand is never gated: pushing a half-finished
branch is normal, and a guard that argues about it is a guard people route
around.

The PR body carries `## Plan`, both sets of findings and what was done about
them, any issue that was edited and why, and `Closes #N`. That body is not
decoration — `merge-gate` reads it.

### Stage 5 — Independent review, and the merge

On GitHub:

- [`ci.yml`](../.github/workflows/ci.yml) — host tests against a real RomM, three
  Switch targets, `core/` include hygiene. ~5.5 minutes.
- [`agent-config.yml`](../.github/workflows/agent-config.yml) — regression-tests
  the agent configuration whenever it changes.
- [`claude-review.yml`](../.github/workflows/claude-review.yml) — the independent
  review, from a context that has not seen the conversation which produced the
  diff. It submits a **real GitHub review**: `REQUEST_CHANGES` when it has an
  Important finding, `COMMENT` when it does not, never `APPROVE`.
- [`merge-gate.yml`](../.github/workflows/merge-gate.yml) — the required check
  that decides whether the PR may merge itself.

Back in the worktree the agent waits with one blocking call:

```bash
./scripts/orca/await-review.sh
```

This is the cheap half of the loop. An agent that waits by *thinking about
whether the review has arrived* burns tokens the whole time. An agent that waits
inside one tool call burns nothing — the session is suspended until the script
returns. No webhook, no ingress, no daemon; `gh` on a 30-second poll.

Then it fixes what is real, replies with a reason where it disagrees, resolves
every thread, pushes, re-requests review, and checks:

```bash
./scripts/orca/review-status.sh    # exit 0 = every thread resolved, every check green
```

Resolution comes from GitHub's own state through GraphQL, not from whether a
reply exists — the REST endpoint for PR comments cannot report it, and
`isOutdated` is not `isResolved`.

**At most three rounds, counted by the script.** `await-review.sh` keeps the
count in `.orca/review-rounds` and exits 5 on the fourth call rather than
waiting, so this is not something an agent has to remember. When it trips, the
agent stops, comments saying exactly what is unresolved and why it disagrees, and flags
the card. Another lap is not what a disagreement needs; your attention is.

When it is green the agent runs `gh pr merge --auto --squash`. That does **not**
merge — it asks GitHub to merge once the required checks pass. Then it stops.

### The merge gate

[`merge_gate.py`](../.github/scripts/merge_gate.py) is the required check
`--auto` waits on. It passes only when all five hold:

1. the PR body shows a local `/code-review` pass;
2. …and a local `/mattpocock-skills:code-review` pass;
3. an independent review exists on the **current head SHA** — pushing a fix
   invalidates it, so a re-review is required;
4. the **latest** review from each author is not `CHANGES_REQUESTED`;
5. no review thread is unresolved.

Point 4 is the whole trick. GitHub's `reviewDecision` is sticky: once a reviewer
requests changes it stays `CHANGES_REQUESTED` until dismissed or until that
reviewer *approves* — and this reviewer never approves, by design. Reading it
would leave a PR whose findings were all addressed blocked forever. Taking the
latest review per author instead lets a clean re-review supersede the old verdict
on its own, with no dismissal step and nothing waiting on a person.

A PR touching **`.claude/**`**, **`.github/workflows/**`** or
**`.github/scripts/**`** fails the gate on purpose. Those are the paths that can
disable the checks gating their own PR — the last one because `merge_gate.py`
*is* the gate, and a change to what "may merge" means must not merge itself on
the strength of its own new rules. `enforce_admins` is off, so you merge them by
hand with the check red — which is exactly the intended shape.

The decision lives in a script rather than in the YAML so it can be run and
tested without a pull request: `python3 .github/scripts/merge_gate.py --selftest`,
and `evals/lint.sh` runs it.

Say `@claude …` on a PR or a review comment and the mention job picks it up,
makes the change and pushes — gated to `OWNER`, `MEMBER` and `COLLABORATOR`,
because that is the job that can write.

### Stage 6 — Maintain

Once the PR merges, `fleet.sh` marks the card `completed`, comments which PR
landed, and removes the worktree with `--run-hooks` — which is not optional:
without it `orca.yaml`'s archive hook never runs and that worktree's RomM stack
survives under `restart: unless-stopped`, holding two ports forever with nothing
left on disk to identify it by. Then the next `ready` issue takes the slot.

And what comes back from the change re-enters at stage 1:

- A review finding that appears **twice** stops being a review finding: the
  correction goes into [CLAUDE.md](../CLAUDE.md) or a skill as part of that
  review. `/mattpocock-skills:writing-for-agents` is the skill for editing those.
- Anything that reached `main` and had to be reverted earns an eval case in
  [`evals/cases/`](../evals/cases), written by whoever handled it.
- Anything the work invalidated in the tracker is edited as it is found —
  including issues that are not yours.

---

## Where to look

Everything runs through Orca, so the board is the status surface. `fleet.sh`
drives it: **`in-progress`** while a worktree builds, **`in-review`** once the
agent has opened its PR, **`completed`** on merge, and a one-line comment on each
card saying what it is waiting for.

You get a macOS notification for the two cases you would otherwise miss: the
fleet stopping, and an issue giving up on its time-box. Everything else is
visible without being interrupted.

## Guardrails

Three layers, in increasing order of how hard they are to ignore.

**`CLAUDE.md`** — read in full at the start of every session, so its size is paid
on every task in every worktree. `evals/lint.sh` fails if it grows past 200 lines.

**Skills** — loaded when they become relevant rather than read every time.
Repo-owned ones live in [`.claude/skills/`](../.claude/skills): `save-safety` on
anything that writes a save, `core-portability` on anything reaching for a
platform facility inside `core/`, `tracker-is-spec` on anything that finds an
issue to be wrong. The `mattpocock-skills` plugin is enabled in the **committed**
`.claude/settings.json`, so every worktree has it — `code-review` (standards and
spec-vs-diff), `tdd`, `diagnosing-bugs`, `writing-for-agents`, `research`,
`grilling` for stress-testing a plan before you commit to it. `evals/lint.sh`
fails if that entry disappears, because the agent brief names those skills.

**Hooks** ([`.claude/hooks/guard.py`](../.claude/hooks/guard.py)) — deterministic.

| It blocks | Why |
|---|---|
| `gh pr merge`, and the `gh api …/merge` spelling | separation of duties: the agent that wrote it does not merge it |
| `gh pr merge --auto` is **allowed** | that asks GitHub to merge once `merge-gate` passes — a rule decides, not the agent |
| force-pushing `main` | the commit chain is the audit trail |
| writing to `server/contract/captures/` | rewriting a capture silences the only test that notices RomM changing |
| editing secrets, `.env`, `token.dat`, `device.dat` | hard rule 5 |
| editing `unblock.yml` | it decides what other worktrees may start |
| editing `.claude/hooks/` and `settings.json` **in a fleet worktree** | an agent rewriting its own guards while nobody is watching has none |
| `gh api` with `-X POST/PUT/PATCH/DELETE` while stopped | a write is outward; a read is not |
| pushing or opening a PR from a fleet worktree with no `.orca/reviewed-<sha>` | a PR arrives reviewed or it does not arrive |
| anything outward while `~/.rommsync-fleet/STOP` exists | a stop that depends on cooperation is not a stop |

The last three apply **only in a worktree the fleet opened**. In your own
worktree you are the control, and a guard that argues with a person doing manual
work is a guard people route around. It is also why the guards can still be
improved: the first version protected itself everywhere, and made its own bug
unfixable.

**What the hook is not: a sandbox.** It reads a command and decides; it does not
confine one. A session that means to get past it can — an interpreter one-liner
that opens a file, a path assembled from a variable. What it holds is the
*routine* line: the heredoc, the redirect, the `sed -i`, the `gh pr merge`, the
shapes an agent reaches for while solving the problem in front of it rather than
working around a rule. Past that, the backstops are the diff and `merge-gate`.
Do not write documentation — or a commit message — that claims more.

Four properties are deliberate:

- **It does not fail open.** An unreadable payload or an unparseable command
  blocks. A guard that quietly stops guarding when something upstream changes
  shape is worse than no guard, because nothing says the enforcement went away.
- **It tokenises with `shlex`**, splits compound commands on `;`, `&&`, `||` and
  `|`, recurses into `bash -c`, and skips heredoc *bodies* — so
  `true && rm .env` is caught while a document quoting that line is not.
- **A write is a write whichever verb performs it.** Redirects, `tee`, `cp`/`mv`
  destinations, `sed -i`, `rm`, `dd of=` all go through the same rules an `Edit`
  does. The first version checked paths only for the editing tools, so
  `cat >` into a guarded file rewrote it and the guard said nothing.
- **Skills and subagents are never protected.** They are advisory by design, and
  an agent improving one is the loop working.

`guard.py --selftest` is 68 assertions kept next to the code they constrain,
and it counts what it ran rather than asserting a number kept in step by hand. Every
row is either a rule this repo depends on or an escape somebody actually found.
It is the record of what has been checked — **not** a proof that nothing else
gets through.

Agents run in **auto** permission mode (`permissions.defaultMode`), which is only
safe because the above decides what they may do rather than a prompt for each
command.

## Repository settings this depends on

`main` is protected. Required checks: `static`, `host-tests`, `switch-build`,
`configuration is well-formed`, `merge-gate`. Conversation resolution required.
**No required approving review** — that would make you the bottleneck on exactly
the PRs that already did the work. `enforce_admins: false`, so you can always
merge the enforcement-layer PRs that `merge-gate` deliberately fails.

Auto-merge is enabled at the repository level; without it `gh pr merge --auto`
errors out.

## The configuration is tested

Every way `.claude/` breaks is silent. A skill whose frontmatter does not parse
never loads. A hook whose path is wrong never runs. A guard whose pattern stopped
matching stops blocking. None of it shows in a diff review or turns a build red.

- **`./evals/lint.sh`** — deterministic, free, no model. Also `ctest -R
  agent.config`, so a worktree sees a break before CI does.
- **`./evals/run.sh`** — one headless session per case in `evals/cases/`, scoring
  what the agent actually answers. Needs `CLAUDE_CODE_OAUTH_TOKEN`.

The eval job runs on **push to `main`**, never on a pull request. What it
evaluates is the instructions an agent loads, and it evaluates them by handing
them to an agent holding the token — on a PR trigger those files are whatever the
branch says they are, and a settings hook is plain command execution. A PR gets
the token-less lint.

## Working in parallel

At most **three worktrees**. The ceiling is not machine capacity; it is how many
streams one person can review properly. Each is fully isolated — its own ports,
compose project, RomM database and `build/`. Only the immutable, expensive things
are shared (`.cache/roms`, `.cache/ccache`), so no agent can corrupt another's
fixtures.

Removing a worktree from the **Orca UI** runs the teardown hook. `orca worktree
rm` does **not** unless you pass `--run-hooks` — which is why `fleet.sh` always
does. Sweep anything left behind with `./scripts/orca/reap.sh --yes`.

## When the loop stalls

| Symptom | Cause | Fix |
|---|---|---|
| `fleet.sh run` opens nothing | the stop file is set | `./scripts/orca/fleet.sh resume` |
| `fleet.sh status` shows no next issue | everything `ready` is already in flight | merge something, or file work |
| Worktree provisioned, agent idle, nothing in the composer | Orca drafts the issue prompt instead of sending it | `./scripts/orca/agent-autostart.sh` — `setup.sh` starts the `--watch` form |
| Every hook says "this worktree has no linked issue" | the `orca` CLI on `PATH` cannot find `Orca.app` | nothing — the hooks probe it and fall back. If it persists: `sudo chmod -h 755 /usr/local/bin/orca` |
| `git push` refused, "nothing leaves one of those unreviewed" | the local review is not recorded for this commit | run both passes, then `./scripts/orca/record-review.sh` |
| `await-review.sh` times out | the review job never ran | `gh run list`; check `CLAUDE_CODE_OAUTH_TOKEN` is a repo secret |
| `merge-gate` red on a PR that looks fine | usually the body is missing a review section, or the review predates the last push | read the check's output; it says which of the five |
| A PR sits queued and never merges | a required check never reported | `gh pr checks <n>` |
| `ctest` reports `rig.smoke` **Skipped** | RomM is not running for this worktree | `./scripts/orca/compose.sh up -d` |
