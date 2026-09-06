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
# The census reads all three source roots, not just core/, so all three are
# copied -- a fixture missing one would fail for the wrong reason and say so in
# words that look like the finding under test.
fake_repo() {
  local root="$1"
  mkdir -p "$root/scripts" "$root/core" "$root/sysmodule" "$root/overlay"
  cp "$GATE" "$root/scripts/v1-gate.sh"
  cp -R "$REPO_ROOT/core/src" "$root/core/src"
  cp -R "$REPO_ROOT/sysmodule/source" "$root/sysmodule/source"
  cp -R "$REPO_ROOT/overlay/source" "$root/overlay/source"
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

# Deletes exactly one call to BackUpFirst -- never the definition, never a
# mention in a comment -- and insists the census notices. `$expr` is the sed that
# removes the call in `$file`.
no_backup_call_fails() {
  local file="$1" expr="$2" what="$3" build="$4"
  local root out
  root="$(scratch)/no-backup-$(basename "$file" .cpp)"
  rm -rf "$root"; fake_repo "$root"
  sed "$expr" "$root/$file" > "$root/$file.edited"
  mv "$root/$file.edited" "$root/$file"
  grep -q "NoBackupAtAll(" "$root/$file" \
    || fail "$what: the fixture did not remove a call -- '$expr' matched nothing in $file"
  out="$("$root/scripts/v1-gate.sh" --audit --build-dir "$build" 2>&1)"
  [ $? -ne 0 ] || fail "$what: a save path with no call to BackUpFirst passed the census:
$out"
  case "$out" in
    *"no CALL to BackUpFirst ahead of it"*) ;;
    *) fail "$what: expected the census to name the missing backup, got:
$out" ;;
  esac
  echo "ok: removing only the call, in $what, fails the census"
}

phase_sites() {
  local build; build="$(build_dir "${1:-}")" || exit 1
  local root out

  # A second commit onto bytes, added to a file the census already knows. This
  # is what a new save-writing path looks like on the day somebody writes one,
  # and the gate's `backup` row says "every" -- so it has to notice.
  root="$(scratch)/site-added"; rm -rf "$root"; fake_repo "$root"
  cat >> "$root/core/src/state_db.cpp" <<'EOF'
namespace { void ProbeSite() { io::CommitStaged("a", "b"); } }
EOF
  out="$("$root/scripts/v1-gate.sh" --audit --build-dir "$build" 2>&1)"
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
  out="$("$root/scripts/v1-gate.sh" --audit --build-dir "$build" 2>&1)"
  [ $? -ne 0 ] || fail "a commit site in a brand new file did not fail the census:
$out"
  case "$out" in
    *"is not in the census"*) ;;
    *) fail "expected the census to say the file is unknown to it, got:
$out" ;;
  esac
  echo "ok: a commit site in an uncensused file fails"

  # A commit whose line carries a `/` before it -- `dir / name`, which is how
  # anyone writing a path in C++20 writes it. Anchoring the census at the left
  # of the line to skip comments refuses this line too, so a brand new save path
  # written in the obvious style was invisible to the check whose whole job is
  # to say there are only two of them.
  root="$(scratch)/site-slash"; rm -rf "$root"; fake_repo "$root"
  cat > "$root/core/src/probe_slash_path.cpp" <<'EOF'
#include "rommsync/atomic_file.hpp"
namespace {
void ProbeSite(const std::string& dir, const std::string& name) {
  if (auto st = fs::Stat(dir + "/" + name); st.exists) io::CommitStaged(dir, dir + "/" + name);
}
}  // namespace
EOF
  out="$("$root/scripts/v1-gate.sh" --audit --build-dir "$build" 2>&1)"
  [ $? -ne 0 ] || fail "a commit site on a line carrying a slash escaped the census:
$out"
  case "$out" in
    *"is not in the census"*) ;;
    *) fail "expected the census to say the file is unknown to it, got:
$out" ;;
  esac
  echo "ok: a commit site on a line with a slash in it fails"

  # ...while a MENTION in prose is still not a call. The prose is why the census
  # cannot simply grep for the name.
  root="$(scratch)/site-prose"; rm -rf "$root"; fake_repo "$root"
  cat > "$root/core/src/probe_prose.cpp" <<'EOF'
// This file discusses io::CommitStaged( and io::WriteAtomically( at length,
/// including in a doc comment, and calls neither.
namespace { void ProbeSite() {} }  // io::CopyAtomically( is not called here
EOF
  out="$("$root/scripts/v1-gate.sh" --audit --build-dir "$build" 2>&1)"
  [ $? -eq 0 ] || fail "prose naming the helpers was counted as calling them:
$out"
  echo "ok: a file that only talks about the helpers is not a commit site"

  # And the half that matters most: a save path that stopped backing up first.
  # Removing only the CALL leaves a file that still compiles, still commits onto
  # a save, and -- in sync_execute.cpp -- still contains the word BackUpFirst,
  # because that file DEFINES the helper. A check that accepts any occurrence
  # sits green through exactly this edit, which is the one that destroys a save.
  # Both save paths get the case; the definition only exists in one of them.
  no_backup_call_fails core/src/sync_execute.cpp 's/return BackUpFirst(/return NoBackupAtAll(/' \
    "the file that defines the helper" "$build"
  no_backup_call_fails core/src/state_sync.cpp 's/^      BackUpFirst(/      NoBackupAtAll(/' \
    "the save-state path" "$build"
  # M7-1's restore, which lands on a save through CopyAtomically rather than
  # CommitStaged -- the census names the landing call per row for exactly this
  # reason, since a check hardcoded to CommitStaged would not see this path.
  no_backup_call_fails core/src/conflict_record.cpp \
    's/      sync::BackUpFirst(/      sync::NoBackupAtAll(/' \
    "the conflict restore" "$build"

  # The tree as it stands is the positive case, and it is checked last so a
  # green here cannot be an artefact of the copies above.
  out="$("$GATE" --audit --build-dir "$build" 2>&1)"
  [ $? -eq 0 ] || fail "the census refuses core/ as it is:
$out"
  echo "ok: core/ as it stands is censused and both save paths back up first"
}

# --- what counts as evidence --------------------------------------------------

# A recorded ctest run, in the format ctest actually prints. The `#` matters:
# ctest right-aligns `#N:` in a five-wide field -- `Test   #1:`, `Test  #42:`,
# `Test #121:` -- and the gate's awk requires that shape. A fixture that padded
# after the `#` instead (`Test # 42:`) would be silently unparsable for every
# one- and two-digit index, which is most of the suite, and this file would then
# be testing the gate against a hundred lines it never reads.
transcript_line() {
  printf '%3d/303 Test %5s %s %s%s    0.01 sec\n' "$2" "#$2:" "$1" \
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

  # 3b. The same, for a LOW-numbered test. ctest pads `#N:` differently at one,
  #     two and three digits, and rig.smoke -- the test this whole rule is named
  #     after -- is #42. A fixture that only parses at three digits would let
  #     every one of these assertions pass while reading nothing.
  local low
  low="$(ctest --test-dir "$build" -N 2>/dev/null | sed -n 's/^ *Test *#[0-9]*: //p' | sed -n '19p')"
  [ -n "$low" ] || fail "could not find a low-numbered test to pin the format with"
  transcript "$build" "$low" "***Failed" > "$dir/low-red"
  out="$(ROMMSYNC_GATE_TRANSCRIPT="$dir/low-red" "$GATE" --build-dir "$build" 2>&1)"
  echo "$out" | grep -q "failing:.*$low" \
    || fail "a failing '$low' (a two-digit index) was not seen at all:
$out"
  echo "ok: a two-digit test index parses, so the fixture is in ctest's own format"

  # 3c. A run that stopped early. Every name the row matched but that never
  #     reported anything is the case that reads as green if you only look at
  #     the failures -- ctest erroring out, or a run interrupted part way.
  head -40 "$dir/all-green" > "$dir/truncated"
  out="$(ROMMSYNC_GATE_TRANSCRIPT="$dir/truncated" "$GATE" --build-dir "$build" 2>&1)"
  status=$?
  echo "$out" | grep -q "^\[FAIL\] sync " \
    || fail "a run that stopped after 40 tests left the sync row reading as decided:
$out"
  case "$out" in
    *"never reported a result"*) ;;
    *) fail "the gate did not say which tests said nothing:
$out" ;;
  esac
  [ "$status" -eq 1 ] || fail "expected exit 1 for a run that never finished, got $status"
  echo "ok: a test that reported nothing is not a test that passed"

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
  # The stub goes first on PATH; a stub that refuses is what an absent or offline
  # gh looks like from here. NOT --dry: --dry deliberately does not look a
  # release up, so it cannot exercise these branches. An empty transcript stands
  # in for the suite instead -- it makes the four suite rows fail for want of
  # results, which is true and is not what any of these assertions read.
  out="$(cd "$root" && PATH="$root/bin:$PATH" ROMMSYNC_GATE_TRANSCRIPT=/dev/null \
         "$root/scripts/v1-gate.sh" --build-dir "$build" 2>&1)"
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
  # HELD, not FAIL: "we could not ask" and "we asked and the answer is no" are
  # different answers, and only one of them is a finding. Either way the gate
  # does not open.
  release_says "$dir/no-gh" "is visible from here" HELD \
    "a tag whose release cannot be seen holds the row rather than failing it" "$build"

  # A 0.x tag is not a v1 build, however published it is.
  release_repo "$dir/zero" 0.9.0 v0.9.0 "$published"
  release_says "$dir/zero" "no v1 tag in this repository" FAIL \
    "a 0.x tag does not satisfy a row that says v1" "$build"

  # ...and --dry does not reach for the network at all. The stub here fails the
  # test if it is called, which is the only way to assert an absence.
  release_repo "$dir/dry" 1.0.0 v1.0.0 "$published"
  printf '#!/bin/sh\ntouch "%s/called"\nexit 1\n' "$dir/dry" > "$dir/dry/bin/gh"
  chmod +x "$dir/dry/bin/gh"
  out="$(cd "$dir/dry" && PATH="$dir/dry/bin:$PATH" \
         "$dir/dry/scripts/v1-gate.sh" --dry --build-dir "$build" 2>&1)"
  [ -e "$dir/dry/called" ] && fail "--dry looked a release up over the network"
  echo "$out" | grep -q "^\[HELD\] release " \
    || fail "--dry did not hold the release row it declined to look up:
$(echo "$out" | grep -A3 'release ')"
  echo "ok: --dry does not look a release up, and holds the row rather than judging it"
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
  # ...and the claims, not only the ids, and only within the gate's own section:
  # a claim head that happens to appear elsewhere on the page would otherwise
  # satisfy this while the table said something else.
  local section id claim head
  section="$(awk '/^## Rung 3/,/^## What can/' "$doc")"
  while IFS='|' read -r id _ claim; do
    [ -n "$id" ] || continue
    # The first few words are enough, and are what survives a table cell being
    # wrapped differently from the script's line.
    head="$(echo "$claim" | cut -c1-28)"
    echo "$section" | grep -qF "$head" \
      || fail "the Rung 3 table does not carry row '$id' -- looked for: $head"
  done <<EOF
$("$GATE" --rows)
EOF
  # ...and the EVIDENCE column, not only the claim. This is the half that drifted:
  # the table listed three of the four edge cases the sync row names, and nothing
  # noticed, because ids and claim heads both matched. Each CTest pattern the
  # script names is reduced to a literal token -- `^harness\.same_timestamp$`
  # becomes `harness.same_timestamp`, `^http\.range` becomes `http.range` -- and
  # that token has to appear in the doc's row. It cannot check the reverse (a
  # doc naming evidence the script does not), which `--audit` covers from the
  # other side by refusing a pattern that matches no registered test.
  local row_line pattern token
  while IFS='|' read -r id kind claim; do
    [ "$kind" = "suite" ] || continue
    row_line="$(echo "$section" | grep "^| \`$id\` |")"
    [ -n "$row_line" ] || fail "the Rung 3 table has no row for '$id'"
    while IFS= read -r pattern; do
      [ -n "$pattern" ] || continue
      token="$(echo "$pattern" | sed -e 's/[$^]//g' -e 's/\\//g')"
      case "$row_line" in
        *"$token"*) ;;
        *) fail "the Rung 3 table's '$id' row does not name '$token', which the gate counts as its evidence" ;;
      esac
    done <<PATTERNS
$("$GATE" --rows >/dev/null; sed -n "/^    $id)\$/,/^      ;;\$/p" "$GATE" | sed -n 's/^\(\^[^ ]*\)$/\1/p')
PATTERNS
  done <<EOF
$("$GATE" --rows)
EOF
  echo "ok: docs/TESTING.md publishes the same eight rows, claims and evidence the gate evaluates"
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
