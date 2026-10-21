#!/usr/bin/env bash
# The main thread's stack against a real cross-compile's frames (M9-18, #216).
#
#   test_stack_budget.sh measured   cross-compiles sysmodule/ in
#                                   devkitpro/devkita64 with
#                                   -fstack-usage -fcallgraph-info=su and runs
#                                   `scripts/stack-budget.py measured` over the
#                                   .ci files it leaves: the deepest chain from
#                                   `main`, with the JSON recursion counted
#                                   kMaxDepth + 1 times, stays under the ceilings
#                                   sysmodule/source/stack_budget.hpp records.
#
# The other three phases of the same script read the tree only, and are run
# straight from tests/CMakeLists.txt. This one cannot: frame sizes exist only
# once the compiler has decided them, and the compiler is in the container.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../scripts/toolchain.sh
. "$REPO_ROOT/scripts/toolchain.sh"
IMAGE="$DEVKITPRO_IMAGE"
SKIP=77

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit $SKIP; }

SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT

phase_measured() {
  command -v docker >/dev/null 2>&1 || skip "no docker"
  docker info >/dev/null 2>&1 || skip "docker daemon not running"
  docker image inspect "$IMAGE" >/dev/null 2>&1 ||
    skip "$IMAGE not pulled (docker pull $IMAGE)"

  # A copy, for tests/test_switch_build.sh's reasons: the container builds as
  # root, and three worktrees may run this at once. A copy also means the extra
  # flags never reach an object a later `make` in the worktree would reuse --
  # make tracks no flags, so a .o built here would be taken as up to date.
  SCRATCH="$(mktemp -d)"
  cp "$REPO_ROOT/VERSION" "$REPO_ROOT/switch.mk" "$SCRATCH/"
  cp -R "$REPO_ROOT/core" "$REPO_ROOT/sysmodule" "$SCRATCH/"
  rm -rf "$SCRATCH/sysmodule/build"

  local log="$SCRATCH/build.log"
  if ! docker run --rm --user "$(id -u):$(id -g)" -v "$SCRATCH:/work" -w /work "$IMAGE" \
        bash -lc 'make -C sysmodule -j"$(nproc)" DEFINES="-fstack-usage -fcallgraph-info=su"' \
        >"$log" 2>&1; then
    cat "$log" >&2
    fail "the sysmodule cross-compile failed"
  fi
  # Asserted: a build that left no call graph would make every ceiling pass
  # over an empty walk.
  compgen -G "$SCRATCH/sysmodule/build/*.ci" >/dev/null ||
    fail "the cross-compile left no .ci files; -fcallgraph-info=su did not reach the compiler"

  "${PYTHON:-python3}" "$REPO_ROOT/scripts/stack-budget.py" measured \
      --repo "$REPO_ROOT" --ci "$SCRATCH/sysmodule/build" ||
    fail "the main thread's measured stack is outside its budget"
}

case "${1:-}" in
  measured) phase_measured ;;
  *)        echo "usage: $0 measured" >&2; exit 2 ;;
esac
