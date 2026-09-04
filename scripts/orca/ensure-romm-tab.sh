#!/usr/bin/env bash
# Guarantee this worktree ends up with a tab following RomM -- setup.sh's last step.
#
# orca.yaml already asks for that tab declaratively (`defaultTabs`), and when Orca
# honours the request this script finds a follower running and does nothing. Orca
# does not always honour it: a worktree created on 2026-09-04 came up with the
# right NUMBER of tabs, every one of them untitled and running no command at all,
# while setup.sh had completed and RomM was serving. A live fixture with no window
# onto it looks exactly like a fixture that failed to start, which is the whole
# failure `romm-logs.sh` was written to prevent.
#
# So `defaultTabs` is the request and this is the check that it was granted.
#
# This never fails setup. A missing log tab is a cosmetic problem; a worktree that
# refuses to provision because of one is not.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

PIDFILE="${ROMM_LOGS_PIDFILE:-$REPO_ROOT/.orca/romm-logs.pid}"
FOLLOWER="$REPO_ROOT/scripts/orca/romm-logs.sh"
HINT="    open a tab and run: ./scripts/orca/romm-logs.sh"
# How long to wait for a created tab to actually start following. Short in tests.
WAIT_SECONDS="${ROMM_TAB_WAIT_SECONDS:-10}"
CLI_SECONDS=20

# `kill -0` alone would accept any process that inherited a dead follower's pid,
# and the whole point here is to not report a tab that is not there.
follower_is_running() {
  local pid
  pid="$(cat "$PIDFILE" 2>/dev/null)" || return 1
  [ -n "$pid" ] || return 1
  kill -0 "$pid" 2>/dev/null || return 1
  ps -o command= -p "$pid" 2>/dev/null | grep -q 'romm-logs'
}

# `orca` talks to a runtime that can accept the connection and then never answer.
# setup.sh runs under `set -e` and orca.yaml holds the agent's tab until it
# returns, so hanging here costs the whole worktree -- strictly worse than the
# missing tab this is guarding against. macOS has no `timeout`, hence the manual
# watchdog.
run_with_deadline() {
  local seconds="$1"; shift
  "$@" >/dev/null 2>&1 &
  local cli=$! waited=0
  while kill -0 "$cli" 2>/dev/null; do
    if [ "$waited" -ge "$seconds" ]; then
      kill "$cli" 2>/dev/null
      wait "$cli" 2>/dev/null
      return 124
    fi
    sleep 1
    waited=$((waited + 1))
  done
  wait "$cli"
}

# The CLI exiting 0 is not the outcome we want -- a tab that follows nothing is
# the very state being repaired, and `orca terminal create` documents a fallback
# to a background handle the UI never adopts. The follower publishes its pidfile
# as its first action, so wait for that instead of trusting the exit code.
follower_appears() {
  local waited=0
  while [ "$waited" -lt "$WAIT_SECONDS" ]; do
    follower_is_running && return 0
    sleep 1
    waited=$((waited + 1))
  done
  return 1
}

if follower_is_running; then
  echo "==> romm tab is following (pid $(cat "$PIDFILE"))"
  exit 0
fi

if ! command -v orca >/dev/null 2>&1; then
  # A plain clone or CI, where there are no tabs to create in the first place.
  echo "==> no romm tab and no orca CLI to create one"
  echo "$HINT"
  exit 0
fi

echo "==> orca.yaml's romm tab never started; creating it"
# --command is shell text Orca runs in the tab, not an argv element, so an
# unquoted worktree path containing a space would be split into two words and
# leave behind exactly the dead tab this is here to prevent. Single quotes cover
# every path git will hand us bar one containing a quote of its own.
if ! run_with_deadline "$CLI_SECONDS" \
       orca terminal create --worktree "path:$REPO_ROOT" --title romm \
       --command "'$FOLLOWER'"; then
  echo "    the orca CLI did not create it -- is the runtime reachable?"
  echo "$HINT"
elif follower_appears; then
  echo "    created; following in a tab titled romm"
else
  echo "    the CLI accepted the tab but nothing is following it"
  echo "$HINT"
fi

exit 0
