#!/usr/bin/env bash
# Builds the release zip: the two Switch artifacts, renamed and filed where
# Horizon expects them, plus the two files a user reads.
#
#   scripts/package.sh                    -> dist/rommsync-nx-<VERSION>.zip
#   scripts/package.sh --list             -> the entry paths, without packaging
#
# The layout is the whole point of this script. `make -C sysmodule` and
# `make -C overlay` produce sys-rommsync.nsp and ovl-rommsync.ovl, and BOTH of
# those names are wrong where they land:
#
#   atmosphere/contents/<TID>/exefs.nsp   Atmosphere loads `exefs.nsp`. A
#                                         correctly built sys-rommsync.nsp left
#                                         under that name installs cleanly,
#                                         boots, and does nothing at all.
#   switch/.overlays/ovl-rommsync.ovl     ...this one keeps its name, but only
#                                         Ultrahand's directory finds it.
#
# <TID> comes out of sysmodule/sys-rommsync.json rather than being typed here:
# the id is still unconfirmed against the installed homebrew set
# (sysmodule/README.md), so it will move, and a second copy of it is how a move
# lands in one place only.
#
# What is deliberately NOT in the archive:
#
#   flags/boot2.flag   its presence is what makes Atmosphere launch the
#                      sysmodule at boot, and the sysmodule ships DISABLED
#                      (#33, and the first step of the install guide).
#   config.ini         only config.ini.example. An upgrade is this same zip
#                      unpacked over the top, and replacing a user's settings
#                      is not something a release may do.
#   tlsprobe           a manually launched spike, never installed
#                      (tlsprobe/README.md). It stays a CI artifact.
#
# The archive is byte-deterministic: fixed entry order, fixed timestamps, fixed
# permissions, and no extra fields (`zip -X`), so two runs on the same inputs
# produce the same bytes and the checksums #34 publishes mean something. That
# holds for one `zip` build; it is not a claim about two different ones.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The zip epoch. DOS timestamps cannot represent anything earlier, and anything
# later would just be a second arbitrary number.
readonly FIXED_TIMESTAMP="198001010000"

die() { echo "package.sh: $*" >&2; exit 1; }

usage() {
  cat <<'USAGE'
usage: scripts/package.sh [--nsp PATH] [--ovl PATH] [--out DIR] [--list]

  --nsp PATH   the sysmodule build output   (default: sysmodule/sys-rommsync.nsp)
  --ovl PATH   the overlay build output     (default: overlay/ovl-rommsync.ovl)
  --out DIR    where the zip is written     (default: dist/)
  --list       print the archive's entry paths and exit, building nothing
USAGE
}

NSP=""
OVL=""
OUT_DIR=""
LIST_ONLY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --nsp)  [ $# -ge 2 ] || die "--nsp needs a path";  NSP="$2";     shift 2 ;;
    --ovl)  [ $# -ge 2 ] || die "--ovl needs a path";  OVL="$2";     shift 2 ;;
    --out)  [ $# -ge 2 ] || die "--out needs a path";  OUT_DIR="$2"; shift 2 ;;
    --list) LIST_ONLY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown argument: $1" ;;
  esac
done

NSP="${NSP:-$REPO_ROOT/sysmodule/sys-rommsync.nsp}"
OVL="${OVL:-$REPO_ROOT/overlay/ovl-rommsync.ovl}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/dist}"

# --- what the archive is called, and what it installs as ----------------------

# Read the same way CMakeLists.txt and switch.mk read it, so one file is the
# only place a version is written down: the first line, stripped. Stripped and
# not squeezed -- `0.1.0 rc` would otherwise become the filename
# rommsync-nx-0.1.0rc.zip, which is a release nobody asked for and nothing
# reports. It is a filename component, so anything left inside it is refused.
VERSION="$(sed -n '1{s/^[[:space:]]*//;s/[[:space:]]*$//;p;}' "$REPO_ROOT/VERSION" \
           2>/dev/null || true)"
[ -n "$VERSION" ] || die "could not read a version from $REPO_ROOT/VERSION"
case "$VERSION" in
  *[[:space:]]*|*/*) die "version '$VERSION' is not usable in a file name" ;;
esac

# `"title_id"` and not `title_id_range_min`/`_max`, which sit beside it with the
# same value today and are a different setting. Anchored on the quoted key for
# that reason.
CONFIG_JSON="$REPO_ROOT/sysmodule/sys-rommsync.json"
[ -f "$CONFIG_JSON" ] || die "no $CONFIG_JSON to read the title id from"
TITLE_ID="$(sed -n 's/.*"title_id"[[:space:]]*:[[:space:]]*"0[xX]\([0-9A-Fa-f]*\)".*/\1/p' \
            "$CONFIG_JSON" | head -n 1 | tr '[:lower:]' '[:upper:]')"
[ -n "$TITLE_ID" ] || die "no \"title_id\" in $CONFIG_JSON"
# A program id is 64 bits. Anything else is a typo that would install into a
# directory Atmosphere never looks in, which is silent on the console.
case "$TITLE_ID" in
  [0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F]\
[0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F]) ;;
  *) die "title id '$TITLE_ID' is not 16 hex digits" ;;
esac

# The archive's contents, in the order they are written. Explicit rather than
# sorted: the order must not change when the title id does.
NSP_ENTRY="atmosphere/contents/$TITLE_ID/exefs.nsp"
OVL_ENTRY="switch/.overlays/ovl-rommsync.ovl"
CONFIG_ENTRY="config/rommsync/config.ini.example"
ENTRIES=("README.txt" "LICENSE" "$NSP_ENTRY" "$CONFIG_ENTRY" "$OVL_ENTRY")

if [ "$LIST_ONLY" -eq 1 ]; then
  printf '%s\n' "${ENTRIES[@]}"
  exit 0
fi

command -v zip >/dev/null 2>&1 || die "zip is not installed"

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
ZIP="$OUT_DIR/rommsync-nx-$VERSION.zip"

# Before the checks below and not after them, so a refused run leaves no archive
# at all rather than the previous run's. A stale zip under the name this build
# was supposed to write is the worse of the two failures: it is the file a
# release job would pick up, and nothing about it says it is a build old enough
# to predate whatever the refusal was about.
#
# It also means zip never opens an existing archive: it merges into one it can
# read, which would keep an entry a previous layout had.
rm -f "$ZIP"

# --- refuse to ship something that is not what it claims to be ----------------
#
# The same two assertions .github/workflows/ci.yml and tests/test_switch_build.sh
# make, repeated here because this is the last place they can be made: an .ovl
# with no ULTR signature builds, zips, installs, and is simply absent from the
# Ultrahand menu, and a .nsp that is not a PFS0 archive is a sysmodule
# Atmosphere will not load. Neither says anything on the console.

[ -s "$NSP" ] || die "no sysmodule build at $NSP (make -C sysmodule)"
[ -s "$OVL" ] || die "no overlay build at $OVL (make -C overlay)"

[ "$(head -c 4 "$NSP")" = "PFS0" ] ||
  die "$NSP is not a PFS0 archive; Atmosphere would not load it"
[ "$(tail -c 4 "$OVL")" = "ULTR" ] ||
  die "$OVL has no ULTR signature; Ultrahand would not list it"

TEMPLATE="$REPO_ROOT/packaging/README.txt.in"
CONFIG_EXAMPLE="$REPO_ROOT/packaging/config.ini.example"
LICENSE="$REPO_ROOT/LICENSE"
for f in "$TEMPLATE" "$CONFIG_EXAMPLE" "$LICENSE"; do
  [ -f "$f" ] || die "missing $f"
done

# --- stage the tree, then zip it ----------------------------------------------

STAGE="$(mktemp -d)"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

mkdir -p "$STAGE/$(dirname "$NSP_ENTRY")" \
         "$STAGE/$(dirname "$OVL_ENTRY")" \
         "$STAGE/$(dirname "$CONFIG_ENTRY")"

cp "$NSP" "$STAGE/$NSP_ENTRY"
cp "$OVL" "$STAGE/$OVL_ENTRY"
cp "$CONFIG_EXAMPLE" "$STAGE/$CONFIG_ENTRY"
cp "$LICENSE" "$STAGE/LICENSE"

# The version and the title id a user reads are the ones the archive was built
# with, substituted the way switch.mk substitutes version.hpp.in.
sed -e "s/@VERSION@/$VERSION/g" -e "s/@TITLE_ID@/$TITLE_ID/g" \
  "$TEMPLATE" > "$STAGE/README.txt"

# Determinism, part two: the mode and the mtime are recorded in the archive, and
# both would otherwise come from whatever the build left behind. Applied to
# every entry, including the ones copied out of the worktree.
( cd "$STAGE" && chmod 644 "${ENTRIES[@]}" && touch -t "$FIXED_TIMESTAMP" "${ENTRIES[@]}" )

# -X drops the extra fields (unix uid/gid, the high-resolution timestamp) that
# would otherwise differ between two runs; -D writes no directory entries, so
# there is nothing in the archive but the five files named above; and the file
# list is given explicitly, so the order is the one above rather than whatever
# the filesystem hands back.
( cd "$STAGE" && zip -X -D -q "$ZIP" "${ENTRIES[@]}" )

# The manifest. docs/INSTALL.md (#35) is checked against these paths rather than
# against a copy of them, so a renamed artifact or a moved title id is a red
# build instead of a wrong instruction.
printf '%s\n' "${ENTRIES[@]}"
echo "packaged $ZIP" >&2
