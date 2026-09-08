#!/usr/bin/env bash
# Two `ctest` invocations against ONE build tree, at the same time (#151).
#
# That is not a contrived arrangement: the local verification loop and the
# automated review both run `ctest` on this worktree, and an agent may be running
# a third. RUN_SERIAL orders tests within one invocation and says nothing about a
# second one (#118), so anything the test binaries share on disk -- the scratch
# directory they download into -- is shared across invocations too.
#
# The regression this is written against is a fixed destination name in a shared
# scratch directory: two `http.download` processes writing `download.bin` and
# `download.bin.part` at once, each removing and renaming the other's file.
# Either invocation could see it, so both are required to pass.
#
#   test_scratch_concurrent.sh <ctest> <build-dir> <regex>
#
# Skips with 77, like rig.smoke, when the tests it drives skip for want of RomM.
set -uo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $(basename "$0") <ctest> <build-dir> <regex>" >&2
  exit 2
fi
CTEST="$1"
BUILD_DIR="$2"
PATTERN="$3"
SKIP=77

work="$(mktemp -d)"
first=""
second=""

# Job control, so that each backgrounded `ctest` below leads a process group of
# its own. Without it they share this script's, and the only thing that could be
# signalled is a group that includes the OUTER ctest running this test.
set -m

# The jobs are killed as well as the directory removed. If CTest ends this script
# on TIMEOUT, two detached `ctest`s would otherwise keep hammering the shared
# RomM fixture while the outer run moves on to other rig tests -- which presents
# as those tests failing, with nothing pointing back here.
#
# The GROUP is signalled, not the pid, because `ctest` does not pass a signal on
# to the test it is running: measured, for TERM and for KILL alike, the inner
# `test_http_native` stays alive and is reparented to init. What holds a
# connection to the fixture is that grandchild, so killing the two `ctest` pids
# leaves exactly the thing that had to go.
#
# `scratch.orphans` (tests/test_scratch_orphans.sh) is the test of this, and it
# is red against a copy of this script with the group kill taken out. Two earlier
# attempts at that test were green against the same broken copy, because they
# polled until no inner process was left -- which always comes true a few seconds
# later when the scenario ends on its own. The question is whether anything is
# orphaned *promptly* after the kill, not whether it dies eventually.
cleanup() {
  for job in $first $second; do
    kill -- "-$job" 2>/dev/null || kill "$job" 2>/dev/null
  done
  wait 2>/dev/null
  rm -rf "$work"
}
trap cleanup EXIT

# Backgrounded and then waited on individually: `wait` without arguments loses
# the exit status of each job, and it is precisely each job that has to pass.
"$CTEST" --test-dir "$BUILD_DIR" -R "$PATTERN" --output-on-failure >"$work/a.log" 2>&1 &
first=$!
"$CTEST" --test-dir "$BUILD_DIR" -R "$PATTERN" --output-on-failure >"$work/b.log" 2>&1 &
second=$!

wait "$first"
first_status=$?
wait "$second"
second_status=$?
first=""
second=""

# A failure is reported before anything else, so that one invocation skipping
# while the other went red cannot be filed as "skipped".
if [[ "$first_status" -ne 0 || "$second_status" -ne 0 ]]; then
  echo "FAIL: concurrent ctest invocations of $PATTERN corrupted each other" >&2
  echo "  first exited $first_status, second exited $second_status" >&2
  for log in a b; do
    echo "--- $log ---" >&2
    cat "$work/$log.log" >&2
  done
  exit 1
fi

# A rig that is not running makes both invocations green without either having
# downloaded anything, which proves nothing -- say so rather than pass. Counted
# per invocation, because ONE of them skipping is not a skip: it is half a run
# reported as none, and the half that did download proved nothing on its own.
skipped=0
for log in a b; do
  if grep -q '\*\*\*Skipped' "$work/$log.log"; then
    skipped=$((skipped + 1))
  fi
done
if [[ "$skipped" -eq 2 ]]; then
  echo "skipped: $PATTERN needs RomM"
  echo "  start it with: ./scripts/orca/compose.sh up -d"
  exit "$SKIP"
fi
if [[ "$skipped" -eq 1 ]]; then
  echo "FAIL: one invocation of $PATTERN reached RomM and the other did not," >&2
  echo "  so nothing ran concurrently and the result means nothing" >&2
  for log in a b; do
    echo "--- $log ---" >&2
    cat "$work/$log.log" >&2
  done
  exit 1
fi

# Neither invocation may have run zero tests: an empty selection is two green
# `ctest` runs that never opened a file, and the assertion above would hold.
for log in a b; do
  if grep -q 'No tests were found' "$work/$log.log"; then
    echo "FAIL: $PATTERN selected no tests" >&2
    cat "$work/$log.log" >&2
    exit 1
  fi
done

echo "ok: two concurrent ctest invocations of $PATTERN both passed"
