#!/usr/bin/env bash
# The body of a GitHub Release, printed on stdout.
#
#   scripts/release-notes.sh [--checksums FILE]
#   scripts/release-notes.sh --prerelease
#
# Three parts, in this order:
#
#   a fixed header   what to download, what to do with it, where the guide is,
#                    and what the build targets. A release page is the first
#                    thing a user meets, and it has to answer "now what?"
#                    without them opening the archive first.
#   the changes      commit subjects since the previous tag. `git describe`
#                    finds that tag, so this needs an unshallow checkout with
#                    tags -- .github/workflows/ci.yml uses fetch-depth: 0.
#   the checksums    whatever SHA256SUMS the job just built and verified,
#                    verbatim. The archive is byte-deterministic for one `zip`
#                    build (scripts/package.sh), so someone rebuilding the tag
#                    can compare against this without downloading a second file.
#
# The version comes from VERSION, read the same way CMakeLists.txt, switch.mk
# and scripts/package.sh read it -- and by the time this runs, `ctest -R
# version.tag` has already refused a tag that disagrees with it, so naming the
# archive after VERSION and the release after the tag cannot produce two
# different numbers on one page.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The compatibility statement a release makes, and the ONLY place it is written
# down -- the notes are the only artifact that carries it, so a second copy
# would have nothing keeping it honest.
#
# Read it as a target, not as a result. Nothing in this repo has run on a
# console: the M8-1 gate is what turns this line into a verified claim, and
# until it passes the sentence below says so in the release itself. The range is
# what the sysmodule is built against -- libnx's service bindings and an
# Atmosphère new enough to load an ExeFS sysmodule out of
# atmosphere/contents/ -- not a set of versions anybody has booted it on.
readonly ATMOSPHERE_TARGET="Atmosphère 1.7.0 or newer (Horizon 18.0.0 and up)"

die() { echo "release-notes.sh: $*" >&2; exit 1; }

usage() {
  cat <<'USAGE'
usage: scripts/release-notes.sh [--checksums FILE]
       scripts/release-notes.sh --prerelease

  --checksums FILE  a SHA256SUMS to quote verbatim (default: none)
  --prerelease      print `true` or `false` for VERSION and exit, writing no
                    notes -- the flag the GitHub Release is created with
USAGE
}

CHECKSUMS=""
PRERELEASE_ONLY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --checksums)  [ $# -ge 2 ] || die "--checksums needs a path"; CHECKSUMS="$2"; shift 2 ;;
    --prerelease) PRERELEASE_ONLY=1; shift ;;
    -h|--help)    usage; exit 0 ;;
    *) usage >&2; die "unknown argument: $1" ;;
  esac
done

VERSION="$(sed -n '1{s/^[[:space:]]*//;s/[[:space:]]*$//;p;}' "$REPO_ROOT/VERSION" \
           2>/dev/null || true)"
[ -n "$VERSION" ] || die "could not read a version from $REPO_ROOT/VERSION"

# Semver: anything after the first `-` is a prerelease identifier. The release
# job asks this script rather than repeating the rule, so `0.2.0-rc1` publishes
# as a prerelease and `0.2.0` does not without the test for it needing a tag,
# a token or a network -- `ctest -R release.prerelease` runs exactly this.
if [ "$PRERELEASE_ONLY" -eq 1 ]; then
  case "$VERSION" in
    *-*) echo "true" ;;
    *)   echo "false" ;;
  esac
  exit 0
fi

TAG="v$VERSION"
ZIP="rommsync-nx-$VERSION.zip"

# Where the project lives. Three sources, in this order, and none of them is a
# URL typed into this file: CI's own environment, then whichever git remote
# yields one, then the URL packaging/README.txt.in already ships inside every
# archive. That last one is the fallback rather than a literal here because a
# second hardcoded copy is one fork away from a release note and a shipped
# README pointing at two different repositories.
#
# A remote whose URL is a path -- a local clone, a bare mirror -- is skipped
# rather than printed: the notes would otherwise carry a link to somebody's home
# directory. Nothing here dies, because a release without notes is worse than a
# release whose link is the project's own front page.
project_url() {
  if [ -n "${GITHUB_SERVER_URL:-}" ] && [ -n "${GITHUB_REPOSITORY:-}" ]; then
    printf '%s/%s\n' "${GITHUB_SERVER_URL%/}" "$GITHUB_REPOSITORY"
    return
  fi

  local remote url
  for remote in $(git -C "$REPO_ROOT" remote 2>/dev/null); do
    url="$(git -C "$REPO_ROOT" config --get "remote.$remote.url" 2>/dev/null || true)"
    # git@host:owner/name.git -> https://host/owner/name
    url="$(sed -e 's#^git@\([^:]*\):#https://\1/#' -e 's#\.git$##' <<<"$url")"
    case "$url" in
      http://*|https://*) printf '%s\n' "$url"; return ;;
    esac
  done

  # The one the archive's own README.txt carries, read out of the template
  # rather than restated.
  url="$(grep -oE 'https://[^ ]*/rommsync-nx' "$REPO_ROOT/packaging/README.txt.in" \
         2>/dev/null | head -n 1 || true)"
  [ -n "$url" ] || die "no git remote, no GITHUB_REPOSITORY, and no URL in packaging/README.txt.in"
  printf '%s\n' "$url"
}

URL="$(project_url)"

# The previous release tag, excluding this one: on a tag push HEAD *is* the tag,
# so `git describe HEAD` would answer with it and the range would be empty.
# Absent -- an unshallow checkout with no earlier tag -- means this is the first
# release, and the whole history is the change list. That is the path this repo
# takes today, and `ctest -R release.notes` is what exercises it.
#
# A STABLE release also skips every prerelease tag. Cutting v0.2.0 after a
# v0.2.0-rc1 would otherwise produce "Changes since v0.2.0-rc1" and a list of
# one commit, silently dropping everything since v0.1.0 -- and the rc is the
# recommended path (docs/DEVELOPMENT.md#releases), so that is the normal case,
# not a corner. A prerelease keeps them: rc2's interesting delta really is
# "since rc1". `ctest -R release.history` is what holds both halves.
RELEASE_EXCLUDES=(--exclude "$TAG")
case "$VERSION" in
  *-*) ;;                                        # cutting a prerelease
  *)   RELEASE_EXCLUDES+=(--exclude '*-*') ;;    # cutting a stable one
esac
PREVIOUS="$(git -C "$REPO_ROOT" describe --tags --abbrev=0 \
            "${RELEASE_EXCLUDES[@]}" HEAD 2>/dev/null || true)"

# What produced these bytes. dkp-pacman is how the devkitPro container records
# it, so the line is derived where the release is built and degrades to the
# image name anywhere else, rather than being a number typed here that nothing
# checks.
toolchain() {
  local q
  if q="$(dkp-pacman -Q libnx devkitA64 2>/dev/null)" && [ -n "$q" ]; then
    # `libnx 4.12.0-1\ndevkitA64 r29.2-1` -> `libnx 4.12.0-1, devkitA64 r29.2-1`
    printf '%s\n' "$q" | paste -sd, - | sed 's/,/, /g'
    return
  fi
  echo "devkitpro/devkita64 (versions not recorded)"
}

cat <<EOF
## rommsync-nx $VERSION

Download **\`$ZIP\`** below and unzip it onto the **root** of your SD card.
It merges with what is already there; it does not replace your
\`config.ini\`, your pairing or your sync state.

The sysmodule ships **disabled** — nothing runs until you enable it with
ovl-sysmodules. \`README.txt\` inside the archive is the short version; the full
guide is here:

  $URL#readme

Built with $(toolchain), for $ATMOSPHERE_TARGET.

> Not yet verified on hardware. Every guarantee this build carries was proven
> against a real RomM server and a cross-compile, never on a console — see the
> M8-1 gate before trusting it with a save you care about.

EOF

if [ -n "$PREVIOUS" ]; then
  echo "## Changes since $PREVIOUS"
  RANGE="$PREVIOUS..HEAD"
else
  echo "## Changes"
  echo
  echo "The first release: everything up to \`$TAG\`."
  RANGE="HEAD"
fi
echo

# `%s (%h)` and nothing else: a release note is read, not replayed, and a body
# is a paragraph nobody wants a hundred of. --no-merges because a squash-merge
# repo has none worth listing and a rebase one lists the same work twice.
#
# Captured rather than streamed, so a failure here is a failure of git and not
# of whatever this was piped into: `| head` closes the pipe, git dies of
# SIGPIPE, and a streamed version would report that as an unreadable history.
CHANGES="$(git -C "$REPO_ROOT" log --no-merges --pretty=format:'- %s (%h)' "$RANGE")" ||
  die "could not read the history for $RANGE"
# A tag on the same commit as the last one. Saying so beats an empty section,
# which reads as a generator that failed quietly.
[ -n "$CHANGES" ] || CHANGES="- No commits since ${PREVIOUS:-the previous release}."
printf '%s\n' "$CHANGES"

if [ -n "$CHECKSUMS" ]; then
  [ -f "$CHECKSUMS" ] || die "no checksums file at $CHECKSUMS"
  echo
  echo "## Checksums"
  echo
  echo '```'
  cat "$CHECKSUMS"
  echo '```'
fi
