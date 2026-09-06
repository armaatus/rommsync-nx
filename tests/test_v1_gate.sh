#!/usr/bin/env bash
# The v1 gate's own machinery -- scripts/v1-gate.sh, which is M8-1 (#43).
#
#   test_v1_gate.sh audit BUILD    every row still points at something that
#                                  exists in this build. The entry that goes red
#                                  the day a CTest group is renamed out from
#                                  under a gate row.
#   test_v1_gate.sh rows BUILD     ...and the audit can actually fail: a row
#                                  naming a group nobody registers, a suite row
#                                  naming nothing, a row with a kind that is not
#                                  one of the three.
#   test_v1_gate.sh sites          the save-overwrite census: a new commit site
#                                  in core/, a commit site in a file the census
#                                  never heard of, and a save path whose
#                                  BackUpFirst has gone away, each fail.
#   test_v1_gate.sh evidence BUILD what counts as evidence, driven by recorded
#                                  ctest output: all-green passes, a FAILED test
#                                  fails its row, and -- the one this is really
#                                  for -- a SKIPPED test does not pass it.
#   test_v1_gate.sh release        the release row, against throwaway repos with
#                                  real tags and a stubbed `gh`: no tag, a draft
#                                  release, a release missing an asset, a tag
#                                  that disagrees with VERSION, and the one
#                                  shape that holds.
#   test_v1_gate.sh doc            docs/TESTING.md publishes the same eight rows
#                                  the script evaluates, in the same order.
#
# `audit`, `rows` and `evidence` need a configured build directory to read the
# registered test names out of; the rest read files in the checkout. None of
# them needs Docker, a network or a console -- a gate that can only be exercised
# by walking up to a Switch is a gate nobody has tested.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GATE="$REPO_ROOT/scripts/v1-gate.sh"

fail() { echo "FAIL: $*" >&2; exit 1; }

SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT
scratch() {
  [ -n "$SCRATCH" ] || SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/v1gate.XXXXXX")"
  echo "$SCRATCH"
}

build_dir() {
  local dir="${1:-}"
  [ -n "$dir" ] || fail "this phase needs a build directory as its second argument"
  [ -d "$dir" ] || fail "no build directory at $dir"
  echo "$dir"
}

# A throwaway copy of the gate with a real-enough repository under it: the
# script reads core/src and git, and nothing else, so those are what gets copied.
# Everything mutated by a negative case is mutated here rather than in the
# checkout -- a test that edits core/ to prove a check works is a test that can
# leave the tree wrong.
fake_repo() {
  local root="$1"
  mkdir -p "$root/scripts" "$root/core"
  cp "$GATE" "$root/scripts/v1-gate.sh"
  cp -R "$REPO_ROOT/core/src" "$root/core/src"
  cp "$REPO_ROOT/VERSION" "$root/VERSION"
  chmod +x "$root/scripts/v1-gate.sh"
}

# --- the audit ----------------------------------------------------------------

phase_audit() {
  local build; build="$(build_dir "${1:-}")" || exit 1
  local out
  out="$("$GATE" --audit --build-dir "$build" 2>&1)"
  [ $? -eq 0 ] || fail "the gate does not audit clean against $build:
$out"
  case "$out" in
    *"8 gate rows"*) ;;
    *) fail "expected the audit to report eight rows, got: $out" ;;
  esac
  echo "ok: $out"
}

# --- ...and it can fail -------------------------------------------------------

# Runs the audit against a doctored copy of the script and insists it fails,
# naming what it should have named. A check that cannot fail is not a check, and
# this is the one the whole gate rests on: every other entry here trusts that a
# row pointing at nothing goes red.
audit_must_fail() {
  local what="$1" sed_expr="$2" expect="$3" build="$4"
  local root out status
  root="$(scratch)/$what"
  rm -rf "$root"; fake_repo "$root"
  sed "$sed_expr" "$GATE" > "$root/scripts/v1-gate.sh"
  chmod +x "$root/scripts/v1-gate.sh"
  out="$("$root/scripts/v1-gate.sh" --audit --build-dir "$build" 2>&1)"
  status=$?
  [ "$status" -ne 0 ] || fail "$what: the audit passed a gate it should have refused:
$out"
  case "$out" in
    *"$expect"*) ;;
    *) fail "$what: expected the audit to mention '$expect', got:
$out" ;;
  esac
  echo "ok: $what -> $(echo "$out" | head -1)"
}

phase_rows() {
  local build; build="$(build_dir "${1:-}")" || exit 1

  # A group renamed out from under a row. This is the failure mode the audit
  # exists for: `sync.*` becoming `syncing.*` in some future refactor leaves the
  # gate quietly demonstrating nothing.
  audit_must_fail renamed \
    's|\^harness\\\.conflict\$|^harness\\.conflict_that_nobody_registers$|' \
    "matches no registered test" "$build"

  # A suite row with no evidence at all.
  audit_must_fail empty \
    '/^    ipc)$/,/^      ;;$/{/^\^/d;}' \
    "names no tests" "$build"

  # A kind that is not suite, repo or console -- so a row cannot be smuggled
  # past the audit by inventing a fourth kind that nothing checks.
  audit_must_fail kind \
    's#^ipc|suite|#ipc|nonsense|#' \
    "unknown kind" "$build"

  # A console row that does not say what would settle it, which is the only
  # thing that makes a console row different from a shrug.
  audit_must_fail silent \
    's|^    media)$|    media_that_says_nothing)|' \
    "does not say what would settle it" "$build"
}

# --- the save-overwrite census ------------------------------------------------

census_must_fail() {
  local what="$1" expect="$2"
  local root="$3" out status
  out="$("$root/scripts/v1-gate.sh" --rows >/dev/null 2>&1; cd "$root" && "$root/scripts/v1-gate.sh" --audit --build-dir "${4:-}" 2>&1)"
  status=$?
  [ "$status" -ne 0 ] || fail "$what: the census accepted core/ as it now is:
$out"
  case "$out" in
    *"$expect"*) ;;
    *) fail "$what: expected '$expect', got:
$out" ;;
  esac
  echo "ok: $what"
}

phase_sites() {
  local build="${1:-}"
  local root out status

  # A second commit onto bytes, added to a file the census already knows. This
  # is what a new save-writing path looks like on the day somebody writes one,
  # and the gate's `backup` row says "every" -- so it has to notice.
  root="$(scratch)/site-added"; rm -rf "$root"; fake_repo "$root"
  cat >> "$root/core/src/state_db.cpp" <<'EOF'
namespace { void ProbeSite() { io::CommitStaged("a", "b"); } }
EOF
  out="$("$root/scripts/v1-gate.sh" --audit ${build:+--build-dir "$build"} 2>&1)"
  [ $? -ne 0 ] || fail "a new commit site in state_db.cpp did not fail the census:
$out"
  case "$out" in
    *"census says"*) ;;
    *) fail "expected the census to name the count it expected, got:
$out" ;;
  esac
  echo "ok: a new commit site in a censused file fails"

  # A commit onto bytes in a file the census has never heard of -- the loop over
  # the census cannot see this one, so there is a second pass for it.
  root="$(scratch)/site-new-file"; rm -rf "$root"; fake_repo "$root"
  cat > "$root/core/src/probe_new_path.cpp" <<'EOF'
#include "rommsync/atomic_file.hpp"
namespace { void ProbeSite() { io::CommitStaged("a", "b"); } }
EOF
  out="$("$root/scripts/v1-gate.sh" --audit ${build:+--build-dir "$build"} 2>&1)"
  [ $? -ne 0 ] || fail "a commit site in a brand new file did not fail the census:
$out"
  case "$out" in
    *"is not in the census"*) ;;
    *) fail "expected the census to say the file is unknown to it, got:
$out" ;;
  esac
  echo "ok: a commit site in an uncensused file fails"

  # And the half that matters most: a save path that stopped backing up first.
  # Deleting the call leaves a file that still compiles and still commits.
  root="$(scratch)/no-backup"; rm -rf "$root"; fake_repo "$root"
  sed -i.bak 's/BackUpFirst(/BackUpLater(/g' "$root/core/src/sync_execute.cpp"
  rm -f "$root/core/src/sync_execute.cpp.bak"
  out="$("$root/scripts/v1-gate.sh" --audit ${build:+--build-dir "$build"} 2>&1)"
  [ $? -ne 0 ] || fail "a save path with no BackUpFirst passed the census:
$out"
  case "$out" in
    *"without a BackUpFirst ahead of it"*) ;;
    *) fail "expected the census to name the missing backup, got:
$out" ;;
  esac
  echo "ok: a save commit with no backup ahead of it fails"

  # The tree as it stands is the positive case, and it is checked last so a
  # green here cannot be an artefact of the copies above.
  out="$("$GATE" --audit ${build:+--build-dir "$build"} 2>&1)"
  [ $? -eq 0 ] || fail "the census refuses core/ as it is:
$out"
  echo "ok: core/ as it stands is censused and both save paths back up first"
}

# --- what counts as evidence --------------------------------------------------

# A recorded ctest run, in the format ctest prints. `status` is what the line
# should read: Passed, ***Skipped or ***Failed.
transcript_line() {
  printf '%3d/303 Test #%3d: %s %s%s    0.01 sec\n' "$2" "$2" "$1" \
    "$(printf '%.0s.' $(seq 1 $((40 - ${#1}))))" "$3"
}

# Every test the gate names, recorded as Passed, with `override` (a name and a
# status) applied to one of them.
transcript() {
  local build="$1" override_name="${2:-}" override_status="${3:-}"
  local n=0 name
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    n=$((n + 1))
    if [ "$name" = "$override_name" ]; then
      transcript_line "$name" "$n" "$override_status"
    else
      transcript_line "$name" "$n" "   Passed"
    fi
  done <<EOF
$(ctest --test-dir "$build" -N 2>/dev/null | sed -n 's/^ *Test *#[0-9]*: //p')
EOF
}

phase_evidence() {
  local build; build="$(build_dir "${1:-}")" || exit 1
  local dir out status
  dir="$(scratch)"

  # 1. Everything green. The four suite rows pass; the gate as a whole does not,
  #    because two rows are held on a console and one on a release that does not
  #    exist -- which is the point of exit code 3 and 1 being different.
  transcript "$build" > "$dir/all-green"
  out="$(ROMMSYNC_GATE_TRANSCRIPT="$dir/all-green" "$GATE" --build-dir "$build" 2>&1)"
  status=$?
  for row in sync downloads auth ipc backup; do
    echo "$out" | grep -q "^\[PASS\] $row " \
      || fail "with an all-green run, row '$row' is not PASS:
$out"
  done
  echo "$out" | grep -q "^\[HELD\] ssl " || fail "the ssl row is not held:
$out"
  echo "$out" | grep -q "^\[HELD\] media " || fail "the media row is not held:
$out"
  [ "$status" -ne 0 ] || fail "the gate reported a pass while two rows are held"
  echo "ok: an all-green run passes the six rows a laptop can decide, and no more"

  # 2. A failure inside a row fails that row.
  transcript "$build" "execute.conflict" "***Failed" > "$dir/one-red"
  out="$(ROMMSYNC_GATE_TRANSCRIPT="$dir/one-red" "$GATE" --build-dir "$build" 2>&1)"
  status=$?
  echo "$out" | grep -q "^\[FAIL\] sync " \
    || fail "a failing execute.conflict did not fail the sync row:
$out"
  [ "$status" -eq 1 ] || fail "expected exit 1 for a failing row, got $status"
  echo "ok: a failing test fails the row that named it"

  # 3. The one this file is really for. `rig.smoke` skips whenever RomM is not
  #    running, and most of this gate is written against RomM -- so a run that
  #    skipped is a run that demonstrated nothing, and must not read as a pass.
  #    `execute.conflict` is used here because it is a rig test in a gate row.
  transcript "$build" "execute.conflict" "***Skipped" > "$dir/one-skip"
  out="$(ROMMSYNC_GATE_TRANSCRIPT="$dir/one-skip" "$GATE" --build-dir "$build" 2>&1)"
  status=$?
  echo "$out" | grep -q "^\[HELD\] sync " \
    || fail "a skipped execute.conflict left the sync row reading as decided:
$out"
  case "$out" in
    *"a skip is not a pass"*) ;;
    *) fail "the gate did not say why a skipped row is held:
$out" ;;
  esac
  [ "$status" -ne 0 ] || fail "a run with a skipped test reported the gate as passing"
  echo "ok: a skipped test holds its row instead of passing it"

  # 4. --dry decides nothing, and says so rather than printing four PASSes for
  #    tests it never ran.
  out="$("$GATE" --dry --build-dir "$build" 2>&1)"
  echo "$out" | grep -q "^\[PASS\] sync " \
    && fail "--dry reported a suite row as passing without running it:
$out"
  case "$out" in
    *"--dry reads the gate"*) ;;
    *) fail "--dry did not say that it decides nothing:
$out" ;;
  esac
  echo "ok: --dry reads the gate and does not decide it"
}

# --- the release row ----------------------------------------------------------

# A throwaway repository with real tags in it, and a `gh` on PATH that answers
# whatever this test wants it to. The real repo has no tags at all, so every
# branch below except the first is unreachable from the checkout -- and the
# branch that actually ships is the one that says a release is published.
release_repo() {
  local root="$1" version="$2" tag="$3" gh_json="$4"
  rm -rf "$root"; fake_repo "$root"
  printf '%s\n' "$version" > "$root/VERSION"
  mkdir -p "$root/bin"
  # A stub for the release row's only network call. "REFUSE" is the stub that
  # answers nothing, which is what an absent or offline gh looks like from here.
  if [ "$gh_json" = "REFUSE" ]; then
    printf '#!/bin/sh\nexit 1\n' > "$root/bin/gh"
    chmod +x "$root/bin/gh"
  elif [ -n "$gh_json" ]; then
    cat > "$root/bin/gh" <<EOF
#!/bin/sh
# a stub: the release row's only network call, answered from a fixture
[ "\$1" = "release" ] || exit 1
printf '%s\n' '$gh_json'
EOF
    chmod +x "$root/bin/gh"
  fi
  git -C "$root" init --quiet
  git -C "$root" symbolic-ref HEAD refs/heads/main
  git -C "$root" -c user.email=t@example.invalid -c user.name=t \
      add -A >/dev/null 2>&1
  git -C "$root" -c user.email=t@example.invalid -c user.name=t \
      commit --quiet -m "fixture" >/dev/null 2>&1
  [ -n "$tag" ] && git -C "$root" tag "$tag"
  return 0
}

release_says() {
  local root="$1" expect="$2" want_status="$3" what="$4" build="$5"
  local out status
  # The stub goes first on PATH; when there is no stub the row reports the fact
  # that it could not ask, which is its offline answer and also a failing one.
  # --dry, because this row is about the repository and not about the suite --
  # and the build directory is the real one, since the throwaway repo has none.
  out="$(cd "$root" && PATH="$root/bin:$PATH" \
         "$root/scripts/v1-gate.sh" --dry --build-dir "$build" 2>&1)"
  status=$?
  echo "$out" | grep -q "^\[$want_status\] release " \
    || fail "$what: expected the release row to be $want_status:
$(echo "$out" | grep -A6 'release ')"
  case "$out" in
    *"$expect"*) ;;
    *) fail "$what: expected '$expect', got:
$(echo "$out" | grep -A8 'release ')" ;;
  esac
  echo "ok: $what"
}

phase_release() {
  local build; build="$(build_dir "${1:-}")" || exit 1
  local dir; dir="$(scratch)"
  local published='{"isDraft":false,"assets":[{"name":"rommsync-nx-1.0.0.zip"},{"name":"SHA256SUMS"}]}'

  # Today's repository: no tag at all. This is the branch that runs in the real
  # checkout, so it is the one the PR body quotes.
  local out
  out="$("$GATE" --dry --build-dir "$build" 2>&1)"
  echo "$out" | grep -q "^\[FAIL\] release " \
    || fail "with no v1 tag, the release row is not failing:
$out"
  case "$out" in
    *"no v1 tag in this repository"*) ;;
    *) fail "the release row did not say why it fails:
$out" ;;
  esac
  echo "ok: an untagged repository fails the release row"

  release_repo "$dir/published" 1.0.0 v1.0.0 "$published"
  release_says "$dir/published" "is published, on main, and agrees with VERSION" PASS \
    "a v1 tag on main with a published release carrying both assets holds" "$build"

  release_repo "$dir/draft" 1.0.0 v1.0.0 \
    '{"isDraft":true,"assets":[{"name":"rommsync-nx-1.0.0.zip"},{"name":"SHA256SUMS"}]}'
  release_says "$dir/draft" "still a DRAFT" FAIL \
    "a draft release is not a released build" "$build"

  release_repo "$dir/no-sums" 1.0.0 v1.0.0 \
    '{"isDraft":false,"assets":[{"name":"rommsync-nx-1.0.0.zip"}]}'
  release_says "$dir/no-sums" "carries no SHA256SUMS" FAIL \
    "a release with no checksums is not verifiable" "$build"

  release_repo "$dir/disagrees" 1.1.0 v1.0.0 "$published"
  release_says "$dir/disagrees" "disagrees with VERSION" FAIL \
    "a tag that disagrees with VERSION fails, as version.tag would on the push" "$build"

  # A `gh` that cannot answer -- no network, no remote, not installed. All of
  # those leave the same thing unknown, and an unknown is not a pass.
  release_repo "$dir/no-gh" 1.0.0 v1.0.0 "REFUSE"
  release_says "$dir/no-gh" "is visible from here" FAIL \
    "a tag whose release cannot be seen is not a released build" "$build"

  # A 0.x tag is not a v1 build, however published it is.
  release_repo "$dir/zero" 0.9.0 v0.9.0 "$published"
  release_says "$dir/zero" "no v1 tag in this repository" FAIL \
    "a 0.x tag does not satisfy a row that says v1" "$build"
}

# --- the published table ------------------------------------------------------

phase_doc() {
  local doc="$REPO_ROOT/docs/TESTING.md"
  [ -f "$doc" ] || fail "no $doc"

  # docs/TESTING.md carries the gate as a table, the way it carries the M0 exit
  # gate. Two copies of eight rows drift; this is what stops them. The doc rows
  # are the leading `| `id` |` cells of the table under the v1 gate heading.
  local doc_ids script_ids
  doc_ids="$(awk '/^## Rung 3/,/^## What can/' "$doc" \
             | sed -n 's/^| `\([a-z]*\)` *|.*/\1/p')"
  script_ids="$("$GATE" --rows | cut -d'|' -f1)"
  [ -n "$doc_ids" ] || fail "docs/TESTING.md publishes no gate table under Rung 3"
  if [ "$doc_ids" != "$script_ids" ]; then
    fail "docs/TESTING.md and scripts/v1-gate.sh disagree about the rows.
doc:
$doc_ids
script:
$script_ids"
  fi

  # ...and the claims, not only the ids: a table that renamed a row's meaning
  # while keeping its id would pass the check above.
  local id claim
  while IFS='|' read -r id _ claim; do
    [ -n "$id" ] || continue
    # The first few words are enough, and are what survives a table cell being
    # wrapped differently from the script's line.
    local head; head="$(echo "$claim" | cut -c1-28)"
    grep -qF "$head" "$doc" \
      || fail "docs/TESTING.md does not carry row '$id' -- looked for: $head"
  done <<EOF
$("$GATE" --rows)
EOF
  echo "ok: docs/TESTING.md publishes the same eight rows the gate evaluates"
}

case "${1:-}" in
  audit)    phase_audit "${2:-}" ;;
  rows)     phase_rows "${2:-}" ;;
  sites)    phase_sites "${2:-}" ;;
  evidence) phase_evidence "${2:-}" ;;
  release)  phase_release "${2:-}" ;;
  doc)      phase_doc ;;
  *) echo "usage: test_v1_gate.sh {audit|rows|sites|evidence|release|doc} [BUILD_DIR]" >&2; exit 2 ;;
esac
