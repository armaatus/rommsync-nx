#!/usr/bin/env bash
# The launch gate -- M12-0 (#305): may a stable v1.0.0 be published?
#
#   scripts/launch-gate.sh             evaluate every row (running the v1 gate,
#                                      and asking GitHub about M11), print the
#                                      verdict
#   scripts/launch-gate.sh --dry       ...without running the v1 gate or reaching
#                                      the network: which rows are decided from
#                                      the tree alone, and which are held
#   scripts/launch-gate.sh --audit     only: is the gate itself still well
#                                      formed? (this is what `ctest -R gate`
#                                      runs)
#   scripts/launch-gate.sh --rows      the rows, one per line, machine-readable
#
#   --build-dir DIR                    handed to scripts/v1-gate.sh (default:
#                                      ./build, or $ROMMSYNC_BUILD_DIR)
#
# WHY THIS FILE EXISTS. scripts/v1-gate.sh answers "may hardware work start?".
# Nothing answered "may users have it?": the plan for a stable v1.0.0 was a tag
# after #44's smoke test, so a release marked Latest could have shipped before
# the first soak, the first second console, or the first stranger's console.
# This is that second question, written the same way as the first: each row
# names what demonstrates it, and a row nothing here can settle says so.
#
# THREE READERS DEPEND ON `--rows`. scripts/release-notes.sh --verified and
# scripts/package.sh (through it) read `id|kind|claim` lines, exactly as
# v1-gate.sh --rows prints them, and say "verified on hardware" only when no row
# is `console`. A stable release refuses to publish until then (#305). So the
# format is a contract, and a hardware row stays `console` until its evidence
# is committed -- relabelling one early changes what every later release says
# about itself. tests/test_docs_hardware.py reads the same rows to hold
# docs/HARDWARE_TEST_PLAN.md's "What the launch gate cites" table to them.
#
# EXIT CODES, which are the actual answer:
#
#   0   every row holds. v1.0.0 may be published as a stable release.
#   1   a row this machine can decide is FAILING.
#   2   usage.
#   3   nothing failing, but a row is HELD -- it needs a console, a person, or
#       a network this run did not have.
#
# 0 IS UNREACHABLE UNTIL SOMEBODY EDITS THIS FILE, for the reason v1-gate.sh
# gives: no input a script can take is evidence that a console did something.
# When a console has answered a row, its run goes into docs/HARDWARE_RESULTS.md
# and the row's kind changes from `console` to `repo` in the same commit -- a
# reviewed edit, by a person, is the attestation.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROMMSYNC_BUILD_DIR:-$REPO_ROOT/build}"
MODE="run"

die() { echo "launch-gate.sh: $*" >&2; exit 2; }

usage() {
  sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed 's/^#\{1,\} \{0,1\}//; /^set -uo/d'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --run)        MODE="run"; shift ;;
    --dry)        MODE="dry"; shift ;;
    --audit)      MODE="audit"; shift ;;
    --rows)       MODE="rows"; shift ;;
    --build-dir)  [ $# -ge 2 ] || die "--build-dir needs a path"; BUILD_DIR="$2"; shift 2 ;;
    -h|--help)    usage; exit 0 ;;
    *)            die "unknown argument '$1'" ;;
  esac
done

# --- the rows -----------------------------------------------------------------
# id|kind|claim
#
# The order and the claims are #305's table. Two kinds, the two of v1-gate.sh's
# three that apply here -- the host suite is the v1 gate's to judge, and this
# gate asks it rather than repeating its groups:
#
#   repo     decided by `row_repo` from the tree, git, or GitHub.
#   console  nothing off a console can settle it. Always HELD. `row_note` says
#            what would, and the scenarios that are its evidence are read out
#            of docs/HARDWARE_TEST_PLAN.md's "What the launch gate cites".
#
# Claims are plain text with no `|` in them: docs/TESTING.md's Rung 4 table
# carries them verbatim and `gate.launch_doc` compares the two.
gate_rows() {
  cat <<'ROWS'
v1gate|repo|scripts/v1-gate.sh exits 0: the v1 gate (M8-1, #43) passes.
m11|repo|Every M11 issue is closed: the launch blockers found off-console.
selftest|console|The on-console selftest (M12-1) passes on at least two HOS/Atmosphere pairs, and its reports are committed.
smoke|console|#44 (M8-2) is complete: the first console run, on a spare SD or emuMMC.
overlay|console|The overlay README's manual verification run end to end, every screen, handheld and docked, at a 4 MB overlay heap.
soak|console|M12-3: a 72 h soak and lifecycle stress, with zero fatal errors and zero data loss.
matrix|console|M12-4: the compatibility matrix is filled.
servers|console|M12-5: the server and TLS matrix is filled.
beta|console|M12-7: the closed beta's exit criteria are met.
tid|repo|The title id is recorded as final (M12-6).
notes|repo|INSTALL, TROUBLESHOOTING and INTEROP carry no "not yet validated on hardware" note.
ROWS
}

# The files a row reads, relative to the repository. `--audit` insists each
# exists, so a rename fails the gate's machinery rather than turning a row into
# a question about nothing.
row_inputs() {
  case "$1" in
    v1gate)   echo scripts/v1-gate.sh ;;
    overlay)  echo overlay/README.md ;;
    tid)      printf '%s\n' sysmodule/sys-rommsync.json sysmodule/README.md ;;
    notes)    printf '%s\n' docs/INSTALL.md docs/TROUBLESHOOTING.md docs/INTEROP.md ;;
  esac
  return 0
}

# The hardware rows, whatever their kind today. Every other row is about the
# tree; these are about a console, and keep their scenarios in the plan after
# they are flipped to `repo`.
HARDWARE_ROWS=" selftest smoke overlay soak matrix servers beta "
is_hardware() { case "$HARDWARE_ROWS" in *" $1 "*) return 0 ;; esac; return 1; }

PLAN="docs/HARDWARE_TEST_PLAN.md"
RESULTS="docs/HARDWARE_RESULTS.md"

# The scenario ids the plan says are a hardware row's evidence: the last cell of
# its line in "What the launch gate cites". Read rather than restated, so the
# plan stays the one place they are written.
cited_scenarios() {
  awk '/^## What the launch gate cites/{on=1; next} /^## /{on=0} on' \
      "$REPO_ROOT/$PLAN" 2>/dev/null \
    | sed -n "s/^| \`$1\` | \(.*\) |\$/\1/p" | head -1
}

# --- repo predicates ----------------------------------------------------------
# Each prints its reasoning on stdout and answers with an exit status: 0 holds,
# 1 does not, 2 is not known from here -- which is HELD, never a pass.

# Same bound, and same reasoning, as v1-gate.sh's `bounded`: CLAUDE.md wants a
# timeout on every network call, and `timeout(1)` is not on a stock macOS.
GH_TIMEOUT="${ROMMSYNC_GATE_GH_TIMEOUT:-15}"

bounded() {
  local secs="$1"; shift
  if command -v timeout >/dev/null 2>&1; then
    timeout "$secs" env "$@"
    return $?
  fi
  if command -v gtimeout >/dev/null 2>&1; then
    gtimeout "$secs" env "$@"
    return $?
  fi
  local pid watch rc
  env "$@" &
  pid=$!
  ( sleep "$secs"; kill -TERM "$pid" 2>/dev/null ) >/dev/null 2>&1 &
  watch=$!
  wait "$pid"
  rc=$?
  kill "$watch" 2>/dev/null
  wait "$watch" 2>/dev/null
  return "$rc"
}

# The v1 gate's own exit code, which is its verdict. Its 3 -- held on a console
# or a person -- is held here too: not a failure this tree can be blamed for,
# and not a pass. --dry runs its --dry, which can still report a FAIL it already
# knows about (a census breach, a tag that disagrees with VERSION).
repo_v1gate() {
  local gate="$REPO_ROOT/scripts/v1-gate.sh" out rc
  [ -f "$gate" ] || { echo "    no scripts/v1-gate.sh in this tree"; return 1; }
  if [ "$MODE" = "dry" ]; then
    out="$(bash "$gate" --dry --build-dir "$BUILD_DIR" 2>&1)"
    rc=$?
    if [ "$rc" -eq 1 ]; then
      echo "    scripts/v1-gate.sh --dry already finds a failing row:"
      echo "$out" | grep '^\[FAIL\]' | sed 's/^/      /'
      return 1
    fi
    echo "    not run in --dry: scripts/v1-gate.sh runs the gate's CTest groups"
    return 2
  fi
  out="$(bash "$gate" --build-dir "$BUILD_DIR" 2>&1)"
  rc=$?
  echo "$out" | grep -E '^\[(FAIL|HELD)\]' | sed 's/^/      /'
  echo "    scripts/v1-gate.sh exited $rc: $(echo "$out" | tail -1)"
  case "$rc" in
    0) return 0 ;;
    3) return 2 ;;
    *) return 1 ;;
  esac
}

# Every issue in the M11 milestone, epic included, closed. Asked of GitHub,
# because which issues are open is not a fact the tree records. "No answer"
# (no gh, no network, no auth) is unknown; "no M11 milestone" is an answer,
# and a wrong one. One attempt and no retry, like v1-gate.sh's release lookup:
# a gate run by hand degrades to HELD, and running it again is the retry.
M11_PREFIX="M11 "
repo_m11() {
  if [ "$MODE" = "dry" ]; then
    echo "    not looked up in --dry: which M11 issues are open is GitHub's to say"
    return 2
  fi
  local answer view=""
  if command -v gh >/dev/null 2>&1; then
    # Into a file rather than a pipe, for v1-gate.sh's reason: a killed `gh`
    # whose child still holds a pipe would hang this on the read instead.
    answer="$(mktemp "${TMPDIR:-/tmp}/launchgate-gh.XXXXXX")"
    bounded "$GH_TIMEOUT" GH_PAGER=cat gh api \
      'repos/{owner}/{repo}/milestones?state=all&per_page=100' \
      --jq "[.[] | select(.title | startswith(\"$M11_PREFIX\"))] | if length == 0 then \"none\" else (.[0].open_issues | tostring) end" \
      >"$answer" 2>/dev/null
    view="$(tr -d '[:space:]' <"$answer" 2>/dev/null)"
    rm -f "$answer"
  fi
  case "$view" in
    "")
      echo "    the M11 milestone is not visible from here -- gh is absent or could"
      echo "    not answer"
      return 2 ;;
    none)
      echo "    GitHub lists no milestone whose title starts '$M11_PREFIX'"
      return 1 ;;
    0)
      echo "    every issue in M11 is closed"
      return 0 ;;
    *[!0-9]*)
      echo "    gh answered '$view', which is not a count"
      return 2 ;;
    *)
      echo "    $view M11 issue(s) still open -- gh issue list --milestone '<M11 title>'"
      return 1 ;;
  esac
}

# The title id is frozen (M12-6, #308): sysmodule/README.md declares the id
# sys-rommsync.json carries final, on one line -- "`0x4200000000524D53` is
# final" -- and no longer says nothing verifies it.
repo_tid() {
  local json="$REPO_ROOT/sysmodule/sys-rommsync.json" readme="$REPO_ROOT/sysmodule/README.md" id
  id="$(sed -n 's/^[[:space:]]*"program_id":[[:space:]]*"\(0x[0-9A-Fa-f]*\)".*/\1/p' "$json" 2>/dev/null | head -1)"
  [ -n "$id" ] || { echo "    sysmodule/sys-rommsync.json carries no program_id"; return 1; }
  local ok=0
  if grep -q "Nothing verifies it is unused" "$readme" 2>/dev/null; then
    echo "    sysmodule/README.md still says nothing verifies $id is unused"
    ok=1
  fi
  if ! grep -iF -- "$id" "$readme" 2>/dev/null | grep -qi "is final"; then
    echo "    sysmodule/README.md does not declare $id final (M12-6, #308)"
    ok=1
  fi
  [ "$ok" -eq 0 ] && echo "    sysmodule/README.md declares $id final"
  return "$ok"
}

repo_notes() {
  local doc ok=0
  for doc in docs/INSTALL.md docs/TROUBLESHOOTING.md docs/INTEROP.md; do
    if grep -qi "not yet validated on hardware" "$REPO_ROOT/$doc" 2>/dev/null; then
      echo "    $doc still says it is not yet validated on hardware"
      ok=1
    fi
  done
  [ "$ok" -eq 0 ] && echo "    no guide says it is unvalidated on hardware"
  return "$ok"
}

# A hardware row whose kind a person changed to `repo`. The edit is the
# attestation; this only refuses one made with nothing behind it -- a flip with
# no run recorded in docs/HARDWARE_RESULTS.md.
repo_recorded() {
  if grep -q '^### Run ' "$REPO_ROOT/$RESULTS" 2>/dev/null; then
    echo "    flipped by a reviewed edit; runs are recorded in $RESULTS"
    return 0
  fi
  echo "    marked settled, but $RESULTS records no run"
  return 1
}

row_repo() {
  case "$1" in
    v1gate) repo_v1gate ;;
    m11)    repo_m11 ;;
    tid)    repo_tid ;;
    notes)  repo_notes ;;
    *)      if is_hardware "$1"; then repo_recorded; else return 3; fi ;;
  esac
}

# --- what a console row is waiting for ----------------------------------------
row_note() {
  case "$1" in
    selftest) cat <<'NOTE'
    Run the selftest NRO (M12-1, #306) on at least two HOS/Atmosphere pairs and
    commit both machine-readable reports beside the run.
NOTE
      ;;
    smoke) cat <<'NOTE'
    #44 (M8-2) is the first console run: NRO first, the sysmodule installed
    disabled, a disposable collection, .backup/ populated before any sync. Its
    checklists are the plan's scenarios now.
NOTE
      ;;
    overlay) cat <<'NOTE'
    overlay/README.md's "Manual verification" section, every screen, handheld
    and docked, with nx-ovlloader's heap at 4 MB.
NOTE
      ;;
    soak) cat <<'NOTE'
    M12-3 (#309): 72 hours with co-resident sysmodules, sleep/wake and overlay
    cycling; zero fatal errors, zero saves lost or altered.
NOTE
      ;;
    matrix) cat <<'NOTE'
    M12-4 (#310): the newest HOS/Atmosphere and one older pair, emuMMC and
    sysMMC, Erista and Mariko, Ultrahand and the Tesla menu.
NOTE
      ;;
    servers) cat <<'NOTE'
    M12-5 (#311): Let's Encrypt RSA and ECDSA, a private CA, an IP literal,
    .local, TLS 1.3 only, and a tunnel, each from a console.
NOTE
      ;;
    beta) cat <<'NOTE'
    M12-7 (#312): a release candidate on other people's consoles, until the
    issue's exit criteria are met.
NOTE
      ;;
    *) return 0 ;;
  esac
  echo "    Evidence: $(cited_scenarios "$1") (docs/HARDWARE_TEST_PLAN.md)."
  echo "    Results go in docs/HARDWARE_RESULTS.md, and the row's kind changes from"
  echo "    console to repo in that commit. Nothing this script can be handed ticks it."
}

# --- evaluation ---------------------------------------------------------------

FAILING=0
HELD=0

evaluate_row() {
  local id="$1" kind="$2" claim="$3" status detail="" out rc
  if [ "$kind" = "console" ]; then
    status="HELD"
    detail="$(row_note "$id")"
  else
    out="$(row_repo "$id")"
    rc=$?
    case "$rc" in
      0) status="PASS" ;;
      2) status="HELD" ;;
      *) status="FAIL" ;;
    esac
    detail="$out"
  fi
  printf '[%s] %-9s %s\n' "$status" "$id" "$claim"
  [ -n "$detail" ] && echo "$detail" | sed '/^$/d'
  case "$status" in
    FAIL) FAILING=$((FAILING + 1)) ;;
    HELD) HELD=$((HELD + 1)) ;;
  esac
  return 0
}

# --audit is what ctest runs: is every row still pointing at something that
# exists? Like v1-gate.sh's, it never reports the verdict -- it does not pass,
# and ctest going green must not read as though it did.
audit() {
  local ok=0 id kind claim extra seen=" " f
  while IFS='|' read -r id kind claim extra; do
    [ -n "$id" ] || continue
    case "$seen" in *" $id "*) echo "audit: row '$id' appears twice"; ok=1 ;; esac
    seen="$seen$id "
    [ -z "$extra" ] || { echo "audit: row '$id' has more than three fields"; ok=1; }
    [ -n "$claim" ] || { echo "audit: row '$id' has no claim"; ok=1; }
    case "$kind" in
      repo|console) ;;
      *) echo "audit: row '$id' has an unknown kind '$kind'"; ok=1; continue ;;
    esac
    if [ "$kind" = "console" ]; then
      is_hardware "$id" || { echo "audit: console row '$id' is not a hardware row"; ok=1; }
      # An id with no note of its own prints nothing at all, Evidence line included.
      [ -n "$(row_note "$id")" ] || {
        echo "audit: console row '$id' does not say what would settle it"; ok=1; }
    fi
    if is_hardware "$id" && [ -z "$(cited_scenarios "$id")" ]; then
      echo "audit: $PLAN's \"What the launch gate cites\" names no scenarios for '$id'"
      ok=1
    fi
    if [ "$kind" = "repo" ]; then
      case "$id" in
        v1gate|m11|tid|notes) ;;
        *) is_hardware "$id" || { echo "audit: repo row '$id' has no predicate"; ok=1; } ;;
      esac
    fi
    while IFS= read -r f; do
      [ -n "$f" ] || continue
      [ -f "$REPO_ROOT/$f" ] || { echo "audit: row '$id' reads $f, which is not in the tree"; ok=1; }
    done <<EOF
$(row_inputs "$id")
EOF
  done <<EOF
$(gate_rows)
EOF
  for f in "$PLAN" "$RESULTS"; do
    [ -f "$REPO_ROOT/$f" ] || { echo "audit: $f is not in the tree"; ok=1; }
  done
  [ "$ok" -eq 0 ] && echo "ok: $(gate_rows | grep -c .) launch-gate rows, each pointing at something that exists"
  return "$ok"
}

case "$MODE" in
  rows)
    gate_rows
    exit 0
    ;;
  audit)
    audit
    exit $?
    ;;
esac

echo "The launch gate -- M12-0 (#305). A stable v1.0.0 is published when every row is PASS."
echo

while IFS='|' read -r id kind claim; do
  [ -n "$id" ] || continue
  evaluate_row "$id" "$kind" "$claim"
  echo
done <<EOF
$(gate_rows)
EOF

if [ "$FAILING" -gt 0 ]; then
  echo "LAUNCH GATE FAILS: $FAILING row(s) failing, $HELD held. No stable release."
  exit 1
fi
if [ "$HELD" -gt 0 ]; then
  echo "LAUNCH GATE HELD: $HELD row(s) need a console, a person or a network. No stable release."
  exit 3
fi
echo "LAUNCH GATE PASSES. v1.0.0 may be published as a stable release."
exit 0
