#!/usr/bin/env bash
# The ctest entries that need the devkitPro toolchain, run where it is (M9-12, #211).
#
#   tests/toolchain_suite.sh          each entry, reaching the image through docker
#   tests/toolchain_suite.sh --here   each entry in place, inside the pinned image --
#                                     what the `switch-build` CI job runs
#   tests/toolchain_suite.sh --list   the entries, `name script phase`, one a line
#
# Each of these skips (77) where the image is not, and that is every runner the
# host-tests job uses -- so `ctest` alone would let them run nowhere in CI, which
# is how a cross-compile's symbols, NACP and NPDM went unread on every push until
# #211. They run here instead, and here a skip is a FAILURE: inside the image
# there is nothing left for them to be missing.
#
# `ctest -L toolchain` is the same set, and `switch.toolchain_suite` holds the two
# to each other: an entry labelled there and absent here would run nowhere.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../scripts/toolchain.sh
. "$REPO_ROOT/scripts/toolchain.sh"

# name             script                phase
ENTRIES='
toolchain.libnx    test_toolchain.sh     libnx
switch.builds      test_switch_build.sh  builds
switch.tlsprobe    test_switch_build.sh  tlsprobe
npdm.svcs          test_npdm.sh          svcs
package.builds     test_package.sh       builds
'

entries() { grep -v '^ *$' <<<"$ENTRIES" | awk '{ print $1, $2, $3 }'; }

case "${1:-}" in
  --list) entries; exit 0 ;;
  --here)
    export ROMMSYNC_IN_TOOLCHAIN=1
    # Refused rather than skipped, for the same reason as below: the job that
    # says --here is the job that claims to be in the image.
    if missing="$(toolchain_missing)"; then
      echo "::error::--here, but the toolchain is not here: $missing" >&2
      exit 1
    fi ;;
  '') ;;
  *) echo "usage: $0 [--here|--list]" >&2; exit 2 ;;
esac

failed=()
while read -r name script phase; do
  echo "::group::$name"
  started=$SECONDS
  bash "$REPO_ROOT/tests/$script" "$phase" </dev/null
  status=$?
  echo "::endgroup::"
  case $status in
    0)  echo "ok: $name ($(( SECONDS - started )) s)" ;;
    77) echo "::error::$name skipped; inside the toolchain a skip is a failure (output above)" >&2
        failed+=("$name") ;;
    *)  echo "::error::$name failed with status $status (output above)" >&2
        failed+=("$name") ;;
  esac
done < <(entries)

if [ "${#failed[@]}" -gt 0 ]; then
  echo "FAIL: ${failed[*]}" >&2
  exit 1
fi
echo "ok: every toolchain entry ran and passed"
