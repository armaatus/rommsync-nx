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
FOLLOWER_REL="./scripts/orca/romm-logs.sh"
HINT="    open a tab and run: ./scripts/orca/romm-logs.sh"
# How long to wait for a created tab to actually start following. Short in tests.
WAIT_SECONDS="${ROMM_TAB_WAIT_SECONDS:-10}"
CLI_SECONDS=20
CLI_OUT="$(mktemp)"
trap 'rm -f "$CLI_OUT"' EXIT

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
  "$@" >"$CLI_OUT" 2>/dev/null &
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

# Orca titles a tab after the command it was given, and re-derives that title from
# the running process afterwards, so a rename races it and does not reliably
# stick -- it lands sometimes and is overwritten other times. Try anyway, because
# when it wins the tab reads `romm`; the command below is what makes the tab
# recognisable when it loses.
name_tab() {
  local handle
  handle="$(sed -n 's/.*"handle"[[:space:]]*:[[:space:]]*"\(term_[^"]*\)".*/\1/p' \
              "$CLI_OUT" | head -1)"
  [ -n "$handle" ] || return 1
  run_with_deadline "$CLI_SECONDS" orca terminal rename --terminal "$handle" --title romm
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
# The command is relative, and identical to the one orca.yaml asks for. It runs
# in the worktree root, it becomes the tab's title when the rename loses its
# race -- `./scripts/orca/romm-logs.sh` reads as the romm tab, an absolute path
# does not -- and being a fixed literal it cannot be word-split by a worktree
# path containing a space.
# --json for the handle in the reply, which is the only way to rename the tab;
# --title is passed too in case a future Orca honours it on create.
if ! run_with_deadline "$CLI_SECONDS" \
       orca terminal create --json --worktree "path:$REPO_ROOT" --title romm \
       --command "$FOLLOWER_REL"; then
  echo "    the orca CLI did not create it -- is the runtime reachable?"
  echo "$HINT"
else
  # Before waiting on the follower: the tab exists either way, and a named tab is
  # worth having even if what is in it turns out to be wrong.
  name_tab >/dev/null 2>&1 || true
  if follower_appears; then
    echo "    created; the romm tab is following"
  else
    echo "    the CLI accepted the tab but nothing is following it"
    echo "$HINT"
  fi
fi

exit 0
