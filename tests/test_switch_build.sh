#!/usr/bin/env bash
# Covers the Switch half of the build: sysmodule/Makefile, overlay/Makefile and
# the shared switch.mk they include, plus the CI job that is supposed to fail
# when one of them stops producing something loadable.
#
#   test_switch_build.sh ci        the switch-build job builds all three targets
#                                  unconditionally, packages them, and refuses
#                                  to publish an empty artifact set. This is the
#                                  regression: the job shipped with
#                                  `if [ -f Makefile ]` guards and
#                                  `if-no-files-found: ignore` while the
#                                  Makefiles did not exist, so it printed a
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
# The pinned image (#199), named in one place.
# shellcheck source=../scripts/toolchain.sh
. "$REPO_ROOT/scripts/toolchain.sh"
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

  # ...and it packages, on every push. `ctest -R package.builds` is the only
  # other caller of scripts/package.sh inside devkitpro/devkita64, and it skips
  # on every runner `host-tests` uses -- so without this step a failure specific
  # to packaging in the container (a missing `zip`, a tool the image dropped)
  # would first appear on a tag, which is the one moment it must not (#34).
  grep -q 'scripts/package.sh' <<<"$job" ||
    fail "switch-build does not package; a packaging failure would first show on a tag"

  # M9-15 (#204): the image's budget is enforced where the linked ELF exists,
  # which is this job and nowhere else on CI. The step runs the script as it
  # stands -- an `--image-budget` here would be a ceiling moved in a YAML file
  # rather than in the one place that states it and says why.
  grep -q 'scripts/image-budget.sh' <<<"$job" ||
    fail "switch-build does not hold the sysmodule's image to its budget (#204)"
  if grep -q 'image-budget.sh.*--' <<<"$job"; then
    fail "switch-build passes scripts/image-budget.sh a flag; the budget lives in the script (#204)"
  fi

  echo "ok: switch-build builds all three targets, packages them, requires the artifacts, and holds the image to its budget"
}

# --- the real cross-compile ---------------------------------------------------

# Our own code is -Werror, so a warning there fails the compile; but the
# vendored objects drop -Werror (switch.mk, ROMMSYNC_VENDOR_DROPS) and the link
# has no -Werror at all, so a warning from either would scroll past a green
# build. #200 changed the optimisation level under all of them and promised no
# new warnings; this is what keeps that promise past the one log someone read.
check_no_warnings() {
  if grep -qi 'warning:' "$1"; then
    grep -i 'warning:' "$1" >&2
    fail "the Switch build printed warnings"
  fi
}

# M9-9 (#200): every object this build compiles -- ours, core/'s and the
# vendored libultrahand alike -- carries no unwind table. The targets are
# -fno-exceptions, so nothing ever unwinds through them, and `.eh_frame` sits in
# the rodata segment, resident for as long as the process is
# (docs/DEVELOPMENT.md#what-the-image-costs). Checked per object rather than
# only on the linked total, because the total also holds libnx's, newlib's and
# libstdc++'s prebuilt tables, which no flag of ours can remove -- a flag lost
# from one compile would hide inside them.
#
# The container reports every object it inspected, and the count has to match
# the objects on this side: an empty "found none" from a container that never
# ran, or a readelf that failed, is the vacuous pass this file keeps refusing.
check_unwind_tables() {
  local target expected=0 report
  for target in "$@"; do
    local objects=("$SCRATCH/$target/build/"*.o)
    [ -f "${objects[0]}" ] ||
      fail "no objects in $target/build; the unwind-table check would pass vacuously"
    expected=$(( expected + ${#objects[@]} ))
  done
  report="$(toolchain_run "$SCRATCH" 'for t in "$@"; do for o in "$t"/build/*.o; do
                  sections="$("$DEVKITPRO"/devkitA64/bin/aarch64-none-elf-readelf -SW "$o")" ||
                    { echo "unreadable $o"; continue; }
                  if grep -q "\.eh_frame" <<<"$sections"; then echo "unwind $o"; else echo "clean $o"; fi
                done; done' "$@")" || fail "the toolchain inspecting the objects failed"
  [ "$(grep -c '^clean \|^unwind ' <<<"$report")" -eq "$expected" ] ||
    fail "inspected $(grep -c '^clean \|^unwind ' <<<"$report") of $expected objects:
$report"
  if grep -q '^unwind ' <<<"$report"; then
    fail "these objects were compiled with unwind tables (switch.mk lost \
-fno-unwind-tables -fno-asynchronous-unwind-tables for them):
$(sed -n 's/^unwind //p' <<<"$report")"
  fi
}

# image_budget [ARGS] -- scripts/image-budget.sh over the build in $SCRATCH,
# inside the pinned image: the same script, run the same way, that the
# `switch-build` job runs over its own checkout. The ceilings live there, once.
image_budget() {
  toolchain_run "$SCRATCH" 'bash scripts/image-budget.sh "$@"' "$@"
}

# The budget, applied, and then shown to bite. A ceiling nothing has ever seen
# fail is one that may be reading the wrong file, the wrong section or nothing
# at all, and passing every time for that reason (M9-15, #204).
check_image_budget() {
  local out
  out="$(image_budget 2>&1)" || { echo "$out" >&2; fail "the sysmodule's image is over its budget"; }
  echo "$out"

  out="$(image_budget --image-budget 4096 2>&1)" &&
    { echo "$out" >&2; fail "a 4 KiB image budget passed; the ceiling is not being applied"; }
  grep -q 'past its' <<<"$out" ||
    { echo "$out" >&2; fail "a 4 KiB image budget failed, but not on the image's size"; }

  # The .bss tie, against a main.cpp that declares a 4 KiB heap over the one linked:
  # the check has to read the constant out of the source it was built from, or
  # a heap that grew without the constant moving would go unnoticed.
  local main="$SCRATCH/sysmodule/source/main.cpp" saved
  saved="$(cat "$main")"
  sed -i.bak 's/^constexpr size_t kInnerHeapSize = 0x[0-9A-Fa-f]*;/constexpr size_t kInnerHeapSize = 0x1000;/' "$main"
  rm -f "$main.bak"
  out="$(image_budget 2>&1)" &&
    { printf '%s\n' "$saved" >"$main"; echo "$out" >&2;
      fail "a .bss far larger than the declared heap passed the tie"; }
  printf '%s\n' "$saved" >"$main"
  grep -q 'something other than the heap' <<<"$out" ||
    { echo "$out" >&2; fail "the doctored heap failed, but not on the .bss tie"; }
}

phase_builds() {
  local missing
  missing="$(toolchain_missing)" && skip "$missing"

  # The overlay does not build without libultrahand, and an un-initialised
  # submodule is an empty directory rather than a missing one -- so this is
  # checked here, where it reads as "your checkout is incomplete", rather than
  # left to surface as a failed cross-compile. A skip rather than a failure, for
  # the same reason a missing docker image is one: it is a property of the
  # checkout, not of the build under test. The `switch-build` CI job clones with
  # `submodules: recursive` and has no such escape.
  [ -f "$REPO_ROOT/overlay/lib/libultrahand/ultrahand.mk" ] ||
    skip "overlay/lib/libultrahand is empty (git submodule update --init --recursive)"

  # A copy, not the worktree: the container builds as root, and three worktrees
  # may be running this at once.
  SCRATCH="$(mktemp -d)"
  cp "$REPO_ROOT/VERSION" "$REPO_ROOT/switch.mk" "$SCRATCH/"
  cp -R "$REPO_ROOT/core" "$REPO_ROOT/sysmodule" "$REPO_ROOT/overlay" "$SCRATCH/"
  mkdir -p "$SCRATCH/scripts"
  cp "$REPO_ROOT/scripts/image-budget.sh" "$SCRATCH/scripts/" ||
    fail "no scripts/image-budget.sh; nothing holds the image to a budget (#204)"
  # Sources only. A .nsp left over from a local build is exactly what make would
  # accept as already up to date, and the assertions below would then be reading
  # the previous build's output instead of this one's.
  #
  # Named by extension rather than cleared with a `sys-rommsync.*` glob: that
  # glob also matches sys-rommsync.json, the NPDM config without which npdmtool
  # produces no .npdm and the .nsp does not link -- so a glob leaves a restoring
  # `cp` behind it that is load-bearing and looks incidental. Nothing here
  # deletes something it then has to put back. tests/test_package.sh does the
  # same, in the same words.
  local target ext
  rm -rf "$SCRATCH/sysmodule/build" "$SCRATCH/overlay/build"
  for target in sysmodule/sys-rommsync overlay/ovl-rommsync; do
    for ext in nsp nso npdm ovl nro nacp elf map lst; do
      rm -f "$SCRATCH/$target.$ext"
    done
  done

  local log
  log="$SCRATCH/build.log"
  if ! toolchain_run "$SCRATCH" 'make -C sysmodule -j"$(nproc)" && make -C overlay -j"$(nproc)"' \
        >"$log" 2>&1; then
    cat "$log" >&2
    fail "devkitA64 build failed"
  fi
  check_no_warnings "$log"

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

  check_unwind_tables sysmodule overlay
  check_image_budget

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

  # What the console's loaders read out of the files themselves (M9-12, #211),
  # each silent on a console when wrong. scripts/artifact-check.py says why
  # each one matters; the short form:
  #
  #   lny2     libnx's HOS 21.0.0 stamp in MOD0. Ultrahand reads it off the
  #            card and hides an overlay without it on Atmosphere >= 1.10.0.
  #   nacp     the NACP parses by getOverlayInfo's route, with the name and the
  #            version the menu shows -- switch_rules would happily bake
  #            devkitPro's 1.0.0 default in if nothing set APP_VERSION, and an
  #            entry whose NACP does not parse is skipped without a word.
  #   version  ROMMSYNC_NX_VERSION=<VERSION> is a string in both, so a file
  #            from a bug report says what it is, and --gc-sections cannot have
  #            eaten the marker.
  #
  # The overlay has no NPDM to check: it runs inside nx-ovlloader's process,
  # with nx-ovlloader's capabilities. npdm.svcs is the sysmodule's half.
  local check="$REPO_ROOT/scripts/artifact-check.py"
  "${PYTHON:-python3}" "$check" lny2 "$nsp" "$ovl" ||
    fail "a shipped artifact lacks libnx's LNY2 stamp"
  "${PYTHON:-python3}" "$check" nacp "$ovl" --title ovl-rommsync --version "$version" ||
    fail "the overlay's NACP is not one Ultrahand would list as ovl-rommsync $version"
  "${PYTHON:-python3}" "$check" version "$nsp" "$ovl" --version "$version" ||
    fail "a shipped artifact does not say it is $version"

  # Two sources with the same basename produce one object and silently drop the
  # other, and sysmodule/README.md plans an http/ beside core/src/http.cpp. The
  # guard is in switch.mk; this is what says it still fires.
  : >"$SCRATCH/sysmodule/source/json.cpp"
  if toolchain_run "$SCRATCH" 'make -C sysmodule' >"$log" 2>&1; then
    fail "a source shadowing core/src/json.cpp built without complaint"
  fi
  grep -q 'share a basename' "$log" ||
    { cat "$log" >&2; fail "the duplicate basename went unreported"; }
  rm -f "$SCRATCH/sysmodule/source/json.cpp"

  echo "ok: .nsp and .ovl built, signed, hosting and drawing, carrying core/ $version,\
 inside the image budget"
}


# --- the M0-1 probe ------------------------------------------------------------

phase_tlsprobe() {
  local missing
  missing="$(toolchain_missing)" && skip "$missing"

  SCRATCH="$(mktemp -d)"
  cp "$REPO_ROOT/VERSION" "$REPO_ROOT/switch.mk" "$SCRATCH/"
  cp -R "$REPO_ROOT/core" "$REPO_ROOT/tlsprobe" "$SCRATCH/"
  # By extension, for the reason phase_builds gives: a `rommsync-tlsprobe.*`
  # glob has nothing to catch in this target today, but the rule is "nothing
  # here deletes something it then has to put back" and the way that stays true
  # is by not writing the glob in the first place.
  local ext
  rm -rf "$SCRATCH/tlsprobe/build"
  for ext in nsp nso npdm ovl nro nacp elf map lst; do
    rm -f "$SCRATCH/tlsprobe/rommsync-tlsprobe.$ext"
  done

  local log="$SCRATCH/build.log"
  if ! toolchain_run "$SCRATCH" 'make -C tlsprobe -j"$(nproc)"' >"$log" 2>&1; then
    cat "$log" >&2
    fail "the tlsprobe build failed"
  fi
  check_no_warnings "$log"

  local nro="$SCRATCH/tlsprobe/rommsync-tlsprobe.nro"
  [ -s "$nro" ] || fail "no rommsync-tlsprobe.nro"
  # NRO0 sits at 0x10; the first 16 bytes are the entry branch and the mod0
  # offset. A .nro without it is a file hbmenu will not launch.
  [ "$(dd if="$nro" bs=1 skip=16 count=4 2>/dev/null)" = "NRO0" ] ||
    fail "rommsync-tlsprobe.nro has no NRO0 magic"
  # hbmenu launches it on the same HOS 21.0.0 kernel the shipped two run on.
  "${PYTHON:-python3}" "$REPO_ROOT/scripts/artifact-check.py" lny2 "$nro" ||
    fail "the probe lacks libnx's LNY2 stamp"

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

  # The probe's own objects too: it is the measurement of what TLS costs a
  # sysmodule, so it has to be compiled the way the sysmodule is.
  check_unwind_tables tlsprobe

  # The number this target exists to produce, printed on every run so a change
  # in it is visible in the log rather than only on someone's console.
  toolchain_run "$SCRATCH" \
    '"$DEVKITPRO"/devkitA64/bin/aarch64-none-elf-size tlsprobe/rommsync-tlsprobe.elf'

  echo "ok: the M0-1 probe builds as an NRO carrying $version, without core/"
}

case "${1:-}" in
  ci)       phase_ci ;;
  builds)   phase_builds ;;
  tlsprobe) phase_tlsprobe ;;
  *)        echo "usage: $0 {ci|builds|tlsprobe}" >&2; exit 2 ;;
esac
