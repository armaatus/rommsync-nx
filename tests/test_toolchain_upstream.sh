#!/usr/bin/env bash
# M11-13 (#297): is the libultrahand pin behind upstream's newest tag? A
# warning, never a failure -- a new upstream tag is news, not a break.
#
#   test_toolchain_upstream.sh upstream_check
#                     toolchain_check_upstream against a scratch repo standing
#                     in for upstream: behind, current, ahead, no release tags,
#                     unreachable -- and the CLI CI runs exits 0 on every one.
#                     Reads the tree only, so it never skips.
#   test_toolchain_upstream.sh upstream
#                     the same check against the real upstream: passes, with
#                     the warning, when the pin is behind; skips when upstream
#                     cannot be reached, so it never fails offline.
#
# Its own file rather than a phase of test_toolchain.sh: that file's `libnx`
# needs the image, and switch.toolchain_suite counts every skippable entry run
# by a script that reaches for the image as one the `toolchain` label owes CI.
# This one skips for want of a network, which the switch-build job's suite --
# where a skip is a failure -- has no business answering.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN="$REPO_ROOT/scripts/toolchain.sh"
SKIP=77

SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit $SKIP; }

[ -f "$TOOLCHAIN" ] || fail "no scripts/toolchain.sh"
# shellcheck source=../scripts/toolchain.sh
. "$TOOLCHAIN"

# A scratch repo with these tags, as a file:// URL: git ls-remote reads it
# exactly as it reads GitHub, with no network.
scratch_upstream() {
  local repo="$SCRATCH/upstream-$1" tag
  shift
  git init -q "$repo"
  git -C "$repo" -c user.name=t -c user.email=t@t commit -q --allow-empty -m init
  for tag in "$@"; do git -C "$repo" tag "$tag"; done
  echo "file://$repo"
}

phase_upstream_check() {
  SCRATCH="$(mktemp -d)"
  local url out rc pinned want_rc want_text
  # v2.10.0 is the newest: the case a string sort gets wrong (v2.5.4 > v2.10.0),
  # and the one that would hide a whole minor series behind the pin. A
  # pre-release-looking tag is not a release, so it is never the newest.
  url="$(scratch_upstream many v2.5.3 v2.5.4 v2.10.0 v3.0.0-rc1 nightly)"

  # pinned  want_rc  text
  while IFS='|' read -r pinned want_rc want_text; do
    rc=0
    out="$(toolchain_check_upstream "$pinned" "$url" 2>&1)" || rc=$?
    [ "$rc" = "$want_rc" ] ||
      fail "toolchain_check_upstream $pinned exited $rc, expected $want_rc: $out"
    grep -qF -- "$want_text" <<<"$out" ||
      fail "toolchain_check_upstream $pinned said '$out'; expected '$want_text'"
  done <<'EOF'
v2.5.4|1|::warning::libultrahand is pinned at v2.5.4; upstream has tagged v2.10.0
v2.5.3|1|upstream has tagged v2.10.0
v2.10.0|0|ok: libultrahand v2.10.0 is upstream's newest tag
v2.11.0|0|ok: libultrahand v2.11.0 is not behind upstream's newest tag, v2.10.0
EOF

  rc=0
  out="$(toolchain_check_upstream v2.5.4 "$(scratch_upstream bare nightly)" 2>&1)" || rc=$?
  [ "$rc" = 3 ] || fail "an upstream with no release tags answered $rc, expected 3: $out"
  rc=0
  out="$(toolchain_check_upstream v2.5.4 "file://$SCRATCH/nowhere" 2>&1)" || rc=$?
  [ "$rc" = 3 ] || fail "an unreachable upstream answered $rc, expected 3: $out"
  grep -qF 'cannot reach' <<<"$out" || fail "an unreachable upstream said '$out'"

  # The command CI runs is warn-only on every answer: behind, and unreachable.
  out="$(LIBULTRAHAND_UPSTREAM="$url" bash "$TOOLCHAIN" upstream 2>&1)" ||
    fail "toolchain.sh upstream failed on a pin behind upstream: $out"
  grep -qF '::warning::' <<<"$out" ||
    fail "toolchain.sh upstream did not warn on a pin behind v2.10.0: '$out'"
  out="$(LIBULTRAHAND_UPSTREAM="file://$SCRATCH/nowhere" bash "$TOOLCHAIN" upstream 2>&1)" ||
    fail "toolchain.sh upstream failed with upstream unreachable: $out"
  # ...and the ctest entry skips offline rather than failing.
  rc=0
  out="$(LIBULTRAHAND_UPSTREAM="file://$SCRATCH/nowhere" bash "$0" upstream 2>&1)" || rc=$?
  [ "$rc" = "$SKIP" ] || fail "toolchain.upstream answered $rc offline, expected a skip: $out"
  rc=0
  out="$(LIBULTRAHAND_UPSTREAM="$url" bash "$0" upstream 2>&1)" || rc=$?
  [ "$rc" = 0 ] || fail "toolchain.upstream failed on a pin behind upstream: $out"
  echo "ok: toolchain_check_upstream orders v2.10.0 above v2.5.4, warns, and never fails"
}

phase_upstream() {
  local pin url rc=0
  read -r pin url <<<"$(toolchain_libultrahand_pin)"
  [ -n "$pin" ] && [ -n "$url" ] || fail "cannot read the pin and its url out of .gitmodules"
  toolchain_check_upstream "$pin" "$url" || rc=$?
  case $rc in
    0|1) exit 0 ;;   # behind is the warning printed above, never a failure
    3)   skip "upstream not reachable; whether the pin is behind is unknown" ;;
    *)   fail "toolchain_check_upstream answered $rc" ;;
  esac
}


case "${1:-}" in
  upstream_check) phase_upstream_check ;;
  upstream)       phase_upstream ;;
  *)              echo "usage: $0 {upstream_check|upstream}" >&2; exit 2 ;;
esac
