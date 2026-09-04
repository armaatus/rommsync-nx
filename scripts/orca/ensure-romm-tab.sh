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

# `kill -0` alone would accept any process that inherited a dead follower's pid,
# and the whole point here is to not report a tab that is not there.
follower_is_running() {
  local pid
  pid="$(cat "$PIDFILE" 2>/dev/null)" || return 1
  [ -n "$pid" ] || return 1
  kill -0 "$pid" 2>/dev/null || return 1
  ps -o command= -p "$pid" 2>/dev/null | grep -q 'romm-logs'
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
if orca terminal create --worktree "path:$REPO_ROOT" --title romm \
     --command "$FOLLOWER" >/dev/null 2>&1; then
  echo "    created"
else
  echo "    could not create it -- is the Orca runtime reachable?"
  echo "$HINT"
fi

exit 0
