#!/usr/bin/env bash
# The launch gate's own machinery -- scripts/launch-gate.sh, which is M12-0 (#305).
#
#   test_launch_gate.sh audit    every row still points at something that
#                                exists, and `--rows` is the `id|kind|claim`
#                                contract release-notes.sh and package.sh read.
#   test_launch_gate.sh rows     ...and the audit can fail: an invented kind, a
#                                console row that does not say what settles it,
#                                a hardware row the plan cites nothing for, an
#                                input renamed away, a row listed twice.
#   test_launch_gate.sh repo     the repo rows and the verdict, against
#                                throwaway trees with a stub v1-gate.sh and a
#                                stub `gh`: each row's PASS, FAIL and HELD, the
#                                exit codes 0/1/3, a gh that hangs, and --dry
#                                reaching for neither the network nor the suite.
#   test_launch_gate.sh doc      docs/TESTING.md's Rung 4 table publishes the
#                                same rows and claims the script evaluates.
#
# None of them needs Docker, a network, a build or a console.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GATE="$REPO_ROOT/scripts/launch-gate.sh"

fail() { echo "FAIL: $*" >&2; exit 1; }

# Made once, here: `scratch` is read through a command substitution, a subshell,
# so creating it lazily there would make a new directory on every call.
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/launchgate.XXXXXX")"
cleanup() { rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT
scratch() { echo "$SCRATCH"; }

HARDWARE="selftest smoke overlay soak matrix servers beta"

# A copy of the gate over copies of exactly the files it reads, so a negative
# case mutates the copy and never the checkout. scripts/v1-gate.sh is a stub
# answering with $V1_EXIT; bin/gh is a stub answering the milestone lookup.
fake_repo() {
  local root="$1" f
  rm -rf "$root"
  mkdir -p "$root/scripts" "$root/bin"
  cp "$GATE" "$root/scripts/launch-gate.sh"
  for f in docs/HARDWARE_TEST_PLAN.md docs/HARDWARE_RESULTS.md docs/INSTALL.md \
           docs/TROUBLESHOOTING.md docs/INTEROP.md sysmodule/sys-rommsync.json sysmodule/README.md \
           overlay/README.md; do
    mkdir -p "$root/$(dirname "$f")"
    cp "$REPO_ROOT/$f" "$root/$f"
  done
  cat > "$root/scripts/v1-gate.sh" <<'EOF'
#!/usr/bin/env bash
# a stub: the v1 gate's verdict, as its exit code
echo "[HELD] ssl  stub"
echo "stub verdict"
exit "${V1_EXIT:-3}"
EOF
  gh_answers "$root" ""
}

# What `gh api .../milestones --jq ...` prints: a count, "none", REFUSE (gh
# could not answer) or HANG (gh never answers). It records that it was asked.
gh_answers() {
  local root="$1" answer="$2"
  case "$answer" in
    REFUSE) printf '#!/bin/sh\ntouch "%s/gh-called"\nexit 1\n' "$root" ;;
    HANG)   printf '#!/bin/sh\ntouch "%s/gh-called"\nsleep 600\n' "$root" ;;
    *)      printf '#!/bin/sh\ntouch "%s/gh-called"\n[ "$1" = api ] || exit 1\necho "%s"\n' "$root" "$answer" ;;
  esac > "$root/bin/gh"
  chmod +x "$root/bin/gh"
}

# The tree a stable release is cut from: title id frozen, guides without the
# unvalidated note, a run recorded.
settle_repo_rows() {
  local root="$1"
  sed -e 's/Nothing verifies it is unused\./Checked by M12-6./' "$REPO_ROOT/sysmodule/README.md" \
    > "$root/sysmodule/README.md"
  echo '`0x4200000000524D53` is final.' >> "$root/sysmodule/README.md"
  local doc
  for doc in docs/INSTALL.md docs/TROUBLESHOOTING.md docs/INTEROP.md; do
    grep -vi 'not yet validated on hardware' "$REPO_ROOT/$doc" > "$root/$doc"
  done
  printf '\n### Run 2027-01-01 -- someone, Switch OLED\n' >> "$root/docs/HARDWARE_RESULTS.md"
}

# Every hardware row's kind changed to repo: what the reviewed edit does.
flip_console_rows() {
  sed 's/^\([a-z]*\)|console|/\1|repo|/' "$1/scripts/launch-gate.sh" > "$1/scripts/launch-gate.edited"
  mv "$1/scripts/launch-gate.edited" "$1/scripts/launch-gate.sh"
}

# Runs the copied gate; sets OUT and STATUS.
run_gate() {
  local root="$1"; shift
  OUT="$(cd "$root" && PATH="$root/bin:$PATH" V1_EXIT="${V1_EXIT:-3}" bash "$root/scripts/launch-gate.sh" "$@" 2>&1)"
  STATUS=$?
}

row_is() {
  local want="$1" id="$2" what="$3"
  echo "$OUT" | grep -q "^\[$want\] $id " || fail "$what: expected [$want] $id, got:
$OUT"
}

# --- the audit, and --rows ----------------------------------------------------

phase_audit() {
  local out
  out="$("$GATE" --audit 2>&1)" || fail "the launch gate does not audit clean:
$out"
  case "$out" in
    *"11 launch-gate rows"*) ;;
    *) fail "expected the audit to report eleven rows, got: $out" ;;
  esac

  # The contract release-notes.sh --verified and package.sh read.
  local rows id kind claim extra
  rows="$("$GATE" --rows)" || fail "--rows failed"
  while IFS='|' read -r id kind claim extra; do
    [ -n "$id" ] && [ -n "$kind" ] && [ -n "$claim" ] && [ -z "$extra" ] ||
      fail "--rows printed a line that is not id|kind|claim: $id|$kind|$claim|$extra"
  done <<<"$rows"
  # Every hardware row is still console: nothing has been recorded on a console
  # (hard rule 1), and a row relabelled early flips every release's wording.
  for id in $HARDWARE; do
    grep -q "^$id|console|" <<<"$rows" ||
      fail "hardware row '$id' is not a console row, and docs/HARDWARE_RESULTS.md records no run"
  done
  echo "ok: $out"
}

# --- ...and it can fail -------------------------------------------------------

audit_must_fail() {
  local what="$1" expect="$2" root
  root="$(scratch)/$what"
  run_gate "$root" --audit
  [ "$STATUS" -ne 0 ] || fail "$what: the audit passed a gate it should have refused:
$OUT"
  case "$OUT" in
    *"$expect"*) ;;
    *) fail "$what: expected the audit to mention '$expect', got:
$OUT" ;;
  esac
  echo "ok: $what -> $(echo "$OUT" | head -1)"
}

doctor() {  # root sed-expression: edit the copied gate
  sed "$2" "$1/scripts/launch-gate.sh" > "$1/scripts/launch-gate.edited"
  mv "$1/scripts/launch-gate.edited" "$1/scripts/launch-gate.sh"
}

phase_rows() {
  local dir; dir="$(scratch)"

  fake_repo "$dir/kind"; doctor "$dir/kind" 's/^matrix|console|/matrix|nonsense|/'
  audit_must_fail kind "unknown kind"

  # A console row with nothing saying what would settle it.
  fake_repo "$dir/silent"; doctor "$dir/silent" 's/^    beta) cat/    beta_unsaid) cat/'
  audit_must_fail silent "does not say what would settle it"

  # The plan's table loses a row: the gate would print an empty Evidence line.
  fake_repo "$dir/uncited"
  grep -v '^| `servers` |' "$REPO_ROOT/docs/HARDWARE_TEST_PLAN.md" > "$dir/uncited/docs/HARDWARE_TEST_PLAN.md"
  audit_must_fail uncited "names no scenarios for 'servers'"

  # An input renamed away.
  fake_repo "$dir/renamed"; rm "$dir/renamed/docs/TROUBLESHOOTING.md"
  audit_must_fail renamed "reads docs/TROUBLESHOOTING.md, which is not in the tree"

  fake_repo "$dir/twice"; doctor "$dir/twice" 's/^tid|repo|/notes|repo|/'
  audit_must_fail twice "appears twice"

  # A repo row with nothing to decide it would pass by default.
  fake_repo "$dir/nopred"; doctor "$dir/nopred" 's/^m11|repo|/m12|repo|/'
  audit_must_fail nopred "has no predicate"
}

# --- the repo rows and the verdict --------------------------------------------

phase_repo() {
  local dir; dir="$(scratch)"
  local root

  # Today's tree: title id and guides not settled, so FAIL, and exit 1.
  root="$dir/today"; fake_repo "$root"; gh_answers "$root" 3
  run_gate "$root"
  row_is FAIL tid "a README that says nothing verifies the id"
  row_is FAIL notes "guides that say they are unvalidated"
  row_is FAIL m11 "three M11 issues open"
  row_is HELD v1gate "a v1 gate that exits 3"
  for id in $HARDWARE; do row_is HELD "$id" "a console row"; done
  echo "$OUT" | grep -q 'Evidence: PRE-02, CLK-04' ||
    fail "the selftest row does not cite the plan's scenarios:
$OUT"
  [ "$STATUS" -eq 1 ] || fail "expected exit 1 with failing rows, got $STATUS"
  echo "ok: today's tree fails tid, notes and m11, and holds every console row"

  # Every repo row settled: only the console rows hold, and that is exit 3.
  root="$dir/settled"; fake_repo "$root"; settle_repo_rows "$root"; gh_answers "$root" 0
  run_gate "$root"
  for id in tid notes m11; do row_is PASS "$id" "a settled tree"; done
  row_is HELD v1gate "a v1 gate that exits 3"
  [ "$STATUS" -eq 3 ] || fail "expected exit 3 with only held rows, got $STATUS:
$OUT"
  V1_EXIT=0 run_gate "$root"
  row_is PASS v1gate "a v1 gate that exits 0"
  [ "$STATUS" -eq 3 ] || fail "console rows held, but the gate exited $STATUS"
  V1_EXIT=1 run_gate "$root"
  row_is FAIL v1gate "a v1 gate that exits 1"
  [ "$STATUS" -eq 1 ] || fail "expected exit 1 with a failing v1 gate, got $STATUS"
  echo "ok: the v1 gate's 0/1/3 are PASS/FAIL/HELD here, and console rows alone hold at 3"

  # ...and 0 only when a person has flipped the console rows.
  flip_console_rows "$root"
  V1_EXIT=0 run_gate "$root"
  [ "$STATUS" -eq 0 ] || fail "every row settled and flipped, but the gate exited $STATUS:
$OUT"
  echo "$OUT" | grep -q "LAUNCH GATE PASSES" || fail "exit 0 without saying it passes"
  # INTEROP carries the same note as the two guides, and a stable release may
  # not ship any of the three still saying it.
  cp "$REPO_ROOT/docs/INTEROP.md" "$root/docs/INTEROP.md"
  V1_EXIT=0 run_gate "$root"
  row_is FAIL notes "an INTEROP that says it is unvalidated"
  grep -vi 'not yet validated on hardware' "$REPO_ROOT/docs/INTEROP.md" > "$root/docs/INTEROP.md"
  # A flip with no run recorded behind it is refused.
  cp "$REPO_ROOT/docs/HARDWARE_RESULTS.md" "$root/docs/HARDWARE_RESULTS.md"
  V1_EXIT=0 run_gate "$root"
  row_is FAIL smoke "a flipped row with no recorded run"
  echo "ok: 0 needs the console rows flipped by an edit, and a run recorded behind it"

  # M11: no answer holds; no milestone fails; a hung gh is bounded.
  root="$dir/m11"; fake_repo "$root"
  gh_answers "$root" REFUSE; run_gate "$root"
  row_is HELD m11 "a gh that cannot answer"
  gh_answers "$root" none; run_gate "$root"
  row_is FAIL m11 "no M11 milestone"
  gh_answers "$root" HANG
  local began; began="$(date +%s)"
  OUT="$(cd "$root" && PATH="$root/bin:$PATH" ROMMSYNC_GATE_GH_TIMEOUT=2 \
         bash "$root/scripts/launch-gate.sh" 2>&1)"
  [ $(( $(date +%s) - began )) -lt 30 ] || fail "a gh that never answers was not bounded"
  row_is HELD m11 "a gh that hangs"
  echo "ok: an unanswered M11 lookup holds, a missing milestone fails, a hang is bounded"

  # --dry asks neither GitHub nor the suite.
  root="$dir/dry"; fake_repo "$root"; gh_answers "$root" 0
  run_gate "$root" --dry
  [ -e "$root/gh-called" ] && fail "--dry looked M11 up over the network"
  row_is HELD m11 "--dry"
  row_is HELD v1gate "--dry over a v1 gate --dry that finds nothing failing"
  V1_EXIT=1 run_gate "$root" --dry
  row_is FAIL v1gate "--dry over a v1 gate --dry that already finds a failure"
  echo "ok: --dry reaches for neither the network nor the suite, and keeps what it knows"
}

# --- the published table ------------------------------------------------------

phase_doc() {
  local doc="$REPO_ROOT/docs/TESTING.md" section doc_ids script_ids id kind claim head
  section="$(awk '/^## Rung 4/{on=1} on && /^## / && !/^## Rung 4/{exit} on' "$doc")"
  [ -n "$section" ] || fail "docs/TESTING.md has no Rung 4 section"
  doc_ids="$(echo "$section" | sed -n 's/^| `\([a-z0-9]*\)` *|.*/\1/p')"
  script_ids="$("$GATE" --rows | cut -d'|' -f1)"
  [ "$doc_ids" = "$script_ids" ] || fail "docs/TESTING.md's Rung 4 table and scripts/launch-gate.sh disagree about the rows.
doc:
$doc_ids
script:
$script_ids"
  while IFS='|' read -r id kind claim; do
    row="$(echo "$section" | grep "^| \`$id\` |")"
    case "$row" in
      *"| $kind |"*) ;;
      *) fail "the Rung 4 table does not give row '$id' the kind '$kind'" ;;
    esac
    head="$(echo "$claim" | cut -c1-28)"
    case "$row" in
      *"$head"*) ;;
      *) fail "the Rung 4 table's '$id' row does not carry its claim -- looked for: $head" ;;
    esac
  done <<EOF
$("$GATE" --rows)
EOF
  echo "ok: docs/TESTING.md publishes the launch gate's $(echo "$script_ids" | grep -c .) rows, kinds and claims"
}

case "${1:-}" in
  audit) phase_audit ;;
  rows)  phase_rows ;;
  repo)  phase_repo ;;
  doc)   phase_doc ;;
  *) echo "usage: test_launch_gate.sh {audit|rows|repo|doc}" >&2; exit 2 ;;
esac
