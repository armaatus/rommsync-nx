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
FOLLOWER_PIDS=""
stop_follower() {
  local pid="$1"
  pkill -P "$pid" 2>/dev/null
  kill "$pid" 2>/dev/null
  # Waited for, not just signalled: an unreaped follower keeps a cwd inside the
  # fixture that cleanup is about to delete.
  wait "$pid" 2>/dev/null
}
cleanup() {
  local pid
  rm -f "${OUT}.pid"
  for pid in $FOLLOWER_PIDS; do stop_follower "$pid"; done
  rm -f "$OUT"
  [ -n "$TMPDIR_FIXTURE" ] && rm -rf "$TMPDIR_FIXTURE"
  return 0
}
trap cleanup EXIT

# A worktree root holding just enough of the repo to run the orca scripts, with a
# compose project that has no containers -- the state Orca opens the tab in.
#
# Sets $TMPDIR_FIXTURE rather than echoing the path: called as `$(make_fixture)`
# the assignment would happen in a subshell, cleanup would never see it, and
# every run would leak a fixture tree.
make_fixture() {
  TMPDIR_FIXTURE="$(mktemp -d)"
  mkdir -p "$TMPDIR_FIXTURE/scripts/orca" "$TMPDIR_FIXTURE/server/testing"
  # lib.sh included: ensure-romm-tab.sh sources it for the shared CLI watchdog.
  cp "$REPO_ROOT"/scripts/orca/{romm-logs.sh,compose.sh,env.sh,ensure-romm-tab.sh,lib.sh} \
     "$TMPDIR_FIXTURE/scripts/orca/"
  cp "$REPO_ROOT/server/testing/docker-compose.yml" "$TMPDIR_FIXTURE/server/testing/"
  cat >"$TMPDIR_FIXTURE/.env" <<ENV
COMPOSE_PROJECT_NAME=rmx-test-no-stack-$$
ROMM_PORT=21999
PROXY_PORT=23999
ENV
}

# Start a follower against $1 and leave it running; its pidfile is $2. The pid
# lands in $STARTED_FOLLOWER, and every follower is reaped by cleanup.
STARTED_FOLLOWER=""
start_follower() {
  # One log for every follower a phase starts: a fresh mktemp per call would
  # orphan the previous one, since cleanup only ever removes the last $OUT.
  [ -n "$OUT" ] || OUT="$(mktemp)"
  ROMM_LOGS_POLL_SECONDS=1 ROMM_LOGS_PIDFILE="$2" \
    bash "$1/scripts/orca/romm-logs.sh" >>"$OUT" 2>&1 &
  STARTED_FOLLOWER=$!
  FOLLOWER_PIDS="$FOLLOWER_PIDS $STARTED_FOLLOWER"
}

# Wait up to 5s for $1 to hold a pid; 0 if it does.
await_pidfile() {
  local _
  for _ in $(seq 1 50); do [ -s "$1" ] && return 0; sleep 0.1; done
  return 1
}

# An `orca` on PATH that records its arguments instead of touching a workspace.
stub_orca() {
  local dir="$1/stub-bin"
  mkdir -p "$dir"
  cat >"$dir/orca" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"$1/orca-calls.log"
# A create answers with the runtime's JSON, because the handle in it is what the
# tab has to be renamed by. No backticks in here: the heredoc is unquoted, so
# they would run as a command substitution while the stub is being written.
if [ "\${1:-}" = "terminal" ] && [ "\${2:-}" = "create" ]; then
  printf '{\n  "ok": true,\n  "result": {\n    "handle": "term_stub0001"\n  }\n}\n'
fi
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
  # Its own pidfile: the `follow` phase runs against the REAL worktree, and
  # without this it steals the live romm tab's pidfile and deletes it on the way
  # out -- leaving that tab streaming but invisible to ensure-romm-tab.sh.
  ROMM_LOGS_POLL_SECONDS=1 ROMM_LOGS_PIDFILE="${OUT}.pid" \
    bash "$root/scripts/orca/romm-logs.sh" >"$OUT" 2>&1 &
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
    make_fixture; tmp="$TMPDIR_FIXTURE"

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
    make_fixture; tmp="$TMPDIR_FIXTURE"
    pf="$tmp/romm-logs.pid"
    start_follower "$tmp" "$pf"; first="$STARTED_FOLLOWER"

    await_pidfile "$pf" || fail "follower published no pidfile at $pf"
    [ "$(cat "$pf")" = "$first" ] || fail "pidfile says $(cat "$pf"), follower is $first"
    kill -0 "$(cat "$pf")" 2>/dev/null || fail "pidfile names a process that is not running"

    # A second follower -- exactly what the hint tells an operator to start by
    # hand -- takes the pidfile with it when it exits first. Without the reclaim
    # in romm-logs.sh the survivor is then invisible, and the next setup run
    # opens a duplicate tab alongside a perfectly good one.
    start_follower "$tmp" "$pf"; second="$STARTED_FOLLOWER"
    await_pidfile "$pf" || fail "second follower published no pidfile"
    stop_follower "$second"
    reclaimed=""
    for _ in $(seq 1 50); do
      [ "$(cat "$pf" 2>/dev/null)" = "$first" ] && { reclaimed=yes; break; }
      sleep 0.1
    done
    [ -n "$reclaimed" ] \
      || fail "a departing follower stranded the survivor; pidfile is now: $(cat "$pf" 2>/dev/null || echo gone)"

    stop_follower "$first"
    for _ in $(seq 1 50); do [ -e "$pf" ] || break; sleep 0.1; done
    [ -e "$pf" ] && fail "pidfile outlived the follower; setup would think a dead tab is live"
    echo "PASS: pidfile tracks the live follower, through a second one coming and going"
    ;;

  ensure_noop)
    # Orca honoured defaultTabs. Creating a second tab every setup run would be a
    # slow leak of duplicate tabs, so the fix has to recognise its own success.
    make_fixture; tmp="$TMPDIR_FIXTURE"
    pf="$tmp/romm-logs.pid"
    stub="$(stub_orca "$tmp")"
    PATH="$stub:$PATH" command -v orca >/dev/null \
      || fail "fixture is broken: the stub orca is not on PATH, so calling it could not be detected"

    start_follower "$tmp" "$pf"
    await_pidfile "$pf" || fail "follower never started; this phase would pass for the wrong reason"

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
    make_fixture; tmp="$TMPDIR_FIXTURE"
    pf="$tmp/romm-logs.pid"
    stub="$(stub_orca "$tmp")"

    out="$(PATH="$stub:$PATH" ROMM_LOGS_PIDFILE="$pf" ROMM_TAB_WAIT_SECONDS=1 \
           bash "$tmp/scripts/orca/ensure-romm-tab.sh" 2>&1)"
    calls="$(cat "$tmp/orca-calls.log")"
    grep -q "terminal create" <<<"$calls" || fail "asked the CLI for no tab; got: ${calls:-<nothing>} / $out"
    grep -q -- "--title romm" <<<"$calls" || fail "tab would be untitled -- the state we cannot tell apart: $calls"
    # Without --json the reply carries no handle, the rename below cannot happen,
    # and the tab keeps the absolute-path name Orca gives it.
    grep -q -- "terminal create --json" <<<"$calls" || fail "create asked for no handle to rename by: $calls"
    grep -q -- "path:$tmp" <<<"$calls" || fail "tab would land in the wrong worktree: $calls"
    grep -q -- "--command ./scripts/orca/romm-logs.sh" <<<"$calls" \
      || fail "tab would run no follower, or by a path that becomes its title: $calls"
    # The stub exits 0 without starting anything -- which is also how Orca
    # behaves in the bug being worked around, and how it behaves when it falls
    # back to a background handle the UI never adopts. Reporting "created" there
    # rebuilds the silent state this script exists to remove.
    grep -q "nothing is following it" <<<"$out" \
      || fail "claimed a tab that is following nothing; got: $out"
    # Orca names a commanded tab after its command and ignores --title on create,
    # so without the follow-up rename the tab arrives as an absolute path -- which
    # is the "I cannot see romm" complaint this all started from.
    grep -q -- "terminal rename --terminal term_stub0001 --title romm" <<<"$calls" \
      || fail "created tab was never renamed to romm: $calls"

    # A live pid that is not a follower: `kill -0` alone accepts it, and the tab
    # would then be silently skipped for whatever recycled that pid.
    : >"$tmp/orca-calls.log"
    echo $$ >"$pf"
    PATH="$stub:$PATH" ROMM_LOGS_PIDFILE="$pf" ROMM_TAB_WAIT_SECONDS=1 \
      bash "$tmp/scripts/orca/ensure-romm-tab.sh" >/dev/null 2>&1
    grep -q "terminal create" "$tmp/orca-calls.log" \
      || fail "a stale pidfile whose pid was reused suppressed the tab"
    echo "PASS: a missing follower is created, and a stale pidfile does not mask it"
    ;;

  pidfile_streaming)
    # The `pidfile` phase covers a follower that is WAITING. Following is the
    # steady state, and there the script is blocked in `compose logs -f` -- so a
    # reclaim that only runs between loop iterations never runs at all.
    make_fixture; tmp="$TMPDIR_FIXTURE"
    pf="$tmp/romm-logs.pid"
    # A stack that is up and a log stream that never ends, without docker.
    cat >"$tmp/scripts/orca/compose.sh" <<'STUBC'
#!/usr/bin/env bash
case "${1:-}" in
  ps)   echo stub-container ;;
  logs) exec sleep 3600 ;;
  *)    exit 0 ;;
esac
STUBC
    chmod +x "$tmp/scripts/orca/compose.sh"

    start_follower "$tmp" "$pf"; first="$STARTED_FOLLOWER"
    await_pidfile "$pf" || fail "streaming follower published no pidfile"
    streaming=""
    for _ in $(seq 1 50); do
      grep -q "following" "$OUT" && { streaming=yes; break; }
      sleep 0.1
    done
    [ -n "$streaming" ] \
      || fail "fixture is broken: follower never reached the streaming branch; got: $(cat "$OUT")"

    # A second follower comes and goes, taking the pidfile with it.
    start_follower "$tmp" "$pf"; second="$STARTED_FOLLOWER"
    await_pidfile "$pf" || fail "second follower published no pidfile"
    stop_follower "$second"
    reclaimed=""
    for _ in $(seq 1 60); do
      [ "$(cat "$pf" 2>/dev/null)" = "$first" ] && { reclaimed=yes; break; }
      sleep 0.2
    done
    [ -n "$reclaimed" ] \
      || fail "a streaming follower never reclaimed its pidfile; it is now: $(cat "$pf" 2>/dev/null || echo gone)"

    # And a pid left behind by a follower that was killed outright -- no trap, so
    # the file survives holding a dead pid. Present is not the same as live.
    (exit 0) & dead=$!; wait "$dead" 2>/dev/null
    echo "$dead" >"$pf"
    reclaimed=""
    for _ in $(seq 1 60); do
      [ "$(cat "$pf" 2>/dev/null)" = "$first" ] && { reclaimed=yes; break; }
      sleep 0.2
    done
    [ -n "$reclaimed" ] \
      || fail "a dead pid in the pidfile was treated as claimed forever"
    echo "PASS: a streaming follower reclaims its pidfile from a departed and from a dead one"
    ;;

  *)
    echo "usage: $0 wait|follow|pidfile|pidfile_streaming|ensure_noop|ensure_creates" >&2
    exit 2
    ;;
esac
