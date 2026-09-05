#!/usr/bin/env bash
# The fleet: turn a list of issues -- or the whole backlog -- into merged PRs.
#
# Everything below this line is deterministic shell. No model runs here. The
# dispatcher's whole job is to decide WHICH issue gets a worktree and WHEN, and
# that decision has to be readable, interruptible and cheap to run for hours.
# The thinking happens inside the worktrees it opens.
#
#   ./scripts/orca/fleet.sh run 11 12 13   # work exactly these, then stop
#   ./scripts/orca/fleet.sh run --auto     # keep picking `ready` issues, forever
#   ./scripts/orca/fleet.sh status         # what is running, what is next
#   ./scripts/orca/fleet.sh stop           # drain: no new work, let current finish
#   ./scripts/orca/fleet.sh stop --now     # ...and interrupt the agents too
#   ./scripts/orca/fleet.sh resume         # clear the stop and carry on
#
# ## Stopping
#
# The stop is a FILE, not a signal, and it lives outside every worktree
# ($HOME/.rommsync-fleet/STOP). That is deliberate: a signal only reaches a
# process that is still healthy, and the case you most need a stop in is the one
# where something is not. Everything checks the file -- this dispatcher before
# every action, `await-review.sh` between polls, and `.claude/hooks/guard.py`,
# which refuses to push, open a PR or comment while it exists. So a stopped fleet
# cannot produce outward effects even if an agent is mid-thought and never reads
# the news.
#
# ## What it will not do
#
# It does not merge, and it does not remove a worktree it did not create. Those
# are the two irreversible things in this loop and they stay with a person.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

STATE_DIR="${ROMMSYNC_FLEET_DIR:-$HOME/.rommsync-fleet}"
STOP_FILE="$STATE_DIR/STOP"
OWNED_DIR="$STATE_DIR/worktrees"
LOG="$STATE_DIR/fleet.log"
PIDFILE="$STATE_DIR/fleet.pid"

# At most three, because the ceiling is not machine capacity -- it is how many
# streams one person can review properly (CLAUDE.md, "Working in parallel").
MAX_WORKTREES="${ROMMSYNC_FLEET_MAX:-3}"
POLL_SECONDS="${ROMMSYNC_FLEET_POLL:-60}"
# An issue carrying this label defines an interface later issues include, so it
# merges before anything else starts -- the one serialisation worth paying for.
FOUNDATION_LABEL="${ROMMSYNC_FOUNDATION_LABEL:-foundation}"

mkdir -p "$OWNED_DIR"

say() { printf '%s  %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG"; }
die() { printf '%s\n' "$*" >&2; exit 1; }

stopped() { [ -e "$STOP_FILE" ]; }

# Every loop and every long wait passes through here, so a stop takes effect at
# the next decision point rather than at the end of whatever is running.
check_stop() {
  stopped || return 1
  say "stop file present ($STOP_FILE) -- not starting anything new"
  return 0
}

# ---------------------------------------------------------------- orca CLI ---
orca_cli_resolve || die "no orca CLI answers here; is the Orca app running?"

repo_id() {
  orca_json worktree list | python3 -c '
import json,sys
for w in json.load(sys.stdin)["result"]["worktrees"]:
    if w.get("isMainWorktree"):
        print(w["repoId"]); break
'
}

orca_json() {
  local out; out="$(mktemp)"
  orca_run_with_deadline 30 "$out" "$ORCA_CLI" "$@" --json
  local rc=$?
  cat "$out"; rm -f "$out"
  return $rc
}

# --------------------------------------------------------------- the state ---
# Worktrees this dispatcher created, as `<issue>` files holding their path. Only
# these are ever reaped: a worktree someone opened by hand is theirs.
own() { printf '%s\n' "$2" >"$OWNED_DIR/$1"; }
owned_path() { cat "$OWNED_DIR/$1" 2>/dev/null; }
disown_issue() { rm -f "$OWNED_DIR/$1"; }

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
# `ready` and open, minus anything already in flight. Ordered by milestone then
# number, which is the order the backlog is written in.
ready_issues() {
  GH_PAGER=cat gh issue list --state open --label ready --limit 100 \
    --json number,title,labels,milestone 2>/dev/null | python3 -c '
import json,sys
issues = json.load(sys.stdin)
def key(i):
    ms = (i.get("milestone") or {}).get("title") or "zzz"
    return (ms, i["number"])
for i in sorted(issues, key=key):
    labels = ",".join(l["name"] for l in i.get("labels", []))
    print(i["number"], labels, i["title"], sep="\t")
'
}

has_open_pr() {
  local n="$1"
  GH_PAGER=cat gh pr list --state open --search "linked:issue" --json number,body \
    --limit 100 2>/dev/null \
    | python3 -c "
import json,sys,re
for p in json.load(sys.stdin):
    if re.search(r'Closes #$n\b', p.get('body') or ''):
        print(p['number']); break
" | grep -q .
}

in_flight() {
  local n="$1"
  live_worktrees | cut -f1 | grep -qx "$n" && return 0
  has_open_pr "$n" && return 0
  return 1
}

is_foundation() {
  printf '%s' "$2" | tr ',' '\n' | grep -qx "$FOUNDATION_LABEL"
}

# --------------------------------------------------------------- the launch ---
slug() {
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]' \
    | tr -cs 'a-z0-9' '-' | sed 's/^-*//;s/-*$//' | cut -c1-48
}

launch() {
  local num="$1" title="$2"
  local name; name="$(slug "$num-$title")"

  say "opening a worktree for #$num -- $title"
  local out; out="$(mktemp)"
  orca_run_with_deadline 180 "$out" "$ORCA_CLI" worktree create \
    --repo "path:$REPO_ROOT" \
    --name "$name" \
    --issue "$num" \
    --no-parent \
    --agent claude \
    --prompt "$(agent_brief "$num")" \
    --json
  local rc=$?
  if [ "$rc" != 0 ]; then
    say "  could not create it (exit $rc):"
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
  [ -n "$path" ] && own "$num" "$path"
  say "  #$num is running in ${path:-a new worktree}"
}

# The opening prompt. It names the script rather than restating the workflow, so
# this file cannot drift from the brief the same way orca.yaml cannot.
agent_brief() {
  cat <<BRIEF
Run \`GH_PAGER=cat ./scripts/orca/issue-command.sh $1\` first and follow
everything it prints, including the review loop at the end. You were started by
the fleet dispatcher, so work autonomously to a PR that is waiting only on a
human to merge -- do not stop to ask for confirmation on anything CLAUDE.md
already decides.

If \`$STOP_FILE\` appears at any point, stop: finish the sentence you are on,
say where you got to, and do nothing further.
BRIEF
}

# ---------------------------------------------------------------- the reap ---
# A worktree whose PR is merged has done its job and is holding a slot. Only
# ones this dispatcher created are touched, and only when nothing is unpushed.
reap_merged() {
  local f num path branch merged unpushed
  for f in "$OWNED_DIR"/*; do
    [ -e "$f" ] || continue
    num="$(basename "$f")"
    path="$(cat "$f")"
    [ -d "$path" ] || { disown_issue "$num"; continue; }
    branch="$(git -C "$path" rev-parse --abbrev-ref HEAD 2>/dev/null)" || continue
    merged="$(GH_PAGER=cat gh pr list --head "$branch" --state merged \
                --json number --jq '.[0].number' 2>/dev/null)"
    [ -n "$merged" ] && [ "$merged" != "null" ] || continue
    unpushed="$(git -C "$path" log "@{u}..HEAD" --oneline 2>/dev/null | grep -c .)"
    if [ "${unpushed:-0}" != 0 ]; then
      say "#$num: PR #$merged merged but $unpushed commit(s) are unpushed -- leaving it alone"
      continue
    fi
    say "#$num: PR #$merged is merged; removing its worktree to free a slot"
    orca_run_with_deadline 120 /dev/null "$ORCA_CLI" worktree rm \
      --worktree "path:$path" --run-hooks --json >/dev/null 2>&1 \
      || say "  could not remove it; sweep later with ./scripts/orca/reap.sh --yes"
    disown_issue "$num"
  done
}

# --------------------------------------------------------------- commands ---
cmd_status() {
  echo "fleet state: $STATE_DIR"
  if stopped; then
    echo "STOPPED  ($STOP_FILE -- clear it with: ./scripts/orca/fleet.sh resume)"
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
  echo "next up (ready, not in flight):"
  ready_issues | while IFS="$(printf '\t')" read -r num labels title; do
    in_flight "$num" && continue
    printf '  #%-5s %s\n' "$num" "$title"
  done | head -10
}

cmd_stop() {
  mkdir -p "$STATE_DIR"
  date '+stopped at %Y-%m-%d %H:%M:%S' >"$STOP_FILE"
  echo "stop set: $STOP_FILE"
  echo "  no new worktrees will open, and no agent can push, open a PR or comment."
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
    echo "  running agents will finish what they are on. Use --now to interrupt them."
  fi
  if [ -e "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    kill "$(cat "$PIDFILE")" 2>/dev/null && echo "  dispatcher stopped."
  fi
}

cmd_resume() {
  rm -f "$STOP_FILE"
  echo "stop cleared. Start work again with: ./scripts/orca/fleet.sh run --auto"
}

cmd_run() {
  local auto=false; local -a wanted=()
  for arg in "$@"; do
    case "$arg" in
      --auto) auto=true ;;
      [0-9]*) wanted+=("$arg") ;;
      *) die "usage: fleet.sh run [--auto] [ISSUE...]" ;;
    esac
  done
  $auto || [ "${#wanted[@]}" -gt 0 ] || die "give issue numbers, or --auto"

  if stopped; then
    die "the fleet is stopped ($STOP_FILE). Clear it with: ./scripts/orca/fleet.sh resume"
  fi

  echo $$ >"$PIDFILE"
  trap 'rm -f "$PIDFILE"' EXIT
  say "fleet up: max $MAX_WORKTREES worktrees, polling every ${POLL_SECONDS}s"
  $auto && say "mode: auto -- taking ready issues until the backlog is empty or you stop it" \
        || say "mode: list -- ${wanted[*]}"

  while true; do
    check_stop && break
    reap_merged

    local live; live="$(live_count)"
    while [ "$live" -lt "$MAX_WORKTREES" ]; do
      check_stop && break 2
      local picked=""

      if [ "${#wanted[@]}" -gt 0 ]; then
        local remaining=()
        for n in "${wanted[@]}"; do
          if [ -z "$picked" ] && ! in_flight "$n"; then picked="$n"; else remaining+=("$n"); fi
        done
        wanted=("${remaining[@]}")
        [ -n "$picked" ] || break
        local title labels
        IFS="$(printf '\t')" read -r _ labels title < <(ready_issues | grep -P "^$picked\t" || true)
        [ -n "$title" ] || title="$(GH_PAGER=cat gh issue view "$picked" --json title --jq .title 2>/dev/null)"
      else
        $auto || break
        while IFS="$(printf '\t')" read -r n labels t; do
          in_flight "$n" && continue
          # A foundation issue lands alone: if anything else is running, wait
          # for the tree to drain rather than fanning out around it.
          if is_foundation "$n" "$labels" && [ "$live" -gt 0 ]; then
            say "#$n is a foundation issue; waiting for the other $live worktree(s) to land"
            break
          fi
          picked="$n"; title="$t"; labels="$labels"
          break
        done < <(ready_issues)
      fi

      [ -n "$picked" ] || break
      launch "$picked" "$title" || true
      live="$(live_count)"
    done

    if [ "${#wanted[@]}" -eq 0 ] && ! $auto; then
      say "every requested issue is in flight; the worktrees carry it from here"
      break
    fi

    sleep "$POLL_SECONDS"
  done
  say "fleet down"
}

case "${1:-}" in
  run)    shift; cmd_run "$@" ;;
  status) cmd_status ;;
  stop)   shift; cmd_stop "${1:-}" ;;
  resume) cmd_resume ;;
  *)
    cat >&2 <<USAGE
usage: fleet.sh <command>

  run 11 12 13     work exactly these issues, then hand off to the worktrees
  run --auto       keep taking \`ready\` issues until the backlog is empty
  status           what is running, and what is next
  stop [--now]     drain (or interrupt the agents too)
  resume           clear the stop
USAGE
    exit 2 ;;
esac
