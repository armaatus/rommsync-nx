#!/usr/bin/env bash
# Resolve review threads -- and ask the gate again, which is the half nothing
# else does.
#
#   ./scripts/orca/resolve-thread.sh <thread-id> [<thread-id>...]
#
# The ids are the ones `review-status.sh` prints under each unresolved thread.
#
# WHY THIS IS A SCRIPT AND NOT TWO STEPS IN A BRIEF. `merge-gate` re-runs on
# every event that can change its answer, and resolving a thread is not one of
# them: `pull_request_review_thread` is a webhook event, not a workflow trigger
# (actionlint rejects it, and putting it in `on:` invalidated the whole file the
# one time it was tried). So the gate went red on an open thread, the thread was
# resolved, and nothing asked the gate again -- `--auto` never fired and the PR
# sat green-but-blocked until a push or a person came along.
#
# Re-running the gate's own earlier run is what asks it again, because a re-run
# updates that check run IN PLACE, which is the thing branch protection counts.
# `merge-gate.yml`'s `clear-stale` job already relies on this; here it closes the
# resolution gap. A `workflow_dispatch` could not: a dispatched run's checks
# attach to the ref it was dispatched on, not to this PR's head.
#
# Only when the LAST thread closes. With one still open the gate would fail
# again for the same honest reason, and a re-run per resolution is a minute of
# CI to be told what was already known.
#
# Exits 0 when the threads are resolved (whether or not a re-run was needed),
# 1 when a resolution failed, 2 when it could not tell, and 3 when the fleet is
# stopped.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

[ $# -gt 0 ] || {
  echo "usage: resolve-thread.sh <thread-id> [<thread-id>...]" >&2
  echo "  the ids review-status.sh prints under each unresolved thread" >&2
  exit 2; }

# The stop is a stop. This mutates a pull request, which is exactly what nothing
# may do while the file exists.
orca_fleet_stopped && { echo "STOPPED: $ORCA_FLEET_STOP exists."; exit 3; }

orca_owner_repo || { echo "could not read this repository's name from gh" >&2; exit 2; }
owner="$orca_owner"; name="$orca_repo_name"

pr="$(orca_pr_for_branch)" || {
  echo "no open PR for branch $(git rev-parse --abbrev-ref HEAD)" >&2; exit 2; }

failed=0
for id in "$@"; do
  # shellcheck disable=SC2016  -- $id is a GraphQL variable, bound by -F below.
  if GH_PAGER=cat gh api graphql -F id="$id" -f query='
    mutation($id:ID!){ resolveReviewThread(input:{threadId:$id}){
      thread{ id isResolved } } }' >/dev/null 2>&1; then
    echo "resolved $id"
  else
    echo "could not resolve $id" >&2
    failed=1
  fi
done
[ "$failed" = 0 ] || exit 1

# Read the PR back rather than counting what was just resolved: a thread someone
# else opened while this ran is a thread the gate will still fail on, and asking
# it again then is worse than not asking.
payload="$(mktemp)"
trap 'rm -f "$payload"' EXIT
orca_pr_payload "$pr" "$payload" || {
  echo "resolved, but could not re-read PR #$pr to see whether any thread is left" >&2
  exit 2; }

# From merge_gate.py, not re-derived here. Whether a thread list may be read as
# "none left" is the gate's judgement, and this script exists precisely to act on
# the gate's behalf -- a second copy of that rule is a second place for it to
# drift, which is what the shared query already had to fix.
answer="$(python3 -c '
import json, sys
sys.path.insert(0, ".github/scripts")
from merge_gate import thread_list_is_complete, unresolved_threads
pull = json.load(open(sys.argv[1]))["data"]["repository"]["pullRequest"]
print("complete" if thread_list_is_complete(pull) else "partial")
print(len(unresolved_threads(pull)))
' "$payload" 2>/dev/null)" || {
  echo "resolved, but could not load .github/scripts/merge_gate.py to judge what is left" >&2
  exit 2; }
complete="$(printf '%s\n' "$answer" | sed -n 1p)"
left="$(printf '%s\n' "$answer" | sed -n 2p)"

if [ "$complete" != complete ]; then
  echo "PR #$pr has more threads than the gather pages through; not re-running the gate on a partial answer"
  exit 0
fi
if [ "${left:-1}" != 0 ]; then
  # `${left:-?}` in the message as well as in the test: an unreadable count is
  # treated as "still open", and printing the raw value would say " thread(s)
  # still open" with nothing in front of it.
  echo "${left:-an unknown number of} thread(s) still open on PR #$pr; the gate would fail for the same reason, so it is not re-run"
  exit 0
fi

head="$(GH_PAGER=cat gh pr view "$pr" --json headRefOid --jq .headRefOid 2>/dev/null)"
[ -n "$head" ] || { echo "resolved, but could not read PR #$pr's head" >&2; exit 2; }

# By FILE NAME, not by display name: `name:` is prose and gets reworded, and a
# lookup that stops resolving would degrade this to "could not tell" silently.
listing="$(GH_PAGER=cat gh run list --repo "$owner/$name" --workflow merge-gate.yml \
             --limit 40 --json databaseId,conclusion,headSha 2>/dev/null)" || {
  echo "resolved, but could not list merge-gate runs; ask for the gate again with a push" >&2
  exit 2; }

# The NEWEST gate run on this head, and only if it is one that needs re-asking.
#
# `gh run list` returns newest first. If that newest run passed, the check is
# already green and there is nothing to ask; if it is still in flight, it will
# evaluate with the thread now resolved and asking again would only cancel it --
# both are the no-op below. An older failure under a newer success is #84's
# stale-check wedge, and merge-gate.yml's own `clear-stale` job is what clears
# that; re-running it from here would race that job in the gate's
# cancel-in-progress group.
run="$(printf '%s' "$listing" | python3 -c '
import json, sys
head = sys.argv[1]
on_head = [r for r in json.load(sys.stdin) if r.get("headSha") == head]
newest = on_head[0] if on_head else None
print(newest["databaseId"]
      if newest and newest.get("conclusion") in ("failure", "cancelled") else "")
' "$head" 2>/dev/null)"

if [ -z "$run" ]; then
  echo "every thread is resolved; the newest merge-gate run on ${head:0:8} is not one to re-ask"
  exit 0
fi

job="$(GH_PAGER=cat gh api "repos/$owner/$name/actions/runs/$run/jobs" \
         --jq '[.jobs[] | select(.name == "merge-gate") | .id][0]' 2>/dev/null || echo "")"
if [ -z "$job" ] || [ "$job" = "null" ]; then
  # The gate JOB, never the whole run: `clear-stale` is `needs: gate` and would
  # run a second copy of its own rerun loop, which is the wedge it exists to
  # clear reproduced one level down (merge-gate.yml says the same).
  echo "every thread is resolved, but the gate job of run $run could not be found." >&2
  echo "Ask the gate again with: gh run rerun --job <gate job id of run $run>" >&2
  exit 2
fi

if GH_PAGER=cat gh run rerun --job "$job" --repo "$owner/$name" >/dev/null 2>&1; then
  echo "every thread is resolved; re-ran the gate job ($job) so merge-gate is asked again"
else
  echo "every thread is resolved, but re-running the gate job failed." >&2
  echo "Ask it again with: gh run rerun --job $job" >&2
  exit 2
fi
