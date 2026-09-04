#!/usr/bin/env bash
# Send the issue prompt Orca leaves sitting unsent in a new worktree's agent tab.
#
# Creating a worktree from a linked issue puts the resolved spec -- what
# issue-command.sh produced -- into the agent's composer as a DRAFT rather than
# submitting it. Orca does this deliberately for issue-linked workspaces, and it
# is not configurable from orca.yaml: `promptDelivery` is chosen in the app, and
# a linked work item always picks `draft`. The visible result is a fresh
# worktree, fully provisioned, whose agent is sitting there having read nothing,
# waiting for someone to walk over and press Return.
#
# So this presses it. It is deliberately not a blind keystroke: `orca terminal
# read --json` reports the composer's unsent text as `draft`, so there is
# something specific to look for, and nothing is sent unless it is there. A
# worktree created without an issue has no draft, and this exits having done
# nothing.
#
#   ./scripts/orca/agent-autostart.sh          # check once, submit if drafted
#   ./scripts/orca/agent-autostart.sh --watch  # keep checking until it appears
#
# setup.sh starts the watching form, because the order forces it: orca.yaml's
# `setupAgentStartupPolicy: wait-for-setup` holds the agent's tab until setup
# returns, so the draft does not exist yet while setup is running.
#
# Set ROMMSYNC_AGENT_AUTOSTART=0 to keep the manual Return.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

WATCH=false
case "${1:-}" in
  --watch) WATCH=true ;;
  ""|--once) ;;
  *) echo "usage: $0 [--watch]" >&2; exit 2 ;;
esac

CLI_SECONDS="${AGENT_AUTOSTART_CLI_SECONDS:-20}"
POLL_SECONDS="${AGENT_AUTOSTART_POLL_SECONDS:-3}"
# Long enough for an agent to finish starting on a cold machine, short enough
# that a worktree created without an issue is not watched forever.
DEADLINE_SECONDS="${AGENT_AUTOSTART_DEADLINE_SECONDS:-300}"
PIDFILE="${AGENT_AUTOSTART_PIDFILE:-$REPO_ROOT/.orca/agent-autostart.pid}"

CLI_OUT="$(mktemp)"
trap 'rm -f "$CLI_OUT"; [ "$(cat "$PIDFILE" 2>/dev/null)" = "$$" ] && rm -f "$PIDFILE"' EXIT

if [ "${ROMMSYNC_AGENT_AUTOSTART:-1}" = "0" ]; then
  echo "==> agent autostart disabled (ROMMSYNC_AGENT_AUTOSTART=0)"
  exit 0
fi
command -v orca >/dev/null 2>&1 || { echo "==> no orca CLI; nothing to start"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "==> no python3; cannot read the agent's draft"; exit 0; }

# This worktree's agent terminal. `orca terminal list` reports every terminal on
# the machine, so the worktree path is what keeps three parallel worktrees from
# submitting into each other's agents; `agentIdentity` is what separates the
# agent from the shell and log tabs beside it.
AGENT_TERMINAL_PY='
import json, sys
root = sys.argv[1]
try:
    terminals = json.load(sys.stdin)["result"]["terminals"]
except Exception:
    sys.exit(1)
for t in terminals:
    if t.get("worktreePath") == root and t.get("agentIdentity") and not t.get("orphaned"):
        print(t["handle"])
        sys.exit(0)
sys.exit(1)
'

# The composer text the agent has NOT sent. Absent when there is nothing drafted,
# which is the normal case for a worktree created without an issue.
DRAFT_PY='
import json, sys
try:
    draft = json.load(sys.stdin)["result"]["terminal"].get("draft")
except Exception:
    sys.exit(1)
if not draft or not str(draft).strip():
    sys.exit(1)
# One line, so a multi-line spec stays comparable between polls without the
# shell having to hold it.
print(len(str(draft)), str(draft).strip().replace("\n", " ")[:60])
'

agent_terminal() {
  orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" orca terminal list --json || return 1
  python3 -c "$AGENT_TERMINAL_PY" "$REPO_ROOT" <"$CLI_OUT"
}

draft_of() {
  orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" \
    orca terminal read --terminal "$1" --screen --limit 1 --json || return 1
  python3 -c "$DRAFT_PY" <"$CLI_OUT"
}

# Two identical readings before submitting. Orca pastes the draft into the
# composer, and a paste that is read half way through is a different string --
# submitting there would send the agent a truncated spec, which is worse than
# the Return this replaces.
last_draft=""
attempt() {
  local handle draft
  handle="$(agent_terminal)" || return 1
  [ -n "$handle" ] || return 1
  draft="$(draft_of "$handle")" || { last_draft=""; return 1; }
  if [ "$draft" != "$last_draft" ]; then
    last_draft="$draft"
    return 1
  fi
  echo "==> submitting the agent's drafted prompt ($draft)"
  orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" \
    orca terminal send --terminal "$handle" --enter --json || {
      echo "    the orca CLI would not send it; press Return in the agent tab"
      return 0
    }
  echo "    sent."
  return 0
}

if ! $WATCH; then
  # A single check has no second reading to compare against, so take two.
  attempt >/dev/null 2>&1
  attempt || { echo "==> the agent has nothing drafted; nothing to send"; exit 0; }
  exit 0
fi

# One watcher per worktree. setup.sh can run more than once over a worktree's
# life, and a second watcher would race the first into the same composer.
held="$(cat "$PIDFILE" 2>/dev/null)"
if [ -n "$held" ] && kill -0 "$held" 2>/dev/null; then
  echo "==> an autostart watcher is already running (pid $held)"
  exit 0
fi
mkdir -p "$(dirname "$PIDFILE")" 2>/dev/null
echo $$ >"$PIDFILE"

waited=0
while [ "$waited" -lt "$DEADLINE_SECONDS" ]; do
  attempt && exit 0
  sleep "$POLL_SECONDS"
  waited=$((waited + POLL_SECONDS))
done
echo "==> no drafted prompt appeared in ${DEADLINE_SECONDS}s; leaving the agent alone"
exit 0
