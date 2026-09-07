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
# ($HOME/.rommsync-fleet/STOP). That is deliberate: a signal only reaches a
# process that is still healthy, and the case you most need a stop in is the one
# where something is not. Everything checks it -- this dispatcher before every
# action, `await-review.sh` between polls, and `.claude/hooks/guard.py`, which
# refuses to push, open a PR or comment while it exists. So a stopped fleet
# cannot produce outward effects even if an agent is mid-thought and never reads
# the news.
#
# ## What it will not do
#
# It does not merge -- `merge-gate` and GitHub's auto-merge do that. It does not
# touch a worktree it did not create. And it removes one only when nothing goes
# with it: either that worktree's PR merged and nothing is unpushed, or its issue
# can no longer produce a merged PR at all and the worktree is clean and holds no
# commit that is not already in origin/main.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

# The state dir, the stop file and the owned-worktree registry come from
# lib.sh: `await-review.sh` and `guard.py` read the same paths, and a stop only
# some of them can see is not a stop.
STATE_DIR="$ORCA_FLEET_DIR"
STOP_FILE="$ORCA_FLEET_STOP"
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

stopped() { orca_fleet_stopped; }
check_stop() {
  stopped || return 1
  say "stop file present ($STOP_FILE) -- not starting anything new"
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
        "$STATE_DIR/git-blind-$1" "$STATE_DIR/warned-$1"
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
# A worktree whose PR is merged has done its job and is holding a slot. Only
# ones this dispatcher created are touched, and only when nothing is unpushed.
# `--run-hooks` is not optional: without it orca.yaml's archive hook never runs,
# and the worktree's RomM stack survives under `restart: unless-stopped`, holding
# two ports forever with nothing left on disk to identify it by.
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
# Forcing is safe HERE specifically: the only caller checks the PR is merged and
# that nothing is unpushed first. --force forces the worktree removal, not the
# branch deletion.
remove_worktree() {
  local path="$1" out
  out="$(mktemp)"
  ORCA_RUN_CAPTURE_STDERR=1 orca_run_with_deadline 180 "$out" "$ORCA_CLI" worktree rm \
    --worktree "path:$path" --run-hooks --json
  if [ ! -d "$path" ]; then rm -f "$out"; return 0; fi
  ORCA_RUN_CAPTURE_STDERR=1 orca_run_with_deadline 180 "$out" "$ORCA_CLI" worktree rm \
    --worktree "path:$path" --run-hooks --force --json
  if [ ! -d "$path" ]; then rm -f "$out"; return 0; fi
  # Labelled, because the caller's "could not remove it" comes after these and
  # an unlabelled fatal: line above it reads like the fleet's own.
  while IFS= read -r line; do
    [ -n "$line" ] && say "  the removal refused: $line"
  done < <(sed -n '1,3p' "$out")
  rm -f "$out"
  return 1
}

reap_merged() {
  local f num path branch merged unpushed
  for f in "$OWNED_DIR"/*; do
    [ -e "$f" ] || continue
    num="$(basename "$f")"; path="$(cat "$f")"
    [ -d "$path" ] || { disown_issue "$num"; continue; }
    branch="$(git -C "$path" rev-parse --abbrev-ref HEAD 2>/dev/null)" || continue
    merged="$(GH_PAGER=cat gh pr list --head "$branch" --state merged \
                --json number --jq '.[0].number' 2>/dev/null)"
    [ -n "$merged" ] && [ "$merged" != "null" ] || continue

    unpushed="$(git -C "$path" log '@{u}..HEAD' --oneline 2>/dev/null | grep -c .)"
    if [ "${unpushed:-0}" != 0 ]; then
      say "#$num: PR #$merged merged, but $unpushed commit(s) are unpushed -- leaving it"
      card "$path" --comment "#$num: PR #$merged merged, $unpushed unpushed commit(s) here"
      continue
    fi

    say "#$num: PR #$merged is merged; marking it done and removing the worktree"
    card "$path" --workspace-status completed --comment "#$num: merged in PR #$merged"
    # NOT reap.sh: that removes RomM stacks whose WORKTREE IS GONE, so a worktree
    # that failed to delete is precisely the case it skips -- and it says
    # "nothing to reap", which reads like success.
    remove_worktree "$path" \
      || say "  could not remove it; by hand: git worktree remove --force '$path'"
    disown_issue "$num"
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
  out="$(git -C "$path" status --porcelain 2>/dev/null)" || return 1
  dirty="$(printf '%s' "$out" | grep -c .)"
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
  local f num path answer reason holds
  for f in "$OWNED_DIR"/*; do
    [ -e "$f" ] || continue
    num="$(basename "$f")"; path="$(cat "$f")"
    [ -d "$path" ] || { disown_issue "$num"; continue; }
    # Already tried once, and the removal refused. Not retried, because
    # `orca worktree rm --run-hooks` takes the RomM stack down BEFORE the removal
    # is known to succeed (#163) -- so a retry loop is a stack torn down once a
    # minute under whoever is still working in there.
    [ -e "$STATE_DIR/stuck-$num" ] && continue

    reason=""
    if answer="$(poll_issue "$num")"; then
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
    fi
    # enforce_timebox's own record, so this one still answers through a GitHub
    # outage -- and so a lookup that failed above cannot be read as "no reason".
    [ -n "$reason" ] || ! gave_up_on "$num" || reason="the time-box stopped its agent with no PR"
    [ -n "$reason" ] || continue

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
    # Interrupted again before the removal: `--run-hooks` archives this worktree's
    # RomM stack, and an agent that ignored the warning would lose its rig
    # mid-suite and then keep writing into a directory being deleted (#163).
    interrupt_agent_in "$path"
    # The comment and no status. `completed` is reap_merged's word for work that
    # landed, and this worktree is being released precisely because it did not.
    # Phrased as what it is about to do, not as done: if the removal refuses, this
    # card is still on the board and still the line a person reads.
    card "$path" --comment "#$num: $reason; nothing is in it, removing the worktree"
    if remove_worktree "$path"; then
      disown_issue "$num"
    else
      # NOT disowned. reap_merged iterates OWNED issues only, so an issue dropped
      # on a failed removal is a worktree nothing ever looks at again -- #122's
      # stood from 16:49 until it was removed by hand. Kept owned and not retried,
      # so it is a slot this dispatcher will go on counting until a person takes
      # it -- which is why both lines below say so.
      : >"$STATE_DIR/stuck-$num"
      say "  could not remove it; it keeps its slot until you do: git worktree remove --force '$path'"
      card "$path" --comment "#$num: the removal refused -- still here, still counted; git worktree remove --force '$path'"
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

# --------------------------------------------------------------- commands ---
cmd_status() {
  echo "fleet state: $STATE_DIR"
  if stopped; then
    echo "STOPPED  ($STOP_FILE -- clear with: ./scripts/orca/fleet.sh resume)"
  elif [ -e "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "running   (pid $(cat "$PIDFILE"))"
  else
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
  date '+stopped at %Y-%m-%d %H:%M:%S' >"$STOP_FILE"
  echo "stop set: $STOP_FILE"
  echo "  no new worktrees, and no agent can push, open a PR or comment."

  case "$mode" in
    --now|--all)
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
      if [ -e "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        kill "$(cat "$PIDFILE")" 2>/dev/null && echo "  dispatcher stopped."
      fi ;;
    "")
      echo "  running agents finish what they are on, and the dispatcher stays up to"
      echo "  reap their worktrees when their PRs land. It exits once nothing is left."
      echo "  Use --now to interrupt the fleet's agents, --all for every agent." ;;
  esac
  notify "stopped" "No new work will start."
}

cmd_resume() {
  rm -f "$STOP_FILE"
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
  stopped && die "the fleet is stopped ($STOP_FILE). Clear it with: fleet.sh resume"

  echo $$ >"$PIDFILE"
  trap 'rm -f "$PIDFILE"' EXIT
  say "fleet up: max $MAX_WORKTREES worktrees, polling every ${POLL_SECONDS}s, ${TIMEBOX_SECONDS}s per issue"
  $auto && say "mode: auto -- most-unblocking first, until the backlog is empty or you stop it" \
        || say "mode: list -- ${wanted[*]}"
  [ -n "$deadline" ] && say "stopping at $(date -r "$deadline" '+%Y-%m-%d %H:%M')"
  [ -n "$max_prs" ] && say "stopping after $max_prs worktree(s) opened"

  local opened=0 reason="the queue is empty"
  local draining=false
  # An issue dropped from `wanted` did not land, and saying it did is a lie the
  # run's last line would tell every time the fleet declines one.
  local declined=false
  while true; do
    # A stop means "launch nothing more", not "abandon what is running". The
    # dispatcher is what reaps a worktree once its PR merges, so killing it here
    # would strand every in-flight stack under `restart: unless-stopped`. It
    # keeps reaping and exits when nothing it owns is left.
    if stopped && ! $draining; then
      draining=true
      say "stopped -- launching nothing more, still reaping what is in flight"
    fi
    if [ -n "$deadline" ] && [ "$(date +%s)" -ge "$deadline" ] && ! $draining; then
      draining=true
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

    while ! $draining && [ "$live" -lt "$MAX_WORKTREES" ]; do
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
    elif $auto && ! $draining; then
      # Counted from the lists already in hand rather than by asking `in_flight`
      # per issue: that made two API calls each, and a 200-issue backlog on a
      # 60-second poll is how you meet gh's secondary rate limit.
      queued="$(count_startable)"
    fi
    if [ "$queued" -eq 0 ] && [ "${owned:-0}" -eq 0 ]; then
      if $draining; then
        reason="${reason:-you stopped it}; everything in flight has landed"
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
