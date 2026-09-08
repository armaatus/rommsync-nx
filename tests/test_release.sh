#!/usr/bin/env bash
# Covers the release path: what a `v*` tag has to agree with before it may
# publish, the CI job that turns that tag into a downloadable zip, and the notes
# that go on it.
#
#   test_release.sh version   VERSION is a semver string, and when GITHUB_REF
#                             names a v-tag the two agree. A no-op off a tag,
#                             which is what lets it live in `ctest` rather than
#                             only in the workflow: on a tag push GitHub sets
#                             GITHUB_REF for every step, so the `host-tests` job
#                             runs this without knowing it exists, and the
#                             release job depends on that job.
#   test_release.sh ci        the release job in .github/workflows/ci.yml builds
#                             all three Switch targets, packages, checksums and
#                             uploads both assets, and is gated on the two test
#                             jobs. Also that exactly ONE thing triggers a
#                             release: the workflow shipped `on: release:` with
#                             no job consuming it, so a tag published nothing and
#                             nothing said so.
#   test_release.sh notes     scripts/release-notes.sh produces a body that names
#                             the version, the archive, where the guide is, what
#                             the build targets, and the checksums it was handed.
#   test_release.sh prerelease  ...and it answers `--prerelease` with the semver
#                             rule the release job creates the release by, so an
#                             inverted answer is caught here rather than by a
#                             v0.2.0-rc1 that published as a stable release.
#   test_release.sh compatibility  the one compatibility statement a release
#                             makes is written down ONCE, and the second place
#                             it is quoted -- docs/INSTALL.md, which asks for
#                             exactly this check -- still says the same thing.
#   test_release.sh history   ...and it picks the right previous tag, against a
#                             throwaway repo with real tags in it. This repo has
#                             none, so `notes` only ever exercises the
#                             first-release fallback; the case that actually
#                             ships -- v0.1.0, some rcs, then v0.2.0 -- can only
#                             be seen here.
#   test_release.sh procedure  ...and the procedure docs/DEVELOPMENT.md hands a
#                             human quotes what `merge-gate` will require of the
#                             pull request that procedure opens. The VERSION
#                             bump is an ordinary PR judged by an ordinary gate,
#                             and `gh pr create --fill` does not satisfy it.
#
# Every phase reads files in the checkout. None of them needs Docker, a network
# or a tag, so none of them ever skips -- a release path that is only exercised
# by releasing is a release path nobody has tested.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKFLOW="$REPO_ROOT/.github/workflows/ci.yml"
NOTES="$REPO_ROOT/scripts/release-notes.sh"

fail() { echo "FAIL: $*" >&2; exit 1; }

SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT

# The first line, stripped -- read the way CMakeLists.txt, switch.mk and
# scripts/package.sh read it, so a VERSION they would accept is one this accepts.
read_version() {
  sed -n '1{s/^[[:space:]]*//;s/[[:space:]]*$//;p;}' "$REPO_ROOT/VERSION" 2>/dev/null
}

# --- the tag and VERSION say the same thing -----------------------------------

phase_version() {
  local version
  version="$(read_version)"
  [ -n "$version" ] || fail "could not read a version from $REPO_ROOT/VERSION"

  # Checked on every run, tag or no tag, because two things downstream read this
  # string structurally rather than as an opaque label: the release is marked
  # prerelease when it carries a suffix, and the asset is named after it. A
  # VERSION that is not semver makes both of those mean something else.
  local core="${version%%[-+]*}"
  case "$core" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) fail "VERSION '$version' is not MAJOR.MINOR.PATCH[-prerelease][+build]" ;;
  esac
  case "$core" in
    *[!0-9.]*) fail "VERSION '$version' has a non-numeric component before any suffix" ;;
  esac

  local ref="${GITHUB_REF:-}"
  case "$ref" in
    refs/tags/*) ;;
    *) echo "ok: no tag in GITHUB_REF; VERSION is $version"; return 0 ;;
  esac

  local tag="${ref#refs/tags/}"
  # Only `v*` publishes (the trigger in ci.yml), so only `v*` has to agree. A
  # tag for something else -- a fixture snapshot, someone's bisect marker -- is
  # not a release and is not this test's business.
  case "$tag" in
    v*) ;;
    *) echo "ok: $tag is not a release tag; VERSION is $version"; return 0 ;;
  esac

  # The whole point of the entry. A v0.2.0 tag on a tree whose VERSION says
  # 0.1.0 would publish an archive named rommsync-nx-0.1.0.zip, containing a
  # sysmodule that reports 0.1.0, from a release page titled v0.2.0.
  [ "$tag" = "v$version" ] ||
    fail "the tag $tag disagrees with VERSION ($version); bump VERSION and re-tag"

  echo "ok: $tag and VERSION agree"
}

# --- the job that publishes ----------------------------------------------------

# The release job, from `  release:` up to the next key at the same indentation.
# Same shape, and the same reason for it, as switch_build_job() in
# tests/test_switch_build.sh: grepping the whole file would happily match
# another job's step, and so would stopping on a narrower pattern than a job id
# can be.
#
# One difference, and it is why this is not that function with an argument: this
# is scoped to the `jobs:` block first. `  release:` at two spaces is also how
# the `on: release:` trigger is spelled, so a workflow that declared that
# trigger and no job would otherwise hand back the trigger's own body, and every
# assertion below would be read against it and pass or fail on the wrong text.
release_job() {
  awk '/^jobs:/ { in_jobs = 1; next }
       in_jobs && /^[^ #]/ { in_jobs = 0 }
       !in_jobs { next }
       /^  release:/ { in_job = 1; next }
       in_job && /^  [^ #]+:/ { in_job = 0 }
       in_job { print }' "$WORKFLOW"
}

# One step of that job, by name, up to the next step. Needed because the job has
# two `for asset in ...` loops -- one uploading, one pulling the assets back out
# to verify them -- and an assertion about "the upload names both files" run
# over the whole job is satisfied by the other loop no matter what the upload
# does. Asked for by name so a renamed step is a red test rather than a silently
# vacuous one.
release_step() {
  release_job | awk -v want="      - name: $1" '
       $0 == want { in_step = 1; next }
       in_step && /^      - / { in_step = 0 }
       in_step { print }'
}

# The `on:` block, by the same rule.
on_block() {
  awk '/^on:/ { in_on = 1; next }
       in_on && /^[^ #]/ { in_on = 0 }
       in_on { print }' "$WORKFLOW"
}

phase_ci() {
  [ -f "$WORKFLOW" ] || fail "no $WORKFLOW"

  # --- exactly one thing triggers a release ---
  #
  # The workflow shipped with `on: release: types: [created]` and no job reading
  # it: creating a release ran the build and published nothing, and a tag ran the
  # build and published nothing. Two half-wired paths is how that happens, so
  # this asserts there is one -- without deciding which, since either is a valid
  # answer to the issue as long as the other is gone.
  local on tags_trigger release_trigger
  on="$(on_block)"
  [ -n "$on" ] || fail "no on: block in $WORKFLOW"

  tags_trigger=0
  release_trigger=0
  grep -qE '^ +tags:' <<<"$on" && tags_trigger=1
  grep -qE '^  release:' <<<"$on" && release_trigger=1

  if [ "$tags_trigger" -eq 0 ] && [ "$release_trigger" -eq 0 ]; then
    fail "nothing in on: can start a release: no tag push, no release event"
  fi
  if [ "$tags_trigger" -eq 1 ] && [ "$release_trigger" -eq 1 ]; then
    fail "both a tag push and an on: release hook are wired; pick one"
  fi

  local job
  job="$(release_job)"
  [ -n "$job" ] || fail "no release job in $WORKFLOW"

  if [ "$release_trigger" -eq 1 ]; then
    # Kept only if something actually consumes it. This is the assertion that
    # fails on the dead wiring being left behind.
    grep -q 'github.event.release' <<<"$job" ||
      fail "on: release is declared but the release job never reads github.event.release"
  else
    # A tag trigger means every other push runs this workflow too, so the job
    # has to refuse to publish off one.
    grep -q "refs/tags/" <<<"$job" ||
      fail "the release job does not restrict itself to a tag"
  fi

  # --- gated on the tests ---
  local needs
  needs="$(grep -E '^    needs:' <<<"$job")"
  [ -n "$needs" ] || fail "the release job has no needs:; it would publish untested bytes"
  grep -q 'host-tests' <<<"$needs" || fail "the release job does not depend on host-tests"
  grep -q 'switch-build' <<<"$needs" || fail "the release job does not depend on switch-build"
  grep -q 'static' <<<"$needs" || fail "the release job does not depend on static"

  # ...and on the tag being somewhere a human reviewed. Those three jobs say the
  # commit is good; none of them says it was reviewed, and `git push origin
  # v9.9.9` at any branch head would otherwise publish it.
  grep -q 'merge-base --is-ancestor' <<<"$job" ||
    fail "the release job does not check that the tag is on main"

  # --- it builds what it ships ---
  grep -q 'container: devkitpro/devkita64' <<<"$job" ||
    fail "the release job does not run in the devkitPro container"

  # ...and a container's default shell is `sh -e`, not bash. Seven of this job's
  # twelve run steps open with `set -euo pipefail`, and each died on `set:
  # Illegal option -o pipefail` before running a word of what followed. Nothing
  # found out for as long as the project had no tag: reaching this job needs a
  # `v*` tag whose three dependencies all pass, so the first tag ever cut here
  # is what found it.
  #
  # Comments are stripped before the match on purpose. Grepping the job's whole
  # text would be satisfied by a comment mentioning the setting -- including
  # the one in ci.yml explaining why it is there, which would leave this green
  # over a job that had lost it.
  if grep -q 'set -euo pipefail' <<<"$job"; then
    grep -v '^[[:space:]]*#' <<<"$job" | grep -q '^[[:space:]]*shell: bash' ||
      fail "the release job runs in a container and uses 'set -o pipefail', but never asks for bash -- dash has no pipefail"
  fi

  local target
  for target in sysmodule overlay tlsprobe; do
    grep -q -- "make -C $target" <<<"$job" || fail "the release job does not build $target"
  done

  # A shallow checkout has neither the previous tag nor the commits since it, and
  # the notes would silently come out empty.
  grep -q 'fetch-depth: 0' <<<"$job" ||
    fail "the release job checks out shallow; the release notes need the history"

  # --- and it ships what it built ---
  grep -q 'scripts/package.sh' <<<"$job" ||
    fail "the release job does not run scripts/package.sh"
  grep -q 'SHA256SUMS' <<<"$job" || fail "the release job produces no SHA256SUMS"
  # Beside the build, not somewhere else: the archive is deterministic for one
  # zip build, which is what makes the published checksum a statement about
  # these bytes rather than about whichever host recomputed it.
  grep -qE 'sha256sum -c' <<<"$job" ||
    fail "the release job does not verify SHA256SUMS against the asset it built"
  # ...and against what a user actually gets. `sha256sum -c` run on the file it
  # was computed from seconds earlier proves nothing that survived the upload,
  # and a truncated upload is the one failure that leaves a release page looking
  # completely fine. So the assets have to come back out of the release first.
  grep -q 'releases/assets/' <<<"$job" ||
    fail "the release job never re-reads the published assets to verify them"

  # ...and nothing is visible until that has passed. A release is live the
  # moment it is POSTed, so it is created as a draft and undrafted last; a
  # truncated upload would otherwise leave a public page advertising a download
  # that is corrupt, and the 422 on re-creating a tag's release means recovering
  # from that needs a human.
  grep -q '"draft": True' <<<"$job" ||
    fail "the release is created live, before its assets exist"
  grep -q -- '-X PATCH' <<<"$job" ||
    fail "nothing undrafts the release once its assets are verified"
  grep -q 'releases' <<<"$job" ||
    fail "the release job never posts to the releases API"

  # Both assets, and the upload itself. `grep releases` above is satisfied by the
  # call that CREATES the release, and `grep rommsync-nx-` by the checksum step
  # -- so neither says anything about an asset reaching the release page. Read
  # off the Publish step alone, for the reason release_step() gives.
  local publish
  publish="$(release_step Publish)"
  [ -n "$publish" ] || fail "the release job has no Publish step"
  grep -q -- '--data-binary' <<<"$publish" ||
    fail "the Publish step never streams an asset's bytes to the upload URL"

  local uploads
  uploads="$(grep -E '^ +for asset in ' <<<"$publish")"
  [ -n "$uploads" ] || fail "the Publish step has no asset upload loop"
  grep -q 'rommsync-nx-' <<<"$uploads" ||
    fail "the Publish step does not upload the versioned archive"
  grep -q 'SHA256SUMS' <<<"$uploads" ||
    fail "the Publish step does not upload SHA256SUMS beside the archive"

  # A tag with a semver prerelease suffix has to publish as a prerelease. The
  # rule itself lives in scripts/release-notes.sh, where `release.prerelease`
  # can exercise it against made-up versions -- so what this asserts is that the
  # job ASKS. Anchored on the flag rather than on the bare word, which the
  # comments in this job satisfy on their own.
  grep -q -- '--prerelease' <<<"$job" ||
    fail "the release job never asks release-notes.sh which releases are prereleases"

  # The other half of this -- that `switch-build` packages on every push, so a
  # container-specific packaging failure does not first appear on a tag -- is an
  # assertion about that job, and lives with it in
  # tests/test_switch_build.sh (`switch.ci_requires_artifacts`).

  echo "ok: one trigger, and a gated release job that builds, packages and uploads"
}

# --- the body that goes on the release ----------------------------------------

phase_notes() {
  [ -x "$NOTES" ] || fail "no executable $NOTES"
  bash -n "$NOTES" || fail "$NOTES does not parse"

  local version
  version="$(read_version)"

  SCRATCH="$(mktemp -d)"
  local sums="$SCRATCH/SHA256SUMS"
  # The shape sha256sum writes: digest, two spaces, the name it was given.
  printf '%s  rommsync-nx-%s.zip\n' \
    "0000000000000000000000000000000000000000000000000000000000000000" \
    "$version" > "$sums"

  local body="$SCRATCH/notes.md"
  "$NOTES" --checksums "$sums" > "$body" || fail "release-notes.sh failed"
  [ -s "$body" ] || fail "release-notes.sh produced an empty body"

  # The version, and the file a user is being asked to download. A body naming
  # the wrong archive is a release nobody can follow.
  grep -q "rommsync-nx-$version\.zip" "$body" ||
    fail "the notes do not name rommsync-nx-$version.zip"

  # Where the guide is. packaging/README.txt.in leads with the repo README for
  # the reason this does: docs/INSTALL.md is #35 and may not have landed, and a
  # release note pointing at a 404 is worse than one pointing at the front page.
  grep -q 'github.com/.*rommsync-nx' "$body" ||
    fail "the notes do not link the project"

  # What it was built against, which is the one compatibility statement a
  # release makes -- and it is a statement about a target, not a verified claim:
  # nothing here has run on hardware before M8-1.
  grep -q 'Atmosph' "$body" ||
    fail "the notes do not say which Atmosphere versions this targets"

  # The checksums it was handed, verbatim, so someone rebuilding the tag can
  # compare without downloading a second file.
  grep -q "0000000000000000000000000000000000000000000000000000000000000000" "$body" ||
    fail "the notes do not carry the checksums they were given"

  # An unsubstituted placeholder, the same shape packaging/README.txt.in is
  # checked for.
  grep -qE '@[A-Z_]+@' "$body" && fail "the notes hold an unsubstituted @PLACEHOLDER@"

  # The changes. This repo has no tags yet, so the fallback -- the whole history
  # -- is what runs here, and it must not come out empty either way.
  grep -qi 'change' "$body" || fail "the notes have no changes section"
  # The newest commit the notes are *supposed* to list, which is not always the
  # one at HEAD: `release-notes.sh` passes `--no-merges` on purpose (a
  # squash-merge repo has no merge worth listing), so `--no-merges` here too.
  # Without it this phase went red on any branch that had merged `main` back in
  # -- an ordinary thing to do when a PR falls behind -- by demanding the notes
  # carry the one subject the generator is documented to leave out.
  local head_subject
  head_subject="$(git -C "$REPO_ROOT" log -1 --no-merges --pretty=format:%s 2>/dev/null)"
  if [ -n "$head_subject" ]; then
    grep -qF "$head_subject" "$body" ||
      fail "the notes do not list the newest non-merge commit ($head_subject)"
  fi

  echo "ok: the notes name the archive, the guide, the target and the checksums"
}

# --- the compatibility statement, and its second copy -------------------------

# `ATMOSPHERE_TARGET` is the sentence a release page makes its only
# compatibility claim with, and docs/INSTALL.md restates it as the first line of
# "Before you start" -- a user reading the guide and a user reading the release
# meet the same two version numbers, and there is no third place. INSTALL.md
# named the drift itself ("this line is the second place it is written -- the
# two should be checked against each other rather than left to drift") and
# nothing checked it; this is that check.
#
# It deliberately does NOT assert which versions those are. The numbers are a
# target that M8-2 (#44) confirms or corrects on a console, and a test that
# pinned them would have to be edited by the same commit that corrects them,
# which is not a test. What it pins is that there is one answer and not two.
phase_compatibility() {
  [ -x "$NOTES" ] || fail "no executable $NOTES"

  local target
  target="$(sed -n 's/^readonly ATMOSPHERE_TARGET="\(.*\)"$/\1/p' "$NOTES")"
  [ -n "$target" ] || \
    fail "no ATMOSPHERE_TARGET assignment in $NOTES -- the compatibility line moved"

  local version
  version="$(read_version)"

  SCRATCH="$(mktemp -d)"
  local sums="$SCRATCH/SHA256SUMS"
  printf '%s  rommsync-nx-%s.zip\n' \
    "0000000000000000000000000000000000000000000000000000000000000000" \
    "$version" > "$sums"
  local body="$SCRATCH/notes.md"
  "$NOTES" --checksums "$sums" > "$body" || fail "release-notes.sh failed"

  grep -qF "$target" "$body" || \
    fail "the notes do not carry ATMOSPHERE_TARGET verbatim:
  wanted: $target"
  echo "ok: the release body carries the compatibility line verbatim"

  # INSTALL.md wraps its prose, so the comparison is on the text with runs of
  # whitespace flattened -- a line break between two words is not a difference
  # in what the sentence says, and demanding one file match the other's line
  # width would make this a formatting test.
  local flat_target flat_install
  flat_target="$(printf '%s' "$target" | tr -s '[:space:]' ' ')"
  flat_install="$(tr -s '[:space:]' ' ' < "$REPO_ROOT/docs/INSTALL.md")"
  case "$flat_install" in
    *"$flat_target"*) ;;
    *) fail "docs/INSTALL.md and $NOTES disagree about what this build targets.
  release-notes.sh: $flat_target
  INSTALL.md says something else -- the two are one sentence, in two places, and
  a user meets both. Update the guide in the commit that moves the constant." ;;
  esac
  echo "ok: docs/INSTALL.md restates the same compatibility line"

  # ...and there is no THIRD place. Every version of Atmosphere written down in
  # a tracked file has to be one of those two, or the claim that the constant is
  # the single source is already false and the next correction will miss a copy.
  # That is not a hypothetical: these two drifted for a year with INSTALL.md
  # asking in prose for someone to check them, and #43's body held a third,
  # stale copy the whole time.
  #
  # `git ls-files` is what scopes this to prose this project writes:
  # lib/libultrahand is a submodule, so it comes back as one gitlink and its
  # sources are never read. A vendored library is not making this project's
  # compatibility claim.
  #
  # The pattern is spelt with a character class so this file does not match it.
  # `git grep`, not `ls-files | xargs grep`: it resolves paths against the
  # repository rather than against the caller's cwd, and ctest runs this from
  # build/tests. The xargs form silently found nothing there -- every path
  # missed, and `2>/dev/null` swallowed the errors -- so the assertion passed
  # by looking at no files at all. `-I` skips binaries.
  #
  # The pattern is built from two pieces so this file cannot match itself, and
  # it spells the accented letter as `[^ ]*` rather than a bracket holding a
  # multibyte character: `[eè]` is three BYTES in the C locale, where it cannot
  # match `Atmosphère` at all. That is the same silent pass wearing a different
  # hat, and CI does not promise a UTF-8 locale.
  local mention strays
  mention='Atmosph[^ ]*'
  strays="$(git -C "$REPO_ROOT" grep -lIE "$mention [0-9]" -- . \
            | grep -v -e '^scripts/release-notes\.sh$' -e '^docs/INSTALL\.md$' || true)"
  [ -z "$strays" ] || fail "an Atmosphere version is written down outside the two
places that are allowed to hold one. Point at ATMOSPHERE_TARGET instead of
restating it -- or, if a third place genuinely has to carry the numbers, add it
here deliberately rather than letting the copies drift:
$strays"
  echo "ok: no third copy of the compatibility line"

  # The Horizon number in that sentence is not a number somebody typed: it is
  # the highest firmware version this project's OWN code checks for. Every
  # `hosversionAtLeast` guard marks an optional path -- guarded precisely
  # because the build still works below it -- so the highest of them is the
  # version at and above which every path rommsync-nx can take is live, which
  # is what "targets Horizon X" means for a build that degrades rather than
  # refuses. Derived rather than declared, so a call added against a newer
  # firmware turns this red instead of quietly widening what the release claims.
  #
  # It is a textual match, not a parse: a `hosversionAtLeast(18, 0, 0)` written
  # inside a comment counts as a gate. That is the conservative direction --
  # it asks a question rather than missing one -- and the answer is to write
  # the comment without a literal call in it.
  #
  # sysmodule/source and overlay/source, and NOT overlay/lib/libultrahand: the
  # vendored overlay library carries gates of its own, up to
  # `hosversionAtLeast(21, 0, 0)`, and they are guarded for the same reason
  # these are. Deriving the target from them would raise what a release claims
  # because a UI library gained a progressive-enhancement path, which is not a
  # statement about whether rommsync-nx runs.
  local stated gated
  stated="$(printf '%s' "$target" \
            | sed -n 's/.*Horizon \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')"
  [ -n "$stated" ] || fail "ATMOSPHERE_TARGET names no Horizon version: $target"
  gated="$(grep -rhoE 'hosversionAtLeast\([0-9]+, *[0-9]+, *[0-9]+\)' \
             "$REPO_ROOT/sysmodule/source" "$REPO_ROOT/overlay/source" 2>/dev/null \
           | sed -E 's/.*\(([0-9]+), *([0-9]+), *([0-9]+)\)/\1.\2.\3/' \
           | sort -V | tail -1)"
  [ -n "$gated" ] || fail "nothing in sysmodule/source or overlay/source gates on
a firmware version any more, so the Horizon $stated in ATMOSPHERE_TARGET is
derived from nothing. Say where the number comes from, or drop it from the line."
  [ "$stated" = "$gated" ] || fail "the compatibility line targets Horizon $stated,
but the highest firmware gate in this project's own code is $gated
(hosversionAtLeast, in sysmodule/source or overlay/source). One of the two is
wrong: either the line claims a target nothing needs, or a call was added
against a firmware the release does not say it targets."
  echo "ok: the Horizon target is the highest firmware gate first-party code checks for"
}

# --- which releases are prereleases -------------------------------------------

phase_prerelease() {
  [ -x "$NOTES" ] || fail "no executable $NOTES"

  # Against a copy holding a made-up VERSION, because the answer is a function
  # of that file and this repo holds one version at a time. The script reads
  # $REPO_ROOT/VERSION relative to its own location and `--prerelease` returns
  # before it touches git, so a directory with those two files in it is the
  # whole world it needs.
  SCRATCH="$(mktemp -d)"
  mkdir -p "$SCRATCH/scripts"
  cp "$NOTES" "$SCRATCH/scripts/"

  local version expected answer
  # The suffix forms semver actually produces, and the two that must NOT be
  # read as one: a build identifier after `+` is not a prerelease, and neither
  # is a hyphen-free version however many components it has.
  for version in \
      "0.1.0:false" \
      "1.0.0:false" \
      "10.20.30:false" \
      "0.2.0+build7:false" \
      "0.2.0-rc1:true" \
      "0.2.0-rc.1:true" \
      "1.0.0-alpha:true" \
      "1.0.0-beta.2+build7:true"; do
    expected="${version##*:}"
    echo "${version%%:*}" > "$SCRATCH/VERSION"
    answer="$("$SCRATCH/scripts/release-notes.sh" --prerelease)" ||
      fail "--prerelease failed on ${version%%:*}"
    [ "$answer" = "$expected" ] ||
      fail "${version%%:*} answered $answer, expected $expected"
  done

  echo "ok: a semver suffix is a prerelease and a build identifier is not"
}

# --- which tag the changes are counted from -----------------------------------

# A throwaway repo: one commit per tag, so "which commits are listed" and "which
# tag was chosen" are the same question and the answer is readable.
build_history() {
  local dir="$1"
  mkdir -p "$dir/scripts"
  cp "$NOTES" "$dir/scripts/"
  mkdir -p "$dir/packaging"
  cp "$REPO_ROOT/packaging/README.txt.in" "$dir/packaging/"
  git -C "$dir" init -q
  git -C "$dir" config user.email "test@example.invalid"
  git -C "$dir" config user.name "release test"
  # No remote, so the notes fall back to the URL packaging/README.txt.in ships.
  local step
  for step in "before-0.1.0:v0.1.0" "after-0.1.0:" "the-rc-commit:v0.2.0-rc1" "after-the-rc:"; do
    echo "${step%%:*}" > "$dir/step"
    git -C "$dir" add -A
    git -C "$dir" commit -q -m "${step%%:*}"
    [ -z "${step##*:}" ] || git -C "$dir" tag "${step##*:}"
  done
}

phase_history() {
  command -v git >/dev/null 2>&1 || fail "git is needed to build a history"
  SCRATCH="$(mktemp -d)"
  build_history "$SCRATCH"

  local body="$SCRATCH/notes.md"

  # Cutting a STABLE release. The last tag is v0.2.0-rc1, and counting from it
  # would list one commit and silently drop everything since v0.1.0 -- which is
  # every change the release is actually made of.
  echo "0.2.0" > "$SCRATCH/VERSION"
  "$SCRATCH/scripts/release-notes.sh" > "$body" || fail "release-notes.sh failed on 0.2.0"
  grep -q "Changes since v0.1.0" "$body" ||
    fail "a stable release counts from the last prerelease, not the last release"
  grep -q 'after-0.1.0' "$body" ||
    fail "the commits between v0.1.0 and the rc are missing from the stable notes"
  grep -q 'the-rc-commit' "$body" || fail "the rc's own commit is missing"
  grep -q 'after-the-rc' "$body" || fail "the commits after the rc are missing"

  # Cutting the NEXT prerelease. Here the last rc is the right base: what an
  # rc2 reader wants is what changed since rc1, not since the last stable.
  echo "0.2.0-rc2" > "$SCRATCH/VERSION"
  "$SCRATCH/scripts/release-notes.sh" > "$body" || fail "release-notes.sh failed on 0.2.0-rc2"
  grep -q "Changes since v0.2.0-rc1" "$body" ||
    fail "a prerelease does not count from the previous prerelease"
  grep -q 'after-the-rc' "$body" || fail "the commits since the rc are missing"
  grep -q 'after-0.1.0' "$body" &&
    fail "the prerelease notes reach back past v0.2.0-rc1"

  # And a first release, where there is no previous tag at all. That is the path
  # `notes` takes in this repo, asserted here against a repo whose history is
  # small enough to read.
  local first="$SCRATCH/first"
  mkdir -p "$first"
  cp -R "$SCRATCH/scripts" "$SCRATCH/packaging" "$first/"
  git -C "$first" init -q
  git -C "$first" config user.email "test@example.invalid"
  git -C "$first" config user.name "release test"
  echo "0.1.0" > "$first/VERSION"
  git -C "$first" add -A
  git -C "$first" commit -q -m "the only commit"
  "$first/scripts/release-notes.sh" > "$body" || fail "release-notes.sh failed on a first release"
  grep -q 'The first release' "$body" || fail "a repo with no tags is not treated as a first release"
  grep -q 'the only commit' "$body" || fail "the first release lists no commits"

  echo "ok: a stable counts from the last stable, a prerelease from the last prerelease"
}

# --- the procedure a person follows, and the gate it walks into ---------------

# Cutting a release is the one path here that a PERSON walks end to end, and its
# first step is an ordinary pull request: the `VERSION` bump goes through review
# like every other change -- nothing pushes to `main`, and guard.py will not let
# it. So that pull request meets `merge-gate`, a required check on `main`, and
# `.github/scripts/merge_gate.py` refuses a body that does not show both local
# review passes. It has no exemption for a release and none for its author, so
# the maintainer's bump is judged by exactly the rule an agent's PR is.
#
# `gh pr create --fill` -- which is what this guide's own code block used to
# hand a maintainer -- fills the body from the commit message, and a
# `Release 1.0.0` commit carries neither phrase. The check comes back red on the
# one pull request nobody has ever practised opening, at the moment a release is
# being cut. Documenting the two commands without documenting that is how the
# procedure gets walked for the first time under exactly the wrong conditions.
#
# Two assertions, because the guide has two halves and a maintainer follows the
# second one. The prose has to QUOTE what the gate will demand, read out of the
# gate rather than typed here -- the rule phase_compatibility applies to the
# compatibility line, for the same reason, so renaming a pass in merge_gate.py
# turns this red instead of leaving the guide asking for something the check no
# longer accepts. And the command block may not go back to filling the body from
# the commit message, which a correct bullet three lines above it would not
# stop.
phase_procedure() {
  local gate="$REPO_ROOT/.github/scripts/merge_gate.py"
  local guide="$REPO_ROOT/docs/DEVELOPMENT.md"
  [ -f "$gate" ] || fail "no $gate -- the merge gate moved, and
docs/DEVELOPMENT.md#releases is written against where it used to be"
  [ -f "$guide" ] || fail "no $guide"

  # The needle of each LOCAL_PASSES entry: the first quoted string on a line
  # opening a tuple inside the constant. Read from the constant and not from the
  # whole file, which holds the same phrases a dozen more times in its selftest
  # fixtures -- a match against one of those would say nothing about what the
  # gate requires.
  local needles
  needles="$(sed -n '/^LOCAL_PASSES = (/,/^)/p' "$gate" \
             | sed -n 's/^    ("\([^"]*\)".*/\1/p')"
  [ -n "$needles" ] || fail "no LOCAL_PASSES needles could be read out of $gate.
The constant moved or changed shape, and this phase would otherwise pass by
asserting nothing at all."

  # ...and ALL of them, not just the ones that happen to be shaped the way the
  # sed above expects. A third entry whose needle sat on a continuation line --
  # entry two already wraps its second element, so a formatter reaching the line
  # width is all it would take -- yields no needle and no error, and this phase
  # would go on passing while the guide stayed silent about a pass the gate had
  # started demanding. An empty result is not the only way to assert nothing.
  local declared extracted
  declared="$(sed -n '/^LOCAL_PASSES = (/,/^)/p' "$gate" | grep -c '^    (')"
  extracted="$(printf '%s\n' "$needles" | grep -c .)"
  [ "$declared" = "$extracted" ] || fail "LOCAL_PASSES declares $declared \
entries and only $extracted needle(s) could be read out of them. One of them is
written in a shape this phase cannot see, so the guide is being checked against
some of what the gate requires rather than all of it."

  # Scoped to the section that documents the procedure. `/code-review` somewhere
  # else in the guide -- and it is in CLAUDE.md and in the agent brief already --
  # says nothing about whether a maintainer cutting a release is told about it.
  local section
  section="$(awk '/^## Releases$/ { in_s = 1; next }
                  in_s && /^## / { in_s = 0 }
                  in_s { print }' "$guide")"
  [ -n "$section" ] || fail "no '## Releases' section in $guide"

  local needle
  while IFS= read -r needle; do
    printf '%s\n' "$section" | grep -qF -- "$needle" || fail \
"docs/DEVELOPMENT.md#releases never tells a maintainer that the VERSION bump's
pull request has to show '$needle' in its body. merge_gate.py requires it of
every pull request, with no exemption for a release or for whoever opened it, so
the procedure as written walks into a red required check on the one pull request
that is only ever opened while a release is being cut."
  done <<<"$needles"

  # The other half of it. The bullet above can say everything correct about the
  # gate while the code block three lines up still hands a maintainer the one
  # command that defeats it -- and a maintainer follows the block, not the
  # prose. Scoped to what is inside the fences, and with comments stripped:
  # both the bullet and the block's own annotation name `--fill` deliberately,
  # to say what it does and why it is not there. What may not come back is a
  # command that runs it.
  local commands
  commands="$(printf '%s\n' "$section" \
              | awk '/^```/ { in_b = !in_b; next } in_b' \
              | sed 's/#.*//')"
  [ -n "$commands" ] || fail "the '## Releases' section of $guide has no fenced
command block any more. The assertion below reads it, so this phase would pass
by looking at nothing."
  printf '%s\n' "$commands" | grep -q -- '--fill' && fail \
"the release procedure's command block still runs \`gh pr create --fill\`. That
fills the pull request body from the commit message, so the body carries neither
local review pass and \`merge-gate\` -- a required check on main, with no
exemption for a release -- comes back red on it."

  echo "ok: the release procedure quotes what merge-gate will require of its PR"
}

case "${1:-}" in
  version)    phase_version ;;
  ci)         phase_ci ;;
  notes)      phase_notes ;;
  prerelease) phase_prerelease ;;
  history)    phase_history ;;
  compatibility) phase_compatibility ;;
  procedure)  phase_procedure ;;
  *)          echo "usage: $0 {version|ci|notes|prerelease|history|compatibility|procedure}" >&2
              exit 2 ;;
esac
