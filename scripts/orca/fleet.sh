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
# touch a worktree it did not create. And it removes one only when that
# worktree's PR is merged and nothing is unpushed.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

STATE_DIR="${ROMMSYNC_FLEET_DIR:-$HOME/.rommsync-fleet}"
STOP_FILE="$STATE_DIR/STOP"
OWNED_DIR="$STATE_DIR/worktrees"
STARTED_DIR="$STATE_DIR/started"
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

mkdir -p "$OWNED_DIR" "$STARTED_DIR"

say() { printf '%s  %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG"; }
die() { printf '%s\n' "$*" >&2; exit 1; }

# The two cases you would otherwise not learn about until morning: the fleet
# finishing, and an issue giving up. Everything else is on the Orca board.
notify() {
  command -v osascript >/dev/null 2>&1 || return 0
  osascript -e "display notification \"$(printf '%s' "$2" | sed 's/"/\\"/g')\" with title \"rommsync fleet\" subtitle \"$1\"" >/dev/null 2>&1 || true
}

stopped() { [ -e "$STOP_FILE" ]; }
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
card() {
  local path="$1"; shift
  orca_run_with_deadline 30 /dev/null "$ORCA_CLI" worktree set \
    --worktree "path:$path" "$@" --json >/dev/null 2>&1 || true
}

# --------------------------------------------------------------- the state ---
own()          { printf '%s\n' "$2" >"$OWNED_DIR/$1"; date +%s >"$STARTED_DIR/$1"; }
owned_path()   { cat "$OWNED_DIR/$1" 2>/dev/null; }
disown_issue() { rm -f "$OWNED_DIR/$1" "$STARTED_DIR/$1"; }

live_worktrees() {
  orca_json worktree list | python3 -c '
import json,sys
for w in json.load(sys.stdin)["result"]["worktrees"]:
    if not w.get("isMainWorktree") and not w.get("isArchived"):
        print(w.get("linkedIssue") or "-", w["path"], sep="\t")
'
}
live_count() { live_worktrees | grep -c . ; }

# --------------------------------------------------------------- the queue ---
# Every open issue, with how many other open issues are blocked BY it. That
# number is the ordering: the work that frees the most other work goes first.
# It reads the same `Blocked by #N` lines unblock.yml parses, so nothing new has
# to be maintained.
ready_issues() {
  GH_PAGER=cat gh issue list --state open --limit 200 \
    --json number,title,body,labels 2>/dev/null | python3 -c '
import json, re, sys
issues = json.load(sys.stdin)
blocks = {}
for i in issues:
    for m in re.finditer(r"Blocked by #(\d+)", i.get("body") or ""):
        blocks[int(m.group(1))] = blocks.get(int(m.group(1)), 0) + 1
ready = [i for i in issues
         if any(l["name"] == "ready" for l in i.get("labels", []))]
# Most-unblocking first, then oldest issue number: predictable inside a tie.
for i in sorted(ready, key=lambda i: (-blocks.get(i["number"], 0), i["number"])):
    labels = ",".join(l["name"] for l in i.get("labels", []))
    print(i["number"], blocks.get(i["number"], 0), labels, i["title"], sep="\t")
'
}

# `ready` overstates availability: the label stays until the PR merges, so an
# issue with a PR already open still carries it.
has_open_pr() {
  GH_PAGER=cat gh pr list --state open --json number,body --limit 100 2>/dev/null \
    | python3 -c "
import json,sys,re
for p in json.load(sys.stdin):
    if re.search(r'Closes #$1\b', p.get('body') or ''):
        print(p['number']); break
" | grep -q .
}

in_flight() {
  live_worktrees | cut -f1 | grep -qx "$1" && return 0
  has_open_pr "$1" && return 0
  return 1
}

is_foundation() { printf '%s' "$1" | tr ',' '\n' | grep -qx "$FOUNDATION_LABEL"; }

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
    orca_run_with_deadline 180 /dev/null "$ORCA_CLI" worktree rm \
      --worktree "path:$path" --run-hooks --json >/dev/null 2>&1 \
      || say "  could not remove it; sweep later with ./scripts/orca/reap.sh --yes"
    disown_issue "$num"
  done
}

# ------------------------------------------------------------- the time-box ---
# An agent that cannot get green will grind. On expiry it is interrupted, the
# issue gets a comment saying so, and the worktree is LEFT STANDING: a stuck task
# is exactly the one worth looking at, and its fixture and build state are the
# evidence. It keeps its slot, and the notification is how you find out.
enforce_timebox() {
  local f num path started now agent
  now="$(date +%s)"
  for f in "$STARTED_DIR"/*; do
    [ -e "$f" ] || continue
    num="$(basename "$f")"; started="$(cat "$f")"
    path="$(owned_path "$num")"
    [ -d "$path" ] || continue
    [ $((now - started)) -ge "$TIMEBOX_SECONDS" ] || continue
    # A PR being up means it got where it was going; the review loop has its own
    # cap and is not this timer's business.
    has_open_pr "$num" && { rm -f "$f"; continue; }

    say "#$num: $((TIMEBOX_SECONDS / 3600))h with no PR -- stopping it and leaving the worktree for you"
    agent="$(orca_json terminal list | python3 -c "
import json,sys
for t in json.load(sys.stdin)['result']['terminals']:
    if t.get('worktreePath') == '$path' and t.get('agentIdentity') and not t.get('orphaned'):
        print(t['handle']); break
")"
    [ -n "$agent" ] && orca_run_with_deadline 20 /dev/null "$ORCA_CLI" terminal send \
      --terminal "$agent" --interrupt --json >/dev/null 2>&1
    card "$path" --comment "#$num: timed out after $((TIMEBOX_SECONDS / 3600))h -- needs you"
    GH_PAGER=cat gh issue comment "$num" --body "The fleet stopped work on this after $((TIMEBOX_SECONDS / 3600)) hours with no pull request opened. Its worktree is left standing at \`$path\` so the build state and the RomM fixture are still there to look at." >/dev/null 2>&1 || true
    notify "#$num gave up" "$((TIMEBOX_SECONDS / 3600))h with no PR. Worktree left standing."
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
  echo "next up (ready, not in flight; 'unblocks' is how many issues it frees):"
  printf '  %-6s %-9s %s\n' "issue" "unblocks" "title"
  ready_issues | while IFS="$(printf '\t')" read -r num unblocks labels title; do
    in_flight "$num" && continue
    printf '  #%-5s %-9s %s\n' "$num" "$unblocks" "$title"
  done | head -12
}

cmd_stop() {
  mkdir -p "$STATE_DIR"
  date '+stopped at %Y-%m-%d %H:%M:%S' >"$STOP_FILE"
  echo "stop set: $STOP_FILE"
  echo "  no new worktrees, and no agent can push, open a PR or comment."
  if [ "${1:-}" = "--now" ]; then
    echo "  interrupting the agents..."
    orca_json terminal list | python3 -c '
import json,sys
for t in json.load(sys.stdin)["result"]["terminals"]:
    if t.get("agentIdentity") and not t.get("orphaned"):
        print(t["handle"])
' | while read -r handle; do
      orca_run_with_deadline 20 /dev/null "$ORCA_CLI" terminal send \
        --terminal "$handle" --interrupt --json >/dev/null 2>&1 \
        && echo "    interrupted $handle"
    done
  else
    echo "  running agents finish what they are on. Use --now to interrupt them."
  fi
  if [ -e "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    kill "$(cat "$PIDFILE")" 2>/dev/null && echo "  dispatcher stopped."
  fi
}

cmd_resume() {
  rm -f "$STOP_FILE"
  echo "stop cleared. Start again with: ./scripts/orca/fleet.sh run --auto"
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
  while true; do
    check_stop && { reason="you stopped it"; break; }
    if [ -n "$deadline" ] && [ "$(date +%s)" -ge "$deadline" ]; then
      reason="the deadline passed"; break
    fi

    reap_merged
    enforce_timebox

    local live; live="$(live_count)"
    while [ "$live" -lt "$MAX_WORKTREES" ]; do
      check_stop && { reason="you stopped it"; break 2; }
      [ -n "$max_prs" ] && [ "$opened" -ge "$max_prs" ] && { reason="it opened $opened worktree(s)"; break 2; }

      local picked="" title="" labels=""
      if [ "${#wanted[@]}" -gt 0 ]; then
        local remaining=()
        for n in "${wanted[@]}"; do
          if [ -z "$picked" ] && ! in_flight "$n"; then picked="$n"; else remaining+=("$n"); fi
        done
        wanted=("${remaining[@]+"${remaining[@]}"}")
        [ -n "$picked" ] || break
        title="$(GH_PAGER=cat gh issue view "$picked" --json title --jq .title 2>/dev/null)"
        labels="$(GH_PAGER=cat gh issue view "$picked" --json labels --jq '[.labels[].name]|join(",")' 2>/dev/null)"
      else
        $auto || break
        while IFS="$(printf '\t')" read -r n _unblocks l t; do
          in_flight "$n" && continue
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
      launch "$picked" "$title" && opened=$((opened + 1))
      live="$(live_count)"
    done

    if [ "${#wanted[@]}" -eq 0 ] && ! $auto; then
      say "every requested issue is in flight; the worktrees carry it from here"
      reason="its list is in flight"
      break
    fi
    sleep "$POLL_SECONDS"
  done

  say "fleet down: $reason"
  notify "fleet down" "$reason. $opened worktree(s) opened."
}

case "${1:-}" in
  run)    shift; cmd_run "$@" ;;
  status) cmd_status ;;
  stop)   shift; cmd_stop "${1:-}" ;;
  resume) cmd_resume ;;
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

Run it in an Orca terminal so it is as visible as the work it starts:
  orca terminal create --worktree active --title fleet --command "./scripts/orca/fleet.sh run --auto"
USAGE
    exit 2 ;;
esac
