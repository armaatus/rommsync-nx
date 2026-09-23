#!/usr/bin/env bash
# The knobs a host project sets, and their defaults. Sourced by lib.sh, never
# executed -- but not therefore harmless: the validation at the foot of this file
# `exit`s on a knob it cannot accept, and in a sourced file that ends the
# SOURCING shell. A bad value in `.autofleet/config` is fatal to every fleet
# command, `stop.sh` included. Deliberate, and argued where the check is.
#
# autofleet ships no knowledge of any one project. Everything it needs to know
# about the repo it is driving -- what a worktree has to provision, which labels
# gate the work, what a stack is called -- arrives through this file and through
# `.autofleet/config` in the host repo, which is read after the defaults below
# and can override any of them.
#
# Environment beats the config file, which beats these defaults, so a one-off
# `AUTOFLEET_MAX=1 ./scripts/fleet/fleet.sh run --auto` still works.

# ---------------------------------------------------------------- dispatcher
# How many worktrees run at once. Three is the number rommsync-nx settled on:
# enough parallelism to matter, few enough that a human can still read what
# happened.
: "${AUTOFLEET_MAX:=3}"
# How often the dispatcher re-reads the world, in seconds.
: "${AUTOFLEET_POLL:=60}"
# THE TIME-BOXES ARE GONE. What bounded an issue used to be hours -- a build box
# and a second one for the answering half -- because an interactive session runs
# until something stops it. See AUTOFLEET_BUILD_MAX_TURNS and
# AUTOFLEET_BUILD_MAX_BUDGET_USD under "the runner": a run ends by itself now,
# and hours were never the thing that cost anything anyway.
# How long a worktree removal may take before it is reported as refused.
: "${AUTOFLEET_RM_DEADLINE:=180}"

# ---------------------------------------------------------------- the labels
# The convention `unblock.yml` maintains and `fleet.sh` reads.
#
# RENAMING WORKS FOR THREE OF THESE, and the doc says which. `AUTOFLEET_READY_LABEL`
# is read by nothing -- `ready_issues` fetches every open issue and then filters
# on the LITERAL word in its Python block, never passing `gh` a `--label` -- so
# renaming it hands you an empty queue forever, with nothing on screen saying
# why. armaatus/autofleet#57. That sentence lives in docs/CONFIGURATION.md, and
# this is the file somebody actually has open while renaming, so it says it too.
# Found by the independent review, which pointed out the invitation was here and
# the warning was one file away.
: "${AUTOFLEET_READY_LABEL:=ready}"
: "${AUTOFLEET_BLOCKED_LABEL:=blocked}"
# An issue that defines an interface later issues include: it lands alone.
: "${AUTOFLEET_FOUNDATION_LABEL:=foundation}"
# An issue whose last step is a person's. The fleet opens no worktree for it.
: "${AUTOFLEET_HUMAN_STEP_LABEL:=needs-human-step}"
# Work that goes before the queue's own ordering. The only one of these labels
# that says WHEN rather than WHAT: the other four are properties of the issue
# (two of them derived by `unblock.yml`, two applied by a person), and this one
# is a property of the week. It moves an issue to the FRONT of the ready list and
# does nothing else -- it cannot start a blocked issue, cannot start a
# `needs-human-step` one, and does not lift a foundation hold.
: "${AUTOFLEET_PRIORITY_LABEL:=priority}"

# ---------------------------------------------------------------- the runner
# Which driver creates a worktree and runs the build in it. `headless` is the
# default and needs nothing but `git`, `gh` and the build command; `orca` runs
# the identical build in a terminal on a machine that has the app, so the
# maintainer can watch it. The contract a third one has to meet is docs/RUNNERS.md, which
# CLAUDE.md hard rule 4 names as the authority -- fleet/runner/README.md, which
# this used to point at, is about the seam and lists no `runner_*` at all.
#
# A name with no `scripts/fleet/runner/<name>.sh` beside it is named where
# lib.sh sources it -- the file it looked for and the drivers that do ship --
# and stops the three scripts that call `fleet_require_runner`: the dispatcher,
# the setup hook and the board -- refused where `lib.sh` sources the driver,
# before an agent is ever opened.
: "${AUTOFLEET_RUNNER:=headless}"

# What the build agent is, and how much of it one issue may have.
#
# THE TIME-BOX IS THESE TWO NUMBERS NOW. The old one was a wall clock the
# dispatcher enforced by interrupting a live session with a turn that asked it
# to stop, which needed a session that could be typed into and a runtime that
# would relay the keystrokes. `claude -p` ends by itself at either of these, and
# the dispatcher's job shrinks to reading the exit and saying which one it was.
# armaatus/autofleet#41, armaatus/autofleet#151.
#
# A budget rather than only a turn count because the two fail differently: a
# build that reads whole files burns dollars at a low turn count, and one that
# greps in a loop burns turns cheaply. The defaults are this repository's
# measurements -- a median issue here has cost about $20 -- and a host project
# is expected to move them, which is what docs/CONFIGURATION.md says.
: "${AUTOFLEET_BUILD_CMD:=claude}"
# WHAT THE BUILD MAY DO WITHOUT ASKING, and this default is NOT the one
# armaatus/autofleet#151 asked for. The issue says `acceptEdits`; that mode
# auto-accepts FILE EDITS ONLY, so every Bash call not on the settings
# allow-list still asks -- and under `-p` there is nobody to ask, so it is
# denied. A build that edits files and cannot run `git commit`, `git push` or
# the test command is not a build. `auto` is Claude Code's own per-call
# decision and is what `.claude/settings.json` already sets for this
# repository's sessions. Set it to `acceptEdits` to get the issue's literal
# flag. Found by the local `/code-review` pass.
: "${AUTOFLEET_BUILD_PERMISSION_MODE:=auto}"
: "${AUTOFLEET_BUILD_MAX_TURNS:=400}"
# WHAT ONE BUILD MAY SPEND. 15 rather than the 25 this was: 25 was a first
# guess written beside the measurement it was guessing from, and
# armaatus/autofleet#154 -- the first unattended night on a real host -- budgets
# a build at $15. A number that ends a run is only useful at the value somebody
# is willing to pay, and nothing else in the loop reads this one.
: "${AUTOFLEET_BUILD_MAX_BUDGET_USD:=15}"
# THE THIRD BOUND, and the one the other two cannot be: turns and dollars end a
# build that is still SPENDING. A build wedged on a prompt nobody will answer, or
# on a network read that never returns, spends neither and reaches neither --
# and until armaatus/autofleet#161 the dispatcher had nothing to end it with, so
# it held a worktree and a slot until a person noticed. Two hours is longer than
# any build this repository has measured and short enough that a wedged one is
# not the whole night.
#
# Enforced by the dispatcher's poll, not by the build command line: that line is
# deliberately the same for both drivers, and the stop goes through
# `runner_build_stop` so the Orca driver -- which has no pid to signal -- is
# stopped the way it is started. A build stopped this way lands in the same
# state a turns or budget exhaustion lands in: `fleet.sh status` shows it under
# "gave up on" and `retry N` starts it again.
: "${AUTOFLEET_BUILD_TIMEOUT:=7200}"

# Where the headless driver puts the worktrees it creates. Empty means
# `$AUTOFLEET_DIR/trees`, which is the answer for every host that does not care.
# A host that keeps its checkouts on another volume sets this.
: "${AUTOFLEET_WORKTREE_ROOT:=}"

# ---------------------------------------------------------------- the review
#
# ONE REVIEW PER PULL REQUEST, and at most one fix answering it. That is the
# whole loop after the build (armaatus/autofleet#152), and these are the knobs
# that size it. What used to be here was five: a review mode, two self-review
# passes with three knobs of their own, a validation cap, a delta scope and a
# context ceiling. The loop they described never once ended on its own.

# What `review.sh` runs to produce the review, and `fix.sh` to answer it. A
# command on PATH, invoked with `-p`. Named rather than hardcoded so a project
# can point it at a wrapper -- a different model, a different account, an `ssh`
# to the machine that holds the subscription.
: "${AUTOFLEET_REVIEW_CMD:=claude}"
# How long one review may run before it is killed and the PR left for the next
# poll to pick up. Long enough for a real diff; short enough that a wedged
# reviewer is not an overnight hold on the worktree waiting for it.
: "${AUTOFLEET_REVIEW_TIMEOUT:=1800}"
# The reviewer's turn budget, passed through as `--max-turns`. The wall clock
# above is the backstop for a wedged process; this is the bound the reviewer can
# see and spend against.
#
# LOWER THAN THE 80 THIS USED TO BE, because the reviewer's job got smaller in
# the same change: it no longer submits its own verdict, no longer fans out into
# `/mattpocock-skills:code-review`, and is handed the policy, the issue and the
# diff in its prompt rather than made to fetch them. Issue #152 asks for a
# review costing at most $4 against a measured $0.84-$8.65 per round.
: "${AUTOFLEET_REVIEW_MAX_TURNS:=40}"

# The byte ceiling on the diff inlined into the reviewer's prompt.
#
# A diff has no upper bound and a prompt does: PR #158 is 1,315,566 bytes, about
# 328k tokens, which is past the context window entirely -- so an uncapped
# inline is a review that fails before it reads anything, on exactly the change
# too big to review by eye. Past this the reviewer is handed `gh pr diff --stat`
# and reads the hunks itself with the `gh pr diff` and `git diff` it is already
# granted. Nothing is truncated: a reviewer given half a hunk reports
# confidently on the half it can see.
#
# 256K is about 64k tokens, which leaves the policy, the brief and the issue
# room in a 200k window. Raise it on a model with more, and `0` is not an off
# switch -- it would mean "never inline", which is the stat path for every PR.
: "${AUTOFLEET_REVIEW_DIFF_MAX:=262144}"

# ------------------------------------------------------------------- the fix
#
# The one session that answers a review asking for changes (`scripts/fleet/fix.sh`).
# Its own knobs rather than the build's, because it is a different shape of run:
# the build starts from an issue and an empty branch, this starts from a diff
# somebody has already read and a list of things to change in it.
#
# The PERMISSION MODE is the build's, deliberately and not by omission: this
# edits, tests, commits and pushes in the same worktree the build ran in, under
# the same guard hooks, and a fix that needed a different answer to "may I edit
# this" than the build did would mean the two disagree about what the worktree
# is.
: "${AUTOFLEET_FIX_MAX_TURNS:=120}"
: "${AUTOFLEET_FIX_MAX_BUDGET_USD:=8}"
# The wall clock, the same backstop the reviewer has and for the same reason.
# Longer, because this one runs the project's test suite.
: "${AUTOFLEET_FIX_TIMEOUT:=3600}"


# ------------------------------------------------------------- what is KEPT ---
#
# Every store under $FLEET_DIR only ever grew. On this machine the reviewer
# transcripts reached 184K across 46 files in under two days, 65% of them
# belonging to pull requests that had already merged, and `fleet.log` grew
# without rotation. None of it is read again: a transcript matters while its
# review is being answered, and a merged PR's never is.
#
# A cap stops a thing getting worse; only deletion makes it smaller. These are
# the two numbers that decide what goes. Set either to 0 to keep everything,
# which is what a host project debugging its own reviewer wants.

# Reviewer transcripts to keep PER OPEN pull request. Older ones for that PR go,
# and every transcript for a PR that is no longer open goes -- after one grace
# pass, and never while a reviewer for that PR is still writing to it.
#
# TRANSCRIPTS ARE ALL IT GOVERNS NOW. It also drove the `reviewed-<sha>` marker
# sweep until armaatus/autofleet#152 removed the markers; `prune_review_logs` is
# the one consumer left, so `0` means "keep every transcript" rather than "keep
# every piece of review state". Found by the independent review.
: "${AUTOFLEET_KEEP_REVIEWS:=3}"

# Bytes of fleet.log to keep. At the cap the file is rotated to fleet.log.1 --
# ONE generation, because the point is a bound, and two files at the cap is
# twice the cap.
#
# NOT while a reviewer is running: `review.sh` is spawned with `>>` on this file
# and holds the inode for up to AUTOFLEET_REVIEW_TIMEOUT, so the cap is a bound
# the fleet reaches between reviews rather than a hard ceiling.
: "${AUTOFLEET_LOG_MAX_BYTES:=1048576}"

# One line per dispatcher pass, saying the pass ended. OFF by default, because a
# line a minute is exactly the log volume the say-once markers elsewhere in this
# file exist to prevent -- and ON it is the only deterministic answer to "has a
# pass finished", which is a question both an operator watching a quiet fleet and
# a test asserting what a pass COST have to be able to ask. armaatus/autofleet#69
# measured the per-pass budget by watching call counts stop moving, which is a
# wall-clock guess; this is the signal that guess was standing in for.
#
# `on` or `off`, and validated below, because the first shape of this knob was
# "set to anything" -- read with `[ -n ]`, which turns ON for `0` and for `off`.
# A host writing `AUTOFLEET_LOG_PASSES=0` into `.autofleet/config` then gets the
# line a minute this default exists to prevent, with no diagnostic saying why.
# Found by the local review.
: "${AUTOFLEET_LOG_PASSES:=off}"

# ---------------------------------------------------- what replaced all this
# FIVE KNOBS USED TO LIVE HERE and they are gone with armaatus/autofleet#151:
# AUTOFLEET_HANDOFF_MAX_WORDS, AUTOFLEET_CONTEXT_RESET, AUTOFLEET_AGENT_CLEAR_CMD,
# AUTOFLEET_HANDOFF_GRACE_SECONDS and AUTOFLEET_CONTEXT_RECYCLE.
#
# Every one of them was about keeping ONE interactive session usable for the
# whole of an issue: cap the note it writes before being cleared, decide whether
# to clear it at the pull request, name the command that clears it, say how long
# it gets to write the note, and how often to do the whole dance mid-build. The
# cost they were managing is real -- sessions were measured past 900,000 tokens,
# most of it a build nobody was still reading -- and the cause was that the
# session could not be allowed to END.
#
# A `claude -p` run ends. AUTOFLEET_BUILD_MAX_TURNS and
# AUTOFLEET_BUILD_MAX_BUDGET_USD above are what bounds it, and a second run
# starts from the branch and the pull request, which is state that outlives any
# session and needs nothing written down. AUTOFLEET_BUILD_MAX_RUNS is how many
# of those one worktree gets.
: "${AUTOFLEET_BUILD_MAX_RUNS:=3}"

# ------------------------------------------------ what the cost report reads
# NOTHING TO CONFIGURE, which is the point. AUTOFLEET_TRANSCRIPT_DIR lived here
# and named where the agent CLI writes its session transcripts, because
# `cost.sh` reconstructed what an issue spent by slugging a worktree path,
# finding the matching directory and summing the `usage` block on every
# assistant message in it. That inference is gone: `--output-format json` makes
# the build print its own total, and it lands in
# `$AUTOFLEET_DIR/builds/<issue>/`, which the fleet already owns.
#
# A host that points AUTOFLEET_BUILD_CMD at a wrapper which does not honour the
# flag gets rows with no figures in them and a line on stderr saying so, rather
# than a knob to set. armaatus/autofleet#151.


# ---------------------------------------- the compression proxy, REMOVED
# AUTOFLEET_HEADROOM pointed the fleet's model calls at a local compressing
# proxy. Measured here it was worth 8.8% of what it could reach -- and what it
# could reach was the reviewer and the fix session, never the build, which is
# where 63% of an issue's tokens go. armaatus/autofleet#153 set the bar at 10%
# of the BUILD's tokens and it cleared neither half, so the knob, its URL, its
# TCP probe and the environment it exported are gone rather than carried.

# ------------------------------------------------------------- per-worktree
# The prefix every derived compose project name carries, and the thing reap.sh
# sweeps by. Must be unique to this project on this machine: reap.sh removes
# stacks matching it whose worktree is gone.
: "${AUTOFLEET_PROJECT_PREFIX:=af}"
# `name:base` pairs. Each worktree gets `base + offset`, written to .env as
# `NAME=<port>`. Empty means this project needs no ports.
: "${AUTOFLEET_PORTS:=}"
# The modulus the per-worktree offset is taken over. Also the width of each
# port range above, so the bases must be at least this far apart.
: "${AUTOFLEET_PORT_SPAN:=2000}"
# The compose file a worktree's stack is described by, relative to the repo
# root. Empty means this project runs no containers, and teardown skips docker
# entirely.
: "${AUTOFLEET_COMPOSE_FILE:=}"
# Extra arguments for the teardown `docker compose down`, e.g. `--profile tls`.
# A service whose profile is not active survives `down`, comes back under
# `restart: unless-stopped`, and holds its port with no worktree left to find
# it by.
: "${AUTOFLEET_COMPOSE_DOWN_ARGS:=}"

# ------------------------------------------------------------- project hooks
# Where the project says what a worktree needs. Both are optional, both run
# from the repo root with .env already exported, and a non-zero exit from the
# setup hook fails provisioning loudly rather than handing an agent a broken
# rig.
: "${AUTOFLEET_SETUP_HOOK:=.autofleet/setup.sh}"
: "${AUTOFLEET_TEARDOWN_HOOK:=.autofleet/teardown.sh}"
# What the summary tells the agent to run. Cosmetic, but it is the first thing
# an agent reads in a fresh worktree.
: "${AUTOFLEET_TEST_COMMAND:=}"

# -------------------------------------------------------------- niceties
# The title on the macOS notifications the dispatcher posts.
: "${AUTOFLEET_NOTIFY_TITLE:=autofleet}"

# The host repo's overrides, last so they win over the defaults but not over
# the environment -- every default above is `:=`, so an exported value is
# already in place by the time this runs, and a config file that uses `:=` too
# preserves that. A config file that assigns outright deliberately overrides
# even the environment.
if [ -f "${AUTOFLEET_CONFIG:-$REPO_ROOT/.autofleet/config}" ]; then
  . "${AUTOFLEET_CONFIG:-$REPO_ROOT/.autofleet/config}"
fi

# ----------------------------------------------------- knobs that must be sane
#
# AFTER the host config, because that is the route that matters: a value only
# checked before it is checked on the one path nobody uses. See the note by
# AUTOFLEET_REVIEW_MAX_TRIES above for why this knob in particular cannot be
# allowed through wrong -- a non-number makes `[ N -ge X ]` return 2, the cap
# test false, and the cap itself absent.
#
# FATAL, and fatal to every fleet command, not just the dispatcher: this file is
# sourced by lib.sh, so `exit` here takes the sourcing shell with it -- including
# `stop.sh`, which is the one you reach for when something is wrong. That is the
# intended trade and it is named here rather than discovered: the message says
# exactly which knob and what it got, and the fix is a one-line edit to the file
# the message is about. A cap that is silently absent is the failure this exists
# to prevent, and it cannot be prevented by a warning nobody reads in a
# dispatcher log.

# ONE refusal, four knobs. Spelled out inline it was two copies; the third and
# fourth would have been where a `-gt` met a `-ge` and one cap became
# off-by-one silently. The comment blocks below stay attached to the knobs they
# are about -- each of them is a failure somebody had -- and only the mechanism
# is shared.
#
# $1 the name, $2 the value, $3 the smallest legal value, $4 how to say that.
config_whole_number() {
  case "$2" in
    # Digits first, then a NUMERIC test for the floor. `''|*[!0-9]*|0` rejected
    # the literal `0` and let `00` straight through -- all digits, not that
    # literal -- and `[ 0 -ge 00 ]` is true.
    ''|*[!0-9]*)
      echo "$1 must be $4; got '$2'" >&2
      exit 2 ;;
  esac
  [ "$2" -ge "$3" ] || { echo "$1 must be $4; got '$2'" >&2; exit 2; }
}

# Digits first, then a NUMERIC test for positive. `''|*[!0-9]*|0` rejected the
# literal `0` and let `00` straight through -- all digits, not that literal --
# and `[ 0 -ge 00 ]` is true, so the cap is zero attempts: no PR is ever
# reviewed, every PR blocks on a review that cannot arrive, and the hold
# announces "0 reviewers on <sha> submitted nothing, which is the cap", which is
# the exact untrue line the 0 rejection exists to prevent. One spare zero and the
# guard was the failure. Found by the independent review.
config_whole_number AUTOFLEET_REVIEW_TIMEOUT "$AUTOFLEET_REVIEW_TIMEOUT" \
  1 "a positive whole number of seconds"
config_whole_number AUTOFLEET_REVIEW_MAX_TURNS "$AUTOFLEET_REVIEW_MAX_TURNS" \
  1 "a positive whole number of turns"
# The only consumer is `[ "$diff_bytes" -gt "$AUTOFLEET_REVIEW_DIFF_MAX" ]`, and
# a non-number makes `[` return 2, which reads as FALSE -- so the whole diff is
# inlined whatever its size, which is the failure the ceiling exists to prevent.
config_whole_number AUTOFLEET_REVIEW_DIFF_MAX "$AUTOFLEET_REVIEW_DIFF_MAX" \
  1 "a positive whole number of bytes"
# The fix session's three, refused for the same reason and with a sharper edge
# on the timeout: its only consumer is `[ "$waited" -ge "$AUTOFLEET_FIX_TIMEOUT" ]`
# in fix.sh, so a non-number makes `[` return 2, the test FALSE, and the
# deadline never fires -- a wedged fix then holds the worktree until the
# dispatcher's own time-box expires hours later.
config_whole_number AUTOFLEET_FIX_TIMEOUT "$AUTOFLEET_FIX_TIMEOUT" \
  1 "a positive whole number of seconds"
config_whole_number AUTOFLEET_FIX_MAX_TURNS "$AUTOFLEET_FIX_MAX_TURNS" \
  1 "a positive whole number of turns"
config_whole_number AUTOFLEET_FIX_MAX_BUDGET_USD "$AUTOFLEET_FIX_MAX_BUDGET_USD" \
  1 "a positive whole number of dollars (0 would mean the fix may spend nothing)"

# The same shape, for the same reason: anything that is not `on` would leave the
# quiet default, but silently -- and here the misspelling fails in the LOUD
# direction instead (`[ -n ]` read `0` and `off` as on), which is worse than
# either. See the knob's own comment.
case "$AUTOFLEET_LOG_PASSES" in
  on|off) ;;
  *) echo "AUTOFLEET_LOG_PASSES must be 'on' or 'off';" \
          "got '$AUTOFLEET_LOG_PASSES'" >&2
     exit 2 ;;
esac

# WHAT ONE RUN MAY SPEND, and the reason these are checked rather than trusted
# is the one stated above: the consumers are `claude -p` flags, and a
# non-number reaches the build command as an argument it refuses -- which is a
# build that dies the moment it starts, once per launch, forever.
config_whole_number AUTOFLEET_BUILD_MAX_TURNS "$AUTOFLEET_BUILD_MAX_TURNS" \
  1 "a positive whole number of turns"

config_whole_number AUTOFLEET_BUILD_MAX_RUNS "$AUTOFLEET_BUILD_MAX_RUNS" \
  1 "a positive whole number (1 means a build is never resumed)"

# ...and the wall clock, refused for the reason AUTOFLEET_REVIEW_TIMEOUT is: its
# only consumer is `[ "$age" -ge "$AUTOFLEET_BUILD_TIMEOUT" ]` in the poll, and
# a non-number makes `[` return 2, which reads as FALSE -- so the deadline never
# fires and the knob that exists to end a wedged build silently stops ending it,
# which is hard rule 3 in a number.
config_whole_number AUTOFLEET_BUILD_TIMEOUT "$AUTOFLEET_BUILD_TIMEOUT" \
  1 "a positive whole number of seconds"

# ...and the budget, which is the one of the three that is NOT a whole number.
# It went through no check at all while the comment beside its sibling argued
# exactly why it needed one -- a non-number reaches `--max-budget-usd` as an
# argument the build refuses, so every run dies the instant it starts and is
# resumed up to AUTOFLEET_BUILD_MAX_RUNS before the fleet gives up on the issue.
# Found by both local review passes.
case "$AUTOFLEET_BUILD_MAX_BUDGET_USD" in
  ''|*[!0-9.]*|*.*.*|.) 
    echo "AUTOFLEET_BUILD_MAX_BUDGET_USD must be a positive amount in dollars;" \
         "got '$AUTOFLEET_BUILD_MAX_BUDGET_USD'" >&2
    exit 2 ;;
esac
# ...and it must be more than nothing. COMPARED, not enumerated: the first
# version of this guard listed `0|0.|0.0|0.00|.0|.00`, and `00`, `0.000` and
# `0.0000` walked straight past it into the failure the guard was added to
# prevent. A list of spellings is not a test of a number. Found by
# `/mattpocock-skills:code-review`, on a guard added one round earlier to answer
# a finding of its own.
# NO EXTERNAL COMMAND, not even `awk`. This file is sourced by every fleet
# command including `setup.sh`, whose job is to diagnose a machine that is
# missing its basic tools -- and `tests/test_env.sh setup_fails_fast` drives it
# on a PATH stripped down to prove exactly that. An `awk` here turned "python3
# is missing" into "your budget of $25 is zero", which is the wrong machine
# named confidently. Caught by the suite, one commit after the guard was added.
#
# The first `case` above has already refused anything that is not digits and at
# most one dot, so "more than nothing" is "carries a digit that is not zero".
case "$AUTOFLEET_BUILD_MAX_BUDGET_USD" in
  *[1-9]*) ;;
  *) echo "AUTOFLEET_BUILD_MAX_BUDGET_USD must be more than \$0;" \
          "got '$AUTOFLEET_BUILD_MAX_BUDGET_USD', which is a build that may" \
          "spend nothing and therefore do nothing" >&2
     exit 2 ;;
esac

# THE CAP THAT BOUNDS WHAT AN ISSUE SPENDS, refused for the reason every other
# number in this section is: a non-number makes `[ N -ge X ]` return 2, bash
# reads 2 as false, and the cap never fires. Silently, which is worse here than
# for most of these, because this IS the bound on a loop that has already been
# measured running away.
#
#   BUILD_MAX_RUNS    `[ "$runs" -ge "$AUTOFLEET_BUILD_MAX_RUNS" ]` in
#                     fleet.sh's `build_exited`. False forever is the resume
#                     loop with no bound -- a run that ends the instant it
#                     starts, restarted every poll, spending the account one
#                     session at a time with the log saying "resuming".
# THE OLD KNOBS ARE AN ERROR, NOT AN ALIAS.
#
# A host repository that tuned one of these meant something by the number, and
# the shape it meant it about is gone. Aliasing any of them onto a surviving
# knob would keep the config file working and quietly change what it asks for,
# which is the failure every other check in this file is written against: a
# setting that is read, accepted, and does something else.
#
# So it is loud, once, on the first run after upgrading, naming what replaced
# the loop rather than pretending there is a knob for it.
#
# `${VAR+set}`, not `-n "${VAR:-}"`. A half-edited config leaves `KNOB=` with
# nothing after the `=` -- still a line about a knob nothing reads, and still
# someone who thinks they have configured the cap. `-n` sees an empty string and
# says nothing, which is the silent acceptance this check exists to refuse.
#
#   REVIEW_MAX_ROUNDS   bounded reviews-per-PR at four, when four was the shape.
#   REVIEW_MAX          one review, and the code no longer counts them.
#   VALIDATE_MAX        the validator is gone; a re-review answers what it asked.
#   SELF_REVIEW_*       the two pre-PR passes are gone; the reviewer is cheaper
#                       than either of them was.
#   REVIEW_SCOPE        there is no round two to take a delta of.
#   REVIEW_FULL_EVERY   likewise.
#   REVIEW_CONTEXT_MAX  likewise -- nothing is carried forward between rounds.
#   REVIEW_MAX_TRIES    bounded reviewers that submitted NOTHING. The verdict is
#                       a return value now, so that state does not exist.
#   REVIEW_MODE         there is one venue. `.github/workflows/claude-review.yml`
#                       is gone; a host that wants the review in Actions uses
#                       `anthropics/claude-code-action` directly.
# armaatus/autofleet#152.
retired=""
for knob in AUTOFLEET_REVIEW_MAX_ROUNDS AUTOFLEET_REVIEW_MAX AUTOFLEET_VALIDATE_MAX \
            AUTOFLEET_SELF_REVIEW_CMD AUTOFLEET_SELF_REVIEW_TIMEOUT \
            AUTOFLEET_SELF_REVIEW_MAX_TURNS AUTOFLEET_SELF_REVIEW_MAX \
            AUTOFLEET_REVIEW_SCOPE AUTOFLEET_REVIEW_FULL_EVERY \
            AUTOFLEET_REVIEW_CONTEXT_MAX AUTOFLEET_REVIEW_MAX_TRIES \
            AUTOFLEET_REVIEW_MODE; do
  # `eval` rather than `${!knob+set}`: the payload runs on the /bin/bash macOS
  # ships, which is 3.2, and indirect expansion there has no `+set` form.
  eval "[ -n \"\${$knob+set}\" ]" && retired="$retired $knob"
done
if [ -n "$retired" ]; then
  echo "These knobs are set and nothing reads them any more:$retired" >&2
  echo "The loop after the build is ONE review and at most ONE fix answering" >&2
  echo "it, then GitHub's own rules decide. What is left to tune:" >&2
  echo "  AUTOFLEET_REVIEW_CMD        what runs the review, and the fix" >&2
  echo "  AUTOFLEET_REVIEW_TIMEOUT    how long one review may take" >&2
  echo "  AUTOFLEET_REVIEW_MAX_TURNS  the reviewer's turn budget" >&2
  echo "  AUTOFLEET_FIX_MAX_TURNS     the fix session's" >&2
  echo "  AUTOFLEET_FIX_MAX_BUDGET_USD, AUTOFLEET_FIX_TIMEOUT" >&2
  echo "Remove the old lines from .autofleet/config; docs/CONFIGURATION.md has" >&2
  echo "a row for each of the replacements." >&2
  exit 2
fi

