#!/usr/bin/env bash
# Covers scripts/package.sh: the install tree it builds, what it refuses to
# ship, and the two properties a release depends on -- that the same inputs
# produce the same bytes, and that unzipping over an existing install does not
# take a user's settings with it.
#
#   test_package.sh layout         the unpacked tree is exactly the paths
#                                  #32, #33 and #211 specify, with the title id
#                                  directory taken from sys-rommsync.json rather
#                                  than typed here, and nothing else in it. One
#                                  is toolbox.json, which ovl-sysmodules parses
#                                  -- so it is asserted as JSON with the fields
#                                  that overlay requires, not just as a name in
#                                  the list.
#   test_package.sh toolbox        every toolbox.json ovl-sysmodules would skip, or
#                                  abort on, is refused by the check that reads
#                                  the packaged one (#211).
#   test_package.sh refuses        a .nsp that is not PFS0 and an .ovl with no
#                                  ULTR signature are refused, and no archive is
#                                  left behind. Both build, zip and install
#                                  cleanly; neither does anything on a console.
#   test_package.sh deterministic  two runs on the same inputs are byte-identical,
#                                  which is what makes #34's published checksums
#                                  a statement about the build rather than about
#                                  one upload.
#   test_package.sh upgrade        unzipping over an existing install replaces
#                                  the artifacts and leaves config.ini,
#                                  token.dat, device.dat, state.db, queue.json,
#                                  rommsync.log
#                                  and the flags/boot2.flag that says the
#                                  sysmodule is enabled alone.
#   test_package.sh wording        README.txt calls the build a PRE-RELEASE only
#                                  when VERSION is one, and says it has not run
#                                  on a console only while the launch gate (#305)
#                                  holds a console row (#303).
#   test_package.sh reproducible   the three Switch targets, built from the same
#                                  sources at two different paths, are the same
#                                  bytes, and so is the zip packaged from each --
#                                  what lets the release job ship switch-build's
#                                  artifacts and a rebuilder check them (#303).
#   test_package.sh builds         ...and it packages a real devkitPro build,
#                                  run inside the same container, so the script
#                                  is exercised where #34's release job will run
#                                  it and not only against stubs.
#
# Everything but `builds` and `reproducible` uses stubbed artifacts -- four bytes of magic is all
# the script inspects -- so it needs no Docker and never skips. `builds` needs
# the devkitpro/devkita64 image, exactly like `switch.builds`, and skips rather
# than pulling 2.7GB unasked -- which means that on a machine without it,
# nothing here exercises the packaging in the container. CI does: `switch-build`
# runs scripts/package.sh and this entry on every push, in place inside the
# image (#211), and the `release` job runs the script on a tag (#34).
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE="$REPO_ROOT/scripts/package.sh"
NOTICES="$REPO_ROOT/scripts/notices.sh"
# The pinned image (#199), named in one place.
# shellcheck source=../scripts/toolchain.sh
. "$REPO_ROOT/scripts/toolchain.sh"
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
program_id() {
  command -v python3 >/dev/null 2>&1 || fail "python3 is needed to read the program id"
  python3 -c '
import json, sys
with open(sys.argv[1]) as f:
    tid = json.load(f)["program_id"]
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

# The layout, path by path AND in order. The order is not decoration: it is
# half of what makes two runs byte-identical, and `deterministic` cannot see
# it -- that phase compares two runs of the same script, so it stays green
# through any reordering of ENTRIES. This is the only place the order is
# pinned.
#
# `flags/` is a directory entry, deliberately (#211): ovl-sysmodules has
# `mkdir`ed it before writing boot2.flag only since its #6 ("autostart can't be
# enabled if the flags folder doesn't exist"), and builds without that fix are
# still in the wild. An empty directory costs nothing and enables nothing --
# boot2.flag inside it is what does that, and it is not shipped.
expected_entries() {
  printf '%s\n' \
    "config/rommsync/README.txt" \
    "config/rommsync/LICENSE" \
    "config/rommsync/THIRD_PARTY_NOTICES.txt" \
    "config/rommsync/COPYING.GPL-2.0" \
    "atmosphere/contents/$1/exefs.nsp" \
    "atmosphere/contents/$1/toolbox.json" \
    "atmosphere/contents/$1/flags/" \
    "config/rommsync/config.ini.example" \
    "switch/.overlays/ovl-rommsync.ovl"
}

# assert_archive ZIP TID -- the archive, read as an archive. The same assertions
# over the stubbed build `layout` packages and the real one `builds` does, so
# what CI ships is held to what the stubs are.
assert_archive() {
  local zip="$1" tid="$2"
  [ -s "$zip" ] || fail "no $zip"

  # Read off the archive rather than off the unpacked tree: an entry named
  # `/etc/passwd` or `../../atmosphere` is normalised away by unzip and would be
  # invisible in a `find` of the result. This is the only place it can be seen.
  local entries
  entries="$(unzip -Z1 "$zip")" || fail "cannot list $zip"

  # Junk a macOS or Windows zip tool adds, which Ultrahand filters both when it
  # builds a zip and again when it scans one -- which says how often it turns up.
  local entry
  while IFS= read -r entry; do
    case "$entry" in
      /*)     fail "absolute path in the archive: $entry" ;;
      ../*|*/../*|*/..|..) fail "a .. entry in the archive: $entry" ;;
      *\\*)   fail "a backslash in an archive path: $entry" ;;
      __MACOSX/*|*/.DS_Store|.DS_Store|._*|*/._*|Thumbs.db|*/Thumbs.db)
              fail "editor/OS junk in the archive: $entry" ;;
    esac
  done <<<"$entries"

  local expected
  expected="$(expected_entries "$tid")"
  [ "$entries" = "$expected" ] || {
    echo "--- in the archive ---"; printf '%s\n' "$entries"
    echo "--- expected ---";       printf '%s\n' "$expected"
    fail "the archive is not the install layout"
  }

  # A directory entry, by the zip's own record of it and not only by a trailing
  # slash in its name: zipinfo prints the entry's mode, and a directory's starts
  # with `d`.
  unzip -Z "$zip" "atmosphere/contents/$tid/flags/" | grep -q '^d' ||
    fail "atmosphere/contents/$tid/flags/ is not a directory entry"
}

phase_layout() {
  bash -n "$PACKAGE" || fail "scripts/package.sh does not parse"

  SCRATCH="$(mktemp -d)"
  stub_inputs

  local tid; tid="$(program_id)" || exit 1
  local out="$SCRATCH/dist"
  local manifest="$SCRATCH/manifest.txt"
  run_package --out "$out" > "$manifest" || fail "packaging failed"

  local zip="$out/rommsync-nx-$VERSION.zip"
  assert_archive "$zip" "$tid"
  local expected entries
  expected="$(expected_entries "$tid")"
  entries="$(unzip -Z1 "$zip")"

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
  [ -f "$tree/atmosphere/contents/$tid/toolbox.json" ] ||
    fail "no atmosphere/contents/$tid/toolbox.json"

  # ovl-sysmodules PARSES this one, and drops the module from its list without a
  # word if it cannot: `tid`, `name` and a boolean `requires_reboot` are the
  # three fields it requires, `tid` goes through strtoul base 16 so an
  # unsubstituted @TITLE_ID@ reads as program id 0, and `requires_reboot: true`
  # would put sys-rommsync in the Static section, where the toggle needs a
  # reboot the two-switch table promises it does not. Asserted field by field
  # rather than by comparing the file to a copy of itself.
  python3 "$REPO_ROOT/tests/toolbox_check.py" \
      "$tree/atmosphere/contents/$tid/toolbox.json" "$tid" "$VERSION" ||
    fail "the packaged toolbox.json is not one ovl-sysmodules would list"

  # `exefs.nsp`, not `sys-rommsync.nsp`. The wrong name installs cleanly, boots,
  # and does nothing at all -- so it is asserted from both directions.
  [ "$(head -c 4 "$tree/atmosphere/contents/$tid/exefs.nsp")" = "PFS0" ] ||
    fail "the packaged exefs.nsp is not a PFS0 archive"
  [ "$(tail -c 4 "$tree/switch/.overlays/ovl-rommsync.ovl")" = "ULTR" ] ||
    fail "the packaged .ovl has no ULTR signature"

  # boot2.flag is what makes Atmosphere launch the sysmodule at boot. Shipping
  # it contradicts "installed disabled first" (M8-2), and creating it is
  # ovl-sysmodules' job. The directory it goes in arrives, empty (#211).
  [ -d "$tree/atmosphere/contents/$tid/flags" ] ||
    fail "unpacking did not create atmosphere/contents/$tid/flags/"
  [ -z "$(ls -A "$tree/atmosphere/contents/$tid/flags")" ] ||
    fail "atmosphere/contents/$tid/flags/ arrives holding something"
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
  local readme="$tree/config/rommsync/README.txt"
  grep -q "$VERSION" "$readme" || fail "README.txt does not carry $VERSION"
  grep -q "$tid" "$readme" || fail "README.txt does not carry the title id"
  # The placeholder shape, not every at-sign: a contact address or a GitHub
  # handle in the shipped README is not a bug, and banning them would fail here
  # with a message about something else entirely.
  grep -qE '@[A-Z_]+@' "$readme" &&
    fail "README.txt still holds an unsubstituted @PLACEHOLDER@"

  cmp -s "$REPO_ROOT/packaging/config.ini.example" \
         "$tree/config/rommsync/config.ini.example" ||
    fail "the packaged config.ini.example is not packaging/config.ini.example"
  cmp -s "$REPO_ROOT/LICENSE" "$tree/config/rommsync/LICENSE" ||
    fail "the packaged LICENSE differs"

  # Nothing lands at the SD root (#300). The root is shared by every piece of
  # homebrew on the card, and a README.txt or LICENSE there is somebody else's
  # file this archive would silently replace.
  grep -qv '/' <<<"$entries" &&
    fail "the archive puts a file at the SD root: $(grep -v '/' <<<"$entries" | head -n 1)"

  # The overlay binary is GPL-2.0 (docs/LICENSING.md), so its licence text and
  # the notices for everything linked into either binary travel with it. Both
  # are compared against their sources, not merely found: a notices file from a
  # stale manifest is the non-compliance this exists to prevent.
  cmp -s "$REPO_ROOT/packaging/licenses/GPL-2.0.txt" \
         "$tree/config/rommsync/COPYING.GPL-2.0" ||
    fail "the packaged COPYING.GPL-2.0 is not packaging/licenses/GPL-2.0.txt"
  "$NOTICES" > "$SCRATCH/notices.txt" || fail "scripts/notices.sh failed"
  cmp -s "$SCRATCH/notices.txt" "$tree/config/rommsync/THIRD_PARTY_NOTICES.txt" ||
    fail "the packaged THIRD_PARTY_NOTICES.txt is not what scripts/notices.sh generates"

  # The overlay names the same title id, because it looks for `exefs.nsp` and
  # `flags/boot2.flag` under it to tell "not installed" from "not set to boot"
  # (#33). It is a C++ constant rather than an archive entry, so nothing above
  # would catch it drifting -- and a stale copy is an overlay reporting a
  # correctly installed sysmodule as missing.
  local overlay_tid
  overlay_tid="$(sed -n 's/.*kProgramIdHex = "\([0-9A-Fa-f]*\)".*/\1/p' \
                 "$REPO_ROOT/overlay/source/card_probe.hpp" | head -n 1 |
                 tr '[:lower:]' '[:upper:]')"
  [ -n "$overlay_tid" ] ||
    fail "no kProgramIdHex in overlay/source/card_probe.hpp"
  [ "$overlay_tid" = "$tid" ] ||
    fail "overlay/source/card_probe.hpp says $overlay_tid; the archive installs under $tid"

  echo "ok: the archive is exactly the install layout, under $tid"
}

# --- toolbox.json, read the way the overlay that parses it would ----------------

# Every way a toolbox.json can be wrong that ovl-sysmodules either skips in
# silence or -- built -fno-exceptions over nlohmann/json, with no try/catch --
# aborts on, taking every other sysmodule's entry with it (#211). Each is a
# file the generator does not write today; this is what keeps it that way, by
# showing the check refuses each one rather than trusting that it would.
phase_toolbox() {
  SCRATCH="$(mktemp -d)"
  local check="$REPO_ROOT/tests/toolbox_check.py"
  local tid=4200000000524D53 version=1.2.3 dir out
  dir="$SCRATCH/$tid"
  mkdir -p "$dir"

  # toolbox_case NAME JSON [ARGS...] -- 0 when the check accepts it.
  toolbox_case() {
    printf '%s\n' "$2" > "$dir/toolbox.json"
    out="$(python3 "$check" "$dir/toolbox.json" "$tid" "$version" "${@:3}" 2>&1)"
  }
  local good='{"name": "sys-rommsync", "tid": "4200000000524D53", "version": "1.2.3", "requires_reboot": false}'
  toolbox_case good "$good" || { echo "$out" >&2; fail "a correct toolbox.json was refused"; }
  toolbox_case good-npdm "$good" --program-id 0x4200000000524D53 ||
    { echo "$out" >&2; fail "a toolbox.json agreeing with the NPDM was refused"; }

  local name json
  while IFS='|' read -r name json; do
    [ -n "$name" ] || continue
    toolbox_case "$name" "$json" && fail "toolbox.json with $name was accepted: $json"
    grep -q '^FAIL: ' <<<"$out" || { echo "$out" >&2; fail "$name was refused without a reason"; }
  done <<'EOF'
requires_reboot as a string|{"name": "sys-rommsync", "tid": "4200000000524D53", "version": "1.2.3", "requires_reboot": "false"}
requires_reboot as a number|{"name": "sys-rommsync", "tid": "4200000000524D53", "version": "1.2.3", "requires_reboot": 0}
tid as a number|{"name": "sys-rommsync", "tid": 4200000000524, "version": "1.2.3", "requires_reboot": false}
name as a bool|{"name": true, "tid": "4200000000524D53", "version": "1.2.3", "requires_reboot": false}
version as a number|{"name": "sys-rommsync", "tid": "4200000000524D53", "version": 1.2, "requires_reboot": false}
no name|{"tid": "4200000000524D53", "version": "1.2.3", "requires_reboot": false}
no requires_reboot|{"name": "sys-rommsync", "tid": "4200000000524D53", "version": "1.2.3"}
a fifth key|{"name": "sys-rommsync", "tid": "4200000000524D53", "version": "1.2.3", "requires_reboot": false, "author": "x"}
a duplicated key|{"name": "sys-rommsync", "tid": "4200000000524D53", "version": "1.2.3", "requires_reboot": false, "requires_reboot": false}
a 15-digit tid|{"name": "sys-rommsync", "tid": "420000000524D53", "version": "1.2.3", "requires_reboot": false}
a 0x-prefixed tid|{"name": "sys-rommsync", "tid": "0x4200000000524D53", "version": "1.2.3", "requires_reboot": false}
a lowercase tid beside an uppercase directory|{"name": "sys-rommsync", "tid": "4200000000524d53", "version": "1.2.3", "requires_reboot": false}
another version|{"name": "sys-rommsync", "tid": "4200000000524D53", "version": "1.2.2", "requires_reboot": false}
requires_reboot true|{"name": "sys-rommsync", "tid": "4200000000524D53", "version": "1.2.3", "requires_reboot": true}
an array|[{"name": "sys-rommsync", "tid": "4200000000524D53", "version": "1.2.3", "requires_reboot": false}]
a trailing comma|{"name": "sys-rommsync", "tid": "4200000000524D53", "version": "1.2.3", "requires_reboot": false,}
EOF

  # The same good file, in a directory that is not its tid: Atmosphere loads
  # the directory, so the two have to name the same program.
  mkdir -p "$SCRATCH/0100000000000352"
  printf '%s\n' "$good" > "$SCRATCH/0100000000000352/toolbox.json"
  out="$(python3 "$check" "$SCRATCH/0100000000000352/toolbox.json" "$tid" "$version" 2>&1)" &&
    fail "a toolbox.json naming $tid was accepted inside 0100000000000352/"
  grep -q '^FAIL: .*sits in' <<<"$out" ||
    { echo "$out" >&2; fail "the misplaced toolbox.json was refused, but not for its directory"; }

  # ...and a tid the built NPDM does not carry: the toggle would launch a
  # program id that is not this process.
  toolbox_case npdm "$good" --program-id 0x4200000000524D54 &&
    fail "a toolbox.json disagreeing with the NPDM's program id was accepted"
  grep -q "^FAIL: .*program id" <<<"$out" ||
    { echo "$out" >&2; fail "the NPDM mismatch was refused, but not for the program id"; }

  echo "ok: toolbox.json is held to its exact keys, types, tid, directory and program id"
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
  local tid; tid="$(program_id)" || exit 1

  run_package --out "$SCRATCH/dist" >/dev/null || fail "packaging failed"
  local zip="$SCRATCH/dist/rommsync-nx-$VERSION.zip"

  # An SD card with a previous install and a configured, paired console on it.
  local sd="$SCRATCH/sd"
  mkdir -p "$sd/config/rommsync" "$sd/atmosphere/contents/$tid/flags" \
           "$sd/switch/.overlays"
  echo "[server]"                  > "$sd/config/rommsync/config.ini"
  echo "url = https://romm.lan"   >> "$sd/config/rommsync/config.ini"
  : > "$sd/atmosphere/contents/$tid/flags/boot2.flag"
  # A previous release's toolbox.json, naming a version this build is not. It
  # sits beside the flag and must be replaced -- ovl-sysmodules renders the
  # `version` field into its list row, so a stale one has the overlay reporting
  # the build the user upgraded *away* from.
  printf '{"name":"sys-rommsync","tid":"%s","version":"0.0.1","requires_reboot":false}\n' \
    "$tid" > "$sd/atmosphere/contents/$tid/toolbox.json"
  local user_state
  for user_state in token.dat device.dat state.db queue.json rommsync.log; do
    echo "the user's $user_state" > "$sd/config/rommsync/$user_state"
  done
  # Somebody else's homebrew, in the two directories this archive merges into.
  echo "someone else's overlay" > "$sd/switch/.overlays/ovl-sysmodules.ovl"
  mkdir -p "$sd/atmosphere/contents/0100000000000352"
  echo "someone else's sysmodule" > "$sd/atmosphere/contents/0100000000000352/exefs.nsp"
  # ...and the two files at the SD root that releases before #300 took over.
  # This archive keeps its README.txt and LICENSE under config/rommsync/, so
  # somebody else's at the root must come through untouched.
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
  # the boot2.flag that says the sysmodule is enabled, inside the flags/
  # directory the archive now also carries: a directory entry unpacked over a
  # directory leaves what is in it alone.
  local line path
  while IFS= read -r line; do
    # `cksum` prints `crc size path`, so the path is everything after the second
    # field -- not after the last space, which would truncate a path containing
    # one and could slip past the exemptions below.
    path="${line#* * }"
    case "$path" in
      *"/atmosphere/contents/$tid/exefs.nsp"|*"/atmosphere/contents/$tid/toolbox.json"| \
      *"/switch/.overlays/ovl-rommsync.ovl")
        continue ;;  # the archive's own files, replaced on purpose
    esac
    grep -qxF "$line" <<<"$after" || fail "unpacking changed or removed $path"
  done <<<"$before"

  # ...and the two artifacts arrived.
  [ "$(head -c 4 "$sd/atmosphere/contents/$tid/exefs.nsp")" = "PFS0" ] ||
    fail "the upgrade did not install exefs.nsp"
  [ "$(tail -c 4 "$sd/switch/.overlays/ovl-rommsync.ovl")" = "ULTR" ] ||
    fail "the upgrade did not install the overlay"
  grep -q 'rommsync-nx' "$sd/config/rommsync/README.txt" ||
    fail "the upgrade did not install config/rommsync/README.txt"
  [ -s "$sd/config/rommsync/THIRD_PARTY_NOTICES.txt" ] ||
    fail "the upgrade did not install config/rommsync/THIRD_PARTY_NOTICES.txt"
  python3 "$REPO_ROOT/tests/toolbox_check.py" \
      "$sd/atmosphere/contents/$tid/toolbox.json" "$tid" "$VERSION" ||
    fail "the upgrade did not replace the previous release's toolbox.json"

  echo "ok: unzipping twice over an install leaves config.ini, boot2.flag and the rest alone"
}

# --- the notices: nothing linked without its licence (#300) -------------------

phase_notices() {
  bash -n "$NOTICES" || fail "scripts/notices.sh does not parse"
  SCRATCH="$(mktemp -d)"
  local manifest="$REPO_ROOT/packaging/third_party.manifest"

  # The real tree: every -l, every submodule, a row.
  "$NOTICES" --check 2>"$SCRATCH/err" ||
    { cat "$SCRATCH/err" >&2; fail "the manifest misses a linked library"; }

  # ...and the check is not vacuous. Each case is the manifest minus one row,
  # which must be refused by name: -lcurl is in switch.mk, libultrahand is in
  # .gitmodules.
  local name token
  for name in curl libultrahand; do
    grep -v "^$name |" "$manifest" > "$SCRATCH/short.manifest"
    "$NOTICES" --check --manifest "$SCRATCH/short.manifest" 2>"$SCRATCH/err" &&
      fail "a manifest without its $name row was accepted"
    case "$name" in curl) token="-lcurl" ;; *) token="overlay/lib/libultrahand" ;; esac
    grep -qF -- "$token" "$SCRATCH/err" ||
      fail "the refusal for a missing $name row does not name $token: $(cat "$SCRATCH/err")"
  done

  # A link map is read the same way: an archive the linker pulled in with no
  # row, and one in a binary its row does not list, are both refused. Map lines
  # in the shape GNU ld writes them.
  printf 'LOAD /opt/devkitpro/portlibs/switch/lib/libfoo.a\n' > "$SCRATCH/foo.map"
  "$NOTICES" --check --map overlay="$SCRATCH/foo.map" 2>"$SCRATCH/err" &&
    fail "a link map naming libfoo.a, which has no row, was accepted"
  grep -q 'libfoo.a' "$SCRATCH/err" || fail "the refusal does not name libfoo.a"
  printf 'LOAD /opt/devkitpro/portlibs/switch/lib/libcurl.a\n' > "$SCRATCH/curl.map"
  "$NOTICES" --check --map sysmodule="$SCRATCH/curl.map" 2>/dev/null &&
    fail "libcurl.a in the sysmodule was accepted though its row says overlay only"
  "$NOTICES" --check --map overlay="$SCRATCH/curl.map" 2>/dev/null ||
    fail "libcurl.a in the overlay was refused though its row says overlay"

  # The libultrahand row names the commit the submodule pointer does: a bump
  # that leaves the row behind is a notice pointing at somebody else's source.
  local pinned
  pinned="$(git -C "$REPO_ROOT" ls-files -s overlay/lib/libultrahand | awk '{print $2}')"
  [ -n "$pinned" ] || fail "cannot read the libultrahand gitlink"
  grep -E '^libultrahand \|' "$manifest" | grep -q "$pinned" ||
    fail "the libultrahand row does not name the pinned commit $pinned"

  # The GPL text shipped is the one libultrahand itself is licensed by.
  if [ -f "$REPO_ROOT/overlay/lib/libultrahand/LICENSE" ]; then
    cmp -s "$REPO_ROOT/overlay/lib/libultrahand/LICENSE" \
           "$REPO_ROOT/packaging/licenses/GPL-2.0.txt" ||
      fail "packaging/licenses/GPL-2.0.txt is not libultrahand's LICENSE"
  fi

  # Every row reaches the notices, with its licence text in full.
  local out="$SCRATCH/notices.txt" text missing
  "$NOTICES" > "$out" || fail "scripts/notices.sh failed"
  while IFS='|' read -r name _ _ _ _ _ text; do
    name="$(sed 's/[[:space:]]*$//' <<<"$name")"
    text="$(tr -d '[:space:]' <<<"$text")"
    grep -qF "$name" "$out" || fail "the notices do not name $name"
    # Line by line rather than as one block: grep has no multi-line match, and
    # every line of the text being present is what "carries it" means here.
    missing="$(grep -vxF -f "$out" "$REPO_ROOT/packaging/licenses/$text" | grep -v '^$' | head -n 1)"
    [ -z "$missing" ] || fail "the notices lack a line of packaging/licenses/$text: $missing"
  done < <(grep -vE '^[[:space:]]*(#|$)' "$manifest")
  grep -q "tree/v$VERSION" "$out" || fail "the notices do not link the source tag v$VERSION"
  # ...and the release's own source archive, submodule included (#320).
  grep -q "releases/download/v$VERSION/rommsync-nx-$VERSION-source\.tar\.gz" "$out" ||
    fail "the notices do not link rommsync-nx-$VERSION-source.tar.gz on the v$VERSION release"

  echo "ok: every linked library has a row, and every row reaches the notices"
}

# --- the source archive: the GPL-2.0 section 3 offer (#320) --------------------

# The regular files (and links) a tar.gz holds, one a line, sorted -- read by
# tar rather than by the script under test's own Python.
archive_files() {
  tar -tzf "$1" | grep -v '/$' | LC_ALL=C sort
}

phase_source() {
  local script="$REPO_ROOT/scripts/source-archive.sh"
  [ -x "$script" ] || fail "no executable $script"
  bash -n "$script" || fail "$script does not parse"
  SCRATCH="$(mktemp -d)"

  local name="rommsync-nx-$VERSION-source.tar.gz" top="rommsync-nx-$VERSION"
  "$script" --out "$SCRATCH/one" >"$SCRATCH/out" 2>"$SCRATCH/err" ||
    { cat "$SCRATCH/err" >&2; fail "source-archive.sh failed"; }
  local tgz="$SCRATCH/one/$name"
  [ -s "$tgz" ] || fail "no $name in --out; it wrote: $(ls "$SCRATCH/one")"
  [ "$(ls "$SCRATCH/one")" = "$name" ] || fail "--out holds more than $name: $(ls "$SCRATCH/one")"

  # Exactly the tagged tree and the tree the submodule pointer names -- the half
  # GitHub's own "Source code" archive drops -- under one top directory, and
  # nothing a checkout happens to have lying around.
  local sub=overlay/lib/libultrahand pinned
  pinned="$(git -C "$REPO_ROOT" ls-tree HEAD "$sub" | awk '$2 == "commit" { print $3 }')"
  [ -n "$pinned" ] || fail "cannot read the $sub gitlink"
  git -C "$REPO_ROOT/$sub" cat-file -e "$pinned^{commit}" 2>/dev/null ||
    fail "$sub is not checked out at $pinned (git submodule update --init --recursive)"
  {
    git -C "$REPO_ROOT" ls-tree -r HEAD | awk -F'\t' '$1 !~ / commit / { print $2 }'
    git -C "$REPO_ROOT/$sub" ls-tree -r --name-only "$pinned" | sed "s|^|$sub/|"
  } | sed "s|^|$top/|" | LC_ALL=C sort > "$SCRATCH/want"
  archive_files "$tgz" > "$SCRATCH/got" || fail "tar cannot read $name"
  grep -q "^$top/$sub/ultrahand.mk\$" "$SCRATCH/got" ||
    fail "the archive has no libultrahand sources; it is GitHub's archive over again"
  diff "$SCRATCH/want" "$SCRATCH/got" >"$SCRATCH/diff" ||
    { head -n 20 "$SCRATCH/diff" >&2; fail "the archive is not exactly HEAD plus the pinned submodule"; }

  # ...with the committed bytes, not the work tree's.
  mkdir -p "$SCRATCH/x"
  tar -xzf "$tgz" -C "$SCRATCH/x" || fail "cannot unpack $name"
  git -C "$REPO_ROOT/$sub" show "$pinned:ultrahand.mk" | cmp -s - "$SCRATCH/x/$top/$sub/ultrahand.mk" ||
    fail "the archive's ultrahand.mk is not the pinned commit's"
  git -C "$REPO_ROOT" show HEAD:switch.mk | cmp -s - "$SCRATCH/x/$top/switch.mk" ||
    fail "the archive's switch.mk is not HEAD's"

  # Deterministic like the zip: a gzip header carrying the time it was written
  # would make every run, and so every checksum, different.
  [ "$(od -An -tx1 -j4 -N4 "$tgz" | tr -d ' \n')" = 00000000 ] ||
    fail "the gzip header carries a timestamp"
  "$script" --out "$SCRATCH/two" >/dev/null 2>&1 || fail "the second run failed"
  cmp -s "$tgz" "$SCRATCH/two/$name" || fail "two runs on the same commit produced different bytes"

  # A submodule whose commit is not here is refused, not archived as the empty
  # directory GitHub's archive leaves -- and no half-written archive is left.
  local repo="$SCRATCH/repo"
  mkdir -p "$repo/scripts"
  cp "$script" "$repo/scripts/"
  echo "$VERSION" > "$repo/VERSION"
  printf '[submodule "lib/gone"]\n\tpath = lib/gone\n\turl = https://example.invalid/gone.git\n' \
    > "$repo/.gitmodules"
  git -C "$repo" init -q
  git -C "$repo" add VERSION .gitmodules scripts
  git -C "$repo" update-index --add --cacheinfo \
    "160000,1111111111111111111111111111111111111111,lib/gone"
  git -C "$repo" -c user.name=t -c user.email=t@t -c commit.gpgsign=false \
      -c core.hooksPath=/dev/null commit -qm fixture ||
    fail "cannot build the fixture repo"
  "$repo/scripts/source-archive.sh" --out "$SCRATCH/gone" >/dev/null 2>"$SCRATCH/err" &&
    fail "a submodule with no checkout was archived"
  grep -q 'lib/gone' "$SCRATCH/err" || fail "the refusal does not name lib/gone: $(cat "$SCRATCH/err")"
  [ ! -e "$SCRATCH/gone/$name" ] || fail "a refused run left an archive behind"

  echo "ok: $name is HEAD plus the pinned libultrahand, byte for byte and deterministic"
}

# --- what the README says about the build -------------------------------------

# A checkout holding only what package.sh reads, so VERSION and the launch gate
# can be set per case without touching this one's.
mini_repo() {
  local dir="$1"
  mkdir -p "$dir/scripts" "$dir/sysmodule"
  cp "$REPO_ROOT/LICENSE" "$dir/"
  cp "$REPO_ROOT/sysmodule/sys-rommsync.json" "$dir/sysmodule/"
  cp -R "$REPO_ROOT/packaging" "$dir/"
  cp "$PACKAGE" "$NOTICES" "$REPO_ROOT/scripts/release-notes.sh" "$dir/scripts/"
}

# The stand-in for scripts/launch-gate.sh (#305) that tests/test_release.sh's
# `wording` uses too: `--rows` as `id|kind|claim`, and a held gate is one with a
# `console` row left in it.
fake_gate() {
  local dir="$1" held="$2" kind=repo
  [ "$held" = held ] && kind=console
  printf '#!/usr/bin/env bash\necho "smoke|%s|#44 complete"\n' "$kind" \
    > "$dir/scripts/launch-gate.sh"
  chmod +x "$dir/scripts/launch-gate.sh"
}

phase_wording() {
  SCRATCH="$(mktemp -d)"
  stub_inputs
  local repo="$SCRATCH/repo"
  mini_repo "$repo"

  local version gate readme tree="$SCRATCH/tree"
  for version in 1.0.0 1.0.0-rc1; do
    for gate in none held passed; do
      echo "$version" > "$repo/VERSION"
      rm -f "$repo/scripts/launch-gate.sh"
      [ "$gate" = none ] || fake_gate "$repo" "$gate"
      "$repo/scripts/package.sh" --nsp "$SCRATCH/sys-rommsync.nsp" \
          --ovl "$SCRATCH/ovl-rommsync.ovl" --out "$SCRATCH/out" >/dev/null 2>&1 ||
        fail "package.sh failed on $version, launch gate $gate"
      rm -rf "$tree"
      unzip -qq "$SCRATCH/out/rommsync-nx-$version.zip" -d "$tree" ||
        fail "cannot unpack the $version archive"
      readme="$tree/config/rommsync/README.txt"

      # README.txt.in carries its conditional paragraphs as marker lines, and
      # none of them may reach a user whatever the case.
      grep -qE '^@|@[A-Z_]+@' "$readme" &&
        fail "$version, gate $gate: README.txt still holds template markers"

      case "$version" in
        *-*) grep -q 'PRE-RELEASE' "$readme" ||
               fail "$version: a prerelease's README does not say it is one" ;;
        *)   grep -qi 'pre-release' "$readme" &&
               fail "$version: a stable release's README calls it a PRE-RELEASE" ;;
      esac

      # No gate is not a passed gate: until #305 exists, nothing has said a
      # console ran this.
      if [ "$gate" = passed ]; then
        grep -q 'not yet been run on a real console' "$readme" &&
          fail "$version, gate passed: README.txt still says it never ran on a console"
      else
        grep -q 'not yet been run on a real console' "$readme" ||
          fail "$version, gate $gate: README.txt does not say it never ran on a console"
      fi
    done
  done

  echo "ok: README.txt says PRE-RELEASE and 'not run on a console' only when each is true"
}

# --- the real thing ------------------------------------------------------------

# copy_sources DEST -- what a devkitPro build and package.sh read, and nothing a
# previous build left: the build directories and the targets' outputs are
# removed, so what DEST holds afterwards is this build's.
copy_sources() {
  local dest="$1"
  mkdir -p "$dest/scripts"
  cp "$REPO_ROOT/VERSION" "$REPO_ROOT/switch.mk" "$REPO_ROOT/LICENSE" "$dest/"
  cp -R "$REPO_ROOT/core" "$REPO_ROOT/sysmodule" "$REPO_ROOT/overlay" \
        "$REPO_ROOT/tlsprobe" "$REPO_ROOT/packaging" "$dest/"
  cp "$PACKAGE" "$NOTICES" "$REPO_ROOT/scripts/release-notes.sh" "$dest/scripts/"
  cp "$REPO_ROOT/.gitmodules" "$dest/"
  # Removing the object directory is what actually forces the rebuild --
  # `$(OUTPUT).elf: $(OFILES)` and the objects live in `build/`, so the whole
  # link chain is already out of date once it is gone. Clearing the target's
  # own outputs on top of that is defence in depth against a make rule that
  # stops going through the objects.
  #
  # Named by extension rather than cleared with a `sys-rommsync.*` glob: that
  # glob also matches sys-rommsync.json, the NPDM config without which npdmtool
  # produces no .npdm and the .nsp does not link -- so a glob leaves a restoring
  # `cp` behind it that is load-bearing and looks incidental. Nothing here
  # deletes something it then has to put back.
  local target ext
  rm -rf "$dest/sysmodule/build" "$dest/overlay/build" "$dest/tlsprobe/build"
  for target in sysmodule/sys-rommsync overlay/ovl-rommsync tlsprobe/rommsync-tlsprobe; do
    for ext in nsp nso npdm ovl nro nacp elf map lst; do
      rm -f "$dest/$target.$ext"
    done
  done
}

# The three targets and the zip, as `sha256sum` lines relative to DIR.
built_digests() {
  ( cd "$1" && sha256sum sysmodule/sys-rommsync.nsp overlay/ovl-rommsync.ovl \
                          tlsprobe/rommsync-tlsprobe.nro "dist/rommsync-nx-$VERSION.zip" )
}

# Two builds of the same sources at two different absolute paths -- the
# difference between a CI runner's checkout and anyone else's -- must be the
# same bytes. Path is the input that varied in practice: `__FILE__` and the
# DWARF a link writes carry it, and before switch.mk mapped it away the same
# commit built in two directories gave three different binaries (#303). Built
# in sequence and in separate containers, so nothing one build leaves is
# visible to the other.
phase_reproducible() {
  local missing
  missing="$(toolchain_missing)" && skip "$missing"
  [ -f "$REPO_ROOT/overlay/lib/libultrahand/ultrahand.mk" ] ||
    skip "overlay/lib/libultrahand is empty (git submodule update --init --recursive)"
  command -v sha256sum >/dev/null 2>&1 || skip "no sha256sum"

  SCRATCH="$(mktemp -d)"
  local one="$SCRATCH/one" two="$SCRATCH/elsewhere/at/another/depth/two"
  copy_sources "$one"
  copy_sources "$two"

  local build='make -C sysmodule -j"$(nproc)" && make -C overlay -j"$(nproc)" &&
               make -C tlsprobe -j"$(nproc)" && scripts/package.sh'
  # Through docker each copy is mounted where it is asked to be, so the two
  # builds see two paths there too; in the image (--here) they already do.
  TOOLCHAIN_WORKDIR=/work toolchain_run "$one" "$build" >"$SCRATCH/one.log" 2>&1 ||
    { cat "$SCRATCH/one.log" >&2; fail "the first build failed"; }
  TOOLCHAIN_WORKDIR=/elsewhere/at/another/depth/two toolchain_run "$two" "$build" \
      >"$SCRATCH/two.log" 2>&1 ||
    { cat "$SCRATCH/two.log" >&2; fail "the second build failed"; }

  local a b
  a="$(built_digests "$one")" || fail "the first build is missing an output"
  b="$(built_digests "$two")" || fail "the second build is missing an output"
  echo "$a"
  [ "$a" = "$b" ] || {
    diff <(echo "$a") <(echo "$b") >&2
    fail "the same sources built at two paths are different bytes; a published checksum
nobody else can reproduce is a statement about one runner, not about the tag"
  }
  echo "ok: the same sources at two paths build the same .nsp, .ovl, .nro and zip"
}


phase_builds() {
  local missing
  missing="$(toolchain_missing)" && skip "$missing"
  [ -f "$REPO_ROOT/overlay/lib/libultrahand/ultrahand.mk" ] ||
    skip "overlay/lib/libultrahand is empty (git submodule update --init --recursive)"

  # A copy, not the worktree, for the reasons tests/test_switch_build.sh gives:
  # the container builds as root and three worktrees may be doing this at once.
  # Sources only, so the assertions below read this build's output and not a
  # previous one's.
  SCRATCH="$(mktemp -d)"
  copy_sources "$SCRATCH"

  local log="$SCRATCH/build.log"
  # Packaged inside the container, not on this host: #34's release job runs the
  # script there, and "zip is not installed in the image" is a failure that
  # would otherwise only appear on a tag.
  if ! toolchain_run "$SCRATCH" 'make -C sysmodule -j"$(nproc)" && make -C overlay -j"$(nproc)" \
                  && scripts/package.sh' >"$log" 2>&1; then
    cat "$log" >&2
    fail "the devkitA64 build-and-package failed"
  fi

  local tid; tid="$(program_id)" || exit 1
  local zip="$SCRATCH/dist/rommsync-nx-$VERSION.zip"
  [ -s "$zip" ] || { cat "$log" >&2; fail "no $zip"; }

  # What the linker really pulled into each binary, specs-file archives
  # included, each with a manifest row that lists that binary (#300).
  "$NOTICES" --check --manifest "$REPO_ROOT/packaging/third_party.manifest" \
      --map sysmodule="$SCRATCH/sysmodule/build/sys-rommsync.map" \
      --map overlay="$SCRATCH/overlay/build/ovl-rommsync.map" ||
    fail "a library in a real link map has no row in packaging/third_party.manifest"

  assert_archive "$zip" "$tid"

  local sd="$SCRATCH/sd"
  mkdir -p "$sd"
  unzip -qq "$zip" -d "$sd" || fail "cannot unpack $zip"

  # toolbox.json's tid against the program id the kernel will actually give
  # the process -- read out of the built NPDM, not out of the JSON that
  # generated it (#211).
  local npdm_id
  npdm_id="$("${PYTHON:-python3}" "$REPO_ROOT/scripts/npdm-check.py" \
                  --nsp "$SCRATCH/sysmodule/sys-rommsync.nsp" --print |
                sed -n 's/^program_id: //p')"
  [ -n "$npdm_id" ] || fail "npdm-check.py read no program id out of the built .nsp"
  "${PYTHON:-python3}" "$REPO_ROOT/tests/toolbox_check.py" \
      "$sd/atmosphere/contents/$tid/toolbox.json" "$tid" "$VERSION" \
      --program-id "$npdm_id" ||
    fail "the packaged toolbox.json disagrees with the build it ships beside"

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

# The release's source archive is the complete source (#320): unpacked on its
# own -- no checkout, no .git, no submodule fetch -- it builds both shipped
# binaries in the pinned image with the network cut off. The portlibs and libnx
# come from that image, which is the one thing a rebuilder brings.
phase_source_builds() {
  local missing
  missing="$(toolchain_missing)" && skip "$missing"
  [ -f "$REPO_ROOT/overlay/lib/libultrahand/ultrahand.mk" ] ||
    skip "overlay/lib/libultrahand is empty (git submodule update --init --recursive)"

  SCRATCH="$(mktemp -d)"
  "$REPO_ROOT/scripts/source-archive.sh" --out "$SCRATCH/dist" >/dev/null 2>"$SCRATCH/err" ||
    { cat "$SCRATCH/err" >&2; fail "source-archive.sh failed"; }
  local tree="$SCRATCH/src"
  mkdir -p "$tree"
  tar -xzf "$SCRATCH/dist/rommsync-nx-$VERSION-source.tar.gz" -C "$tree" ||
    fail "cannot unpack the source archive"
  tree="$tree/rommsync-nx-$VERSION"
  [ -d "$tree" ] || fail "the archive has no rommsync-nx-$VERSION/ at its top"

  local log="$SCRATCH/build.log"
  if ! TOOLCHAIN_NETWORK=none toolchain_run "$tree" \
         'make -C sysmodule -j"$(nproc)" && make -C overlay -j"$(nproc)"' >"$log" 2>&1; then
    tail -n 60 "$log" >&2
    fail "the source archive does not build both binaries offline"
  fi
  "${PYTHON:-python3}" "$REPO_ROOT/scripts/artifact-check.py" container \
      "$tree/sysmodule/sys-rommsync.nsp" "$tree/overlay/ovl-rommsync.ovl" ||
    fail "the source archive built something that is not a loadable .nsp and .ovl"

  echo "ok: the source archive alone builds sys-rommsync.nsp and ovl-rommsync.ovl, offline"
}

case "${1:-}" in
  layout)        phase_layout ;;
  toolbox)       phase_toolbox ;;
  refuses)       phase_refuses ;;
  deterministic) phase_deterministic ;;
  upgrade)       phase_upgrade ;;
  notices)       phase_notices ;;
  source)        phase_source ;;
  wording)       phase_wording ;;
  reproducible)  phase_reproducible ;;
  builds)        phase_builds ;;
  source_builds) phase_source_builds ;;
  *) echo "usage: $0 {layout|toolbox|refuses|deterministic|upgrade|notices|source|wording|reproducible|builds|source_builds}" >&2; exit 2 ;;
esac
