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
# So this presses it. Everything below exists to make sure it presses Return on
# Orca's paste and on nothing else -- because the alternative failure, submitting
# a human's half-typed prompt, is much worse than the keypress being replaced:
#
#   * `--watch` refuses to run unless this worktree has a LINKED ISSUE. Without
#     one Orca drafts nothing, so any text in that composer was typed by a
#     person, and there is nothing here to do.
#   * The draft has to arrive with the agent, inside a short grace window. Orca
#     puts it there as part of launching the agent -- sometimes as a launch
#     argument, sometimes pasted once the agent is ready -- so the window cannot
#     be zero. Once it closes on an empty composer, anything appearing later is
#     someone typing, and is left alone.
#   * The same draft has to be read TWICE. A paste caught half way through is a
#     shorter string, and submitting there would send the agent a truncated
#     issue.
#
#   ./scripts/orca/agent-autostart.sh          # check once, submit if drafted
#   ./scripts/orca/agent-autostart.sh --watch  # keep checking until it appears
#
# The single-shot form is not gated on a linked issue: running it is itself the
# deliberate act, and "submit whatever is drafted" is then what was asked for.
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
# Long enough for an agent to finish starting on a cold machine. It does not
# need to be longer: what is being waited for is a paste Orca performs as part
# of that launch, not something that can turn up later.
DEADLINE_SECONDS="${AGENT_AUTOSTART_DEADLINE_SECONDS:-120}"
# How long after the agent terminal first appears the draft may still show up.
# It cannot be zero: Orca delivers the draft either as a launch argument, where
# it is there from the first frame, or by pasting it once the agent is ready,
# which is later than the watcher's first poll. It should not be long either --
# every second of it is a second in which a person typing into a brand-new agent
# tab, and pausing, would have it submitted for them.
GRACE_SECONDS="${AGENT_AUTOSTART_GRACE_SECONDS:-30}"
PIDFILE="${AGENT_AUTOSTART_PIDFILE:-$REPO_ROOT/.orca/agent-autostart.pid}"

CLI_OUT="$(mktemp)"
# TERM and INT as well as EXIT: archive.sh signals this watcher when the worktree
# is being removed, and bash runs no EXIT trap for an untrapped SIGTERM -- the
# pidfile would outlive the process and name a pid the system is free to reuse.
release() {
  rm -f "$CLI_OUT"
  [ "$(cat "$PIDFILE" 2>/dev/null)" = "$$" ] && rm -f "$PIDFILE"
  return 0
}
trap 'release' EXIT
trap 'release; exit 0' TERM INT

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

# The composer text the agent has NOT sent, as a single comparable line. Prints
# nothing when there is no draft, which is a normal answer rather than a failure.
DRAFT_PY='
import json, sys
try:
    draft = json.load(sys.stdin)["result"]["terminal"].get("draft")
except Exception:
    sys.exit(1)
if draft and str(draft).strip():
    print(len(str(draft)), str(draft).strip().replace("\n", " ")[:60])
'

# The issue this worktree was created from, if any. Orca records it on the
# worktree, which is the only durable statement that a draft is expected here.
LINKED_ISSUE_PY='
import json, sys
try:
    wt = json.load(sys.stdin)["result"]["worktree"]
except Exception:
    sys.exit(1)
for key in ("linkedIssue", "linkedLinearIssue", "linkedWorkItem"):
    if wt.get(key):
        print(wt[key] if isinstance(wt[key], (str, int)) else "linked")
        sys.exit(0)
sys.exit(1)
'

linked_issue() {
  orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" orca worktree current --json || return 1
  python3 -c "$LINKED_ISSUE_PY" <"$CLI_OUT"
}

agent_terminal() {
  orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" orca terminal list --json || return 1
  python3 -c "$AGENT_TERMINAL_PY" "$REPO_ROOT" <"$CLI_OUT"
}

# Non-zero only when the read itself failed. An empty answer means "no draft",
# which the caller has to be able to tell apart from "could not look".
draft_of() {
  orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" \
    orca terminal read --terminal "$1" --screen --limit 1 --json || return 1
  python3 -c "$DRAFT_PY" <"$CLI_OUT"
}

# 0 means stop -- either the prompt was sent or there is nothing here to send.
# 1 means the answer is not in yet: the agent has not started, a read failed, or
# the draft is still settling.
baselined=false
last_draft=""
empty_polls=0
grace_polls=0
attempt() {
  local handle draft
  handle="$(agent_terminal)" || return 1
  [ -n "$handle" ] || return 1
  draft="$(draft_of "$handle")" || return 1

  if ! $baselined; then
    if [ -z "$draft" ]; then
      # Still inside the grace window: the agent is up but Orca may not have
      # delivered the draft yet.
      if [ "$empty_polls" -lt "$grace_polls" ]; then
        empty_polls=$((empty_polls + 1))
        return 1
      fi
      echo "==> the agent came up with an empty composer; nothing drafted to send"
      return 0
    fi
    baselined=true
    last_draft="$draft"
    return 1
  fi

  if [ -z "$draft" ]; then
    echo "==> the drafted prompt was withdrawn; leaving the agent alone"
    return 0
  fi
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
  # grace_polls stays 0: running this by hand is itself the statement that a
  # draft is there now, so an empty composer is an answer rather than a wait.
  # Two readings, spaced: the point of comparing them is that a paste in flight
  # looks different from one that has landed, and back-to-back CLI calls are not
  # far enough apart for that to mean anything.
  if ! attempt; then
    sleep "$POLL_SECONDS"
    attempt || echo "==> the agent's draft never settled; nothing sent"
  fi
  exit 0
fi

# A linked issue is the only reason to expect a draft. Without one, whatever is
# in that composer was typed by a person and must not be submitted for them.
if ! issue="$(linked_issue)"; then
  echo "==> this worktree has no linked issue; nothing will be drafted, not watching"
  exit 0
fi

# One watcher per worktree. setup.sh can run more than once over a worktree's
# life, and a second watcher would race the first into the same composer. A live
# pid is not enough on its own: a watcher killed with -9 leaves its pid behind
# for whatever the system hands that number to next.
held="$(cat "$PIDFILE" 2>/dev/null)"
if [ -n "$held" ] && kill -0 "$held" 2>/dev/null \
   && ps -o command= -p "$held" 2>/dev/null | grep -q 'agent-autostart'; then
  echo "==> an autostart watcher is already running (pid $held)"
  exit 0
fi
mkdir -p "$(dirname "$PIDFILE")" 2>/dev/null
echo $$ >"$PIDFILE"

# Whole polls, at least one: the draft cannot be required before the agent has
# been looked at even once.
grace_polls=$(( (GRACE_SECONDS + POLL_SECONDS - 1) / POLL_SECONDS ))
[ "$grace_polls" -ge 1 ] || grace_polls=1

echo "==> watching for the prompt Orca drafted from issue $issue"
waited=0
while [ "$waited" -lt "$DEADLINE_SECONDS" ]; do
  attempt && exit 0
  sleep "$POLL_SECONDS"
  waited=$((waited + POLL_SECONDS))
done
echo "==> no drafted prompt appeared in ${DEADLINE_SECONDS}s; leaving the agent alone"
exit 0
