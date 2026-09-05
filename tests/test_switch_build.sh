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
#   test_switch_build.sh tlsprobe  ...and the M0-1 probe builds as a real NRO,
#                                  reporting the footprint it costs. Separate
#                                  from `builds` because it is a spike, not a
#                                  shipped target: it may be retired when M8
#                                  lands the real backend, and retiring it
#                                  should not touch the assertions about the two
#                                  targets that ship (tlsprobe/README.md).
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

  for target in sysmodule overlay tlsprobe; do
    [ -f "$REPO_ROOT/$target/Makefile" ] || fail "$target/Makefile is missing"
    grep -q -- "make -C $target" <<<"$job" || fail "switch-build does not build $target"
  done

  # The guard, in any of the shapes it could come back as: a conditional on the
  # Makefile's existence, or the ::notice:: that stood in for a real build.
  if grep -qE '\[ *-f +(sysmodule|overlay|tlsprobe)/Makefile' <<<"$job"; then
    fail "switch-build still guards the build on the Makefile existing"
  fi
  if grep -q '::notice::' <<<"$job"; then
    fail "switch-build still reports a skipped build as a notice"
  fi

  # A green job that uploaded nothing is the same failure wearing a different
  # hat, so the upload has to be the thing that goes red.
  grep -q 'if-no-files-found: error' <<<"$job" ||
    fail "switch-build would publish an empty artifact set"

  echo "ok: switch-build builds all three targets and requires their artifacts"
}

# --- the real cross-compile ---------------------------------------------------

phase_builds() {
  command -v docker >/dev/null 2>&1 || skip "no docker"
  docker info >/dev/null 2>&1 || skip "docker daemon not running"
  docker image inspect "$IMAGE" >/dev/null 2>&1 ||
    skip "$IMAGE not pulled (docker pull $IMAGE)"

  # A copy, not the worktree: the container builds as root, and three worktrees
  # may be running this at once.
  # The overlay does not build without libultrahand, and an un-initialised
  # submodule is an empty directory rather than a missing one -- so this is
  # checked here, where it reads as "your checkout is incomplete", rather than
  # left to surface as a failed cross-compile.
  [ -f "$REPO_ROOT/overlay/lib/libultrahand/ultrahand.mk" ] ||
    skip "overlay/lib/libultrahand is empty (git submodule update --init --recursive)"

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

  # The sysmodule hosts the service the overlay opens (M4-1). Nothing in this
  # repo can run it before the M8-1 gate, so this is the only automated check
  # that the loop is still in the image at all.
  grep -q 'rommsync::sysmodule::ServiceServer::Serve' "$SCRATCH/sysmodule/build/sys-rommsync.lst" ||
    fail "the sysmodule does not host the IPC service"
  # The name it registers has to be in the NPDM's service_host or the kernel
  # refuses the registration at boot, and the failure is a process that is not
  # there rather than an error anyone sees.
  grep -q '"rommsync"' "$REPO_ROOT/sysmodule/sys-rommsync.json" ||
    fail "sys-rommsync.json does not declare the rommsync service host"

  # libultrahand is linked, not merely present: the overlay draws with it, and a
  # build that quietly stopped compiling it would still produce a signed .ovl
  # that shows an empty panel. Checked on the linked ELF's own symbol dump.
  local ovl_lst="$SCRATCH/overlay/build/ovl-rommsync.lst"
  [ -s "$ovl_lst" ] || fail "no symbol listing at $ovl_lst; the checks below would pass vacuously"
  grep -q 'tsl::gfx::Renderer::drawString' "$ovl_lst" ||
    fail "libultrahand is not linked into the overlay"
  # ...and so is the half of the status screen that lives in core/ and is what
  # `ctest -R overlay.status` actually tests. A screen drawing something else
  # would make that suite a test of nothing shipped.
  grep -q 'rommsync::overlay::Render' "$ovl_lst" ||
    fail "the status screen's view model is not linked into the overlay"

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

  echo "ok: .nsp and .ovl built, signed, hosting and drawing, carrying core/ $version"
}


# --- the M0-1 probe ------------------------------------------------------------

phase_tlsprobe() {
  command -v docker >/dev/null 2>&1 || skip "no docker"
  docker info >/dev/null 2>&1 || skip "docker daemon not running"
  docker image inspect "$IMAGE" >/dev/null 2>&1 ||
    skip "$IMAGE not pulled (docker pull $IMAGE)"

  SCRATCH="$(mktemp -d)"
  cp "$REPO_ROOT/VERSION" "$REPO_ROOT/switch.mk" "$SCRATCH/"
  cp -R "$REPO_ROOT/core" "$REPO_ROOT/tlsprobe" "$SCRATCH/"
  rm -rf "$SCRATCH/tlsprobe/build"
  rm -f "$SCRATCH"/tlsprobe/rommsync-tlsprobe.*

  local log="$SCRATCH/build.log"
  if ! docker run --rm --user "$(id -u):$(id -g)" -v "$SCRATCH:/work" -w /work "$IMAGE" \
        bash -lc 'make -C tlsprobe -j"$(nproc)"' >"$log" 2>&1; then
    cat "$log" >&2
    fail "the tlsprobe build failed"
  fi

  local nro="$SCRATCH/tlsprobe/rommsync-tlsprobe.nro"
  [ -s "$nro" ] || fail "no rommsync-tlsprobe.nro"
  # NRO0 sits at 0x10; the first 16 bytes are the entry branch and the mod0
  # offset. A .nro without it is a file hbmenu will not launch.
  [ "$(dd if="$nro" bs=1 skip=16 count=4 2>/dev/null)" = "NRO0" ] ||
    fail "rommsync-tlsprobe.nro has no NRO0 magic"

  # The probe asks for version.hpp without compiling core/ (ROMMSYNC_WANT_VERSION
  # in tlsprobe/Makefile). If that knob stops working the probe still builds and
  # still runs -- it just reports a version it did not get from VERSION, which is
  # the one thing a heap measurement must not do.
  local version
  version="$(cat "$REPO_ROOT/VERSION")"
  grep -q "kVersion = \"$version\"" "$SCRATCH/tlsprobe/build/rommsync/version.hpp" ||
    fail "the probe's version.hpp was not generated from VERSION ($version)"

  # ...and core/ is NOT in it. The measurement is only attributable if the image
  # holds the TLS path and nothing else (tlsprobe/Makefile).
  #
  # The listing's existence is asserted first, because this is a *negative*
  # check: an absent file makes grep exit 1, which reads as "no core symbol
  # found" and passes. The path comes from devkitPro's base_rules rather than
  # from switch.mk, so it can move without anything in this repo changing.
  local lst="$SCRATCH/tlsprobe/build/rommsync-tlsprobe.lst"
  [ -s "$lst" ] || fail "no symbol listing at $lst; the core/ check would pass vacuously"
  if grep -q 'rommsync::version()' "$lst"; then
    fail "core/ was linked into the probe; its footprint no longer means what it says"
  fi

  # The number this target exists to produce, printed on every run so a change
  # in it is visible in the log rather than only on someone's console.
  docker run --rm --user "$(id -u):$(id -g)" -v "$SCRATCH:/work" -w /work "$IMAGE" \
    bash -lc '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-size \
      tlsprobe/rommsync-tlsprobe.elf'

  echo "ok: the M0-1 probe builds as an NRO carrying $version, without core/"
}

case "${1:-}" in
  ci)       phase_ci ;;
  builds)   phase_builds ;;
  tlsprobe) phase_tlsprobe ;;
  *)        echo "usage: $0 {ci|builds|tlsprobe}" >&2; exit 2 ;;
esac
