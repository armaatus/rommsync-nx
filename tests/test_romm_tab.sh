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
#
# The rest cover scripts/orca/ensure-romm-tab.sh, which exists because Orca has
# shipped worktrees where `defaultTabs` created the tabs but dispatched none of
# their commands -- RomM serving, and nothing anywhere showing it.
#
#   test_romm_tab.sh pidfile        a follower publishes a live pidfile and takes
#                                   it away again when it stops. Everything below
#                                   reads that file to decide.
#   test_romm_tab.sh ensure_noop    a follower is already running -> create no
#                                   second tab. This is what keeps the fix from
#                                   piling up a duplicate tab per setup run.
#   test_romm_tab.sh ensure_creates no follower -> ask the orca CLI for the tab.
#                                   This is the regression: without the fix
#                                   nothing asks, and the tab never appears.
#
# The two `ensure_*` phases stub the `orca` CLI on PATH, so they assert what the
# script would do to a real workspace without touching one.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE="$REPO_ROOT/scripts/orca/compose.sh"
SCRIPT="$REPO_ROOT/scripts/orca/romm-logs.sh"
SKIP=77

fail() { echo "FAIL: $*" >&2; exit 1; }

OUT=""
TMPDIR_FIXTURE=""
FOLLOWER_PID=""
cleanup() {
  [ -n "$FOLLOWER_PID" ] && { pkill -P "$FOLLOWER_PID" 2>/dev/null; kill "$FOLLOWER_PID" 2>/dev/null; }
  rm -f "$OUT"
  [ -n "$TMPDIR_FIXTURE" ] && rm -rf "$TMPDIR_FIXTURE"
  return 0
}
trap cleanup EXIT

# A worktree root holding just enough of the repo to run the orca scripts, with a
# compose project that has no containers -- the state Orca opens the tab in.
make_fixture() {
  local tmp
  tmp="$(mktemp -d)"
  TMPDIR_FIXTURE="$tmp"
  mkdir -p "$tmp/scripts/orca" "$tmp/server/testing"
  cp "$REPO_ROOT"/scripts/orca/{romm-logs.sh,compose.sh,env.sh,ensure-romm-tab.sh} \
     "$tmp/scripts/orca/"
  cp "$REPO_ROOT/server/testing/docker-compose.yml" "$tmp/server/testing/"
  cat >"$tmp/.env" <<ENV
COMPOSE_PROJECT_NAME=rmx-test-no-stack-$$
ROMM_PORT=21999
PROXY_PORT=23999
ENV
  echo "$tmp"
}

# Start a follower against $1 and leave it running; its pidfile is $2.
start_follower() {
  OUT="$(mktemp)"
  ROMM_LOGS_POLL_SECONDS=1 ROMM_LOGS_PIDFILE="$2" \
    bash "$1/scripts/orca/romm-logs.sh" >"$OUT" 2>&1 &
  FOLLOWER_PID=$!
}

# An `orca` on PATH that records its arguments instead of touching a workspace.
stub_orca() {
  local dir="$1/stub-bin"
  mkdir -p "$dir"
  cat >"$dir/orca" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"$1/orca-calls.log"
STUB
  chmod +x "$dir/orca"
  : >"$1/orca-calls.log"
  echo "$dir"
}

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
    tmp="$(make_fixture)"

    # Without this the phase is vacuous: stack_is_up returns 1 on ANY failure --
    # including a fixture too incomplete to run compose at all -- and the script
    # then waits, which is exactly what the assertions below look for. Deleting
    # compose.sh from the fixture used to still report PASS.
    "$tmp/scripts/orca/compose.sh" config --services >/dev/null 2>&1 \
      || fail "fixture is broken: compose.sh cannot read the compose file, so waiting proves nothing"

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
    # Count only container output: the script's own `==>` status lines are
    # present either way, so counting every line would pass even for a tab that
    # showed no RomM logs at all. `logs --tail` on a live stack always has
    # something to show.
    streamed="$(grep -v '^==>' "$OUT" | grep -c .)"
    [ "${streamed:-0}" -gt 0 ] \
      || fail "attached but streamed no container output; got: $(tail -5 "$OUT")"
    echo "PASS: tab attaches and streams while the stack is up"
    ;;

  pidfile)
    # Everything below decides "is a tab already following?" from this file, so
    # if it is not published and withdrawn accurately the rest is guesswork.
    tmp="$(make_fixture)"
    pf="$tmp/romm-logs.pid"
    start_follower "$tmp" "$pf"

    for _ in $(seq 1 50); do [ -s "$pf" ] && break; sleep 0.1; done
    [ -s "$pf" ] || fail "follower published no pidfile at $pf"
    [ "$(cat "$pf")" = "$FOLLOWER_PID" ] \
      || fail "pidfile says $(cat "$pf"), follower is $FOLLOWER_PID"
    kill -0 "$(cat "$pf")" 2>/dev/null || fail "pidfile names a process that is not running"

    pkill -P "$FOLLOWER_PID" 2>/dev/null
    kill "$FOLLOWER_PID" 2>/dev/null
    wait "$FOLLOWER_PID" 2>/dev/null
    FOLLOWER_PID=""
    for _ in $(seq 1 50); do [ -e "$pf" ] || break; sleep 0.1; done
    [ -e "$pf" ] && fail "pidfile outlived the follower; setup would think a dead tab is live"
    echo "PASS: follower publishes a live pidfile and withdraws it on exit"
    ;;

  ensure_noop)
    # Orca honoured defaultTabs. Creating a second tab every setup run would be a
    # slow leak of duplicate tabs, so the fix has to recognise its own success.
    tmp="$(make_fixture)"
    pf="$tmp/romm-logs.pid"
    stub="$(stub_orca "$tmp")"
    PATH="$stub:$PATH" command -v orca >/dev/null \
      || fail "fixture is broken: the stub orca is not on PATH, so calling it could not be detected"

    start_follower "$tmp" "$pf"
    for _ in $(seq 1 50); do [ -s "$pf" ] && break; sleep 0.1; done
    [ -s "$pf" ] || fail "follower never started; this phase would pass for the wrong reason"

    out="$(PATH="$stub:$PATH" ROMM_LOGS_PIDFILE="$pf" \
           bash "$tmp/scripts/orca/ensure-romm-tab.sh" 2>&1)"
    [ -s "$tmp/orca-calls.log" ] \
      && fail "created a tab while one was already following: $(cat "$tmp/orca-calls.log")"
    grep -q "is following" <<<"$out" || fail "did not report the live tab; got: $out"
    echo "PASS: an already-following tab is left alone"
    ;;

  ensure_creates)
    # The regression. Orca created the tabs and dispatched no commands, so no
    # follower is running and nothing but this script will ever ask for one.
    tmp="$(make_fixture)"
    pf="$tmp/romm-logs.pid"
    stub="$(stub_orca "$tmp")"

    out="$(PATH="$stub:$PATH" ROMM_LOGS_PIDFILE="$pf" \
           bash "$tmp/scripts/orca/ensure-romm-tab.sh" 2>&1)"
    calls="$(cat "$tmp/orca-calls.log")"
    grep -q "terminal create" <<<"$calls" || fail "asked the CLI for no tab; got: ${calls:-<nothing>} / $out"
    grep -q -- "--title romm" <<<"$calls" || fail "tab would be untitled -- the state we cannot tell apart: $calls"
    grep -q -- "path:$tmp" <<<"$calls" || fail "tab would land in the wrong worktree: $calls"
    grep -q "romm-logs.sh" <<<"$calls" || fail "tab would run no follower: $calls"

    # A live pid that is not a follower: `kill -0` alone accepts it, and the tab
    # would then be silently skipped for whatever recycled that pid.
    : >"$tmp/orca-calls.log"
    echo $$ >"$pf"
    PATH="$stub:$PATH" ROMM_LOGS_PIDFILE="$pf" \
      bash "$tmp/scripts/orca/ensure-romm-tab.sh" >/dev/null 2>&1
    grep -q "terminal create" "$tmp/orca-calls.log" \
      || fail "a stale pidfile whose pid was reused suppressed the tab"
    echo "PASS: a missing follower is created, and a stale pidfile does not mask it"
    ;;

  *)
    echo "usage: $0 wait|follow" >&2
    exit 2
    ;;
esac
