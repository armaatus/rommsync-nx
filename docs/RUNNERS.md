# Runners

A **runner** is whatever creates a worktree, opens a terminal in it, and can be
asked what it is doing. `scripts/fleet/runner/$AUTOFLEET_RUNNER.sh` is the
driver, sourced by `lib.sh`.

Two ship. **`headless` is the default**: a plain `git worktree` and one
`claude -p`, needing `git`, `gh` and the build command and nothing else, and it
is what CI runs. `orca` is the desk path — it creates the worktree through the
app and runs the *identical* build command in a terminal tab, so a maintainer
can watch it. No code outside `scripts/fleet/runner/` calls the app's CLI, and

```sh
grep -rni 'orca' scripts/fleet --include='*.sh' \
  | grep -v '^scripts/fleet/runner/' \
  | awk 'BEGIN { sq = sprintf("%c", 39) }
    {
      code = $0
      sub(/^[^:]*:[0-9]+:/, "", code)   # drop path:lineno:
      # Truncate at the first # that is NOT inside quotes, so a trailing comment
      # is prose and a `"#$num"` in the middle of a line does not hide the rest.
      out = ""; q = ""
      for (i = 1; i <= length(code); i++) {
        c = substr(code, i, 1)
        if (q == "") {
          if (c == "\"" || c == sq) q = c
          else if (c == "#") break
        } else if (c == q) q = ""
        out = out c
      }
      if (tolower(out) ~ /orca/) print
    }' \
  | grep -v '^scripts/fleet/config.sh:[0-9]*:: "${AUTOFLEET_RUNNER:=orca}"$'
```

returning nothing is what keeps it that way. It used to be run by a lint check
that also compared itself against this fence, so the page and the check could not
drift; armaatus/autofleet#153 removed both, along with 3,600 lines of other
assertions that one document agreed with another. **This fence is now the
definition**, and running it against a diff is a line in
[`.autofleet/review.md`](../.autofleet/review.md) rather than a build step.

The comment is truncated rather than the line skipped, and that is deliberate:
Orca is *named* outside the driver all over this repo, which is the rule above
rather than a violation of it. What the grep forbids is **code** outside the
driver reaching for the runtime — so a trailing `# not orca` is prose, while
`ORCA_DEADLINE=20  # a knob` is a leak, and a `"#$num: ..."` string in the
middle of a line does not hide the rest of it. `scripts/fleet/config.sh` is the one exception,
allowed by name: it is where the default driver is chosen, so naming one there
is the choice. A second runner is a second file in
that directory.

Orca is still *named* outside it — `orca.yaml` is where a worktree's hooks are
wired, and the hooks it points at explain Orca's behaviour in their comments.
["What autofleet still assumes about the runtime"](#what-autofleet-still-assumes-about-the-runtime)
below is the honest list of what that leaves a second driver to answer.

## The contract

A driver defines the functions below and nothing outside the directory may
assume how any of them work. Two properties matter more than the shapes, and
both are load-bearing rather than stylistic:

1. **Every call has a deadline.** A driver that can block forever takes the
   worktree with it: `orca.yaml`'s `setupAgentStartupPolicy: wait-for-setup`
   holds the agent's tab until `setup.sh` returns, so a hook that never comes
   back costs the whole worktree. macOS ships no `timeout`, so `lib.sh` provides
   `fleet_run_with_deadline` — a watchdog, not part of this contract, because
   `finish_removal` runs `reap.sh` through it too.
2. **"I could not tell" is not "nothing".** Every one of these distinguishes a
   negative answer from a failed question. Conflating them is how a dispatcher
   waits out its whole time-box and then reports that nothing arrived — see the
   `*_blind_ps` and `foundation_blind` phases in `tests/test_fleet.sh`, which
   pin exactly this.

Nothing below returns JSON. A caller that parses JSON has hardcoded one runner,
which is the thing this seam exists to prevent.

The tab-separated shapes assume **paths contain no literal tab**. One would shift
every column and the fleet would read a branch name as an issue number — the JSON
these replaced was immune to that, and this is the honest cost of the seam. A tab
in a worktree path is legal on unix and vanishingly rare; a backslash is not
rare. Raised by the independent review.

Where a function relays **the runtime's own words** on failure, it relays **at
most the first three lines**. That bound is the driver's, not the caller's: the
dispatcher used to cap them itself at each callsite and gave that up when the
calls moved, so a driver relaying a verbose runtime unchecked would flood
`fleet.log` with nothing left to stop it. Three lines is what the first line of a refusal plus its
context has always taken. Raised by the independent review.

### Is it there

```sh
runner_available           # 0 if this runner can be used at all
runner_dispatcher_hint     # one line: how a person starts the dispatcher visibly
runner_set_deadline <secs> # for calls from this process
```

`runner_available` **says why on stderr when it cannot** — only the driver knows
what to check next, and "is the Orca app running?" is not a sentence the
dispatcher can write for an arbitrary runner. The caller adds the consequence.

What it says is **the runtime, what was tried, and what to install**, within the
same three-line bound every relay has. Naming the runtime and stopping there
leaves the reader with a true sentence and no next step, and this is the one
message a person sees on a machine that has never worked — so a driver lists the
candidates it probed and how each was turned down, one comma-joined line rather
than one line each, and ends with the install or start. The Orca driver collects
those reasons *during* its resolve rather than re-deriving them afterwards: a
second pass costs another `--version` timeout per candidate, and this is the
first call `setup.sh` makes while the runner holds the agent's tab.

**Every caller that reaches for the runtime probes before it spends anything.**
`fleet.sh` (at source time) and `setup.sh` both call it; `setup.sh` is
fatal on a no, because everything it provisions is for a build the dispatcher is
about to start in that worktree. `setup.sh` probes *after* `env.sh` and before
the submodules and the project hook — env.sh is milliseconds and is
where a machine missing its basic tools says so, and a probe in front of it
answers a missing `shasum` with "is the runtime running?", which is the wrong
machine named confidently.

**Your driver must have defined `runner_available` and `runner_worktree_create`
by the time sourcing ends.** They are the two `lib.sh` looks for, and they are
those two because they are the first calls every guarded caller makes: the probe,
and then the one whose failure this issue opens with. A driver that defines the
probe and then `return`s -- the documented shape for one that bails when a
dependency it needs is absent -- used to pass as working, and its first real call
was `command not found` relayed as "could not create it:" with nothing under it.
A non-zero status from sourcing the file is not by itself a refusal: a conformant
driver that ends on `[ -n "${FOO:-}" ] && export FOO` sources non-zero and
implements everything. It raises the bar to that second function rather than
deciding on its own.

A driver that is **not there at all** is a different question and is answered
one layer up. `lib.sh` names `scripts/fleet/runner/$AUTOFLEET_RUNNER.sh` and the
drivers that do ship, sets `FLEET_RUNNER_MISSING`, and **finishes sourcing** —
it neither returns nor exits, because most of what it defines has nothing to do
with a runner and the scripts that source it mostly call no `runner_*` at all
(`cost`, and the whole post-PR loop). **Two** — `fleet.sh` and `setup.sh` —
call `fleet_require_runner`
before their first `runner_*`, which adds the consequence and stops; without it
that first call is `command not found`, and rc 127 through a `|| die` reports
the consequence as the cause.

`issue-command.sh` is the third script that names a `runner_*` and is
deliberately **not** guarded: it prints an agent's brief, which needs no
runtime, and asks `runner_available` only behind
`if [ -z "$ref" ] && runner_available 2>/dev/null` to decide whether it can
*also* resolve this worktree's issue. With no driver that call is rc 127, the
`&&` is false, and the script carries on doing what it was run for. Guarding it
would refuse an agent its own brief. A sixth caller is guarded unless it can
make the same argument.

The ORDER is the property, not the presence: a guard after the first `runner_*`
guards nothing. A repository whose configured runner names no file at all is
refused by `lib.sh` where it sources the driver, red before an agent is opened
rather than after. Both were lint checks until armaatus/autofleet#153;
[`.autofleet/review.md`](../.autofleet/review.md) is where a reviewer is told to
look for them now.

**Which stream a failure's words go on, per function, because the answer is not
the same for all of them.** An earlier version of this paragraph said "stderr
for `runner_available`, stdout for everything else" — an absolute rule that the
reference driver broke in one branch and whose stated reason did not survive a
reader checking it. The truth is three cases:

- **`runner_worktree_create` MUST use stdout.** `launch` does
  `runner_worktree_create … >"$out"` and captures stdout *only*, because stdout
  is where the new worktree's path comes back. A reason on stderr goes to the
  dispatcher's terminal and never into `$out`, so the log reads
  `  could not create it:` followed by nothing. This one is load-bearing, and
  the reference driver shipped it wrong three times before the page said so.
- **`runner_worktree_set` should use stdout, by convention rather than
  necessity.** Its caller merges the streams — `card` does
  `runner_worktree_set … >"$out" 2>&1` — so either stream reaches the reader
  today. Stdout keeps it the same shape as create; nothing breaks if a driver
  uses stderr. (`board.sh` was the second caller and went with the post-PR
  protocol it reported into; the convention outlived it.)
- **`runner_available` uses stderr**, because it is a probe whose output nobody
  captures, and the caller adds the consequence.

- **`runner_worktree_remove` relays too, on rc 1** — "answered and REFUSED" is
  the one answer a person has to act on, and `remove_worktree` has no other
  source for the cause: it prints `the removal refused: <line>` straight from
  what the driver wrote. A driver that returns 1 silently leaves only
  `nothing was torn down -- its stack is still up`, and nobody can tell a
  submodule refusal from a dirty working tree on the one path where a stack and
  its ports are still running. Its caller merges the streams, so like `set` this
  is convention rather than load-bearing — but **relaying at all is not
  optional here**.

One thing the rule deliberately does not cover: a message about the CALLER being
malformed is not a runtime failure and goes to stderr — `runner_worktree_set`'s
rc 2 for a bad pair list is the one that exists. `runner_build_stop` produces no
failure words at all: its one failure is a pid, not a sentence — see below.

`runner_dispatcher_hint` is the one human-facing string that is runner-specific;
`fleet.sh`'s usage prints it rather than hardcoding a command line that is wrong
for every other driver.

`runner_set_deadline` exists so a caller that needs a shorter one can ask
**without knowing which driver it has**. A driver with no deadline to set may do
nothing, but it must DEFINE the function — a missing one is `command not found`
on a script that does not set `-e`. `tests/run.sh fleet runner_stub` drives the
whole fleet on a driver that is not Orca, which is what actually exercises the
contract; a missing function fails there. Creating a
worktree is exempt by convention — nobody polls a create, and a short deadline
on a call that legitimately takes minutes is a failed launch rather than a
shorter wait. **The build is exempt too, and for a stronger reason**: it runs
for hours and no call in this contract waits on it. `runner_build_start` returns
once the build is up and `runner_build_state` answers from files, so neither can
block whatever a deadline says.

### Worktrees

```sh
runner_worktree_create <repo> <name> <issue>
runner_worktree_list      # `path<TAB>branch<TAB>issue` lines
runner_worktree_issue     # THIS worktree's linked issue
runner_worktree_set <path> <key> <value> [<key> <value> ...]
runner_worktree_remove <path> [<deadline>]
```

- **`runner_worktree_create`** prints the new worktree's path on success, and the
  runtime's own words on failure — "could not create it" with nothing after it
  reads the same whether the app is down or the branch already exists. Those
  words go on **stdout**, not stderr: `launch` captures stdout only, because
  that is where the path comes back, so a reason on stderr never reaches the log.
  **Rc 0 with empty stdout is a THIRD state, not a success**: it means the
  runtime answered and the driver could not find a path in the answer. The
  dispatcher depends on that being distinguishable — it says "created, but the
  runner reported no path" and keeps the slot rather than owning a worktree it
  cannot address — so a driver must not turn an unparseable answer into rc 0
  with a made-up path, and need not invent a new code for it either.
- **`runner_worktree_list`** emits `path<TAB>branch<TAB>issue`, `-` where the
  runner has no answer for a field, **scoped to THIS repository**, excluding the
  main worktree and archived ones — the fleet counts these to decide whether it
  may launch, and neither of those is a slot. Scoping is the DRIVER's job, and
  it is a property of this contract rather than an optimisation: the fleet
  resolves every issue number this returns against its own repository, so one
  worktree belonging
  to another project takes a slot from `MAX_WORKTREES`, its issue number can
  answer `in_flight` for one of ours, and — because a foundation issue waits for
  the count to reach zero, and `foundation_in_flight` cannot read a foreign
  issue's labels — it stops the dispatcher launching **anything at all**,
  indefinitely. Nothing this fleet does can close another project's worktree, so
  that stall is permanent. A driver whose runtime cannot scope the query must
  filter the answer itself, and must return non-zero rather than hand back an
  unscoped list: "could not tell" costs one pass, an unscoped list costs the
  fleet.

  If a runtime takes a `path:`-style selector, note what it names: a repository
  ROOT, not a worktree. Half of `scripts/fleet/` runs from inside a worktree, so
  a selector built from the caller's cwd names the wrong thing and the runtime
  answers "no such repository" — the same permanent stall, through the fix for
  it. The Orca driver resolves the root with `git rev-parse --git-common-dir`
  and falls back to the checkout it was given.

  **Non-zero when the list could not be read**, which is not the same as
  "nothing is running": reading a failed call as zero live worktrees is how one
  transient hiccup turns into three duplicate worktrees. That clause is the
  contract's, not Orca's, and applies to every driver.
- **`runner_worktree_issue`** has THREE answers, and the middle one is why it is
  not a boolean: `0` with the issue on stdout, `2` for "there is no linked
  issue", `1` for "the runtime would not say". A hook that reads 1 as 2
  announces that a worktree the fleet linked to an issue has none — which is
  exactly what happened on 2026-09-05, and cost three worktrees a night.
- **`runner_worktree_set`** takes `key value` PAIRS, because every caller sets a
  status and the comment explaining it: sent separately, a failure between them
  leaves the board carrying a new status with the previous line under it. The
  keys the fleet uses are `workspace-status` and `comment`. Silent on success,
  the runtime's own words on failure, **on stdout by convention** — both callers
  merge the streams, so unlike `create` this one is not load-bearing; see "which
  stream" above. Like `create`, it relays **stderr first** within the three-line
  bound, so a runtime that prints an error object on stdout cannot push the real
  reason off the end. Any non-zero means the card was not updated; `2` specifically means the CALLER passed something that is not a pair
  list, which is a bug in the caller rather than a statement about the runtime —
  an odd argument count is refused rather than rounded down, because a dropped
  key is a board update that silently did less than it was asked for.
- **`runner_worktree_remove`** returns `0` only if the worktree is REALLY gone,
  `1` if the runner answered and refused, `2` if it never answered. The caller
  acts on the difference: a refusal is a decision about this worktree and is not
  worth retrying, while a deadline is the runtime restarting and says nothing
  about the worktree at all. **A runtime that cannot be reached at all is `2`,
  not `1`** — reporting "the app is not running" as a decision about the worktree
  parks a slot that only needed retrying, which is design note 2 inverted inside
  the function that exists to honour it. It must not run the runner's own
  teardown hooks — see the comment on the Orca implementation for what that cost
  (armaatus/rommsync-nx#163).

  Its deadline is the caller's second argument, not `runner_set_deadline`'s: the
  dispatcher tunes this one per removal through `AUTOFLEET_RM_DEADLINE`, and a
  removal is not a call anyone polls. Same exemption as create, for the same
  reason.

### The build

```sh
runner_build_start <path> <issue>
runner_build_state <path>
runner_build_stop <path>
```

**The build command is not a driver's.** `lib.sh`'s `fleet_build_command_line`
composes one `claude -p` line and both drivers run exactly it; a driver decides
only WHERE it runs. The headless driver forks it into the background of the
dispatcher, the app-backed one hands it to a terminal tab so a person can watch
it, and neither is allowed to assemble its own flags — a second copy of that
line is how "both drivers do the same thing" stops being true without anything
going red.

- **`runner_build_start`** reads `prompt` and `system.md` out of
  `$(fleet_build_dir <issue>)`, which the dispatcher wrote, and returns as soon
  as the build is running rather than waiting for it. A build that is **already
  running is success, not an error**: the dispatcher calls this to make sure one
  is up, and two `claude -p` in one worktree is two agents editing one tree. It
  must call `fleet_build_started` before it starts anything — that is what
  clears the previous run's verdict, writes the path index `runner_build_state`
  reads and stamps the run's start for the wall clock below; a driver that skips
  it reports a build that has just started as already finished, and one the
  clock can never end.
- **`runner_build_state`** prints `running` or `exited <rc>`, and is non-zero
  when there is no build here to describe. That third answer is not decoration:
  "I could not tell" and "it has stopped" lead the dispatcher to opposite
  actions, and conflating them comments on an issue to say a running build gave
  up. `fleet_build_state_of` in `lib.sh` answers it for both drivers from the
  files the command line itself writes, so a driver that has nothing to add
  delegates to it.
- **`runner_build_stop`** ends the build in one worktree. Silent and idempotent:
  every caller reaches it on a path where the worktree is going away regardless,
  and a driver that reported "there was none" would be describing the ordinary
  case. 0 means **nothing of that build is still running**; a build that was
  running and still is answers **non-zero, with the surviving pid on stdout**,
  and `fleet.sh stop --now` names that pid rather than claiming a stop
  (armaatus/autofleet#163: it printed `stopped the build for #N` over a
  `claude -p` that was still there, which is the one line a person reads to
  decide whether they have to go and kill something by hand). A driver that
  cannot tell answers 0 — the app-backed one closes a terminal tab and has no pid
  to check, so the old contract is unchanged for it. It must stop **the whole
  process group** where it forked one — `AUTOFLEET_BUILD_CMD` is a wrapper seam,
  so the process holding the credentials is routinely a child of what was
  forked, and signalling the direct child leaves a full-budget run nobody is
  counting. And a driver whose handle on the build is a pid it recorded must be
  able to RECOGNISE that pid later: the headless one records the `bash -c`
  running the build line, whose command line names the build directory, so that
  the check guarding against a reused pid number cannot answer no for every
  build the dispatcher ever started. A driver that killed a build **records the
  kill** with `fleet_build_mark_stopped`, because the command line writes its own
  `rc` last and a killed one never reaches that line: without the record the
  state reader has a worktree, no `rc` and no pid and answers `running` forever.
  The marker rather than an `rc` of 143 the driver made up — the reader renders
  it as `exited 143` either way, but `build_exited` has to be able to tell a stop
  the fleet asked for from a build that died at its budget, and only the latter
  earns a comment on the issue. A stop of an issue the fleet still owns is
  recorded as given up on locally (`build_stopped` in fleet.sh), so the slot is
  released and `fleet.sh retry N` brings it back; a stop the reaper performed
  gets nothing, since the reaper's own line is the record. On the failure
  branch the driver **keeps its handle**: the headless one leaves the pid file
  in place for a pid it has just confirmed alive and its own, so the next stop
  reaches the same process instead of finding no pid and printing `stopped`.

**Six functions left this contract** with armaatus/autofleet#151:
`runner_agent_states`, `runner_agent_terminals`, `runner_agent_terminal`,
`runner_terminal_draft`, `runner_terminal_send`, `runner_terminal_enter` and
`runner_terminal_interrupt`. Every one of them existed to drive an interactive
session — read whether a tab was working or waiting, type a prompt into a
composer, press Return, interrupt it — and a `claude -p` run has none of those
questions. A driver is no longer required to have a terminal at all.

Nothing in this contract is provided by `lib.sh` any more.
`runner_agent_terminal` was — a filter over a driver's machine-wide terminal
listing, defined above the line that sources the driver so that a runtime which
could answer it directly still won — and it went with the listing it filtered.

## Writing a second driver

`tests/test_fleet.sh runner_stub` is the worked example and the regression
guard: it defines the whole contract over a handful of text files, points
`AUTOFLEET_RUNNER` at them, and then drives the dispatcher, the reap and
`issue-command.sh` through it — asserting at
the end that the `orca` CLI was never asked anything, with an `orca` planted on
`PATH` that records anything reaching for it. A driver that satisfies that phase
satisfies the fleet.

`scripts/fleet/runner/headless.sh` is the second driver, and it is the default.
It needs `git`, `gh` and the build command and nothing else, so the fleet runs
on any machine rather than on one desk. The one thing it has to answer that the
app-backed driver gets for free:

- **A linked issue.** Orca records the issue on the worktree object. Git has
  nowhere to put it, so the headless driver keeps the mapping itself — one file
  per worktree under `$AUTOFLEET_DIR/headless/links/`, named by the path with
  `/` folded to `%`. Not `git config --worktree`, which needs
  `extensions.worktreeConfig` and would be this driver turning a repository-wide
  flag on in somebody else's repo; and not a file inside the worktree, because
  `self-review.sh` refuses a dirty tree.

## What autofleet still assumes about the runtime

Named here rather than left to be discovered:

- One worktree, one directory on this machine, at a path the dispatcher can
  `stat`. `runner_worktree_remove` is judged on that directory being gone.
- The build is a command that ENDS. `fleet_build_command_line` bounds it with
  `--max-turns` and `--max-budget-usd`, and the dispatcher's poll carries a
  third bound those two cannot be: past `AUTOFLEET_BUILD_TIMEOUT` seconds — two
  hours by default — a build that has still written no `rc` is stopped through
  `runner_build_stop` and lands in the state a turns or budget exhaustion lands
  in, so `fleet.sh status` shows it under "gave up on" and `retry N` starts it
  again. Turns and dollars only end a run that is still SPENDING; a wedged one
  spends nothing, and on a runtime whose agent cannot be given a budget at all
  the wall clock is the only thing that ends a build.

  Two things it asks of a driver, both already in the contract above. Its stop
  is `runner_build_stop`, so a driver with no pid to signal is stopped the way
  it is started; and it believes that call's answer — a non-zero return names a
  surviving pid in `fleet.log` and the build is left alone and tried again next
  poll, rather than being written off while it still holds its worktree.
- The worktree's setup and archive hooks are wired somewhere. `orca.yaml` is
  where the app-backed driver does it; the headless driver has no hook
  mechanism at all, and `setup.sh` runs from `runner_worktree_create`'s caller
  rather than from the runtime. The scripts themselves (`setup.sh`,
  `archive.sh`, `issue-command.sh`) are runner-agnostic and are reused as they
  are.
- ...with one thing a driver author has to know about `setup.sh`. It closes by
  naming the command that starts an agent, because a worktree a PERSON opened
  has nothing behind it that will (#156). `launch` sets
  `AUTOFLEET_DISPATCHER_LAUNCH` on the call it makes itself, which suppresses
  that stanza. A driver that ALSO wires `setup.sh` into its runtime's
  worktree-creation hook — which is what `orca.yaml` does — gets a second,
  unmarked run of it per dispatcher launch, and that run prints the stanza in a
  worktree a build is about to start in. The wording is conditional so it stays
  true there; a runtime that can set the variable on its own hook should.
