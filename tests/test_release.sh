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
#
# All three read files in the checkout. None of them needs Docker, a network or a
# tag, so none of them ever skips -- a release path that is only exercised by
# releasing is a release path nobody has tested.
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

# One named job, from `  <name>:` up to the next key at the same indentation.
# Same shape, and the same reason for it, as switch_build_job() in
# tests/test_switch_build.sh: grepping the whole file would happily match
# another job's step, and so would stopping on a narrower pattern than a job id
# can be.
#
# Scoped to the `jobs:` block first, which switch_build_job() does not need to
# be and this does: `  release:` at two spaces is also how the `on: release:`
# trigger is spelled, so a workflow that declared that trigger and no job would
# otherwise hand back the trigger's own body and every assertion below would be
# read against it.
job_block() {
  awk -v want="  $1:" '
       /^jobs:/ { in_jobs = 1; next }
       in_jobs && /^[^ #]/ { in_jobs = 0 }
       !in_jobs { next }
       $0 == want { in_job = 1; next }
       in_job && /^  [^ #]+:/ { in_job = 0 }
       in_job { print }' "$WORKFLOW"
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
  job="$(job_block release)"
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

  # --- it builds what it ships ---
  grep -q 'container: devkitpro/devkita64' <<<"$job" ||
    fail "the release job does not run in the devkitPro container"
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
  grep -q 'rommsync-nx-' <<<"$job" ||
    fail "the release job never names the versioned archive it uploads"
  # Both assets reach the release, not just the zip.
  grep -q 'releases' <<<"$job" ||
    fail "the release job never posts to the releases API"

  # A tag with a semver prerelease suffix has to publish as a prerelease, and
  # the only place that can be decided is here.
  grep -q 'prerelease' <<<"$job" ||
    fail "the release job never marks a prerelease"

  # --- the gap between tags ---
  #
  # ctest -R package.builds is the only thing that runs scripts/package.sh in the
  # devkitPro container, and it skips on every runner host-tests uses. Without a
  # package step on every push, a failure specific to packaging in that container
  # would first appear on a tag -- the one moment it must not.
  local switch_job
  switch_job="$(job_block switch-build)"
  [ -n "$switch_job" ] || fail "no switch-build job in $WORKFLOW"
  grep -q 'scripts/package.sh' <<<"$switch_job" ||
    fail "switch-build does not package on every push; packaging would first fail on a tag"

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
  local head_subject
  head_subject="$(git -C "$REPO_ROOT" log -1 --pretty=format:%s 2>/dev/null)"
  if [ -n "$head_subject" ]; then
    grep -qF "$head_subject" "$body" ||
      fail "the notes do not list the commit at HEAD ($head_subject)"
  fi

  echo "ok: the notes name the archive, the guide, the target and the checksums"
}

case "${1:-}" in
  version) phase_version ;;
  ci)      phase_ci ;;
  notes)   phase_notes ;;
  *)       echo "usage: $0 {version|ci|notes}" >&2; exit 2 ;;
esac
