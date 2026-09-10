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
closing it. It reads that line the way GitHub does — any of the nine closing
keywords, in any case — from
[`.github/scripts/issue_refs.py`](../.github/scripts/issue_refs.py), which is
also where the `Blocked by #N` pattern lives, spelled to match `unblock.yml`.

**What it will not do:** it does not merge, and it never touches a worktree it did
not create.

## Stop it

```bash
./scripts/orca/stop.sh          # drain: no new worktrees, the agents in flight finish
./scripts/orca/stop.sh --now    # ...and interrupt the fleet's agents, and freeze
                                #    every outward effect while it is set
./scripts/orca/stop.sh --all    # ...and every other agent Orca knows about
./scripts/orca/fleet.sh resume  # carry on
```

The stop is a **file**, not a signal, and it lives outside every worktree. That
is deliberate: a signal only reaches a process that is still healthy, and the
moment you most need a stop is the one where something is not.

There are **two** of them, in `~/.rommsync-fleet/`, because "start nothing new"
and "let nothing out" are two instructions and only one of them is the agents'
(#183):

| file | means | set by | read by |
|---|---|---|---|
| `DRAIN` | no new worktrees | every stop | `fleet.sh` alone |
| `STOP` | nothing goes out | `--now`, `--all` | `guard.py`, `await-review.sh`, `review-status.sh`, `resolve-thread.sh`, `fleet.sh` |

Under a **drain**, the dispatcher launches nothing and keeps reaping, and the
agents in flight are *not* frozen: they finish, push, open their PRs and
comment. That is the point rather than a leak — a merged PR is what releases the
worktree the drain is waiting on, so a drain that also froze them would wait for
what it had itself forbidden, and end only when the time-box gave each worktree
up three hours at a time. That is exactly what it used to do.

Under a **stop**, nothing reaches GitHub from any agent, whether or not it has
read the news:

- `fleet.sh` checks it before every decision and opens nothing new.
- `await-review.sh` checks it between polls and returns exit 3.
- **`guard.py` refuses `git push`, `gh pr create`, every `gh … comment/edit`
  and any `gh api` with a write method while it exists.**

Reading, building and testing stay open in both — the point is to stop work
reaching anyone, not to freeze the machine.

`fleet.sh status` says which of the two it is in, and nothing removes either
file except `fleet.sh resume`, which removes both.

## Restart it

`fleet.sh run` is a long-lived bash process. It parses its functions **once**, at
start, and never re-reads the file. So a fix merged to `main` is live in your
worktree and *not* live in the dispatcher that is running — and it fails in
halves, which is what makes it confusing: anything the poll re-derives through
`gh` (the queue filter, the labels) keeps working, while everything living in an
already-parsed shell function does not. On 2026-09-07 a dispatcher ran for 27
hours across four merged PRs that changed `fleet.sh`, none of them running
(#173).

`fleet.sh status` now says so. It records what it parsed at start, and reports
the difference:

```
running   (pid 59280)
  up since 2026-09-06 16:57:58, running fleet.sh @ 6d610ca

  STALE -- /path/to/rommsync-nx/scripts/orca/fleet.sh has changed since it
  started, and it parses the file once. These are NOT live in the dispatcher
  running:
    3994f12 harness.partial's flake is RomM retiring a gunicorn worker
    7bdb8ec A worktree whose issue can no longer merge is released
```

It answers about the **dispatcher's** checkout — the main worktree it was
started in, recorded at start — not about the one you are standing in. So
running it from a fleet worktree branched before the fix still says the
dispatcher is stale, which is the case that actually comes up.

There are two ways to be behind, and it distinguishes them, because they need
different things done:

- **STALE** — the checkout has the fix and the dispatcher is running the file as
  it was. A restart is enough.
- **BEHIND** — the fix merged and *nothing pulled that checkout*. Nothing in the
  fleet does: an agent rebases its own worktree, never the main one. So the
  bytes on disk are still the bytes the dispatcher parsed, and a restart alone
  would start the same old code again. It says so, and prints the `git pull`
  first. (This is read from the shared `origin/main`, so it costs no network and
  is as fresh as the last fetch any worktree of this repo made.)

A dispatcher that recorded nothing — one started before this existed, or whose
checkout has since gone — says "cannot say" and prints the restart anyway. A
staleness report that fails open is the same silence.

To make a change live:

```bash
./scripts/orca/stop.sh          # drain: the agents in flight finish and their
                                # PRs land, the dispatcher stays up to reap
                                # their worktrees, then exits
./scripts/orca/fleet.sh status  # until it says `idle` (it says that while
                                # draining too — that is how a drain ends)
git -C /path/to/rommsync-nx pull --ff-only   # if it said BEHIND
./scripts/orca/fleet.sh resume  # clear the drain
cd /path/to/rommsync-nx && ./scripts/orca/fleet.sh run --auto
```

Start it from the **main worktree**, and only there — which is why `status`
names that path rather than printing a relative command. A dispatcher started
inside a fleet worktree has its cwd and its own `fleet.sh` under a directory the
fleet removes as soon as that worktree's PR merges.

A drain is the polite version and it can take hours — it waits for the PRs in
flight to merge, because the dispatcher is what reaps a worktree once one does,
and killing it strands the stacks under `restart: unless-stopped`.
`./scripts/orca/stop.sh --now` interrupts the fleet's agents and stops the
dispatcher immediately; anything it had not reaped is then yours, with
`./scripts/orca/reap.sh --yes`.

A drain is safe with agents mid-work — it sets `DRAIN` and not `STOP`, so they
finish and their PRs land. What it costs is the wait. When you want the
dispatcher restarted *now* and the agents left alone, take it over directly
instead:

```bash
./scripts/orca/fleet.sh status  # names the pid, and verifies it is a dispatcher
kill <that pid>
./scripts/orca/fleet.sh status  # until it says `idle`
cd /path/to/rommsync-nx && ./scripts/orca/fleet.sh run --auto
```

`status` first, and take the pid from it rather than from `cat fleet.pid`. The
pidfile alone names whoever wrote it last; if a `kill -9` left it behind and the
OS has since recycled that number, `kill $(cat …)` signals a stranger — plausibly
one of the agents. `status` is what checks.

**Only one dispatcher runs at a time**, and `fleet.sh run` refuses to be the
second: `MAX_WORKTREES` is enforced per process, so two of them count the same
worktrees and open twice the cap between them, then reap, card and interrupt
each other's (#179). The refusal names the pid that holds
`~/.rommsync-fleet/fleet.pid` and prints both restarts above.

It refuses only to a dispatcher this machine can still see running one: `kill
-0` *and* `ps` still naming a `fleet.sh run`. A pidfile a `kill -9` left behind,
or whose pid the OS has since handed to an unrelated process, is taken over
rather than obeyed — otherwise one stale file would hold the fleet down for
good. `status` and `stop --now` ask the same question, so they cannot disagree
about whether the fleet is up, and `--now` will not signal a pid that is no
longer a dispatcher.

There is a third answer, and the two commands want opposite things from it: a
pid that is alive while `ps` says nothing at all about it. `status` reports
`running?` rather than `idle`, because `idle` is the line that sends somebody to
start a second dispatcher. `run` starts anyway — one unreadable pidfile may not
hold the fleet down — but warns and names the pid. `stop --now` signals it
anyway, because it promises the dispatcher is down when it returns.

**The settings work the same way.** `ROMMSYNC_FLEET_MAX` (how many worktrees run
at once, default 3), `ROMMSYNC_FLEET_POLL` and `ROMMSYNC_FLEET_TIMEBOX` are read
once at start, so putting one in front of `fleet.sh status` changes nothing. The
cap changes across a restart and only there:

```bash
ROMMSYNC_FLEET_MAX=2 ./scripts/orca/fleet.sh run --auto
```

Restarting deliberately does *not* clear what the dispatcher gave up on — a
crash and a reboot are not decisions about an issue. `fleet.sh retry N` is
still the only way one comes back onto the queue.

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
standing example) merges before anything else starts. Label it `foundation` and
`fleet.sh` holds the fan-out for it.

*Alone* runs both ways, and `foundation_hold` is the one place that decides it:
a `foundation` issue does not start while anything else is running, and nothing
else starts while one is. Until #215 only the first half was checked, and since
a foundation issue is the most-unblocking work by construction it is picked
*first* — so it launched into an empty fleet and everything else launched beside
it, which is the arrangement the rule exists to prevent.

The dispatcher says which it is waiting on, once, and again when that changes.
It does not narrow the rule to the issues that declare a dependency: those
`Blocked by` lines are exactly what has not been written yet for the work this
guards against.

And one the labels express badly: **an issue no agent can close.** Label it
**`needs-human-step`**. That happens two ways, and the label means either:

- **The last step is a person's.** Tagging a v1 (#148), touching a real console
  (#44) — outward, irreversible, and the maintainer's call. `ready` cannot say
  this: every blocker really is closed, and the label stays through the PR that
  does the preparatory work, so the issue comes back to the front of the queue
  the moment that PR merges. #148 was picked up again eighteen seconds after its
  own preparation landed, and would have been picked up once per cycle forever.
- **No agent can start at all.** #139 is the standing example: its scope is
  `.claude/hooks/guard.py`, and `.claude/hooks/` is in `PROTECTED_TAILS` in
  [`guard.py`](../.claude/hooks/guard.py), so from a fleet worktree the guard
  refuses to write there — from an `Edit` and from a shell command alike. #139's
  agent hit exactly that, produced no PR, and applied the label itself: the rule
  it would have had to go around is the rule its own issue exists to tighten.

Both want the **same queue behaviour** — never open a worktree — so both carry
the same label. Which one you have is in the issue's own **Scope**, not in the
label: read it before assuming there is agent-ready work behind it.

`fleet.sh` does four things with it:

- it is **not startable** — the dispatcher opens no worktree for it, in `--auto`
  or from an explicit `fleet.sh run 148`. Removing the label hands it to an
  agent, which only ever makes sense in the first sense above;
- an agent `waiting` on one is reported as **waiting for you, as expected**
  rather than as a stall. #142 stopped before `git tag` exactly as its issue told
  it to, and was flagged for it — noise on the one signal that is supposed to
  mean something is wrong;
- it is **exempt from the time-box**. This is the half that matters: #44 was
  interrupted at three hours for correctly producing nothing;
- and its worktree is **released** once there is nothing left in it — see
  "Releasing a worktree" below. Without this, #139's worktree waits for a merged
  PR that can never exist.

**In the first meaning only**, the label is a claim about the *last* step, not
the whole issue — and where there is real agent work in front of that step, it
belongs in **its own issue with its own `Closes` line**, because `merge-gate`
refuses a PR that closes nothing. That is what #142 and #148 are: #142 prepared
the release and closed on PR #146; #148 *is* the release and only the maintainer
can close it. An issue split that way can carry `needs-human-step` from the
moment it is filed.

**In the second, do nothing but label it.** There is no preparation to carve out:
a child issue of #139 is no more doable than #139 — the same guard refuses the
same writes for the same reason — and it would arrive `ready`, so the fleet would
open a worktree for it. That is the once-per-cycle re-pick above, the loop #145
closed on #148, coming back through the front door. The work is the maintainer's
start to finish; leave it as one issue.

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

An issue gets **three hours**. On expiry, if no PR closing it is open, the fleet
interrupts the agent, comments on the issue saying so, and **leaves the worktree
standing** — a stuck task is exactly the one worth looking at, and its fixture
and build state are the evidence. You get a notification.

If it cannot reach GitHub to find out, it does nothing and says so in the log:
an agent is only ever stopped on an answer, never on a lookup that failed. The
one it would otherwise stop is as likely to be the one waiting on a review as
the one grinding.

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

  Silence is its failure mode, so two things guard it. The review job is keyed on
  the **head sha** and never cancels in progress: a build can be superseded by
  the next push, but a review cannot, because the gate wants a verdict on one
  specific commit and a cancelled run leaves that commit with none — and the
  no-verdict notice is `needs: review`, so it does not fire either. And when a
  run finishes having submitted nothing, the `verdict` job says so in a comment
  *and asks for one more review, once per head*. That dispatch runs as
  `github-actions[bot]`, which the action refuses as a non-human actor unless it
  is named in `allowed_bots` — so the review job names it, and only it. Before that, both remedies the
  comment named were manual, so a PR whose review was silent waited for a person
  to notice — which is the same dead end as a gate nothing re-runs.

  **A PR that edits this file gets no review at all, and that is the action's
  rule rather than this repository's.** `claude-code-action` refuses to run
  unless the workflow file is byte-identical to the copy on the default branch:

  > Skipping action due to workflow validation: Workflow validation failed. The
  > workflow file must exist and have identical content to the version on the
  > repository's default branch.

  It is a supply-chain control — otherwise a pull request could rewrite the
  prompt and the tool list of the agent reviewing it.

  **The remedy is a dispatch, not giving up.** A `workflow_dispatch` run uses
  the DEFAULT BRANCH's copy of the workflow, so it passes that validation and
  reviews `refs/pull/N/head` — the PR's content, judged by the agreed workflow:

  ```bash
  gh workflow run claude-review.yml -f pr=<N>
  ```

  The review it submits lands against the PR's current head, so `merge-gate`'s
  independence requirement is genuinely satisfied by it. PR #87 was reviewed
  this way while editing this same file, and so was #147, which is where the
  rule was diagnosed. What must NOT happen is waiting out `await-review.sh` for
  an automatic review that cannot start; the `verdict` job's comment is the
  signal to dispatch one instead.

  The reviewer is also told to submit with `gh pr review --body`, not
  `--body-file`. Its allowed tools are Read, Grep, Glob and a fixed list of
  `gh`/`git` calls: no Write, no generic Bash, no redirection, so it had no way
  to create the file the instruction named. A run submitted only if it
  improvised away from what it was told, and PR #131 produced three green review
  runs and zero reviews. `evals/lint.sh` now checks the documented command
  against the granted tool list.
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

It also returns early on the two things a review cannot answer, rather than
spending the deadline on them: exit 7 when the PR's build is red, and exit 8 when
GitHub says `DIRTY` because something merged underneath the branch. Both print
what to do instead — for the conflict that is a rebase, a fresh
`record-review.sh` (the marker is per-commit, and a rebase changes every sha),
and `git push --force-with-lease`.

What it waits *for* is not "any review record": it imports
[`merge_gate.py`](../.github/scripts/merge_gate.py) and asks the gate, exactly as
`review-status.sh` does. A review counts when it is not by the PR's own author,
is on the head GitHub currently has, and carries a body worth reading or at least
one inline comment. That matters because replying to a review thread submits a
`COMMENTED` review attributed to the replier — so an agent answering findings
used to be handed its own empty reply back as "the review", spend one of its
three rounds on it, and then watch `merge-gate` refuse the PR for the reason the
wait had just called satisfied ([#114](https://github.com/armaatus/rommsync-nx/issues/114)).

It also hands a given review back exactly once. A round can begin on an unchanged
head — `claude-review.yml` fires on `review_requested` as well as on
`synchronize` — so the wait remembers the newest review it reported, in
`.orca/review-rounds` beside the round count, and waits for a *newer* one rather
than spending a second round on findings already in hand.

Then it fixes what is real, replies with a reason where it disagrees, and
resolves every thread. If it changed anything it pushes and comes back for the
next round; when a review arrives it is not going to change anything for, it says
so and checks:

```bash
./scripts/orca/resolve-thread.sh <thread-id> ...   # close them, and re-ask the gate
./scripts/orca/answer-review.sh "<what you did, or why you did not>"
./scripts/orca/review-status.sh    # exit 0 = every thread resolved, every check green
```

The order matters: an answer has to come *after* the review it answers, and a
push invalidates that review. So answering a review you have just pushed over
would be discarded by the review of the new head — `answer-review.sh` refuses
when no review has been submitted against the current head, rather than posting
one that nothing will count.

Resolving goes through that script rather than the `resolveReviewThread` mutation
because **no GitHub event re-runs `merge-gate` when a thread is resolved**.
`pull_request_review_thread` is a webhook event, not a workflow trigger — putting
it in `on:` invalidates the whole file, and actionlint rejects it — so the gate
went red on an open thread, the agent closed the thread, and nothing asked the
gate again. `--auto` never fired, and only a later push or `review_requested`
rescued it. `resolve-thread.sh` resolves the threads and, once the *last* one is
shut, re-runs the gate's own failed run on this head, which updates that check
run in place. That is the same mechanism `merge-gate.yml`'s `clear-stale` job
uses, and the only one available: a `workflow_dispatch` run's checks attach to
the ref it was dispatched on, not to a PR head.

### Answering the review, and why the branch waits for it

`gh pr merge --auto --squash` is run the moment the PR exists, and the review
only runs after that. So from the instant a review is submitted the branch is one
green check away from merging — and `merge-gate`'s other conditions do not stop
it. A standing `--request-changes` does. An open thread does. **Nits in the body
of a `COMMENTED` review do not**: the gate is satisfied, `--auto` fires, and the
branch goes in while the agent is still editing.

That happened four times — [#146](https://github.com/armaatus/rommsync-nx/pull/146),
[#154](https://github.com/armaatus/rommsync-nx/pull/154),
[#159](https://github.com/armaatus/rommsync-nx/pull/159),
[#168](https://github.com/armaatus/rommsync-nx/pull/168) — each leaving real work
uncommitted in a worktree the dispatcher then tried to delete, and #154's took a
defect that silences the stalled-agent detector to `main` with it
([#170](https://github.com/armaatus/rommsync-nx/issues/170)).

The window is not the life of the PR. `merge-gate` requires a review on the
*current head*, so the agent's next push turns the gate red on its own; the race
runs from the review being submitted until anything is pushed. What the agent
does in between is read the findings, fix them, run `ctest`, and push — and a
full suite here is 20 to 30 minutes. All four merged inside that run, because
that run *is* the window.

So the review declares what it left, and the author answers it:

- every review body ends with `<!-- review-findings: N -->`
  ([REVIEW.md](../REVIEW.md), and `claude-review.yml`'s prompt demands it). `0`
  is the only thing that can tell "nothing at all" from "five nits" — both are a
  `COMMENTED` verdict;
- a review reporting anything other than `0`, **or not saying**, holds the PR
  until its author comments an answer. `answer-review.sh` writes it, carrying the
  head sha, and re-runs the gate's failed run exactly as `resolve-thread.sh` does
  — an issue comment is not one of `merge-gate.yml`'s triggers and cannot be, since
  an `issue_comment` run's check attaches to the default branch rather than to
  this PR's head;
- **"I am not doing this, because" is as good an answer as a fix.** The gate
  cannot tell them apart and does not try. What it asserts is that somebody read
  the findings and decided before the code went in;
- a review reporting `0` needs no answer, so a finished PR still merges with
  nobody watching. That is the property arming `--auto` early exists to keep
  ([#90](https://github.com/armaatus/rommsync-nx/issues/90)), and it is why the
  requirement is conditional rather than a blanket "the agent declares done" — an
  agent that dies must not be able to strand a PR that nothing was wrong with.

Missing the trailer fails *closed*: the PR is held as though findings were left.
Guessing "clean" re-opens the race; guessing "found something" costs one command.

Two consequences of failing closed, both deliberate and neither free:

- **a human leaving a `COMMENTED` review holds the PR too**, since a human does
  not write the trailer. Add `<!-- review-findings: 0 -->` to the body if you
  meant "nothing here", **approve it** — an approval asks for nothing, so it is
  never held, and it is the one verdict the review job cannot give — or merge by
  hand, since `enforce_admins` is off and that works with the check red. What
  the gate deliberately does *not* do is exempt human reviewers as such: that
  would make *who* reviewed decide whether findings can be outrun, and the
  reviewer's identity is not what the race is about;
- **a review whose findings were all inline** cannot carry a trailer if its body
  is empty, so it is held until answered even once every thread is resolved.
  That is the right way round: resolving a thread says the finding was handled,
  and the answer says the review was.

One thing the answer does *not* close: `merge_gate.py` reads the **latest**
substantive review per author, so a second review on the same head declaring `0`
supersedes an earlier unanswered one. That is the same property that lets a clean
re-review clear a standing `CHANGES_REQUESTED` without a dismissal step, and it
is older than this condition; taking it away here would wedge the PRs it exists
to unwedge.

Exit 4 is the same verdict on a PR that touches `.claude/`, `.github/workflows/`
or `.github/scripts/`: nothing left to fix, and a person merges it. Exit 1 prints
the reasons, and three of them are not waiting on a review — GitHub answering
`BLOCKED` while every check is green is [#84](https://github.com/armaatus/rommsync-nx/issues/84),
a stale run still counted by branch protection, and the script prints the
`gh run rerun --job` that clears it; `DIRTY` is a conflict with the base, and
`BEHIND` is a base that moved. None of the three is answered by another review.

Checks are judged the way GitHub judges them, newest run per check *name*.
`merge-gate` runs several times on one head on purpose, and every run before the
review lands is an honest failure its own later run supersedes.

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
`--auto` waits on. It passes only when all seven hold:

1. the PR body shows a local `/code-review` pass;
2. …and a local `/mattpocock-skills:code-review` pass;
3. an independent review exists on the **current head SHA** — pushing a fix
   invalidates it, so a re-review is required;
4. the **latest** review from each author is not `CHANGES_REQUESTED`;
5. no review thread is unresolved — read from a **complete** thread list, not
   from the first page of one;
6. the body says which issue it closes. Any keyword GitHub acts on counts, so
   `Fixes #12` is as good as `Closes #12` — but a body with none merges without
   closing anything, `unblock.yml` relabels nothing, and the fleet then reports
   "nothing startable" with the work available;
7. every review still standing that reports findings — or does not say what it
   found — has been **answered** by the PR's author, on this head, since it was
   submitted. "Answering the review" above is what that is for.

Point 5's second half is its own trap. Both readers of the thread list asked for
`reviewThreads(first:100)`, the first hundred, so on a longer PR the newest
threads fell off the end and the gate reported "no review thread is unresolved"
about a state it had never established. The query now lives once, in
[`pr_payload.sh`](../.github/scripts/pr_payload.sh), which pages to the end; if
paging ever runs out the gate refuses to answer rather than answering from half
a list.

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
landed, and removes the worktree — first checking the working tree is clean, the
way it already checks nothing is unpushed. Then the next `ready` issue takes the
slot.

The removal runs **no Orca hooks**, and sweeps the stack itself once the worktree
is confirmed gone. `--run-hooks` looks like the obvious way to run `orca.yaml`'s
archive hook, and it is the wrong one: Orca runs the hook *before* it decides
whether it will remove the worktree at all, so a removal it then refuses — a
dirty tree, the submodule — has already taken that worktree's RomM stack and
volumes down. #163 caught this in #122's worktree: the agent was mid-`ctest`,
`ipc.engine` failed after 90s with ~130 tests skipped behind it, and the log said
only "could not remove it". So `fleet.sh` removes without hooks and then runs
`./scripts/orca/reap.sh --yes`, which needs no worktree; a refused removal now
leaves the stack up and says so.

#### Releasing a worktree

A merged PR is not the only way an issue stops being worked, and for a long time
it was the only way a worktree was ever released — so a blocked or abandoned one
held a slot forever. Two were cleared by hand on 2026-09-07, each holding four
containers, two ports and four volumes; while #148 sat blocked with its worktree
open, #119, #122 and #139 were queued behind work that could never start.

So `fleet.sh` also releases a worktree whose issue **went `blocked`**, **closed
with no merged PR for that branch**, **acquired `needs-human-step`**, or whose
agent the **time-box stopped**. None of those carries the guarantee "the PR
merged and nothing is unpushed" does — an abandoned worktree may hold the only
copy of real work — so the release refuses rather than guesses, on the pair that
was verified by hand before every one of those removals:

- `git status --porcelain` empty, **and**
- nothing in `git log origin/main..HEAD`.

Fail either, or fail to answer at all, and the worktree is **kept** and the log
says what is in there — the way the merged reap already reports unpushed commits
rather than removing them. `origin/main` is read as it stands and never fetched:
a stale one only ever makes commits look absent that are in fact merged, so every
error it can cause keeps a worktree rather than deleting one.

That pair answers *is anything here worth keeping*. It does not answer *is anyone
using this* — every by-hand check behind it was made on a worktree that was
already finished — and this file is where the two come apart: **an agent plans
before it edits**, so a worktree forty minutes into real work is legitimately
empty. `blocked` is not a label a person types either —
`unblock.yml` re-derives it on every merge, so it can arrive under an agent that
is mid-plan, as `needs-human-step` can arrive from the agent's own hand. So the
first pass that finds a reason **warns**: it interrupts the agent, says on the
card that the worktree goes next pass, and leaves it. The pass after that asks
the pair again — a minute is long enough to commit, or to write a plan down — and
only then removes it. If the agent is working on a `blocked` issue, that is the
point: the label says stop.

A time-box release has one extra half. Releasing the slot would otherwise hand
the issue straight back to the front of the queue and to the same three hours, so
the dispatcher records that it gave up and declines the issue — in `--auto` and
from an explicit `fleet.sh run 44` alike, the same way `needs-human-step` is
declined from both. It is listed by `fleet.sh status`, and cleared **by name**:

```bash
./scripts/orca/fleet.sh retry 44
```

Restarting the dispatcher deliberately does not clear it. A crash and a reboot
are not decisions about an issue.

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
| anything outward while `~/.rommsync-fleet/STOP` exists (a drain sets `DRAIN`, which this does not read) | a stop that depends on cooperation is not a stop |

Two of those apply **only in a worktree the fleet opened**: editing
`.claude/hooks/` and `settings.json`, and pushing with no recorded review. In
your own worktree you are the control, and a guard that argues with a person
doing manual work is a guard people route around. The two stop rows apply
everywhere — a stop that reached only the fleet's own worktrees would not be
one. It is also why the guards can still be
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
rm` does **not** unless you pass `--run-hooks`. Sweep anything left behind with
`./scripts/orca/reap.sh --yes` — which is what `fleet.sh` does deliberately
rather than passing the flag, for the reason in Stage 6.

## When the loop stalls

| Symptom | Cause | Fix |
|---|---|---|
| `fleet.sh run` opens nothing | the stop file is set | `./scripts/orca/fleet.sh resume` |
| `fleet.sh status` shows no next issue | everything `ready` is already in flight | merge something, or file work |
| ...and `status` lists the issue you want under "gave up on" | the time-box stopped it, and the fleet will not start it again on its own | `./scripts/orca/fleet.sh retry <n>` |
| Worktree provisioned, agent idle, nothing in the composer | Orca drafts the issue prompt instead of sending it | `./scripts/orca/agent-autostart.sh` — `setup.sh` starts the `--watch` form |
| Every hook says "this worktree has no linked issue" | the `orca` CLI on `PATH` cannot find `Orca.app` | nothing — the hooks probe it and fall back. If it persists: `sudo chmod -h 755 /usr/local/bin/orca` |
| `git push` refused, "nothing leaves one of those unreviewed" | the local review is not recorded for this commit | run both passes, then `./scripts/orca/record-review.sh` |
| `await-review.sh` times out | the review job never ran. Any other reason the wait had — records the gate discounts, a review already handed back, an unpushed worktree — it printed the moment it found it | `gh run list`; check `CLAUDE_CODE_OAUTH_TOKEN` is a repo secret |
| `await-review.sh` exits 2, naming `merge_gate.py` | that file is what decides which reviews count, and it does not import | fix the syntax or the missing name; nothing in the loop can answer until it does |
| `await-review.sh` exits 8, "GitHub says DIRTY" | something merged underneath the branch | rebase, re-run `record-review.sh`, `git push --force-with-lease` |
| `merge-gate` red on a PR that looks fine | usually the body is missing a review section, the review predates the last push, or a review reporting findings has not been answered | read the check's output; it says which of the seven |
| A PR sits queued and never merges | a required check never reported | `gh pr checks <n>` |
| `ctest` reports `rig.smoke` **Skipped** | RomM is not running for this worktree | `./scripts/orca/compose.sh up -d` |
