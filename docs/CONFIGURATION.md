# Configuration

Everything autofleet knows about your project arrives through four files. The
payload in `scripts/fleet/` knows about none of it, and a change that puts a
project detail in there is the thing that makes the next repo fork this one
(CLAUDE.md, hard rule 2).

| File | Read by | Overwritten on upgrade? |
|---|---|---|
| `.autofleet/config` | every fleet script, via `scripts/fleet/config.sh` | no |
| `.autofleet/setup.sh` | `scripts/fleet/setup.sh`, once per worktree | no |
| `.autofleet/teardown.sh` | `scripts/fleet/archive.sh`, once per removal | no |
| `.autofleet/guard.json` | `.claude/hooks/guard.py`, on every tool call | no |

Three files here are **human-merge-only**: `merge-gate` refuses to let a PR
touching `.autofleet/guard.json`, `.autofleet/config` or `.autofleet/review.md`
merge itself, the same way it refuses one touching `.claude/` or
`.github/workflows/`. Those three decide what the rules *are* — the guard's
project rules, every knob the dispatcher reads, and the correctness rules the
reviewer applies — so a person merges the change. `setup.sh` and `teardown.sh` are
ordinary project code and merge like anything else.

## `.autofleet/config`

Shell, sourced. `scripts/fleet/config.sh` holds every default with `:=`, so the
precedence is **environment beats this file beats the defaults** — a one-off
`AUTOFLEET_MAX=1 ./scripts/fleet/fleet.sh run --auto` still works. Assign
outright (`AUTOFLEET_MAX=1`) if you want this file to win over the environment
too.

Some knobs use `=` rather than `:=`, and each says why at its own default:
where **empty is the off switch**, `:=` would substitute on unset *or null* and
hand an explicitly emptied value straight back the default.

### The dispatcher

| Knob | Default | What it costs to change |
|---|---|---|
| `AUTOFLEET_MAX` | `3` | The ceiling is not machine capacity, it is how many streams one person can review properly. Raising it makes the review queue the bottleneck. |
| `AUTOFLEET_POLL` | `60` | How often the dispatcher re-reads the world, in seconds. Lower burns API quota; higher delays every reap and every resume. |
| `AUTOFLEET_BUILD_CMD` | `claude` | What a build is. A wrapper seam, like `AUTOFLEET_REVIEW_CMD`: point it at a different model, a different account, or an `ssh` to the machine that holds the subscription. A wrapper need not honour `--output-format json`, and one that does not gets a cost report with no figures in it and a line on stderr saying so, rather than a crash. |
| `AUTOFLEET_BUILD_MAX_TURNS` | `400` | **What bounds a build, together with the row below.** There used to be two wall clocks here — one for the build and one for the answering half — because an interactive session runs until something stops it, and stopping it meant interrupting a live terminal with a turn that asked it to stop. A `claude -p` run ends by itself at whichever of these two it reaches, and the dispatcher reads the exit instead of enforcing one. Hours were never what cost anything: issue #71 spent 65M tokens *inside* its three-hour box. **Must be a positive whole number** — it reaches the build command as a flag, and a non-number is a build that dies the instant it starts, once per launch, forever. |
| `AUTOFLEET_BUILD_MAX_BUDGET_USD` | `15` | The other half. Two numbers rather than one because they fail differently: a build that reads whole files burns dollars at a low turn count, and one that greps in a loop burns turns cheaply. When a run ends at either, the worktree **stays** — its diff is the evidence — the dispatcher logs what it ran out of, and `gaveup-<n>` keeps the fleet from handing the same issue the same budget again. `./scripts/fleet/fleet.sh retry N` is how it comes back. |
| `AUTOFLEET_BUILD_TIMEOUT` | `7200` | **The third bound, and the one the other two cannot be.** Turns and dollars end a build that is still spending; a build wedged on a prompt nobody will answer spends neither and reaches neither, and held a worktree and a slot until a person noticed. Past this many seconds the dispatcher's poll stops the build through `runner_build_stop` — the same call `stop --now` makes, so the Orca driver, which has no pid to signal, is stopped the way it is started — and the issue lands under "gave up on" like any other exhaustion, with `retry N` to bring it back. **Must be a positive whole number**: the only consumer is `[ "$age" -ge "$AUTOFLEET_BUILD_TIMEOUT" ]`, and a non-number makes `[` return 2, which reads as false — the deadline then never fires. |
| `AUTOFLEET_BUILD_PERMISSION_MODE` | `auto` | **What the build may do without asking, and the one default here that departs from what armaatus/autofleet#151 asked for.** The issue names `--permission-mode acceptEdits`; that mode auto-accepts *file edits only*, so every Bash call not on the settings allow-list still asks — and under `-p` there is nobody to ask, so it is denied. A build that edits files and cannot run `git commit`, `git push` or the test command is not a build. `auto` is Claude Code's own per-call decision and is what `.claude/settings.json` already sets for this repository's sessions. Set it to `acceptEdits` for the issue's literal flag, and get a build that edits and never commits. Whatever the value, `--bare` is never passed: the guard hook has to run, and it is what keeps a headless agent from merging its own pull request. |
| `AUTOFLEET_BUILD_MAX_RUNS` | `3` | How many runs one worktree gets. A run that stopped at a limit **with its pull request open** is resumed: a second `claude -p` in the same worktree, whose brief is the after-PR contract. Nothing is carried across, because the branch and the PR are the state. Without a bound, a run that ends the instant it starts — a bad model name, an expired token — is an infinite resume loop that spends the account one session at a time with the log saying "resuming". **Must be a positive whole number**; `1` means a build is never resumed. |
| `AUTOFLEET_WORKTREE_ROOT` | `$AUTOFLEET_DIR/trees` | Where the headless driver creates worktrees. Empty is the default, which is the answer for every host that does not care; a host that keeps its checkouts on another volume sets this. |
| `AUTOFLEET_RM_DEADLINE` | `180` | How long a worktree removal may take before it is reported as refused. |

### The labels

`unblock.yml` maintains the first two and `fleet.sh` reads them; the other three
a person applies. `install.sh` prints the `gh label create` line for all five —
create them before anything can carry one.

**Renaming works for three of the five today.** `AUTOFLEET_FOUNDATION_LABEL`,
`AUTOFLEET_HUMAN_STEP_LABEL` and `AUTOFLEET_PRIORITY_LABEL` are read through the
variable. `AUTOFLEET_READY_LABEL` is not — `ready` is a literal in the
dispatcher's queue filter, and `unblock.yml` writes both `ready` and `blocked` by
name in JavaScript — so renaming that one gives you a dispatcher looking for a
label nothing writes: an empty queue, forever, with nothing on screen saying why.
That is [#57](https://github.com/armaatus/autofleet/issues/57). Until it lands,
keep `ready` and `blocked`.

`AUTOFLEET_READY_LABEL` (`ready`), `AUTOFLEET_BLOCKED_LABEL` (`blocked`),
`AUTOFLEET_FOUNDATION_LABEL` (`foundation`),
`AUTOFLEET_HUMAN_STEP_LABEL` (`needs-human-step`),
`AUTOFLEET_PRIORITY_LABEL` (`priority`).

`priority` is the only one that says *when* rather than *what*, and the only
ordering the tracker cannot derive: an issue carrying it goes to the front of the
ready list, ahead of whatever frees the most other work. It reorders and nothing
more — it cannot start a `blocked` issue, cannot start a `needs-human-step` one,
and does not lift a foundation hold. Label several and the ordinary ordering
decides between them. `fleet.sh status` marks the rows that carry it, so a queue
that looks reordered says why.

One consequence worth knowing before you use it, and it is larger than a
reordering. A `priority` issue can start ahead of a `ready` foundation issue,
and while it runs the foundation issue waits — a foundation issue will not join
worktrees already in flight. The part that costs: the scan **stops at the first
foundation issue** once anything is in flight, so every ready issue behind it is
skipped for that pass as well, whether or not it has anything to do with it.

The fleet therefore runs at **one** worktree for the priority issue's whole
time-box, and at one again while the foundation issue lands alone. Nothing about
"a foundation issue lands alone" is weakened, and what depends on it is `blocked`
either way — but the dependants are not what stalls, so that is the wrong thing
to be reassured by.

Measure that against the right baseline. Those dependants are also what make the
foundation issue sort to the *front* of an unlabelled queue — the second sort key
is how many issues it frees — so it would have taken a solo time-box first
anyway. The label therefore adds one solo time-box rather than costing the
difference between a full fleet and one worktree; `docs/WORKFLOW.md` works the
arithmetic through. If another time-box at one worktree is not what you meant,
the foundation issue is the one to label.

### The review

**One review per pull request, at most one fix answering it, and a re-review of
that fix whose verdict is final.** Two reviews maximum, ever. That is the whole
of what happens after the build, it is `scripts/fleet/after-pr.sh`, and the
dispatcher runs it — the agent's job ends at an open pull request carrying
`Closes #N`.

| Knob | Default | Notes |
|---|---|---|
| `AUTOFLEET_REVIEW_CMD` | `claude` | What runs the review, and the fix. A command on `PATH`, invoked with `-p`, so a wrapper can point it at another model or another account. |
| `AUTOFLEET_REVIEW_TIMEOUT` | `1800` | Seconds one review may run before it is killed and the pull request left for the next poll. The wall-clock backstop for a wedged process. **Must be a positive whole number**: the only consumer is `[ "$waited" -ge "$AUTOFLEET_REVIEW_TIMEOUT" ]`, and a non-number makes `[` return 2, which reads as false — the deadline then never fires. A bad value is refused, and because this file is sourced the refusal ends every fleet command that reads it, `stop.sh` included. |
| `AUTOFLEET_REVIEW_MAX_TURNS` | `40` | The reviewer's turn budget, passed as `--max-turns`. Half what it was: the reviewer no longer submits its own verdict, no longer fans out into a second review pass, and is handed the policy, the issue and the diff in its prompt rather than made to fetch them. |
| `AUTOFLEET_REVIEW_DIFF_MAX` | `262144` | Bytes of diff inlined into the reviewer's prompt. A diff has no upper bound and a prompt does — PR #158's own is 1.3 MB, about 328k tokens, past the whole context window — so an uncapped inline is a review that fails before it reads anything, on exactly the change too big to review by eye. Past this the reviewer is handed `gh pr diff --stat` and reads the hunks itself with the `gh pr diff` and `git diff` it is already granted. **Nothing is truncated**: a reviewer given half a hunk reports confidently on the half it can see. 256K is about 64k tokens, which leaves the policy, the brief and the issue room in a 200k window; raise it on a model with more. `0` is not an off switch — it would mean "never inline", which is the stat path for every PR. |
| `AUTOFLEET_FIX_MAX_TURNS` | `120` | The fix session's, passed the same way. Higher than the reviewer's because it edits and runs the project's suite. |
| `AUTOFLEET_FIX_MAX_BUDGET_USD` | `8` | ...and its dollar cap, passed as `--max-budget-usd`. |
| `AUTOFLEET_FIX_TIMEOUT` | `3600` | Seconds one fix may run. Longer than the reviewer's for the same reason, and validated the same way. |

The fix session's **permission mode is the build's**
(`AUTOFLEET_BUILD_PERMISSION_MODE`), deliberately and not by omission: it edits
the same worktree the build edited, under the same hooks, and a fix that needed
a different answer to "may I edit this" than the build did would mean the two
disagree about what the worktree is.

#### How the loop terminates

1. The dispatcher runs `gh pr merge --auto --squash` the moment the pull request
   exists. It does not merge — it asks GitHub to, once the required checks pass,
   which is what makes the rules decide. It is armed **first** because GitHub
   refuses to queue auto-merge on a PR that is already mergeable, so a PR that
   goes green before anything queued it has nobody left to merge it. That is
   [#90](https://github.com/armaatus/autofleet/issues/90).
2. `review.sh` runs one `claude -p` from the repository root, with `--json-schema`,
   and gets back `{verdict, findings[{severity, file, line, text}]}`. The
   **script** posts it: the Critical and Important findings as the review body,
   the Suggestions as an ordinary comment that blocks nothing, and a marker
   naming the head it judged.
3. `verdict: approve` and there is nothing left to do. `request-changes` buys one
   `fix.sh` in the worktree, whose prompt is the review text and the diff, and
   which commits and pushes.
4. `review.sh` again, on the head the fix pushed. **That verdict is final**: a
   second `request-changes` parks the pull request with a comment and the
   dispatcher moves on.

The ceiling is a file — `<pr>.reviews` under the fleet's `reviewing/` directory
— rather than the shape of the script, because a dispatcher restart, a second
machine or a person running `after-pr.sh` by hand would each otherwise get their
own two. It is written **before** each review runs: counting on success is a
crash loop that re-reviews every poll at full budget.

> **Why the loop used to be five passes.** Two self-review passes before the
> push, one independent review, and up to two validations asking whether the
> review's findings had been addressed. The validations never once ended the
> loop on their own: every validation of #132 and #133 came back `fail` for
> reasons unrelated to the code — "cannot get the head's tree", "no answer
> posted" — so every pull request landed on the maintainer at the cap anyway.
> PR #132 alone bought four full reviews, $11.54. The question a validation
> asked is what a re-review of the fixed head answers in one pass with no
> protocol, and the protocol was where it failed.
>
> The trade is stated plainly: **a fix that CI passes but a second reviewer
> would have caught, merges.** That is the price of a bounded loop, and the
> measured alternative was a loop that ended on a person 100% of the time.

#### What this gives up, exactly

The reviewer signs in as whoever `gh` is, which is normally the same account
that opened the pull request. Independence is **context-level, not
identity-level**: the reviewer is a fresh process that has not seen the
conversation which produced the diff, and that is the whole of the guarantee.

| | what it means |
|---|---|
| **The reviewer runs as you.** | It holds this machine's `gh` login — every repository and organisation that account can reach. `Bash(gh api:*)` is deliberately **not** on its tool list for that reason: it is the one grant with no ceiling. The cost is inline comments, which need the API; the findings go in the body instead. |
| **GitHub refuses `--approve` on your own PR.** | And `--request-changes`. Every review this repository has ever received is `COMMENTED` for that reason (PRs #133, #135, #146). `review.sh` tries the real state first and falls back to a comment carrying the marker, so a repository with a separate reviewer identity gets the badge for free. |
| **The marker is what the gate reads.** | `<!-- autofleet-verdict: approve <sha> -->`, written by the script from the schema's `verdict` field — never spelled by the model. `.claude/hooks/guard.py` refuses `gh pr review`, its REST spelling and its GraphQL spelling from a fleet-owned worktree, which is what keeps it out of the author's reach. |
| **There is no second venue.** | `.github/workflows/claude-review.yml` and `validate.yml` are gone. A host that wants the review inside Actions writes a workflow around [`anthropics/claude-code-action`](https://github.com/anthropics/claude-code-action) directly — which is a thing to choose, and the thing autofleet should not choose on a host's behalf. |

#### What the gate still checks, and what it leaves to GitHub

`.github/scripts/merge_gate.py` is **three questions**: the body says which issue
it closes, the change does not touch the enforcement layer, and a review of the
current head said approve. It was 3,162 lines.

Everything else is **branch protection**, which `install.sh` sets through
`gh api` on the default branch:

| rule | what it replaced |
|---|---|
| required status checks (`merge-gate`, plus `$AUTOFLEET_REQUIRED_CHECKS`) | the gate re-reading the check rollup itself |
| `required_conversation_resolution` | ~400 lines of review-thread paging in the gate, which had a truncation bug that made a partial list read as a clean one |
| `dismiss_stale_reviews` | the gate binding an approval to a head sha. It still does; this is the half that makes GitHub's own UI agree |

`enforce_admins` is left **off** on purpose: a repository admin merging the
enforcement layer by hand, with `merge-gate` red, is the designed path.

**Your build check is not guessed.** A required context is the *job's* name
inside a workflow file, which the installer cannot know — autofleet's own is
`suite`, not `ci` — and a context no job ever produces makes every pull request
wait forever on a check that never reports. So the installer sets `merge-gate`
and takes the rest from `AUTOFLEET_REQUIRED_CHECKS`: space-separated, or one
per line when a job name has spaces in it (agent-config.yml's own job is
"configuration is well-formed"). A dry run prints the exact list it would set.

```bash
AUTOFLEET_REQUIRED_CHECKS="suite" ./install.sh /path/to/your/repo
AUTOFLEET_REQUIRED_CHECKS=$'host-tests\nconfiguration is well-formed' ./install.sh /path/to/your/repo
```

**It never writes over rules you already have.** `PUT .../protection` is a full
replace rather than a merge, so a branch that already carries protection is left
alone and the installer says so: adding a payload must not silently drop a
human-approval requirement or a push restriction. It also asks GitHub which
branch is the **default** rather than using the one it happens to run on —
vendoring on a branch is the natural way to open a pull request for it, and
protecting that branch would leave `main` with no required `merge-gate` at all.

If the installer could not set them — it needs admin on the repository — or
declined because rules already exist, it says so and prints nothing else fatal.
To set or merge them by hand:

```bash
gh api -X PUT "repos/<owner>/<repo>/branches/main/protection" --input - <<'JSON'
{
  "required_status_checks": {"strict": false, "contexts": ["merge-gate", "<your build job>"]},
  "enforce_admins": false,
  "required_pull_request_reviews": {"dismiss_stale_reviews": true,
                                    "required_approving_review_count": 0},
  "required_conversation_resolution": true,
  "restrictions": null
}
JSON
```

#### What a round costs

Every round appends one row to `$FLEET_DIR/reviews/cost.tsv` —
`when / pr / head / input tokens / output tokens / usd / ms / turns` — taken
from the `--output-format json` envelope the reviewer emits. The row is written
**whatever the round decided**, including a round that produced no verdict at
all: the round happened and it cost what it cost, and a ledger that records only
the successful ones cannot answer "what did this pull request spend".

**It degrades, as far as it can:** `AUTOFLEET_REVIEW_CMD` is a wrapper seam, and
output that does not parse is written through as text with no row recorded. The
measurement is never a prerequisite for anything.

**`cost.tsv` is the one store under `$FLEET_DIR/reviews/` that
`AUTOFLEET_KEEP_REVIEWS` never sweeps.** It is the before-and-after of every
change that claims to have made a review cheaper, and it is appended to, never
rewritten — a run that rewrote it would delete the "before".

### What replaced the handoff note

A build used to be one interactive session covering the plan, the
implementation, both self-review passes, the push, the PR, the review and up to
two validations. It could not be allowed to *end*, so five subsystems existed to
keep it usable: a note it wrote before being cleared and a cap on that note's
length, a decision about whether to clear it at the pull request, the command
that cleared it, how long it got to write the note, and how often to do the
whole dance mid-build. `AUTOFLEET_HANDOFF_MAX_WORDS`, `AUTOFLEET_CONTEXT_RESET`,
`AUTOFLEET_AGENT_CLEAR_CMD`, `AUTOFLEET_HANDOFF_GRACE_SECONDS` and
`AUTOFLEET_CONTEXT_RECYCLE` are all gone with it.

**The branch and the pull request are the state now.** A run ends at
`AUTOFLEET_BUILD_MAX_TURNS`, `AUTOFLEET_BUILD_MAX_BUDGET_USD` or
`AUTOFLEET_BUILD_TIMEOUT`; if its PR is
still unopened, the dispatcher starts a second `claude -p` in the same worktree
with the same brief, and that run reads the branch and the pull request. Nothing
has to be written down, so nothing can be written down badly — which was the
note's real cost: a recycle threw away everything not in 300 words, and an
interval chosen badly made the work worse *and* dearer by making it redo what it
forgot.

The one thing a host has to do about this is **commit as it goes**, which the
opening brief says in those words. Uncommitted work is what a second run cannot
see.

### What the fleet keeps

Every store under `$FLEET_DIR` only ever grew until these two knobs arrived, and
the cost is not the bytes: stale state gets read as current. A transcript named
for a head nobody is reviewing is a file the next reader opens looking for an
answer. Set either to `0` to keep everything, which is what a host project
debugging its own reviewer wants.

| Knob | Default | Notes |
|---|---|---|
| `AUTOFLEET_KEEP_REVIEWS` | `3` | Reviewer transcripts kept **per open pull request**, and the off switch for the `reviewed-<sha>` sweep too, so `0` means every piece of review state the dispatcher would otherwise delete. |
| `AUTOFLEET_LOG_MAX_BYTES` | `1048576` | Bytes of `fleet.log` kept before it rotates to `fleet.log.1`. **One** generation, because the point is a bound and two files at the cap is twice the cap. |
| `AUTOFLEET_LOG_PASSES` | `off` | `on` and the dispatcher writes one line per poll saying the pass ended and what it found; anything that is not `on` or `off` is refused at startup rather than read as one of them (`0` used to turn it **on**). **Off by default** — a line a minute is the log volume the say-once markers exist to prevent — and the only way to know a pass has finished without guessing from a clock. Turn it on when you are measuring what a pass costs (see [WORKFLOW.md, "What one poll costs"](WORKFLOW.md)) or watching a fleet that appears to be doing nothing. |

**What goes, and when.** Older transcripts for an open PR go; every transcript
for a PR that is no longer open goes regardless, because a review of a closed PR
is answering a question nobody is asking. Never on the pass the PR drops off the
open list, though, and never while a reviewer for it is still writing: the sweep
keeps one bookkeeping file per closed PR, `reviews/.closed-<n>`, recording that
it was already seen closed on an earlier pass, and collects that file as soon as
the transcripts it tracks are gone.

**What it costs.** `review_open_prs` runs the sweep from the open list it already
has, plus **one `gh pr view <n> --json state` per candidate pull request per
pass** — per pull request, not per transcript, and only after a whole grace pass.
The open list is `--author "@me"`, which is right for deciding whom to review and
wrong for "is this PR still open": on a host whose worktrees open PRs under a
different account than the dispatcher's `gh` login, absence from it would mean
every PR looked closed. Anything but a confident `CLOSED`/`MERGED` keeps the file.
An `OPEN` answer returns the PR to the open list for the rest of that sweep, so
its transcripts are capped like any other open PR's rather than accumulating with
nothing to trim them — and drops the `.closed-<n>` marker, so the question is
asked again on the pass after next rather than on every one. The sweep says how
many it took.

**The review records go the same way.** `$FLEET_DIR/reviewing/` holds three
small records per pull request beside the loop's lock — `.done`, `.reviews` and
`.said`; what each one holds is written out once, in `scripts/fleet/fleet.sh`'s
header above `is_review_record`, and not restated here. What a host operator
needs from this page is that they are swept on **the same rule and through the
same code** as the transcripts above — `pr_sweep_verdict`, which both call: not
on the pass the PR drops off the open list, not without a confirming
`gh pr view <n> --json state`, and one decision per pull request per pass rather
than one per file, with `reviewing/.closed-<n>` as that sweep's bookkeeping. Two
things are NOT shared, and both are deliberate. A PR whose reviewer is still
running is skipped by the transcript sweep and not by this one — the record
sweep only ever reaches a PR GitHub has confirmed closed, and a reviewer still
writing against one of those is finishing work on a PR that is already gone. And
`<pr>.rounds` outlives a `stop.sh`: it counts what the PULL REQUEST has cost
rather than what this dispatcher's run has, nothing re-derives it, and clearing
it on a restart would hand every open PR a fresh set of rounds — the cap not
existing for anybody who restarts the fleet. The record sweep described here is
what prunes it, once the PR has closed. The protection is not decoration — the
file at stake is `<pr>.done`, and a `.done` deleted under an open PR hands that
head a reviewer every poll.

**The rotation** is `mv` rather than truncate-in-place, and **not while a
reviewer is running** — `review.sh` is spawned with `>>` on `fleet.log` and holds
the inode for up to `AUTOFLEET_REVIEW_TIMEOUT`, so rotating under it sends its
output to a file nobody reads and the next rotation unlinks what it is still
writing. The dispatcher's own writes are `tee -a`, fresh per call, and follow a
rename without noticing. So the cap is a bound the fleet reaches **between**
reviews, not a hard ceiling: on a busy fleet the log can sit above it until the
reviewers finish. If the rotation cannot happen at all — a read-only state
directory, an undeletable `fleet.log.1` — it says so once per dispatcher, and
again after a rotation that works.


### What a run costs

`./scripts/fleet/fleet.sh cost` reports what each issue's worktree spent, read
back out of the agent CLI's own session transcripts — no extra instrumentation,
because the CLI already wrote the answer.

```
issue  sessions  input  output  cache read  cache write
   48         3    170  23,340   2,898,820      102,120
   52         1     48  24,838   2,024,487       89,837
-----  --------  -----  ------  ----------  -----------
total         4    218  48,178   4,923,307      191,957
```

`--json` prints the same figures for something that is not a person, which is
the point: these numbers are only useful compared across runs.

| Knob | Default | Notes |
|---|---|---|
| — | — | **Nothing to configure.** The report reads `$AUTOFLEET_DIR/builds/<issue>/`, which the fleet already owns. |

It moves the **path**, not the format. `cost.sh` reads a fixed entry shape
(`type: assistant`, `message.usage`, the four token keys) and a fixed slug rule,
so a host whose CLI writes that shape somewhere else points this at it — and a
host whose CLI writes something else entirely sets it empty rather than getting
a report of zeros.

`scripts/fleet/cost.sh`'s header is the authority on how the report reads a
transcript; this section describes the same rules for whoever is setting the
knob, and if the two disagree, this one is wrong.

**The four figures are never added together.** A cache read is roughly a tenth
of an input token, so a run that looks expensive on `input` may be almost
entirely cache, and one total would hide exactly the difference the report
exists to show.

**What ties a session to an issue** is the worktree path. The slug is the
absolute working directory with every non-alphanumeric character turned into
`-`, truncated at **200** characters with a hash of the path appended past that
— a slug over the cap is matched on its prefix, and a prefix that matches more
than one directory identifies none of them and is reported rather than guessed.
`own()` records each issue's path under `$FLEET_DIR/ran/<issue>` —
*appended*, so an issue that `retry` ran twice is measured across both
worktrees, and deliberately *not* cleared when the worktree is released, or the
report would empty itself exactly when a run finishes. One short file per issue
the fleet ever starts.

**Subagent transcripts count.** A subagent writes its own file under
`<session-id>/subagents/`, and this repo uses them per issue — the researcher,
and the review pass `/implement` runs before it commits. They are folded into the
four token figures; `sessions` counts the top-level sessions only, so it stays the
number of times an agent *produced* something in that worktree — a session
interrupted before its first assistant message carries no `usage` and is not
one.

**It never fails a run.** No transcript root, a root that is not there, a
worktree already reaped, an entry with no `usage`, a half-written last line in a
transcript a live agent is still appending to: each costs one line on stderr and
exits 0. Explanations go to stderr in both shapes, so `--json` on stdout stays
machine-readable.

**What it does not see.** Only sessions whose working directory was the worktree
itself. The reviewer runs from the main checkout — deliberately, so `guard.py`'s
fleet-worktree rules do not apply to it — so it lands under the main checkout's
slug and its tokens are not in that issue's row. `$FLEET_DIR/reviews/cost.tsv`
is where those are.

### Per-worktree isolation

A worktree's identity is a pure function of its absolute path: a slug, an offset,
and a project name. Teardown **recomputes** it rather than reading `.env` back,
because `.env` is generated by a hook that is itself allowed to fail half way,
and a port nothing can re-derive is a port nothing can release.

| Knob | Default | Notes |
|---|---|---|
| `AUTOFLEET_PROJECT_PREFIX` | `af` | Must be unique to this project on this machine: `reap.sh` sweeps stacks matching it whose worktree is gone. |
| `AUTOFLEET_PORTS` | *(empty)* | `name:base` pairs, e.g. `API:21000 PROXY:23000`. Each worktree gets `base + offset`, written to `.env` as `NAME=<port>`. |
| `AUTOFLEET_PORT_SPAN` | `2000` | The modulus the offset is taken over, and therefore the width of each range above. Bases must be at least this far apart. |
| `AUTOFLEET_COMPOSE_FILE` | *(empty)* | Empty means this project runs no containers, and teardown skips docker entirely. |
| `AUTOFLEET_COMPOSE_DOWN_ARGS` | *(empty)* | Your `--profile` flags. **Getting this wrong leaks**: `down` only touches services whose profile is active, so an inactive one survives teardown, comes back under `restart: unless-stopped`, and holds a port with no directory left to identify it by. |

### The rest

`AUTOFLEET_RUNNER` (`headless`) — see [RUNNERS.md](RUNNERS.md). A name with no
`scripts/fleet/runner/<name>.sh` beside it is named where `lib.sh` sources it —
the file it looked for and the drivers that do ship — and stops the three scripts
that call `fleet_require_runner`, so a typo here is caught before a worktree is
opened rather than by an agent sitting on a prompt that never sends.
`ORCA_CLI_COMMAND` (Orca driver only) — the CLI to try first, ahead of `orca`,
`orca-dev`, `orca-ide` and the `/Applications` fallback. **Exactly one command**,
so a path containing spaces is a single candidate and a value with arguments in
it (`orca --debug`) is not a command at all: nothing on `PATH` has that name, the
candidate is turned down, and the resolve falls through to plain `orca` — a
different CLI from the one that was configured. It used to be expanded unquoted,
where the same value was two candidates that both failed. The driver's refusal
names this knob, so it is listed here rather than left in
`scripts/fleet/runner/orca.sh`, and it has no default line in `config.sh` but is
settable in `.autofleet/config`, which `config.sh` sources into the same shell
before `lib.sh` sources the driver.
`AUTOFLEET_SETUP_HOOK` / `AUTOFLEET_TEARDOWN_HOOK` — paths to the two hooks.
`AUTOFLEET_TEST_COMMAND` — quoted into the agent's opening prompt, so it names
the command your project actually runs rather than one autofleet guessed.
`AUTOFLEET_NOTIFY_TITLE` — the title on the macOS notifications the dispatcher
posts.
`AUTOFLEET_DIR` (env only, `~/.autofleet`) — where `STOP`, `DRAIN` and the owned-
worktree registry live. Outside every worktree on purpose: a stop that only some
processes can see is not a stop.

## `.autofleet/setup.sh`

Runs once per worktree, from the repo root, with `.env` already exported and
`FLEET_OFFSET` available. **Its exit status is provisioning's exit status** — a
non-zero exit fails the worktree loudly rather than handing an agent a broken
rig, which is the failure mode `wait-for-setup` exists to prevent.

This is where containers, fixtures, a build, a venv, seeded data all go.

## `.autofleet/teardown.sh`

Runs before the stack comes down, from the repo root, when a worktree is removed.
Never fatal: the runner is removing the worktree either way, and a hook that
exits non-zero must not stop the stack removal, which is the part that leaks if
it is skipped.

## `.autofleet/guard.json`

What `guard.py` refuses to write in **this** repo, over and above its universal
rules — merging a pull request, force-pushing the default branch, editing
`.env`, anything outward while the fleet is stopped, and (in a fleet worktree
only) submitting the review of its own pull request. The **key** here also names
that default branch, so a host on `trunk` gets the force-push rule rather than
an exemption from it.

```json
{
  "protected_paths": [
    {"path": "server/contract/captures/",
     "tail": "captures/",
     "reason": "the pinned API contract a test diffs a live probe against"}
  ],
  "secret_suffixes": ["/config.ini"],
  "secret_contains": ["/token.dat"],
  "secret_tails":    ["token.dat", "config.ini"]
}
```

`reason` is the whole value of a `protected_paths` entry: "blocked" with no *why*
sends the agent looking for a way around it. `tail` is how a relative path after
a `cd` is still recognised — suffix-matching a distinctive tail is imperfect, and
the module docstring says so.

A malformed file is **fatal**, not ignored. A guard that silently falls back to
"protect nothing" on a typo reports success on every write it was installed to
stop.

A *well-formed* file with a mistyped key is the same failure wearing a nicer
suit: `guard.py` reads every key with `.get()`, so `protected_path` is not an
error, it is no rule at all. `guard.py --selftest` drives every key in this file
against the rules it produces, so a key nothing reads is a key with no assertion
behind it.

## Checking your work

```bash
./evals/run.sh                                 # bash -n, shellcheck, both selftests
python3 .claude/hooks/guard.py --selftest      # ...either on its own
python3 .github/scripts/merge_gate.py --selftest
```
