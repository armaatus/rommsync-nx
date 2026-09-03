#!/usr/bin/env bash
# Covers scripts/orca/romm-logs.sh -- the `romm` default tab in orca.yaml.
#
# The tab used to run `compose.sh logs -f` directly. Orca starts the default tabs
# in parallel with setup.sh, which brings the stack up on its last step, and
# `docker compose logs -f` against a project with no containers exits 0
# immediately -- so the tab was reliably dead by the time RomM started.
#
#   test_romm_tab.sh wait     no stack exists -> keep waiting, do not exit.
#                             This is the regression: the old command exits at
#                             once here, so this phase fails without the fix.
#   test_romm_tab.sh follow   stack is up -> still attaches and streams, so the
#                             waiting did not cost the tab its actual job.
#                             Skips with 77 when the rig is down, like rig.smoke.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE="$REPO_ROOT/scripts/orca/compose.sh"
SCRIPT="$REPO_ROOT/scripts/orca/romm-logs.sh"
SKIP=77

fail() { echo "FAIL: $*" >&2; exit 1; }

OUT=""
TMPDIR_FIXTURE=""
cleanup() { rm -f "$OUT"; [ -n "$TMPDIR_FIXTURE" ] && rm -rf "$TMPDIR_FIXTURE"; }
trap cleanup EXIT

# Run romm-logs.sh from $1 in the background for $2 seconds, leaving its output
# in $OUT. Returns 0 if it was still running at the end, 1 if it exited early.
survived_for() {
  local root="$1" seconds="$2" pid
  OUT="$(mktemp)"
  ROMM_LOGS_POLL_SECONDS=1 bash "$root/scripts/orca/romm-logs.sh" >"$OUT" 2>&1 &
  pid=$!
  sleep "$seconds"
  local alive=1
  kill -0 "$pid" 2>/dev/null && alive=0
  # The script's own `sleep` is a child, so kill the group's stragglers too.
  pkill -P "$pid" 2>/dev/null
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  return $alive
}

case "${1:-}" in
  wait)
    # A worktree root whose compose project has no containers -- exactly the
    # state Orca opens the tab in, before setup.sh reaches `compose up -d`.
    tmp="$(mktemp -d)"
    TMPDIR_FIXTURE="$tmp"
    mkdir -p "$tmp/scripts/orca" "$tmp/server/testing"
    cp "$REPO_ROOT"/scripts/orca/{romm-logs.sh,compose.sh,env.sh} "$tmp/scripts/orca/"
    cp "$REPO_ROOT/server/testing/docker-compose.yml" "$tmp/server/testing/"
    cat >"$tmp/.env" <<ENV
COMPOSE_PROJECT_NAME=rmx-test-no-stack-$$
ROMM_PORT=21999
PROXY_PORT=23999
ENV

    survived_for "$tmp" 6 \
      || fail "the tab exited while the stack was still coming up (this is the bug)"
    grep -q "waiting for RomM" "$OUT" \
      || fail "no indication the tab is waiting; got: $(tail -5 "$OUT")"
    echo "PASS: tab waits instead of exiting when the stack is not up yet"
    ;;

  follow)
    services="$("$COMPOSE" config --services 2>/dev/null | grep -c .)"
    containers="$("$COMPOSE" ps -q 2>/dev/null | grep -c .)"
    if [ "${services:-0}" -eq 0 ] || [ "${containers:-0}" -lt "${services:-1}" ]; then
      echo "SKIP: RomM stack is not up ($containers/$services containers)"
      exit $SKIP
    fi

    survived_for "$REPO_ROOT" 12 || fail "the tab exited while the stack was up"
    # `logs --tail` on a live stack always has something to show.
    [ "$(grep -c . "$OUT")" -gt 1 ] || fail "attached but streamed nothing"
    echo "PASS: tab attaches and streams while the stack is up"
    ;;

  *)
    echo "usage: $0 wait|follow" >&2
    exit 2
    ;;
esac
