#!/usr/bin/env bash
# The complete source of a release: dist/rommsync-nx-<VERSION>-source.tar.gz.
#
#   scripts/source-archive.sh [--out DIR] [--rev REV]
#
# Why it exists (#320): ovl-rommsync.ovl is GPL-2.0 (docs/LICENSING.md), and
# section 3 asks that its complete source be offered from the same place as the
# binary. The tag alone does not do that: libultrahand is a submodule, and
# GitHub's automatic "Source code" archives hold it as an empty directory. This
# is the tree at REV with every submodule's tree, at the commit REV's gitlink
# names, inside it -- what a rebuilder needs, beside the pinned devkitPro image
# (scripts/toolchain.sh), to build both binaries with no network
# (`ctest -R package.source_builds`).
#
# Read from git objects, never from the work tree: an uncommitted edit, a build
# directory or a submodule checked out at some other commit cannot reach it.
# Deterministic like the zip: git archive stamps every entry with the commit's
# own time, and the gzip header carries no time or name -- so the same commit
# gives the same bytes for one Python and zlib, the caveat the zip has for one
# `zip` build: the published checksum is a statement about the release job's.
#
# Needs git and python3 (the merge of the per-repository tars, which no tar on
# both GNU and BSD does the same way). Both are in the devkitPro image the
# release job runs in.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

die() { echo "source-archive.sh: $*" >&2; exit 1; }

OUT="$REPO_ROOT/dist"
REV=HEAD

while [ $# -gt 0 ]; do
  case "$1" in
    --out) [ $# -ge 2 ] || die "--out needs a directory"; OUT="$2"; shift 2 ;;
    --rev) [ $# -ge 2 ] || die "--rev needs a revision"; REV="$2"; shift 2 ;;
    -h|--help) echo "usage: scripts/source-archive.sh [--out DIR] [--rev REV]"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

# Pinned so a contributor's git config cannot change the bytes: tar.umask sets
# every entry's mode, and autocrlf would rewrite line endings on the way out.
git_pinned() { git -c tar.umask=0002 -c core.autocrlf=false "$@"; }

COMMIT="$(git_pinned -C "$REPO_ROOT" rev-parse --verify --quiet "$REV^{commit}")" ||
  die "$REV is not a commit in $REPO_ROOT"
# VERSION as the archived commit has it, so the name and the contents agree.
VERSION="$(git_pinned -C "$REPO_ROOT" show "$COMMIT:VERSION" 2>/dev/null |
           sed -n '1{s/^[[:space:]]*//;s/[[:space:]]*$//;p;}')"
[ -n "$VERSION" ] || die "no VERSION at $REV"

TOP="rommsync-nx-$VERSION"
NAME="$TOP-source.tar.gz"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
TARS=()

# add_tree DIR COMMIT PATH -- DIR's tree at COMMIT under $TOP/PATH, then each
# gitlink in it, recursively. Gitlinks are read off the tree, not .gitmodules:
# the tree is what a checkout of the tag would fetch, at the commit it names.
add_tree() {
  local dir="$1" commit="$2" path="$3" tar
  tar="$WORK/${#TARS[@]}.tar"
  git_pinned -C "$dir" archive --format=tar --prefix="$TOP/$path" "$commit" > "$tar" ||
    die "git archive of ${path:-the repository} failed"
  TARS+=("$tar")

  # -z: without it git quotes a path holding unusual bytes, and the quoted
  # spelling names no directory.
  local entry mode type sha sub
  while IFS= read -r -d '' entry; do
    mode="${entry%%$'\t'*}" sub="${entry#*$'\t'}"
    read -r _ type sha <<<"$mode"
    [ "$type" = commit ] || continue
    # A submodule that is not checked out leaves an empty directory, and git -C
    # on it would answer for the parent repository -- the empty directory
    # GitHub's archive ships, by another road. Its own top level, and the
    # pinned commit inside it, or no archive.
    [ -d "$dir/$sub" ] &&
      [ "$(cd "$dir/$sub" && git rev-parse --show-toplevel 2>/dev/null)" = "$(cd "$dir/$sub" && pwd -P)" ] &&
      git_pinned -C "$dir/$sub" cat-file -e "$sha^{commit}" 2>/dev/null ||
      die "submodule $path$sub is not checked out with $sha (git submodule update --init --recursive)"
    add_tree "$dir/$sub" "$sha" "$path$sub/"
  done < <(git_pinned -C "$dir" ls-tree -r -z "$commit")
}

add_tree "$REPO_ROOT" "$COMMIT" ""

# One tar out of several, then gzip with no timestamp and no file name. An
# entry already present -- the empty directory the parent records for a
# gitlink -- is kept once. The global header names the commit, the way git
# archive's own does, so `git get-tar-commit-id` still answers.
"${PYTHON:-python3}" - "$WORK/$NAME" "$COMMIT" "${TARS[@]}" <<'PY' || die "merging the tars failed"
import gzip, sys, tarfile
out, commit, inputs = sys.argv[1], sys.argv[2], sys.argv[3:]
seen = set()
with open(out, "wb") as raw, \
     gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz, \
     tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT,
                  pax_headers={"comment": commit}) as tar:
    for path in inputs:
        with tarfile.open(path) as src:
            for member in src:
                if member.name in seen:
                    continue
                seen.add(member.name)
                tar.addfile(member, src.extractfile(member) if member.isfile() else None)
PY

# Written whole and then moved, so a failure above leaves nothing in --out.
mkdir -p "$OUT"
mv -f "$WORK/$NAME" "$OUT/$NAME"
echo "$OUT/$NAME"
