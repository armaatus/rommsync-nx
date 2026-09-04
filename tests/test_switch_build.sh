#!/usr/bin/env bash
# Covers the Switch half of the build: sysmodule/Makefile, overlay/Makefile and
# the shared switch.mk they include, plus the CI job that is supposed to fail
# when one of them stops producing something loadable.
#
#   test_switch_build.sh ci        the switch-build job builds both targets
#                                  unconditionally and refuses to publish an
#                                  empty artifact set. This is the regression:
#                                  the job shipped with `if [ -f Makefile ]`
#                                  guards and `if-no-files-found: ignore` while
#                                  the Makefiles did not exist, so it printed a
#                                  ::notice:: and stayed green over a Switch
#                                  build that had never happened. Re-introducing
#                                  either would make a broken cross-compile
#                                  invisible again.
#   test_switch_build.sh builds    devkitA64 actually produces a PFS0 .nsp and an
#                                  ULTR-signed .ovl, from a copy of the tree, so
#                                  a failed run leaves no half-built objects
#                                  behind and no root-owned files in the
#                                  worktree.
#
# `ci` needs nothing but the checkout, so it runs everywhere. `builds` needs
# Docker and the devkitpro/devkita64 image, which the host CI runner does not
# have -- there the switch-build job itself is the enforcement, and this entry
# is its local equivalent. It skips rather than pulling 2.7GB unasked.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="devkitpro/devkita64:latest"
SKIP=77

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit $SKIP; }

SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT

# --- the CI job ---------------------------------------------------------------

# Everything from `  switch-build:` up to the next job at the same indentation.
# Grepping the whole file instead would happily match a guard in another job --
# and so would stopping on a narrower pattern than a job id can be, which is why
# this ends on any 2-space key rather than on lowercase and dashes. A job named
# `switch_build_hw:` appended below this one would otherwise be folded in, and
# its upload step could satisfy an assertion switch-build had stopped meeting.
switch_build_job() {
  awk '/^  switch-build:/ { in_job = 1; next }
       in_job && /^  [^ #]+:/ { in_job = 0 }
       in_job { print }' "$REPO_ROOT/.github/workflows/ci.yml"
}

phase_ci() {
  local job
  job="$(switch_build_job)"
  [ -n "$job" ] || fail "no switch-build job in .github/workflows/ci.yml"

  for target in sysmodule overlay; do
    [ -f "$REPO_ROOT/$target/Makefile" ] || fail "$target/Makefile is missing"
    grep -q -- "make -C $target" <<<"$job" || fail "switch-build does not build $target"
  done

  # The guard, in any of the shapes it could come back as: a conditional on the
  # Makefile's existence, or the ::notice:: that stood in for a real build.
  if grep -qE '\[ *-f +(sysmodule|overlay)/Makefile' <<<"$job"; then
    fail "switch-build still guards the build on the Makefile existing"
  fi
  if grep -q '::notice::' <<<"$job"; then
    fail "switch-build still reports a skipped build as a notice"
  fi

  # A green job that uploaded nothing is the same failure wearing a different
  # hat, so the upload has to be the thing that goes red.
  grep -q 'if-no-files-found: error' <<<"$job" ||
    fail "switch-build would publish an empty artifact set"

  echo "ok: switch-build builds both targets and requires their artifacts"
}

# --- the real cross-compile ---------------------------------------------------

phase_builds() {
  command -v docker >/dev/null 2>&1 || skip "no docker"
  docker info >/dev/null 2>&1 || skip "docker daemon not running"
  docker image inspect "$IMAGE" >/dev/null 2>&1 ||
    skip "$IMAGE not pulled (docker pull $IMAGE)"

  # A copy, not the worktree: the container builds as root, and three worktrees
  # may be running this at once.
  SCRATCH="$(mktemp -d)"
  cp "$REPO_ROOT/VERSION" "$REPO_ROOT/switch.mk" "$SCRATCH/"
  cp -R "$REPO_ROOT/core" "$REPO_ROOT/sysmodule" "$REPO_ROOT/overlay" "$SCRATCH/"
  # Sources only. A .nsp left over from a local build is exactly what make would
  # accept as already up to date, and the assertions below would then be reading
  # the previous build's output instead of this one's.
  rm -rf "$SCRATCH/sysmodule/build" "$SCRATCH/overlay/build"
  rm -f "$SCRATCH"/sysmodule/sys-rommsync.* "$SCRATCH"/overlay/ovl-rommsync.*
  cp "$REPO_ROOT/sysmodule/sys-rommsync.json" "$SCRATCH/sysmodule/"

  local log
  log="$SCRATCH/build.log"
  # --user so the build leaves files this user can delete afterwards. Docker
  # Desktop maps ownership back on macOS, but on Linux the default root would
  # leave a temp directory rm -rf cannot clear.
  if ! docker run --rm --user "$(id -u):$(id -g)" -v "$SCRATCH:/work" -w /work "$IMAGE" \
        bash -lc 'make -C sysmodule -j"$(nproc)" && make -C overlay -j"$(nproc)"' \
        >"$log" 2>&1; then
    cat "$log" >&2
    fail "devkitA64 build failed"
  fi

  local nsp="$SCRATCH/sysmodule/sys-rommsync.nsp"
  local ovl="$SCRATCH/overlay/ovl-rommsync.ovl"
  [ -s "$nsp" ] || fail "no sys-rommsync.nsp"
  [ -s "$ovl" ] || fail "no ovl-rommsync.ovl"
  [ "$(head -c 4 "$nsp")" = "PFS0" ] || fail "sys-rommsync.nsp is not a PFS0 archive"
  # Without this Ultrahand does not list the overlay, and nothing else about the
  # file looks wrong -- see overlay/README.md.
  [ "$(tail -c 4 "$ovl")" = "ULTR" ] || fail "ovl-rommsync.ovl has no ULTR signature"

  # switch.mk generates version.hpp the way CMake does for the host build, from
  # the same VERSION file and the same template.
  local version
  version="$(cat "$REPO_ROOT/VERSION")"
  grep -q "kVersion = \"$version\"" "$SCRATCH/sysmodule/build/rommsync/version.hpp" ||
    fail "version.hpp was not generated from VERSION ($version)"

  # Every core/ translation unit compiled for aarch64 -- the reason the
  # sysmodule builds the engine at all this early. A source that quietly became
  # host-only has no object here, and this is what says so.
  local src obj
  for src in "$REPO_ROOT"/core/src/*.cpp; do
    obj="$SCRATCH/sysmodule/build/$(basename "${src%.cpp}").o"
    [ -f "$obj" ] || fail "core/src/$(basename "$src") produced no aarch64 object"
  done

  # ...and core code reached the image, not just the linker's input. The .lst is
  # the toolchain's own demangled symbol dump of the linked ELF.
  grep -q 'rommsync::version()' "$SCRATCH/sysmodule/build/sys-rommsync.lst" ||
    fail "no core/ symbol in the linked sysmodule"

  # The overlay's NACP is where a user sees a version, and switch_rules will
  # happily bake devkitPro's 1.0.0 default in if nothing sets APP_VERSION.
  LC_ALL=C grep -aq "$version" "$SCRATCH/overlay/ovl-rommsync.nacp" ||
    fail "the overlay NACP does not carry $version"

  # Two sources with the same basename produce one object and silently drop the
  # other, and sysmodule/README.md plans an http/ beside core/src/http.cpp. The
  # guard is in switch.mk; this is what says it still fires.
  : >"$SCRATCH/sysmodule/source/json.cpp"
  if docker run --rm --user "$(id -u):$(id -g)" -v "$SCRATCH:/work" -w /work "$IMAGE" \
        bash -lc 'make -C sysmodule' >"$log" 2>&1; then
    fail "a source shadowing core/src/json.cpp built without complaint"
  fi
  grep -q 'share a basename' "$log" ||
    { cat "$log" >&2; fail "the duplicate basename went unreported"; }
  rm -f "$SCRATCH/sysmodule/source/json.cpp"

  echo "ok: .nsp and .ovl built, signed and carrying core/ $version"
}

case "${1:-}" in
  ci)     phase_ci ;;
  builds) phase_builds ;;
  *)      echo "usage: $0 {ci|builds}" >&2; exit 2 ;;
esac
