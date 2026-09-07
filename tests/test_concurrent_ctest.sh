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
#   test_concurrent_ctest.sh <ctest> <build-dir> <regex>
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
trap 'rm -rf "$work"' EXIT

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

# A rig that is not running makes both invocations green without either having
# downloaded anything, which proves nothing -- say so rather than pass.
if grep -q '\*\*\*Skipped' "$work/a.log" "$work/b.log"; then
  echo "skipped: $PATTERN needs RomM"
  echo "  start it with: ./scripts/orca/compose.sh up -d"
  exit "$SKIP"
fi

if [[ "$first_status" -ne 0 || "$second_status" -ne 0 ]]; then
  echo "FAIL: concurrent ctest invocations of $PATTERN corrupted each other" >&2
  echo "  first exited $first_status, second exited $second_status" >&2
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
