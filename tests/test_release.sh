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
#   test_release.sh history   ...and it picks the right previous tag, against a
#                             throwaway repo with real tags in it. This repo has
#                             none, so `notes` only ever exercises the
#                             first-release fallback; the case that actually
#                             ships -- v0.1.0, some rcs, then v0.2.0 -- can only
#                             be seen here.
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
  local head_subject
  head_subject="$(git -C "$REPO_ROOT" log -1 --pretty=format:%s 2>/dev/null)"
  if [ -n "$head_subject" ]; then
    grep -qF "$head_subject" "$body" ||
      fail "the notes do not list the commit at HEAD ($head_subject)"
  fi

  echo "ok: the notes name the archive, the guide, the target and the checksums"
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

case "${1:-}" in
  version)    phase_version ;;
  ci)         phase_ci ;;
  notes)      phase_notes ;;
  prerelease) phase_prerelease ;;
  history)    phase_history ;;
  *)          echo "usage: $0 {version|ci|notes|prerelease|history}" >&2; exit 2 ;;
esac
