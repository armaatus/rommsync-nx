#!/usr/bin/env bash
# THIRD_PARTY_NOTICES.txt, generated from packaging/third_party.manifest (#300).
#
#   scripts/notices.sh                         -> the notices, on stdout
#   scripts/notices.sh --check                 -> refuse a linked library with no row
#   scripts/notices.sh --check --map overlay=overlay/build/ovl-rommsync.map ...
#
# Why it exists: ovl-rommsync.ovl statically links libultrahand (GPL-2.0) and,
# through it, Mbed TLS, curl and zlib; both binaries link libnx, newlib and the
# GCC runtime. Shipping those without their notices breaks the attribution
# clauses of the permissive ones, and shipping the overlay without the GPL text
# and a pointer to its source breaks GPL-2.0 sections 1 and 3. docs/LICENSING.md
# has the reasoning; this script is what keeps the zip agreeing with it.
#
# --check is the half that matters over time. A notices file is only as good as
# the list it was made from, and the way that list goes stale is somebody adding
# `-lfoo` to switch.mk. So the check reads what is actually linked -- switch.mk's
# and the two targets' -l flags, .gitmodules' submodules and, when given, the
# linker's own map files, which also name what the specs file pulls in without
# any -l (libc, libstdc++, libgcc) -- and fails on anything without a row.
#
# Bash and awk only: package.sh runs this inside the devkitPro image, which is
# not a place to assume a Python.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

die() { echo "notices.sh: $*" >&2; exit 1; }

MANIFEST="$REPO_ROOT/packaging/third_party.manifest"
TEXTS="$REPO_ROOT/packaging/licenses"
CHECK=0
MAPS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --manifest) [ $# -ge 2 ] || die "--manifest needs a path"; MANIFEST="$2"; shift 2 ;;
    --check)    CHECK=1; shift ;;
    --map)      [ $# -ge 2 ] || die "--map needs BINARY=PATH"; MAPS+=("$2"); shift 2 ;;
    -h|--help)
      echo "usage: scripts/notices.sh [--manifest PATH] [--check [--map BINARY=PATH]...]"
      exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[ -f "$MANIFEST" ] || die "no manifest at $MANIFEST"

# The manifest's multi-valued fields are split on spaces below, unquoted on
# purpose; with globbing off a token can never expand against the directory.
set -f

# --- the manifest ---------------------------------------------------------------

NAMES=() VERSIONS=() LICENSES=() BINARIES=() PROVIDES=() SOURCES=() FILES=()

trim() { local s="$1"; s="${s#"${s%%[![:space:]]*}"}"; printf '%s' "${s%"${s##*[![:space:]]}"}"; }

lineno=0
while IFS= read -r line || [ -n "$line" ]; do
  lineno=$((lineno + 1))
  case "$(trim "$line")" in ''|'#'*) continue ;; esac
  IFS='|' read -r -a f <<<"$line"
  [ "${#f[@]}" -eq 7 ] ||
    die "$MANIFEST:$lineno: expected 7 '|'-separated fields, found ${#f[@]}"
  for i in 0 1 2 3 4 5 6; do
    f[i]="$(trim "${f[i]}")"
    [ -n "${f[i]}" ] || die "$MANIFEST:$lineno: field $((i + 1)) is empty"
  done
  for b in ${f[3]}; do
    case "$b" in sysmodule|overlay) ;; *)
      die "$MANIFEST:$lineno: '$b' is not a shipped binary (sysmodule, overlay)" ;;
    esac
  done
  # A row whose licence text is missing would generate a notice that names a
  # licence and then does not carry it -- the exact gap this file closes.
  [ -f "$TEXTS/${f[6]}" ] || die "$MANIFEST:$lineno: no packaging/licenses/${f[6]}"
  NAMES+=("${f[0]}") VERSIONS+=("${f[1]}") LICENSES+=("${f[2]}")
  BINARIES+=("${f[3]}") PROVIDES+=("${f[4]}") SOURCES+=("${f[5]}") FILES+=("${f[6]}")
done < "$MANIFEST"

[ "${#NAMES[@]}" -gt 0 ] || die "$MANIFEST has no rows"

# row_for TOKEN -> the index of the row providing it, or nothing.
row_for() {
  local i p
  for i in "${!NAMES[@]}"; do
    for p in ${PROVIDES[i]}; do
      [ "$p" = "$1" ] && { echo "$i"; return 0; }
    done
  done
  return 1
}

# --- the check ------------------------------------------------------------------

if [ "$CHECK" -eq 1 ]; then
  missing=()

  # tlsprobe is deliberately not read: it is a spike that is never installed
  # (tlsprobe/README.md), so nothing it links is distributed.
  for mk in "$REPO_ROOT/switch.mk" "$REPO_ROOT/sysmodule/Makefile" "$REPO_ROOT/overlay/Makefile"; do
    [ -f "$mk" ] || continue
    # Captured first, so a failing awk stops the script under set -e instead of
    # producing an empty list -- and a vacuous "every library has a row".
    libs="$(awk '/^[[:space:]]*LIBS[[:space:]]*[:+]?=/ {
                   for (i = 1; i <= NF; i++) if ($i ~ /^-l/) print substr($i, 3) }' "$mk")"
    for lib in $libs; do
      row_for "lib:$lib" >/dev/null ||
        missing+=("-l$lib in ${mk#"$REPO_ROOT/"}")
    done
  done

  if [ -f "$REPO_ROOT/.gitmodules" ]; then
    subs="$(awk '$1 == "path" && $2 == "=" { print $3 }' "$REPO_ROOT/.gitmodules")"
    for sub in $subs; do
      row_for "submodule:$sub" >/dev/null || missing+=("submodule $sub in .gitmodules")
    done
  fi

  # What the linker actually pulled in, binary by binary. The map names every
  # archive it loaded, including the ones the specs file adds behind -l's back,
  # and the row must also say the component ships in *that* binary: a notice
  # that says "overlay only" for something the sysmodule links is wrong too.
  for spec in "${MAPS[@]+"${MAPS[@]}"}"; do
    bin="${spec%%=*}" map="${spec#*=}"
    case "$bin" in sysmodule|overlay) ;; *) die "--map $spec: not sysmodule= or overlay=" ;; esac
    [ -s "$map" ] || die "--map $spec: no map file at $map"
    # `|| true` on grep alone: a map naming no archive is a valid (if odd) map,
    # while a failing sed or sort still stops the script.
    libs="$({ grep -oE '/lib[A-Za-z0-9_+.-]+\.a' "$map" || true; } |
            sed -e 's|^/lib||' -e 's|\.a$||' | sort -u)"
    for lib in $libs; do
      if i="$(row_for "lib:$lib")"; then
        case " ${BINARIES[i]} " in *" $bin "*) ;; *)
          missing+=("lib$lib.a is in the $bin, and its row (${NAMES[i]}) does not say $bin") ;;
        esac
      else
        missing+=("lib$lib.a in the $bin link map")
      fi
    done
  done

  if [ "${#missing[@]}" -gt 0 ]; then
    echo "notices.sh: linked into a shipped binary with no row in ${MANIFEST#"$REPO_ROOT/"}:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
  fi
  echo "notices.sh: every linked library has a manifest row" >&2
  exit 0
fi

# --- the notices ----------------------------------------------------------------

VERSION="$(sed -n '1{s/^[[:space:]]*//;s/[[:space:]]*$//;p;}' "$REPO_ROOT/VERSION" 2>/dev/null || true)"
[ -n "$VERSION" ] || die "could not read a version from $REPO_ROOT/VERSION"

# The project URL the shipped README.txt already carries, read the way
# scripts/release-notes.sh reads it, so there is one copy of it.
URL="$(grep -oE 'https://[^ ]*/rommsync-nx' "$REPO_ROOT/packaging/README.txt.in" 2>/dev/null |
       head -n 1 || true)"
[ -n "$URL" ] || die "no project URL in packaging/README.txt.in"

rule() { printf '%s\n' "-----------------------------------------------------------------------------"; }

cat <<EOF
=============================================================================
rommsync-nx $VERSION -- licences and third-party notices
=============================================================================

WHICH LICENCE EACH BINARY IS UNDER
----------------------------------

  ovl-rommsync.ovl (the overlay)       GNU GPL version 2. It statically links
                                       libultrahand, which is GPL-2.0, so the
                                       binary as a whole is distributed under
                                       it. The text is COPYING.GPL-2.0, beside
                                       this file.
  exefs.nsp (sys-rommsync, the         MIT, the project's own licence: LICENSE,
  sysmodule)                           beside this file. The only GPL code it
                                       links is the GCC runtime, whose
                                       exception (below) leaves it free.

rommsync-nx's own source code is MIT-licensed in both binaries.


SOURCE CODE
-----------

The complete source of this release, at the tag it was built from, including
the libultrahand commit it pins as a submodule:

  $URL/tree/v$VERSION

Each third-party component below names where the source of the exact version
linked is. That is the offer GPL-2.0 section 3 asks for; it is repeated in the
release notes of every GitHub release.


THIRD-PARTY COMPONENTS
----------------------

EOF

for i in "${!NAMES[@]}"; do
  printf '  %-36s %s\n' "${NAMES[i]}" "${VERSIONS[i]%% *}"
done
echo

for i in "${!NAMES[@]}"; do
  echo
  rule
  echo "${NAMES[i]} ${VERSIONS[i]}"
  echo "  Licence: ${LICENSES[i]}"
  echo "  In:      ${BINARIES[i]}"
  echo "  Source:  ${SOURCES[i]}"
  rule
  echo
  cat "$TEXTS/${FILES[i]}"
done
