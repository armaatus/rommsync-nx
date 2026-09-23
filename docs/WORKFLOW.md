# How work happens here

This project is built by agents working in parallel, one per Orca worktree, with
one human deciding what the rules are. **This page is the maintainer's**, read
once and not per issue: it is that loop end to end — what each stage produces,
what starts the next, where a person is required, how to stop the whole thing,
and why each rule is the shape it is. An agent's own instructions are the brief
([`scripts/fleet/issue-command.sh`](../scripts/fleet/issue-command.sh) `<n>`),
with [CLAUDE.md](../CLAUDE.md) the working agreement above it. Between them they
are under 500 words, and that is the whole of what an agent reads before its
first edit.

An agent comes here for the conventions behind a rule — the `<!-- blockers -->`
marker in [Stage 2](#stage-2--spec) chief among them — and not for what to run.
It used to be told otherwise: the
brief opened by sending the agent here, and the agent that did as it was told
read 13,425 words — CLAUDE.md, the brief, and this page's longer retelling of
the brief — before its first edit, then carried them in the prompt prefix of
every request for the rest of the session
([#54](https://github.com/armaatus/autofleet/issues/54)). What changed is who is told to read it. Where this page and the brief disagree
about what to do, **the brief is right**: this one explains, and the brief
instructs.

The shape is adapted from Anthropic's
[AI-native SDLC playbook](https://claude.com/blog/the-ai-native-sdlc-playbook).
The idea it turns on: **each stage ends by committing an artifact, and the next
stage begins by reading it.** The chain of commits is the audit trail — who asked
for what, what the agent produced, what approved it. No handoff meetings; only
files.

Three things shape every decision below.

**The machine is free.** This Mac has effectively unlimited time and the fixtures
already on disk, so the heavy work — building, the full suite against a real
the fixture, and the review of the diff — happens locally. GitHub Actions runs
the suite and the gate, and at roughly 5.5 minutes per PR it is not the scarce
resource.

**A PR is reviewed once, and at most one fix answers it.** The review runs after
the pull request exists, from a context that has not seen the conversation which
produced the diff, and the dispatcher runs it — see
[Stage 5](#stage-5--one-review-at-most-one-fix-and-the-merge).

**Nothing merges on trust.** The agent never merges and never approves. It asks
GitHub to merge, and one required check — [`merge-gate`](#the-merge-gate) —
decides whether that is allowed.

---

## Start it

Run the dispatcher in a terminal the runner opens, so it is as visible as the
work it starts. The exact command is the runner's, so ask the fleet for it
rather than reading one off this page — a host project on a different
`AUTOFLEET_RUNNER` gets a different line:

```bash
./scripts/fleet/fleet.sh          # prints it, under "Run it in a terminal..."
```

If it answers "the runner is not usable here" instead, that IS the answer: the
dispatcher checks the runner before it will do anything, and there is no terminal
for it to open until that is fixed. `docs/RUNNERS.md` says what each driver
needs.

Or directly:

```bash
./scripts/fleet/fleet.sh run 11 12 13              # work exactly these issues
./scripts/fleet/fleet.sh run --auto                # keep taking `ready` issues
./scripts/fleet/fleet.sh run --auto --until 08:00  # ...and stop then
./scripts/fleet/fleet.sh run --auto --for 6h --max-prs 5
./scripts/fleet/fleet.sh status                    # what is running, what is next
./scripts/fleet/stop.sh                            # stop. See below — this always works.
```

`fleet.sh` is deterministic shell. No model runs in it. Its whole job is to decide
*which* issue gets a worktree and *when*.

**What it picks:** anything `ready` and not already in flight, ordered by how many
open issues name it in a `Blocked by #N` line. The work that frees the most other
work goes first, which is the fastest way to turn a mostly-blocked backlog into a
wide one. Milestones do not order it — `ready` already means every blocker is
closed, and a milestone number is not a claim about what can be built *now*.

Ahead of all of it: anything labelled `priority`. The blocker graph says what
*can* start, not what *should* go first, and the only way to say "this one next"
used to be to invent a dependency. A person applies that label; it reorders the
ready list and changes nothing else, so a `blocked` or `needs-human-step` issue
is no more startable for carrying it. `fleet.sh status` marks those rows.

It does not lift a foundation hold, and delaying one costs more than it looks.
A `priority` issue that starts first holds a worktree, and a foundation issue
will not join work already in flight — so the foundation issue waits. But the
scan **stops at the first foundation issue** once anything is in flight, so
every ready issue behind it is skipped for that pass too, including ones that
have nothing to do with it.

Ready list `[#151 priority, #F foundation, #A, #B]`, nothing running: pass one
launches #151; pass two reaches #F, sees a worktree in flight, and stops — #A
and #B are never considered. The fleet runs at **one** worktree for #151's whole
time-box, then at one again while #F lands alone.

Unlabelled, the same backlog — all four; #151 does not vanish when the label
comes off — sorts `[#A, #B, #151, #F]` and fills **three**, with #F waiting on
them. (`[#A, #B, #F]` would fill two, not three: the scan breaks at #F with two
worktrees live, for the same reason this passage is about.)

That order stipulates a foundation issue that frees nothing, which is the worst
case rather than the usual one. The second sort key is how many issues an issue
frees, so an `#F` that even one open `Blocked by #N` line names sorts **first**
unlabelled — `[#F, #A, #B, #151]` — and the fleet is already down to one worktree
while it lands alone, filling three only afterwards. Against that `#F`, which is
the kind the "lands alone" rule exists for, the label buys one *extra* solo
time-box, not the whole gap.

So the question to ask before applying it is not "does this jump the queue" but
"is this worth another time-box at one worktree". If it is not, the foundation
issue is the one to label.

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

### What one poll costs

The dispatcher polls every `AUTOFLEET_POLL` seconds (60 by default) for as long
as it is up, so an hour costs 60 times what one pass costs. Three different
things are being spent, and only one of them is money. The figures below are
`--auto`, which is the mode that runs unattended; **list mode differs, and the
last paragraph says how**.

| | per pass (`--auto`) | scales with | costs |
|---|---|---|---|
| **GitHub API calls** | 2 shared listings, plus 2 per owned worktree; a launch adds none | worktrees, not the backlog | rate limit |
| **Runner calls** | 1 worktree listing, plus 1 build-state read per owned worktree, plus 2 per launch and 2 per release | worktrees | nothing |
| **Model invocations** | 1 per launch; 1 more per owned worktree whose build stopped at a limit, bounded by `AUTOFLEET_BUILD_MAX_RUNS`; and a **post-PR loop** per open PR the dispatcher has not finished with, bounded by `AUTOFLEET_MAX` and by two reviews per pull request | launches, owned worktrees, open PRs | **tokens** |

Only the third row spends anything. **No model runs inside `cmd_run`'s own
process** — its body is shell and `gh` from end to end — but it is what decides
when one runs somewhere else, in two different ways, and only the first is a new
session:

*Sessions it spawns*, three kinds: `start_build` is the work itself — one
`claude -p` per run — and `after-pr.sh` runs the other two, `review.sh` and, at
most once per pull request, `fix.sh`.

**There is no fourth kind any more**, and that is what armaatus/autofleet#151
changed here. The dispatcher used to type turns into a session that was already
running: a handoff note before it cleared the conversation, a second brief
after, and the same again at each time-box and each recycle. Each was a
model turn that went out over the runner CLI and so looked free in the row
above. A run ends by itself now, and a resume is an ordinary new session —
counted in the third row like the other two, and bounded by
`AUTOFLEET_BUILD_MAX_RUNS` rather than by a clock.

Every other call a pass makes is an API or a CLI query and costs no tokens at
all. Every spawned session is marker-guarded, so none is a spawn per PR per
poll: the post-PR loop is skipped while `$REVIEWING_DIR/<pr>` names a live one,
and again while `<pr>.done` holds the current head. `review.sh` exits in two API
calls when a verdict is already on the head, and `<pr>.reviews` is the ceiling
of two that survives a dispatcher restart. A dispatcher left
polling an idle fleet overnight is free in the only sense that matters; it is
the runs that *launch* and *review* that cost, and `AUTOFLEET_MAX` is the number
that bounds both.

Concretely, at the defaults: an **idle** pass — nothing owned, every `ready`
issue already claimed by an open PR, and the loop finished with each of them —
is **3 `gh` calls and 1 `worktree list`**, whether the backlog holds ten issues
or fifty. A launch adds no `gh` call at all, because the title and labels the
card needs come out of the `ready` listing the pass already has. A **full** fleet
of three worktrees is **15 `gh` calls**: the three shared listings, and per
worktree a merged-PR check, an issue-state lookup, a merged-PR sweep for
`Closes #N`, and a state-and-labels lookup. It was published as 8 until
armaatus/autofleet#151, and 8 was measured on a fixture whose three owned issues
all named ONE directory — so the three per-worktree calls that depend on the
branch collapsed into one. A real fleet has three branches and always paid this.
The third shared listing is `review_open_prs`' own, and it arrived with
armaatus/autofleet#152: the dispatcher runs the review now rather than leaving
it to a workflow. A pull request the loop has not finished with adds three calls
and, if it needs reviewing, one model agent — up to `AUTOFLEET_MAX` of those
**on top of** the worktree agents, so six concurrent sessions at the defaults,
which is the number to know before leaving one running.

The flat idle figure is the point, and it was not always flat. Each candidate
the launch loop scanned used to take its own `gh pr list` of every open PR, so a
pass over this repository's own `ready` queue — 53 of 56 open issues, measured
2026-09-15 — made ~57 calls a minute: 3400 an hour, past the comfortable half of
the 5000/hour primary limit and into the secondary ones.
The open-PR listing, the `ready` listing and the worktree listing are now each
taken **once per pass** and read from `$STATE_DIR/poll-cache`, which
`forget_poll_answers` empties at the top of every pass. Everything in that cache
keeps one contract: an answer that **could not be read** is cached as "could not
tell" and never as "nothing found", because "no PR closes this issue" is what
sends the fleet off to open a worktree. The cost of caching is one poll of
staleness — a PR opened mid-pass is invisible until the next one — which is
why the listing is taken before the launch loop rather than during it.
`tests/test_fleet.sh`'s `budget_` phases assert these numbers, so a change that
puts the slope back fails the suite rather than the rate limit. To watch it on a
running fleet rather than in the suite, set `AUTOFLEET_LOG_PASSES=on` and the
dispatcher writes one line per poll saying the pass ended — off by default,
because a line a minute is what the say-once markers elsewhere exist to prevent.

One slope is **not** gone: `has_open_pr` still forks `python3` once per candidate
the launch loop scans, to re-parse the listing it already has. That is a process
start per `ready` issue per poll, and it is none of the three columns above — no
API quota, no runner call, no tokens — which is why it is out of this table
rather than in it. `count_startable` already answers the same question for the
whole queue in one parse, so the launch loop could read that set instead; it is
work for a day when the cost being measured is wall-clock rather than money.

**100 open pull requests used to be a cliff.** `gh pr list` is asked for 100
rows, and a listing that comes back with exactly 100 might have a 101st on the
next page — so nothing in it can be trusted to mean "no PR closes this issue".
The dispatcher refused it rather than guessing, which was right about the risk
and catastrophic about the remedy: while it held, nothing launched, nothing was
time-boxed, and the run loop polled forever on a repository whose only sin was a
hundred open pull requests.

**The page is used now** (armaatus/autofleet#151, folding
armaatus/autofleet#122). What that can get wrong is bounded: an issue whose PR
sits past the page boundary reads as free and gets a second worktree, which
`reap_merged`'s `Closes #N` sweep finds within a pass. The page is ordered
newest-first, so the pull requests a live fleet cares about are the ones on it.
It says so in `fleet.log` once per outage rather than once a minute, so a host
that really does keep 100 PRs open learns why a duplicate can appear rather than
discovering it.

**It is not the first cliff, and the row count is not what decides.**
`count_startable` hands that same listing — every row with its full `body` — to
`python3` as a SINGLE command-line argument, and the kernel caps how long one
argument may be. On Linux `MAX_ARG_STRLEN` is 32 pages, 131,072 bytes, whatever
room `ARG_MAX` leaves; on darwin there is no per-argument cap and `ARG_MAX` is
1 MiB. So the ceiling that arrives first is **total body bytes, not rows**: PR
bodies in this repository run to ~20 KB, which puts seven of them past the Linux
limit and roughly fifty past the darwin one — both well under 100, and a
different number on each host, which is the shape hard rule 1 names. Over it,
`execve` fails with `Argument list too long`, the pipeline is non-zero under
`pipefail`, and `count_startable` returns 1: the same total wedge the paragraph
above describes, reached earlier and by a different measure. And that `python3`
is the one listing parse with no `2>/dev/null`, so bash's diagnostic reaches the
dispatcher's stderr once a poll for as long as it lasts — the flood the
say-once markers exist to prevent, through a door they do not cover. The way out
is not the one-line move to stdin `has_open_pr` made: `count_startable`'s stdin
already carries the `ready` listing. It is a swap — the bodies onto stdin, the
`ready` rows, which have none, onto argv — and it is armaatus/autofleet#122's,
with the rest of the paging work. Found by the local review.

The `ready` listing's own `--limit 200` has **no** such guard, and that is a
trade rather than a free pass. Guarding it would stop a repository with exactly
200 open issues from starting anything at all, which is the worse cliff. What it
costs instead is not merely late work: `count_startable` counts off the same
truncated page, so a repository with more than 200 open issues whose newest 200
are all claimed reaches `queued == 0` with nothing owned, and the dispatcher
**exits** saying "the backlog has nothing startable left" while real startable
work sits behind the page boundary. Both listings are armaatus/autofleet#122.

**`fleet.sh status` is not a poll**, and the table above does not price it. It
asks the same questions from outside the dispatcher, where `$POLL_CACHE` is
deliberately unavailable — #35's rule is that `status` leaves `$STATE_DIR`
byte-identical — so it keeps its answers in memory for the one process instead.
That makes a screen **one issue listing and one open-PR listing** whatever the
queue holds, where it used to be one open-PR listing *per ready row*. The
**worktree listing is still per row** — `live_worktrees` keeps its answers in
`$POLL_CACHE`, which is exactly what `status` may not touch — so the runner-side
slope survives there. That is deliberate rather than missed: the table above
prices runner calls at nothing, and they are local. It is still the most
expensive read in the tree, because it prints a table the dispatcher never has
to.

**List mode still has the slope**, and the table above does not describe it.
`fleet.sh run 11 12 13` asks `issue_is_done` about every issue still on its
command line, every pass, and that is two uncached `gh` calls each — a `gh issue
view` whose `state` field `poll_issue` already has, and a `gh pr list --state
merged`. Both are named in armaatus/autofleet#69's own list of what stays
uncached, and both were left there: list mode is a person driving a named set of
issues while watching, not the unattended overnight run the flat figure is
about. It is the mode to keep short.

## Stop it

```bash
./scripts/fleet/stop.sh          # drain: no new worktrees, the agents in flight finish
./scripts/fleet/stop.sh --now    # ...and interrupt the fleet's agents, and freeze
                                #    every outward effect while it is set
./scripts/fleet/stop.sh --all    # ...and every other agent Orca knows about
./scripts/fleet/fleet.sh resume  # carry on
```

The stop is a **file**, not a signal, and it lives outside every worktree. That
is deliberate: a signal only reaches a process that is still healthy, and the
moment you most need a stop is the one where something is not.

There are **two** of them, in `~/.autofleet/`, because "start nothing new"
and "let nothing out" are two instructions and only one of them is the agents'
(#183):

| file | means | set by | read by |
|---|---|---|---|
| `DRAIN` | no new worktrees | every stop | `fleet.sh` alone |
| `STOP` | nothing goes out | `--now`, `--all` | `guard.py`, `after-pr.sh`, `review.sh`, `fix.sh`, `await-review.sh`, `fleet.sh` |

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

  STALE -- /path/to/your-repo/scripts/fleet/fleet.sh has changed since it
  started, and it parses the file once. These are NOT live in the dispatcher
  running:
    3994f12 harness.partial's flake is the fixture retiring a worker
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
./scripts/fleet/stop.sh          # drain: the agents in flight finish and their
                                # PRs land, the dispatcher stays up to reap
                                # their worktrees, then exits
./scripts/fleet/fleet.sh status  # until it says `idle` (it says that while
                                # draining too — that is how a drain ends)
git -C /path/to/your-repo pull --ff-only   # if it said BEHIND
./scripts/fleet/fleet.sh resume  # clear the drain
cd /path/to/your-repo && ./scripts/fleet/fleet.sh run --auto
```

Start it from the **main worktree**, and only there — which is why `status`
names that path rather than printing a relative command. A dispatcher started
inside a fleet worktree has its cwd and its own `fleet.sh` under a directory the
fleet removes as soon as that worktree's PR merges.

A drain is the polite version and it can take hours — it waits for the PRs in
flight to merge, because the dispatcher is what reaps a worktree once one does,
and killing it strands the stacks under `restart: unless-stopped`.
`./scripts/fleet/stop.sh --now` interrupts the fleet's agents and stops the
dispatcher immediately; anything it had not reaped is then yours, with
`./scripts/fleet/reap.sh --yes`.

A drain is safe with agents mid-work — it sets `DRAIN` and not `STOP`, so they
finish and their PRs land. What it costs is the wait. When you want the
dispatcher restarted *now* and the agents left alone, take it over directly
instead:

```bash
./scripts/fleet/fleet.sh status  # names the pid, and verifies it is a dispatcher
kill <that pid>
./scripts/fleet/fleet.sh status  # until it says `idle`
cd /path/to/your-repo && ./scripts/fleet/fleet.sh run --auto
```

`status` first, and take the pid from it rather than from `cat fleet.pid`. The
pidfile alone names whoever wrote it last; if a `kill -9` left it behind and the
OS has since recycled that number, `kill $(cat …)` signals a stranger — plausibly
one of the agents. `status` is what checks.

**Only one dispatcher runs at a time**, and `fleet.sh run` refuses to be the
second: `MAX_WORKTREES` is enforced per process, so two of them count the same
worktrees and open twice the cap between them, then reap, card and interrupt
each other's (#179). The refusal names the pid that holds
`~/.autofleet/fleet.pid` and prints both restarts above.

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

**The settings work the same way.** `AUTOFLEET_MAX` (how many worktrees run
at once, default 3), `AUTOFLEET_POLL` and `AUTOFLEET_BUILD_MAX_TURNS` are read
once at start, so putting one in front of `fleet.sh status` changes nothing. The
cap changes across a restart and only there:

```bash
AUTOFLEET_MAX=2 ./scripts/fleet/fleet.sh run --auto
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
| 3. Build | the diff and its tests | the test suite green | enforced by the hook |
| 4. The pull request | an open PR carrying `Closes #N` | the dispatcher picking it up | the agent stops here |
| 5. Review, and at most one fix | a verdict on the head | `merge-gate` going green | the rules you set |
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
those lines, and `fleet.sh` reads the same lines to order its queue. **Never
hand-edit those labels.** The lines are editable, but changing one changes what
other agents may start — do it deliberately, alone, and say so in the PR body.

It runs on a merged pull request, and on an issue `opened`, `edited`, `closed` or
`reopened` — not only on a merge. Two consequences worth holding on to, because
this file tells you to edit issue bodies as you work:

- **Your edit relabels the whole backlog**, not just the issue you touched: every
  run recomputes every open issue. That is deliberate and idempotent, and the
  runs are serialised by a `concurrency` group on the job, newest wins. A
  cancelled run can still stop partway through the backlog — so the stale label
  is removed *before* the new one is added, which leaves an issue with neither
  label rather than both. Neither is the fail-open state: `fleet.sh` starts
  nothing that does not carry `ready`, and the next run finishes the job.

  The ordering alone was not enough, and it took two goes to say so accurately.
  It covers a run that *dies* between the calls. A run whose *removal fails* fell
  straight through to the add, because the label list is a pre-run snapshot — so
  a rate-limited `removeLabel('ready')` was recorded and `addLabels('blocked')`
  ran anyway, producing the both-labels state the reorder was supposed to rule
  out. The add is now **gated on the removal succeeding**: either both writes
  land or neither does, and the issue keeps the single label it had.

  What that leaves, stated because it is the honest end of it: an issue whose
  removal failed keeps its old label. One that should have become `blocked` still
  carries `ready`, and `ready_issues()` will start it on an open blocker until a
  later run repairs it. The failure is recorded, the rest of the backlog is still
  relabelled, and the run goes red — but a red check is not something `fleet.sh`
  reads. Closing that last gap means teaching `ready_issues()` to consult
  `blocked`, which is its own change.
- **Only a line below the marker counts — when the body has one.** `Blocked by
  #N` has to begin a line, below the **first** `<!-- blockers -->` marker;
  everything under the earliest one is read, so pasting a second marker lower
  down does not supersede the section above it, it adds to it. Below a marker,
  prose in Goal, Scope or Design notes does not block anything — including a
  sentence like *"no longer blocked by #7"*, which used to register as a blocker
  and could get the agent that wrote it interrupted and its worktree reaped
  (#47).

  **A body with no marker is read whole**, and that is the case to be careful in.
  The fallback is deliberate — reading those as unblocked would start work on a
  foundation that has not landed — but it means any line that *begins* with
  `Blocked by #N`, or with a list bullet and then `Blocked by #N`, counts
  wherever it appears. Writing `- Blocked by #12 until that lands` into Design
  notes on such an issue marks it `blocked`, and `fleet.sh` reads `blocked` as
  "this worktree will never produce a merged PR": it interrupts the agent and
  reclaims the slot. **If you are adding blocker lines to an issue that has no
  marker, add the marker too** — that is what makes the rest of the body inert.

  Three boundaries either way. The anchor sees the start of a line and nothing
  before it, so a negation that *wrapped* onto the previous line still counts —
  keep a blocker line, and any sentence about one, on one line. The prefix accepts
  `-`, `*`, `+`, `>`, `1.`, `1)`, `- [ ]` and `**bold**`, so a bulleted mention is
  a blocker as surely as a bare one. And **neither reader knows what a fenced code
  block is** — a `<!-- blockers -->` line inside one is a marker like any other,
  and being first it wins, so everything below it (ordinary prose included) is
  read as blocker space. A pasted example is the body most likely to trip this,
  and the two obvious dodges do not work: **indenting** it does not help (the
  pattern allows leading whitespace) and **splitting it across two lines** does
  not either (the pattern allows a newline inside the comment). What works is
  leaving anything else on the line — write it inline in backticks the way this
  page's prose does, or put a note after it. A line that contains only the marker
  is a marker, however it is indented or wrapped.

One rule the labels cannot express: **a foundation issue lands alone.** An issue
that defines an interface later issues include (M0-2's `HttpClient` is the
standing example) merges before anything depending on it starts. Label it
`foundation` and `fleet.sh` holds the fan-out for it.

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
full build, its own fixture stack, its own data, a browser tab signed in as the
fixture admin. `setupAgentStartupPolicy: wait-for-setup` holds the agent's tab
until that finishes, so its first the test suite means something.

**There is no planning phase.** The agent's opening prompt is `/implement`, and
the issue is the plan: Goal, Scope, Design notes and Acceptance were settled at
Stage 2, on the tracker, which is where the deciding belongs. A planning round
before the first edit re-derived what the issue already said, and then rode in
the prompt prefix of every request for the rest of the session.

`## Plan` in the PR body survives, with its meaning narrowed to the half that was
ever read downstream: **what the issue asked for, and where the implementation
departed from it and why.** Departing is normal; departing silently is not, and
the review checks that section against the diff.

`/implement` is a skill from the mattpocock plugin, and it ships
`disable-model-invocation: true` — an agent will not reach for it on its own, and
no other skill can call it. It is reachable because the brief arrives as the
run's **prompt**, not as something a model merely reads. That is worth knowing
before anyone moves the brief somewhere else.

**The brief is one text, and it is under 400 words.**
[`scripts/fleet/issue-command.sh`](../scripts/fleet/issue-command.sh) `<n>`
prints the spec and two steps: build it, and open the pull request. It arrived in
two stages for a while — the second fetched with `--after-pr` when it applied —
because whole it was 1,521 words of which 1,272 were the post-PR contract, and
all of it arrived before the agent had opened a file and then rode in the prompt
prefix of every request it made for the rest of the session
(armaatus/autofleet#49). armaatus/autofleet#152 deleted that half instead: the
agent's job ends at an open pull request and what follows is the dispatcher's.
and none that is not — a brief that told an agent to wait for a review would be
a brief that spent its budget waiting.

Then it builds, with three things that are not negotiable:

- **There is no mock for the service under test.** Tests run against a real service in Docker, per
  worktree, on its own port. Failure modes a healthy server will not produce on
  demand — 401 mid-sync, a truncated body, a dropped connection, a stall — are
  forced with the fault proxy. See [TESTING.md](TESTING.md).
- **Every change carries a test that would have failed before it.** Not "the
  suite still passes". For a bug fix, write the failing test first, watch it fail
  for the reason you expect, commit it, and only then fix the code —
  `/mattpocock-skills:tdd` is that loop.
- **Verification is part of "done".** Run the test suite and read the output. If
  the rig's smoke test reports **Skipped**, the fixture is not running and most of the suite is
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

There used to be a `verifier` subagent here: a fresh context that built, ran the
suite, and answered `READY` or `NOT READY` before the PR opened. It is gone, and
the reason is arithmetic rather than distrust. `/implement` runs the full suite
at the end of the build, CI runs it on the push, and the one fix session runs it
again if a review asks for changes. A fourth run, before the pull request
existed, was the belt-and-braces this whole change is about removing.

An issue gets **three hours**. On expiry, if no PR closing it is open, the fleet
interrupts the agent, comments on the issue saying so, and **leaves the worktree
standing** — a stuck task is exactly the one worth looking at, and its fixture
and build state are the evidence. You get a notification.

If it cannot reach GitHub to find out, it does nothing and says so in the log:
an agent is only ever stopped on an answer, never on a lookup that failed. The
one it would otherwise stop is as likely to be the one waiting on a review as
the one grinding.

### Stage 4 — The pull request, and where the agent stops

**The agent's job ends at an open pull request carrying `Closes #N`.** It does
not merge, review, or wait for a review; `guard.py` refuses the first two. What the body carries is the brief's step 2,
and it is not decoration: [`merge-gate`](#the-merge-gate) reads the closing line,
and the reviewer reads `## Plan` against the diff.

**There is no self-review pass any more, and no push gate.** Both existed:
`/code-review` and `/mattpocock-skills:code-review` ran on the author's own diff
before the push, `guard.py` refused `git push` and `gh pr create` without a
`.autofleet/run/reviewed-<sha>` marker recording them, and `merge_gate.py`
refused a body that did not name both. They were the largest per-issue line item
after the build — measured on this repository, PR #126 ran **sixteen** rounds of
both passes and PR #129 eight, each round two fresh full-budget agents re-reading
the whole branch diff, 36% of one issue's 207M tokens — and what they bought was
a second opinion from the same context that wrote the code.

armaatus/autofleet#152 removed them and moved the one opinion that is
independent to *after* the PR exists, where the dispatcher runs it and the agent
cannot reach it. The marker went with them: a gate nothing can ever write is not
a gate.

### Stage 5 — One review, at most one fix, and the merge

**Two reviews maximum, ever**, and this is the section about why that number.

1. The dispatcher runs `gh pr merge --auto --squash` the moment the PR exists.
   That does not merge — it asks GitHub to, once the required checks pass. It is
   armed **first** because GitHub refuses to queue auto-merge on a PR that is
   already mergeable, so one that goes green before anything queued it has
   nobody left to merge it. That is
   [#90](https://github.com/armaatus/autofleet/issues/90), and it sat clean and
   untouched forever.
2. [`review.sh`](../scripts/fleet/review.sh) runs one `claude -p` from the repo
   root, in a context that has not seen the conversation which produced the
   diff. It is handed [REVIEW.md](../REVIEW.md), the host's
   `.autofleet/review.md` where there is one, the issue, and the diff against
   the merge base — and `--json-schema` makes its answer a shape rather than a
   hope: `{verdict, findings[{severity, file, line, text}]}`.
3. **The script posts, not the model.** The Critical and Important findings go in
   the review body with a marker naming the head; the Suggestions go in an
   ordinary comment that blocks nothing. `verdict` is `request-changes` if and
   only if something is Critical or Important, so nothing reads prose to decide.
4. On `request-changes`, [`fix.sh`](../scripts/fleet/fix.sh) gets **one** session
   in the worktree, with the review text and the diff as its prompt. It commits
   and pushes; it cannot review, approve or merge.
5. `review.sh` again, on the head the fix pushed. **That verdict is final**: a
   second `request-changes` parks the pull request with a comment and the
   dispatcher moves on.

#### What that replaced, and why

A loop bounded at four **reviews** per pull request, which spent them. The
mechanism was not a bad reviewer; it was a correct one in a loop with no fixed
point. A review is bound to the commit it judged. The author answers a finding
with a commit. The commit moves the head. The moved head invalidates the review
that asked for the fix, so the gate wants a verdict on the new head, so the
reviewer reads the whole diff again — and finds one more thing, a level down,
because there is always one more thing. #86 burned four reviews without one of
them ever judging the commit that eventually merged. #79 converged on behaviour
after two rounds and spent six more on the same finding one level further down;
every one of the six was correct, which is the whole problem.

What replaced *that* was one review and up to two **validations** — a narrower
pass asking whether the review's findings had been addressed and whether the
commits answering them broke anything. It never once ended the loop on its own.
Every validation of #132 and #133 came back `fail` for reasons unrelated to the
code — "cannot get the head's tree", "no answer posted" — so every pull request
landed on the maintainer at the cap anyway, having spent five model passes to
get there. PR #132 alone bought four full reviews, $11.54
(`~/.autofleet/reviews/cost.tsv`).

The validator's premise — were the findings addressed — is what a re-review of
the fixed head answers anyway, in one pass with no protocol. It failed because it
needed a checkout it could not have and an answer comment nobody wrote; both are
protocol, not judgement. So the protocol went: no `answer-review.sh`, no
`record-review.sh`, no `review-status.sh`, no `resolve-thread.sh`, no
`review-answered` marker, and no `<!-- validated: ... -->` trailer.

**The trade is stated plainly: a fix that CI passes but a second reviewer would
have caught, merges.** That is the price of a bounded loop, and the measured
alternative was a loop that ended on a person 100% of the time.

#### Why the verdict is a marker

GitHub refuses `--approve` and `--request-changes` on your own pull request, and
the reviewer signs in as whoever `gh` is — normally the account that opened the
PR. Every review this repository has ever received is `COMMENTED` for that
reason (PRs #133, #135, #146). So `review.sh` tries the real state first and
falls back to a `COMMENTED` review carrying
`<!-- autofleet-verdict: approve <sha> -->`, which is what `merge_gate.py` reads.

The marker is written by the **script**, from the schema's `verdict` field —
never spelled by the model — and it names a sha, so an approval cannot be
inherited by a later head. `guard.py` refuses `gh pr review`, its REST spelling
(`gh api .../pulls/N/reviews`), its GraphQL spelling (`addPullRequestReview`) and
any `gh api` whose body it cannot read, from a fleet-owned worktree — which is
what keeps it out of the author's reach. A repository with a separate reviewer identity gets the
real `APPROVED` badge for free.

#### Why the model returns a value instead of posting one

The most common failure of the whole fleet used to be a reviewer that read the
diff, formed a verdict, and ended without ever running `gh pr review`. The
verdict lived only in its transcript, the gate saw no review on the head, and
the worktree on the other side waited for something that was never coming —
`review.sh` exited 5, and the dispatcher retried at full budget up to
`AUTOFLEET_REVIEW_MAX_TRIES` times. A model that returns a value cannot forget
to return it, and the knob that bounded the silence is gone with the silence.

### The merge gate

[`merge_gate.py`](../.github/scripts/merge_gate.py) is the required check
`--auto` waits on. It asks **three** things:

1. the body says which issue it closes. Any keyword GitHub acts on counts, so
   `Fixes #12` is as good as `Closes #12` — but a body with none merges without
   closing anything, `unblock.yml` relabels nothing, and the fleet then reports
   "nothing startable" with the work available;
2. the change does not touch the enforcement layer;
3. a review of the **current head** said approve — an `APPROVED` state on that
   commit, or the marker naming it. Pushing a fix invalidates it, so a
   re-review is required.

**Everything else is branch protection**, set by `install.sh` through `gh api`:
required status checks, `required_conversation_resolution`, and
`dismiss_stale_reviews`. The gate was 3,162 lines and re-implemented all three
in Python, on a payload it had to page itself — and the paging had its own trap:
both readers asked for `reviewThreads(first:100)`, so on a longer PR the newest
threads fell off the end and the gate reported "no review thread is unresolved"
about a state it had never established. GitHub answers that question itself now,
and cannot be wrong about it.

The fourth layer it lost is the one that could not be checked at all: **has the
author ANSWERED the review**, decided by reading prose the agent wrote. Two
validation passes existed to judge that prose. Stage 5 is what happened to them.

A PR touching **`.claude/**`**, **`.github/workflows/**`**,
**`.github/scripts/**`**, or `.autofleet/`'s `guard.json`, `config` and
`review.md`, fails the gate on purpose. Those are the paths that can
disable the checks gating their own PR — the last one because `merge_gate.py`
*is* the gate, and a change to what "may merge" means must not merge itself on
the strength of its own new rules. `enforce_admins` is off, so you merge them by
hand with the check red — which is exactly the intended shape.

The decision lives in a script rather than in the YAML so it can be run and
tested without a pull request: `python3 .github/scripts/merge_gate.py --selftest`,
and the merge gate's `--selftest` covers it.

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
dirty tree, the submodule — has already taken that worktree's fixture stack and
volumes down. #163 caught this in #122's worktree: the agent was mid-the test suite,
`ipc.engine` failed after 90s with ~130 tests skipped behind it, and the log said
only "could not remove it". So `fleet.sh` removes without hooks and then runs
`./scripts/fleet/reap.sh --yes`, which needs no worktree; a refused removal now
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
`unblock.yml` re-derives it on any issue or merge event, so it can arrive under
an agent that
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
./scripts/fleet/fleet.sh retry 44
```

Restarting the dispatcher deliberately does not clear it. A crash and a reboot
are not decisions about an issue.

And what comes back from the change re-enters at stage 1:

- A review finding that appears **twice** stops being a review finding: the
  correction goes into [CLAUDE.md](../CLAUDE.md) as part of that review.
  `/mattpocock-skills:writing-for-agents` is the skill for editing it.
- Anything that reached the default branch and had to be reverted earns a rule
  with an assertion behind it — a `--selftest` row, or a suite phase — written by
  whoever handled it. A rule with nothing running it is a rule that has already
  stopped holding (hard rule 3).
- Anything the work invalidated in the tracker is edited as it is found —
  including issues that are not yours.

---

## Where to look

Everything runs through the runner, so its board is the status surface. `fleet.sh`
drives it: **`in-progress`** while a worktree builds, **`in-review`** once the
agent has opened its PR, **`completed`** on merge, and a one-line comment on each
card saying what it is waiting for. An agent used to set its own card from
inside a worktree, with `board.sh`; that went with the post-PR protocol it
reported into (armaatus/autofleet#152), and the dispatcher is now the only
writer. Its own updates still go through the runner driver, so nothing outside
`scripts/fleet/runner/` names one runner's CLI.

You get a macOS notification for the two cases you would otherwise miss: the
fleet stopping, and an issue giving up on its time-box. Everything else is
visible without being interrupted.

## Guardrails

Two layers, in increasing order of how hard they are to ignore.

**[CLAUDE.md](../CLAUDE.md)** — read in full at the start of every session, so
its size is paid on every task in every worktree. Under 400 words, which is the
acceptance of armaatus/autofleet#153 and not a checked one: a lint that asserted
it lived here too, and 43 of 63 open issues were about assertions like it. A
document that grows back is a finding on the pull request that grew it.

**Hooks** ([`.claude/hooks/guard.py`](../.claude/hooks/guard.py)) —
deterministic. Five rules, each with a `--selftest` row:

| It blocks | Why |
|---|---|
| `gh pr merge`, and its REST (`gh api …/pulls/N/merge`) and GraphQL (`mergePullRequest`) spellings | separation of duties: the agent that wrote it does not merge it. `--auto` is allowed — that asks GitHub to merge *once `merge-gate` passes*, so a rule decides rather than the agent |
| `gh pr review`, and its REST (`gh api …/pulls/N/reviews`) and GraphQL (`addPullRequestReview`, `submitPullRequestReview`) spellings, **in a fleet worktree** | the verdict is what releases the merge gate, and the reviewer signs in as the same account the author does — so the marker in a review body is the only thing separating the two. A branch that could review itself could certify itself |
| `gh api` carrying a body from a file (`--input`, `@file`) **in a fleet worktree** | a payload this hook cannot read is one it cannot judge, and `-F query=@file` put a whole mutation out of its sight |
| force-pushing the default branch | the commit chain is the audit trail. The branch name comes from `.autofleet/guard.json`, so a host on `trunk` gets the rule rather than an exemption from it |
| editing `.env` and whatever `.autofleet/guard.json` names as a secret or a pinned path | hard rule 5, and the project's own half of rule 3 |
| anything outward while `~/.autofleet/STOP` exists (a drain sets `DRAIN`, which this does not read) | a stop that depends on cooperation is not a stop |

Two of those hold **only in a worktree the fleet opened** — submitting a review,
and the `gh api` file-body refusal that makes it reachable. In your own worktree
you are the control, and a guard that argues with a person doing manual work is
a guard people route around. The stop rows apply everywhere: a stop that reached
only the fleet's own worktrees would not be one.

**What is deliberately NOT here**, because armaatus/autofleet#153 took it out and
a rule that comes back silently is worse than one that never existed:

- **The guards do not protect themselves.** `.claude/hooks/`, `settings.json`
  and the `.autofleet/` rule files were unwritable from a fleet worktree. The
  backstop that actually holds is `merge_gate.py`: a pull request touching any
  of them never merges itself, whoever wrote it. A guard nobody can improve is a
  guard that rots, and the first version of this one made its own bug unfixable.
- **The shell is not a write model.** Redirects, `tee`, `cp`/`mv`, `sed -i`,
  `rm` and `dd of=` were each modelled, and `patch -p1` and `git apply` — the two
  an agent actually reaches for — were not (armaatus/autofleet#40). A partial
  model of writing reports success on every spelling it does not know. Paths are
  judged through the editing tools instead, which is how a diff gets applied.
- **The agent is not stopped from *starting* `review.sh`.** It inherits the
  worktree, so its `gh pr review` is refused one process later by the rule that
  is still here.
- **`.github/workflows/unblock.yml` is not a protected path.** It derives the
  `blocked`/`ready` labels that decide what other worktrees may start, and the
  guard refused every write to it. `merge_gate.py` is the backstop that actually
  holds: `.github/workflows/` is in `HUMAN_ONLY_PREFIXES`, so a pull request
  touching that file never merges itself whoever wrote it. What is given up is
  the window between the edit and the merge — and unlike `.autofleet/guard.json`,
  which this hook re-reads on every tool call, nothing reads `unblock.yml`
  until GitHub runs it on the merged result.

**What the hook is not: a sandbox.** It reads a command and decides; it does not
confine one. A session that means to get past it can — an interpreter one-liner
that opens a file, a path assembled from a variable. What it holds is the
*routine* line: the `gh pr merge`, the `gh pr review`, the `--force`, the shapes
an agent reaches for while solving the problem in front of it rather than working
around a rule. Past that, the backstops are the diff and `merge-gate`. Do not
write documentation — or a commit message — that claims more.

Three properties are deliberate:

- **It does not fail open.** An unreadable payload or an unparseable command
  blocks. A guard that quietly stops guarding when something upstream changes
  shape is worse than no guard, because nothing says the enforcement went away.
- **It tokenises with `shlex`**, splits compound commands on `;`, `&&`, `||` and
  `|`, recurses into `bash -c`, and skips heredoc *bodies* — so
  `true && gh pr merge 3` is caught while a document quoting that line is not.
- **`--selftest` counts what it ran** rather than asserting a number kept in step
  by hand. Every row is either a rule this repo depends on or an escape somebody
  actually found. It is the record of what has been checked — **not** a proof
  that nothing else gets through.

Agents run in **auto** permission mode (`permissions.defaultMode`), which is only
safe because the above decides what they may do rather than a prompt for each
command.

## Repository settings this depends on

The default branch is protected, and `install.sh` sets it: `merge-gate` required
(plus whatever `AUTOFLEET_REQUIRED_CHECKS` names), conversation resolution
required, approvals dismissed when the head moves. **No required approving
review** — that would make the maintainer the bottleneck on exactly the pull
requests that already did the work. `enforce_admins: false`, so an admin can
always merge the enforcement-layer PRs that `merge-gate` deliberately fails.

Auto-merge is enabled at the repository level; without it `gh pr merge --auto`
errors out.

## The lint

Every way the payload breaks is quiet. A script that stops parsing fails in the
next worktree the dispatcher opens, during setup. A guard whose pattern stopped
matching stops blocking. Neither shows in a diff review.

**[`./evals/run.sh`](../evals/run.sh)** is the whole of it, and it runs things
rather than reading prose: `bash -n` over every script in the payload and the
suite, `shellcheck --severity=error` over the same list, and the two
`--selftest`s. `./tests/run.sh lint` is the same check, so a worktree sees a
break before CI does.

It replaced 3,600 lines that asserted one document agreed with another — that
CLAUDE.md restated a rule the brief also stated, that a page printing a pipeline
printed the one that ran, that what an agent read before its first edit stayed
under a word ceiling — plus four hand-written python scanners for classes
shellcheck already knows. 43 of 63 open issues were about those assertions
(armaatus/autofleet#153). A document that drifts is a review finding; a guard
that stops guarding is not, which is why the selftests stayed.


## Working in parallel

At most **`AUTOFLEET_MAX`** worktrees (two here). The ceiling is not machine
capacity; it is how many streams one person can review properly, and raising it
makes the review queue the bottleneck. Each worktree is isolated by everything
`.autofleet/config` gives it a seam for — its own ports, its own compose project,
its own `.env`. A host that sets none of those, as autofleet does, shares nothing
because there is nothing to share.

Removing a worktree through a runner's UI runs the teardown hook; removing it by
hand may not. Sweep anything left behind with `./scripts/fleet/reap.sh --yes` —
which is what `fleet.sh` does deliberately rather than passing `--run-hooks`, for
the reason in Stage 6.

## When the loop stalls

| Symptom | Cause | Fix |
|---|---|---|
| `fleet.sh run` opens nothing | the stop file is set | `./scripts/fleet/fleet.sh resume` |
| `fleet.sh status` shows no next issue | everything `ready` is already in flight | merge something, or file work |
| ...and `status` lists the issue you want under "gave up on" | its build ran out of turns, of budget or of `AUTOFLEET_BUILD_TIMEOUT` — the wall clock that ends a build which is wedged rather than spending — and the fleet will not start it again on its own | `./scripts/fleet/fleet.sh retry <n>` |
| Worktree provisioned, nothing building in it | the build command would not start — a bad `AUTOFLEET_BUILD_CMD`, or no credentials | the launch says so in `fleet.log`; the run's own words are in `$AUTOFLEET_DIR/builds/<n>/build.log` |
| Every hook says "this worktree has no linked issue" | the `orca` CLI on `PATH` cannot find `Orca.app` | nothing — the hooks probe it and fall back. If it persists: `sudo chmod -h 755 /usr/local/bin/orca` |
| `gh pr review` refused, "an agent does not submit the independent review of its own pull request" | the review is the dispatcher's | nothing — the dispatcher runs it, and the agent's job ended at the open PR |
| `await-review.sh` times out | the review never ran. Any other reason the wait had — records the gate discounts, a review already handed back, an unpushed worktree — it printed the moment it found it | the review is not a workflow: `scripts/fleet/review.sh`, started by the dispatcher's own poll. Read `fleet.log`, and the run's own words in `$AUTOFLEET_DIR/reviews/pr-<n>-<sha>.log` |
| `await-review.sh` exits 2, naming `merge_gate.py` | that file is what decides which reviews count, and it does not import | fix the syntax or the missing name; nothing in the loop can answer until it does |
| `await-review.sh` exits 4, "moved to &lt;sha&gt;" | the head moved while it waited | nothing — the verdict that matters is the one on the new head, and the dispatcher re-reviews it |
| `merge-gate` red on a PR that looks fine | usually the body has no `Closes #N`, or the verdict predates the last push | read the check's output; it says which of the three |
| A PR sits queued and never merges | a required check never reported | `gh pr checks <n>` |
| `./tests/run.sh` reports a phase **skip** | that phase could not judge anything — it is not a pass | read its reason; `AUTOFLEET_TEST_NO_SKIP=1` makes it a failure |
