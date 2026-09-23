#!/usr/bin/env bash
# The fleet: turn a list of issues -- or the whole backlog -- into merged PRs.
#
# Everything below this line is deterministic shell. No model runs here. The
# dispatcher's whole job is to decide WHICH issue gets a worktree and WHEN, and
# that decision has to be readable, interruptible and cheap to run for hours.
# The thinking happens inside the worktrees it opens.
#
#   ./scripts/fleet/fleet.sh run 11 12 13      # work exactly these, then hand off
#   ./scripts/fleet/fleet.sh run --auto        # keep taking `ready` issues
#   ./scripts/fleet/fleet.sh run --auto --until 08:00 --max-prs 5
#   ./scripts/fleet/fleet.sh status            # what is running, what is next
#   ./scripts/fleet/fleet.sh stop [--now]      # see "Stopping"
#   ./scripts/fleet/fleet.sh resume
#
# Run it in a terminal the runner opens in the main worktree, so the dispatcher
# is as visible as the work it starts. `fleet.sh` with no command prints the
# exact line for the configured runner -- it is the one instruction here that
# cannot be written runner-agnostically, so the driver supplies it.
#
# ## What it picks
#
# Anything `ready` and not already in flight, ordered by how many open issues
# name it in a `Blocked by #N` line. The work that frees the most other work goes
# first, which is the fastest way to turn a mostly-blocked backlog into a wide
# one. Milestones do not order it: `ready` already means every blocker is closed,
# and a milestone number is not a claim about what can be built now.
#
# Ahead of all of that: anything labelled `priority`. That is the one ordering a
# person states rather than derives, and it exists because the blocker graph
# cannot express "this one first" -- the only way to say it before was to file
# fake dependencies on issues that did not have them. It reorders the ready list
# and nothing else: a `blocked` or `needs-human-step` issue is no more startable
# for carrying it, and a foundation issue that is holding still holds. It can
# DELAY one, and the cost is bigger than "the foundation issue starts later".
# The scan BREAKS on the first foundation issue once anything is in flight, so
# every ready issue behind it in the list is skipped for that pass too --
# including issues that have nothing to do with it and are not `blocked`.
#
# Worked through, because this is the number a maintainer needs before applying
# the label. Ready list `[#151 priority, #F foundation, #A, #B]`, nothing in
# flight: pass 1 launches #151; pass 2 reaches #F, sees `live=1`, breaks, and #A
# and #B are never considered. The fleet runs at ONE worktree for #151's whole
# time-box, then at one worktree again while #F lands alone.
#
# Without the label the same backlog -- all four issues; #151 does not vanish
# when you take its label off -- sorts `[#A, #B, #151, #F]` and fills THREE,
# with #F waiting on them. An earlier version of this said `[#A, #B, #F]` fills
# three, which is wrong twice over: it drops #151, and that list fills two,
# because the scan breaks at #F with `live=2` for the very reason this passage
# exists to explain. Found by the independent review, which noted the number is
# the one the docs tell a maintainer to decide on.
#
# THAT ORDER STIPULATES A FOUNDATION ISSUE THAT FREES NOTHING, and that is the
# atypical one -- so read the comparison as a bound, not as the usual case. The
# second key is `-blocks`, so a `#F` named in even one open `Blocked by #N` line
# outranks three issues that free nothing and sorts FIRST, unlabelled. Run that
# through the same code: `[#F, #A, #B, #151]` launches #F on pass 1 and breaks on
# `is_foundation`, so the fleet is at ONE worktree while #F lands alone and fills
# three only afterwards. Labelled, the same backlog is one worktree for #151,
# then one for #F, then two.
#
# So: against a foundation issue that anything is waiting on -- the kind the
# "lands alone" rule exists for -- the label costs ONE EXTRA SOLO TIME-BOX, and
# the three-versus-one gap is the worst case, reached only when #F frees nothing.
# Either way it is time-boxes at one worktree and not a reordering, which is the
# thing to decide about. Found by the independent review, which noted the
# stipulation was doing the work the arithmetic was getting credit for.
#
# An earlier version of this comment said "nothing that depends on it could have
# started either way -- those are `blocked` -- so the rule holds", which is true
# and is the wrong reassurance: the dependants are not what stalls. Found by the
# independent review, which noted this change wrote all three copies of that
# sentence.
#
# ## Stopping
#
# The stop is a FILE, not a signal, and it lives outside every worktree
# ($HOME/.autofleet). That is deliberate: a signal only reaches a process
# that is still healthy, and the case you most need a stop in is the one where
# something is not.
#
# There are TWO of them, because "start nothing new" and "let nothing out" are
# two instructions and only one of them is the agents' (#183):
#
#   DRAIN   no new worktrees. Every stop sets it, and only this dispatcher reads
#           it. An agent mid-work carries on, pushes, opens its PR and comments
#           -- which is the point: a drain WAITS for those PRs to merge, because
#           a merged PR is what releases the worktree it is waiting on. A drain
#           that also froze them waited for what it had itself forbidden, and
#           ended only when the time-box gave each worktree up, three hours at a
#           time.
#   STOP    nothing goes out: no push, no PR, no comment, from any agent,
#           whether or not it has read the news. `stop --now` and `stop --all`
#           set it, and `.claude/hooks/guard.py`, `await-review.sh`,
#           `review-status.sh` and `resolve-thread.sh` are what make it hold
#           without cooperation.
#
# ## What it will not do
#
# It does not merge -- `merge-gate` and GitHub's auto-merge do that. It does not
# touch a worktree it did not create. And it removes one only when nothing goes
# with it: either that worktree's PR merged and the worktree is clean with
# nothing unpushed, or its issue can no longer produce a merged PR at all and the
# worktree is clean and holds no commit that is not already in origin/main. A
# removal that is REFUSED changes nothing either -- the stack comes down after
# the worktree is gone, never before.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/fleet/lib.sh

# The state dir, the stop file and the owned-worktree registry come from
# lib.sh: `await-review.sh` and `guard.py` read the same paths, and a stop only
# some of them can see is not a stop.
STATE_DIR="$FLEET_DIR"
STOP_FILE="$FLEET_STOP"
DRAIN_FILE="$FLEET_DRAIN"
OWNED_DIR="$FLEET_OWNED"
# WHEN THIS ATTEMPT STARTED, which is no longer a deadline and is still worth
# recording: `status` prints it, and it is the one thing that says whether a
# build that has been running since Tuesday is progress or a wedge. The
# wall-clock time-box that used to read it is gone -- turns and dollars bound a
# run now -- so nothing ACTS on this number.
STARTED_DIR="$STATE_DIR/started"
# The `Closes #N` and `Blocked by #N` patterns, shared with merge_gate.py so the
# dispatcher, the gate and GitHub cannot read the same body three ways. It sits
# under .github/scripts/ because merge-gate.yml sparse-checks out that directory
# alone; see the module's docstring.
#
# Set on each `python3` below rather than exported: the dispatcher runs for
# hours and shells out to gh, git, the Orca CLI and python constantly, and an
# exported PYTHONPATH would put this directory at the front of sys.path for
# every one of them. The first module added here whose name shadowed a stdlib
# one would then quietly change what they all import.
ISSUE_REFS="$REPO_ROOT/.github/scripts"
# One pass's worth of answers, and no longer. Emptied at the top of every poll
# by `cmd_run`, and NOWHERE ELSE -- not at startup, not by any other command.
#
# The poll cache belongs to whoever is POLLING, and until #35 every command
# emptied it at SOURCE TIME. So `status`, `stop`, `retry` and `resume` each
# deleted it out from under a live dispatcher mid-pass -- and `interrupted-$n`
# is the only thing stopping `reap_abandoned` re-interrupting an agent that
# `enforce_timebox` interrupted earlier in that same pass. docs/WORKFLOW.md
# tells you to run `status` in a loop until it says idle, so the way to hit it
# was to watch the fleet. It also dropped `issue-$n`, making every watcher
# re-issue the `gh issue view` that `poll_issue` exists to avoid.
#
# THERE IS NO CALL HERE AT ALL, which is #35's Acceptance in one line:
# "fleet.sh status leaves $STATE_DIR byte-identical". The first fix kept the
# source-time call and made it conditional on the dispatcher being dead, which
# (a) still wrote to $STATE_DIR on the ordinary idle path, and (b) had to get
# `dispatcher_alive`'s THREE answers right -- `||` collapses 2, "alive but ps
# would not say", into "no dispatcher", so on a host where ps cannot answer
# every `status` in the watch loop still wiped a live dispatcher's cache. The
# bug unfixed, in an environment this repo keeps three phases for.
#
# Deleting it removes both problems, and costs nothing: `cmd_run` empties the
# cache at the top of every pass before anything reads it, and NOTHING outSIDE
# `cmd_run` reads it. `poll_issue` and `foundation_in_flight` are called only
# from the poll body, from `launch`, and from the watchers the poll body calls.
# So a cache left behind by a dispatcher that died is never read by anyone --
# the staleness the conditional was protecting against is unobservable. Found by
# the independent review, which proposed exactly this.
POLL_CACHE="$STATE_DIR/poll-cache"
forget_poll_answers() {
  rm -rf "$POLL_CACHE"; mkdir -p "$POLL_CACHE"
  # ...and the out-of-poll memo with it. See $OPEN_PR_MEMO below.
  OPEN_PR_MEMO=""; OPEN_PR_MEMO_STATE=""
  # ...and the sweeps' shared answer about each PR's state. See $PR_STATE_MEMO.
  # BOTH halves: the memo AND the variable the answer comes back in. Resetting
  # only the memo left a stale $PR_STATE_ANSWER outliving its pass, safe solely
  # because the one read follows the write that fills it -- which is the exact
  # coupling `pr_state_once`'s own header warns against. Found by
  # `/mattpocock-skills:code-review`.
  PR_STATE_MEMO=" "; PR_STATE_ANSWER=""
}

# THE POLL CACHE IS THE POLL'S, and this is the line that says so rather than
# leaving it to the call graph. `cmd_run` sets it once per pass; every other
# entry point -- `status`, `stop`, `reap`, a test sourcing one function -- leaves
# it false, and `live_worktrees` then reads the runner directly and writes
# nothing.
#
# #35's acceptance -- "fleet.sh status leaves $STATE_DIR byte-identical" -- held
# until now because no helper `cmd_status` happened to call wrote to the cache.
# `cmd_status` calls `in_flight` for its `next up` table, and `in_flight` reads
# the worktree list, so caching that list made `status` a writer. The damage is
# not the write: it is that a `status` in the watch loop docs/WORKFLOW.md
# prescribes would hand the dispatcher beside it a list read at another moment,
# and a stale list is a duplicate worktree or a missed foundation hold.
# armaatus/autofleet#30.
IN_POLL=false
# OUTSIDE a poll there is no $POLL_CACHE to read -- that is the $IN_POLL gate's
# whole point -- and `open_pr_listing` therefore went to `gh` afresh for every
# caller. `cmd_status` has one caller PER READY ROW (`in_flight`), so one
# `status` screen against this repository's own queue was ~54 `gh pr list` calls:
# the same slope armaatus/autofleet#69 removed from the dispatcher, left standing
# in the command a person actually types.
#
# A VARIABLE and not a file, which is what keeps armaatus/autofleet#35's
# acceptance ("`fleet.sh status` leaves $STATE_DIR byte-identical") true. It
# lives for one process, which is exactly right for `status`; a dispatcher never
# reads it, because `poll_cache_open` is true there and the file cache answers
# first. `forget_poll_answers` clears it anyway, so a long-lived process that
# ever did fall through here cannot inherit a stale listing across passes.
#
# Three states, because two would make "could not tell" indistinguishable from
# "not asked yet" -- and that is the one distinction every cache in this file
# exists to keep. Found by the local review.
# NOT a fifth caller of `poll_cache_get`/`_fail`/`_put`, and the reason is the
# one thing those three cannot do: they are FILES. `cmd_status` may not leave a
# byte behind in $STATE_DIR (armaatus/autofleet#35), which is the whole reason
# this path exists, so the trio is unusable here by construction rather than by
# preference. The three states are the same three, deliberately, so that a
# reader who knows one knows the other. Raised by the local review.
OPEN_PR_MEMO=""
OPEN_PR_MEMO_STATE=""
# ...read through a test, never as a bare `$IN_POLL`. Bash runs a simple command
# that expands to no words with status 0, so an EMPTY $IN_POLL would read as
# true and turn the caching on -- fail-open, in the one function whose every
# comment is about failing closed. Nothing in this tree can reach that (the
# assignment above is at source time), but `forget_worktree_answers` carries a
# `${POLL_CACHE:-}` guard for the armaatus/rommsync-nx extraction that may bring
# functions without the top-level assignments, and this is the other half of
# exactly that case. Found by the independent review.
#
# Named for the gate rather than for the flag because `tests/test_fleet.sh` has
# a helper called `in_poll`, and every phase sources this file inside it.
poll_cache_open() { [ "${IN_POLL:-false}" = true ]; }
LOG="$STATE_DIR/fleet.log"
PIDFILE="$STATE_DIR/fleet.pid"

# Three by default, because the ceiling is not machine capacity -- it is how
# many streams one person can review properly. AUTOFLEET_MAX in
# .autofleet/config moves it.
MAX_WORKTREES="${AUTOFLEET_MAX:-3}"
POLL_SECONDS="${AUTOFLEET_POLL:-60}"
# WHAT AN ISSUE MAY SPEND, which is turns and dollars now rather than hours.
# The wall-clock boxes are gone with the sessions they policed: a `claude -p`
# run ends at whichever of these it reaches, so nothing has to be interrupted
# and the dispatcher reads an exit instead of enforcing one.
BUILD_MAX_TURNS="${AUTOFLEET_BUILD_MAX_TURNS:-400}"
BUILD_MAX_BUDGET_USD="${AUTOFLEET_BUILD_MAX_BUDGET_USD:-25}"
# How many runs one worktree gets before the fleet stops resuming it. See
# `build_exited`: without a bound, a run that ends the instant it starts is an
# infinite resume loop that spends the account one session at a time.
BUILD_MAX_RUNS="${AUTOFLEET_BUILD_MAX_RUNS:-3}"
FOUNDATION_LABEL="${AUTOFLEET_FOUNDATION_LABEL:-foundation}"
# The human's thumb on the queue -- see "What it picks" above. Read in exactly one
# place that can change what STARTS (`ready_issues`, where it only sorts), plus
# `cmd_status`, which reports. That split is the point: an override that also
# decided what may start would be a second way past `blocked`, and this fleet has
# one set of rules about what is startable.
PRIORITY_LABEL="${AUTOFLEET_PRIORITY_LABEL:-priority}"
# An issue whose LAST step is outward, irreversible and the maintainer's --
# tagging a release, touching real hardware, signing something. The fleet may not
# finish one, so it does not start one, does not read an agent waiting on one as
# a stall, does not time-box it, and releases its worktree once there is nothing
# left in one (armaatus/rommsync-nx#139 needed a write guard.py refuses from a
# fleet worktree, so its agent correctly produced no PR and reap_merged would
# have waited for one forever). See docs/WORKFLOW.md, "an issue whose last step
# is a person's".
HUMAN_STEP_LABEL="${AUTOFLEET_HUMAN_STEP_LABEL:-needs-human-step}"
# unblock.yml's label, derived from the `Blocked by #N` lines below each issue's
# marker. The fleet only ever READS it, and it reads it for one thing: a
# worktree open on an issue that has SINCE gone blocked will never produce a
# merged PR, so it is holding a slot for nothing. On rommsync-nx, #148 went
# blocked with its worktree open and kept three other issues queued behind work
# that could not start.
BLOCKED_LABEL="${AUTOFLEET_BLOCKED_LABEL:-blocked}"
# poll_issue answers two questions in one string; issue_state_in and
# issue_labels_in split it here.
ANSWER_SEP="$(printf '\t')"

# The poll cache is emptied further down, and only when nobody is using it: see
# the note above the dispatch at the end of this file. armaatus/autofleet#35.

say() { printf '%s  %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG"; }
# THE SAME LINE, ON STDERR, for the one caller whose stdout is captured.
#
# `count_parked_owned` returns its count on stdout and the poll reads it through
# a command substitution, so a `say` underneath it lands inside `$parked` and
# breaks the arithmetic on the next line -- which is why that callsite is
# `parked_for_person "$n" say >/dev/null`. The three-line "could not read the
# agent states ... a drain will not end while that stays true" warning therefore
# reached `fleet.log` only, and never the terminal running `fleet.sh run --auto`,
# while every other `say` in the poll body appears on screen. That is #37's
# opening complaint -- "nothing says the drain has become unbounded" -- answered
# on the wrong channel. `$( )` captures stdout and not stderr, so this reaches
# both the log and the operator. armaatus/autofleet#71.
# ...and it IS `say`, with the pipeline's stdout sent to fd 2. Two copies of one
# `printf | tee` is two places for the timestamp format to drift.
say_err() { say "$@" >&2; }
die() { printf '%s\n' "$*" >&2; exit 1; }

# The two cases you would otherwise not learn about until morning: the fleet
# finishing, and an issue giving up. Everything else is on the runner's board.
notify() {
  command -v osascript >/dev/null 2>&1 || return 0
  osascript -e "display notification \"$(printf '%s' "$2" | sed 's/"/\\"/g')\" with title \"$AUTOFLEET_NOTIFY_TITLE\" subtitle \"$1\"" >/dev/null 2>&1 || true
}

# What every LAUNCH decision asks, and it is the drain rather than the stop: a
# hard stop sets both files, so this is true in both states and the dispatcher
# opens nothing new in either.
draining() { fleet_draining; }
# ...and what only the SCREEN asks, because the two states need different words
# on it. Nothing in here gates on this: refusing to launch under one and not the
# other is how the two would drift apart.
hard_stopped() { fleet_stopped; }
# The word for the state in force, and the file carrying it, for the two screens
# that need nothing more than the word: this one and `run`'s refusal. `status`
# and the notification say a sentence per state rather than a word, so they
# branch on `hard_stopped` themselves.
#
# Only meaningful once `draining` is true: with neither file set the state is
# empty, and the file named is the one a drain WOULD write.
stop_state() { hard_stopped && { echo stopped; return 0; }; draining && echo draining; }
stop_state_file() { hard_stopped && { printf '%s\n' "$STOP_FILE"; return 0; }; printf '%s\n' "$DRAIN_FILE"; }

# Named for what it gates rather than for the file it used to read: since the
# split it is the DRAIN that stops a launch, and `check_stop` at the call site
# read as the opposite of what it does.
check_drain() {
  draining || return 1
  say "$(stop_state) ($(stop_state_file)) -- not starting anything new"
  return 0
}

# ------------------------------------------------------------- the runner ---
# `cost` FIRST, because it is the one subcommand that needs no runner: it reads
# transcripts off disk and opens no worktree, no terminal and no board. The
# probe below is at SOURCE time and `die`s, so a `cost` arm in the case at the
# foot of this file was unreachable on any machine without a runtime -- a CI
# runner, a laptop with the app shut, the very machines somebody asks "what did
# last night cost" from. It answered "the orca runner is not usable here, so
# there is nothing to dispatch with", which is true and has nothing to do with
# the question. Found by CI, which is exactly such a machine; the suite was
# green on the laptop where the runtime answers.
#
# Guarded on BASH_SOURCE so sourcing this file for tests still defines
# everything below rather than exec-ing away mid-source. This is also why
# `fleet_require_runner` is below rather than beside the `. lib.sh` at the top:
# `cost` calls no `runner_*`, so a driver that is not there is not its problem,
# and it has to be able to exec away before anything requires one.
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = cost ]; then
  # A separate script rather than a cmd_* in here: it knows nothing about
  # dispatching, and this file is 3.5k lines already. `exec` so its exit status
  # is the one the caller sees.
  shift
  exec "$REPO_ROOT/scripts/fleet/cost.sh" "$@"
fi

# BELOW the cost dispatch, not above it. Up at the top this ran for `cost` too,
# so a reporting command created three state directories under ~/.autofleet on a
# machine that had never run the fleet -- and then reported that the fleet had
# run nothing. "It reads transcripts off disk and opens no worktree" is how the
# dispatch above describes itself. Nothing between here and there reads these
# directories at source time. Found by the independent review.

# A driver that is not THERE, before anything else: without one the probe below
# is `command not found`, and `|| die` turned rc 127 into "the runner is not
# usable here" -- true of a missing file and the wrong sentence to act on.
# lib.sh has already named the file it looked for.
#
# Above the `mkdir` for the reason the `mkdir` is below the `cost` dispatch: a
# command about to be refused should not create state directories first. Found
# by the local review.
fleet_require_runner

# At SOURCE time, not at first use: everything below assumes a runner that
# answers, and a dispatcher that discovers otherwise three functions deep
# reports the consequence instead of the cause. The driver has already said why
# on stderr; this is the consequence.
runner_available || die "the $AUTOFLEET_RUNNER runner is not usable here, so there is nothing to dispatch with"

# BELOW BOTH REFUSALS, which is the rationale the guard above was given and the
# probe below it was not: a command about to be refused should not create state
# directories first. `fleet.sh status` on a machine whose app is not running was
# making three directories under ~/.autofleet and then declining to do
# anything. Found by the self-review.
mkdir -p "$OWNED_DIR" "$STARTED_DIR"

# The runner's board is the status surface: `in-progress` while it builds,
# `in-review` once the PR is up (the agent sets that itself), `completed` on
# merge. The comment is the one line the card shows.
#
# A failed update is SAID, not swallowed. WORKFLOW.md calls the board the status
# surface, so a card that did not update is a board showing something that is not
# true -- and the `|| true` this replaces meant the dispatcher reported nothing
# wrong while it happened. When the runner's CLI broke on 2026-09-05 (the Orca
# driver's orca_cli_resolve records it) every card in the fleet would have
# frozen in silence. Still non-fatal: the board is a display, and a display that cannot be
# written is not a reason to stop dispatching work.
#
# `key value` pairs, which is the driver's shape: a status and the comment
# explaining it go up together or the board carries one without the other.
card() {
  local path="$1" out rc; shift
  out="$(mktemp)"
  runner_worktree_set "$path" "$@" >"$out" 2>&1
  rc=$?
  if [ "$rc" != 0 ]; then
    say "  board update FAILED (rc $rc) for $path: $*"
    # The runner's own words -- the driver caps them, this only relays them.
    # They are the difference between "the app is not running" and "that
    # worktree is gone", and both look like silence.
    while IFS= read -r line; do
      [ -n "$line" ] && say "    $line"
    done <"$out"
  fi
  rm -f "$out"
  return 0
}

# Stop the build in one worktree, if there is one running.
#
# WHAT THIS REPLACED is the whole reason armaatus/autofleet#151 exists. There
# used to be five functions here -- type at the agent, ask it for a handoff
# note, reset its context when the PR opened, recycle its context every 45
# minutes, interrupt it -- and every one of them existed because an interactive
# session cannot be allowed to end. A `claude -p` build ends by itself, and the
# branch and the PR are the state, so there is nothing to ask it to write down
# and nothing to clear.
#
# Both callers are about to take something away from the build -- `reap_abandoned`
# its whole directory, `cmd_stop --now` the rest of its run -- and a build still
# writing into a directory that is being removed is how a worktree removal
# fails halfway. The driver signals the process GROUP: the build command is a
# wrapper seam, so the process holding the credentials is routinely a child of
# what was forked.
#
# Silent about a worktree with no build in it -- the ordinary case on most polls
# -- and a caller that had to tell "no build" from "could not tell" would be the
# `*_blind` distinction all over again, except there is no runtime here to be
# blind to. `runner_build_state` is where that distinction lives, and it is the
# one the dispatcher reads.
#
# NOT ALWAYS 0, though, which is what it used to be. The one answer this does
# carry is "the build was running and it still is": the driver returns non-zero
# and prints the surviving pid, and `cmd_stop` prints that instead of claiming a
# stop that did not happen (#163). A pass-through, because the driver is the
# only thing that can tell.
stop_build_in() {
  runner_build_stop "$1"
}

# --------------------------------------------------------------- the state ---
own() {
  printf '%s\n' "$2" >"$OWNED_DIR/$1"; date +%s >"$STARTED_DIR/$1"
  # THERE IS NO SECOND REGISTRY any more. `$FLEET_DIR/ran` held every path an
  # issue had ever run in, because `cost.sh` found what an issue spent by
  # slugging those paths into the agent CLI's transcript root and the record had
  # to outlive the worktree. The report reads `$FLEET_DIR/builds/<issue>/` now,
  # which is keyed on the issue and outlives everything, so the registry became
  # a store nothing read -- with three comments across two files still claiming
  # the report was built on it. Found by `/mattpocock-skills:code-review`.
  clear_issue_markers "$1"
}
owned_path()   { cat "$OWNED_DIR/$1" 2>/dev/null; }

# Every per-issue marker the dispatcher writes, in one list, because the bug
# this replaces was exactly one list drifting from another.
#
# All of these throttle a message to once per event. A marker that outlives the
# worktree that wrote it therefore does not misreport anything -- it SILENCES
# the next worktree for the same issue, which is worse, because the thing that
# goes missing is the line saying why. `own()` clears them for that reason: a
# fresh worktree starts with nothing already said on its behalf, and so does
# `disown_issue`, which is what stopped `stalled-` leaking one small file per
# issue that ever stalled. That is the root fix; the two exits in
# enforce_timebox tidying up after themselves is the belt.
#
# The `*-blind-` family is swept as a FAMILY rather than named one at a time.
# Every one of them means the same thing -- some lookup could not answer for this
# issue, said once -- and they are the markers most likely to be renamed, because
# they are named after whatever was doing the looking. `runner-blind-` was
# `orca-blind-` until the runner seam landed, and a fleet upgraded mid-flight has
# the old name on disk with nothing left that clears it. The glob also keeps a
# runner's name from having to appear here at all (hard rule 4).
clear_issue_markers() {
  rm -f "$STATE_DIR/queue-labels-$1" \
        "$STATE_DIR/unreachable-$1" "$STATE_DIR/human-step-$1" \
        "$STATE_DIR/held-$1" "$STATE_DIR/stuck-$1" \
        "$STATE_DIR/warned-$1" "$STATE_DIR/parked-since-$1" \
        "$STATE_DIR/build-done-$1" "$STATE_DIR/exit-blind-$1"
  # ...and the two park reasons the names above do not already cover. The
  # `*-blind-` glob below takes `git-blind-` and `merge-blind-`.
  rm -f "$STATE_DIR/merge-held-$1"
  # ONLY the `*` is unquoted. $STATE_DIR is `${AUTOFLEET_DIR:-$HOME/.autofleet}`,
  # both user-supplied paths: leaving the whole word bare word-splits a directory
  # with a space in it into two operands that match nothing, and the markers are
  # then never cleared -- silently, which is the failure this list exists to
  # prevent. Found by the independent review.
  rm -f "$STATE_DIR"/*-blind-"$1"
}

# `gaveup-` is the one per-issue marker deliberately NOT in that list, and the
# exception is load-bearing rather than an oversight. It records that this
# dispatcher stopped an agent at the time-box, and reap_abandoned then RELEASES
# that worktree -- which calls disown_issue. Clearing the record there would put
# the issue straight back at the front of a queue that has just spent a whole
# budget on it, once per cycle forever. It outlives the worktree on purpose, and only
# `fleet.sh retry N` clears it -- not a restart, because a crash and a reboot are
# not decisions about an issue.
gave_up_on()     { [ -e "$STATE_DIR/gaveup-$1" ]; }
gave_up_issues() { ls "$STATE_DIR" 2>/dev/null | sed -n 's/^gaveup-//p'; }

disown_issue() {
  rm -f "$OWNED_DIR/$1" "$STARTED_DIR/$1"
  clear_issue_markers "$1"
}

# `issue<TAB>path`, which is the driver's `path<TAB>branch<TAB>issue` with the two
# columns this dispatcher reads brought to the front and the branch dropped: a
# column no caller reads is one the next caller reads wrong. Four callers depend
# on that order -- `waiting_worktrees`, `in_flight`, `foundation_in_flight` and
# `cmd_status` -- and it is stated here because the `awk` alone does not say it.
#
# THE PARK REASONS: which one applies, and what a person is told about it.
#
# There are five, and the count and the two places that REPORT them drifted
# apart the moment there were more than three: `count_parked_owned` learned
# `held-` and `git-blind-` and the farewell list and `cmd_status` did not, so a
# worktree could be counted as waiting for a person and then never named as one
# -- present in the tally, absent from the list that says what to do about it.
# That is worse than not counting it at all. One SOURCE, three readers -- and a
# source rather than a list is the whole of what the next paragraph is about.
#
# TWO QUESTIONS, TWO SOURCES, and the call graph rather than a count, because an
# earlier version of this paragraph invented a set of callers that did not exist.
# `parked_marker` over `PARK_MARKERS` answers WHICH reason applies, and is the
# only place the five are ordered. `why_parked` answers what a person is TOLD
# about one, and prints nothing when the issue is not parked.
#
#   how_to_release      asks `parked_marker`: its line differs by reason, and it
#                       never needs the sentence.
#   parked_for_person   asks BOTH -- the marker for the agent gate, `why_parked`
#                       for the sentence it returns.
#   the count, `status`
#   and the farewell    ask `parked_for_person`, so they get the sentence and
#                       never touch either of the two directly. These are the
#                       "three readers" the paragraph above means; the number is
#                       not the number of callers of `parked_marker`.
#
# THERE IS NO LIST OF THE REASON SENTENCES, and that is deliberate. `PARK_MARKERS`
# below is not one: it holds the five marker NAMES, has a reader in
# `parked_marker`, and is load-bearing where a list of the sentences would not be.
# A list of the sentences was added here once, under a comment promising "one
# list, three readers", and it had NO reader:
# `why_parked`, `how_to_release` and the phase each enumerated the five by hand,
# so the list was the drift it was added to prevent, one indirection later. An
# attempt to make it load-bearing through `clear_issue_markers` failed too --
# that function's existing `*-blind-` glob and explicit names already cover all
# five, so removing a reason from the list changed nothing.
# Found by the independent review, twice: once for the list, and once for the
# half of this header that still called `why_parked` the single source after
# `parked_marker` had become it.

# WHICH MARKER PARKS $1, as the marker's own name, in precedence order. One
# list, and it is the only place the five are ordered. The order is deliberate
# where two can coexist: a refused removal is the one a person acts on, so it
# wins over "git could not say".
#
# Split out from `why_parked` because TWO places need to know which reason this
# is, and they want different things with it: `how_to_release`, whose line
# differs for the fifth reason and which never prints the sentence at all, and
# the agent gate inside `parked_for_person`, which applies to four of the five
# and does want the sentence beside it.
# Both used to infer it -- the gate from the prose `why_parked` printed, then
# from the ABSENCE of `stuck-`, which is correct only while `stuck-` is the first
# test here and is a coupling nothing pins. A sixth reason added above it would
# have silently ungated all five. armaatus/autofleet#71, found by
# `/mattpocock-skills:code-review` of the change that keyed on the absence.
PARK_MARKERS="stuck merge-held held merge-blind git-blind"
parked_marker() {
  local n="$1" m
  for m in $PARK_MARKERS; do
    [ -e "$STATE_DIR/$m-$n" ] && { printf '%s\n' "$m"; return 0; }
  done
  return 1
}

# $2 is the marker, when the caller already has it: `parked_for_person` looks it
# up to decide the gate and then wanted the sentence for the same one, and three
# subshells per worktree per pass for a value the caller is holding is the slope
# armaatus/autofleet#69 is about. Optional because THE TESTS ask by number alone;
# `cmd_status` used to and does not any more -- it goes through
# `parked_for_person` like the other two readers, and `parked_for_person` always
# has the marker to pass. Found by `/mattpocock-skills:code-review`.
why_parked() {
  case "${2:-$(parked_marker "$1")}" in
    stuck)       printf 'its removal was refused\n' ;;
    merge-held)  printf 'merged, and it holds uncommitted work\n' ;;
    held)        printf 'it holds uncommitted work\n' ;;
    merge-blind) printf 'merged, and git could not say what it holds\n' ;;
    git-blind)   printf 'git could not say what it holds\n' ;;
    *) return 1 ;;
  esac
}

# ...and what to DO about it, which is not the same line for all five.
#
# `BY_HAND_REMOVAL` is `git worktree remove --force`, and it was printed from one
# place -- `park_worktree`, on the one path where the dispatcher had already
# established that nothing goes with the worktree. Printed for every reason it
# tells a person to discard exactly what the line above it says is in there:
# "it holds uncommitted work", then `--force`. #37's Design notes are explicit --
# "what must NOT happen is releasing the worktree to make the loop terminate:
# the whole reason it is parked is that removing it would destroy something" --
# and the dispatcher kept that promise itself while breaking it through the
# operator, two lines apart. Found by the independent review.
# NO `$3` FOR THE MARKER, unlike `why_parked` above: every callsite reaches this
# through `parked_for_person`, which has already returned, so nothing is holding
# one to pass and the fast path would be unreachable. #71.
how_to_release() {
  local n="$1" path="$2" marker
  marker="$(parked_marker "$n")"
  # By the marker's NAME, like the gate -- not by testing a file this function
  # would then be the second place to spell. #71.
  if [ "$marker" = stuck ]; then
    printf "$BY_HAND_REMOVAL" "$path"
    return 0
  fi
  # Everything else is "there is something in there", or "git would not say
  # whether there is". Look first; the removal is the operator's call afterwards.
  printf 'git -C %s status --short   # then commit, move or discard what is there' "$path"
}

# How many OWNED worktrees are waiting for a person rather than for an agent.
#
# EVERY reason a worktree waits for a person, and there are FIVE. `stuck-`
# is a refused removal; `merge-held-` a merged worktree still holding
# uncommitted work; `merge-blind-` one whose git could not say what it holds
# -- which the change that added this counter also made permanent, since
# nothing recreates a pruned upstream. `reap_abandoned` keeps two more, and
# an earlier version of this comment asserted they did not exist: `held-`,
# when the issue is closed, blocked or timed out and the worktree holds
# uncommitted work, and `git-blind-`, when its git state could not be read.
# Neither is exotic -- an agent whose issue goes `blocked` with one commit in
# its worktree reaches `held-` -- and uncounted, `owned` never reaches 0 and
# the drain never ends. That is #37's unbounded drain through a different
# door, in the PR that closes #37. Found by the independent review.
#
# COUNTED PER ISSUE, not per marker, which is the other half of the same
# finding. One worktree can carry two of these at once: `reap_merged` writes
# `merge-blind-42` and keeps it owned, then `reap_abandoned` runs on that
# same entry in the same pass -- its own header wrongly assumes a merged
# worktree has been disowned by now -- reads `origin/main` rather than `@{u}`
# so it CAN answer, finds nothing held, and a refused removal writes
# `stuck-42` beside it. Two markers, one worktree, `parked` over-counts by
# one, and `owned` goes to 0 with another worktree mid-work: the dispatcher
# exits, its stack up under `restart: unless-stopped`. The `-lt 0` clamp is
# what made that a silent wrong answer instead of a visible one.
  # ...and only the ones that are actually OWNED. The two counts come from
  # different directories, and a stale marker with no owned entry would make
  # `owned` under-count and the dispatcher exit with a worktree still in
  # flight -- the failure the drain bound exists to prevent, inverted.
  # `disown_issue` -> `clear_issue_markers` keeps the pair together on
  # release, and `reap_merged`'s `[ -d "$path" ] || disown_issue` self-heals
  # a missing directory, so this test is what closes the remaining gap.
# Still clamped, and now it should be unreachable: `parked` counts distinct
# owned issues, so it cannot exceed `owned`. Kept because a wrong answer here
# ends the dispatcher with work in flight, and a clamp is cheaper than that.
# Is this issue's worktree waiting for a PERSON, rather than for an agent?
#
# `why_parked` answers "does it carry a keep-marker", which is not the same
# question: the two `reap_abandoned` markers are written while the agent is
# deliberately left running. `count_parked_owned` wrapped that predicate in an
# agent gate and `cmd_status` called it raw, so a worktree the counter refused
# to call parked was printed by `status` as parked -- and told a person to go
# and discard what is in a directory somebody is writing to. Same crack,
# opposite direction. One predicate now. Found by the independent review.
#
# Prints the reason, or nothing. Non-zero when it is not waiting for a person.
# The second argument is the VOICE, and it is the difference between a poll and
# a look. `say` is `tee -a "$STATE_DIR/fleet.log"` and the `ps-blind-` latch is
# per-issue dispatcher state, so a caller that does either is WRITING to
# $STATE_DIR. #35's Acceptance -- the issue this PR closes -- is "fleet.sh
# status leaves $STATE_DIR byte-identical", and `cmd_status` reached this
# function for every live worktree. WORKFLOW.md tells an operator to run
# `status` in a loop until it says idle, so the first look created the latch and
# the dispatcher's own terminal then never printed the sentence explaining why
# the drain had become unbounded -- it survived in fleet.log alone. A read-only
# command consuming a live dispatcher's said-once marker because somebody
# looked: the same shape as the bug #35 is about, through the door #37 opened.
# Only `count_parked_owned`, which runs in the poll body, passes `say`. Found by
# the independent review.
parked_for_person() {
  local n="$1" voice="${2:-quiet}" marker reason state
  marker="$(parked_marker "$n")" || return 1
  reason="$(why_parked "$n" "$marker")" || return 1
  # EVERY reason that means "there is something in there" is gated, not just the
  # two `reap_abandoned` writes. The gate used to be reached only when a `held-`
  # or `git-blind-` marker was on disk, so `merge-held-` and `merge-blind-`
  # matched the pattern, failed that test, and were counted with no agent check
  # at all -- and `reap_merged`'s own comment says why the tree is dirty there:
  # "auto-merge fires the moment the last check passes, so review fixes made
  # after it sit uncommitted here". That is an agent mid-work, in the window
  # CLAUDE.md step 6 exists for. The last worktree's PR auto-merges while its
  # agent is making review fixes, and two passes later the dispatcher signs off
  # with it still writing.
  #
  # `stuck-` is the exception and stays ungated: a refused removal is the
  # dispatcher having already ASKED and been told no, and gating it on a stale
  # marker beside it is how the drain hung two rounds ago. Found by the
  # independent review.
  #
  # KEYED ON THE MARKER, not on the sentence. This `case` used to match `$reason`
  # -- the human line `why_parked` prints -- against `*"holds uncommitted work"`
  # and `*"git could not say what it holds"`. Reword either line and the gate
  # silently stops applying: the worktree counts as waiting for a person while
  # its agent is writing, `owned` drops to 0 and the dispatcher signs off
  # mid-work. Worse, only two of the four gated reasons were pinned with a
  # working agent, so rewording the blind sentence took the check off
  # `git-blind-` and `merge-blind-` with the suite still green.
  #
  # BY NAME, and the four names are written out: that is the rule, and it does
  # not move when `parked_marker`'s precedence does. An earlier version of this
  # tested the ABSENCE of `stuck-`, which is the same answer only while `stuck-`
  # is the first entry in $PARK_MARKERS -- so a sixth reason added above it would
  # have ungated all five in silence, which is the drift this whole issue is
  # about, one function to the left again. Adding a reason now forces a decision
  # here. armaatus/autofleet#71, and found by `/mattpocock-skills:code-review`.
  case "$marker" in
    merge-held|held|merge-blind|git-blind)
      # A RUNNING BUILD IS AN AGENT AT WORK, and this is the one question that
      # replaced the machine-wide agent-state listing. The old one asked a
      # runtime to classify a terminal -- `working`, `waiting`, idle -- and the
      # classification is what `notice_stalled` was built on. `claude -p` has
      # exactly two states and neither of them is ambiguous.
      # NO BUILD RECORDED IS NOT "COULD NOT TELL". A worktree the dispatcher
      # never started a build in -- one a restart inherited, one whose build
      # directory was swept -- has nothing running in it, which is a real
      # answer and the one that lets a drain end. Read as blind it never counted
      # as parked, `owned` never reached 0, and the drain polled forever. Found
      # by the local `/code-review` pass.
      state="$(runner_build_state "$(owned_path "$n")")" || state="none"
      if [ "$state" = none ] && [ -e "$(fleet_build_dir "$n")/worktree" ]; then
        # SAID, once per pass. Taking the safe direction silently is #37's own
        # complaint -- "nothing says the drain has become unbounded". One
        # unreadable `worktree ps` is a hiccup; a persistent one means this
        # worktree never counts, `owned` never reaches 0 and the drain never
        # ends, and the operator has no way to know why. `live_worktrees`
        # already says the equivalent for its own call. Found by the independent
        # review.
        # The `*-blind-` family idiom: one marker per issue, said once, and
        # swept with the rest when the issue is released.
        # `say_err`, NOT `say`: this function's stdout is inside the poll's
        # `$(count_parked_owned)` substitution. See say_err.
        if [ "$voice" = say ] && [ ! -e "$STATE_DIR/ps-blind-$n" ]; then
          : >"$STATE_DIR/ps-blind-$n"
          say_err "  could not read the build's state, so whether #$n is still being"
          say_err "  worked in cannot be answered -- it is NOT counted as waiting for"
          say_err "  you, and a drain will not end while that stays true"
        fi
        return 1
      fi
      # Releasing the latch is the milder half of the same write -- it makes the
      # dispatcher re-say a line it already said -- but it is still a write, so
      # it is the poll's to make too.
      if [ "$voice" = say ]; then rm -f "$STATE_DIR/ps-blind-$n"; fi
      case "$state" in running) return 1 ;; esac ;;
  esac
  printf '%s\n' "$reason"
}

count_parked_owned() {
  local parked=0 n
  # Over OWNED issues rather than over markers: one worktree can carry two
  # reasons at once, and counting markers made `parked` exceed the worktrees it
  # described. `parked_for_person` is the same predicate `status` and the farewell
  # use -- `why_parked` answers the narrower "does it carry a keep-marker", which
  # is the distinction that function's own header is about.
  #
  # A MARKER MUST SURVIVE A PASS BEFORE IT COUNTS, and that is the difference
  # between ending a drain and ending it too early. `stuck-` is terminal --
  # written once on a refused removal and never retried. `held-` and
  # `git-blind-` are RE-DERIVED every pass and cleared the moment the reason
  # goes away, which is routine: `unblock.yml` rewrites `blocked` on every merged
  # PR, so an issue can go blocked, be warned, come off `blocked` when its
  # dependency lands, and go blocked again. Counting those the pass they appear
  # let one transient label -- or one `worktree_holdings` hiccup -- drop `owned`
  # to 0 and sign the dispatcher off with an agent still writing in there. That
  # is #36's failure through a fifth door, opened by the fix for #37.
  #
  # Surviving a pass costs one poll of waiting on a worktree that really is
  # parked, and costs nothing at all on `stuck-`, which is still there next pass.
  # shellcheck disable=SC2045 # $OWNED_DIR holds issue numbers by construction,
  # so there is nothing here for a glob to survive that `ls` does not; a glob
  # would also yield the literal pattern when the directory is empty.
  for n in $(ls "$OWNED_DIR" 2>/dev/null); do
    # ONE CALL, NOT TWO. There used to be a `why_parked` pre-check on this line
    # with the same outcome as the gate below -- `parked_for_person` opens with
    # `parked_marker "$n" || return 1`, so a worktree that is not parked at all
    # leaves through the same `rm` and `continue`. Two forks per owned worktree
    # per pass for an answer already being computed, in the function
    # `parked_marker` was extracted to make cheap. Found by `/code-review`.
    #
    # ...and the agent gate, through the shared predicate so `status` cannot
    # disagree with this count about the same worktree.
    if ! parked_for_person "$n" say >/dev/null; then
      rm -f "$STATE_DIR/parked-since-$n"
      continue
    fi
    if [ -e "$STATE_DIR/parked-since-$n" ]; then
      parked=$((parked + 1))
    else
      : >"$STATE_DIR/parked-since-$n"
    fi
  done
  printf '%s\n' "$parked"
}

# NON-ZERO WHEN THE ANSWER COULD NOT BE READ, which is not the same as "nothing
# is running": reading a failed call as zero live worktrees is how one transient
# hiccup turns into three duplicate worktrees for issues that already have one,
# because `in_flight` goes blind at the same moment and from the same list.
#
# (These ten lines were removed with the selector's comment block and re-homed
# here by the independent review. The selector prose had to go -- it named a
# runner's flag in the file hard rule 2 says may not know one. This half names
# no runtime and was collateral.)
#
# ...and ONE READ PER POLL, cached in $POLL_CACHE like `poll_issue` below it.
# Three callers take this list inside one pass -- the launch gate's own count,
# `foundation_in_flight` and `count_startable` -- and `in_flight` takes one more
# per issue a list-mode run is still waiting on. Three subprocesses for one
# answer is the pattern `count_startable` exists to avoid, run against the runner
# instead of against `gh`. armaatus/autofleet#30.
#
# Cached HERE rather than in the driver: a driver that caches is a driver every
# other driver has to remember to cache in, and $POLL_CACHE is the dispatcher's
# state, not the runner's.
#
# The invalidation is the part that has to be right, and it is one function.
# The list changes when this dispatcher LAUNCHES a worktree and when it REMOVES
# one; both go through `forget_worktree_answers`, so the two per-poll answers
# derived from this list cannot drift onto two different invalidation points.
#
# A FAILED read is NOT cached, so the next caller in the pass asks again rather
# than inheriting a failure as an answer. It does NOT make
# `foundation_in_flight`'s "could not read the worktree list" branch reachable
# from `cmd_run`, which is what an earlier version of this said: the launch loop
# re-reads at the bottom of the iteration that launched, so the next iteration's
# `foundation_in_flight` is always a cache hit. See the note there.
live_worktrees() {
  local cached="$POLL_CACHE/worktrees" list
  # `-e`, not `-s`: no worktrees at all is an ANSWER and caches as an empty file.
  # Outside a poll neither half runs, and this is a plain read of the runner --
  # see $IN_POLL. ONE gate rather than a second uncached reader beside this one:
  # `cmd_status` reaches this function twice, once for its own listing and once
  # through `in_flight`, so a reader it could be pointed at would still leave the
  # other callsite writing.
  if poll_cache_open && [ -e "$cached" ]; then
    # `|| return 1`, and NOT a fall-through to a fresh read: a `cat` that died
    # part-way has already printed half a listing, and reading the runner again
    # behind it would hand the caller that half twice. Non-zero is the answer
    # this function's header is about -- "could not be read" is not "nothing is
    # running" -- and every caller of it skips rather than guesses.
    cat "$cached" || return 1
    return 0
  fi
  list="$(runner_worktree_list)" || return 1
  # `|| return 1`, for the reason the cache branch above has one. This reshape
  # used to be the function's LAST command, so an awk that died -- OOM, a
  # resource limit, a host image this file was vendored onto without one -- came
  # back as the function's own non-zero. It is a middle command now, and
  # `print_listing` below always succeeds, so without this the caller gets an
  # empty listing with success: three free slots, and the empty answer cached
  # for the rest of the pass.
  list="$(printf '%s' "$list" | awk -F'\t' 'NF { print $3 "\t" $1 }')" || return 1
  if poll_cache_open; then
    mkdir -p "$POLL_CACHE" 2>/dev/null
    # Whole or not at all. A half-written file is one `[ -e ]` says is an answer
    # and every later caller in the pass trusts -- and a short listing reads as a
    # free slot, which is the duplicate worktree this function's header opens
    # with. The `rm` is a no-op when the `mv` worked.
    # `2>/dev/null` BEFORE the redirection it is there for: bash applies them
    # left to right, so with it second a $STATE_DIR that will not take the file
    # still printed bash's own diagnostic to the real stderr -- once a poll,
    # into $LOG. Found by the independent review.
    print_listing "$list" 2>/dev/null >"$cached.new" \
      && mv -f "$cached.new" "$cached" 2>/dev/null
    rm -f "$cached.new"
  fi
  print_listing "$list"
}

# A listing back exactly as it was read: one trailing newline per record, and
# NOTHING AT ALL when there are none. The cache round-trips through `$(...)`,
# which eats the trailing newline, and the two obvious ways to put it back are
# both wrong -- `printf '%s'` leaves the last record without one, so
# `while read num path` drops it, and `printf '%s\n'` on an empty list prints a
# blank line, which the same loop reads as a worktree with no number and
# `cmd_status` prints as an empty row.
#
# `|| return 0` FIRST and no `return 0` at the end, so `printf`'s own status is
# what comes back. `[ -n "$1" ] && printf ...; return 0` swallowed it, and the
# caller above is `&& mv` -- so a `printf` that wrote half the listing and then
# died (ENOSPC or EIO on $STATE_DIR, its stderr already swallowed) had that half
# installed as the pass's answer. A SHORT listing is worse than an empty one: it
# is what `in_flight` reads, so an issue whose worktree fell off the end of the
# file reads as free, which is the duplicate worktree this file keeps coming
# back to. It also made `live_worktrees`' fresh path -- whose last command this
# is -- unable to report a failure at all. Found by the independent review.
print_listing() { [ -n "$1" ] || return 0; printf '%s\n' "$1"; }

# Everything derived from the worktree list, dropped together. `launch` and
# `remove_worktree` are the two places this dispatcher changes that list, and
# both call this rather than naming cache files themselves:
# `foundation_in_flight`'s answer is read OFF this list, so a change that drops
# one and not the other leaves a per-poll answer describing a world that is gone.
# Two caches with two invalidation points is the failure armaatus/autofleet#30
# says not to create, and `launch`'s drop already has its own test phase.
#
# $POLL_CACHE is guarded HERE and nowhere else, and the guard is insurance rather
# than a case anyone has demonstrated. armaatus/rommsync-nx exercises
# `remove_worktree` by extracting it with `sed` -- which is why the deadline
# there is a local and not one of the tunables at the top of this file -- and
# that extraction is not in this tree to read. If it brings the functions a
# caller needs but not the top-level assignments, an empty $POLL_CACHE makes
# this `rm -f /worktrees /foundation`; if it brings neither, the call is a
# command-not-found and the guard never runs. One `[ -n ]` covers the first and
# costs nothing in the second, which is the whole argument for it.
# `live_worktrees` is not extracted and reads $POLL_CACHE bare. Raised by the
# independent review, which could not read the extraction either.
forget_worktree_answers() {
  [ -n "${POLL_CACHE:-}" ] || return 0
  rm -f "$POLL_CACHE/worktrees" "$POLL_CACHE/foundation"
}

# How many worktrees a listing holds. Split out because the launch loop needs
# the count AND the names from ONE listing: two calls would let the count that
# shut the gate and the names printed beside it disagree.
#
# There used to be a `live_count` wrapper here -- one `live_worktrees` and one
# `count_worktrees`, the pair below -- and after the poll learned to keep the
# listing it counted, nothing in this file called it. Three test phases still
# did, so the count the suite asserted was a route the dispatcher no longer
# took: the same listing, reached through a function only the tests ran. A
# guard aimed one function to the left of the one that runs is the shape
# armaatus/autofleet#71 is about, so the wrapper is gone and those phases call
# this pair the way `cmd_run` does.
count_worktrees() { printf '%s\n' "$1" | grep -c . || true; }

# What a held foundation issue is actually waiting for, as `#N` where the
# worktree is linked to an issue and a basename where it is not. A count alone
# names nothing a person can go and land -- and the two are not equivalent, since
# one may be this repo's in-flight work and the other a worktree nobody here can
# close. armaatus/autofleet#46 was a line that could not say which, repeating indefinitely.
#
# $1 is the listing the caller already has, for the reason above.
#
# Sorted, because the caller keys its say-once marker on this string: unsorted,
# two unchanged worktrees coming back in the other order read as news and
# reprint the line every poll. `-V` rather than a plain sort -- lexically `#42`
# comes before `#7`, which is the wrong order for the one question a person asks
# of it.
waiting_worktrees() {
  printf '%s\n' "$1" | while IFS="$(printf '\t')" read -r num path; do
    [ -n "$path" ] || continue
    worktree_label "$num" "$path"
  done | sort -V | tr '\n' ' ' | sed 's/ $//'
}

# HOW A WORKTREE IS NAMED in a line a person reads: `#N` where `live_worktrees`
# gave it a linked issue, and its directory's basename where it gave `-`.
#
# ONE function because there are two readers of the same listing and they
# disagreed: the hold printed the basename and `cmd_status` printed `#-`, so the
# worktree a person most needs to recognise -- the hand-opened or foreign one,
# the one nobody in this repository can close -- appeared under two names, one
# of which names nothing. The basename is the one that survives, because a
# person can act on a directory and cannot act on a dash.
# armaatus/autofleet#71.
worktree_label() {
  case "$1" in
    ''|-) basename "$2" ;;
    *)    printf '%s\n' "#$1" ;;
  esac
}

# --------------------------------------------------- the poll cache's shape ---
# The bookkeeping every per-poll cache does, once. Six steps -- gate on the
# poll, refuse a cached failure, serve a cached answer, and on a fresh read
# record either the failure or the answer WHOLE -- and this branch added the
# third and fourth copies of them before extracting this. The ordering rule
# inside them (`2>/dev/null` before the redirection, not after) then had to be
# fixed in five places at once, which is the argument for one copy rather than
# any amount of reasoning about duplication.
#
# `live_worktrees` and `poll_issue` are deliberately NOT converted. They were
# added by armaatus/autofleet#30 and #35 -- both since CLOSED, so that is
# provenance and NOT work parked on an issue that could receive it -- each has
# its own phases, and neither reshapes its answer the way these two do. Rewriting
# them here would have put two tested functions in a change about something else.
# They stay as the fourth and fifth copies; whoever adds a sixth cache should
# reach for these, and converting those two is a fair change on its own day.
# Raised three times by the local review, the last time for citing closed issues
# as though they were somewhere the work could go.
#
# $1 is the cache file. Prints the cached answer and returns 0; returns 1 when a
# failure is cached; returns **3** when there is nothing cached and the caller
# must read for itself.
#
# 3 AND NOT 2, and the odd number is the point. In `has_open_pr` and `in_flight`
# 2 means "could not tell" -- a fact about GitHub -- where a miss here is a fact
# about the cache, and the two must never be conflated: one says "ask again" and
# the other says "do not act". Numbering them apart makes a call site that
# forgets to translate produce a status nothing matches, instead of one that
# silently means the wrong thing. This used to be a 2 held apart by a paragraph
# of comment, which is a worse guard than a different number. Found by the local
# review.
#
# A `cat` that died PART WAY is a 1 and not a 2, for
# `live_worktrees`' reason: half a listing has already reached the caller, and
# reading afresh behind it would hand them that half twice.
poll_cache_get() {
  poll_cache_open || return 3
  [ -e "$1.unreadable" ] && return 1
  # `-e`, not `-s`: an empty answer -- no worktrees, no open PRs, no startable
  # issue -- is an ANSWER, and caches as an empty file.
  [ -e "$1" ] || return 3
  cat "$1" || return 1
  return 0
}

# "Could not tell", recorded so the rest of the pass does not ask again. Never
# an empty answer: every caller of every one of these caches reads an empty
# answer as "nothing found", and that is the reading that opens a duplicate
# worktree or ends the run on a backlog that is not empty.
# The one `mkdir`, so the two writers below do not each carry a copy of it --
# which is the duplication this trio exists to remove, and which it had two of.
# Found by the local review.
poll_cache_dir() { mkdir -p "$POLL_CACHE" 2>/dev/null || true; }

poll_cache_fail() {
  poll_cache_open || return 0
  poll_cache_dir
  # `2>/dev/null` BEFORE the redirection it is there for: bash applies them left
  # to right, so with it second a $STATE_DIR that will not take the file still
  # prints bash's own diagnostic to the real stderr -- into $LOG, once a poll,
  # on the one path that is already an outage. `|| true` hides the status, not
  # the diagnostic. CLAUDE.md, "Code".
  : 2>/dev/null >"$1.unreadable" || true
}

# ...and the answer, WHOLE OR NOT AT ALL. A half-written file is one the `[ -e ]`
# above says is an answer and every later caller in the pass trusts, and a SHORT
# listing is the worst shape of all: the records that fell off the end read as
# "this issue is free". Written beside the real file and moved into place, so
# there is no moment at which a reader can see half of it. The `rm` is a no-op
# when the `mv` worked.
#
# $2 is the value; it goes through `print_listing`, so an empty value writes an
# empty file rather than a blank line and a non-empty one keeps the single
# trailing newline `$(...)` ate on the way in.
poll_cache_put() {
  poll_cache_open || return 0
  poll_cache_dir
  print_listing "$2" 2>/dev/null >"$1.new" && mv -f "$1.new" "$1" 2>/dev/null
  rm -f "$1.new"
}

# --------------------------------------------------------------- the queue ---
# Every open issue, with how many other open issues are blocked BY it. That
# number is the SECOND key, not the ordering: `priority` sorts ahead of it (see
# "What it picks" above), and within one priority class the work that frees the
# most other work goes first.
# It reads the same `Blocked by #N` lines unblock.yml parses, so nothing new has
# to be maintained.
# ONE LISTING PER PASS. The launch loop takes one per iteration of the outer
# `while ! $drain_mode` loop and `count_startable` takes another at the bottom
# of the pass to count what the first already listed, so an idle pass paid for
# two and a pass that launched three paid for four. The cache is the poll's, so
# an issue relabelled `ready` mid-pass is seen on the next one -- one poll of
# staleness, the same contract `poll_issue` takes.
#
# `count_startable` reading a list taken BEFORE this pass's launches can
# OVER-count, and that is the harmless direction rather than an accident. It
# excludes what is running off the worktree listing, whose cache `launch` drops
# -- but whether a `worktree create` that returned moments ago is already in the
# runner's next listing is exactly the property the launch loop refuses to rely
# on further down this file, so this comment must not rely on it either. If the
# listing has not caught up, `queued` is one too high, `cmd_run` polls once more
# and counts again. An UNDER-count is what would matter, because `queued == 0`
# with nothing owned is how the dispatcher decides the backlog is finished, and
# nothing on THIS path can produce one -- the `ready` cache holds a listing that
# was read, whole, this pass. The listing being a truncated PAGE is a different
# question and a real under-count: `--limit 200` has no full-page guard, so a
# backlog past it reaches `queued == 0` and the dispatcher exits. That is
# documented in docs/WORKFLOW.md and owned by armaatus/autofleet#122, and this
# comment used to read as though it denied it. Found by the local review, which
# read two comments in this file disagreeing about the same property.
# armaatus/autofleet#69.
#
# A FAILED read is cached as a failure and never as an empty list. "No issue is
# ready" ends the dispatcher -- `queued` reaches 0, and with nothing owned the
# run loop exits -- so a `gh` outage read as an empty backlog is a fleet that
# stops for the night on the first blip.
ready_issues() {
  # THE CACHE IS INSIDE THIS FUNCTION, not wrapped around a split-out reader:
  # this queue has to select on the same `Blocked by #N` pattern `unblock.yml`
  # writes the labels with, and `issue_refs` is the one spelling of it. A lint
  # check asserted by name that `ready_issues` imported it; that went with
  # armaatus/autofleet#153, and what holds the two parsers together now is
  # `issue_refs.py --selftest`, which compares the literals directly.
  local cached="$POLL_CACHE/ready" listing
  poll_cache_get "$cached"; case $? in 0) return 0 ;; 1) return 1 ;; esac
  # NO `[ -z "$listing" ]` HERE, and that is not an oversight -- it is the one
  # place this cache and `open_pr_listing`'s deliberately differ. `gh` hands
  # back `[]` for an empty PR list, so THERE a zero-length body can only be an
  # outage. Here the python below prints nothing when no issue is startable,
  # which is a real and ordinary answer, so emptiness says nothing about
  # whether the read worked. The failure is caught by the pipeline's status
  # instead: `gh` failing leaves python with nothing to parse, `json.load`
  # raises, and `pipefail` brings that back as the assignment's non-zero.
  # Raised by the local review, which read the two as an accident.
  #
  # `2>/dev/null` on the python, because that raise is the MECHANISM and its
  # traceback is not the message: uncaught, a `gh` outage put a JSONDecodeError
  # on the dispatcher's stderr once a poll, into $LOG, for as long as it lasted.
  # `gh`'s own stderr was already silenced and python's was not, which is the
  # same defect this branch fixed in `open_pr_listing`'s probe. Found by the
  # local review.
  listing="$(GH_PAGER=cat gh issue list --state open --limit 200 \
    --json number,title,body,labels 2>/dev/null \
    | PYTHONPATH="$ISSUE_REFS" python3 -c '
import json, sys
from issue_refs import blocked_by
human_step, priority = sys.argv[1], sys.argv[2]
issues = json.load(sys.stdin)
# ...and it has to BE a listing. A top-level object -- an error envelope `gh`
# handed back with status 0 -- iterates its keys, so `{}` printed nothing and
# was cached as "the backlog is empty", which is how the dispatcher decides
# there is no work left and exits for the night. `open_pr_listing` carries the
# same guard; found by the local review.
if not isinstance(issues, list):
    raise SystemExit("the issue listing is not a list")


def has(issue, label):
    return any(l["name"] == label for l in issue.get("labels", []))

blocks = {}
for i in issues:
    for n in blocked_by(i.get("body")):
        blocks[n] = blocks.get(n, 0) + 1
# `ready` says every blocker is closed. It does not say an agent can finish the
# work: an issue whose last step belongs to a person stays open through the PR
# that prepares it, and stays `ready` with it. #148 was picked up again 18
# seconds after its own preparatory PR merged, and would have been picked up
# once per cycle forever, each attempt further from the point.
ready = [i for i in issues if has(i, "ready") and not has(i, human_step)]
# Priority first, then most-unblocking, then oldest issue number: predictable
# inside a tie. The priority flag is a BOOLEAN in the key and not a weight -- a
# labelled issue outranks every unlabelled one whatever either unblocks, which is
# the whole of what a person applying the label is asking for. Within the
# labelled set the ordinary ordering still decides, so labelling five issues does
# not throw away what the queue itself says about which of the five goes first.
# NOTE: no apostrophes anywhere in this block -- it lives inside a single-quoted
# `python3 -c` string, and one closes it.
for i in sorted(ready, key=lambda i: (not has(i, priority),
                                      -blocks.get(i["number"], 0),
                                      i["number"])):
    labels = ",".join(l["name"] for l in i.get("labels", []))
    print(i["number"], blocks.get(i["number"], 0), labels, i["title"], sep="\t")
' "$HUMAN_STEP_LABEL" "$PRIORITY_LABEL" 2>/dev/null)" || {
    poll_cache_fail "$cached"
    # Once per $AUTOFLEET_HOLD_RESAY, and onto STDERR -- this function's stdout
    # is the queue, and a line printed onto it would be read as an issue row.
    # `say` tees to $LOG either way, which is where an overnight run is read.
    poll_cache_open && hold_say_into "$READY_UNREADABLE_SAID" ready \
      "could not read the issue listing, so nothing will start this pass." \
      "The dispatcher keeps polling rather than deciding the backlog is empty." >&2
    return 1
  }
  # `poll_cache_open` on the removal, for the reason `PR_PAGE_FULL_SAID`'s has
  # one: `cmd_status` reaches this function too, and an ungated `rm` deletes a
  # live dispatcher's say-once marker -- which re-floods the log AND writes to
  # $STATE_DIR, against armaatus/autofleet#35's byte-identical acceptance. The
  # sibling got this gate a round ago and this one shipped without it. Found by
  # the local review.
  poll_cache_open && rm -f "$READY_UNREADABLE_SAID"
  poll_cache_put "$cached" "$listing"
  # Through `print_listing` rather than `printf '%s\n'`, because `$(...)` ate
  # the trailing newline: without it the launch loop's `read` drops the LAST
  # candidate, and a one-issue backlog never starts at all.
  print_listing "$listing"
}

# `ready` overstates availability: the label stays until the PR merges, so an
# issue with a PR already open still carries it.
# 0 = a PR closes it, 1 = none does, 2 = could not tell. The third answer is
# not decoration: python printing nothing is what "no PR" looks like, and it is
# also what a failed import or a malformed listing looks like. Read as "free",
# that opens a second worktree for work already in flight -- which is the whole
# failure this shared module exists to prevent.
# EVERY OPEN PULL REQUEST, ONCE A PASS. `has_open_pr` -- and through it
# `in_flight`, and through that the launch loop's scan of the whole `ready`
# queue -- plus `count_startable` and `enforce_timebox` all want this one
# listing, and each used to take its own copy -- one per candidate the launch
# loop scanned, and it scanned to the end whenever nothing was startable. The
# measurement and its arithmetic live in docs/WORKFLOW.md, "What one poll
# costs", and are NOT restated here: the backlog moves, and two copies of a
# derived number are two chances to disagree about which measurement they are
# describing. It was thousands of calls an hour on a backlog this size, past the
# 5000/hour primary limit's comfort zone and into the secondary limits the
# comments in `has_open_pr` and `count_startable` cite as the reason those
# functions exist at all. armaatus/autofleet#69.
#
# THE WINDOW THIS OPENS, named here because it is what the saving costs. A PR
# opened AFTER the listing is taken is invisible for the rest of the pass, so
# `in_flight` can read an issue as free seconds after somebody claimed it and
# the launch loop opens a second worktree for work already running. One poll
# wide -- `forget_poll_answers` empties the cache at the top of every pass --
# and the same one-poll contract `poll_issue` already takes, but the failure
# direction is worse here: `poll_issue` stale by a poll is a comment made late,
# this is a duplicate worktree.
#
# Which is why `cmd_run` takes it NO LATER THAN the line before the launch loop.
# Read the direction carefully, because an earlier version of this comment had
# it backwards: taking the listing SOONER makes the window WIDER, not narrower
# -- the window is the interval between the fetch and the launch loop acting on
# it, so every watcher that runs before the prefetch and asks first (
# `reset_context_for_answering` per owned worktree, `enforce_timebox` per
# overdue one) lengthens it. What the prefetch guarantees is the other end: the
# window CLOSES at the launch loop, and it is at most one pass wide whoever
# opened it. That is the whole claim, and it is the one a reader can check.
# Without it the fetch would land mid-scan, at a different candidate on every
# poll, and there would be no width to state at all. Found by the local review.
#
# 0 and the listing on stdout, or 1 and nothing at all. "Could not tell" is
# cached in `.unreadable` and NEVER as an empty listing: an empty listing is the
# answer "no PR closes any issue", and a caller that reads a `gh` outage that
# way opens a worktree for every issue in the backlog at once.
open_pr_listing() {
  # ONE NUMBER for the page size, because the truncation guard below is only a
  # guard while it equals the limit actually asked for -- the same split, for
  # the same reason, as `review_open_prs`' `pr_page`.
  local cached="$POLL_CACHE/open-prs" listing pr_page=100
  # 3 is "nothing cached, go and ask", which is deliberately not any status this
  # function returns. See `poll_cache_get`'s header.
  poll_cache_get "$cached"; case $? in 0) return 0 ;; 1) return 1 ;; esac
  # ...and outside a poll, the process-scoped memo. Same three states, no file.
  if ! poll_cache_open; then
    case "$OPEN_PR_MEMO_STATE" in
      ok)   print_listing "$OPEN_PR_MEMO"; return 0 ;;
      fail) return 1 ;;
    esac
  fi
  # EMPTY IS NOT AN ANSWER from `gh` itself: an empty list comes back as `[]`,
  # so a zero-length body is a `gh` that printed nothing, which the parsers
  # below would each read as "no PR". Caught here, once, rather than five times.
  # A FULL PAGE IS A DIFFERENT QUESTION, and it is answered below rather than
  # here: at the limit an absent PR cannot be told from one on page two, and
  # this listing is the single authority the launch loop reads. It used to be
  # refused outright; see the `case` below for why it is used instead, and what
  # that costs. armaatus/autofleet#151, folding armaatus/autofleet#122.
  # THE COUNT AND THE PARSE ARE ONE THING. What the probe below prints is
  # `row_count`, and it carries a third state in the empty string: "the body was
  # not a listing at all". The `case` below reads all three.
  local row_count=""
  if listing="$(GH_PAGER=cat gh pr list --state open --json number,body --limit "$pr_page" 2>/dev/null)" \
     && [ -n "$listing" ]; then
    # THE SAME PARSE DECIDES BOTH QUESTIONS, and its failure is the third
    # answer. `python3` dying here is a body `gh` handed back with status 0 and
    # which is not a listing at all -- and without this the empty substitution
    # simply failed the `= "$pr_page"` test, so the garbage was written to the
    # cache AS AN ANSWER with status 0. Every caller then re-ran its own parser
    # to rediscover it was garbage, and `count_startable`'s has no `try` around
    # its `json.loads`, so a raw traceback went to the dispatcher's stderr once
    # a minute for as long as `gh` misbehaved. The header above promises this
    # function never caches "could not tell" as a listing; this is the line that
    # makes that true. Found by the local review.
    # ...and it asks `isinstance(..., list)`, because `len()` alone is
    # TYPE-BLIND: a top-level JSON object -- an error envelope `gh` handed back
    # with status 0 -- has a length too, so it passed this probe and was cached
    # as an answer. Every caller then rediscovered it was not a listing, and
    # `count_startable`'s `json.loads` has no `try` around it, which is the
    # once-a-minute traceback this guard is supposed to close. `-1` fails the
    # digits-only test below. Found by the local review.
    row_count="$(printf '%s' "$listing" | python3 -c '
import json, sys
loaded = json.load(sys.stdin)
print(len(loaded) if isinstance(loaded, list) else -1)
' 2>/dev/null)" || row_count=""
  fi
  case "$row_count" in
    ''|*[!0-9]*) listing="" ;;
    # SAID through `hold_say_into`, which re-says on staleness: once per
    # $AUTOFLEET_HOLD_RESAY (an hour by default) rather than once a minute. Said
    # only from inside a poll, because `cmd_status` reaches this function too
    # and does not write to the log.
    # A FULL PAGE IS NO LONGER A REASON TO WAIT, which is the half of
    # armaatus/autofleet#122 that armaatus/autofleet#151 folds in.
    #
    # It used to blank the listing: at the limit nothing distinguishes an absent
    # PR from one on page two, so the safe direction was to answer "could not
    # tell" everywhere. The cost of that safety was the whole dispatcher --
    # nothing launched, nothing was time-boxed, and `count_startable`'s non-zero
    # kept the run loop polling for work it would never start, on a repository
    # whose only sin was 100 open pull requests.
    #
    # THE ONE PAGE IS KEPT AND USED. What it can still get wrong is bounded and
    # nothing like as bad as the stop was: an issue whose PR sits past the page
    # boundary reads as free, and the fleet opens a second worktree for it. The
    # `Closes #N` sweep in `reap_merged` finds that within a pass, and the page
    # is ordered newest-first, so the PRs a live fleet cares about are the ones
    # on it. Said, through `hold_say_into`, so a host that really does keep 100
    # PRs open learns why a duplicate can appear rather than discovering it.
    #
    # ...ONTO STDERR, and that redirection is load-bearing rather than tidy.
    # This function's STDOUT IS THE ANSWER: `has_open_pr` and `count_startable`
    # both take it through `$(...)`, so a `say` here goes into the listing
    # rather than to the operator. `say` tees to $LOG either way, so the line
    # still lands where an overnight run is read. Found by the local review's
    # phase for it, which measured silence.
    "$pr_page")
       poll_cache_open && hold_say_into "$PR_PAGE_FULL_SAID" "$pr_page" \
         "$pr_page open pull requests is this listing's page limit, so a PR on the next page" \
         "cannot be told from one that does not exist. The page is used anyway: the fleet may" \
         "open a second worktree for an issue whose PR is past the boundary, and reap_merged" \
         "finds that within a pass. See docs/WORKFLOW.md, \"What one poll costs\"." >&2 ;;
  esac
  # ...and DROPPED the moment a whole listing comes back, so a wedge that clears
  # and recurs inside $AUTOFLEET_HOLD_RESAY is announced again rather than
  # swallowed by an hour-old marker. `launch` drops `FOUNDATION_HOLD_SAID` for
  # the same reason: a say-once marker must not outlive the condition it is
  # about. Found by the local review.
  # `poll_cache_open` on the REMOVAL too, not only on the writes. `cmd_status`
  # reaches this function through `in_flight` -> `has_open_pr`, and #35's
  # acceptance is that `status` leaves $STATE_DIR byte-identical; an ungated `rm`
  # made it delete a live dispatcher's say-once marker, which is the same class
  # of bug as the `IN_POLL` gate on the cache itself. `status_keeps_cache` missed
  # it only because its fixture never creates the marker. Found by the local
  # review.
  poll_cache_open && [ -n "$listing" ] && rm -f "$PR_PAGE_FULL_SAID"
  if [ -z "$listing" ]; then
    poll_cache_fail "$cached"
    poll_cache_open || OPEN_PR_MEMO_STATE=fail
    return 1
  fi
  poll_cache_put "$cached" "$listing"
  if ! poll_cache_open; then OPEN_PR_MEMO="$listing"; OPEN_PR_MEMO_STATE=ok; fi
  print_listing "$listing"
}

has_open_pr() {
  local listing found
  # `|| return 2` -- "could not tell", not "no PR". See the header above.
  listing="$(open_pr_listing)" || return 2
  # The issue number goes in as an ARGUMENT, not spliced into the source. A PR
  # body is third-party text and so, in principle, is anything that reaches the
  # pattern.
  found="$(printf '%s' "$listing" \
    | PYTHONPATH="$ISSUE_REFS" python3 -c '
import json, sys
from issue_refs import closes_issue
try:
    prs = json.load(sys.stdin)
except ValueError:
    raise SystemExit("could not read the pull request listing")
for p in prs:
    if closes_issue(p.get("body"), sys.argv[1]):
        print(p["number"]); break
' "$1")" || return 2
  [ -n "$found" ]
}

# 0 = in flight, 1 = free, 2 = could not tell. The third answer matters: a
# caller that treats "could not tell" as "free" opens a second worktree for work
# that is already running.
in_flight() {
  local list rc
  list="$(live_worktrees)" || return 2
  # NOT `grep -qx`: `-q` exits on the first match, `cut` dies of EPIPE, and
  # `set -o pipefail` makes the pipeline 141 -- the same race measured at
  # `clear_stale_review_records` below. Past a long enough list this said "not
  # in flight" about an issue that is.
  printf '%s\n' "$list" | cut -f1 | grep -x "$1" >/dev/null && return 0
  has_open_pr "$1"; rc=$?
  [ "$rc" = 0 ] && return 0
  [ "$rc" = 2 ] && return 2
  return 1
}

# $1 is a comma-separated label list, $2 one label. Exact matches only: `ready`
# must not answer for `ready-ish`, and `foundation` must not answer for
# `foundational`. -F and -- keep that true for a label carrying a regex
# metacharacter or a leading dash, both of which the env overrides above allow.
# `grep -xF ... >/dev/null` rather than `grep -qxF`: see the comment above, and
# `clear_stale_review_records` for where this race was first measured.
has_label() { printf '%s' "$1" | tr ',' '\n' | grep -xF -- "$2" >/dev/null; }
is_foundation() { has_label "$1" "$FOUNDATION_LABEL"; }

FOUNDATION_HOLD_SAID="$STATE_DIR/holding-for-foundation"
# The rotation's say-once marker, named here rather than spelled out at each
# use: it was written as a bare path at three sites, and a list of literals is
# the drift `clear_issue_markers` already has a comment about. In $STATE_DIR so
# it outlives a poll, cleared when a dispatcher STARTS -- the same rule, for the
# same reason, as the one above. Found by the independent review.
ROTATE_BLIND_SAID="$STATE_DIR/rotate-blind"
# `open_pr_listing`'s say-once for the page-limit cliff. In $STATE_DIR rather
# than $POLL_CACHE because it has to outlive a poll -- the whole point is one
# line per outage, not one a minute -- and cleared at dispatcher startup with
# the other two, so a restart explains itself rather than inheriting silence.
PR_PAGE_FULL_SAID="$STATE_DIR/pr-page-full"
# ...and `ready_issues`' own. Its two sibling listings both say something when
# they cannot be read; this one silently cached the failure, so a persistent one
# -- expired `gh` auth, a broken $ISSUE_REFS -- left the dispatcher polling
# forever, launching nothing, with NOTHING IN THE LOG. That got worse, not
# better, when the python's traceback was silenced: the traceback was ugly and
# it was also the only signal. Found by the local review.
READY_UNREADABLE_SAID="$STATE_DIR/ready-unreadable"
# Its sibling for "the rotation could not happen at all" is a VARIABLE, not a
# file, and that is the whole point: the condition it reports is a $STATE_DIR
# nothing can write to, so a marker in $STATE_DIR cannot be created in exactly
# the case it exists for and the line repeats anyway. A variable is once per
# dispatcher, which is what the marker was reaching for -- a new process starts
# with it empty, so "cleared at dispatcher start" comes for free. Found by the
# independent review, and by the first fix for it, which used a file.
ROTATE_STUCK_SAID=""

# Hold, remember the reason, and say it if it is news. The three branches below
# were three copies of `printf hold` / `foundation_hold_say` / `return 0`, which
# is three chances to write a fourth that caches a hold and never explains it.
# NOTE the call convention: `foundation_hold ... && return 0`. A `return` in here
# returns from THIS function, not from `foundation_in_flight` -- collapsing the
# three branches into it without that dropped every hold straight through to
# "nothing in flight", which three phases caught at once.
foundation_hold() {
  local cached="$1" what="$2"; shift 2
  # `2>/dev/null` BEFORE the redirection, per CLAUDE.md "Code": with it second a
  # $STATE_DIR that will not take the file still prints bash's own diagnostic to
  # the real stderr. Found by the local review.
  printf 'hold' 2>/dev/null >"$cached" || true
  foundation_hold_say "$what" "$@"
}

# How long a standing hold stays quiet before it says itself again.
#
# Without this the marker never expires: it is cleared by a launch and at
# dispatcher start, and a hold that lifts and returns hours later with nothing
# launched in between says nothing, because the marker still names it. The
# benign end of that is a label flapping; the other end is a fleet that has
# opened no worktree since morning with the explanation scrolled off the log.
# Hourly against a 60-second poll is 1 line where the bug was 180, and it is
# still an answer for somebody who runs `tail fleet.log` at noon. Found by the
# independent review.
: "${AUTOFLEET_HOLD_RESAY:=3600}"

# Say something once per reason, remembering the reason in $1.
#
# ONE MARKER PER THING BEING HELD. This took a single shared file, so two
# different holds -- a foundation issue in flight and a PR whose reviewer hit
# the cap -- overwrote each other's reason every poll and BOTH re-announced,
# once a minute, forever. That is the flooding this whole change exists to
# remove, reintroduced by the fix for it. The two holds are also evaluated in
# the same poll body, which is what made it certain rather than unlucky. Found
# by the independent review.
hold_say_into() {
  local where="$1" what="$2"; shift 2
  local said_at now stale=false
  said_at="$(fleet_mtime "$where")" || said_at=""
  now="$(date +%s)"
  # Guarded, because an mtime this could not read must degrade to "say it" and
  # never to an arithmetic error inside the dispatcher's poll.
  case "$said_at" in
    ''|*[!0-9]*) stale=true ;;
    *) [ "$(( now - said_at ))" -ge "$AUTOFLEET_HOLD_RESAY" ] && stale=true ;;
  esac
  if $stale || [ "$(cat "$where" 2>/dev/null)" != "$what" ]; then
    # ...and here it costs more than a stray line. A marker that never lands
    # leaves `said_at` unreadable, so `stale` is true on the next poll and this
    # says its lines AGAIN -- once a minute, which is the flood the marker exists
    # to prevent, with bash's own diagnostic beside each one. New callers arrive
    # here (`PR_PAGE_FULL_SAID`), so the order is fixed rather than inherited.
    # CLAUDE.md, "Code"; found by the local review.
    printf '%s' "$what" 2>/dev/null >"$where" || true
    local line
    for line in "$@"; do say "$line"; done
  fi
}

foundation_hold_say() { hold_say_into "$FOUNDATION_HOLD_SAID" "$@"; }

# Is one of the worktrees in flight working on a FOUNDATION issue?
#
# CLAUDE.md: "a foundation issue lands alone. When an issue defines an interface
# later issues include, it merges before anything that depends on it starts."
# The dispatcher enforced half of that -- a foundation issue would not JOIN
# running worktrees -- and nothing stopped others joining a foundation issue. On
# a cold start `live` is 0, so the foundation issue is picked first, `live`
# becomes 1, and every later candidate that pass is not itself a foundation
# issue and sails past the check.
#
# That is not theoretical. autofleet's own first real run, in eleven seconds:
# #1 (foundation, the runner driver), then #4 (docs/WORKFLOW.md, which describes
# the runner), then #7 (install.sh, which lists what ships and which #1 adds
# runner files to). Three worktrees on exactly the collision the rule exists to
# prevent.
#
# `live` is a COUNT, and the rule is about what those worktrees are working on --
# which is why this asks the issues rather than the number, and why it keeps
# holding across a dispatcher restart, where the count alone says nothing.
#
# 0 = yes, hold. 1 = no. Never 2: an unreadable answer HOLDS, and says so, for
# the same reason `in_flight` treats "could not tell" as in flight -- launching
# on a guess is the expensive direction, and the next pass asks again.
#
# ONE ANSWER PER POLL, cached in $POLL_CACHE like every other repeated lookup in
# this file: the launch loop asks up to MAX_WORKTREES times per pass and the
# answer cannot change in between except by this loop launching something, which
# is what `launch` invalidates it for. An earlier version of this claimed no new
# API call while making one per iteration.
#
# The worktree list underneath it is cached too, in the same pass and dropped at
# the same two points -- see `live_worktrees`. It used to be a second read of a
# list `count_startable` took a third of, which is the convention this function
# follows within itself and did not follow across the three.
# armaatus/autofleet#30, found by the independent review of #28.
#
# ...and it SAYS SO ONCE, not once per poll, which is a different question from
# the cache: a three-hour foundation issue against a 60-second poll is 180
# identical lines in fleet.log. The marker in $STATE_DIR is this file's idiom for
# it -- `queue-labels-$n` and `gaveup-$n` are the same shape -- and it is cleared
# the moment the hold ends, so the next hold speaks again. Both found by the
# local review of the change that added this.
# In $STATE_DIR so it outlives a POLL, and cleared when a dispatcher STARTS --
# the same rule $POLL_CACHE follows and for the same reason. Left standing across
# a restart, a new dispatcher reads back the old one's marker and holds in total
# silence: zero worktrees opened and not one line in fleet.log saying why, which
# is precisely the case this function's own comment says it exists for. Found by
# the local review of the change that added it.
foundation_in_flight() {
  local cached="$POLL_CACHE/foundation" list n _path answer labels
  mkdir -p "$POLL_CACHE" 2>/dev/null
  # TWO VALUES, because two is all anything reads. It used to write four --
  # `no`, `unreadable`, `labels-unreadable-$n`, `$n` -- while only ever asking
  # `= no`, so three of them implied a contract nothing honoured. The REASON for
  # a hold is carried by the say-once marker, which is the thing that needs it.
  # Found by the independent review.
  if [ -e "$cached" ]; then
    [ "$(cat "$cached")" = no ] && return 1
    return 0
  fi

  # NOT REACHED FROM `cmd_run` any more, and kept anyway. Both reads the launch
  # loop makes -- the one at the top of the pass and the one at the bottom of an
  # iteration that launched -- handle this failure first, one by skipping the
  # pass and the other by breaking the loop, and both leave a populated cache
  # behind them, so this call is a cache hit every time the dispatcher makes it.
  #
  # It stays because it is this FUNCTION's fail-closed answer, not the launch
  # loop's: `foundation_blind` calls it directly and asserts exactly this, and a
  # function whose contract is "an unreadable answer holds, and says so" does not
  # get to drop the branch that holds. What it must not be read as is the pass's
  # handler for this failure -- armaatus/autofleet#30 expected it to become that
  # and it did not. Found by the local review.
  if ! list="$(live_worktrees)"; then
    foundation_hold "$cached" "list-unreadable" \
      "could not read this repository's worktree list, so whether a foundation" \
      "  issue is in flight cannot be answered -- launching nothing rather than guessing"
    return 0
  fi

  # Every number below is resolved against THIS repository, which is safe only
  # because `runner_worktree_list` is scoped to it -- see the driver. It was not,
  # and an unrelated worktree whose linked number matched a `foundation` issue
  # here -- or matched nothing here at all, which fails the lookup and holds
  # fail-closed -- stopped the fleet launching anything, indefinitely, after a
  # single line in the log.
  #
  # The premise was older than this function: `in_flight` and `count_startable`
  # shared it, where it merely inflated a count. Here it was fatal rather than
  # inaccurate, which is why it is written down. Fixed in
  # armaatus/autofleet#46, in `runner_worktree_list` -- the driver scopes the
  # query, so all three callers get it at once. Found by the independent review.
  while IFS="$(printf '\t')" read -r n _path; do
    # `-` is `live_worktrees` saying this worktree has no linked issue at all,
    # which is a worktree somebody opened by hand. That is an ANSWER, not a
    # failure to answer -- a worktree on no issue is on no foundation issue --
    # so it does not hold, unlike the two lookups below that genuinely cannot
    # say. The distinction was implicit and is now written down.
    case "$n" in ''|-|*[!0-9]*) continue ;; esac
    if ! answer="$(poll_issue "$n")"; then
      foundation_hold "$cached" "labels-$n" \
        "#$n is in flight and its labels would not read, so whether it is a" \
        "  foundation issue cannot be answered -- launching nothing"
      return 0
    fi
    # The STATE half of the answer, not just the labels. A foundation issue that
    # has been closed -- merged, and its worktree not yet reaped -- is finished,
    # and holding the whole fleet for it until the reap catches up is a stall
    # with no reason left behind it. `poll_issue` returns both halves and this
    # used only one. Found by the independent review.
    case "$(issue_state_in "$answer")" in
      CLOSED|closed) continue ;;
    esac
    labels="$(issue_labels_in "$answer")"
    if is_foundation "$labels"; then
      foundation_hold "$cached" "$n" \
        "#$n is a foundation issue and is still in flight; it lands alone, so" \
        "  nothing else starts until it does"
      return 0
    fi
  done <<<"$list"

  # See `foundation_hold` above for why the order is this way round.
  printf 'no' 2>/dev/null >"$cached" || true
  # NOT cleared here, and that is the fix rather than an omission.
  #
  # This function runs first on every iteration, so the only way the OTHER hold
  # -- a foundation CANDIDATE declining to join ordinary worktrees -- is ever
  # reached is by this one falling through to here. Clearing the marker on the
  # way past deleted it one step before `foundation_hold_say "waiting-$n"` read
  # it, so that marker was write-only and the line it guards printed every poll:
  # exactly the 180-lines-per-three-hours the marker exists to prevent, restored
  # by the change that claimed to prevent it. Found by the independent review,
  # which also noted that no phase covered that branch -- which is why it
  # survived.
  #
  # `launch` clears it instead: a hold announcement is stale once the fleet has
  # actually moved, and until then repeating it says nothing new.
  return 1
}

# Everything the watchers ask GitHub about ONE issue, in one call and one answer
# per POLL. Prints `<state><TAB><labels>`; 0 = read it, 2 = could not tell.
#
# The third answer is the same one has_open_pr gives, for the same reason: the
# callers below interrupt an agent, comment on an issue and delete a worktree,
# and a listing that could not be read is no basis for any of those. Asked live
# rather than cached at launch, because a label is often what a person adds AFTER
# seeing the card.
#
# One answer per issue per POLL, though. `reap_abandoned` and
# `notice_build_exit` both ask about the same issue in the same pass -- an issue stuck
# long enough to overrun is often also the one a maintainer has just blocked --
# and two calls for one answer is the pattern count_startable exists to avoid. State and labels come back together for that
# same reason: they are one `gh issue view`, not two. The cache lives for one
# pass, so a label a person adds is still seen on the next one.
poll_issue() {
  local cached="$POLL_CACHE/issue-$1" answer
  mkdir -p "$POLL_CACHE" 2>/dev/null
  [ -e "$cached.unreadable" ] && return 2
  [ -e "$cached" ] && { cat "$cached"; return 0; }
  if answer="$(GH_PAGER=cat gh issue view "$1" --json state,labels \
                 --jq '.state + "\t" + ([.labels[].name]|join(","))' 2>/dev/null)"; then
    # ...and the same ordering here, in the copy this rule predates. See the
    # note in `ready_issues` above.
    printf '%s' "$answer" 2>/dev/null >"$cached" || true
    printf '%s' "$answer"
    return 0
  fi
  : 2>/dev/null >"$cached.unreadable" || true
  return 2
}

# The two halves of what poll_issue prints. Split here rather than at each call
# site so a `gh` that ever answers without the separator cannot be read as a
# state that is also a label list: both print nothing at all instead.
issue_state_in()  { case "$1" in *"$ANSWER_SEP"*) printf '%s' "${1%%"$ANSWER_SEP"*}" ;; esac; }
issue_labels_in() { case "$1" in *"$ANSWER_SEP"*) printf '%s' "${1#*"$ANSWER_SEP"}" ;; esac; }

# Is this issue's last step a person's? 0 = yes, 1 = no, 2 = could not tell.
issue_needs_human_step() {
  local answer
  answer="$(poll_issue "$1")" || return 2
  has_label "$(issue_labels_in "$answer")" "$HUMAN_STEP_LABEL"
}

# Landed: the issue is closed, or a PR that closes it has merged. `ready` does
# not answer this -- unblock.yml only relabels dependants -- and neither does
# "no worktree", which is also true of work that never started.
issue_is_done() {
  local state
  state="$(GH_PAGER=cat gh issue view "$1" --json state --jq .state 2>/dev/null)"
  [ "$state" = "CLOSED" ] && return 0
  local landed
  # A failed lookup answers "not done", the same as before: an issue that stays
  # on `fleet.sh run 11 12 13` is retried, which is the harmless direction. It
  # is not silently conflated with a real answer, though -- see has_open_pr.
  landed="$(GH_PAGER=cat gh pr list --state merged --limit 50 --json body 2>/dev/null \
    | PYTHONPATH="$ISSUE_REFS" python3 -c '
import json, sys
from issue_refs import closes_issue
try:
    prs = json.load(sys.stdin)
except ValueError:
    raise SystemExit("could not read the pull request listing")
for p in prs:
    if closes_issue(p.get("body"), sys.argv[1]):
        print("done"); break
' "$1")" || return 1
  [ -n "$landed" ]
}

# How many `ready` issues could start right now, from ONE worktree list and ONE
# PR list. Zero also means "nothing to wait for" to the run loop, so it must not
# silently answer zero when a lookup failed -- it returns non-zero instead.
count_startable() {
  local live prs ready gaveup
  live="$(live_worktrees)" || return 1
  # The pass's one listing, not a fourth copy of it. It carries `number` as well
  # as `body`, which this block does not read and does not have to.
  #
  # IT GOES IN ON STDIN, and that swap is armaatus/autofleet#122's other half.
  # It used to go out as ONE ARGV STRING, which is a lower ceiling than the 100
  # rows `open_pr_listing` pages at: the kernel caps a single argument
  # (`MAX_ARG_STRLEN`, 128 KiB on Linux) and these rows carry full PR bodies, so
  # what ran out first was bytes -- well under 100 rows, and a different number
  # on each platform. Over it `execve` failed with `Argument list too long` and
  # `cmd_run` polled forever. `$ready` has no bodies and goes onto argv in its
  # place; the trade is exact and it removes the cliff rather than guarding it.
  prs="$(open_pr_listing)" || return 1
  ready="$(ready_issues)" || return 1
  # An issue the fleet gave up on is one it will decline every pass, so counting
  # it is the run loop polling forever for work that never starts.
  gaveup="$(gave_up_issues)"
  # BOUNDED BY $MAX_WORKTREES, not by the backlog. The only two readers are the
  # run loop's exit test ("is there anything left to start") and the status
  # screen's queue line, and neither is improved by a number that counts 63
  # issues the fleet cannot begin this decade. What it costs to count them is
  # not the arithmetic -- it is that every one of those issues is read as a
  # reason to keep polling, so `cmd_run --until` never returns early on a repo
  # with a long backlog.
  printf '%s\n' "$prs" | PYTHONPATH="$ISSUE_REFS" python3 -c '
import json, sys
from issue_refs import closes
running = {line.split("\t")[0] for line in sys.argv[1].splitlines() if line.strip()}
gave_up = {n for n in sys.argv[3].split() if n}
cap = int(sys.argv[4])
# One parse per BODY, not one over all of them joined: a keyword may be the last
# word of one body and `#12` the first token of the next, and `\s+` would span
# the join -- claiming an issue nobody is working on and hiding it from the
# count. One parse per issue would be the other way round; this is neither.
claimed = set()
for p in json.loads(sys.stdin.read()):
    claimed.update(closes(p.get("body")))
n = 0
for line in sys.argv[2].splitlines():
    if not line.strip():
        continue
    issue = line.split("\t")[0]
    if issue in running or issue in gave_up or int(issue) in claimed:
        continue
    n += 1
    if n >= cap:
        break
print(n)
' "$live" "$ready" "$gaveup" "$MAX_WORKTREES"
}

# --------------------------------------------------------------- the launch ---
slug() {
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]' \
    | tr -cs 'a-z0-9' '-' | sed 's/^-*//;s/-*$//' | cut -c1-48
}

# ------------------------------------------------------------- the build ---
# THE BRIEF IS A FILE NOW, not a prompt typed into a composer.
#
# `issue-command.sh` prints what an agent on this issue has to know, and it
# always did; what changed is where it lands. It used to be the agent's FIRST
# TURN -- the dispatcher drafted "run this command", a watcher pressed Return,
# the agent shelled out and read the output. That is one turn, one tool call and
# one copy of the brief in the transcript before any work starts, on every
# session and on every recycle.
#
# Here it goes into `--append-system-prompt-file`, so it is in the system prompt
# from the first token: cached, never re-read, and impossible to lose to a
# `/clear`. The handoff note went with the recycle it existed to survive, and
# `handoff_note_for` with it -- the branch and the pull request are what a
# second run reads now, and both outlive any session.
#
# IT MUST DEGRADE. `gh` can be rate-limited, logged out, or offline at exactly
# the moment a worktree opens. A build that started with an EMPTY brief would
# look like a build and produce nothing, so the fallback is the old prompt: go
# and run the command yourself. One turn, and it works.
#
# ONE BRIEF, AND A RESUME GETS THE SAME ONE. It used to take a second argument
# and fetch `--after-pr` for a resume, because the post-PR contract was a
# separate half of the text. armaatus/autofleet#152 deleted that half -- the
# agent's job ends at an open pull request -- so a resumed run reads the branch,
# the pull request and the same instructions the first run had.
build_brief() {
  local num="$1" out
  out="$(mktemp)"
  GH_PAGER=cat ./scripts/fleet/issue-command.sh "$num" >"$out" 2>/dev/null
  if [ -s "$out" ]; then
    cat "$out"; rm -f "$out"; return 0
  fi
  rm -f "$out"
  return 1
}

# The standing instructions, which are the FLEET's and not the issue's.
#
# Separate from the brief above because they are true of every build on every
# issue, and because they must still be said when the brief could not be
# fetched. A run that does not know it may not stop to ask is a run that stops
# to ask, and there is nobody there.
build_preamble() {
  cat <<PREAMBLE
You were started by the autofleet dispatcher as a non-interactive run. There is
no person watching and nothing you print is read until you stop, so work
autonomously to a pull request that is waiting only on GitHub's auto-merge, and
do not stop to ask for confirmation on anything this repository's working
agreement already decides. If a question is genuinely open, write it in the pull
request body and carry on with the rest of the scope.

This run ends when it reaches its turn or budget limit, whichever comes first.
It is not interrupted and it is not asked to hand anything over: the BRANCH and
the PULL REQUEST are the state, so anything you have not committed is what a
second run cannot see. Commit as you go.

If \`$STOP_FILE\` appears at any point, stop: say where you got to and do nothing
further. Nothing can leave this worktree while it exists.
PREAMBLE
}

# Write the two files the build command reads, and start it.
#
# `$1` issue, `$2` worktree. A resume and a first run go through here and differ
# in NOTHING: the brief is the same text, and what a resumed run starts from is
# the branch and the pull request. The old resume was a `/clear` plus a re-send
# plus a handoff note plus a grace period plus four markers, because it had to
# put a live session back the way it found it; the one before that at least
# differed in its brief.
start_build() {
  local num="$1" path="$2" dir runs
  # THE BUILD COMMAND IS CHECKED HERE, and this is the only place that knows a
  # build is about to happen. The driver's `runner_available` asked for it once
  # and stopped every fleet command on a machine with no agent CLI -- the
  # reviewer, the validator, `cost`, none of which builds anything. Asked here
  # it costs one `command -v` per launch and fails the launch out loud instead
  # of spawning a run that dies instantly and is resumed up to
  # AUTOFLEET_BUILD_MAX_RUNS. Found by the suite in CI.
  local prog; prog="$(fleet_build_program)"
  command -v "$prog" >/dev/null 2>&1 || {
    say "  the build command is '$prog', which is not on PATH (AUTOFLEET_BUILD_CMD)"
    return 1; }
  dir="$(fleet_build_dir "$num")"
  mkdir -p "$dir" || { say "  could not make the build directory $dir"; return 1; }

  if build_brief "$num" >"$dir/system.md.new" && [ -s "$dir/system.md.new" ]; then
    { build_preamble; printf '\n'; cat "$dir/system.md.new"; } >"$dir/system.md"
    printf 'Your brief is in the system prompt: the issue, and the instructions that follow it. Implement it end to end.\n' >"$dir/prompt"
  else
    say "  could not read the brief for #$num -- the build will fetch it itself"
    build_preamble >"$dir/system.md"
    printf 'Run `GH_PAGER=cat ./scripts/fleet/issue-command.sh %s` first and follow everything it prints.\n' "$num" >"$dir/prompt"
  fi
  rm -f "$dir/system.md.new"

  # COUNTED, and bounded by AUTOFLEET_BUILD_MAX_RUNS in `build_exited`. A run
  # that ends the instant it starts -- a bad model name, an expired token -- is
  # otherwise an infinite resume loop that spends the account one session at a
  # time with the log saying "resuming" forever.
  # `run-count`, and NOT `runs`, which is the DIRECTORY `fleet_build_started`
  # keeps each finished run's result in. One name for both made `launch`'s reset
  # write a file where a directory had to go, and the next build refused to
  # start with `mkdir: .../runs: File exists` -- a worktree opened, provisioned
  # and left with nothing running in it.
  runs="$(cat "$dir/run-count" 2>/dev/null || echo 0)"
  printf '%s\n' "$((runs + 1))" >"$dir/run-count"
  # The marker the build's own exit is reported through, cleared here so a
  # second run is reported as its own.
  rm -f "$STATE_DIR/build-done-$num" "$STATE_DIR/build-blind-$num" "$STATE_DIR/exit-blind-$num"

  runner_build_start "$path" "$num" || { say "  the runner would not start the build"; return 1; }
  return 0
}

launch() {
  # The worktree list is about to change, and everything derived from it is
  # cached per poll. Dropping it here is what makes the check on the NEXT
  # iteration see the worktree this launch is about to create -- which is the
  # half of the rule that stops anything starting behind a foundation issue --
  # and what makes the launch loop's own re-read below see it too.
  #
  # Before the drop, not after: a `launch` that fails changes nothing, and the
  # only cost of dropping early is one extra read of a list that did not move.
  # Getting that backwards costs a worktree the rule exists to prevent.
  forget_worktree_answers
  local num="$1" title="$2"
  local name; name="$(slug "$num-$title")"

  say "opening a worktree for #$num -- $title"
  local out; out="$(mktemp)"
  runner_worktree_create "$REPO_ROOT" "$name" "$num" >"$out"
  if [ $? != 0 ]; then
    say "  could not create it:"
    sed 's/^/    /' "$out" | tee -a "$LOG"
    rm -f "$out"
    return 1
  fi
  local path
  path="$(cat "$out")"
  rm -f "$out"
  [ -n "$path" ] || { say "  created, but the runner reported no path; not tracking it"; return 1; }
  own "$num" "$path"
  # A FRESH RUN COUNT, here rather than in `start_build`: `own` is what says
  # this worktree is a new attempt, and a `fleet.sh retry` that inherited the
  # previous attempt's count would give the new one one run and then stop.
  rm -f "$(fleet_build_dir "$num")/run-count"
  # The announcement is stale once the fleet has actually MOVED, and this is
  # where it has: a `launch` that FAILED moved nothing and must not re-arm the
  # line, which is what clearing this at the top of the function did. See
  # foundation_in_flight for why it is cleared here rather than on its no-hold
  # path. Found by the independent review.
  rm -f "$FOUNDATION_HOLD_SAID"
  card "$path" workspace-status in-progress comment "#$num: provisioning"

  # THE WORKTREE PROVISIONS ITSELF FROM HERE, not from a runtime hook.
  #
  # `setup.sh` derives the worktree's isolated identity, writes its `.env`,
  # updates submodules and runs the project's own setup hook. The app-backed
  # runner used to call it through `orca.yaml`, which meant it did not happen at
  # all for a driver with no hook mechanism -- and a build whose rig is not up
  # reads the connection error as a code bug and goes chasing it, which is the
  # failure `setupAgentStartupPolicy: wait-for-setup` was there to prevent.
  # Running it HERE gives every driver the same guarantee, in the one place that
  # knows the build has not started yet.
  #
  # Fatal to the launch. Everything it provisions is for the build that is about
  # to start, and a worktree that failed to provision is one where every test
  # run fails for a reason that has nothing to do with the issue.
  # BOUNDED, like the teardown hook it mirrors: a project setup hook that never
  # returns held the whole poll loop, and `remove_worktree` already runs
  # `archive.sh` through the watchdog for exactly that reason. Found by the
  # local `/code-review` pass, which noticed the same PR guarding one side and
  # not the other.
  #
  # AUTOFLEET_DISPATCHER_LAUNCH is how the hook tells the two callers apart. It
  # closes with a stanza naming the command that starts the agent, for the
  # person-opened worktree that has nothing behind it (armaatus/autofleet#156);
  # `start_build` is a few lines below here, so this caller must not print it.
  local setup_out; setup_out="$(mktemp)"
  FLEET_RUN_CAPTURE_STDERR=1 fleet_run_with_deadline "${AUTOFLEET_SETUP_DEADLINE:-900}" \
    "$setup_out" env -C "$path" AUTOFLEET_DISPATCHER_LAUNCH=1 ./scripts/fleet/setup.sh
  if [ $? != 0 ]; then
    say "  #$num: its worktree would not provision:"
    sed -n '1,10p' "$setup_out" | sed 's/^/    /' | tee -a "$LOG"
    rm -f "$setup_out"
    card "$path" comment "#$num: the worktree would not provision -- needs you"
    # THE SLOT GOES BACK. `own` ran above, and a `launch` that returns 1 after
    # it left the issue owned with no build in it -- held for the life of the
    # dispatcher, counted against AUTOFLEET_MAX, with nothing that retries or
    # releases it. Found by the local `/code-review` pass.
    disown_issue "$num"
    return 1
  fi
  rm -f "$setup_out"
  card "$path" comment "#$num: building"
  # THE BUILD IS STARTED BY THE DISPATCHER, not by the runtime's own hooks. The
  # app-backed runner used to start an agent from a worktree-creation hook and
  # the dispatcher never saw it happen; when the hook failed, a fully
  # provisioned worktree sat on an unsent prompt and only a person noticed.
  # Here a build that will not start fails the launch, loudly, on the pass that
  # tried it.
  if ! start_build "$num" "$path"; then
    say "  #$num has a worktree at $path but no build running"
    card "$path" comment "#$num: worktree opened, but the build would not start"
    # ...and the same release. The worktree stays -- whatever provisioning did
    # is in it and a person may want to look -- but the ISSUE is not owned by a
    # dispatcher that has nothing running for it.
    disown_issue "$num"
    return 1
  fi
  say "  #$num is building in $path"
}

# -------------------------------------------------------------- the review ---
# THE DISPATCHER IS WHAT RUNS THE REVIEW, and nothing else can: the agent that
# wrote the pull request must not review it (`.claude/hooks/guard.py` refuses
# `gh pr review` from a fleet worktree), and there is no second venue --
# `.github/workflows/claude-review.yml` is gone with armaatus/autofleet#152, and
# a host that wants the review in Actions uses `anthropics/claude-code-action`
# directly.
#
# What this pass decides is only WHETHER THERE IS A SLOT. Everything about the
# loop itself -- arm the merge, review once, buy at most one fix, re-review, park
# -- is `scripts/fleet/after-pr.sh`'s, which is also what a person runs by hand
# on one pull request.
REVIEWING_DIR="$FLEET_REVIEWING"
# WHAT IS IN THIS DIRECTORY, in one place, because a fourth consumer that had to
# work it out from three use sites is exactly how the third one came to disagree:
#
#   <pr>         the LOCK. Holds `pid head`. Its presence means the post-PR loop
#                is running for this PR; the pass releases it when the pid goes.
#   <pr>.done    a RECORD. Holds the head `after-pr.sh` finished with, so a PR
#                that is waiting on a person is not re-examined every poll for
#                the rest of its life. A head move clears it, because what it
#                recorded was about that commit.
#   <pr>.reviews a RECORD. Holds `n` -- how many reviews this PULL REQUEST has
#                been bought, against the ceiling of two. Written by
#                `after-pr.sh` BEFORE each review, because a reviewer that
#                crashes has still been bought.
#   <pr>.fixes   a RECORD. The same, for the ONE fix session answering a
#                review. It arrived without a row here and without a suffix in
#                `is_review_record`, so `live_reviewers` read it as a LOCK,
#                took its `1` for a pid, found `fleet_agent_alive 1` answering
#                "dead" -- pid 1 is launchd -- and deleted it every poll. The
#                ceiling it is was therefore never a ceiling. This list warns
#                two lines down that a consumer working the suffixes out for
#                itself is how the last one disagreed; the fix was to add the
#                suffix, and the row. Found by the independent review.
#   <pr>.said    a RECORD. Which hold has already been explained for this PR, so
#                it cannot overwrite -- or be overwritten by -- the foundation
#                hold's marker.
#   .closed-<pr> NOT a record and NOT a lock: the sweep's own bookkeeping, and a
#                DOTFILE, which is what keeps it out of every `"$REVIEWING_DIR"/*`
#                loop without any of them learning a new name. Its presence
#                means "a previous pass found this number absent from the open
#                listing"; the records go on the pass AFTER that, and only once
#                `gh pr view --json state` confirms it -- see `pr_sweep_verdict`.
#
# Only the first is a lock, and the records must never be counted as one.
# `is_review_record` is the predicate; use it rather than respelling the suffix
# list. `cmd_status` respelled it as a `find ! -name` and counted every record as
# a reviewer in flight, permanently, on the screen its own comment calls the
# first anybody looks at.
is_review_record() { case "$1" in *.done|*.reviews|*.fixes|*.said) return 0 ;; esac; return 1; }

# A TRANSCRIPT IS MORE THAN ONE FILE: a reviewer killed outside its own trap
# leaves `.log.raw` and `.log.err` beside `pr-<n>-<head>.log`. They go WITH the
# log rather than by globs of their own, so the set cannot fall out of step --
# anything that outlives its log is a store that grows for as long as the fleet
# runs, which is the growth AUTOFLEET_KEEP_REVIEWS exists to stop.
rm_transcript() { rm -f "$1" "${1%.log}.context.md" "$1.raw" "$1.err"; }

# Is pid $1 one of OUR reviewers, or merely a live pid?
#
# `kill -0` alone is not the question. This is the second place in the fleet that
# SIGNALS a pid it read out of a file -- `cmd_stop` is the other, and it uses
# `dispatcher_alive`, whose whole reason for existing is that a marker a `kill -9`
# left behind names whoever the OS has since given that number to. Nothing clears
# $REVIEWING_DIR across a dispatcher's death, so a stale marker outlives its
# process by hours and the number is reused: on the head-moved path that meant
# SIGTERM to a stranger, and on the unmoved path a marker nothing could ever
# reap, so that PR was never reviewed again. Both found by the independent review.
#
# Same three answers as dispatcher_alive: 0 yes, 1 no, 2 alive but ps would not
# say. On "cannot say" the caller treats it as ours -- the conservative choice
# here is to leave a possible reviewer running and its slot held, not to signal
# an unidentified process.
#
# THE PATTERN IS IN lib.sh, anchored, and why it must be is stated there. The
# short of it: an unanchored `review\.sh` also matches `await-review.sh`, which
# is where every worktree agent sits waiting for the review this file starts,
# and `stop_reviewers` SIGTERMs whatever this matches.
#
# IN lib.sh SINCE armaatus/autofleet#64, because `review.sh` asks the same
# question: the lock it may not claim is one a live agent holds. Kept as a name
# here -- six call sites and the three-way answer they read is this file's
# vocabulary -- but there is one copy of the pattern, and it is next to the lock
# helper that depends on it.
reviewer_alive() { fleet_agent_alive "$@"; }

# Every reviewer this dispatcher started, stopped, and their markers cleared.
#
# Called from `cmd_stop --now` and at dispatcher start. Without the first,
# `--now` -- which CLAUDE.md calls the one that "also freezes the agents" --
# left an in-flight reviewer running for up to AUTOFLEET_REVIEW_TIMEOUT more
# minutes: an agent holding this machine's gh credentials, unreaped because the
# dispatcher that would have reaped it was the thing just killed, and invisible
# to `fleet.sh status`, which counts markers. Without the second, markers from a
# dispatcher that was `kill -9`d are inherited by the next one and never cleared.
stop_reviewers() {
  local marker held stopped=0
  [ -d "$REVIEWING_DIR" ] || return 0
  # THE SWEEP'S OWN BOOKKEEPING GOES WITH THE RECORDS, and it needs saying here
  # because `"$REVIEWING_DIR"/*` cannot see it: `.closed-<pr>` is a dotfile, which
  # is exactly what keeps it out of the three loops that would read it as a lock.
  # The loop below clears the records this function is allowed to clear -- not
  # `.reviews` or `.done`, which it skips on purpose -- so without this line a
  # grace marker is orphaned for good, and the number it names is then swept
  # with no grace at all if it comes round again, which is the one thing the
  # marker exists to prevent. Unconditional for that reason: a PR whose records
  # survive loses its marker too, which costs one extra grace pass and cannot
  # cost a record.
  # The record sweep in `review_open_prs` collects the markers it graces itself;
  # this is the other path out. Found by `/code-review` of the branch that added
  # it, and its "every record" corrected by the next round of the other pass.
  rm -f "$REVIEWING_DIR"/.closed-*
  for marker in "$REVIEWING_DIR"/*; do
    [ -e "$marker" ] || continue
    # The records go too: a dispatcher starting fresh re-derives what has been
    # reviewed from the pull request itself, which is the only source that
    # cannot be stale.
    #
    # `.reviews`, `.fixes` and `.done` are the exceptions, and none is an
    # oversight. All three are properties of the PULL REQUEST rather than of
    # this dispatcher's run: how many of its two reviews and its one fix have
    # been bought, and the head the loop finished with. Clearing the two counts
    # would hand every open PR a fresh set on each drain, which is the ceilings
    # not existing for anybody who restarts the fleet. Clearing `.done` would
    # re-run the whole post-PR loop over every open pull request on every
    # dispatcher start -- cheap per PR, since the review exits in two API calls
    # once a verdict is on the head, and pure churn. `.done` is per HEAD, so a
    # push clears it by itself; all three are pruned when the PR closes, by the
    # sweep at the end of review_open_prs.
    case "$marker" in *.reviews|*.fixes|*.done) continue ;; esac
    is_review_record "$marker" && { rm -f "$marker"; continue; }
    held=""
    read -r held _ 2>/dev/null <"$marker" || true
    # ...and the same here, for the same reason: a hand-run that claims this
    # marker between the read and the removal must not have its live claim
    # deleted by a drain that was reaping somebody else's.
    if reviewer_alive "$held"; then
      kill "$held" 2>/dev/null && stopped=$((stopped + 1))
    fi
    fleet_lock_reap "$marker" "$held"
  done
  [ "$stopped" -gt 0 ] && echo "  stopped $stopped local reviewer(s)."
  return 0
}

# ----------------------------------------------------------- what is KEPT ---
#
# Nothing here deletes anything an agent or a person still needs. It deletes
# what has stopped being about anything: a transcript of a review that was
# answered three heads ago, on a pull request that merged yesterday.
#
# WHY THIS IS NOT JUST TIDINESS. Every one of these stores only ever grew --
# reviewer transcripts reached 184K across 46 files in under two days on the
# machine this was written on, 65% of them belonging to merged PRs -- and the
# cost is not the bytes. It is that stale state is read as current: a `reviewed-`
# record for a sha on no branch, or a transcript named for a head nobody is
# reviewing, answers a question nobody should be asking. See
# armaatus/autofleet#70.

# Reviewer transcripts. Keeps the newest AUTOFLEET_KEEP_REVIEWS per OPEN pull
# request and drops the rest, including every transcript of a PR that is no
# longer open. `$1` is the space-separated list of open PR numbers, which
# `review_open_prs` already has in hand, so deciding WHICH PRs to look at costs
# nothing. One `gh pr view <n> --json state` is then spent per candidate pull
# request per pass, and only after a grace pass -- see the comment at that call
# for why absence from `$1` is not an answer on its own.
#
# `$2` is whether that list is an ANSWER, and an empty list with `$2` = yes
# sweeps everything: that is a drained fleet, which is the peak of the pile this
# exists for. An empty list that is a FAILURE is `$2` = no and does nothing.
#
# This header used to say "costs no API call" and "with no list it does nothing",
# and both stopped being true when the confirmation call and the two-valued `$2`
# arrived. Found by the independent review -- the same class of stale claim this
# branch had already fixed once.
# The fetched PR refs of pull requests that are no longer open.
#
# `review.sh` fetches `refs/pull/<n>/head` into `refs/autofleet/review/<n>` so a
# delta round's range resolves, and drops it in its EXIT trap -- which does not
# run on a SIGTERM taken before the reviewer is spawned (the TERM trap is
# installed after it), nor on a SIGKILL from `stop.sh --now`, nor on an OOM. A
# ref left behind pins every object that pull request ever had, `git gc` can
# never collect them, and nothing else in the fleet looks at these at all. This
# is the only place that sees both the refs and the open list.
#
# ITS OWN FUNCTION, called past `prune_review_logs`'s `AUTOFLEET_KEEP_REVIEWS`
# gate rather than from inside it. That knob is documented as "keep every piece
# of review state", and a GC pin on a closed PR's objects is not review state --
# a project that sets 0 to keep its transcripts has not asked to keep those.
# Both found by the independent review.
#
# OPEN PRs ARE SPARED whatever their state, because a ref belonging to a
# reviewer running right now is the one thing this must not take: the range it
# was fetched for is resolved against it for the whole run. `$2` is the same
# "could the caller answer" signal the record sweep reads, so a listing nobody
# could answer for prunes nothing.

# WHAT THIS PASS MAY DO WITH PR $1's FILES -- a VERDICT and not a predicate, so
# the name says `if` is the wrong construct at a callsite. $2 is the directory
# its `.closed-` grace marker lives in. Two protections, and they are not
# decoration:
#
#   the GRACE PASS -- never prune on the pass a PR drops off the open listing.
#   A blip in the listing then costs a pass rather than a store.
#
#   the CONFIRMATION -- `gh pr view <n> --json state`, because the listing is
#   `gh pr list --author "@me"`, which is right for deciding whom to REVIEW and
#   wrong for "is this PR still open". On a host whose worktrees open PRs under
#   a different account than the dispatcher's `gh` login, every one of them
#   reads as closed.
#
# The transcript sweep had both and the record sweep had neither: it deleted as
# soon as a number was absent. The blast radius looked small -- records exist
# only for PRs that appeared in that listing -- but the file deleted is
# `<pr>.done`, and a `.done` deleted in error is the re-spawn loop
# armaatus/autofleet#42 exists to remove, back for a poll. One function now,
# rather than the record sweep growing a second copy that can drift.
# armaatus/autofleet#71.
#
#   0  confirmed closed, and this is not the first pass seeing it absent
#   1  keep it: first pass, or `gh` would not say. Anything but a confident
#      answer keeps the files AND keeps the grace, so a blip costs a pass.
#   2  it is OPEN -- the author-scoped listing was wrong about it. The grace is
#      dropped, so the next pass asks nothing and the question costs one call
#      every SECOND poll rather than every one.
#
# THE QUESTION RECURS, and that is chosen rather than overlooked. Keeping the
# marker on OPEN would ask on every poll; caching the answer for good would mean
# never noticing it had closed, and its files would outlive it. Halved and
# alternating is the bound, not "asked once".
#
# ASKED ONCE PER PULL REQUEST PER PASS, and `pr_state_once` below is what makes
# that true across BOTH sweeps rather than within each. The call used to
# sit inside the deleting loop, so the PR with thirteen transcripts #70 measured
# cost thirteen identical calls -- every poll, forever, because a PR that
# answers OPEN is never deleted and so never stops being a candidate. At the
# default 60s poll that is ~18,700 calls a day against the same budget
# `next_issue` and `review_open_prs` spend, and gh's secondary rate limit is how
# this dispatcher breaks.
pr_sweep_verdict() {
  local num="$1" dir="$2"
  if [ ! -e "$dir/.closed-$num" ]; then
    : >"$dir/.closed-$num"
    return 1
  fi
  # NOT `case "$(pr_state_once ...)"`: the memo is written by that function, and
  # a command substitution is a subshell, so every write would be discarded and
  # every caller would ask GitHub again. The answer comes back in a variable for
  # the same reason `count_parked_owned`'s warning had to leave stdout.
  pr_state_once "$num"
  case "$PR_STATE_ANSWER" in
    CLOSED|MERGED) return 0 ;;
    OPEN) rm -f "$dir/.closed-$num"; return 2 ;;
    *) return 1 ;;
  esac
}

# WHAT GITHUB SAYS PR $1 IS, asked at most once per pass however many sweeps ask.
#
# `pr_sweep_verdict` is called against TWO grace-marker stores every pass --
# `prune_review_logs` for `reviews/`, and the record sweep for
# `$REVIEWING_DIR` -- and the alternation the header above describes is per
# STORE. So on the passes they ask, one pull request cost two identical
# `gh pr view` calls: for a host whose PRs are opened under another account, and
# which therefore always answers OPEN, that is calls every poll per PR forever,
# which is the "every one" that paragraph promises to avoid. The bound is the
# point of asking at all. (It was three stores until armaatus/autofleet#152 took
# the validator's away; this paragraph has been wrong about the arithmetic once
# already, which is why it states the number rather than saying "each".)
#
# A VARIABLE and not a file, for $OPEN_PR_MEMO's reason, and dropped by
# `forget_poll_answers` with the rest of the pass's answers. Pure parameter
# expansion rather than a pipe: `printf | sed | head` under `pipefail` is the
# EPIPE race this file has measured twice.
#
# `-` is "gh would not say", memoised like any other answer: one outage is one
# call, and every caller in the pass treats it as "keep the files".
# THE ANSWER IS IN $PR_STATE_ANSWER, not on stdout, because the memo is the
# point: a function whose caller reads it through `$( )` runs in a subshell and
# every memo write it makes is thrown away.
PR_STATE_MEMO=" "
PR_STATE_ANSWER=""
pr_state_once() {
  local num="$1" rest
  case "$PR_STATE_MEMO" in
    *" $num="*) rest="${PR_STATE_MEMO#*" $num="}"; PR_STATE_ANSWER="${rest%% *}"; return 0 ;;
  esac
  PR_STATE_ANSWER="$(GH_PAGER=cat gh pr view "$num" --json state --jq .state 2>/dev/null)"
  case "$PR_STATE_ANSWER" in ''|*[!A-Za-z]*) PR_STATE_ANSWER="-" ;; esac
  PR_STATE_MEMO="$PR_STATE_MEMO$num=$PR_STATE_ANSWER "
}

# ONE TRANSCRIPT STORE NOW. There were two: `validate.sh` wrote
# `$FLEET_DIR/validations/` in exactly the shape `reviews/` uses, and for a
# while nothing pruned it -- a store that grows for as long as the fleet runs,
# which is the growth AUTOFLEET_KEEP_REVIEWS exists to stop. The validator is
# gone with armaatus/autofleet#152 and so is its store.
#
# $3 and $4 -- the directory, and the prefix its locks carry in $REVIEWING_DIR --
# stay parameters rather than becoming constants. They are what made the second
# store cost one call site instead of a second copy of this function, and the
# next store to appear is cheaper for the same reason. Defaulted, so the one
# call site reads as if they were not there.
prune_review_logs() {
  local open_prs="$1" dir="${3:-$FLEET_DIR/reviews}" lock="${4:-}" f base num kept orphans=0 ref
  [ "${AUTOFLEET_KEEP_REVIEWS:-0}" -gt 0 ] 2>/dev/null || return 0
  # `$2` is whether the caller COULD ANSWER, and it is separate from the list
  # because an empty list has two meanings and they are opposite instructions.
  #
  # A drained fleet whose last PR merged is a real empty answer, and the peak of
  # the pile this sweep exists for. But three things produce a wrongly-empty
  # list, and each would delete every transcript on the machine: the `python3`
  # parse between `gh` and here swallows everything (`except: pass`, and a
  # missing python3 or a `--json` shape change is indistinguishable from `[]`);
  # `--limit 50` pages, so an open PR past the page reads as closed; and
  # `--author "@me"` is right for deciding whom to REVIEW and wrong for "is this
  # PR still open", so a host whose worktrees open PRs under a different account
  # than the dispatcher's `gh` login loses everything.
  #
  # A blip self-heals through the grace pass. A systematic failure empties the
  # store, once, permanently. Found by the independent review -- and the header
  # of this function had promised this behaviour while the code did the reverse.
  local answered="${2:-no}"
  [ "$answered" = yes ] || return 0
  [ -d "$dir" ] || return 0
  # WHICH PRs ARE ELIGIBLE, decided per PULL REQUEST and in two steps, because
  # one step has the bug twice over: collect the closed numbers FIRST, then ask
  # about each one's marker. Done in a single pass, the first transcript of a PR
  # creates `.closed-N` and the second finds it already there -- so the PR is
  # "graced" on the same pass it was recorded, and every transcript but the
  # first goes in the same breath as the merge. That is the failure the grace
  # exists to prevent, and it survived one attempt at fixing it. Found by the
  # independent review.
  local removed=0 graced="" closed="" num_seen
  for f in "$dir"/pr-*.log; do
    [ -e "$f" ] || continue
    num_seen="$(basename "$f")"; num_seen="${num_seen#pr-}"; num_seen="${num_seen%%-*}"
    case " $open_prs " in *" $num_seen "*) continue ;; esac
    # A PR with a reviewer still writing is not eligible for anything: the
    # deleting loop skips it, and granting grace here would mean the pass after
    # the reviewer finishes deletes with no grace at all.
    if [ -e "$REVIEWING_DIR/$lock$num_seen" ]; then
      local held_seen=""
      read -r held_seen _ 2>/dev/null <"$REVIEWING_DIR/$lock$num_seen" || true
      reviewer_alive "$held_seen" && continue
    fi
    case " $closed " in *" $num_seen "*) ;; *) closed="$closed$num_seen " ;; esac
  done
  for num_seen in $closed; do
    # THE GRACE PASS AND THE CONFIRMATION, both of them `pr_sweep_verdict`'s since
    # armaatus/autofleet#71 -- the record sweep needed the same two and had
    # neither, and a second copy of them is what drifts. Why the grace pass and
    # the confirmation are both needed is `pr_sweep_verdict`'s header now, where
    # the code that applies them went; what is left here is what THIS sweep does
    # with each answer.
    pr_sweep_verdict "$num_seen" "$dir"
    case $? in
      0) graced="$graced$num_seen " ;;
      # An OPEN answer puts the PR BACK ON THE OPEN LIST for the rest of this
      # sweep, which is what it is. Leaving it a permanent candidate meant its
      # transcripts were never swept AND never trimmed either -- the newest-N cap
      # below iterates `$open_prs`, which by construction did not contain it --
      # so the one case the confirmation exists to protect was the one case that
      # grew without bound.
      2) open_prs="$open_prs $num_seen" ;;
    esac
  done

  for f in "$dir"/pr-*.log; do
    [ -e "$f" ] || continue
    base="$(basename "$f")"; num="${base#pr-}"; num="${num%%-*}"
    # NOT WHILE A REVIEWER FOR THAT PR IS RUNNING. `review.sh` writes `$log` as
    # a placeholder before it spawns and rewrites it at the end -- the file the
    # reviewer actually streams into is `$log.raw`/`$log.err`, since
    # armaatus/autofleet#65 split them so the JSON envelope could be parsed --
    # and all three stand for the whole run, up to AUTOFLEET_REVIEW_TIMEOUT,
    # thirty minutes, while the grace is one pass, a minute. A PR that auto-merges two minutes into its own review would have
    # had that review's output unlinked under the agent still writing it, and
    # the sweep would have SAID it swept it. `rotate_fleet_log` was given this
    # guard for the sibling file; this sweep was not. Found by `/code-review`.
    # A LIVE reviewer, asked the same way the rotation asks -- the comment
    # claimed parity and the code tested mere existence, so a marker left by a
    # SIGKILL blocked this PR's transcripts from ever being swept. Only a
    # definite 0 blocks, for the same reason as there: a 2-marker is never
    # cleared, so blocking on it is permanent. Found by the independent review.
    if [ -e "$REVIEWING_DIR/$num" ]; then
      local held_by=""
      read -r held_by _ 2>/dev/null <"$REVIEWING_DIR/$num" || true
      reviewer_alive "$held_by" && continue
    fi
    case " $open_prs " in
      *" $num "*) rm -f "$dir/.closed-$num"; continue ;;   # still open
    esac
    # PER PR, DECIDED BEFORE THIS LOOP. The marker used to be created inside it:
    # iteration one for a PR made `.closed-N` and continued, and iteration two
    # onward found it already there and deleted immediately -- so on the very
    # pass a PR dropped off the open list, ALL BUT ONE of its transcripts went,
    # and the survivor was whichever head-sha sorted first. #70 measured
    # thirteen transcripts on one PR; twelve would have gone in the same breath
    # as the merge, which is exactly what the grace exists to prevent and what
    # its own comment claimed it did. The `sweeps` phase could not catch it
    # because no PR there ever had two transcripts alive at once. Found by the
    # independent review.
    # `$graced` is "past its grace pass AND confirmed closed by gh", both
    # decided per pull request above. Everything else keeps its transcripts.
    case " $graced " in
      *" $num "*) ;;                # eligible since a previous pass: sweep it
      *) continue ;;                # first pass seeing it closed: keep them all
    esac
    rm_transcript "$f" && removed=$((removed + 1))
  done
  # ...and the newest N for each PR that IS open. `ls -t` is mtime order, which
  # is the order they were written.
  for num in $open_prs; do
    kept=0
    # `ls -t` through a `while read`, not a `for` over an unquoted substitution:
    # $FLEET_DIR is a documented knob and a space in it word-split the cap into
    # `rm -f` on fragments, so it silently stopped enforcing. Hard rule 3.
    while IFS= read -r f; do
      [ -n "$f" ] || continue
      kept=$((kept + 1))
      [ "$kept" -le "$AUTOFLEET_KEEP_REVIEWS" ] && continue
      rm_transcript "$f" && removed=$((removed + 1))
    done <<EOF
$(ls -t "$dir"/pr-"$num"-*.log 2>/dev/null)
EOF
  done
  # ...and the grace markers of PRs whose transcripts are all gone, so the
  # directory does not trade one kind of growth for another.
  for f in "$dir"/.closed-*; do
    [ -e "$f" ] || continue
    num="$(basename "$f")"; num="${num#.closed-}"
    ls "$dir"/pr-"$num"-*.log >/dev/null 2>&1 || rm -f "$f"
  done
  # ...and a context file with no log beside it, which is the one way the
  # pairing above can be escaped: a reviewer killed between writing its context
  # and starting leaves a context whose log was never written. `review.sh`
  # creates the log as a placeholder before it spawns precisely so that a LIVE
  # reviewer is never this case -- deleting a running review's context out from
  # under it is the failure this loop would otherwise be.
  # `.raw` and `.err` are in the list for the same reason one step further on:
  # `finish_log` folds them into the log and deletes them, but an exit that
  # skips the trap -- SIGKILL, the OOM killer -- leaves them, and no glob in
  # this function matched either. Found by `/code-review`.
  for f in "$dir"/pr-*.context.md "$dir"/pr-*.log.raw "$dir"/pr-*.log.err; do
    [ -e "$f" ] || continue
    case "$f" in
      *.context.md) base="${f%.context.md}.log" ;;
      *)            base="${f%.raw}"; base="${base%.err}" ;;
    esac
    [ -e "$base" ] || { rm -f "$f"; orphans=$((orphans + 1)); }
  done
  # SAID, not silent. A sweep nobody can see is one nobody can debug, and the
  # first question about a missing transcript is whether this took it. The
  # comment sat two blocks above the line it describes, which is the same drift
  # as a stale one. Found by the independent review.
  [ "$removed" -gt 0 ] && say "swept $removed reviewer transcript(s) no longer being answered"
  # COUNTED SEPARATELY, because they are not transcripts. Three files belong to
  # one round -- the log, its context and its two raw streams -- so folding the
  # strays into the number above made one orphaned round read as three swept
  # transcripts, on the one line a person reads to find out whether this took
  # the file they are looking for.
  [ "$orphans" -gt 0 ] && say "...and $orphans stray file(s) whose transcript is gone"

  return 0
}

# fleet.log, at AUTOFLEET_LOG_MAX_BYTES. ONE generation: the point is a bound,
# and two files at the cap is twice the cap.
#
# NOT WHILE A REVIEWER IS RUNNING, and that is the whole of the care here. The
# dispatcher's own writes are `tee -a`, fresh per call, so they follow a rename
# without noticing -- but `review_open_prs` spawns `review.sh ... >>"$LOG"`, and
# that redirect holds the INODE for up to AUTOFLEET_REVIEW_TIMEOUT, which is
# thirty minutes by default and many polls. Rotate under it and its output goes
# to `fleet.log.1`, invisible in the log anybody is reading; the next rotation
# then `mv -f`s over `fleet.log.1` and unlinks the file it is still writing to.
# The output is gone and nothing errors. An earlier version of this comment
# named the dispatcher as the long-lived holder; it is not. Found by the
# independent review.
#
# Waiting costs nothing: the cap is a bound on a log, not a deadline, and the
# reviewer that blocks the rotation is the thing writing most of what is in it.
rotate_fleet_log() {
  local max="${AUTOFLEET_LOG_MAX_BYTES:-0}" size live blind=""
  [ "$max" -gt 0 ] 2>/dev/null || return 0
  [ -f "$LOG" ] || return 0
  # UNDER THE CAP IS THE ANSWER FOR ALMOST EVERY POLL, so it is decided FIRST.
  # The size check used to run after the loop below, which meant a poll that was
  # never going to rotate anything still burned the say-once marker: an rc-2
  # holder plus a log at half the cap wrote `rotate-blind`, said the warning, and
  # then returned without renaming a thing. The real blind rotation, whenever it
  # came, was then silent -- the marker having been spent on a rotation that did
  # not happen. Found by the independent review.
  size="$(wc -c 2>/dev/null <"$LOG" | tr -d ' ')"
  case "$size" in ''|*[!0-9]*) return 0 ;; esac
  [ "$size" -gt "$max" ] || return 0
  # A LIVE REVIEWER, asked properly. Two things were wrong here and both were
  # silent. It called `is_review_record` when that predicate did not yet exist on
  # this branch, so `command not found` was swallowed by `2>/dev/null`, the
  # `&& continue` never fired, and EVERY file in the directory blocked rotation
  # while the dispatcher ran a nonexistent command once per file per poll. And
  # blocking on any marker rather than a live pid means rotation starves:
  # `fleet.sh` deliberately keeps a marker whose pid `ps` cannot identify, and
  # that one survives for the dispatcher's life, after which
  # AUTOFLEET_LOG_MAX_BYTES is not a bound at all. Found by `/code-review`, which
  # reproduced the 127.
  #
  # The predicate DOES exist now -- THIS PR adds it, a few hundred lines up; main
  # still carries only the comment saying it arrives with #42, which is what the
  # note above used to be. Said precisely because it is provenance for whoever
  # reverts this: reverting #42 takes the predicate with it and this loop must go
  # back with it. So this loop uses it rather than leaning on a record's first
  # field being a sha that
  # `reviewer_alive` happens to reject. That is the rule stated where
  # `is_review_record` is defined: use the predicate, do not respell the suffix
  # list or rely on what the contents happen to look like. This was the one loop
  # over the directory still doing neither, and the comment above it still said
  # the predicate was unavailable. Found by the independent review.
  if [ -d "$REVIEWING_DIR" ]; then
    for live in "$REVIEWING_DIR"/*; do
      [ -e "$live" ] || continue
      is_review_record "$live" && continue
      local held=""
      read -r held _ 2>/dev/null <"$live" || true
      # `reviewer_alive` is the fleet's own three-way answer: 0 ours, 1 dead,
      # 2 alive-but-ps-would-not-say. ONLY 0 BLOCKS.
      #
      # An earlier version blocked on 2 as well -- "the whole point is not to
      # rename an inode somebody still has open" -- and that reinstated exactly
      # the starvation the comment above names. A 2-marker is never cleared:
      # `live_reviewers` keeps it deliberately ("only a definite 1 clears it")
      # and `stop_reviewers` runs at dispatcher start, so one SIGKILLed reviewer
      # whose pid the OS reuses for something `ps` will not name blocks every
      # rotation for the life of the dispatcher, silently -- the `say` below is
      # never reached, because the function returned before the `mv`. The cap
      # stops being a bound at all.
      #
      # Weighing the two: blocking on 2 risks an unbounded log, forever, with no
      # message. Not blocking risks ONE rename under a writer nobody can
      # identify -- one generation, the file still on disk as fleet.log.1, and
      # the writer's fd still valid. The second is recoverable and the first is
      # not. It is said once, because rotating out from under something is worth
      # knowing about even when it is the better answer. Found by the
      # independent review, which noted the two comments contradicted and the
      # code implemented the losing one.
      reviewer_alive "$held"; local is=$?
      [ "$is" = 0 ] && return 0
      [ "$is" = 2 ] && blind="$held"
    done
  fi
  mv -f "$LOG" "$LOG.1" 2>/dev/null || {
    # SAID, AND SAID ONCE. A read-only state dir or an undeletable fleet.log.1
    # made this fail on every poll forever with nothing logged -- the opposite
    # of the rule stated forty lines above. Ungated, it then did the opposite of
    # the rule this whole function is: a directory `mv` cannot write to is still
    # a file `tee -a` can append to, so the one state where the cap CANNOT hold
    # was the one writing ~1440 lines a day into the log it is failing to bound.
    # Same shape and same marker rule as the warning below. Found by the
    # independent review.
    if [ -z "$ROTATE_STUCK_SAID" ]; then
      ROTATE_STUCK_SAID=1
      say "could not rotate $LOG past $max bytes; it will keep growing"
    fi
    return 0; }
  # ...and cleared by a rotation that works, so the next time it sticks it says
  # so. The condition is a directory permission somebody fixes while the fleet
  # runs, so "once per dispatcher" would otherwise be once per dispatcher even
  # after the operator had fixed and re-broken it.
  ROTATE_STUCK_SAID=""
  # SAID AFTER THE RENAME, into the file people read. `say` is `tee -a "$LOG"`,
  # so a warning printed before the `mv` was appended to the inode that became
  # `fleet.log.1` -- the one this very line says nobody reads. Found by the
  # independent review.
  if [ -n "$blind" ] && [ ! -e "$ROTATE_BLIND_SAID" ]; then
    : >"$ROTATE_BLIND_SAID"
    say "  rotated fleet.log with pid $blind holding it and ps unable to name it;"
    say "  its output may land in fleet.log.1 -- the alternative is never rotating"
  fi
  say "fleet.log passed $max bytes; the previous one is $LOG.1"
  return 0
}

review_open_prs() {
  # A stopped fleet writes nothing to a pull request, and the scripts below know
  # that -- they exit 3. But they exit 3 AFTER being spawned, once per open PR,
  # every poll: churn with an answer already known here. A DRAIN deliberately
  # does not stop this, because a drain lets the work in flight finish and a PR
  # waiting on a verdict is exactly that work.
  fleet_stopped && return 0
  mkdir -p "$REVIEWING_DIR"

  # OUR OWN PULL REQUESTS, and this is a limit rather than an oversight. The
  # reviewer is an agent with this machine's credentials reading a diff written
  # by somebody else; starting one unattended on every PR a repository receives
  # is a thing to opt into deliberately, not a side effect of running a fleet.
  # `@me` is the account gh is logged in as, which in the fleet's case is also
  # the account every worktree opens PRs with.
  #
  # ONE NUMBER, because the guard below only guards while it equals the limit.
  # Both were literal `50`, and a page size changed in one place would have left
  # the guard passing a truncated listing through as an answer -- which empties
  # the transcript store once, permanently, and that is the single failure the
  # `prs_answered` split exists to prevent. Hard rule 3.
  local listing pr_page=50
  listing="$(GH_PAGER=cat gh pr list --state open --author "@me" \
               --json number,isDraft,headRefOid --limit "$pr_page" 2>/dev/null)" || {
    say "could not list the open PRs; skipping the review pass"
    return 0; }

  # TWO ANSWERS, not one: the numbers, and whether the parse worked at all.
  # `except: pass` with `2>/dev/null` made a missing python3 and a real `[]` the
  # same string, and the sweep below treats them oppositely.
  local open_prs prs_answered=no
  open_prs="$(printf '%s' "$listing" | python3 -c '
import json, sys
print(" ".join(str(p["number"]) for p in json.load(sys.stdin)))
' 2>/dev/null)" && prs_answered=yes
  # ...and a listing that came back truncated is not an answer either: at the
  # page limit we cannot tell an absent PR from one on the next page.
  if [ "$(printf '%s' "$listing" | python3 -c '
import json, sys
print(len(json.load(sys.stdin)))
' 2>/dev/null)" = "$pr_page" ]; then
    prs_answered=no
  fi
  prune_review_logs "$open_prs" "$prs_answered"

  # Forget the runs that have finished, and count what is left. Both in one
  # function, because the count has to be RE-TAKEN inside the loop below: a
  # count taken only at the top of the pass is what starved the fourth pull
  # request when the first three each exited in two API calls having claimed a
  # slot for the whole pass.
  live_reviewers() {
    local m p n=0
    for m in "$REVIEWING_DIR"/*; do
      [ -e "$m" ] || continue
      is_review_record "$m" && continue
      p=""
      read -r p _ 2>/dev/null <"$m" || true
      # `kill -0` with a pid this could not read must never fall back to `0`,
      # which is not "no process" but THIS PROCESS GROUP -- the dispatcher and
      # every child it has -- and `kill -0 0` SUCCEEDS, which made an empty
      # marker immortal and its PR never reviewed again.
      # BY CONTENT, through `fleet_lock_reap`, never `rm -f "$m"` by path.
      # Between the `ps` above -- the slow part of this loop -- and the removal,
      # a fresh loop can claim the same marker under its own pid, and a
      # by-path delete then throws away a LIVE claim. The very same pass finds
      # `[ -e "$marker" ]` false and spawns a second loop on that head: that is
      # armaatus/autofleet#64, and the window is WIDER here than it was in the
      # reviewer this replaced, because `after-pr.sh` claims its own lock. The
      # helper exists for exactly this and its header says so. Found by the
      # independent review, which noticed the merge base called it here and
      # this did not.
      reviewer_alive "$p"; local is=$?
      if [ "$is" = 0 ]; then
        n=$((n + 1))
      elif [ "$is" = 1 ]; then
        fleet_lock_reap "$m" "$p"
      fi
      # rc 2 is "ps would not say", which is neither alive nor reapable: the
      # marker stands and the slot stays taken until something can answer.
    done
    printf '%s\n' "$n"
  }

  # ONE PER PULL REQUEST, and the same ceiling the worktrees have. Each of these
  # is a model call holding this machine's gh login; there is no reason the
  # post-PR half of the fleet should be able to run more of them at once than
  # the building half.
  local running; running="$(live_reviewers)"
  local pr head draft marker rpid published lockpid
  while read -r pr head draft; do
    [ -n "$pr" ] || continue
    # A DRAFT IS NOT READY TO BE JUDGED. It is open, so its transcripts and
    # records are spared above; it is not finished, so nothing spends a review
    # on it.
    [ "$draft" = true ] && continue
    fleet_stopped && return 0
    [ "$running" -ge "$AUTOFLEET_MAX" ] && break

    marker="$REVIEWING_DIR/$pr"
    # A LOCK THAT IS STILL THERE AFTER THE SWEEP ABOVE NAMES A LIVE LOOP.
    # `live_reviewers` removed every marker whose pid is gone, so presence here
    # is the answer. The claim itself is `after-pr.sh`'s -- it cannot be made
    # here, because a lock has to name a pid and there is no pid until the fork
    # (armaatus/autofleet#64).
    [ -e "$marker" ] && continue

    # `<pr>.done` is this PR's head-shaped record of "there is nothing left to
    # do here": written when `after-pr.sh` finishes with it, so a PR waiting on
    # a person is not re-examined every poll for the rest of its life. A head
    # move clears it, because the thing it recorded was about that commit.
    local done_head=""
    read -r done_head _ 2>/dev/null <"$marker.done" || true
    if [ "$done_head" = "$head" ]; then
      fleet_lock_release "$marker"
      continue
    fi

    # The whole post-PR loop for this PR, in one background process: arm the
    # merge, review, at most one fix, re-review, park. `after-pr.sh` carries the
    # reasoning and the ceilings; this decides only that there is a slot for it.
    #
    # EXEC'D DIRECTLY, NEVER WRAPPED IN A SUBSHELL, and the record-keeping that
    # would have needed a wrapper is passed to it instead. `( ... ) &` forks a
    # bash that keeps THIS script's argv, so `ps -o command=` on the pid the
    # lock holds reads `bash ./scripts/fleet/fleet.sh run --auto` --
    # `fleet_agent_alive` matches `after-pr|review|fix\.sh` and so answers 1,
    # "dead or a recycled pid". `live_reviewers` then deletes a LIVE lock every
    # poll, `fleet_lock_claim` finds no file, and a second loop starts beside
    # the first: armaatus/autofleet#64 exactly, plus two concurrent fix sessions
    # editing one worktree. `stop_reviewers` takes the same false branch, so
    # `stop --now` would skip the kill and orphan an agent holding this
    # machine's gh login -- the failure its own comment says it exists to
    # prevent. Found by the local /code-review pass, which reproduced the `ps`
    # output rather than reasoning about it.
    AUTOFLEET_PR_MARKER="$marker" AUTOFLEET_PR_HEAD="$head" \
      "$REPO_ROOT/scripts/fleet/after-pr.sh" "$pr" >>"$LOG" 2>&1 </dev/null &
    rpid=$!
    # BEST EFFORT, and the file is what is read back rather than the status.
    # The child claims the same lock for itself, so this succeeds only in the
    # window before it gets there -- and either way the marker ends up naming
    # `$rpid`, because the child's `$$` IS `$rpid`. What must not happen is an
    # unconditional write: a hand-run that won the window would have its claim
    # overwritten with a pid that is about to stand down.
    fleet_lock_publish "$marker" "$rpid" "$head"; published=$?
    lockpid=""
    read -r lockpid _ 2>/dev/null <"$marker" || true
    if [ "${lockpid:-}" = "$rpid" ]; then
      say "PR #$pr: working the post-PR loop at ${head:0:8} (pid $rpid)"
    elif [ "$published" = 2 ]; then
      say "PR #$pr: could not write $marker, so nothing here can stop a second"
      say "  loop starting beside this one. Check that directory is writable."
    else
      say "PR #$pr: a post-PR loop is already in flight (pid ${lockpid:-unknown});"
      say "  the one just spawned stands down."
    fi
    running="$((running + 1))"
  done <<EOF
$(printf '%s' "$listing" | python3 -c '
import json, sys
for p in json.load(sys.stdin):
    print(p["number"], p.get("headRefOid") or "", str(bool(p.get("isDraft"))).lower())
' 2>/dev/null)
EOF

  # ...and the records of pull requests that are no longer open. They were
  # pruned ONLY by `stop_reviewers`, so a merged PR's records sat here until the
  # next dispatcher start -- days, on a fleet that stays up. A LOCK is never
  # pruned here: a run on a PR that just merged still owns its pid and its slot,
  # and `live_reviewers` is what reaps it.
  #
  # NOT on an empty answer. `open_prs` is empty both when no PR is open and when
  # the parse above failed, and those are opposite instructions.
  [ -n "${open_prs:-}" ] || return 0
  # ...and NOT on a truncated one either: the same listing that is too weak to
  # prune a transcript is too weak to prune these.
  [ "${prs_answered:-no}" = yes ] || return 0
  # ...and NOT on the pass a number drops off the listing, nor without asking
  # GitHub. Both are `pr_sweep_verdict`'s. DECIDED ONCE PER PULL REQUEST, not
  # once per record: a PR carries several, and the confirmation is a `gh` call.
  local rec base num verdict_keep="" verdict_go=""
  for rec in "$REVIEWING_DIR"/*; do
    [ -e "$rec" ] || continue
    is_review_record "$rec" || continue
    base="$(basename "$rec")"; num="${base%%.*}"
    # ...AND THE GRACE GOES WITH IT, the way the transcript sweep's does. A PR
    # that drops off the listing, comes back, and later closes for good would
    # otherwise find its marker already there and lose the grace pass entirely.
    case " ${open_prs:-} " in
      *" $num "*) rm -f "$REVIEWING_DIR/.closed-$num"; continue ;;
    esac
    case " $verdict_keep " in *" $num "*) continue ;; esac
    case " $verdict_go "   in *" $num "*) rm -f "$rec"; continue ;; esac
    # `case $?`, not `if`: `pr_sweep_verdict` has THREE answers and only one of
    # them deletes. Read as a boolean the OPEN answer disappears into an `else`
    # that happens to do the right thing, which is a callsite that stops saying
    # what it knows.
    pr_sweep_verdict "$num" "$REVIEWING_DIR"
    case $? in
      0) verdict_go="$verdict_go$num "; rm -f "$rec" ;;
      # 1 is "first pass, or gh would not say" and 2 is "GitHub says it is
      # open". Both keep the records; only the second is a statement about the PR.
      *) verdict_keep="$verdict_keep$num " ;;
    esac
  done
  for num in $verdict_go; do rm -f "$REVIEWING_DIR/.closed-$num"; done
}

# ---------------------------------------------------------------- the reap ---
# A worktree whose PR is merged has done its job and is holding a slot. Only ones
# this dispatcher created are touched, and only when nothing goes with them:
# nothing unpushed, and a clean working tree. Its docker stack has to come down
# with it, or it survives under `restart: unless-stopped` holding two ports
# forever with nothing left on disk to identify it by -- but AFTER the removal,
# not before it. See remove_worktree.
# Remove a worktree and, only if it really went, sweep what it left behind.
#
# The three answers -- gone, refused, no answer -- come from the driver, which is
# where the how of a removal now lives: the force retry, and the reason the
# runner's own archive hook is never asked for. What is left here is the ORDER,
# which is this dispatcher's decision rather than the runner's.
#
# armaatus/rommsync-nx#27 logged "could not remove it" at 02:08 and kept the
# slot; the very same
# command, run again by hand, removed it and printed
# `warning: local branch "..." was kept because Git could not safely delete it`.
# A non-zero exit has meant both "nothing happened" and "it worked, with a
# caveat", and the fleet cannot tell those apart from the code alone -- which is
# why `runner_worktree_remove` answers on the filesystem instead.
#
# This matters more than one stuck worktree. The fleet runs at a cap of three,
# and a slot held by a worktree whose work is already merged is a slot that never
# starts the next issue -- the loop quietly runs at two, then one.
#
# The teardown runs AFTER the removal, never before it
# (armaatus/rommsync-nx#163): a hook run first takes the stack down and then
# leaves it down when the removal refuses, and armaatus/rommsync-nx#122 lost its
# RomM mid-ctest exactly that way. A refusal here changes nothing. Spelled with
# the repository because both numbers resolve to unrelated issues in autofleet,
# which is what CLAUDE.md's citation rule is for -- and a bare `(#163)` was
# already corrected once on this branch.
#
# The sweep is reap.sh, which is exactly the tool for "a stack whose worktree no
# longer exists" and needs no worktree to run -- the README names it as the manual
# counterpart of this same trade. Scoped with `--only` to the names read off the
# worktree before it went, so releasing one worktree does not also delete the
# database of an orphan somebody is still looking at; `--only` narrows reap.sh's
# stale set and can never widen it.
#
# Returns 0 when the worktree is gone, 2 when the runner never answered, and 1
# when it answered and refused. The caller acts on the difference: a refusal is a
# decision about THIS worktree and is not worth retrying, while a deadline is the
# runtime restarting and says nothing about the worktree at all.
remove_worktree() {
  local path="$1" out projects env_project rc
  # The deadline the removal gets, LOCAL rather than a constant beside the other
  # tunables: armaatus/rommsync-nx exercises this function by extracting it with
  # `sed` and sourcing it alone, so anything it reads from the file around it
  # arrives empty -- and an empty deadline is not 180, it is zero.
  local deadline="${AUTOFLEET_RM_DEADLINE:-180}"
  # THE WORKTREE'S OWN TEARDOWN, before anything is destroyed and from INSIDE
  # it -- the project's teardown hook and whatever stack `.autofleet/config`
  # named. The app-backed runner used to run this through `orca.yaml`'s archive
  # hook; a driver with no hook mechanism ran it never, and a host project's
  # containers would have outlived every worktree the fleet released.
  #
  # Never fatal, and bounded: this is teardown, and a hook that hangs must not
  # take the removal with it. `finish_removal` sweeps the stack by name
  # afterwards for whatever this did not reach.
  if [ -x "$path/scripts/fleet/archive.sh" ]; then
    # BOUNDED, as the paragraph above claims. Run inline it was not: a project
    # teardown hook that never returns held the whole poll loop, and the sweep
    # in `finish_removal` two screens down already goes through the watchdog for
    # exactly that reason. Found by the local `/code-review` pass.
    fleet_run_with_deadline "$deadline" /dev/null \
      env -C "$path" ./scripts/fleet/archive.sh || true
  fi
  # Pure reads, before anything can be destroyed. Both live INSIDE the worktree
  # and the sweep below needs them after it is gone.
  # Both names archive.sh would have used. The derived one is what setup.sh
  # would have called this worktree; the one in .env is what its stack was
  # actually created under, and after a directory rename the two disagree with
  # the containers still running under the older name.
  projects=""
  fleet_derive_env "$path" && projects="$fleet_project"
  env_project="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' "$path/.env" 2>/dev/null | tail -1)"
  case " $projects " in
    *" $env_project "*) ;;
    *) [ -n "$env_project" ] && projects="$projects $env_project" ;;
  esac
  out="$(mktemp)"
  runner_worktree_remove "$path" "$deadline" >"$out" 2>&1
  rc=$?
  if [ "$rc" = 0 ]; then
    rm -f "$out"
    # The other half of `launch`'s drop: this worktree is no longer in the list,
    # so the pass's cached copy -- and `foundation_in_flight`'s answer read off
    # it -- describes a world that no longer exists. Only on rc 0: a refusal and
    # a runner that never answered both leave the worktree standing.
    forget_worktree_answers
    finish_removal "$path" "$projects"
    return 0
  fi
  if [ "$rc" = 2 ]; then
    rm -f "$out"
    # No "in ${deadline}s": rc 2 is also a runner that could not be reached at
    # all, which returns immediately. Naming seconds never spent is worst on the
    # one path where a person is being asked to tell a restarting app from a
    # wedged one. Found by the independent review.
    say "  the runner did not answer -- nothing was torn down, and this is not a refusal"
    return 2
  fi
  # Labelled, because the caller's "could not remove it" comes after these and
  # an unlabelled fatal: line above it reads like the fleet's own.
  while IFS= read -r line; do
    [ -n "$line" ] && say "  the removal refused: $line"
  done <"$out"
  # The line armaatus/rommsync-nx#122 needed and did not get. Not "the worktree
  # is still usable":
  # reap_abandoned interrupts the agent immediately before calling this, so on
  # that path somebody has just been stopped. What is true on both paths is that
  # the removal changed nothing, which is the fact that saves the next test run.
  say "  nothing was torn down -- its stack is still up, so whatever is in there still has its rig"
  rm -f "$out"
  return 1
}

# What --run-hooks used to do, run only once the worktree is established to be
# gone: take down the stack it left behind. It used to stop the autostart
# watcher too -- the process that pressed Return on a drafted prompt -- and
# there is no such process now.
# Named for the moment rather than for the hook, because it is no longer a hook
# and no longer runs inside the worktree it is tearing down.
#
# Never fatal: the worktree IS removed by the time this is called, so a docker
# that is down or a sweep that half-finished is a stack to collect later, not a
# removal to report as failed.
finish_removal() {
  local path="$1" projects="$2" name out rc only=""
  # Scoped to the names read off this worktree before it went, so releasing ONE
  # worktree does not also delete the database of an orphan somebody is still
  # looking at. Unscoped only when neither name could be read at all -- an
  # unswept stack comes back on every `docker start` holding two ports, with
  # nothing left on disk to identify it by, so that failure resolves towards
  # sweeping rather than towards leaking.
  for name in $projects; do only="$only --only $name"; done
  [ -n "$only" ] \
    || say "  could not name $path's stack; sweeping every rmx-* stack with no worktree"
  # On a deadline, and this is the reason the sweep is not simply run inline: the
  # archive hook used to be Orca's problem and inherited the 180s above, while a
  # `docker compose down` against a wedged daemon has no timeout of its own. The
  # dispatcher polls every minute and would otherwise stop polling for as long as
  # docker stayed stuck.
  out="$(mktemp)"
  # $only is a list of `--only <project>` pairs this function built itself, and a
  # project name is [a-z0-9-] by construction, so the split is the point.
  # shellcheck disable=SC2086
  FLEET_RUN_CAPTURE_STDERR=1 fleet_run_with_deadline 180 "$out" \
    "$REPO_ROOT/scripts/fleet/reap.sh" --yes $only
  rc=$?
  cat "$out" >>"$LOG"
  rm -f "$out"
  # Said rather than swallowed, because a sweep that did not finish is two ports
  # and four volumes coming back on every `docker start`, with no worktree left
  # on disk to identify them by.
  [ "$rc" = 0 ] \
    || say "  $path is gone, but the stack sweep did not finish (rc $rc) -- see $LOG, then ./scripts/fleet/reap.sh"
  return 0
}

# What both reaps do when a removal did not happen. `rc` is remove_worktree's:
# 2 means nobody answered, and that is not a decision about this worktree, so it
# is said once and retried on the next pass rather than parked.
#
# A parked worktree is NOT disowned. Both reaps iterate OWNED issues only, so an
# issue dropped on a failed removal is a worktree nothing ever looks at again --
# armaatus/rommsync-nx#122's stood from 16:49 until a person removed it. It keeps
# its slot, which is
# why both lines say so.
#
# The by-hand recovery is TWO commands, and the second is the one that is easy to
# forget: removing the directory makes the next pass disown the issue, and
# nothing then ever calls finish_removal for it. Its stack would come back on
# every `docker start` under `restart: unless-stopped`, holding two ports with no
# directory left to identify it by -- and the sweep is `--only`-scoped now, so no
# later removal collects it either.
BY_HAND_REMOVAL="git worktree remove --force '%s' && ./scripts/fleet/reap.sh --yes"
park_worktree() {
  local num="$1" path="$2" rc="$3" what="$4" byhand
  # BY_HAND_REMOVAL is this file's own format string, not anything a caller sets.
  # shellcheck disable=SC2059
  byhand="$(printf "$BY_HAND_REMOVAL" "$path")"
  if [ "$rc" = 2 ]; then
    [ -e "$STATE_DIR/runner-blind-$num" ] && return 0
    : >"$STATE_DIR/runner-blind-$num"
    say "  could not ask -- leaving it owned, and trying again next pass"
    card "$path" comment "#$num: $what, but the runner did not answer -- retrying"
    return 0
  fi
  rm -f "$STATE_DIR/runner-blind-$num"
  : >"$STATE_DIR/stuck-$num"
  # ...and the two re-derived markers go with it. After a refused removal they
  # describe a question already answered: `held-` and `git-blind-` mean "the
  # worktree holds something, or git would not say", and a removal that git
  # itself refused has settled that. Left behind they are PERMANENT -- both reaps
  # return early on `stuck-`, so nothing ever clears them -- and they then gate
  # a worktree that is plainly waiting for a person. Found by the independent
  # review.
  rm -f "$STATE_DIR/held-$num" "$STATE_DIR/git-blind-$num"
  say "  could not remove it; it keeps its slot until you do: $byhand"
  card "$path" comment "#$num: $what, but the removal refused -- still here, still counted"
  return 0
}

reap_merged() {
  local f num path branch merged unpushed dirty holds blind rc
  for f in "$OWNED_DIR"/*; do
    [ -e "$f" ] || continue
    num="$(basename "$f")"; path="$(cat "$f")"
    [ -d "$path" ] || { disown_issue "$num"; continue; }
    # Already attempted once, and REFUSED -- a CLI that never answered does not
    # get here, see park_worktree. Not retried: a refusal is a decision about
    # this worktree, so retrying it is a board comment once a minute under
    # whoever is still working in there, and an interruption with it down
    # reap_abandoned's path.
    #
    # ONE marker shared with reap_abandoned, deliberately. It does not record
    # which reap tried; it records that a removal was attempted here and refused,
    # and the recovery is the same two commands whichever one asked. Two markers
    # would buy a worktree already waiting on a person a second card saying so.
    # Ahead of the `gh pr list` below for the same reason: nothing about the
    # answer would change what happens.
    [ -e "$STATE_DIR/stuck-$num" ] && continue
    branch="$(git -C "$path" rev-parse --abbrev-ref HEAD 2>/dev/null)" || continue
    merged="$(GH_PAGER=cat gh pr list --head "$branch" --state merged \
                --json number --jq '.[0].number' 2>/dev/null)"
    [ -n "$merged" ] && [ "$merged" != "null" ] || continue

    # What is in there that a removal would take with it. Two questions, because
    # a merged PR answers neither on its own.
    #
    # `@{u}..HEAD` is the older one. The WORKING TREE is the half
    # armaatus/rommsync-nx#122 fell through (and armaatus/rommsync-nx#163 caught):
    # auto-merge fires the moment the last check passes, so review fixes made
    # after it -- that one's became armaatus/rommsync-nx#160 -- sit uncommitted
    # here while `@{u}..HEAD` is empty.
    #
    # Not worktree_holdings, which reap_abandoned uses for the same job: that
    # also asks what is absent from origin/main, and a SQUASH merge leaves every
    # commit on this branch absent from it by construction. Asked here, no merged
    # worktree would ever be released again.
    #
    # And a git that cannot answer is the THIRD answer. Reading it as "clean" is
    # how this guard fails open on the one thing it exists to protect, so it is
    # refused rather than guessed -- reap_abandoned's discipline exactly.
    holds=""; blind=0
    # The EXIT STATUS, not just the count. `grep -c` prints 0 and succeeds when
    # git printed nothing -- including when it printed nothing because `@{u}`
    # does not resolve. GitHub deletes the head branch on merge, a `fetch
    # --prune` in the worktree drops `origin/<branch>`, and from then on this
    # read "holds nothing" for a worktree that may hold a commit made after
    # auto-merge fired. Clean tree plus that answer is a `--force` removal, and
    # the commit goes with the directory.
    #
    # The comment four lines up already said this is the third answer and must
    # be refused rather than guessed; the discipline was applied to
    # `worktree_dirty_count` below and skipped here. armaatus/autofleet#34.
    if unpushed="$(git -C "$path" log '@{u}..HEAD' --oneline 2>/dev/null)"; then
      unpushed="$(printf '%s' "$unpushed" | grep -c . || true)"
      [ "${unpushed:-0}" != 0 ] && holds="$unpushed unpushed commit(s)"
    else
      blind=1
    fi
    if ! dirty="$(worktree_dirty_count "$path")"; then
      blind=1
    elif [ "${dirty:-0}" != 0 ]; then
      [ -n "$holds" ] && holds="$holds and "
      holds="$holds$dirty uncommitted change(s)"
    fi

    # Two keeps, two markers, and each clears the other -- reap_abandoned's
    # `held-`/`git-blind-` pair, for the same reason: one marker for both would
    # mean whichever fired first silenced the other for good. Said once per
    # worktree rather than once per poll, because a worktree this pass decided to
    # keep is one the next pass in sixty seconds will decide to keep again, and
    # the board card carries the standing state either way.
    #
    # Their own markers rather than reap_abandoned's: that function clears
    # `held-` and `git-blind-` whenever an issue has no reason to be released,
    # which for a MERGED issue is every single pass.
    if [ "$blind" = 1 ]; then
      rm -f "$STATE_DIR/merge-held-$num"
      [ -e "$STATE_DIR/merge-blind-$num" ] && continue
      : >"$STATE_DIR/merge-blind-$num"
      say "#$num: PR #$merged merged, but git could not say what the worktree holds -- leaving it"
      card "$path" comment "#$num: PR #$merged merged; kept -- git could not say what is in it"
      continue
    fi
    if [ -n "$holds" ]; then
      rm -f "$STATE_DIR/merge-blind-$num"
      [ -e "$STATE_DIR/merge-held-$num" ] && continue
      : >"$STATE_DIR/merge-held-$num"
      say "#$num: PR #$merged merged, but the worktree holds $holds -- leaving it"
      card "$path" comment "#$num: PR #$merged merged, $holds here"
      continue
    fi
    rm -f "$STATE_DIR/merge-held-$num" "$STATE_DIR/merge-blind-$num"

    say "#$num: PR #$merged is merged; marking it done and removing the worktree"
    card "$path" workspace-status completed comment "#$num: merged in PR #$merged"
    remove_worktree "$path"; rc=$?
    if [ "$rc" = 0 ]; then
      disown_issue "$num"
    else
      park_worktree "$num" "$path" "$rc" "merged in PR #$merged"
    fi
  done
}

# ------------------------------------------------------ the other releases ---
# `reap_merged` above is the only thing that ever removed a worktree, and it
# removes one only when a PR for that branch has MERGED. Every other way an issue
# can stop being worked left the worktree standing, owned, and counted against
# the cap of three -- forever. Two were cleared by hand on 2026-09-07, each
# holding four containers, two ports and four volumes: armaatus/rommsync-nx#44,
# which the time-box stopped at three hours for correctly producing nothing, and
# armaatus/rommsync-nx#148, which the maintainer blocked with its worktree open,
# keeping armaatus/rommsync-nx#119, armaatus/rommsync-nx#122 and
# armaatus/rommsync-nx#139 queued behind work that could never start.
#
# The guard has to be its own, because "the issue went blocked" carries none of
# the guarantee "the PR merged and nothing is unpushed" does: a worktree
# abandoned mid-change may hold the only copy of real work. So this refuses
# rather than guesses, on exactly the pair verified by hand before each of those
# removals -- `git status --porcelain` empty AND nothing absent from origin/main.
#
# The pair answers "is anything here worth keeping". It does not answer "is
# anyone using this", and every by-hand check behind it was made on a worktree
# that was already finished -- so it was never once evaluated against a live
# agent mid-change. The working agreement is where the two come apart: an agent plans before
# it edits, so a worktree forty minutes into real work is legitimately EMPTY. And `blocked` is not a label a person types --
# unblock.yml re-derives it on every merge, so it can arrive under an agent that
# is mid-plan, as `needs-human-step` can arrive from the agent's own hand.
#
# So the first pass that finds a reason WARNS: it interrupts the agent, says on
# the board that the worktree goes next pass, and leaves it. The pass after that
# re-asks the pair -- a minute is long enough to commit, or to write a plan down
# -- and only then removes it. That is the whole guard against deleting an
# afternoon, and it needs no lookup of agent state: an agent that has nothing on
# disk after being told is one that had nothing to lose but a prompt.

# How many lines `git status --porcelain` has, which is what both reaps mean by
# "dirty". NON-ZERO when git could not answer, because zero would be the answer
# that releases a worktree -- both callers refuse instead. `|| true` after the
# count because `grep -c` exits 1 on no matches, and a clean tree is an answer.
worktree_dirty_count() {
  local out
  out="$(git -C "$1" status --porcelain 2>/dev/null)" || return 1
  printf '%s' "$out" | grep -c . || true
}

# What removing this worktree would destroy. Prints one phrase naming it, or
# nothing at all when there is nothing. Non-zero means it could not tell, which
# is NOT the same answer: a git that cannot speak is no basis for deleting
# somebody's afternoon.
#
# origin/main is read as it stands and never fetched. A stale one only ever makes
# commits look ABSENT that are in fact merged, so every error it can cause is in
# the direction of KEEPING a worktree -- and a fetch per worktree per poll is a
# network call this loop does not need.
worktree_holdings() {
  local path="$1" dirty ahead out parts=""
  git -C "$path" rev-parse --verify --quiet HEAD >/dev/null 2>&1 || return 1
  git -C "$path" rev-parse --verify --quiet origin/main >/dev/null 2>&1 || return 1
  dirty="$(worktree_dirty_count "$path")" || return 1
  out="$(git -C "$path" log --oneline origin/main..HEAD 2>/dev/null)" || return 1
  ahead="$(printf '%s' "$out" | grep -c .)"
  [ "${dirty:-0}" != 0 ] && parts="$dirty uncommitted change(s)"
  if [ "${ahead:-0}" != 0 ]; then
    [ -n "$parts" ] && parts="$parts and "
    parts="$parts$ahead commit(s) that are not in origin/main"
  fi
  printf '%s' "$parts"
}

# Runs AFTER reap_merged in every pass, and the order is not decoration: a
# worktree whose PR merged is reap_merged's, and by the time this runs it has
# already been disowned. What is left here is what will never merge.
reap_abandoned() {
  local f num path answer reason holds asked rc
  for f in "$OWNED_DIR"/*; do
    [ -e "$f" ] || continue
    num="$(basename "$f")"; path="$(cat "$f")"
    [ -d "$path" ] || { disown_issue "$num"; continue; }
    # Already tried once, and the removal refused. Not retried: the removal
    # interrupts the agent and writes to the board, so a retry loop is both, once
    # a minute, under whoever is still working in there. (It is no longer a stack
    # torn down once a minute -- that was the ordering defect, fixed in
    # remove_worktree for #163 -- but the noise alone is reason enough.)
    [ -e "$STATE_DIR/stuck-$num" ] && continue

    reason=""; asked=1
    if answer="$(poll_issue "$num")"; then
      rm -f "$STATE_DIR/reason-blind-$num"
      # "Closed", and not "closed with no merged PR": reap_merged asks that
      # question and this one does not, so the two ways it leaves a closed issue
      # owned -- unpushed commits, and a `gh pr list --head` that could not
      # answer -- would make the fuller sentence a false one in the line a person
      # reads. The release itself is unaffected: both of those leave something in
      # the worktree, or leave everything already in origin/main.
      if [ "$(issue_state_in "$answer")" = CLOSED ]; then
        reason="its issue is closed"
      elif has_label "$(issue_labels_in "$answer")" "$BLOCKED_LABEL"; then
        reason="its issue went $BLOCKED_LABEL"
      elif has_label "$(issue_labels_in "$answer")" "$HUMAN_STEP_LABEL"; then
        reason="its last step is yours ($HUMAN_STEP_LABEL)"
      fi
    else
      asked=0
    fi
    # `build_exited`'s own record, so this one still answers through a GitHub
    # outage -- and so a lookup that failed above cannot be read as "no reason".
    [ -n "$reason" ] || ! gave_up_on "$num" || reason="its build ran out with no PR"
    # No reason today, so everything a previous poll decided goes with it. The
    # reason is re-derived from a live lookup every pass and can genuinely come
    # and go: unblock.yml re-writes `blocked` on every merged PR, so an issue can
    # go blocked, be warned, come off `blocked` when the dependency lands, and go
    # blocked again on the next one. A `warned-` left standing across that is a
    # worktree removed on the second occurrence with no notice at all -- which is
    # the one thing the warning pass exists to prevent. `stuck-` is not in this
    # list: it means a removal was attempted and refused, and it is checked above
    # this point precisely so that it is never retried.
    # A lookup that could not answer is the THIRD answer, not "no reason", and
    # the difference is the whole discipline this function is built on --
    # worktree_holdings failing gets `git-blind-` rather than being read as
    # "holds nothing". Folded into the branch below, one `gh` hiccup looks
    # exactly like the issue coming off `blocked`: the warning still owed to a
    # reason nobody could read is discarded, the next poll starts the notice
    # over, and under a flaky GitHub the release never arrives with nothing
    # anywhere saying why. So every marker stays exactly where it was, and it is
    # said once per outage rather than once per poll.
    if [ -z "$reason" ] && [ "$asked" = 0 ]; then
      [ -e "$STATE_DIR/reason-blind-$num" ] && continue
      : >"$STATE_DIR/reason-blind-$num"
      say "#$num: could not read its issue -- leaving it, and every marker on it, until GitHub answers"
      continue
    fi
    if [ -z "$reason" ]; then
      rm -f "$STATE_DIR/warned-$num" "$STATE_DIR/held-$num" "$STATE_DIR/git-blind-$num"
      continue
    fi

    # Two keeps, two markers, and each clears the other. One marker for both
    # would mean whichever fired first silenced the other for good: a transient
    # git failure on one poll, then three uncommitted files on the next, and the
    # board still saying git could not be read -- the opposite of the promise
    # that it says WHAT is in there.
    #
    # Either way it is said once per worktree, not once per poll: the dispatcher
    # polls every minute, and a worktree it decided to keep is one it will decide
    # to keep again in sixty seconds. `warned-` goes too, so a worktree that
    # becomes empty again is offered the same pass of notice as any other.
    if ! holds="$(worktree_holdings "$path")"; then
      rm -f "$STATE_DIR/held-$num" "$STATE_DIR/warned-$num"
      [ -e "$STATE_DIR/git-blind-$num" ] && continue
      : >"$STATE_DIR/git-blind-$num"
      say "#$num: $reason, but its git state could not be read -- leaving it"
      card "$path" comment "#$num: $reason; kept -- git could not say what is in it"
      continue
    fi
    if [ -n "$holds" ]; then
      # It says WHAT is in there, the way reap_merged already reports unpushed
      # commits rather than removing them.
      rm -f "$STATE_DIR/git-blind-$num" "$STATE_DIR/warned-$num"
      [ -e "$STATE_DIR/held-$num" ] && continue
      : >"$STATE_DIR/held-$num"
      say "#$num: $reason, but the worktree holds $holds -- leaving it"
      card "$path" comment "#$num: $reason; kept -- it holds $holds"
      continue
    fi
    rm -f "$STATE_DIR/held-$num" "$STATE_DIR/git-blind-$num"

    # The warning pass. One poll of notice, then the pair is asked again above --
    # so an agent that commits, or writes its plan to a file, keeps its worktree.
    if [ ! -e "$STATE_DIR/warned-$num" ]; then
      # THE WARNING PASS STILL EXISTS, and what it is for changed. It used to
      # buy the agent a turn to write a handoff note before its terminal went
      # (armaatus/autofleet#106); there is no note now, because the branch is
      # the note. What it still buys is the thing that always mattered more: one
      # more poll in which a commit can land, which is what the `holds` check
      # above reads on the next pass.
      #
      # The build is stopped HERE rather than at the removal, so the poll of
      # grace is a poll in which nothing new is being written into a directory
      # that is about to go.
      : >"$STATE_DIR/warned-$num"
      say "#$num: $reason, and the worktree holds nothing -- releasing it next pass unless something lands in it"
      stop_build_in "$path" >/dev/null || say "#$num: its build would not stop; the removal next pass will race it"
      card "$path" comment "#$num: $reason; this worktree is released next pass unless something lands in it"
      continue
    fi

    say "#$num: $reason, and the worktree still holds nothing -- releasing the slot"
    # Stopped again before the removal: a build that somehow restarted would
    # otherwise keep writing into a directory being deleted, and lose its rig
    # with it the moment the removal lands (#163).
    stop_build_in "$path" >/dev/null || say "#$num: its build would not stop; removing the worktree anyway"
    # The comment and no status. `completed` is reap_merged's word for work that
    # landed, and this worktree is being released precisely because it did not.
    # Phrased as what it is about to do, not as done: if the removal refuses, this
    # card is still on the board and still the line a person reads.
    card "$path" comment "#$num: $reason; nothing is in it, removing the worktree"
    remove_worktree "$path"; rc=$?
    if [ "$rc" = 0 ]; then
      disown_issue "$num"
    else
      park_worktree "$num" "$path" "$rc" "$reason"
    fi
  done
}

# A give-up record for an issue that has since landed is a line in `fleet.sh
# status` about work that is done, and a file that never goes away -- the same
# leak `stalled-` had before own() started clearing it. The release path disowns
# the issue, so nothing else will ever look at it again; this is what does.
#
# One `gh issue view` per record per poll, shared with the watchers through
# $POLL_CACHE, and self-limiting: the records it can find are the ones it removes.
prune_gaveup() {
  local n answer
  for n in $(gave_up_issues); do
    answer="$(poll_issue "$n")" || continue
    [ "$(issue_state_in "$answer")" = CLOSED ] || continue
    rm -f "$STATE_DIR/gaveup-$n"
    say "#$n: closed since the fleet gave up on it -- dropping the record"
  done
}

# THE BUILD'S WALL CLOCK, asked once per running build per poll. 0 when the
# build has been stopped and the caller should now read its exit; non-zero when
# there is nothing to do -- inside its clock, an age that cannot be told, or a
# stop that did not stop.
#
# STOPPED THROUGH THE RUNNER CONTRACT rather than with a signal of its own. The
# Orca driver has no pid to aim at, so a build is stopped the way it is started;
# `stop_build_in` is the same call `cmd_stop --now` makes, and its answer means
# the same thing here -- 0 is "nothing of that build is left", non-zero prints
# the pid that survived (#163). A clock that assumed the kill worked would
# record a build still holding its worktree as given up on, and the reaper would
# then take the worktree out from under it.
#
# NO `rc` AND NO `gaveup-` WRITTEN HERE. The stop leaves a `stopped` marker, the
# state reader renders it, and `build_exited` routes it to `build_stopped` --
# which is what records the issue as given up on, once, locally, without
# commenting on GitHub. A build the fleet stopped on purpose is not a build that
# ran out of budget, and the marker is what keeps those two apart (#164). So the
# only thing this adds to that path is the line saying which bound ended it.
build_over_clock() {
  local num="$1" path="$2" age dir left
  age="$(fleet_build_age_of "$path")" || return 1
  [ "$age" -ge "$AUTOFLEET_BUILD_TIMEOUT" ] || return 1
  if left="$(stop_build_in "$path")"; then
    # THE MARKER IS WRITTEN HERE TOO, and idempotently: the headless driver
    # already wrote one, and a driver whose stop leaves no record at all --
    # Orca's interrupts and closes, and has no `rc` to write -- would otherwise
    # leave the build reading `running` after a stop that worked, so this clock
    # would fire again every poll for the rest of the night.
    dir="$(fleet_build_dir_for_path "$path")" && fleet_build_mark_stopped "$dir"
    say "#$num: its build passed the ${AUTOFLEET_BUILD_TIMEOUT}s wall clock (${age}s) and was stopped"
    return 0
  fi
  # Said every poll, deliberately: a build that outlives its kill is holding a
  # worktree the fleet believes it can stop, and the stop is retried each pass.
  say "#$num: its build passed the ${AUTOFLEET_BUILD_TIMEOUT}s wall clock (${age}s) and would not stop (pid ${left:-unknown} still running) -- trying again next poll"
  return 1
}

# -------------------------------------------------------- the build's exit ---
# THREE WATCHERS BECAME ONE, and the reason is the whole of
# armaatus/autofleet#151.
#
# `notice_stalled` existed because an interactive agent that sits at a
# confirmation prompt is indistinguishable from one that is working -- 44 lines
# of "waiting for input" in one fleet.log. `enforce_timebox` and
# `enforce_answer_timebox` existed because a session that is never allowed to
# end has to be stopped by a wall clock, enforced by typing at it. A `claude -p`
# run cannot sit at a prompt (there is nobody to answer), and it ends by itself
# at `--max-turns` or `--max-budget-usd`. So the dispatcher's whole job here is
# to notice that it ended and say which way.
#
# The three-hour time-box went with them and is not coming back: a build that is
# cheap and slow was never the problem -- #71 spent 65M tokens inside one
# three-hour box and the box is what let it. Turns and dollars bound the thing
# that actually costs.
#
# WHAT CAME BACK IS A CLOCK FOR THE BUILD THAT COSTS NOTHING (#161). Turns and
# dollars only end a run that is still SPENDING; a build wedged on a network
# read that never returns, or on a runtime that stopped answering, reaches
# neither, and reads `running` for as long as the dispatcher lives -- one
# worktree and one of AUTOFLEET_MAX slots, held until a person notices. That is
# the first unattended night's failure, so `build_over_clock` below is the third
# bound. It is two hours by default, not three, and it is not a box: nothing
# about it is meant to be reached by a build that is working.
notice_build_exit() {
  local f num path state
  for f in "$OWNED_DIR"/*; do
    [ -e "$f" ] || continue
    num="$(basename "$f")"
    path="$(owned_path "$num")"
    [ -n "$path" ] && [ -d "$path" ] || continue
    # "I could not tell" is not "it has stopped", and acting on the confusion
    # here would report a running build as having given up and comment on its
    # issue saying so. Said once per worktree, then left alone.
    if ! state="$(runner_build_state "$path")"; then
      [ -e "$STATE_DIR/build-blind-$num" ] && continue
      : >"$STATE_DIR/build-blind-$num"
      say "#$num: the runner would not say whether its build is running -- leaving it"
      continue
    fi
    rm -f "$STATE_DIR/build-blind-$num"
    if [ "$state" = running ]; then
      build_over_clock "$num" "$path" || continue
      # RE-READ RATHER THAN ASSUMED. The stop records the kill through
      # `fleet_build_mark_stopped`, and the state reader is the one thing that
      # turns that record into a state -- a `143` invented here would be a
      # second implementation of the same answer, and the one that drifts.
      state="$(runner_build_state "$path")" || continue
      [ "$state" = running ] && continue
    fi
    build_exited "$num" "$path" "$state"
  done
}

# What one finished run cost, in the words the log prints.
#
# From the result JSON the build wrote, which is `--output-format json`'s one
# object: `total_cost_usd`, `num_turns`, `subtype`. Reading a file beats the
# transcript grep `cost.sh` used to do -- one file per run rather than a walk of
# every session directory on the machine -- and it is the SAME source, so the
# dispatcher's line and the cost report cannot disagree.
#
# IT MUST DEGRADE. `AUTOFLEET_BUILD_CMD` is a wrapper seam, and a wrapper need
# not honour the flag. Output that does not parse gets the words "an unreadable
# result", not a crash and not a zero -- a cost report that silently says $0 is
# worse than one that says it could not tell.
build_summary() {
  python3 - "$(fleet_build_dir "$1")/result.json" <<'PY' 2>/dev/null || echo "an unreadable result"
import json, sys
doc = json.load(open(sys.argv[1]))
if isinstance(doc, list):
    doc = doc[-1] if doc else {}
if not isinstance(doc, dict):
    raise SystemExit(1)
turns = doc.get("num_turns")
cost = doc.get("total_cost_usd")
why = doc.get("subtype") or ("an error" if doc.get("is_error") else "success")
bits = [f"{turns} turns" if turns is not None else "an unknown number of turns"]
bits.append(f"${cost:.2f}" if isinstance(cost, (int, float)) else "an unknown amount")
print(f"{why} after {bits[0]} and {bits[1]}")
PY
}

# Did the run stop because it hit a limit, rather than because it was done?
#
# The subtype is the answer when there is one; the exit status is the fallback,
# because a wrapper seam need not produce parseable output and "it exited
# non-zero" is still a real answer. `success` is the only value read as done, so
# an unrecognised subtype from a future version is treated as a limit -- which
# resumes a run that was finished, costing one short session, rather than
# abandoning a PR mid-answer, which costs the issue.
build_ran_out() {
  local dir; dir="$(fleet_build_dir "$1")"
  local sub
  sub="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); d=d[-1] if isinstance(d,list) and d else d; print(d.get("subtype") or "")' "$dir/result.json" 2>/dev/null)" || sub=""
  case "$sub" in
    success) return 1 ;;
    "")      [ "$(cat "$dir/rc" 2>/dev/null || echo 1)" = 0 ] && return 1 ;;
  esac
  return 0
}

# One build has stopped. Decide what that means for its issue.
#
# Four answers, and the order matters:
#
#   done        the issue is closed or its PR merged -- `reap_merged` owns the
#               worktree from here, and saying anything else would race it.
#   resume      a pull request is open and the run stopped at a limit. The
#               branch and the PR ARE the state, so the resume is simply a
#               second `claude -p` in the same worktree whose brief is the
#               after-PR contract. This is what replaced the context reset, the
#               handoff note and the recycle: nothing has to be carried across,
#               because nothing was in the session that is not in git.
#   ran out     no pull request. The run is over, the worktree STAYS -- its diff
#               is the evidence and a stuck issue is exactly the one worth
#               looking at -- and `gaveup-` keeps the fleet from handing the
#               same issue the same budget again. `fleet.sh retry N` is how it
#               comes back.
#   finished    it exited cleanly with a PR open, which means it saw the review
#               loop through. Said once; nothing else to do.
#
# AUTOFLEET_BUILD_MAX_RUNS bounds the resume. Without it a run that ends at its
# budget the moment it starts -- a bad model name, an expired token -- is an
# infinite loop that spends the whole account, one session at a time, with the
# log saying "resuming" forever.
# THE FLEET STOPPED THIS BUILD, and the two things that stop one want different
# things afterwards. `reap_abandoned` stops a build whose issue is closed,
# `blocked` or a human step, and removes the worktree on its next pass: that
# stop needs nothing said here, and the reaper's own line is the record. A
# person's `stop --now` (and, once #161 lands, a wall clock) stops the build of
# an issue that is still the fleet's to work -- and a marker that only ever
# returned early left that build INVISIBLE: never resumed, its slot never
# released, `status` showing a worktree with nothing happening in it (the
# re-review of #164). So it is recorded as given up on, ONCE and LOCALLY: `status`
# lists it under "gave up on", `retry N` clears it, and the reaper releases the
# worktree when it holds nothing. No comment on GitHub and no notification: the
# comment `build_exited` posts says the agent's budget ended, and nobody's did.
#
# A lookup that cannot be read decides nothing this poll; the marker stands and
# the next poll asks again. The same label reading as `reap_abandoned`, so the
# two never disagree about whose stop this was.
build_stopped() {
  local num="$1" path="$2" answer labels
  gave_up_on "$num" && return 0
  answer="$(poll_issue "$num")" || return 0
  [ "$(issue_state_in "$answer")" = CLOSED ] && return 0
  labels="$(issue_labels_in "$answer")"
  has_label "$labels" "$BLOCKED_LABEL" && return 0
  has_label "$labels" "$HUMAN_STEP_LABEL" && return 0
  : >"$STATE_DIR/gaveup-$num"
  say "#$num: its build was stopped, not resumed -- ./scripts/fleet/fleet.sh retry $num starts it again"
  card "$path" comment "#$num: its build was stopped; retry $num starts it again"
  return 0
}

build_exited() {
  local num="$1" path="$2" state="$3" dir runs
  dir="$(fleet_build_dir "$num")"

  # WE KILLED THIS ONE, so there is nothing here to notice. Before every other
  # answer, and before the issue lookups: a stop is the fleet's own decision and
  # no reading of GitHub can change what it means.
  #
  # The stop the reaper's warning pass performs is so that "nothing new is being
  # written into a directory that is about to go"; `stop --now` is a person
  # asking for the same thing. Read as an exit like any other, both come back
  # here on the very next poll as a build that ran out -- and for an issue that
  # is merely `blocked` or `human-step` rather than closed, that is a `gaveup-`
  # record, a card, and "The fleet's build agent stopped on this without opening
  # a pull request" posted to the issue of a build nobody's budget ended
  # (armaatus/autofleet#163). Silent, and re-entered every poll while the
  # marker stands, so there is nothing to say once either.
  if fleet_build_was_stopped "$dir"; then
    build_stopped "$num" "$path"
    return 0
  fi

  issue_is_done "$num" && return 0

  # CAPTURED BEFORE ANYTHING ELSE RUNS. `has_open_pr` answers in its exit
  # status, and `$?` is overwritten by the next command -- including a `case`,
  # which is how the first draft of this read every answer as 0.
  local pr_open; has_open_pr "$num"; pr_open=$?
  if [ "$pr_open" = 2 ]; then
    # Could not read the PR listing. Nothing is decided on a blind answer.
    [ -e "$STATE_DIR/exit-blind-$num" ] && return 0
    : >"$STATE_DIR/exit-blind-$num"
    say "#$num: its build has stopped, but the open-PR listing could not be read -- deciding nothing"
    return 0
  fi
  rm -f "$STATE_DIR/exit-blind-$num"

  if [ "$pr_open" = 0 ] && ! build_ran_out "$num"; then
    [ -e "$STATE_DIR/build-done-$num" ] && return 0
    : >"$STATE_DIR/build-done-$num"
    say "#$num: its build finished -- $(build_summary "$num") ($state)"
    card "$path" comment "#$num: the build finished; its pull request is open"
    return 0
  fi

  if [ "$pr_open" = 0 ]; then
    runs="$(cat "$dir/run-count" 2>/dev/null || echo 1)"
    if [ "$runs" -ge "$BUILD_MAX_RUNS" ]; then
      # SAID ONCE. `notice_build_exit` re-enters this function on every poll for
      # every owned worktree whose state is `exited`, and that state never
      # changes by itself -- so an unguarded branch here posts the same issue
      # comment and the same notification once a minute for as long as the
      # dispatcher runs. Both terminal branches had the guard their two
      # neighbours already had; neither had its own. Found by both local review
      # passes, which rated the one below Critical because `reap_abandoned`
      # never disowns a worktree that holds anything -- so there it is unbounded
      # rather than merely repeated.
      gave_up_on "$num" && return 0
      : >"$STATE_DIR/gaveup-$num"
      say "#$num: its build stopped again -- $(build_summary "$num") -- after $runs runs, which is AUTOFLEET_BUILD_MAX_RUNS"
      card "$path" comment "#$num: out of runs with its pull request open -- needs you"
      # WHAT THE WORKTREE ACTUALLY GETS, rather than a promise this line cannot
      # keep: `gaveup-` above makes `reap_abandoned` release the worktree once
      # it holds nothing, and a pushed branch usually holds nothing. The
      # previous wording said "nothing is released", which was false on exactly
      # this path. Found by `/mattpocock-skills:code-review`.
      GH_PAGER=cat gh issue comment "$num" --body "The fleet stopped work on this after $runs runs of its build agent. The pull request is open and is what needs reading; its worktree at \`$path\` is kept while it holds anything uncommitted, and released once it does not." >/dev/null 2>&1 || true
      notify "#$num is out of runs" "Its PR is open and left for you."
      return 0
    fi
    say "#$num: its build stopped at a limit -- $(build_summary "$num") -- resuming in the same worktree (run $((runs + 1)))"
    start_build "$num" "$path" || say "#$num: could not start the resume"
    return 0
  fi

  gave_up_on "$num" && return 0
  : >"$STATE_DIR/gaveup-$num"
  say "#$num: its build ran out -- $(build_summary "$num") -- and opened no pull request"
  card "$path" comment "#$num: ran out before opening a pull request -- needs you"
  GH_PAGER=cat gh issue comment "$num" --body "The fleet's build agent stopped on this without opening a pull request: $(build_summary "$num"). Its worktree at \`$path\` is kept, so whatever it did get to is still there. \`./scripts/fleet/fleet.sh retry $num\` starts it again." >/dev/null 2>&1 || true
  notify "#$num ran out" "No pull request. Its worktree is kept."
  return 0
}

# ------------------------------------------------------- the running code ---
# `fleet.sh run` parses this file ONCE, at start, and never re-reads it. So a fix
# merged to `main` is live in the worktree and not live in the dispatcher that is
# running -- and the confusing half is that a change is only HALF dead: the queue
# filter kept working across four PRs because that path re-reads labels through
# `gh` every poll, while the half living in already-parsed shell functions did
# not run for 27 hours (#173).
#
# Nothing can fix that from in here; a dispatcher cannot re-read itself mid-pass
# without a claim about resumable state that is not tested. What it CAN do is
# stop being silent about it: record what it parsed, and let `status` compare.
DISPATCHER_FILE="$STATE_DIR/dispatcher"

# The content of the file that was parsed, not the commit that last touched it.
# The commit is only how the report NAMES what changed: a fix that is committed
# but not checked out, and a checkout somebody edited, are both "not what is
# running", and only the bytes say so.
#
# cksum rather than shasum: this is a change detector, not a security boundary,
# and cksum is the one that is everywhere.
#
# Both take the root to look in, and the report passes the one the DISPATCHER
# recorded rather than $REPO_ROOT. The dispatcher runs in the main worktree and
# `fleet.sh status` is run from wherever you are -- the README points agents in a
# fleet worktree at it. Hashing the caller's own copy compares a worktree
# branched before the fix against a dispatcher that predates it too, matches,
# and answers "current": #173's silence, rebuilt inside the check for it.
fleet_hash_stdin() { cksum | awk '{print $1 "-" $2}'; }
fleet_code_hash() {
  [ -r "$1/scripts/fleet/fleet.sh" ] || return 0
  fleet_hash_stdin <"$1/scripts/fleet/fleet.sh"
}
fleet_code_commit() {
  git -C "$1" log -1 --format=%H -- scripts/fleet/fleet.sh 2>/dev/null
}

# The fleet.sh commits in $2..$3, in the repo at $1, or nothing. A ref that does
# not resolve -- no `origin`, a checkout with no history in common -- is nothing
# to name rather than an error on the screen.
fleet_commits_between() {
  [ -n "$2" ] || return 0
  git -C "$1" rev-parse --verify --quiet "$3" >/dev/null 2>&1 || return 0
  git -C "$1" log --oneline "$2..$3" -- scripts/fleet/fleet.sh 2>/dev/null
}

# BSD date and GNU date spell "format this epoch" differently, and this runs on
# both -- macOS here, Linux in CI.
fmt_epoch() {
  date -r "$1" '+%Y-%m-%d %H:%M:%S' 2>/dev/null \
    || date -d "@$1" '+%Y-%m-%d %H:%M:%S' 2>/dev/null \
    || printf 'an unreadable time (%s)\n' "$1"
}

record_dispatcher() {
  mkdir -p "$STATE_DIR"
  { printf 'root=%s\n'    "$REPO_ROOT"
    printf 'started=%s\n' "$(date +%s)"
    printf 'commit=%s\n'  "$(fleet_code_commit "$REPO_ROOT")"
    printf 'hash=%s\n'    "$(fleet_code_hash "$REPO_ROOT")"
    # Not derivable from the outside, and that is why it is written: a drain
    # sets a file only a dispatcher that parsed THIS fleet.sh reads, and one
    # that started before #183 would poll straight through it, launching
    # worktrees under a stop that looked set. A running process saying what it
    # understands is the only honest answer -- the bytes on disk are the ones a
    # RESTART would parse, which is a different question.
    printf 'drain=1\n'
  } >"$DISPATCHER_FILE"
}
dispatcher_field() { sed -n "s/^$1=//p" "$DISPATCHER_FILE" 2>/dev/null | head -1; }

release_dispatcher_files() {
  [ "$(cat "$PIDFILE" 2>/dev/null)" = "$$" ] || return 0
  rm -f "$PIDFILE" "$DISPATCHER_FILE"
}

# Is $1 a pid this machine can still see running a DISPATCHER? THREE answers,
# because "ps would not say" is not "no" -- the same shape report_dispatcher_code
# already uses for a fleet.sh it cannot hash.
#
#   0  yes: alive, and its command line names a `fleet.sh run`.
#   1  no:  the process is gone, or ps named something that is not a dispatcher.
#   2  cannot say: it is alive, and ps produced no line to judge it by.
#
# The middle answer is the point. A `kill -9`'d dispatcher leaves its pidfile
# behind, the OS wraps round and hands that number to somebody else, and a check
# that asked `kill -0` alone would from then on refuse to start the fleet at all
# -- forever, on the strength of a stranger's process. lib.sh's
# The watcher stop this replaced took the same precaution for the same reason,
# before it SIGNALS a pid it did not watch die.
#
# `run` as well as the file name, because only `fleet.sh run` is a dispatcher.
# `fleet.sh status` is run constantly and from every worktree; a recycled pid
# landing on one of those would be a refusal with nothing behind it to stop.
#
# The third answer exists because the callers want opposite things from it. A
# `ps` that cannot answer -- a container without procps, a launch shape whose
# argv does not carry the script path -- must not let `run` hold the fleet down,
# and must not stop `stop --now` SIGNALLING the dispatcher it promises to stop.
# Collapsing it into "no" would do both: `--now` would interrupt every agent,
# announce that there was no dispatcher, leave it polling, and let the next `run`
# start a second one -- #179 rebuilt inside the check for it.
dispatcher_alive() {
  local pid="${1:-}" line
  case "$pid" in ''|*[!0-9]*) return 1 ;; esac
  kill -0 "$pid" 2>/dev/null || return 1
  line="$(ps -o command= -p "$pid" 2>/dev/null)"
  [ -n "$line" ] || return 2
  printf '%s\n' "$line" | grep -E 'fleet\.sh[[:space:]]+run([[:space:]]|$)' >/dev/null
}

# How to make a change live, said wherever the change is not. The cap is here
# because it has the same shape and is asked about far more often: MAX_WORKTREES
# is read once at start, so `AUTOFLEET_MAX=4` in front of `status` changes
# nothing at all.
# $2 is the pull a BEHIND checkout needs, printed in its place in the sequence
# rather than ahead of it. The steps are in the order docs/WORKFLOW.md gives
# them, and they have to stay in it: this output points the reader at that
# section, and a screen that contradicts the page it cites is worse than either
# alone.
restart_advice() {
  local root="${1:-}" pull="${2:-}"
  echo "  Restart it -- it is the only way a change to fleet.sh takes effect:"
  echo "    ./scripts/fleet/stop.sh          # drains: the agents in flight finish"
  echo "    ./scripts/fleet/fleet.sh status  # until it says idle"
  [ -n "$pull" ] && echo "    $pull"
  echo "    ./scripts/fleet/fleet.sh resume"
  # Named, not relative. This report is meant to be read from a fleet worktree,
  # and `./scripts/fleet/fleet.sh run --auto` there starts a dispatcher whose cwd
  # and checkout the fleet removes as soon as that worktree's PR merges.
  if [ -n "$root" ]; then
    echo "    cd $root && ./scripts/fleet/fleet.sh run --auto"
  else
    echo "    ./scripts/fleet/fleet.sh run --auto   # from the MAIN worktree"
  fi
  echo "  AUTOFLEET_MAX and _POLL are read at start too, so they change only"
  echo "  across a restart. docs/WORKFLOW.md, 'Restart it'. The three build"
  echo "  knobs -- AUTOFLEET_BUILD_MAX_TURNS, _MAX_BUDGET_USD and _MAX_RUNS --"
  echo "  are read per launch, so moving one takes effect on the next build."
}

# The refusal `fleet.sh run` prints when a dispatcher is already up, and how to
# get past it. A person starting a second one is usually doing the right thing
# for the wrong reason -- the first is running stale code and a restart is the
# remedy `status` names -- so this ends with a takeover they can follow rather
# than just a no.
#
# It calls restart_advice() for the drain rather than restating it. That
# sequence is docs/WORKFLOW.md's, restart_advice's comment says it has to stay
# in that order, and a second copy here is a second place to forget -- it also
# gets the `cd $root` for free, which matters most in exactly this message: the
# refusal is read from wherever you typed `run`, and a relative `run --auto`
# there starts a dispatcher in a directory the fleet removes when that
# worktree's PR merges (status_names_root is the test for it).
#
# BOTH restarts, because they cost different things and both are in
# docs/WORKFLOW.md. A drain is safe with agents mid-work -- since #183 it sets
# DRAIN and not STOP, so the agents finish and their PRs land -- but it WAITS
# for those PRs, and that is hours. A bare `kill` takes the dispatcher over now
# and leaves the agents running; what it gives up is the reaping, so anything
# in flight is then yours with `reap.sh --yes` once it lands.
refuse_second_dispatcher() {
  local holder="$1" root
  # The dispatcher's OWN checkout, not this caller's -- the same asymmetry the
  # staleness report is built on.
  root="$(dispatcher_field root)"
  {
    cat <<REFUSED
a dispatcher is already running (pid $holder).

MAX_WORKTREES is enforced per process, so a second one would count the same
worktrees this one is counting and launch on top of them -- twice the cap
between them, and two of every reap, board comment and time-box interrupt.

  ./scripts/fleet/fleet.sh status
      what it is running, whether the fleet.sh it parsed is still current, and
      the \`git pull --ff-only\` when its checkout never pulled the fix -- a
      restart without that one starts the same bytes over.

To take over -- the usual reason to start a second one is that the first is
running stale code. Drain it, which is safe with agents mid-work but waits for
the PRs in flight to merge:

REFUSED
    # No pull argument, deliberately, and this is the one caller that omits it.
    # Whether the dispatcher's checkout is BEHIND is report_dispatcher_code's
    # answer, off an `origin/main` this function has not looked at -- and
    # re-deriving it here would be a second implementation of the one thing #173
    # exists to get right. The text above sends the reader to `status` first for
    # exactly that: it prints the `git pull --ff-only` in its place in the
    # sequence when there is one to print.
    restart_advice "$root"
    cat <<REFUSED

That drain is safe with agents mid-work -- it sets DRAIN, not STOP, so they
finish and their PRs land -- but it WAITS for those PRs, which is hours. To
take this dispatcher over NOW without interrupting anybody, kill it instead
and reap what it does not get to yourself:

  kill $holder
  ./scripts/fleet/fleet.sh status  # until it says idle
  ./scripts/fleet/reap.sh --yes    # the stacks it did not live to reap
REFUSED
    if [ -n "$root" ]; then
      echo "  cd $root && ./scripts/fleet/fleet.sh run --auto"
    else
      echo "  ./scripts/fleet/fleet.sh run --auto   # from the MAIN worktree"
    fi
    cat <<REFUSED

Both are docs/WORKFLOW.md, "Restart it". The pid above came from
$PIDFILE, and nothing here removed it: it is the running dispatcher's.
REFUSED
  } >&2
  exit 1
}

# What `status` says about the dispatcher's own code. Three answers, because
# "could not tell" is not "current": a staleness report that fails open is the
# same silence #173 was.
report_dispatcher_code() {
  local root started commit hash now log remote
  root="$(dispatcher_field root)"
  started="$(dispatcher_field started)"
  hash="$(dispatcher_field hash)"
  commit="$(dispatcher_field commit)"
  # -r, not -e: fleet_code_hash answers nothing for a file it cannot READ, and
  # nothing compares unequal to the recorded hash -- which would report a
  # dispatcher as STALE on the strength of a permission error. "Cannot say" is
  # the honest answer to every question this cannot ask.
  if [ -z "$hash" ] || [ -z "$root" ] || [ ! -r "$root/scripts/fleet/fleet.sh" ]; then
    echo "  it recorded no fleet.sh at start -- it predates this check, or the"
    echo "  checkout it started from is gone -- so this cannot say whether a"
    echo "  recent fix is live in it."
    restart_advice
    return 0
  fi
  [ -n "$started" ] && echo "  up since $(fmt_epoch "$started")${commit:+, running fleet.sh @ ${commit:0:7}}"
  now="$(fleet_code_hash "$root")"
  if [ -n "$now" ] && [ "$now" = "$hash" ]; then
    # The bytes it parsed are still the bytes on disk, which is not the end of
    # it: nothing in the fleet pulls that checkout, so a fix MERGED while it ran
    # -- #173's own case -- leaves the file untouched and the hashes equal. The
    # remote-tracking ref is shared by every worktree of this repo, so asking it
    # costs nothing and needs no network; a checkout that has genuinely never
    # fetched simply has nothing to name.
    log="$(fleet_commits_between "$root" "$commit" origin/main)"
    [ -n "$log" ] || return 0
    # ...and those commits have to leave the file actually different. A change
    # and its revert are two commits that name each other out, and telling
    # somebody to pull and restart for bytes already running is the report
    # crying wolf on its own first outing.
    remote="$(git -C "$root" show origin/main:scripts/fleet/fleet.sh 2>/dev/null | fleet_hash_stdin)"
    [ -n "$remote" ] && [ "$remote" = "$hash" ] && return 0
    echo
    echo "  BEHIND -- $root has not pulled these, so they are NOT live in the"
    echo "  dispatcher running, and a restart alone will not make them live:"
    printf '%s\n' "$log" | sed 's/^/    /'
    restart_advice "$root" "git -C $root pull --ff-only"
    return 0
  fi

  echo
  echo "  STALE -- $root/scripts/fleet/fleet.sh has changed since it started, and"
  echo "  it parses the file once. These are NOT live in the dispatcher running:"
  log="$(fleet_commits_between "$root" "$commit" HEAD)"
  if [ -n "$log" ]; then
    printf '%s\n' "$log" | sed 's/^/    /'
  else
    # The bytes differ and git cannot name the difference -- an uncommitted edit,
    # or a checkout that never had that commit. Still stale.
    echo "    (git cannot name them from ${commit:-nothing recorded}; the file on disk differs)"
  fi
  restart_advice "$root"
}

# The one case a drain can fail in silently: a dispatcher older than the file it
# writes. DRAIN is read by this dispatcher and by nothing else, so a process
# that parsed a fleet.sh from before #183 polls straight through it, launching
# worktrees while the screen says the fleet is stopping.
#
# record_dispatcher writes `drain=1` and a running process is the only thing
# that can say what it parsed -- the bytes on disk are what a RESTART would
# parse, which is a different question and the one report_dispatcher_code asks.
# So a live dispatcher without that field either predates this or recorded
# nothing at all, and both mean the same thing here: it cannot be confirmed to
# see the drain.
#
# It WARNS. It does not refuse -- `stop.sh` always doing something is the
# promise docs/WORKFLOW.md makes of it -- and it does not quietly fall back to
# writing the STOP file, which would re-arm the freeze this whole change exists
# to remove and would freeze three agents to work around one old process.
warn_blind_dispatcher() {
  local held; held="$(cat "$PIDFILE" 2>/dev/null)"
  dispatcher_alive "$held"; local held_is=$?
  # 1 is "nothing is running", and there is nothing to be blind to the file.
  # 2 -- alive, and ps would not say what it is -- warns with the rest: what
  # cannot be established is exactly what this is about.
  [ "$held_is" = 1 ] && return 0
  [ -n "$(dispatcher_field drain)" ] && return 0
  local root; root="$(dispatcher_field root)"
  echo
  echo "  WARNING: pid $held is running and did not record that it reads"
  echo "           $DRAIN_FILE."
  echo "           A dispatcher that predates that file cannot see this drain:"
  echo "           it goes on launching worktrees while this screen says the"
  echo "           fleet is stopping. Take it over instead, which leaves the"
  echo "           agents alone:"
  echo "             kill $held"
  echo "             ./scripts/fleet/fleet.sh status  # until it says idle"
  if [ -n "$root" ]; then
    echo "             cd $root && ./scripts/fleet/fleet.sh run --auto"
  else
    echo "             ./scripts/fleet/fleet.sh run --auto   # from the MAIN worktree"
  fi
  echo "           Or ./scripts/fleet/stop.sh --now, which stops it and freezes"
  echo "           every agent with it."
}

# --------------------------------------------------------------- commands ---
cmd_status() {
  echo "fleet state: $STATE_DIR"
  # HOW MUCH OF THE POST-PR LOOP IS RUNNING, on the FIRST screen anybody looks
  # at. The failure this heads off is a quiet one: a pull request sitting with
  # nothing happening to it, and nothing anywhere saying whether that is because
  # it is finished, because it is at its review ceiling, or because a slot never
  # came free.
  #
  # LOCKS only. `stop_reviewers` and `live_reviewers` both skip the record
  # files; this third reader of the directory once did not -- and it is the one
  # its own comment calls the first screen anybody looks at. A `.done` stands
  # for as long as its head does, which is the point of the file, so status
  # reported a reviewer in flight permanently and three reviewed PRs read as
  # every slot taken. Through the predicate, not a fourth spelling of the suffix
  # list: the `find ! -name` this replaces WAS the drift.
  local n=0 m
  for m in "$REVIEWING_DIR"/*; do
    [ -e "$m" ] || continue
    is_review_record "$m" && continue
    n=$((n + 1))
  done
  echo "review:      the dispatcher runs it ($AUTOFLEET_REVIEW_CMD), $n in flight"
  # ...and how many of its two reviews each open pull request has spent. A PR
  # quietly on its eleventh round is the failure armaatus/autofleet#65 was
  # about; the ceiling of two makes that impossible, and this is what shows the
  # ceiling being reached rather than leaving "nothing is happening" to be
  # guessed at.
  #
  # A GLOB, not `is_review_record`, and deliberately: the predicate answers "is
  # this any record", and this wants ONE kind of record and its number.
  local r rn rpr found=0
  for r in "$REVIEWING_DIR"/*.reviews; do
    [ -e "$r" ] || continue
    rn="$(cat "$r" 2>/dev/null)"
    case "${rn:-}" in ''|*[!0-9]*) continue ;; esac
    rpr="$(basename "$r")"; rpr="${rpr%.reviews}"
    if [ "$rn" -ge 2 ]; then
      echo "             PR #$rpr: $rn/2 reviews -- AT THE CEILING, a person decides"
    else
      echo "             PR #$rpr: $rn/2 reviews"
    fi
    found=1
  done
  # ...AND WHAT THE LIST IS, because it is local state and not a query. These
  # records are swept when the dispatcher next sees the PR fall off the open
  # list, so on a machine where nothing is polling -- which is exactly when a
  # person runs this -- a merged PR can still print "AT THE CEILING" on the
  # first screen anybody reads. Filtering would cost a `gh pr list` of its own,
  # and the open-PR listing above is `--state open`, which would not answer this
  # question anyway. So the line says what it is instead.
  [ "${found:-0}" = 1 ] \
    && echo "             (from local records; the dispatcher sweeps a PR's when it closes)"
  # A stop and a running dispatcher are not alternatives: a drain leaves the
  # dispatcher up on purpose, because it is what reaps a worktree once its PR
  # merges -- and that draining dispatcher is running whatever code it parsed.
  # Which of the two, in the word the rest of the fleet uses for it. They are
  # not the same state and the person reading this is deciding whether to wait:
  # under a drain the agents are finishing and their PRs will land, under a stop
  # nothing they do can reach GitHub at all.
  if hard_stopped; then
    echo "STOPPED  ($STOP_FILE -- nothing goes out; clear with: ./scripts/fleet/fleet.sh resume)"
  elif draining; then
    echo "DRAINING ($DRAIN_FILE -- no new worktrees; the agents in flight finish"
    echo "          and their PRs land. Clear with: ./scripts/fleet/fleet.sh resume)"
  fi
  # dispatcher_alive, not `kill -0` alone: a pidfile a `kill -9` left behind,
  # whose pid the OS has since handed to somebody else, would otherwise be
  # reported as a dispatcher that is running -- and `run` would start one
  # anyway, because it asks the stricter question. Two screens disagreeing about
  # whether the fleet is up is worse than either answer.
  local pid; pid="$(cat "$PIDFILE" 2>/dev/null)"
  dispatcher_alive "$pid"; local live=$?
  if [ "$live" != 1 ]; then
    if [ "$live" = 2 ]; then
      # Alive, unidentifiable. Not `idle` -- a report that fails open here sends
      # somebody to start a second dispatcher, which is the whole of #179.
      echo "running?  (pid $pid -- alive, but ps would not say whether it is a dispatcher)"
    else
      echo "running   (pid $pid)"
    fi
    report_dispatcher_code
  else
    # Printed while stopped too. A drain ends when the dispatcher exits, and
    # this is the line that says it has -- WORKFLOW.md's restart waits for it.
    echo "idle      (no dispatcher running)"
  fi
  echo
  echo "worktrees now:"
  # A worktree waiting for a PERSON is not a worktree working, and after the
  # drain learned to end with one outstanding, the state a reader actually meets
  # is new: the dispatcher gone, `idle` on screen, and a directory still listed
  # here with nothing saying why it survived or how to release it. Issue 37 asks
  # this line to tell the two apart; the change that fixed the drain did not.
  # Found by the independent review.
  live_worktrees | while IFS="$(printf '\t')" read -r num path; do
    # `parked_for_person`, not `why_parked`: a worktree whose agent is still
    # working is not waiting for anybody, and printing the recovery line for it
    # tells a person to discard what is being written.
    #
    # In the QUIET voice -- the default, spelled out here because this is the
    # caller that makes it matter. `status` answers from $STATE_DIR and writes
    # nothing back to it; see the note on `parked_for_person`. #35.
    why="$(parked_for_person "$num" quiet)" && why="waiting for you -- $why" || why=""
    # `worktree_label`, not `#$num`: an unlinked worktree printed here as `#-`
    # named nothing, and the hold in the poll printed the same directory by its
    # basename. Two readers of one listing, two names. #71.
    label="$(worktree_label "$num" "$path")"
    # The row is the same either way; a parked one gets two lines under it.
    printf '  %-6s %s\n' "$label" "$path"
    if [ -n "$why" ]; then
      printf '         %s\n' "$why"
      printf '         %s\n' "$(how_to_release "$num" "$path")"
    fi
  done
  echo
  echo "next up (ready, not in flight, not labelled $HUMAN_STEP_LABEL;"
  echo "         'unblocks' is how many issues it frees, and a row marked"
  echo "         $PRIORITY_LABEL goes ahead of that ordering):"
  # The column is HEADED with the label word and FILLED with the label word, and
  # sized to it. Heading it `ahead` and filling it with `priority` named two
  # different things in one column; and a fixed `%-9s` fits the default with one
  # character to spare, so a host renaming the label to anything longer pushed
  # that row's title past the header and only that row's -- which reads as the
  # rendering bug this marker exists to prevent. Both found by the independent
  # review. `unblocks` is 8, so 8 is the floor that keeps the columns apart.
  local col="${#PRIORITY_LABEL}"
  [ "$col" -ge 8 ] || col=8
  printf "  %-6s %-9s %-${col}s %s\n" "issue" "unblocks" "$PRIORITY_LABEL" "title"
  # The label column ready_issues already prints is what says which rows are
  # ahead of the queue: a `next up` list reordered with nothing on screen saying
  # why reads as a bug in the ordering, which is the report this marker exists
  # to prevent.
  # THE ONE COMMAND A PERSON RUNS WHEN NOTHING IS LAUNCHING, so it has to be able
  # to say "the fleet cannot tell" rather than printing a full queue beside a
  # dispatcher that will start none of it. `in_flight` answers 2 when the open-PR
  # listing could not be read, and the loop below reads 2 as "not running", so
  # every row prints -- and at the page limit that is the whole backlog. Asked
  # once here rather than judged per row, because the loop is a subshell and
  # could not report back.
  #
  # ...and only with rows under it. Printed unconditionally it announced a
  # caveat about "the rows below" above an empty table, and spent an extra
  # uncached `gh pr list` on every `status` run against a repository with no
  # queue at all. Found by the local review, twice.
  # THE STATUS, kept. `ready_rows="$(ready_issues)"` on its own DROPPED it, so a
  # `gh` outage printed an empty `next up` table under a header that had already
  # gone out -- which reads as "the backlog is finished", the one wrong
  # conclusion this whole block exists to prevent. The PR listing's identical
  # failure got a caveat and two phases; this one had neither. Found by the
  # local review.
  local ready_rows ready_rc
  ready_rows="$(ready_issues)"; ready_rc=$?
  if [ "$ready_rc" != 0 ]; then
    echo "  (the issue listing could not be read, so this table is EMPTY because"
    echo "   nothing could be asked -- not because the backlog is finished)"
  elif [ -n "$ready_rows" ] && ! open_pr_listing >/dev/null 2>&1; then
    echo "  (the open pull request listing could not be read, so the rows below may"
    echo "   already be claimed -- see docs/WORKFLOW.md, \"What one poll costs\")"
  fi
  print_listing "$ready_rows" | while IFS="$(printf '\t')" read -r num unblocks labels title; do
    in_flight "$num" && continue
    gave_up_on "$num" && continue
    local mark=""
    has_label "$labels" "$PRIORITY_LABEL" && mark="$PRIORITY_LABEL"
    printf "  #%-5s %-9s %-${col}s %s\n" "$num" "$unblocks" "$mark" "$title"
  done | head -12

  # ...and where the ones missing from that list went, since a `ready` issue the
  # dispatcher silently declines forever is the confusing half of this.
  local gaveup; gaveup="$(gave_up_issues)"
  if [ -n "$gaveup" ]; then
    echo
    echo "gave up on (hand one back with: ./scripts/fleet/fleet.sh retry N):"
    printf '%s\n' "$gaveup" | while read -r num; do printf '  #%s\n' "$num"; done
  fi
}

cmd_stop() {
  local mode="${1:-}"
  # Validated first: `stop.sh --nwo` used to set the stop and then die with a
  # usage error, which is a confusing way to be safe.
  case "$mode" in
    ""|--now|--all) ;;
    *) die "usage: fleet.sh stop [--now|--all]" ;;
  esac
  mkdir -p "$STATE_DIR"
  # The drain, in every mode: it is the half that means "start nothing new", and
  # a hard stop is that plus a freeze.
  date '+draining since %Y-%m-%d %H:%M:%S' >"$DRAIN_FILE"
  case "$mode" in
    --now|--all)
      date '+stopped at %Y-%m-%d %H:%M:%S' >"$STOP_FILE"
      echo "stop set: $STOP_FILE (and the drain, $DRAIN_FILE)"
      echo "  no new worktrees, and no agent can push, open a PR or comment."
      # --now stops the builds this fleet owns. --all stops every build this
      # machine has a record of, including ones whose worktree the fleet has
      # already disowned -- a bigger hammer, so it has to be asked for by name.
      #
      # THIS KILLS THE RUN, where the interactive stop froze a session and left
      # it sitting there. That is a real difference and it is the right one: a
      # frozen session was still holding its context, still costing nothing but
      # still there to be resumed by hand, and the thing a person reaches for
      # `--now` to prevent is spending. What survives is what was committed,
      # which is what survives a build ending at its budget too.
      echo "  stopping builds..."
      local path dir left stopped=0
      if [ "$mode" = "--all" ]; then
        for dir in "$FLEET_BUILDS"/*; do
          [ -d "$dir" ] || continue
          path="$(cat "$dir/worktree" 2>/dev/null)" || continue
          [ -n "$path" ] || continue
          if left="$(runner_build_stop "$path")"; then
            echo "    stopped the build in $path"
          else
            echo "    could not stop the build in $path (pid ${left:-unknown} still running)"
          fi
          stopped=$((stopped + 1))
        done
      else
        for f in "$OWNED_DIR"/*; do
          [ -e "$f" ] || continue
          path="$(cat "$f")"
          [ -n "$path" ] || continue
          # ASKED FIRST, so the line is about what happened. It printed "stopped
          # the build" for every owned worktree whether or not one was running,
          # which on an idle fleet is a screen of stops that did not occur.
          # Found by the local `/code-review` pass.
          [ "$(runner_build_state "$path" 2>/dev/null)" = running ] || continue
          # SAID AFTER THE FACT, and only about what happened. This printed
          # `stopped the build for #N` over a `claude -p` that was still
          # running for as long as the driver's identity check answered no --
          # the one line a person reads to decide whether they have to go and
          # kill something by hand (#163).
          if left="$(stop_build_in "$path")"; then
            echo "    stopped the build for #$(basename "$f")"
          else
            echo "    could not stop the build for #$(basename "$f") (pid ${left:-unknown} still running)"
          fi
          stopped=$((stopped + 1))
        done
      fi
      # SAID WHEN THERE WERE NONE, rather than printing "stopping builds..."
      # followed by silence. The interactive stop had to distinguish "no agents"
      # from "could not read the listing"; a pid file has no third answer, so
      # the honest line here is simply the count.
      [ "$stopped" = 0 ] && echo "    there were none."
      # ...and the local reviewers, which are children of the dispatcher rather
      # than agents in a worktree, so the terminal interrupts above do not reach
      # them. Before the dispatcher is killed: after it, nothing is left that
      # knows which pids they were.
      stop_reviewers

      # Only here. A drain has to leave the dispatcher alive: it is what reaps a
      # worktree once its PR merges, and killing it strands them.
      #
      # dispatcher_alive before the signal, which is the whole of lib.sh's
      # The watcher stop this replaced, in one line: this is the only place the
      # fleet SIGNALS a pid it read out of a file, and a pidfile a `kill -9`
      # left behind names whoever the OS has since given that number to.
      local held; held="$(cat "$PIDFILE" 2>/dev/null)"
      dispatcher_alive "$held"; local held_is=$?
      # Signalled on "yes" AND on "cannot say". `--now` promises the dispatcher
      # is down when it returns, and a ps that would not answer is not a reason
      # to break that promise and leave it polling -- it is exactly the state
      # where a false "nothing to stop" produces the second dispatcher.
      if [ "$held_is" != 1 ]; then
        kill "$held" 2>/dev/null && echo "  dispatcher stopped."
        [ "$held_is" = 2 ] \
          && echo "  (ps would not say what pid $held was; signalled it because --now promises it is down.)"
      elif [ -e "$PIDFILE" ]; then
        echo "  no dispatcher to stop; $PIDFILE names pid ${held:-nothing}, which is not one."
      fi ;;
    "")
      echo "drain set: $DRAIN_FILE"
      # ...but a drain does not LIFT a stop, and saying "the agents are not
      # frozen" while $STOP_FILE is still there is the reassurance somebody
      # would act on: `--now` to freeze, then a plain `stop.sh` later to let the
      # work land, and the agents stay frozen with the screen saying otherwise.
      if hard_stopped; then
        echo "  ...but $STOP_FILE is STILL SET, so the agents stay frozen: no push,"
        echo "  no PR, no comment. A drain does not lift a stop. To let the work in"
        echo "  flight land, clear it and drain again:"
        echo "    ./scripts/fleet/fleet.sh resume && ./scripts/fleet/stop.sh"
      else
        echo "  no new worktrees. The agents in flight are NOT frozen: they finish,"
        echo "  push, open their PRs and comment, because a merged PR is what releases"
        echo "  the worktree this drain is waiting on."
        echo "  The dispatcher stays up to reap those worktrees as their PRs land, and"
        echo "  exits once nothing is left. Watch it with: fleet.sh status."
        echo "  Use --now to interrupt the fleet's agents and freeze every outward"
        echo "  effect, --all to interrupt every agent the runner knows about."
      fi
      # Last, because it is the line that changes what you do next.
      warn_blind_dispatcher ;;
  esac
  if hard_stopped; then
    notify "stopped" "No new work will start, and nothing goes out."
  else
    notify "draining" "No new work will start; the agents in flight finish."
  fi
}

cmd_resume() {
  # Both, always. Clearing one of the two leaves a fleet that either refuses
  # every `run` with the stop apparently lifted, or lets the agents out while
  # nothing may start -- neither is a state anybody asked for.
  rm -f "$STOP_FILE" "$DRAIN_FILE"
  echo "drain and stop cleared. Start again with: ./scripts/fleet/fleet.sh run --auto"
}

# The counterpart to the time-box, and the ONLY way back onto the queue. By name
# and on purpose: the box fired because three hours produced no PR, and an issue
# handed back unchanged spends the next three the same way. Restarting the
# dispatcher deliberately does not clear these -- a crash and a reboot are not
# decisions about an issue.
cmd_retry() {
  [ "$#" -gt 0 ] || die "usage: fleet.sh retry ISSUE [ISSUE...]"
  local n log
  for n in "$@"; do
    case "$n" in ''|*[!0-9]*) die "not an issue number: $n" ;; esac
  done
  for n in "$@"; do
    if [ -e "$STATE_DIR/gaveup-$n" ]; then
      rm -f "$STATE_DIR/gaveup-$n"
      echo "#$n is startable again."
      # ...and where the attempt that was stopped left its log.
      #
      # SAID HERE because this is the command a person actually runs, and the
      # one place in the retry path that is certain to be reached. It replaced
      # the handoff note, which a resumed session needed and a second
      # `claude -p` does not: the branch and the pull request are the state, and
      # the only thing a person wants at this moment is what the last run said
      # before it stopped.
      # `if`, not `[ -n ... ] && echo`: that AND-list is the last command in
      # this branch, so with no log it makes `cmd_retry` itself return 1 --
      # a command that did exactly what was asked reporting failure. Caught by
      # the phase that asserts the no-log case.
      log="$(fleet_build_dir "$n")/build.log"
      if [ -s "$log" ]; then echo "  its last attempt's log is at $log"; fi
    else
      echo "#$n was not one this dispatcher gave up on; nothing to clear."
    fi
  done
}

# Accepts 08:00 (the next such time), 6h, 90m, or an epoch.
deadline_from() {
  python3 -c '
import sys, time, datetime
spec = sys.argv[1]
now = time.time()
if spec.endswith("h"):   print(int(now + float(spec[:-1]) * 3600)); raise SystemExit
if spec.endswith("m"):   print(int(now + float(spec[:-1]) * 60)); raise SystemExit
if ":" in spec:
    h, m = (int(x) for x in spec.split(":"))
    t = datetime.datetime.now().replace(hour=h, minute=m, second=0, microsecond=0)
    if t.timestamp() <= now:
        t += datetime.timedelta(days=1)
    print(int(t.timestamp())); raise SystemExit
print(int(spec))
' "$1" 2>/dev/null
}

# THE LAST LINES A PERSON READS, when something is waiting for them: a header,
# then one line per worktree naming the issue and the command that frees it.
#
# AFTER `fleet down: $reason`, which is what armaatus/autofleet#37's third
# acceptance bullet asks for and what it got one line short of. The block used to
# run inside the poll loop, so the last thing on the screen was the farewell --
# and `$reason` carries a COUNT ("and 1 worktree(s) are waiting for you"), not
# which issue and not how to release it. A reader stops at the last line.
# What changed is the ORDER and nothing else: `--until`, `--for` and `--max-prs`
# all latch the drain and still leave through `cmd_run`'s
# `queued == 0 && owned == 0` break, which is the poll loop's only exit.
#
# A FUNCTION and not four lines inside `cmd_run`, because its two runner-outage
# branches are the ones a phase has to drive, and reached inline they need the
# runner to stop answering BETWEEN the last poll and the farewell -- a race no
# fixture can arrange. Extracted, `in_fleet farewell_parked 2` with an unreadable
# agent listing is the whole test. Found by `/code-review`, which noted both
# branches could be deleted with the suite green.
#
# `parked_for_person`, NOT `why_parked`: this was the third reader of the park
# predicate and the one still on the ungated one, so it would print
# `how_to_release` -- "commit, move or discard what is there" -- against a
# worktree whose agent is writing. Unreachable from `cmd_run`, because `owned`
# reaching 0 means every one of these already passed the gate inside
# `count_parked_owned`; a guard that holds only because of something two hundred
# lines away is the shape #71 is about. In the QUIET voice, like `cmd_status`:
# nothing after the loop should be writing to $STATE_DIR.
#
# THE LIST IS BUILT BEFORE THE HEADER IS SAID, for the same reason: a runner that
# stops answering makes every `parked_for_person` here return 1, and a header
# announcing N worktrees with nothing under it is the "one line short" failure
# this block exists to fix.
#
# $1 is what the last poll counted.
farewell_parked() {
  local parked="${1:-0}" n why line path parked_lines="" named=0
  case "$parked" in ''|*[!0-9]*) parked=0 ;; esac
  [ "$parked" -gt 0 ] || return 0
  # shellcheck disable=SC2045 # $OWNED_DIR holds issue numbers by construction,
  # so there is nothing here for a glob to survive that `ls` does not; a glob
  # would also yield the literal pattern when the directory is empty.
  for n in $(ls "$OWNED_DIR" 2>/dev/null); do
    why="$(parked_for_person "$n" quiet)" || continue
    named=$((named + 1))
    # `worktree_label`, not a bare `#$n`: this is the THIRD reader of "how a
    # worktree is named in a line a person reads", and it was the one that did
    # not move when the other two were made to agree. $OWNED_DIR holds issue
    # numbers by construction, so `-` cannot reach here today -- which is a
    # guard holding because of something elsewhere, the shape #71 is about.
    # Found by `/mattpocock-skills:code-review`.
    path="$(owned_path "$n")"
    parked_lines="$parked_lines  $(worktree_label "$n" "$path") -- $why
    $(how_to_release "$n" "$path")
"
  done
  if [ "$named" -eq 0 ]; then
    # NOT silence: the poll said there were some, and this is the only line that
    # can say why none of them could be named.
    say "$parked worktree(s) are waiting for you, and the runner would not say"
    say "  which -- ./scripts/fleet/fleet.sh status once it answers again"
    return 0
  fi
  say "$named worktree(s) are waiting for you rather than for an agent:"
  # One `say` per line, so each keeps its own timestamp and the log reads the way
  # every other multi-line message here does.
  printf '%s' "$parked_lines" | while IFS= read -r line; do say "$line"; done
  # TWO COUNTS, RECONCILED, IN BOTH DIRECTIONS. `fleet down: $reason` above
  # carries the number the last poll counted; this re-derives its own by asking
  # the runner again, and an agent that starts -- or stops -- writing in between
  # makes them differ. Either way two numbers for one thing with nothing
  # explaining them is worse than either alone. Found by
  # `/mattpocock-skills:code-review`, the second direction on its second pass.
  #
  # NEITHER BRANCH NAMES ONE CAUSE, because neither has one. A shortfall is a
  # runner that would not answer OR an agent that picked the work back up; a
  # surplus is an agent that finished OR a worktree that parked on the final
  # pass, which this function counts and `count_parked_owned`'s survive-a-pass
  # rule does not. Naming the likelier half only sends a person to look for an
  # outage that is not happening. Found by both passes, one branch each.
  if [ "$named" -lt "$parked" ]; then
    say "  ...and $(( parked - named )) fewer than the line above says: either"
    say "  the runner would not say what their agents are doing just now, or an"
    say "  agent went back to work. The list is the current one."
    say "  ./scripts/fleet/fleet.sh status to see which."
  elif [ "$named" -gt "$parked" ]; then
    say "  ...$(( named - parked )) more than the line above says: an agent"
    say "  finished, or a worktree parked, between the last poll and now. The"
    say "  list is the current one."
  fi
}

cmd_run() {
  # THE DRAIN LATCH, once. Three stop conditions were each spelling the same
  # three lines -- set the flag, set the reason, say "-- launching nothing more,
  # still reaping what is in flight" -- and a fourth arrived with this change.
  # Three copies of a sentence are three chances for the stop conditions to stop
  # saying the same thing, which is the property #36 is about. `reason` is what
  # `fleet down: $reason` prints; `$2` is what this pass says on the way in.
  # It closes over cmd_run's locals rather than taking them, because that is the
  # whole of what it replaces. Found by the independent review.
  enter_drain() {
    drain_mode=true
    reason="$1"
    say "$2 -- launching nothing more, still reaping what is in flight"
  }
  local auto=false deadline="" max_prs="" ; local -a wanted=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --auto)     auto=true; shift ;;
      --until|--for) deadline="$(deadline_from "$2")" || die "cannot read a time from '$2'"
                  [ -n "$deadline" ] || die "cannot read a time from '$2'"; shift 2 ;;
      --max-prs)  max_prs="$2"; shift 2 ;;
      [0-9]*)     wanted+=("$1"); shift ;;
      *) die "usage: fleet.sh run [--auto] [--until HH:MM|--for 6h] [--max-prs N] [ISSUE...]" ;;
    esac
  done
  $auto || [ "${#wanted[@]}" -gt 0 ] || die "give issue numbers, or --auto"
  # The stop is checked first, and it has to say more than "stopped" when a
  # dispatcher is still draining behind it. That sentence IS the way in: a drain
  # leaves the dispatcher up on purpose, `status` says `running (pid N)`
  # throughout, and somebody who reads "stopped" as "down" runs `resume` and
  # `run --auto`. The refusal below would then catch them, but a message that
  # sends them there in the first place is a worse place to be caught.
  if draining; then
    # `held`, not `drain_mode`: cmd_run declares a `local drain_mode=false`
    # further down and RUNS it as a command (`! $drain_mode`). Both branches
    # here die, so the collision is harmless today and would not stay that way.
    local held; held="$(cat "$PIDFILE" 2>/dev/null)"
    local state; state="$(stop_state)"
    local which_file; which_file="$(stop_state_file)"
    if dispatcher_alive "$held"; then
      die "the fleet is $state ($which_file), and pid $held is still DRAINING:
launching nothing new, and reaping what is in flight until nothing it owns is
left, which is what a drain is. It exits on its own; watch it with
\`fleet.sh status\`, and only then start one.

\`fleet.sh resume\` clears the stop. It does NOT make room for a second
dispatcher -- MAX_WORKTREES is enforced per process, so \`run\` would refuse
while that one is up."
    fi
    die "the fleet is $state ($which_file). Clear it with: fleet.sh resume"
  fi

  # One dispatcher per machine, checked before the pidfile is claimed rather
  # than trusted from it: `fleet.pid` names whoever started LAST, so before this
  # the second dispatcher simply became the recorded one and the first kept
  # polling unrecorded (#179).
  #
  # The way in is ordinary rather than exotic. A drain leaves the dispatcher up
  # on purpose -- it is what reaps a worktree once its PR merges -- and `status`
  # says `running (pid N)` for as long as that lasts. And #173's staleness
  # report answers "a dispatcher older than its own fleet.sh" with "restart it",
  # so making staleness visible is also what multiplies the restarts this has to
  # catch.
  #
  # Read-then-claim, and the window between them is not closed: two dispatchers
  # started inside the same millisecond would both get past this. That is not
  # the way in -- the drain leaves the first one up for HOURS and somebody
  # resumes on top of it -- and an atomic claim wants a temp file or an
  # O_EXCL dance whose leftovers are their own failure mode in a state dir a
  # `kill -9` already litters.
  local holder; holder="$(cat "$PIDFILE" 2>/dev/null)"
  dispatcher_alive "$holder"
  case $? in
    0) refuse_second_dispatcher "$holder" ;;
    # Alive, and ps had nothing to judge it by. It starts -- a fleet one stale
    # file can hold down forever is the failure this check exists to avoid, and
    # the acceptance for #179 says so outright. But it does not start SILENTLY:
    # if that pid really is a dispatcher, this is the two-of-them case and the
    # only warning anybody gets.
    2) say "WARNING: $PIDFILE names pid $holder, which is alive, and ps would not say what it is."
       say "         Starting anyway -- one unreadable pidfile may not hold the fleet down."
       say "         But if pid $holder IS a dispatcher there are now two, each enforcing"
       say "         MAX_WORKTREES on its own. Find out before you leave this running:"
       say "           ps -p $holder" ;;
  esac

  # The record BEFORE the pidfile, and the order is the whole of it: every
  # reader of the record reaches it through the pidfile -- `status` and
  # `warn_blind_dispatcher` both ask `dispatcher_alive` first -- so a pid
  # claimed before the record exists is a window in which this dispatcher is
  # live, current, and has recorded nothing. `stop.sh` landing in it would
  # announce that a dispatcher which reads the drain file perfectly well cannot
  # see it. A record with no pidfile is inert the other way round: nothing looks
  # at it, and the next dispatcher overwrites it.
  # Markers left by a dispatcher that died without cleaning up. Their pids are
  # not ours, and after a reboot or a few hours they name strangers -- so they
  # are cleared before this dispatcher counts anything, rather than reaped
  # one-by-one against a `kill -0` that cannot tell the difference.
  stop_reviewers >/dev/null
  # ...and the "already said it" markers, so this dispatcher explains its own
  # holds rather than inheriting a previous run's silence. `rotate-blind` is the
  # same shape and was cleared by nothing at all, so "rotating with a pid ps
  # cannot name" was said once per MACHINE -- an operator debugging truncated
  # reviewer output next month got no line at all. Found by the independent
  # review.
  rm -f "$FOUNDATION_HOLD_SAID" "$ROTATE_BLIND_SAID" "$PR_PAGE_FULL_SAID" \
        "$READY_UNREADABLE_SAID"
  # ...and the survive-a-pass markers. `parked-since-$n` lives in $STATE_DIR
  # rather than the poll cache, and the only thing that removes it is the pass
  # that finds the worktree no longer parked -- `release_dispatcher_files` is on
  # the EXIT trap and does not touch it. So after a `kill -9` a marker left by
  # the dead dispatcher made the next one count that worktree as parked on its
  # FIRST pass, which is the transient-marker case the survive-a-pass rule exists
  # to rule out: it self-heals from the next poll onward, and the window is
  # exactly the pass where a wrong `parked` ends a drain with an agent still
  # writing. armaatus/autofleet#71.
  rm -f "$STATE_DIR"/parked-since-*

  record_dispatcher
  echo $$ >"$PIDFILE"
  # ...but removed only while they still name THIS process. Nothing stops a
  # second dispatcher from starting and claiming both files, and an unconditional
  # `rm` would then have the first one's exit delete the second one's record --
  # leaving a live dispatcher reported as idle, with nothing to check its code
  # against. That is the silence this whole file's staleness report exists to end.
  trap 'release_dispatcher_files' EXIT
  say "fleet up: max $MAX_WORKTREES worktrees, polling every ${POLL_SECONDS}s, ${BUILD_MAX_TURNS} turns and \$${BUILD_MAX_BUDGET_USD} per run"
  $auto && say "mode: auto -- most-unblocking first, until the backlog is empty or you stop it" \
        || say "mode: list -- ${wanted[*]}"
  [ -n "$deadline" ] && say "stopping at $(date -r "$deadline" '+%Y-%m-%d %H:%M')"
  [ -n "$max_prs" ] && say "stopping after $max_prs worktree(s) opened"

  local opened=0 reason="the queue is empty"
  # Not `draining` -- that is the FUNCTION above, which asks the filesystem.
  # This is the run's own latch, and it is also set by --until and --max-prs,
  # neither of which writes a file.
  local drain_mode=false
  # An issue dropped from `wanted` did not land, and saying it did is a lie the
  # run's last line would tell every time the fleet declines one.
  local declined=false
  while true; do
    # A stop means "launch nothing more", not "abandon what is running". The
    # dispatcher is what reaps a worktree once its PR merges, so killing it here
    # would strand every in-flight stack under `restart: unless-stopped`. It
    # keeps reaping and exits when nothing it owns is left.
    if draining && ! $drain_mode; then enter_drain "you stopped it" "draining"; fi
    if [ -n "$deadline" ] && [ "$(date +%s)" -ge "$deadline" ] && ! $drain_mode; then
      enter_drain "the deadline passed" "deadline passed"
    fi

    # Between passes, before anything writes: the log rotation must not land
    # mid-pass, and the marker sweep reads git, which is cheap and local.
    rotate_fleet_log

    forget_poll_answers
    # ...here, once per pass, and nowhere else: see the note beside the
    # definition.
    #
    # AFTER it, not once at startup, and that ordering is the whole of why it is
    # a line inside the loop. $POLL_CACHE is not cleared when a dispatcher
    # starts -- it is cleared on the line above -- so a reader that ran between
    # startup and here would answer from a dead dispatcher's list. Nothing does
    # today; this costs one assignment a minute to keep it that way. It is never
    # set back to false, which is right: this process is a dispatcher for as
    # long as it lives.
    IN_POLL=true
    #
    # The order below is load-bearing in three places. reap_merged first,
    # because a worktree whose PR merged is its business and the two watchers
    # under it only ever look at what is left owned. review_open_prs before
    # notice_build_exit, because a build that has stopped with its PR open is
    # resumed into the findings that pass collected -- resuming first would
    # start a run with nothing yet to answer. And reap_abandoned LAST: it is the
    # one that removes a worktree, and running it earlier took the "waiting for
    # you, as expected" notification away from the very worktrees it exists to
    # release.
    #
    # THREE WATCHERS LEFT THIS LIST with armaatus/autofleet#151 -- the context
    # reset, the two time-boxes and the stall detector. They are one function
    # now, `notice_build_exit`, and it does not act on a clock: `claude -p` ends
    # by itself, so the dispatcher reads an exit instead of enforcing one.
    reap_merged
    # After reap_merged, because a PR that just merged needs no review, and
    # before the rest because it is the only one of these that UNBLOCKS a
    # worktree rather than reclaiming one: a build sitting in await-review.sh is
    # waiting on exactly this, and every pass it waits is a pass of its budget
    # spent.
    review_open_prs
    notice_build_exit
    reap_abandoned
    prune_gaveup

    local live live_list
    if ! live_list="$(live_worktrees)"; then
      say "could not read this repository's worktree list; skipping this pass rather than guessing"
      sleep "$POLL_SECONDS"
      continue
    fi
    live="$(count_worktrees "$live_list")"

    # THE PASS'S OPEN-PR LISTING, taken here at the latest -- see the header on
    # `open_pr_listing` for the window this closes and why the launch loop is
    # the line it has to be closed before. A cache hit when a watcher above
    # already took it; the fetch itself only when none did.
    #
    # ONLY WHEN THE LAUNCH LOOP WILL RUN: not under a drain, and not with every
    # slot already full. Read the saving narrowly, because an earlier version of
    # this comment claimed more than it buys. Under a DRAIN nothing else in the
    # pass reads the listing, so the gate saves a call a minute for however long
    # the drain lasts -- hours, waiting on three PRs to merge. On a full fleet it
    # saves one only in LIST mode, where `count_startable` is unreachable
    # (`queued` comes from `wanted`); in `--auto` `count_startable` runs a few
    # lines below and takes the listing anyway, which is why the busy row in
    # docs/WORKFLOW.md counts it. Both halves are still worth having and both
    # have a phase; neither is worth overstating. Found by the local review.
    #
    # Its failure is not handled here and must not be: every caller has its own
    # "could not tell" branch and they do not agree on what to do about it.
    if ! $drain_mode && [ "$live" -lt "$MAX_WORKTREES" ]; then
      open_pr_listing >/dev/null || true
    fi

    while ! $drain_mode && [ "$live" -lt "$MAX_WORKTREES" ]; do
      # `break`, not `break 2`: this is the drain arriving MID-PASS, after the
      # check at the top of the loop and while worktrees are still owned, which
      # is what `stop.sh` against a running fleet actually looks like. Leaving
      # the whole loop here ended the dispatcher on the spot -- nothing reaped
      # the worktrees it was holding, and their stacks stayed up under
      # `restart: unless-stopped` with nothing left to take them down. It stops
      # LAUNCHING here and keeps reaping, which is the same thing the top of the
      # loop does one pass later.
      if check_drain; then enter_drain "you stopped it" "draining"; break; fi
      # A DRAIN, not an exit. `break 2` left the launch loop AND the poll loop,
      # so the dispatcher stopped with its worktrees mid-work: nothing reaped
      # them when their PRs merged, their stacks stayed up under
      # `restart: unless-stopped`, and $OWNED_DIR kept entries the next
      # dispatcher inherited and counted against its cap. The comment a few
      # lines above records that exact bug being fixed for the drain path;
      # `--until` and `--for` set `drain_mode` and keep reaping, and
      # docs/WORKFLOW.md advertises all three as equivalent. They were not.
      # armaatus/autofleet#36.
      # No `! $drain_mode` guard: the enclosing loop is `while ! $drain_mode`,
      # and the only assignment inside it breaks out at once, so it could never
      # be false here and reading it suggested otherwise.
      if [ -n "$max_prs" ] && [ "$opened" -ge "$max_prs" ]; then
        enter_drain "it opened $opened worktree(s)" "opened $opened worktree(s)"
        break
      fi

      # EVERY ITERATION, which is what makes one check cover both halves of the
      # rule: it holds when a foundation issue was already running when this
      # dispatcher started, and it holds again on the iteration after this pass
      # launches one, because by then that worktree is in the list too.
      #
      # Cheap despite that. The whole answer is cached per poll, and so is the
      # worktree list it is read off, and `launch` drops both -- so a pass costs
      # one `runner_worktree_list` plus one more per worktree it opens, not one
      # per iteration. An earlier version of this comment described the cost
      # before that cache existed and claimed only the label lookups were cached;
      # found by the local review.
      if foundation_in_flight; then break; fi

      local picked="" title="" labels=""
      if [ "${#wanted[@]}" -gt 0 ]; then
        # An issue leaves `wanted` only when it is DONE -- merged, or its
        # worktree gone with a PR standing. An issue that is merely in flight
        # stays, so it is not re-launched when its PR merges and `in_flight`
        # goes false again, and so the termination check below can see that
        # something is still outstanding.
        local remaining=()
        local n rc
        for n in "${wanted[@]}"; do
          if issue_is_done "$n"; then
            say "#$n has landed"
            continue
          fi
          # DROPPED, not skipped, for the same reason the label decline below
          # drops one: an issue kept in `wanted` that can never be launched is a
          # run loop that never ends.
          if gave_up_on "$n"; then
            say "#$n: the fleet gave up on it -- hand it back with: fleet.sh retry $n"
            declined=true
            continue
          fi
          in_flight "$n"; rc=$?
          if [ -z "$picked" ] && [ "$rc" = 1 ]; then
            # Asked only of the one issue this pass would actually launch, not
            # of every issue in the list: count_startable exists because a `gh`
            # call per queued issue per poll is how you meet the secondary rate
            # limit. Named on the command line or picked from the queue, the
            # fleet cannot finish this one either way -- so it is DROPPED rather
            # than skipped, because an issue kept in `wanted` that can never be
            # launched is a run loop that never ends. "Could not tell" keeps it,
            # since opening a worktree for work no agent may finish is the
            # expensive direction and the next pass asks again.
            issue_needs_human_step "$n"; case $? in
              0) say "#$n is labelled $HUMAN_STEP_LABEL -- it is yours to take; remove the label to hand it to an agent"
                 rm -f "$STATE_DIR/queue-labels-$n"; declined=true; continue ;;
              2) if [ ! -e "$STATE_DIR/queue-labels-$n" ]; then
                   : >"$STATE_DIR/queue-labels-$n"
                   say "#$n: could not read its labels -- not starting it this pass"
                 fi
                 remaining+=("$n"); continue ;;
            esac
            rm -f "$STATE_DIR/queue-labels-$n"
            picked="$n"
          fi
          remaining+=("$n")
        done
        wanted=("${remaining[@]+"${remaining[@]}"}")
        [ -n "$picked" ] || break
        title="$(GH_PAGER=cat gh issue view "$picked" --json title --jq .title 2>/dev/null)"
        labels="$(GH_PAGER=cat gh issue view "$picked" --json labels --jq '[.labels[].name]|join(",")' 2>/dev/null)"
      else
        $auto || break
        while IFS="$(printf '\t')" read -r n _unblocks l t; do
          in_flight "$n"; [ "$?" = 1 ] || continue
          # Said once, when the box fired -- not once per poll for the rest of
          # the run.
          gave_up_on "$n" && continue
          # A foundation issue defines an interface later issues include, so it
          # lands alone: three worktrees each inventing their own version of a
          # shared header is the one merge conflict worth serialising to avoid.
          if is_foundation "$l" && [ "$live" -gt 0 ]; then
            # Through `foundation_hold_say`, so this door to the log is as quiet
            # as the other one. The same rule announced every poll from here
            # would have put back the 180 lines per three hours that the marker
            # exists to prevent. Found by the independent review.
            # NAMED, not counted. The marker carries the names too, so the line
            # comes back when what it waits on CHANGES -- which is news -- and
            # stays quiet while it does not.
            local waiting_on; waiting_on="$(waiting_worktrees "$live_list")"
            foundation_hold_say "waiting-$n-$waiting_on" \
              "#$n is a foundation issue; it lands alone, so it waits for $waiting_on to land"
            break
          fi
          picked="$n"; title="$t"; labels="$l"
          break
        done < <(ready_issues)
      fi
      [ -n "$picked" ] || break

      if is_foundation "$labels" && [ "$live" -gt 0 ]; then break; fi
      if launch "$picked" "$title"; then
        opened=$((opened + 1))
        # THE WITHIN-A-PASS HALF OF THE RULE, from what is already in hand.
        #
        # `foundation_in_flight` on the next iteration would also stop here --
        # `launch` drops its cache for exactly that -- but only by asking the
        # Orca CLI to tell us something we already know, and only if two things
        # hold that nothing guarantees: that `worktree list` shows a worktree
        # `worktree create` returned moments ago, and that the entry carries
        # `linkedIssue` rather than a null this function reads as "on no issue".
        # Neither is promised by a CLI backed by an index or a daemon, and the
        # stub in tests/test_fleet.sh is read-your-writes BY CONSTRUCTION -- a
        # property this very PR gave it -- so the phase for this half was
        # asserting the assumption rather than the behaviour.
        #
        # `$labels` is the labels of the issue just launched. Asking them costs
        # nothing and cannot be wrong. The per-iteration check keeps doing what
        # only it can: the across-passes half, which survives a restart.
        # Complementary, not redundant. Found by the independent review.
        if is_foundation "$labels"; then
          say "#$picked is a foundation issue; it lands alone, so nothing else starts this pass"
          break
        fi
      else
        # It stays in the queue. Dropping an issue whose worktree failed to open
        # and then reporting "every issue it was given has landed" is a lie the
        # next poll would repeat forever.
        say "  leaving #$picked in the queue to try again"
        break
      fi
      live_list="$(live_worktrees)" || break
      live="$(count_worktrees "$live_list")"
    done

    # Nothing left to launch, and nothing left to look after: done. Reaching
    # this in --auto is how it stops on an empty backlog; reaching it in list
    # mode is how it stops once every issue it was given has landed. Until then
    # it keeps polling, because reaping a merged worktree and enforcing the
    # time-box are its job in both modes.
    local owned; owned="$(ls "$OWNED_DIR" 2>/dev/null | grep -c .)"
    # ...minus the ones waiting for a PERSON. `park_worktree` keeps a worktree
    # owned when its removal was refused, which is right -- releasing it would
    # destroy what could not be removed. But the loop exits only on
    # `owned == 0`, and under a drain `queued` is forced to 0, so a parked
    # worktree made that the only exit and it never came: the dispatcher polled
    # forever, `status` never said idle, and `cmd_run` refuses a second
    # dispatcher while one is alive. `stop.sh` promises "exits once nothing is
    # left". armaatus/autofleet#37.
    local parked; parked="$(count_parked_owned)"
    [ "$parked" -gt 0 ] && owned=$(( owned - parked ))
    # Still clamped, and now it should be unreachable: `parked` counts distinct
    # owned issues, so it cannot exceed `owned`. Kept because a wrong answer here
    # ends the dispatcher with work in flight, and a clamp is cheaper than that.
    [ "${owned:-0}" -lt 0 ] && owned=0
    # THE DRAIN COMES FIRST, and in BOTH modes. Under a drain nothing launches,
    # so what is still queued cannot keep the dispatcher alive -- only what it
    # still owns can. The guard used to live on the `$auto` branch alone, and in
    # LIST mode that was the wedge this PR's own `--max-prs` fix created:
    #
    #   `wanted` is pruned only INSIDE the launch loop, and that loop is gated
    #   on `while ! $drain_mode`. So `run --max-prs 1 148` launches #148,
    #   `remaining+=("$n")` keeps it in `wanted` because it is in flight rather
    #   than done, the next iteration hits the cap, latches the drain, and the
    #   launch loop is never entered again. `queued` is stuck at 1 forever --
    #   including after #148's PR merges and `reap_merged` disowns it. `owned`
    #   reaches 0; `queued` never does.
    #
    # That is #37's wedge re-created by #36's fix, in the one mode #37's fix does
    # not cover, and `--auto --max-prs N` was fine (`wanted` is empty there),
    # which is why the suite stayed green. `--until` and `--for` in list mode had
    # the same shape already, so this fixes all three rather than `--max-prs`
    # alone -- which is what makes "equivalent to the other two" true.
    # Found by the independent review.
    local queued=0
    if $drain_mode; then
      queued=0
    elif [ "${#wanted[@]}" -gt 0 ]; then
      queued="${#wanted[@]}"
    elif $auto; then
      # Counted from the lists already in hand rather than by asking `in_flight`
      # per issue: that made two API calls each, and a 200-issue backlog on a
      # 60-second poll is how you meet gh's secondary rate limit.
      # ...and its documented non-zero kept. Discarded, `queued` was empty and
      # `[ "" -eq 0 ]` wrote a bash error to stderr every poll during a `gh`
      # outage -- exactly when the log most needs to be readable. "Could not
      # tell" is not "nothing left": it keeps polling.
      queued="$(count_startable)" || queued=1
    fi
    # THE PASS IS OVER, said only when asked for. See $AUTOFLEET_LOG_PASSES: off
    # by default because a line a minute is the log volume this file's say-once
    # markers exist to prevent, and the only thing that can answer "has a pass
    # finished" without guessing from a clock. Here rather than after the launch
    # loop, because everything a pass spends has been spent by this line --
    # `count_startable` above is the last call any pass makes.
    [ "${AUTOFLEET_LOG_PASSES:-off}" = on ] \
      && say "pass complete: $owned owned, $queued startable"
    if [ "$queued" -eq 0 ] && [ "${owned:-0}" -eq 0 ]; then
      if $drain_mode; then
        # NOT "everything in flight has landed" when something has not: the
        # parked worktrees -- named after this line now, in the block below
        # `fleet down` -- are exactly the work that did not land, and signing off
        # with the one thing that did not happen is how a reader stops reading
        # the lines that say what to do about it. Found by the independent
        # review.
        if [ "${parked:-0}" -gt 0 ]; then
          reason="${reason:-you stopped it}; everything else in flight has landed, and $parked worktree(s) are waiting for you"
        else
          reason="${reason:-you stopped it}; everything in flight has landed"
        fi
      elif $auto && $declined; then
        reason="the backlog has nothing startable left, and what it was given was declined"
      elif $auto; then
        reason="the backlog has nothing startable left"
      elif $declined; then
        reason="every issue it was given has landed or was declined"
      else
        reason="every issue it was given has landed"
      fi
      break
    fi
    sleep "$POLL_SECONDS"
  done

  say "fleet down: $reason"
  # ...AND AFTER IT, the lines naming what is parked and how to release it. See
  # `farewell_parked` for why they come last and why they are a function.
  farewell_parked "${parked:-0}"
  notify "fleet down" "$reason. $opened worktree(s) opened."
}

# Sourced by tests/test_fleet.sh, which exercises one function at a time against
# a stubbed runner. Executed, it dispatches as usual. (It said
# `tests/test_orca_fleet.sh` until the independent review of #1: that file was
# renamed with the runner seam and the sweep that fixed the sibling reference in
# `remove_worktree` passed over this one.)
[ "${BASH_SOURCE[0]}" = "$0" ] || return 0

case "${1:-}" in
  run)    shift; cmd_run "$@" ;;
  status) cmd_status ;;
  stop)   shift; cmd_stop "${1:-}" ;;
  resume) cmd_resume ;;
  retry)  shift; cmd_retry "$@" ;;
  # `cost` is NOT here. It is dispatched before the runner probe near the top of
  # this file, because it is the one subcommand that works without a runtime.
  *)
    cat >&2 <<USAGE
usage: fleet.sh <command>

  run 11 12 13                       work exactly these issues
  run --auto                         keep taking \`ready\` issues, most-unblocking first
  run --auto --until 08:00           ...and stop then
  run --auto --for 6h --max-prs 5    ...or after that long, or that many
  status                             what is running, and what is next
  stop [--now]                       drain (or stop the builds too)
  resume                             clear the stop
  retry 44                           hand back an issue the fleet gave up on
  cost [--json] [44 ...]             what each issue's worktree spent, in tokens

How to start it so it outlives the shell you type it in:
  $(runner_dispatcher_hint)
USAGE
    exit 2 ;;
esac
