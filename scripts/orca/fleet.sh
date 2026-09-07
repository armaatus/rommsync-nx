#!/usr/bin/env bash
# The fleet: turn a list of issues -- or the whole backlog -- into merged PRs.
#
# Everything below this line is deterministic shell. No model runs here. The
# dispatcher's whole job is to decide WHICH issue gets a worktree and WHEN, and
# that decision has to be readable, interruptible and cheap to run for hours.
# The thinking happens inside the worktrees it opens.
#
#   ./scripts/orca/fleet.sh run 11 12 13      # work exactly these, then hand off
#   ./scripts/orca/fleet.sh run --auto        # keep taking `ready` issues
#   ./scripts/orca/fleet.sh run --auto --until 08:00 --max-prs 5
#   ./scripts/orca/fleet.sh status            # what is running, what is next
#   ./scripts/orca/fleet.sh stop [--now]      # see "Stopping"
#   ./scripts/orca/fleet.sh resume
#
# Run it in an Orca terminal in the main worktree, so the dispatcher is as
# visible as the work it starts:
#
#   orca terminal create --worktree active --title fleet \
#     --command "./scripts/orca/fleet.sh run --auto"
#
# ## What it picks
#
# Anything `ready` and not already in flight, ordered by how many open issues
# name it in a `Blocked by #N` line. The work that frees the most other work goes
# first, which is the fastest way to turn a mostly-blocked backlog into a wide
# one. Milestones do not order it: `ready` already means every blocker is closed,
# and a milestone number is not a claim about what can be built now.
#
# ## Stopping
#
# The stop is a FILE, not a signal, and it lives outside every worktree
# ($HOME/.rommsync-fleet). That is deliberate: a signal only reaches a process
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
#           set it, and `await-review.sh` and `.claude/hooks/guard.py` are what
#           make it hold without cooperation.
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
. ./scripts/orca/lib.sh

# The state dir, the stop file and the owned-worktree registry come from
# lib.sh: `await-review.sh` and `guard.py` read the same paths, and a stop only
# some of them can see is not a stop.
STATE_DIR="$ORCA_FLEET_DIR"
STOP_FILE="$ORCA_FLEET_STOP"
DRAIN_FILE="$ORCA_FLEET_DRAIN"
OWNED_DIR="$ORCA_FLEET_OWNED"
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
# and once at startup, so a dispatcher never trusts a previous run's.
POLL_CACHE="$STATE_DIR/poll-cache"
forget_poll_answers() { rm -rf "$POLL_CACHE"; mkdir -p "$POLL_CACHE"; }
LOG="$STATE_DIR/fleet.log"
PIDFILE="$STATE_DIR/fleet.pid"

# Three, because the ceiling is not machine capacity -- it is how many streams
# one person can review properly (CLAUDE.md, "Working in parallel").
MAX_WORKTREES="${ROMMSYNC_FLEET_MAX:-3}"
POLL_SECONDS="${ROMMSYNC_FLEET_POLL:-60}"
# Long enough for a real issue including a full ctest run and both local review
# passes; short enough that an overnight run does not spend the night on the one
# task that was never going to work.
TIMEBOX_SECONDS="${ROMMSYNC_FLEET_TIMEBOX:-10800}"
FOUNDATION_LABEL="${ROMMSYNC_FOUNDATION_LABEL:-foundation}"
# An issue whose LAST step is outward, irreversible and the maintainer's --
# tagging a v1, touching a real console. The fleet may not finish one, so it does
# not start one, does not read an agent waiting on one as a stall, does not
# time-box it, and releases its worktree once there is nothing left in one (#139
# needed a write guard.py refuses from a fleet worktree, so its agent correctly
# produced no PR and reap_merged would have waited for one forever). See
# docs/WORKFLOW.md, "an issue whose last step is a person's".
HUMAN_STEP_LABEL="${ROMMSYNC_HUMAN_STEP_LABEL:-needs-human-step}"
# unblock.yml's label, derived from the `Blocked by #N` lines below each issue's
# marker. The fleet only ever READS it (CLAUDE.md), and it reads it for one
# thing: a worktree open on an issue that has SINCE gone blocked will never
# produce a merged PR, so it is holding one of three slots for nothing. #148 went
# blocked with its worktree open and kept #119, #122 and #139 queued behind work
# that could not start.
BLOCKED_LABEL="${ROMMSYNC_BLOCKED_LABEL:-blocked}"
# poll_issue answers two questions in one string; issue_state_in and
# issue_labels_in split it here.
ANSWER_SEP="$(printf '\t')"

mkdir -p "$OWNED_DIR" "$STARTED_DIR"
forget_poll_answers

say() { printf '%s  %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG"; }
die() { printf '%s\n' "$*" >&2; exit 1; }

# The two cases you would otherwise not learn about until morning: the fleet
# finishing, and an issue giving up. Everything else is on the Orca board.
notify() {
  command -v osascript >/dev/null 2>&1 || return 0
  osascript -e "display notification \"$(printf '%s' "$2" | sed 's/"/\\"/g')\" with title \"rommsync fleet\" subtitle \"$1\"" >/dev/null 2>&1 || true
}

# What every LAUNCH decision asks, and it is the drain rather than the stop: a
# hard stop sets both files, so this is true in both states and the dispatcher
# opens nothing new in either.
draining() { orca_fleet_draining; }
# ...and what only the SCREEN asks, because the two states need different words
# on it. Nothing in here gates on this: refusing to launch under one and not the
# other is how the two would drift apart.
hard_stopped() { orca_fleet_stopped; }
check_stop() {
  draining || return 1
  if hard_stopped; then
    say "stop file present ($STOP_FILE) -- not starting anything new"
  else
    say "drain file present ($DRAIN_FILE) -- not starting anything new"
  fi
  return 0
}

# ---------------------------------------------------------------- orca CLI ---
orca_cli_resolve || die "no orca CLI answers here; is the Orca app running?"

orca_json() {
  local out; out="$(mktemp)"
  orca_run_with_deadline 30 "$out" "$ORCA_CLI" "$@" --json
  local rc=$?
  cat "$out"; rm -f "$out"
  return $rc
}

# The Orca board is the status surface: `in-progress` while it builds,
# `in-review` once the PR is up (the agent sets that itself), `completed` on
# merge. The comment is the one line the card shows.
#
# A failed update is SAID, not swallowed. WORKFLOW.md calls the board the status
# surface, so a card that did not update is a board showing something that is not
# true -- and the `|| true` this replaces meant the dispatcher reported nothing
# wrong while it happened. When the Orca CLI broke on 2026-09-05 (lib.sh's
# orca_cli_resolve records it) every card in the fleet would have frozen in
# silence. Still non-fatal: the board is a display, and a display that cannot be
# written is not a reason to stop dispatching work.
card() {
  local path="$1" out rc; shift
  out="$(mktemp)"
  ORCA_RUN_CAPTURE_STDERR=1 orca_run_with_deadline 30 "$out" "$ORCA_CLI" worktree set \
    --worktree "path:$path" "$@" --json
  rc=$?
  if [ "$rc" != 0 ]; then
    say "  board update FAILED (rc $rc) for $path: $*"
    # The CLI's own words, capped: they are the difference between "the app is
    # not running" and "that worktree is gone", and both look like silence.
    while IFS= read -r line; do
      [ -n "$line" ] && say "    $line"
    done < <(sed -n '1,3p' "$out")
  fi
  rm -f "$out"
  return 0
}

# The agent terminal in one worktree, if it has one. The path goes in as an
# argument rather than into the source: a worktree path can contain anything a
# filename can.
agent_terminal_in() {
  orca_json terminal list | python3 -c '
import json, sys
for t in json.load(sys.stdin)["result"]["terminals"]:
    if (t.get("worktreePath") == sys.argv[1] and t.get("agentIdentity")
            and not t.get("orphaned")):
        print(t["handle"]); break
' "$1"
}

# Interrupt the agent in one worktree, if it has one. Both callers are about to
# take something away from it -- the time-box the rest of its hours, the release
# its whole directory -- and an agent that is not told keeps working against a rig
# that is going or already gone. `cmd_stop` spells this out for itself instead: it
# reports each interrupt it managed, which needs the exit code this swallows.
interrupt_agent_in() {
  local handle
  handle="$(agent_terminal_in "$1")"
  [ -n "$handle" ] || return 0
  orca_run_with_deadline 20 /dev/null "$ORCA_CLI" terminal send \
    --terminal "$handle" --interrupt --json >/dev/null 2>&1
  return 0
}

# --------------------------------------------------------------- the state ---
own() {
  printf '%s\n' "$2" >"$OWNED_DIR/$1"; date +%s >"$STARTED_DIR/$1"
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
clear_issue_markers() {
  rm -f "$STATE_DIR/stalled-$1" "$STATE_DIR/stall-labels-$1" \
        "$STATE_DIR/box-labels-$1" "$STATE_DIR/queue-labels-$1" \
        "$STATE_DIR/unreachable-$1" "$STATE_DIR/human-step-$1" \
        "$STATE_DIR/held-$1" "$STATE_DIR/stuck-$1" \
        "$STATE_DIR/merge-blind-$1" "$STATE_DIR/merge-held-$1" \
        "$STATE_DIR/orca-blind-$1" \
        "$STATE_DIR/git-blind-$1" "$STATE_DIR/warned-$1" \
        "$STATE_DIR/reason-blind-$1"
}

# `gaveup-` is the one per-issue marker deliberately NOT in that list, and the
# exception is load-bearing rather than an oversight. It records that this
# dispatcher stopped an agent at the time-box, and reap_abandoned then RELEASES
# that worktree -- which calls disown_issue. Clearing the record there would put
# the issue straight back at the front of a queue that has just spent three hours
# on it, once per cycle forever. It outlives the worktree on purpose, and only
# `fleet.sh retry N` clears it -- not a restart, because a crash and a reboot are
# not decisions about an issue.
gave_up_on()     { [ -e "$STATE_DIR/gaveup-$1" ]; }
gave_up_issues() { ls "$STATE_DIR" 2>/dev/null | sed -n 's/^gaveup-//p'; }

# The subset enforce_timebox owns, for its two exits.
forget_box_markers() {
  rm -f "$STATE_DIR/unreachable-$1" "$STATE_DIR/box-labels-$1" \
        "$STATE_DIR/human-step-$1"
}

disown_issue() {
  rm -f "$OWNED_DIR/$1" "$STARTED_DIR/$1"
  clear_issue_markers "$1"
}

# Non-zero when the answer could not be read, which is NOT the same as "nothing
# is running". Reading a failed CLI call as zero live worktrees is how one
# transient hiccup turns into three duplicate worktrees for issues that already
# have one: `in_flight` goes blind at the same moment, because it reads the same
# list.
live_worktrees() {
  local out; out="$(mktemp)"
  orca_run_with_deadline 30 "$out" "$ORCA_CLI" worktree list --json || {
    rm -f "$out"; return 1; }
  python3 -c '
import json, sys
try:
    worktrees = json.load(open(sys.argv[1]))["result"]["worktrees"]
except Exception:
    raise SystemExit(1)
for w in worktrees:
    if not w.get("isMainWorktree") and not w.get("isArchived"):
        print(w.get("linkedIssue") or "-", w["path"], sep="\t")
' "$out"
  local rc=$?
  rm -f "$out"
  return $rc
}

# Prints the count, or fails. A caller that cannot tell how many are running
# must not launch anything.
live_count() {
  local list
  list="$(live_worktrees)" || return 1
  printf '%s\n' "$list" | grep -c . || true
}

# --------------------------------------------------------------- the queue ---
# Every open issue, with how many other open issues are blocked BY it. That
# number is the ordering: the work that frees the most other work goes first.
# It reads the same `Blocked by #N` lines unblock.yml parses, so nothing new has
# to be maintained.
ready_issues() {
  GH_PAGER=cat gh issue list --state open --limit 200 \
    --json number,title,body,labels 2>/dev/null \
    | PYTHONPATH="$ISSUE_REFS" python3 -c '
import json, sys
from issue_refs import blocked_by
human_step = sys.argv[1]
issues = json.load(sys.stdin)
blocks = {}
for i in issues:
    for n in blocked_by(i.get("body")):
        blocks[n] = blocks.get(n, 0) + 1
# `ready` says every blocker is closed. It does not say an agent can finish the
# work: an issue whose last step belongs to a person stays open through the PR
# that prepares it, and stays `ready` with it. #148 was picked up again 18
# seconds after its own preparatory PR merged, and would have been picked up
# once per cycle forever, each attempt further from the point.
ready = [i for i in issues
         if any(l["name"] == "ready" for l in i.get("labels", []))
         and not any(l["name"] == human_step for l in i.get("labels", []))]
# Most-unblocking first, then oldest issue number: predictable inside a tie.
for i in sorted(ready, key=lambda i: (-blocks.get(i["number"], 0), i["number"])):
    labels = ",".join(l["name"] for l in i.get("labels", []))
    print(i["number"], blocks.get(i["number"], 0), labels, i["title"], sep="\t")
' "$HUMAN_STEP_LABEL"
}

# `ready` overstates availability: the label stays until the PR merges, so an
# issue with a PR already open still carries it.
# 0 = a PR closes it, 1 = none does, 2 = could not tell. The third answer is
# not decoration: python printing nothing is what "no PR" looks like, and it is
# also what a failed import or a malformed listing looks like. Read as "free",
# that opens a second worktree for work already in flight -- which is the whole
# failure this shared module exists to prevent.
has_open_pr() {
  local found
  # The issue number goes in as an ARGUMENT, not spliced into the source. A PR
  # body is third-party text and so, in principle, is anything that reaches the
  # pattern.
  found="$(GH_PAGER=cat gh pr list --state open --json number,body --limit 100 2>/dev/null \
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
  printf '%s\n' "$list" | cut -f1 | grep -qx "$1" && return 0
  has_open_pr "$1"; rc=$?
  [ "$rc" = 0 ] && return 0
  [ "$rc" = 2 ] && return 2
  return 1
}

# $1 is a comma-separated label list, $2 one label. Exact matches only: `ready`
# must not answer for `ready-ish`, and `foundation` must not answer for
# `foundational`. -F and -- keep that true for a label carrying a regex
# metacharacter or a leading dash, both of which the env overrides above allow.
has_label() { printf '%s' "$1" | tr ',' '\n' | grep -qxF -- "$2"; }
is_foundation() { has_label "$1" "$FOUNDATION_LABEL"; }

# Everything the watchers ask GitHub about ONE issue, in one call and one answer
# per POLL. Prints `<state><TAB><labels>`; 0 = read it, 2 = could not tell.
#
# The third answer is the same one has_open_pr gives, for the same reason: the
# callers below interrupt an agent, comment on an issue and delete a worktree,
# and a listing that could not be read is no basis for any of those. Asked live
# rather than cached at launch, because a label is often what a person adds AFTER
# seeing the card.
#
# One answer per issue per POLL, though. reap_abandoned, enforce_timebox and
# notice_stalled all ask about the same issue in the same pass -- an issue stuck
# long enough to overrun is often also the one sitting at a prompt, and the one a
# maintainer has just blocked -- and three calls for one answer is the pattern
# count_startable exists to avoid. State and labels come back together for that
# same reason: they are one `gh issue view`, not two. The cache lives for one
# pass, so a label a person adds is still seen on the next one.
poll_issue() {
  local cached="$POLL_CACHE/issue-$1" answer
  mkdir -p "$POLL_CACHE" 2>/dev/null
  [ -e "$cached.unreadable" ] && return 2
  [ -e "$cached" ] && { cat "$cached"; return 0; }
  if answer="$(GH_PAGER=cat gh issue view "$1" --json state,labels \
                 --jq '.state + "\t" + ([.labels[].name]|join(","))' 2>/dev/null)"; then
    printf '%s' "$answer" >"$cached" 2>/dev/null || true
    printf '%s' "$answer"
    return 0
  fi
  : >"$cached.unreadable" 2>/dev/null || true
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
  prs="$(GH_PAGER=cat gh pr list --state open --json body --limit 100 2>/dev/null)" || return 1
  ready="$(ready_issues)" || return 1
  # An issue the fleet gave up on is one it will decline every pass, so counting
  # it is the run loop polling forever for work that never starts.
  gaveup="$(gave_up_issues)"
  printf '%s\n' "$ready" | PYTHONPATH="$ISSUE_REFS" python3 -c '
import json, sys
from issue_refs import closes
running = {line.split("\t")[0] for line in sys.argv[1].splitlines() if line.strip()}
gave_up = {n for n in sys.argv[3].split() if n}
# One parse per BODY, not one over all of them joined: a keyword may be the last
# word of one body and `#12` the first token of the next, and `\s+` would span
# the join -- claiming an issue nobody is working on and hiding it from the
# count. One parse per issue would be the other way round; this is neither.
claimed = set()
for p in json.loads(sys.argv[2]):
    claimed.update(closes(p.get("body")))
n = 0
for line in sys.stdin:
    if not line.strip():
        continue
    issue = line.split("\t")[0]
    if issue in running or issue in gave_up or int(issue) in claimed:
        continue
    n += 1
print(n)
' "$live" "$prs" "$gaveup"
}

# --------------------------------------------------------------- the launch ---
slug() {
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]' \
    | tr -cs 'a-z0-9' '-' | sed 's/^-*//;s/-*$//' | cut -c1-48
}

agent_brief() {
  cat <<BRIEF
Run \`GH_PAGER=cat ./scripts/orca/issue-command.sh $1\` first and follow
everything it prints, including the review loop at the end. You were started by
the fleet dispatcher: work autonomously to a pull request that is waiting only on
GitHub's auto-merge, and do not stop to ask for confirmation on anything
CLAUDE.md already decides.

If \`$STOP_FILE\` appears at any point, stop: say where you got to and do nothing
further. Nothing can leave this worktree while it exists.
BRIEF
}

launch() {
  local num="$1" title="$2"
  local name; name="$(slug "$num-$title")"

  say "opening a worktree for #$num -- $title"
  local out; out="$(mktemp)"
  orca_run_with_deadline 240 "$out" "$ORCA_CLI" worktree create \
    --repo "path:$REPO_ROOT" \
    --name "$name" \
    --issue "$num" \
    --no-parent \
    --agent claude \
    --prompt "$(agent_brief "$num")" \
    --comment "starting #$num" \
    --json
  if [ $? != 0 ]; then
    say "  could not create it:"
    sed 's/^/    /' "$out" | head -5 | tee -a "$LOG"
    rm -f "$out"
    return 1
  fi
  local path
  path="$(python3 -c '
import json,sys
try:
    print(json.load(sys.stdin)["result"]["worktree"]["path"])
except Exception:
    print("")
' <"$out")"
  rm -f "$out"
  [ -n "$path" ] || { say "  created, but Orca reported no path; not tracking it"; return 1; }
  own "$num" "$path"
  card "$path" --workspace-status in-progress --comment "#$num: building"
  say "  #$num is running in $path"
}

# ---------------------------------------------------------------- the reap ---
# A worktree whose PR is merged has done its job and is holding a slot. Only ones
# this dispatcher created are touched, and only when nothing goes with them:
# nothing unpushed, and a clean working tree. Its RomM stack has to come down
# with it, or it survives under `restart: unless-stopped` holding two ports
# forever with nothing left on disk to identify it by -- but AFTER the removal,
# not before it. See remove_worktree.
# Remove a worktree, judged by whether it is GONE rather than by an exit code.
#
# #27 logged "could not remove it" at 02:08 and kept the slot; the very same
# command, run again by hand, removed it and printed
# `warning: local branch "..." was kept because Git could not safely delete it`.
# A non-zero exit here has meant both "nothing happened" and "it worked, with a
# caveat", and the fleet cannot tell those apart from the code alone. The
# filesystem can: the directory is there or it is not.
#
# This matters more than one stuck worktree. The fleet runs at a cap of three,
# and a slot held by a worktree whose work is already merged is a slot that never
# starts the next issue -- the loop quietly runs at two, then one.
#
# The second attempt adds --force, and that is the one that works.
#
# This repository has a real submodule -- overlay/lib/libultrahand, pinned in
# .gitmodules -- and `git worktree remove` refuses outright:
#
#   fatal: working trees containing submodules cannot be moved or removed
#
# So the plain call fails on every worktree the fleet has ever created, every
# time, and it is not intermittent. Three accumulated in about eighteen hours on
# 2026-09-07, each holding four containers, two ports and four volumes that come
# back on every `docker start` under `restart: unless-stopped`. Worse for
# throughput: reap_merged has already marked the card `completed` and disowned
# the issue by then, so `fleet.sh status` still shows the worktree while the
# dispatcher no longer counts it -- one slot idle for nearly three hours.
#
# Forcing is safe HERE specifically: both callers check the worktree holds
# nothing first. --force forces the worktree removal, not the branch deletion.
#
# ## Why the archive hook does not run through the CLI (#163)
#
# `orca worktree rm --run-hooks` runs orca.yaml's archive hook -- archive.sh,
# which takes the stack and its volumes down -- and it runs it BEFORE Orca
# decides whether it will remove the worktree at all. Orca then refuses (a dirty
# working tree, the submodule), and "could not remove it" has already destroyed
# the thing the worktree could not be worked in without:
#
#   16:49:15  #122: PR #159 is merged; marking it done and removing the worktree
#   16:49:32    could not remove it; sweep later with ./scripts/orca/reap.sh
#
# That agent was mid-ctest. `ipc.engine` failed after 90s with ~130 tests skipped
# behind it, and the fixture had lost its scan and its token -- none of which the
# log above suggests. So the order is inverted here: the removal is attempted
# with no hooks at all, and only a worktree that is genuinely GONE gets its stack
# swept. A refusal now changes nothing.
#
# The sweep is reap.sh, which is exactly the tool for "a stack whose worktree no
# longer exists" and needs no worktree to run -- CLAUDE.md names it as the manual
# counterpart of this same trade. Scoped with `--only` to the names read off the
# worktree before it went, so releasing one worktree does not also delete the
# database of an orphan somebody is still looking at; `--only` narrows reap.sh's
# stale set and can never widen it.
#
# The autostart watcher is the hook's other half, and it does not survive
# dropping --run-hooks by itself: its pidfile lives INSIDE the worktree, so it is
# read before the removal and signalled after one that worked. A watcher left
# behind polls the Orca runtime for a directory that is gone, forever.
# Returns 0 when the worktree is gone, 2 when the CLI never answered, and 1 when
# it answered and refused. The caller acts on the difference: a refusal is a
# decision about THIS worktree and is not worth retrying, while a deadline is
# Orca.app restarting and says nothing about the worktree at all.
remove_worktree() {
  local path="$1" out watcher projects env_project rc
  # The deadline each `worktree rm` gets, LOCAL rather than a constant beside the
  # other tunables: test_orca_browser.sh exercises this function by extracting it
  # with `sed` and sourcing it alone, so anything it reads from the file around it
  # arrives empty -- and an empty deadline is not 180, it is zero.
  local deadline="${ROMMSYNC_FLEET_RM_DEADLINE:-180}"
  # Pure reads, before anything can be destroyed. All three live INSIDE the
  # worktree and the sweep below needs them after it is gone.
  watcher="$(orca_read_autostart_watcher "$path")"
  # Both names archive.sh would have used. The derived one is what setup.sh
  # would have called this worktree; the one in .env is what its stack was
  # actually created under, and after a directory rename the two disagree with
  # the containers still running under the older name.
  projects=""
  orca_derive_env "$path" && projects="$orca_project"
  env_project="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' "$path/.env" 2>/dev/null | tail -1)"
  case " $projects " in
    *" $env_project "*) ;;
    *) [ -n "$env_project" ] && projects="$projects $env_project" ;;
  esac
  out="$(mktemp)"
  ORCA_RUN_CAPTURE_STDERR=1 orca_run_with_deadline "$deadline" "$out" "$ORCA_CLI" worktree rm \
    --worktree "path:$path" --json
  rc=$?
  if [ ! -d "$path" ]; then rm -f "$out"; finish_removal "$path" "$watcher" "$projects"; return 0; fi
  # Only when the CLI actually answered. A first call that hit the deadline means
  # nothing is answering, and a second 180s spent proving it doubles what a poll
  # costs while Orca.app restarts.
  if [ "$rc" != 124 ]; then
    ORCA_RUN_CAPTURE_STDERR=1 orca_run_with_deadline "$deadline" "$out" "$ORCA_CLI" worktree rm \
      --worktree "path:$path" --force --json
    rc=$?
    if [ ! -d "$path" ]; then rm -f "$out"; finish_removal "$path" "$watcher" "$projects"; return 0; fi
  fi
  if [ "$rc" = 124 ]; then
    rm -f "$out"
    say "  the Orca CLI did not answer in ${deadline}s -- nothing was torn down, and this is not a refusal"
    return 2
  fi
  # Labelled, because the caller's "could not remove it" comes after these and
  # an unlabelled fatal: line above it reads like the fleet's own.
  while IFS= read -r line; do
    [ -n "$line" ] && say "  the removal refused: $line"
  done < <(sed -n '1,3p' "$out")
  # The line #122 needed and did not get. Not "the worktree is still usable":
  # reap_abandoned interrupts the agent immediately before calling this, so on
  # that path somebody has just been stopped. What is true on both paths is that
  # the removal changed nothing, which is the fact that saves the next ctest.
  say "  nothing was torn down -- its RomM stack is still up, so whatever is in there still has its rig"
  rm -f "$out"
  return 1
}

# What --run-hooks used to do, run only once the worktree is established to be
# gone: stop its autostart watcher, and take down the stack it left behind.
# Named for the moment rather than for the hook, because it is no longer a hook
# and no longer runs inside the worktree it is tearing down.
#
# Never fatal: the worktree IS removed by the time this is called, so a docker
# that is down or a sweep that half-finished is a stack to collect later, not a
# removal to report as failed.
finish_removal() {
  local path="$1" watcher="$2" projects="$3" name out rc only=""
  orca_stop_autostart_watcher "$watcher" || true
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
  ORCA_RUN_CAPTURE_STDERR=1 orca_run_with_deadline 180 "$out" \
    "$REPO_ROOT/scripts/orca/reap.sh" --yes $only
  rc=$?
  cat "$out" >>"$LOG"
  rm -f "$out"
  # Said rather than swallowed, because a sweep that did not finish is two ports
  # and four volumes coming back on every `docker start`, with no worktree left
  # on disk to identify them by.
  [ "$rc" = 0 ] \
    || say "  $path is gone, but the stack sweep did not finish (rc $rc) -- see $LOG, then ./scripts/orca/reap.sh"
  return 0
}

# What both reaps do when a removal did not happen. `rc` is remove_worktree's:
# 2 means nobody answered, and that is not a decision about this worktree, so it
# is said once and retried on the next pass rather than parked.
#
# A parked worktree is NOT disowned. Both reaps iterate OWNED issues only, so an
# issue dropped on a failed removal is a worktree nothing ever looks at again --
# #122's stood from 16:49 until a person removed it. It keeps its slot, which is
# why both lines say so.
#
# The by-hand recovery is TWO commands, and the second is the one that is easy to
# forget: removing the directory makes the next pass disown the issue, and
# nothing then ever calls finish_removal for it. Its stack would come back on
# every `docker start` under `restart: unless-stopped`, holding two ports with no
# directory left to identify it by -- and the sweep is `--only`-scoped now, so no
# later removal collects it either.
BY_HAND_REMOVAL="git worktree remove --force '%s' && ./scripts/orca/reap.sh --yes"
park_worktree() {
  local num="$1" path="$2" rc="$3" what="$4" byhand
  # BY_HAND_REMOVAL is this file's own format string, not anything a caller sets.
  # shellcheck disable=SC2059
  byhand="$(printf "$BY_HAND_REMOVAL" "$path")"
  if [ "$rc" = 2 ]; then
    [ -e "$STATE_DIR/orca-blind-$num" ] && return 0
    : >"$STATE_DIR/orca-blind-$num"
    say "  could not ask -- leaving it owned, and trying again next pass"
    card "$path" --comment "#$num: $what, but the Orca CLI did not answer -- retrying"
    return 0
  fi
  rm -f "$STATE_DIR/orca-blind-$num"
  : >"$STATE_DIR/stuck-$num"
  say "  could not remove it; it keeps its slot until you do: $byhand"
  card "$path" --comment "#$num: $what, but the removal refused -- still here, still counted"
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
    # `@{u}..HEAD` is the older one. The WORKING TREE is the half #122 fell
    # through (#163): auto-merge fires the moment the last check passes, so
    # review fixes made after it -- #122's became #160 -- sit uncommitted here
    # while `@{u}..HEAD` is empty.
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
    unpushed="$(git -C "$path" log '@{u}..HEAD' --oneline 2>/dev/null | grep -c .)"
    [ "${unpushed:-0}" != 0 ] && holds="$unpushed unpushed commit(s)"
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
      card "$path" --comment "#$num: PR #$merged merged; kept -- git could not say what is in it"
      continue
    fi
    if [ -n "$holds" ]; then
      rm -f "$STATE_DIR/merge-blind-$num"
      [ -e "$STATE_DIR/merge-held-$num" ] && continue
      : >"$STATE_DIR/merge-held-$num"
      say "#$num: PR #$merged merged, but the worktree holds $holds -- leaving it"
      card "$path" --comment "#$num: PR #$merged merged, $holds here"
      continue
    fi
    rm -f "$STATE_DIR/merge-held-$num" "$STATE_DIR/merge-blind-$num"

    say "#$num: PR #$merged is merged; marking it done and removing the worktree"
    card "$path" --workspace-status completed --comment "#$num: merged in PR #$merged"
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
# holding four containers, two ports and four volumes: #44, which the time-box
# stopped at three hours for correctly producing nothing, and #148, which the
# maintainer blocked with its worktree open, keeping #119, #122 and #139 queued
# behind work that could never start.
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
# agent mid-change. CLAUDE.md is where the two come apart: an agent plans before
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
    # enforce_timebox's own record, so this one still answers through a GitHub
    # outage -- and so a lookup that failed above cannot be read as "no reason".
    [ -n "$reason" ] || ! gave_up_on "$num" || reason="the time-box stopped its agent with no PR"
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
      card "$path" --comment "#$num: $reason; kept -- git could not say what is in it"
      continue
    fi
    if [ -n "$holds" ]; then
      # It says WHAT is in there, the way reap_merged already reports unpushed
      # commits rather than removing them.
      rm -f "$STATE_DIR/git-blind-$num" "$STATE_DIR/warned-$num"
      [ -e "$STATE_DIR/held-$num" ] && continue
      : >"$STATE_DIR/held-$num"
      say "#$num: $reason, but the worktree holds $holds -- leaving it"
      card "$path" --comment "#$num: $reason; kept -- it holds $holds"
      continue
    fi
    rm -f "$STATE_DIR/held-$num" "$STATE_DIR/git-blind-$num"

    # The warning pass. One poll of notice, then the pair is asked again above --
    # so an agent that commits, or writes its plan to a file, keeps its worktree.
    if [ ! -e "$STATE_DIR/warned-$num" ]; then
      : >"$STATE_DIR/warned-$num"
      say "#$num: $reason, and the worktree holds nothing -- releasing it next pass unless something lands in it"
      # ...unless the time-box, earlier in this same pass, already interrupted it
      # and said so on the board. Scoped to the poll: this is about not saying one
      # thing twice in one minute, not about never saying it again.
      if [ ! -e "$POLL_CACHE/interrupted-$num" ]; then
        interrupt_agent_in "$path"
        card "$path" --comment "#$num: $reason; this worktree is released next pass unless something lands in it"
      fi
      continue
    fi

    say "#$num: $reason, and the worktree still holds nothing -- releasing the slot"
    # Interrupted again before the removal: an agent that ignored the warning
    # would otherwise keep writing into a directory being deleted, and lose its
    # rig with it the moment the removal lands (#163).
    interrupt_agent_in "$path"
    # The comment and no status. `completed` is reap_merged's word for work that
    # landed, and this worktree is being released precisely because it did not.
    # Phrased as what it is about to do, not as done: if the removal refuses, this
    # card is still on the board and still the line a person reads.
    card "$path" --comment "#$num: $reason; nothing is in it, removing the worktree"
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

# ---------------------------------------------------------- stalled agents ---
# An agent sitting at a confirmation prompt is not working, and nothing said so.
# #23 stopped inside two minutes on a `git submodule add` the auto-mode
# classifier wanted confirmed, while the board still read `in-progress` and the
# time-box had three hours to run. So when one asks, say so once, on the card and
# in a notification, and let a person decide. The alternative is a worktree that
# looks busy for three hours.
#
# "In auto mode nothing should be asking" was the premise, and it is false for an
# issue whose last step is outward and the maintainer's: #142 stopped before
# `git tag` and `gh release create` exactly as its issue told it to, and was
# reported at 10:19:20 as a stall for it. Both of these still reach a person --
# what changes is which signal they are. A stall means something is wrong; this
# one means the work is done as far as an agent may take it.
notice_stalled() {
  local f num path state listing rc
  # ONE listing per poll, matched against every owned worktree -- not one CLI
  # round-trip per worktree, which is three 30-second-deadline calls a minute
  # for an answer that arrives in a single response.
  listing="$(orca_json worktree ps 2>/dev/null)" || return 0
  for f in "$OWNED_DIR"/*; do
    [ -e "$f" ] || continue
    num="$(basename "$f")"; path="$(cat "$f")"
    [ -d "$path" ] || continue
    state="$(printf '%s' "$listing" | python3 -c "
import json, sys
try:
    for w in json.load(sys.stdin)['result']['worktrees']:
        if w.get('path') == sys.argv[1]:
            print(((w.get('agents') or [{}])[0]).get('state') or '')
            break
except Exception:
    pass
" "$path" 2>/dev/null)"
    [ "$state" = "waiting" ] || {
      rm -f "$STATE_DIR/stalled-$num" "$STATE_DIR/stall-labels-$num"; continue; }
    # Once per stall, not once per poll -- and checked before the lookup, so a
    # settled stall costs no `gh` call at all.
    [ -e "$STATE_DIR/stalled-$num" ] && continue
    # The third answer, and it must not be frozen behind the stall marker: a
    # single `gh` blip would otherwise record a #142-style false stall and never
    # re-evaluate it, which is the exact noise this change exists to remove.
    # `stall-labels-` throttles it instead.
    #
    # Each watcher owns its own once-per-outage marker and clears only that one.
    # The branch above drops `stall-labels-` whenever the agent stops being
    # `waiting`, which is the ordinary state of a grinding overrun -- so a marker
    # shared with enforce_timebox would be deleted seconds after that function
    # set it, restoring the line-a-minute the split exists to prevent. Distinct
    # from `unreachable-` for the same reason: that one means the PR lookup
    # failed, not this one.
    #
    # The single exception is `$POLL_CACHE/carded-`, which enforce_timebox writes
    # and this function only reads, and which the next pass empties anyway.
    issue_needs_human_step "$num"; rc=$?
    if [ "$rc" = 2 ]; then
      [ -e "$STATE_DIR/stall-labels-$num" ] && continue
      : >"$STATE_DIR/stall-labels-$num"
      say "#$num is waiting for input, and its labels could not be read -- asking again next poll"
      continue
    fi
    rm -f "$STATE_DIR/stall-labels-$num"
    : >"$STATE_DIR/stalled-$num"
    if [ "$rc" = 0 ]; then
      say "#$num is waiting for you, as expected -- its last step is yours to take"
      # ...unless enforce_timebox, which runs first in this same pass, already
      # said it. Only for this pass: the board is not a log, but a stall that
      # starts later is news no earlier comment covered.
      [ -e "$POLL_CACHE/carded-$num" ] \
        || card "$path" --comment "#$num: waiting for you -- as expected, not a stall"
      notify "#$num is waiting for you" "Its last step is yours to take."
    else
      say "#$num is waiting for input -- in auto mode nothing should be asking"
      card "$path" --comment "#$num: waiting for input -- needs you"
      notify "#$num needs you" "It is sitting at a prompt, not working."
    fi
  done
}

# ------------------------------------------------------------- the time-box ---
# An agent that cannot get green will grind. On expiry it is interrupted and the
# issue gets a comment saying so.
#
# What becomes of the worktree is reap_abandoned's call on the next pass, and it
# turns on what is IN it. One holding uncommitted work, or commits that are not
# on main, is left standing: a stuck task is exactly the one worth looking at and
# its diff is the evidence. One holding nothing is released, because there is
# nothing to look at and a slot is one of three -- #44 ground for three hours,
# produced nothing hard rule 1 allows, and then held its slot until it was
# removed by hand.
#
# Either way the fleet does not start that issue again: `gaveup-` outlives the
# worktree deliberately, since releasing the slot would otherwise hand it straight
# back to the same three hours. `fleet.sh retry N` is how it comes back.
enforce_timebox() {
  local f num path started now
  now="$(date +%s)"
  for f in "$STARTED_DIR"/*; do
    [ -e "$f" ] || continue
    num="$(basename "$f")"; started="$(cat "$f")"
    path="$(owned_path "$num")"
    [ -d "$path" ] || continue
    [ $((now - started)) -ge "$TIMEBOX_SECONDS" ] || continue
    # A PR being up means it got where it was going; the review loop has its own
    # cap and is not this timer's business. "Could not tell" is not "no PR":
    # this branch interrupts an agent and comments on its issue, and a lookup
    # that failed is no basis for either. The started marker stays, so the next
    # pass asks again -- an agent is only ever stopped on an answer.
    has_open_pr "$num"; case $? in
      # Every marker this function owns, not only the ones it happened to set on
      # the way here.
      0) rm -f "$f"; forget_box_markers "$num"; continue ;;
      # Once per outage, not once per poll, the same way notice_stalled does it:
      # the dispatcher polls every POLL_SECONDS, and an hour of GitHub being
      # unreachable would otherwise bury the log a person scans overnight under
      # a line a minute for every issue past its box.
      2) [ -e "$STATE_DIR/unreachable-$num" ] && continue
         : >"$STATE_DIR/unreachable-$num"
         say "#$num: timed out, but could not tell whether a PR is open -- leaving it for the next pass"
         continue ;;
    esac
    # An agent that stopped because the next step is not its to take has not
    # overrun: the box exists to stop work that will not get green, and a
    # decision only the maintainer can make is not that. #44 -- hardware, which
    # hard rule 1 forbids before the v1 gate -- was stopped at three hours for
    # correctly producing nothing.
    #
    # The started marker is KEPT. Deleting it would disarm the box for good, and
    # the label is exactly the thing a person takes off again to hand the issue
    # back to an agent -- which would then run uncapped forever. Said once, by
    # its own marker, rather than once a minute for as long as the label is on.
    issue_needs_human_step "$num"; case $? in
      # Not an exit -- the issue stays owned and past its box -- so it clears
      # only what this poll just proved stale, and keeps `human-step-` because
      # that is the marker it is about to write.
      0) rm -f "$STATE_DIR/box-labels-$num" "$STATE_DIR/unreachable-$num"
         [ -e "$STATE_DIR/human-step-$num" ] && continue
         : >"$STATE_DIR/human-step-$num"
         say "#$num: past the time-box, but it is labelled $HUMAN_STEP_LABEL -- leaving it to wait for you"
         # On the board too. An agent that finished its part and exited is not
         # `waiting`, so notice_stalled never speaks for it, and this worktree
         # keeps a slot until a person looks at it -- one line in fleet.log is
         # not where WORKFLOW.md says status lives.
         card "$path" --comment "#$num: waiting for you -- as expected, not a stall; past the time-box"
         # For notice_stalled, which runs later in THIS pass and would otherwise
         # repeat it. Scoped to the poll, not to the exemption: a stall that
         # begins hours from now is news, and has to reach the board.
         : >"$POLL_CACHE/carded-$num"
         continue ;;
      2) [ -e "$STATE_DIR/box-labels-$num" ] && continue
         : >"$STATE_DIR/box-labels-$num"
         say "#$num: timed out, but could not read its labels -- leaving it for the next pass"
         continue ;;
    esac
    # The other exit, and it owes the same tidiness. `unreachable-` is the one
    # this path used to drop: a PR lookup that failed on an earlier poll, then
    # answered on this one, left its marker standing for the next worktree on
    # the same issue to inherit and be silenced by.
    forget_box_markers "$num"

    say "#$num: $((TIMEBOX_SECONDS / 3600))h with no PR -- stopping it; the worktree goes if it holds nothing"
    interrupt_agent_in "$path"
    # For reap_abandoned, which runs later in THIS pass and would otherwise
    # interrupt the same agent and card the same worktree a second time. Scoped
    # to the poll, like `carded-`: it still gets its own warning next pass.
    : >"$POLL_CACHE/interrupted-$num"
    card "$path" --comment "#$num: timed out after $((TIMEBOX_SECONDS / 3600))h -- needs you"
    GH_PAGER=cat gh issue comment "$num" --body "The fleet stopped work on this after $((TIMEBOX_SECONDS / 3600)) hours with no pull request opened. Its worktree at \`$path\` is kept if it holds uncommitted work or commits that are not on \`main\`, and released otherwise so the slot is free. The fleet will not start this issue again on its own; \`./scripts/orca/fleet.sh retry $num\` hands it back." >/dev/null 2>&1 || true
    notify "#$num gave up" "$((TIMEBOX_SECONDS / 3600))h with no PR. Not starting it again."
    # Read by reap_abandoned on the next pass, and by the queue for as long as it
    # stands. Outlives the worktree on purpose -- see clear_issue_markers, which
    # deliberately does not clear it, and cmd_retry, which does.
    : >"$STATE_DIR/gaveup-$num"
    rm -f "$f"
  done
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
# `fleet.sh status` is run from wherever you are -- CLAUDE.md points agents in a
# fleet worktree at it. Hashing the caller's own copy compares a worktree
# branched before the fix against a dispatcher that predates it too, matches,
# and answers "current": #173's silence, rebuilt inside the check for it.
fleet_hash_stdin() { cksum | awk '{print $1 "-" $2}'; }
fleet_code_hash() {
  [ -r "$1/scripts/orca/fleet.sh" ] || return 0
  fleet_hash_stdin <"$1/scripts/orca/fleet.sh"
}
fleet_code_commit() {
  git -C "$1" log -1 --format=%H -- scripts/orca/fleet.sh 2>/dev/null
}

# The fleet.sh commits in $2..$3, in the repo at $1, or nothing. A ref that does
# not resolve -- no `origin`, a checkout with no history in common -- is nothing
# to name rather than an error on the screen.
fleet_commits_between() {
  [ -n "$2" ] || return 0
  git -C "$1" rev-parse --verify --quiet "$3" >/dev/null 2>&1 || return 0
  git -C "$1" log --oneline "$2..$3" -- scripts/orca/fleet.sh 2>/dev/null
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
# orca_stop_autostart_watcher takes the same precaution for the same reason,
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
  printf '%s\n' "$line" | grep -Eq 'fleet\.sh[[:space:]]+run([[:space:]]|$)'
}

# How to make a change live, said wherever the change is not. The cap is here
# because it has the same shape and is asked about far more often: MAX_WORKTREES
# is read once at start, so `ROMMSYNC_FLEET_MAX=4` in front of `status` changes
# nothing at all.
# $2 is the pull a BEHIND checkout needs, printed in its place in the sequence
# rather than ahead of it. The steps are in the order docs/WORKFLOW.md gives
# them, and they have to stay in it: this output points the reader at that
# section, and a screen that contradicts the page it cites is worse than either
# alone.
restart_advice() {
  local root="${1:-}" pull="${2:-}"
  echo "  Restart it -- it is the only way a change to fleet.sh takes effect:"
  echo "    ./scripts/orca/stop.sh          # drains: the agents in flight finish"
  echo "    ./scripts/orca/fleet.sh status  # until it says idle"
  [ -n "$pull" ] && echo "    $pull"
  echo "    ./scripts/orca/fleet.sh resume"
  # Named, not relative. This report is meant to be read from a fleet worktree,
  # and `./scripts/orca/fleet.sh run --auto` there starts a dispatcher whose cwd
  # and checkout the fleet removes as soon as that worktree's PR merges.
  if [ -n "$root" ]; then
    echo "    cd $root && ./scripts/orca/fleet.sh run --auto"
  else
    echo "    ./scripts/orca/fleet.sh run --auto   # from the MAIN worktree"
  fi
  echo "  ROMMSYNC_FLEET_MAX, _POLL and _TIMEBOX are read at start too, so they"
  echo "  change only across a restart. docs/WORKFLOW.md, 'Restart it'."
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

  ./scripts/orca/fleet.sh status
      what it is running, whether the fleet.sh it parsed is still current, and
      the \`git pull --ff-only\` when its checkout never pulled the fix -- a
      restart without that one starts the same bytes over.

To take over -- the usual reason to start a second one is that the first is
running stale code. With nothing in flight, drain it:

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
  ./scripts/orca/fleet.sh status  # until it says idle
REFUSED
    if [ -n "$root" ]; then
      echo "  cd $root && ./scripts/orca/fleet.sh run --auto"
    else
      echo "  ./scripts/orca/fleet.sh run --auto   # from the MAIN worktree"
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
  if [ -z "$hash" ] || [ -z "$root" ] || [ ! -r "$root/scripts/orca/fleet.sh" ]; then
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
    remote="$(git -C "$root" show origin/main:scripts/orca/fleet.sh 2>/dev/null | fleet_hash_stdin)"
    [ -n "$remote" ] && [ "$remote" = "$hash" ] && return 0
    echo
    echo "  BEHIND -- $root has not pulled these, so they are NOT live in the"
    echo "  dispatcher running, and a restart alone will not make them live:"
    printf '%s\n' "$log" | sed 's/^/    /'
    restart_advice "$root" "git -C $root pull --ff-only"
    return 0
  fi

  echo
  echo "  STALE -- $root/scripts/orca/fleet.sh has changed since it started, and"
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
  echo "             ./scripts/orca/fleet.sh status  # until it says idle"
  if [ -n "$root" ]; then
    echo "             cd $root && ./scripts/orca/fleet.sh run --auto"
  else
    echo "             ./scripts/orca/fleet.sh run --auto   # from the MAIN worktree"
  fi
  echo "           Or ./scripts/orca/stop.sh --now, which stops it and freezes"
  echo "           every agent with it."
}

# --------------------------------------------------------------- commands ---
cmd_status() {
  echo "fleet state: $STATE_DIR"
  # A stop and a running dispatcher are not alternatives: a drain leaves the
  # dispatcher up on purpose, because it is what reaps a worktree once its PR
  # merges -- and that draining dispatcher is running whatever code it parsed.
  # Which of the two, in the word the rest of the fleet uses for it. They are
  # not the same state and the person reading this is deciding whether to wait:
  # under a drain the agents are finishing and their PRs will land, under a stop
  # nothing they do can reach GitHub at all.
  if hard_stopped; then
    echo "STOPPED  ($STOP_FILE -- nothing goes out; clear with: ./scripts/orca/fleet.sh resume)"
  elif draining; then
    echo "DRAINING ($DRAIN_FILE -- no new worktrees; the agents in flight finish"
    echo "          and their PRs land. Clear with: ./scripts/orca/fleet.sh resume)"
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
  live_worktrees | while IFS="$(printf '\t')" read -r num path; do
    printf '  #%-5s %s\n' "$num" "$path"
  done
  echo
  echo "next up (ready, not in flight, not labelled $HUMAN_STEP_LABEL;"
  echo "         'unblocks' is how many issues it frees):"
  printf '  %-6s %-9s %s\n' "issue" "unblocks" "title"
  ready_issues | while IFS="$(printf '\t')" read -r num unblocks labels title; do
    in_flight "$num" && continue
    gave_up_on "$num" && continue
    printf '  #%-5s %-9s %s\n' "$num" "$unblocks" "$title"
  done | head -12

  # ...and where the ones missing from that list went, since a `ready` issue the
  # dispatcher silently declines forever is the confusing half of this.
  local gaveup; gaveup="$(gave_up_issues)"
  if [ -n "$gaveup" ]; then
    echo
    echo "gave up on (hand one back with: ./scripts/orca/fleet.sh retry N):"
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
      echo "stop set: $STOP_FILE"
      echo "  no new worktrees, and no agent can push, open a PR or comment."
      # --now reaches the agents the fleet started. --all reaches every agent
      # Orca knows about, including sessions a person opened by hand -- which is
      # a bigger hammer than a fleet stop, so it has to be asked for by name.
      echo "  interrupting agents..."
      local handle path
      if [ "$mode" = "--all" ]; then
        orca_json terminal list | python3 -c '
import json, sys
for t in json.load(sys.stdin)["result"]["terminals"]:
    if t.get("agentIdentity") and not t.get("orphaned"):
        print(t["handle"])
' | while read -r handle; do
          orca_run_with_deadline 20 /dev/null "$ORCA_CLI" terminal send \
            --terminal "$handle" --interrupt --json >/dev/null 2>&1 \
            && echo "    interrupted $handle"
        done
      else
        for f in "$OWNED_DIR"/*; do
          [ -e "$f" ] || continue
          path="$(cat "$f")"
          handle="$(agent_terminal_in "$path")"
          [ -n "$handle" ] || continue
          orca_run_with_deadline 20 /dev/null "$ORCA_CLI" terminal send \
            --terminal "$handle" --interrupt --json >/dev/null 2>&1 \
            && echo "    interrupted #$(basename "$f")"
        done
      fi
      # Only here. A drain has to leave the dispatcher alive: it is what reaps a
      # worktree once its PR merges, and killing it strands them.
      #
      # dispatcher_alive before the signal, which is the whole of lib.sh's
      # orca_stop_autostart_watcher in one line: this is the only place the
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
      echo "  no new worktrees. The agents in flight are NOT frozen: they finish,"
      echo "  push, open their PRs and comment, because a merged PR is what releases"
      echo "  the worktree this drain is waiting on."
      echo "  The dispatcher stays up to reap those worktrees as their PRs land, and"
      echo "  exits once nothing is left. Watch it with: fleet.sh status."
      echo "  Use --now to interrupt the fleet's agents and freeze every outward"
      echo "  effect, --all to interrupt every agent Orca knows about."
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
  echo "stop cleared. Start again with: ./scripts/orca/fleet.sh run --auto"
}

# The counterpart to the time-box, and the ONLY way back onto the queue. By name
# and on purpose: the box fired because three hours produced no PR, and an issue
# handed back unchanged spends the next three the same way. Restarting the
# dispatcher deliberately does not clear these -- a crash and a reboot are not
# decisions about an issue.
cmd_retry() {
  [ "$#" -gt 0 ] || die "usage: fleet.sh retry ISSUE [ISSUE...]"
  local n
  for n in "$@"; do
    case "$n" in ''|*[!0-9]*) die "not an issue number: $n" ;; esac
  done
  for n in "$@"; do
    if [ -e "$STATE_DIR/gaveup-$n" ]; then
      rm -f "$STATE_DIR/gaveup-$n"
      echo "#$n is startable again."
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

cmd_run() {
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
    local which_file="$DRAIN_FILE"; hard_stopped && which_file="$STOP_FILE"
    if dispatcher_alive "$held"; then
      die "the fleet is draining ($which_file), and pid $held is still DRAINING:
launching nothing new, and reaping what is in flight until nothing it owns is
left, which is what a drain is. It exits on its own; watch it with
\`fleet.sh status\`, and only then start one.

\`fleet.sh resume\` clears the stop. It does NOT make room for a second
dispatcher -- MAX_WORKTREES is enforced per process, so \`run\` would refuse
while that one is up."
    fi
    die "the fleet is draining ($which_file). Clear it with: fleet.sh resume"
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

  echo $$ >"$PIDFILE"
  # Written next to the pidfile and removed with it: a record of a dispatcher
  # that is not running would report staleness about nothing.
  record_dispatcher
  # ...but removed only while they still name THIS process. Nothing stops a
  # second dispatcher from starting and claiming both files, and an unconditional
  # `rm` would then have the first one's exit delete the second one's record --
  # leaving a live dispatcher reported as idle, with nothing to check its code
  # against. That is the silence this whole file's staleness report exists to end.
  trap 'release_dispatcher_files' EXIT
  say "fleet up: max $MAX_WORKTREES worktrees, polling every ${POLL_SECONDS}s, ${TIMEBOX_SECONDS}s per issue"
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
    if draining && ! $drain_mode; then
      drain_mode=true
      say "draining -- launching nothing more, still reaping what is in flight"
    fi
    if [ -n "$deadline" ] && [ "$(date +%s)" -ge "$deadline" ] && ! $drain_mode; then
      drain_mode=true
      say "deadline passed -- launching nothing more, still reaping what is in flight"
      reason="the deadline passed"
    fi

    forget_poll_answers
    # The order is load-bearing in three places. reap_merged first, because a
    # worktree whose PR merged is its business and reap_abandoned only ever looks
    # at what is left owned. enforce_timebox before notice_stalled, because the
    # first writes `$POLL_CACHE/carded-` and the second reads it to avoid
    # repeating a board comment it has already made. And reap_abandoned LAST of
    # the four: it is the one that removes a worktree, and running it earlier
    # took the "waiting for you, as expected" notification away from the very
    # worktrees it exists to release -- the watchers would have found nothing
    # there to speak about.
    reap_merged
    enforce_timebox
    notice_stalled
    reap_abandoned
    prune_gaveup

    local live
    if ! live="$(live_count)"; then
      say "could not read the worktree list; skipping this pass rather than guessing"
      sleep "$POLL_SECONDS"
      continue
    fi

    while ! $drain_mode && [ "$live" -lt "$MAX_WORKTREES" ]; do
      check_stop && { reason="you stopped it"; break 2; }
      [ -n "$max_prs" ] && [ "$opened" -ge "$max_prs" ] && { reason="it opened $opened worktree(s)"; break 2; }

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
            say "#$n is a foundation issue; waiting for the other $live worktree(s) to land"
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
      else
        # It stays in the queue. Dropping an issue whose worktree failed to open
        # and then reporting "every issue it was given has landed" is a lie the
        # next poll would repeat forever.
        say "  leaving #$picked in the queue to try again"
        break
      fi
      live="$(live_count)" || break
    done

    # Nothing left to launch, and nothing left to look after: done. Reaching
    # this in --auto is how it stops on an empty backlog; reaching it in list
    # mode is how it stops once every issue it was given has landed. Until then
    # it keeps polling, because reaping a merged worktree and enforcing the
    # time-box are its job in both modes.
    local owned; owned="$(ls "$OWNED_DIR" 2>/dev/null | grep -c .)"
    local queued=0
    if [ "${#wanted[@]}" -gt 0 ]; then
      queued="${#wanted[@]}"
    elif $auto && ! $drain_mode; then
      # Counted from the lists already in hand rather than by asking `in_flight`
      # per issue: that made two API calls each, and a 200-issue backlog on a
      # 60-second poll is how you meet gh's secondary rate limit.
      queued="$(count_startable)"
    fi
    if [ "$queued" -eq 0 ] && [ "${owned:-0}" -eq 0 ]; then
      if $drain_mode; then
        reason="${reason:-you stopped it}; everything in flight has landed"
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
  notify "fleet down" "$reason. $opened worktree(s) opened."
}

# Sourced by tests/test_orca_fleet.sh, which exercises one function against a
# stubbed CLI. Executed, it dispatches as usual.
[ "${BASH_SOURCE[0]}" = "$0" ] || return 0

case "${1:-}" in
  run)    shift; cmd_run "$@" ;;
  status) cmd_status ;;
  stop)   shift; cmd_stop "${1:-}" ;;
  resume) cmd_resume ;;
  retry)  shift; cmd_retry "$@" ;;
  *)
    cat >&2 <<USAGE
usage: fleet.sh <command>

  run 11 12 13                       work exactly these issues
  run --auto                         keep taking \`ready\` issues, most-unblocking first
  run --auto --until 08:00           ...and stop then
  run --auto --for 6h --max-prs 5    ...or after that long, or that many
  status                             what is running, and what is next
  stop [--now]                       drain (or interrupt the agents too)
  resume                             clear the stop
  retry 44                           hand back an issue the time-box gave up on

Run it in an Orca terminal so it is as visible as the work it starts:
  orca terminal create --worktree active --title fleet --command "./scripts/orca/fleet.sh run --auto"
USAGE
    exit 2 ;;
esac
