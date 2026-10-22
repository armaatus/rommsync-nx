#!/usr/bin/env bash
# The nightly soak, and the check that it is still running (M9-15, #204).
#
#   test_soak.sh workflow   .github/workflows/soak.yml runs on a schedule and on
#                           demand, drives `test_heap_steady soak` against the
#                           rig, and keeps its numbers whether it passed or not;
#                           ci.yml's `soak-fresh` job runs scripts/soak-fresh.sh.
#                           Either half missing is a soak that can stop meaning
#                           anything without a word.
#   test_soak.sh fresh      scripts/soak-fresh.sh's verdict, on times whose
#                           answer is known: recent passes, stale fails, a soak
#                           that has never passed is measured from the day it
#                           was added, and text that is not a time is never a pass.
#
# Neither needs a network, a token or the rig, so neither skips.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOAK="$REPO_ROOT/.github/workflows/soak.yml"
CI="$REPO_ROOT/.github/workflows/ci.yml"
FRESH="$REPO_ROOT/scripts/soak-fresh.sh"

fail() { echo "FAIL: $*" >&2; exit 1; }

# The job named $1 in ci.yml: its key line up to the next job at the same
# indentation, as tests/test_switch_build.sh reads `switch-build`.
ci_job() {
  awk -v job="  $1:" '$0 == job { in_job = 1; next }
       in_job && /^  [^ #]+:/ { in_job = 0 }
       in_job { print }' "$CI"
}

phase_workflow() {
  [ -f "$SOAK" ] || fail "no .github/workflows/soak.yml; nothing soaks the engine (#204)"
  local body
  body="$(grep -v '^[[:space:]]*#' "$SOAK")"
  grep -qE '^[[:space:]]+schedule:' <<<"$body" || fail "soak.yml has no schedule"
  grep -qE "cron: *'[^']+'" <<<"$body" || fail "soak.yml's schedule names no cron"
  grep -q 'workflow_dispatch:' <<<"$body" ||
    fail "soak.yml cannot be run on demand, so a red night cannot be retried"
  grep -q 'test_heap_steady soak' <<<"$body" ||
    fail "soak.yml does not run the soak scenario"
  grep -q 'ROMMSYNC_ALLOC_REPORT' <<<"$body" ||
    fail "soak.yml keeps no record of what it measured"
  # Observable: the samples are uploaded whether the soak passed or failed, and
  # an upload that found nothing is itself a failure. Read off the upload step
  # itself, because the summary step is `if: always()` too and would answer for
  # an upload that had lost it.
  local upload summary
  upload="$(soak_step upload-artifact)"
  [ -n "$upload" ] || fail "soak.yml uploads nothing it measured"
  grep -q 'if: always()' <<<"$upload" || fail "soak.yml uploads nothing when the soak fails"
  grep -q 'soak.csv' <<<"$upload" || fail "soak.yml's upload does not carry the samples"
  grep -q 'if-no-files-found: error' <<<"$upload" ||
    fail "soak.yml would publish an empty artifact without complaint"
  summary="$(soak_step GITHUB_STEP_SUMMARY)"
  grep -q 'if: always()' <<<"$summary" ||
    fail "soak.yml writes its summary only when the soak passed"

  local job
  job="$(ci_job soak-fresh)"
  [ -n "$job" ] || fail "ci.yml has no soak-fresh job; a soak that stops running goes unnoticed"
  grep -q 'scripts/soak-fresh.sh' <<<"$job" || fail "soak-fresh does not run scripts/soak-fresh.sh"
  grep -q 'fetch-depth: 0' <<<"$job" ||
    fail "soak-fresh checks out shallow, so it cannot tell when soak.yml was added"
  grep -q 'actions: read' <<<"$job" || fail "soak-fresh cannot read the soak's runs"
  echo "ok: soak.yml is scheduled, runnable on demand and observable; ci.yml checks it is fresh"
}

# soak_step PATTERN -- the step of soak.yml whose lines mention PATTERN, from
# its `- ` line to the next step's.
soak_step() {
  awk -v want="$1" '
    /^      - / { if (found) exit; step = "" }
    { step = step $0 "\n"; if (index($0, want)) found = 1 }
    END { if (found) printf "%s", step }' "$SOAK"
}

# verdict EXPECTED_STATUS DESCRIPTION -- run soak-fresh.sh with the SOAK_*
# variables already exported, and hold its exit status to EXPECTED_STATUS.
verdict() {
  local out status
  out="$(bash "$FRESH" 3 2>&1)"
  status=$?
  [ "$status" -eq "$1" ] || { echo "$out" >&2; fail "$2: exited $status, wanted $1"; }
  echo "ok: $2"
}

phase_fresh() {
  [ -f "$FRESH" ] || fail "no scripts/soak-fresh.sh"
  export SOAK_NOW="2026-10-21T12:00:00Z"
  export SOAK_SINCE="2026-09-01T00:00:00Z"

  SOAK_LAST_SUCCESS="2026-10-20T03:30:00Z" verdict 0 "a soak that passed last night is fresh"
  SOAK_LAST_SUCCESS="2026-10-17T03:30:00Z" verdict 1 "one that last passed four days ago is not"
  SOAK_LAST_SUCCESS="2026-10-18T12:00:01Z" verdict 0 "just inside the limit is still fresh"
  SOAK_LAST_SUCCESS="none" verdict 1 "one that has never passed in seven weeks is not"
  SOAK_LAST_SUCCESS="none" SOAK_SINCE="2026-10-20T00:00:00Z" \
    verdict 0 "one added yesterday has until its limit to pass the first time"
  SOAK_LAST_SUCCESS="yesterday-ish" verdict 1 "a last-success time that is not a time is not a pass"
  SOAK_LAST_SUCCESS="2026-10-20T03:30:00Z" SOAK_SINCE="" \
    verdict 1 "nor is not knowing when the soak was added"
}

case "${1:-}" in
  workflow)
    phase_workflow ;;
  fresh)
    phase_fresh ;;
  *)
    echo "usage: $0 {workflow|fresh}" >&2; exit 2 ;;
esac
