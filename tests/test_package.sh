#!/usr/bin/env bash
# Covers scripts/package.sh: the install tree it builds, what it refuses to
# ship, and the two properties a release depends on -- that the same inputs
# produce the same bytes, and that unzipping over an existing install does not
# take a user's settings with it.
#
#   test_package.sh layout         the unpacked tree is exactly the five paths
#                                  #32 specifies, with the title id directory
#                                  taken from sys-rommsync.json rather than
#                                  typed here, and nothing else in it.
#   test_package.sh refuses        a .nsp that is not PFS0 and an .ovl with no
#                                  ULTR signature are refused, and no archive is
#                                  left behind. Both build, zip and install
#                                  cleanly; neither does anything on a console.
#   test_package.sh deterministic  two runs on the same inputs are byte-identical,
#                                  which is what makes #34's published checksums
#                                  a statement about the build rather than about
#                                  one upload.
#   test_package.sh upgrade        unzipping over an existing install replaces
#                                  the two artifacts and leaves config.ini,
#                                  token.dat, device.dat, state.db and
#                                  queue.json alone.
#   test_package.sh builds         ...and it packages a real devkitPro build,
#                                  run inside the same container, so the script
#                                  is exercised where #34's release job will run
#                                  it and not only against stubs.
#
# Everything but `builds` uses stubbed artifacts -- four bytes of magic is all
# the script inspects -- so it needs no Docker and never skips. `builds` needs
# the devkitpro/devkita64 image, exactly like `switch.builds`, and skips rather
# than pulling 2.7GB unasked -- which means that on a machine without it,
# nothing exercises the packaging in the container at all. No CI job does either
# yet; see the note on `package.builds` in tests/CMakeLists.txt.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE="$REPO_ROOT/scripts/package.sh"
IMAGE="devkitpro/devkita64:latest"
SKIP=77

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit $SKIP; }

SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT

VERSION="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION")"

# The title id the archive is asserted against, parsed out of the NPDM config by
# something other than the script under test: a JSON reader, where the script
# uses a regex. A literal here would pass on the day the id moves and the
# packaging stops matching it, which is the failure sysmodule/README.md says to
# expect.
title_id() {
  command -v python3 >/dev/null 2>&1 || fail "python3 is needed to read the title id"
  python3 -c '
import json, sys
with open(sys.argv[1]) as f:
    tid = json.load(f)["title_id"]
print(tid[2:].upper() if tid[:2].lower() == "0x" else tid.upper())
' "$REPO_ROOT/sysmodule/sys-rommsync.json"
}

# Four bytes of magic each. The script checks nothing else about them, and a
# stub keeps every phase but `builds` free of a 2.7GB toolchain.
stub_inputs() {
  printf 'PFS0not-a-real-sysmodule' > "$SCRATCH/sys-rommsync.nsp"
  printf 'not-a-real-overlayULTR'   > "$SCRATCH/ovl-rommsync.ovl"
}

run_package() {
  "$PACKAGE" --nsp "$SCRATCH/sys-rommsync.nsp" --ovl "$SCRATCH/ovl-rommsync.ovl" "$@"
}

# --- the tree ------------------------------------------------------------------

phase_layout() {
  bash -n "$PACKAGE" || fail "scripts/package.sh does not parse"

  SCRATCH="$(mktemp -d)"
  stub_inputs

  local tid; tid="$(title_id)" || exit 1
  local out="$SCRATCH/dist"
  local manifest="$SCRATCH/manifest.txt"
  run_package --out "$out" > "$manifest" || fail "packaging failed"

  local zip="$out/rommsync-nx-$VERSION.zip"
  [ -s "$zip" ] || fail "no $zip"

  # Read off the archive rather than off the unpacked tree: an entry named
  # `/etc/passwd` or `../../atmosphere` is normalised away by unzip and would be
  # invisible in a `find` of the result. This is the only place it can be seen.
  local entries
  entries="$(unzip -Z1 "$zip")" || fail "cannot list $zip"

  local entry
  while IFS= read -r entry; do
    case "$entry" in
      /*)     fail "absolute path in the archive: $entry" ;;
      ../*|*/../*|*/..) fail "a .. entry in the archive: $entry" ;;
      *\\*)   fail "a backslash in an archive path: $entry" ;;
      __MACOSX/*|*/.DS_Store|.DS_Store) fail "editor/OS junk in the archive: $entry" ;;
    esac
  done <<<"$entries"

  # The layout, path by path AND in order. The order is not decoration: it is
  # half of what makes two runs byte-identical, and `deterministic` cannot see
  # it -- that phase compares two runs of the same script, so it stays green
  # through any reordering of ENTRIES. This is the only place the order is
  # pinned.
  local expected
  expected="$(printf '%s\n' \
    "README.txt" \
    "LICENSE" \
    "atmosphere/contents/$tid/exefs.nsp" \
    "config/rommsync/config.ini.example" \
    "switch/.overlays/ovl-rommsync.ovl")"
  [ "$entries" = "$expected" ] || {
    echo "--- in the archive ---"; printf '%s\n' "$entries"
    echo "--- expected ---";       printf '%s\n' "$expected"
    fail "the archive is not the install layout"
  }

  # The manifest the script prints is what docs/INSTALL.md (#35) will be checked
  # against, so it has to name the same paths the archive holds.
  [ "$(cat "$manifest")" = "$expected" ] ||
    fail "the printed manifest and the archive's entries disagree"
  # ...and --list produces it without an input to package, which is what makes
  # that check cheap enough to run in a docs test.
  [ "$("$PACKAGE" --list)" = "$expected" ] ||
    fail "--list does not print the archive's entries"

  local tree="$SCRATCH/sd"
  mkdir -p "$tree"
  unzip -qq "$zip" -d "$tree" || fail "cannot unpack $zip"

  # There is no wrapper directory: the archive merges onto an SD card that
  # already has an atmosphere/ and a switch/ full of other people's homebrew,
  # and one extra level would install nothing anywhere.
  [ -f "$tree/atmosphere/contents/$tid/exefs.nsp" ] ||
    fail "no atmosphere/contents/$tid/exefs.nsp"
  [ -f "$tree/switch/.overlays/ovl-rommsync.ovl" ] ||
    fail "no switch/.overlays/ovl-rommsync.ovl"
  [ -f "$tree/config/rommsync/config.ini.example" ] ||
    fail "no config/rommsync/config.ini.example"

  # `exefs.nsp`, not `sys-rommsync.nsp`. The wrong name installs cleanly, boots,
  # and does nothing at all -- so it is asserted from both directions.
  [ "$(head -c 4 "$tree/atmosphere/contents/$tid/exefs.nsp")" = "PFS0" ] ||
    fail "the packaged exefs.nsp is not a PFS0 archive"
  [ "$(tail -c 4 "$tree/switch/.overlays/ovl-rommsync.ovl")" = "ULTR" ] ||
    fail "the packaged .ovl has no ULTR signature"

  # boot2.flag is what makes Atmosphere launch the sysmodule at boot. Shipping
  # it contradicts "installed disabled first" (M8-2), and creating it is
  # ovl-sysmodules' job (#33). Asserted on the name rather than on the flags/
  # directory: whether that directory ships empty is #33's call, and this is not
  # the place to make it for them.
  [ ! -e "$tree/atmosphere/contents/$tid/flags/boot2.flag" ] ||
    fail "the archive ships boot2.flag; the sysmodule must arrive disabled"
  grep -q 'boot2' <<<"$entries" &&
    fail "the archive names boot2 somewhere; the sysmodule must arrive disabled"
  [ ! -e "$tree/config/rommsync/config.ini" ] ||
    fail "the archive ships a config.ini; an upgrade would overwrite the user's"
  grep -q 'tlsprobe' <<<"$entries" &&
    fail "the archive ships a tlsprobe artifact; it is a spike, never installed"

  # The README a user reads is the one this build produced: an unsubstituted
  # placeholder is a file that tells them to install into a directory called
  # @TITLE_ID@.
  grep -q "$VERSION" "$tree/README.txt" || fail "README.txt does not carry $VERSION"
  grep -q "$tid" "$tree/README.txt" || fail "README.txt does not carry the title id"
  # The placeholder shape, not every at-sign: a contact address or a GitHub
  # handle in the shipped README is not a bug, and banning them would fail here
  # with a message about something else entirely.
  grep -qE '@[A-Z_]+@' "$tree/README.txt" &&
    fail "README.txt still holds an unsubstituted @PLACEHOLDER@"

  cmp -s "$REPO_ROOT/packaging/config.ini.example" \
         "$tree/config/rommsync/config.ini.example" ||
    fail "the packaged config.ini.example is not packaging/config.ini.example"
  cmp -s "$REPO_ROOT/LICENSE" "$tree/LICENSE" || fail "the packaged LICENSE differs"

  echo "ok: the archive is exactly the install layout, under $tid"
}

# --- what it refuses to ship ---------------------------------------------------

phase_refuses() {
  SCRATCH="$(mktemp -d)"
  stub_inputs

  local out="$SCRATCH/dist"
  local zip="$out/rommsync-nx-$VERSION.zip"
  local log="$SCRATCH/refuse.log"

  # An earlier good run's archive, sitting under the name this one would write.
  # It is the file a release job picks up, and nothing about it says it predates
  # whatever the refusal below is about -- so a refused run has to take it with
  # it, not merely decline to write a new one.
  run_package --out "$out" >/dev/null || fail "the setup run failed"
  [ -s "$zip" ] || fail "the setup run produced no archive"

  # A corrupted copy of each, one at a time, so the failure is attributable.
  printf 'XXXXnot-a-real-sysmodule' > "$SCRATCH/broken.nsp"
  if "$PACKAGE" --nsp "$SCRATCH/broken.nsp" --ovl "$SCRATCH/ovl-rommsync.ovl" \
       --out "$out" >"$log" 2>&1; then
    fail "a .nsp that is not a PFS0 archive was packaged"
  fi
  grep -q 'PFS0' "$log" || { cat "$log" >&2; fail "the refusal does not name PFS0"; }
  [ ! -e "$zip" ] || fail "a refused build left an archive behind at $zip"

  # An .ovl one byte short of its signature: it builds, it zips, it installs,
  # and Ultrahand silently does not list it.
  head -c 21 "$SCRATCH/ovl-rommsync.ovl" > "$SCRATCH/broken.ovl"
  if "$PACKAGE" --nsp "$SCRATCH/sys-rommsync.nsp" --ovl "$SCRATCH/broken.ovl" \
       --out "$out" >"$log" 2>&1; then
    fail "an .ovl with no ULTR signature was packaged"
  fi
  grep -q 'ULTR' "$log" || { cat "$log" >&2; fail "the refusal does not name ULTR"; }
  [ ! -e "$zip" ] || fail "a refused build left an archive behind at $zip"

  # And a missing input, which is what running the script before the build looks
  # like.
  if "$PACKAGE" --nsp "$SCRATCH/absent.nsp" --ovl "$SCRATCH/ovl-rommsync.ovl" \
       --out "$out" >"$log" 2>&1; then
    fail "a missing sysmodule build was packaged"
  fi
  [ ! -e "$zip" ] || fail "a refused build left an archive behind at $zip"

  echo "ok: a wrong .nsp, an unsigned .ovl and a missing input are all refused"
}

# --- the same inputs, the same bytes -------------------------------------------

phase_deterministic() {
  SCRATCH="$(mktemp -d)"
  stub_inputs

  run_package --out "$SCRATCH/one" >/dev/null || fail "the first run failed"
  # A second later and with a different mtime on the inputs: the timestamps in
  # the archive are fixed, so neither may reach it.
  touch "$SCRATCH/sys-rommsync.nsp" "$SCRATCH/ovl-rommsync.ovl"
  run_package --out "$SCRATCH/two" >/dev/null || fail "the second run failed"

  cmp -s "$SCRATCH/one/rommsync-nx-$VERSION.zip" \
         "$SCRATCH/two/rommsync-nx-$VERSION.zip" ||
    fail "two runs on the same inputs produced different bytes"

  # Re-running into a directory that already holds an archive rewrites it rather
  # than merging into it -- zip updates an archive it can open, and a stale entry
  # from a previous layout would survive.
  run_package --out "$SCRATCH/one" >/dev/null || fail "the re-run failed"
  cmp -s "$SCRATCH/one/rommsync-nx-$VERSION.zip" \
         "$SCRATCH/two/rommsync-nx-$VERSION.zip" ||
    fail "packaging over an existing archive did not reproduce it"

  echo "ok: the same inputs produce the same bytes"
}

# --- an upgrade is this same zip, unpacked again -------------------------------

phase_upgrade() {
  SCRATCH="$(mktemp -d)"
  stub_inputs
  local tid; tid="$(title_id)" || exit 1

  run_package --out "$SCRATCH/dist" >/dev/null || fail "packaging failed"
  local zip="$SCRATCH/dist/rommsync-nx-$VERSION.zip"

  # An SD card with a previous install and a configured, paired console on it.
  local sd="$SCRATCH/sd"
  mkdir -p "$sd/config/rommsync" "$sd/atmosphere/contents/$tid/flags" \
           "$sd/switch/.overlays"
  echo "[server]"                  > "$sd/config/rommsync/config.ini"
  echo "url = https://romm.lan"   >> "$sd/config/rommsync/config.ini"
  : > "$sd/atmosphere/contents/$tid/flags/boot2.flag"
  local user_state
  for user_state in token.dat device.dat state.db queue.json; do
    echo "the user's $user_state" > "$sd/config/rommsync/$user_state"
  done
  # Somebody else's homebrew, in the two directories this archive merges into.
  echo "someone else's overlay" > "$sd/switch/.overlays/ovl-sysmodules.ovl"
  mkdir -p "$sd/atmosphere/contents/0100000000000352"
  echo "someone else's sysmodule" > "$sd/atmosphere/contents/0100000000000352/exefs.nsp"
  # ...and the two files at the SD root that this archive DOES take over. The
  # layout #32 specifies puts README.txt and LICENSE there, so they are replaced
  # like the artifacts are, and README.txt says so. Seeded here so the exemption
  # below is a decision on the record rather than a case the test never met.
  echo "someone else's README" > "$sd/README.txt"
  echo "someone else's LICENSE" > "$sd/LICENSE"

  local before after
  before="$(cd "$sd" && find . -type f -exec cksum {} \; | sort)"

  local pass
  for pass in 1 2; do
    unzip -qq -o "$zip" -d "$sd" || fail "unpacking pass $pass failed"
  done

  after="$(cd "$sd" && find . -type f -exec cksum {} \; | sort)"

  # Everything that was there before is still there, byte for byte -- including
  # the boot2.flag that says the sysmodule is enabled, which an archive shipping
  # a flags/ directory could not promise.
  local line path
  while IFS= read -r line; do
    path="${line##* }"
    case "$path" in
      *"/atmosphere/contents/$tid/exefs.nsp"|*"/switch/.overlays/ovl-rommsync.ovl"| \
      ./README.txt|./LICENSE)
        continue ;;  # the archive's own files, replaced on purpose
    esac
    grep -qxF "$line" <<<"$after" || fail "unpacking changed or removed $path"
  done <<<"$before"

  # ...and the two artifacts arrived.
  [ "$(head -c 4 "$sd/atmosphere/contents/$tid/exefs.nsp")" = "PFS0" ] ||
    fail "the upgrade did not install exefs.nsp"
  [ "$(tail -c 4 "$sd/switch/.overlays/ovl-rommsync.ovl")" = "ULTR" ] ||
    fail "the upgrade did not install the overlay"
  grep -q 'rommsync-nx' "$sd/README.txt" ||
    fail "the upgrade did not replace README.txt at the SD root"
  cmp -s "$REPO_ROOT/LICENSE" "$sd/LICENSE" ||
    fail "the upgrade did not replace LICENSE at the SD root"

  echo "ok: unzipping twice over an install leaves config.ini and the rest alone"
}

# --- the real thing ------------------------------------------------------------

phase_builds() {
  command -v docker >/dev/null 2>&1 || skip "no docker"
  docker info >/dev/null 2>&1 || skip "docker daemon not running"
  docker image inspect "$IMAGE" >/dev/null 2>&1 ||
    skip "$IMAGE not pulled (docker pull $IMAGE)"
  [ -f "$REPO_ROOT/overlay/lib/libultrahand/ultrahand.mk" ] ||
    skip "overlay/lib/libultrahand is empty (git submodule update --init --recursive)"

  # A copy, not the worktree, for the reasons tests/test_switch_build.sh gives:
  # the container builds as root and three worktrees may be doing this at once.
  SCRATCH="$(mktemp -d)"
  cp "$REPO_ROOT/VERSION" "$REPO_ROOT/switch.mk" "$REPO_ROOT/LICENSE" "$SCRATCH/"
  cp -R "$REPO_ROOT/core" "$REPO_ROOT/sysmodule" "$REPO_ROOT/overlay" \
        "$REPO_ROOT/packaging" "$SCRATCH/"
  mkdir -p "$SCRATCH/scripts"
  cp "$PACKAGE" "$SCRATCH/scripts/"
  rm -rf "$SCRATCH/sysmodule/build" "$SCRATCH/overlay/build"
  rm -f "$SCRATCH"/sysmodule/sys-rommsync.* "$SCRATCH"/overlay/ovl-rommsync.*
  cp "$REPO_ROOT/sysmodule/sys-rommsync.json" "$SCRATCH/sysmodule/"

  local log="$SCRATCH/build.log"
  # Packaged inside the container, not on this host: #34's release job runs the
  # script there, and "zip is not installed in the image" is a failure that
  # would otherwise only appear on a tag.
  if ! docker run --rm --user "$(id -u):$(id -g)" -v "$SCRATCH:/work" -w /work "$IMAGE" \
        bash -lc 'make -C sysmodule -j"$(nproc)" && make -C overlay -j"$(nproc)" \
                  && scripts/package.sh' >"$log" 2>&1; then
    cat "$log" >&2
    fail "the devkitA64 build-and-package failed"
  fi

  local tid; tid="$(title_id)" || exit 1
  local zip="$SCRATCH/dist/rommsync-nx-$VERSION.zip"
  [ -s "$zip" ] || { cat "$log" >&2; fail "no $zip"; }

  local sd="$SCRATCH/sd"
  mkdir -p "$sd"
  unzip -qq "$zip" -d "$sd" || fail "cannot unpack $zip"

  # The renames are the point: the build produced sys-rommsync.nsp, and what a
  # user installs is exefs.nsp under the title id.
  cmp -s "$SCRATCH/sysmodule/sys-rommsync.nsp" \
         "$sd/atmosphere/contents/$tid/exefs.nsp" ||
    fail "the packaged exefs.nsp is not the sysmodule that was built"
  cmp -s "$SCRATCH/overlay/ovl-rommsync.ovl" \
         "$sd/switch/.overlays/ovl-rommsync.ovl" ||
    fail "the packaged .ovl is not the overlay that was built"

  echo "ok: a real devkitPro build packages into the install layout under $tid"
}

case "${1:-}" in
  layout)        phase_layout ;;
  refuses)       phase_refuses ;;
  deterministic) phase_deterministic ;;
  upgrade)       phase_upgrade ;;
  builds)        phase_builds ;;
  *) echo "usage: $0 {layout|refuses|deterministic|upgrade|builds}" >&2; exit 2 ;;
esac
