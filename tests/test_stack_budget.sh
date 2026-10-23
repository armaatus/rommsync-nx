#!/usr/bin/env bash
# Every thread's stack against a real cross-compile's frames (M9-18, #216;
# the threads past `main`, M9-31, #268).
#
#   test_stack_budget.sh measured   cross-compiles sysmodule/ in the pinned
#                                   devkitpro/devkita64 (or in place, under
#                                   tests/toolchain_suite.sh --here) with
#                                   -fstack-usage -fcallgraph-info=su and runs
#                                   `scripts/stack-budget.py measured` over the
#                                   .ci files it leaves: the deepest chain from
#                                   `main` and from each `SizedThread::Enter`,
#                                   with the JSON recursion counted kMaxDepth + 1
#                                   times, stays under the ceilings
#                                   sysmodule/source/stack_budget.hpp records and
#                                   inside half of each thread's stack -- and,
#                                   with the worker's stack shrunk, does not.
#
# The other three phases of the same script read the tree only, and are run
# straight from tests/CMakeLists.txt. This one cannot: frame sizes exist only
# once the compiler has decided them, and the compiler is in the container.
# The script runs there too (#253): the build is LTO, so the graph is the LTRANS
# step's, whose nodes are named only by their mangled titles, and the c++filt
# that reads them is the toolchain's.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../scripts/toolchain.sh
. "$REPO_ROOT/scripts/toolchain.sh"
SKIP=77

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit $SKIP; }

SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT

# The walk with one constant shrunk in the scratch copy, put back before it
# returns. The edit, the walk and the restore run in ONE toolchain container:
# a file the host renames over inside a Docker Desktop bind mount can read as
# missing to the next container on macOS, which made the walk after a restore
# fail on a header it said defined nothing. Exits 3 if the edit did not take.
walk_shrunk() {
  toolchain_run "$SCRATCH" '
    file="$1" name="$2" value="$3"
    cp "$file" "$file.orig"
    sed -i "s/$name = 0x[0-9A-Fa-f]*;/$name = $value;/" "$file"
    if ! grep -q "$name = $value;" "$file"; then
      mv "$file.orig" "$file"
      echo "could not shrink $name in the scratch copy of $file"
      exit 3
    fi
    rc=0
    python3 scripts/stack-budget.py measured --repo . --ci sysmodule 2>&1 || rc=$?
    mv "$file.orig" "$file"
    exit "$rc"' "$@"
}

# The walk with FILE's NAME shrunk to VALUE must fail, and its output must say
# so on the line PATTERN matches. A shrink that did not take is its own
# failure: the walk that follows ran on the real constant, and calling that
# "failed, but not on the worker" sends the reader after the wrong half.
expect_shrunk_failure() {
  local file="$1" name="$2" value="$3" pattern="$4" what="$5" out rc=0
  out="$(walk_shrunk "$file" "$name" "$value")" || rc=$?
  if [ "$rc" -eq 3 ]; then
    echo "$out" >&2
    fail "could not shrink $name to $value in the scratch copy of $file"
  fi
  if [ "$rc" -eq 0 ]; then
    echo "$out" >&2
    fail "the walk passed with $name shrunk to $value; it is not holding $what"
  fi
  grep -q "$pattern" <<<"$out" || {
    echo "$out" >&2
    fail "the walk failed with $name shrunk, but not on $what"
  }
}

phase_measured() {
  local missing
  missing="$(toolchain_missing)" && skip "$missing"

  # A copy, for tests/test_switch_build.sh's reasons: three worktrees may run
  # this at once, and a build in the tree leaves objects behind. Here that is
  # sharper: make tracks no flags, so a .o built with the flags below would be
  # taken as up to date by the next ordinary `make` in the worktree.
  SCRATCH="$(mktemp -d)"
  cp "$REPO_ROOT/VERSION" "$REPO_ROOT/switch.mk" "$SCRATCH/"
  cp -R "$REPO_ROOT/core" "$REPO_ROOT/sysmodule" "$SCRATCH/"
  mkdir -p "$SCRATCH/scripts"
  cp "$REPO_ROOT/scripts/stack-budget.py" "$SCRATCH/scripts/"
  # A worktree's own build leaves .ci files beside its ELF as well as in build/,
  # and the walk reads every one under sysmodule/.
  rm -rf "$SCRATCH/sysmodule/build"
  rm -f "$SCRATCH"/sysmodule/*.ci "$SCRATCH"/sysmodule/*.su

  local log="$SCRATCH/build.log"
  # Through the environment rather than on make's command line: a command-line
  # DEFINES would override every `DEFINES +=` switch.mk or a Makefile makes,
  # and the frames measured would stop being the frames that ship. From the
  # environment it is only the starting value those lines append to.
  if ! toolchain_run "$SCRATCH" \
        'DEFINES="-fstack-usage -fcallgraph-info=su" make -C sysmodule -j"$(nproc)"' \
        >"$log" 2>&1; then
    cat "$log" >&2
    fail "the sysmodule cross-compile failed"
  fi
  # Asserted: a build that left no call graph would make every ceiling pass
  # over an empty walk. Under LTO the graph is written by the link, beside the
  # ELF (`sys-rommsync.elf.ltransN.ltrans.ci`); without it, by each compile,
  # in build/. The script reads whichever is there, and refuses both.
  [ -n "$(find "$SCRATCH/sysmodule" -name '*.ci' | head -n 1)" ] ||
    fail "the cross-compile left no .ci files; -fcallgraph-info=su did not reach the compiler"

  toolchain_run "$SCRATCH" 'python3 scripts/stack-budget.py measured --repo . --ci sysmodule' ||
    fail "a thread's measured stack is outside its budget"

  # And the check bites on the real graph (#268): the same walk against a
  # worker stack shrunk to 0x2000 -- half of it under the worker's measured
  # chain -- must fail, and fail on the worker. The script reads the constant
  # out of the header text, so no rebuild; the frames are the ones just measured.
  expect_shrunk_failure sysmodule/source/sized_thread.hpp kThreadStackBytes 0x2000 \
    "the worker thread's deepest" "the worker to its stack"
  echo "ok: with kThreadStackBytes shrunk to 0x2000 the walk fails on the worker thread"

  # And on the ceiling sized_thread.hpp points a reader to rather than quoting a
  # figure: `kWorkerRestBytes` shrunk under the worker's measured chain, with
  # its stack as it is, must fail on that ceiling.
  expect_shrunk_failure sysmodule/source/stack_budget.hpp kWorkerRestBytes 0x100 \
    "the worker thread spends .*(kWorkerRestBytes)" "the worker to its ceiling"
  echo "ok: with kWorkerRestBytes shrunk to 0x100 the walk fails on the worker's ceiling"
}

case "${1:-}" in
  measured) phase_measured ;;
  *)        echo "usage: $0 measured" >&2; exit 2 ;;
esac
