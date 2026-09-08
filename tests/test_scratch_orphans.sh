#!/usr/bin/env bash
# What `test_scratch_concurrent.sh` leaves behind when CTest ends it on TIMEOUT
# (#169).
#
# That script starts two `ctest` invocations. Killing those two is not enough:
# **`ctest` does not forward the signal to the test it is running.** Measured
# both ways -- `kill -TERM` and `kill -KILL` on an inner `ctest` leave its
# `test_http_native` alive and reparented to `ppid=1`. Orphaned, it goes on
# reading, uploading and deleting on the shared RomM fixture alongside whatever
# rig test the outer run reaches next, which presents as *that* test failing with
# nothing pointing back here. `set -m` and a kill of the job's process group are
# the fix; this is what holds it.
#
# **Why the obvious version of this test does not work.** Polling until no inner
# process is left always succeeds, whatever the script does, because the scenario
# finishes on its own a few seconds later -- so the poll outlasts the leak and
# reports health. Two earlier attempts failed exactly there and were thrown away.
# The question is not "do they die eventually" but "are they still running just
# after the script was killed, when they should already be gone", so this samples
# ONCE, promptly, and asks whether anything was orphaned.
#
# The control phase is what keeps that sample honest: it first proves the
# scenario is still running at the sampling moment when nobody kills it. Without
# it, a scenario that got faster would make the kill phase pass for the wrong
# reason, which is the failure mode that wasted the two earlier attempts.
#
#   test_scratch_orphans.sh <ctest> <build-dir> <script-under-test> <regex>
#
# Skips with 77, like rig.smoke, when the inner tests skip for want of RomM:
# nothing is spawned then, so there is nothing to orphan.
set -uo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $(basename "$0") <ctest> <build-dir> <script> <regex>" >&2
  exit 2
fi
CTEST="$1"
BUILD_DIR="$2"
SCRIPT="$3"
PATTERN="$4"
SKIP=77

# The absolute path, so this can never match another worktree's binaries: three
# of them run at once by design.
INNER="$BUILD_DIR/tests/test_http_native"

# Long enough that a process still alive is one nothing is tearing down, and far
# short of how long the scenario itself runs -- the control phase checks that
# second half rather than trusting it.
SETTLE=0.6

log="$(mktemp)"
victim=""

# Whatever happens -- a failure, or this script itself being killed -- no inner
# binary of ours outlives it. A test that diagnoses orphans by leaving orphans
# would poison the run it exists to protect.
cleanup() {
  [[ -n "$victim" ]] && kill "$victim" 2>/dev/null
  pkill -f "$INNER" 2>/dev/null
  rm -f "$log"
}
trap cleanup EXIT

# `wc -l` rather than `pgrep -c`, which is not on every platform this runs on.
inner_count() { pgrep -f "$INNER" 2>/dev/null | wc -l | tr -d ' '; }

# Reparented to init: alive, and with nothing left that could clean it up. That
# is the leak, as distinct from a process still on its way down.
orphan_count() {
  local n=0 p
  for p in $(pgrep -f "$INNER" 2>/dev/null); do
    [[ "$(ps -o ppid= -p "$p" 2>/dev/null | tr -d ' ')" == "1" ]] && n=$((n + 1))
  done
  echo "$n"
}

# Bounded poll, not a fixed wait: the question is "has it started yet", and it is
# asked rather than guessed at. 20s bounds a rig that is simply not answering.
wait_for_inner() {
  local deadline=$((SECONDS + 20))
  while ((SECONDS < deadline)); do
    [[ "$(inner_count)" -ge 1 ]] && return 0
    # The script finishing before anything was seen means the inner runs skipped.
    kill -0 "$victim" 2>/dev/null || return 1
    sleep 0.05
  done
  return 1
}

start_victim() {
  bash "$SCRIPT" "$CTEST" "$BUILD_DIR" "$PATTERN" >"$log" 2>&1 &
  victim=$!
}

stop_victim() {
  [[ -n "$victim" ]] && kill "$victim" 2>/dev/null
  wait "$victim" 2>/dev/null
  victim=""
  pkill -f "$INNER" 2>/dev/null
  # Let the pkill land before the next phase counts anything.
  sleep 0.3
}

# --- control: the scenario is still running when we sample --------------------
start_victim
if ! wait_for_inner; then
  wait "$victim" 2>/dev/null
  victim=""
  if grep -q '\*\*\*Skipped' "$log"; then
    echo "skipped: $PATTERN needs RomM"
    echo "  start it with: ./scripts/orca/compose.sh up -d"
    exit "$SKIP"
  fi
  echo "FAIL: no $INNER ever started, so nothing was put at risk" >&2
  cat "$log" >&2
  exit 1
fi

sleep "$SETTLE"
still_running="$(inner_count)"
stop_victim

if [[ "$still_running" -eq 0 ]]; then
  echo "FAIL: $PATTERN finishes inside ${SETTLE}s, so the kill phase below would" >&2
  echo "  pass whether or not anything was orphaned. Pick a longer scenario, or" >&2
  echo "  raise SETTLE -- do not just delete this check." >&2
  exit 1
fi

# --- the case itself: killed, and nothing left behind -------------------------
start_victim
if ! wait_for_inner; then
  echo "FAIL: the scenario started in the control phase but not in this one" >&2
  cat "$log" >&2
  exit 1
fi

# SIGTERM is what CTest delivers on TIMEOUT, which is the case being forced.
kill -TERM "$victim" 2>/dev/null
wait "$victim" 2>/dev/null
victim=""

sleep "$SETTLE"
orphans="$(orphan_count)"
alive="$(inner_count)"

if [[ "$orphans" -ne 0 ]]; then
  echo "FAIL: $orphans inner test process(es) outlived $(basename "$SCRIPT")" >&2
  echo "  reparented to init and still on this worktree's RomM, so the next rig" >&2
  echo "  test fails instead of this one. See the trap in $(basename "$SCRIPT")." >&2
  pgrep -lf "$INNER" >&2
  exit 1
fi

echo "ok: nothing outlived $(basename "$SCRIPT") when it was terminated" \
     "(${alive} still alive, ${orphans} orphaned)"
