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
#   test_soak.sh bounded    a `gh` that never answers is abandoned within the
#                           script's own timeout, on every attempt.
#   test_soak.sh tracked    the soak's and the image's numbers are handed to
#                           benchmark-action on main, night by night (#250).
#   test_soak.sh --bench F  F, as `soak.smoke` wrote it, is benchmark-action's
#                           `customSmallerIsBetter` shape. A flag rather than a
#                           phase: it takes a file, and `soak.bench` registers it
#                           on its own, behind `soak.smoke`.
#
# None but `--bench` needs a network, a token or the rig, so none other skips;
# `--bench` skips when the soak that writes its file did.
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
  grep -qE 'test_heap_steady schedule [0-9]+' <<<"$body" ||
    fail "soak.yml does not run the schedule on its fast clock (#250)"
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
  # Bounded (#256): without it a hung step holds main's run for GitHub's
  # six-hour default, and the job exists to make main's state legible.
  grep -qE '^    timeout-minutes: *[0-9]+' <<<"$job" ||
    fail "soak-fresh sets no timeout-minutes; a hung step holds main's run for six hours"
  echo "ok: soak.yml is scheduled, runnable on demand and observable; ci.yml checks it is fresh"
}

# The numbers are kept from one night to the next (#250): the soak and the image
# each write benchmark-action's JSON, and two steps hand it to the action on
# main, with the branch it writes to made if it is not there and the job allowed
# to push it.
phase_tracked() {
  [ -f "$SOAK" ] || fail "no .github/workflows/soak.yml"
  local body
  body="$(grep -v '^[[:space:]]*#' "$SOAK")"
  grep -q 'ROMMSYNC_BENCH_JSON=' <<<"$body" || fail "the soak writes no numbers to track"
  grep -q 'IMAGE_BUDGET_JSON=' <<<"$body" || fail "the image's numbers are not measured"
  local file step
  for file in soak-bench.json image-bench.json; do
    step="$(soak_step "output-file-path: $file")"
    [ -n "$step" ] || fail "nothing tracks $file"
    grep -q 'benchmark-action/github-action-benchmark@' <<<"$step" ||
      fail "$file is not handed to benchmark-action"
    grep -q 'tool: customSmallerIsBetter' <<<"$step" ||
      fail "$file is tracked as something other than a number that should not grow"
    grep -q 'gh-pages-branch: benchmarks' <<<"$step" ||
      fail "$file is tracked to a branch the bootstrap step does not make"
    grep -q 'auto-push: true' <<<"$step" || fail "$file's history is never pushed"
  done
  grep -q 'refs/heads/benchmarks' <<<"$body" ||
    fail "nothing makes the benchmarks branch, and benchmark-action needs it to exist"
  awk '/^  soak:/ { job = 1 } job && /^    permissions:/ { p = 1 } p && /contents: write/ { found = 1 }
       END { exit !found }' "$SOAK" ||
    fail "the soak job cannot push the numbers it tracks (contents: write)"
  echo "ok: the soak's and the image's numbers are tracked night by night"
}

# bench FILE -- FILE is benchmark-action's customSmallerIsBetter shape: a
# non-empty array of {name, unit, value} with a numeric value. Skips when FILE
# is missing, which is a soak that skipped for want of the rig.
phase_bench() {
  local file="${1:-}"
  [ -n "$file" ] || fail "usage: $0 --bench FILE"
  if [ ! -s "$file" ]; then
    echo "skip: $file was not written; the soak it comes from did not run"
    exit 77
  fi
  "${PYTHON:-python3}" - "$file" <<'PY' || exit 1
import json, sys
rows = json.load(open(sys.argv[1]))
assert isinstance(rows, list) and rows, "not a non-empty array"
for row in rows:
    assert set(row) >= {"name", "unit", "value"}, f"missing a field: {row}"
    assert isinstance(row["value"], (int, float)) and row["value"] >= 0, f"not a number: {row}"
print(f"ok: {len(rows)} numbers in benchmark-action's shape")
PY
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

# A `gh` that never answers -- a stalled API call, the case the retries do not
# cover -- is given up on, attempt by attempt, rather than waited for until the
# job is killed (#256). The stub sleeps in a child, so killing only the process
# soak-fresh.sh started would leave the sleep holding its output open, and the
# command substitution waiting on it.
phase_bounded() {
  [ -f "$FRESH" ] || fail "no scripts/soak-fresh.sh"
  local bin out status started
  bin="$(mktemp -d)"
  trap 'rm -rf "$bin"' RETURN
  printf '#!/usr/bin/env bash\nsleep 300\necho 2026-10-20T03:30:00Z\n' >"$bin/gh"
  chmod +x "$bin/gh"
  unset SOAK_LAST_SUCCESS
  started=$SECONDS
  out="$(PATH="$bin:$PATH" SOAK_GH_TIMEOUT=1 SOAK_GH_RETRY_DELAY=0 \
         SOAK_NOW="2026-10-21T12:00:00Z" SOAK_SINCE="2026-09-01T00:00:00Z" \
         bash "$FRESH" 3 2>&1)"
  status=$?
  [ "$status" -eq 1 ] || { echo "$out" >&2; fail "a gh that never answers: exited $status, wanted 1"; }
  [ $(( SECONDS - started )) -lt 20 ] ||
    { echo "$out" >&2; fail "a gh that never answers held soak-fresh.sh $(( SECONDS - started )) s"; }
  grep -q 'could not ask GitHub' <<<"$out" ||
    { echo "$out" >&2; fail "soak-fresh.sh gave up on gh without saying so"; }
  echo "ok: a gh call that never answers is abandoned, and soak-fresh says so"
}

case "${1:-}" in
  bounded)
    phase_bounded ;;
  workflow)
    phase_workflow ;;
  fresh)
    phase_fresh ;;
  tracked)
    phase_tracked ;;
  --bench)
    phase_bench "${2:-}" ;;
  *)
    echo "usage: $0 {workflow|fresh|bounded|tracked|--bench FILE}" >&2; exit 2 ;;
esac
