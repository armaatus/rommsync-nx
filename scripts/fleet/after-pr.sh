#!/usr/bin/env bash
# Everything that happens to one pull request after it is opened.
#
#   ./scripts/fleet/after-pr.sh 42
#
# THE AGENT'S JOB ENDS AT "PR OPEN WITH `Closes #N`". This is the rest, and it
# is the dispatcher's: arm the merge, review once, buy at most one fix, review
# the fix once, and then stop -- either GitHub merges it on its own rules or a
# person is told why not.
#
#   1. `gh pr merge --auto --squash`, the moment the PR exists. The dispatcher
#      is the one identity allowed to ask; `guard.py` refuses it from a fleet
#      worktree. It does not merge -- it asks GitHub to, once the required
#      checks pass, which is what makes the RULES decide rather than an agent.
#   2. `review.sh`. Approve and there is nothing left to do.
#   3. On `request-changes`, `fix.sh`: one session, in the worktree, answering
#      the findings.
#   4. `review.sh` once more, on the head the fix pushed. THAT VERDICT IS FINAL.
#      A second `request-changes` parks the PR with a comment and the dispatcher
#      moves on.
#
# TWO REVIEWS MAXIMUM, EVER, and the ceiling is a file rather than the shape of
# this script: a dispatcher restart, a second machine or a person running this
# by hand would otherwise each get their own two. `<pr>.reviews` under
# $FLEET_REVIEWING counts them, and is written BEFORE the review runs -- a
# reviewer that crashes has still been bought.
#
# What this replaces never terminated. One review, then up to two validations
# judging the author's prose answer to it: every validation of #132 and #133
# came back `fail` for reasons unrelated to the code ("cannot get the head's
# tree", "no answer posted"), so every pull request landed on the maintainer at
# the cap having spent five model passes. armaatus/autofleet#152.
#
# Exit codes:
#   0  done with this PR: approved, or parked with a reason on it
#   2  could not tell what to do: no PR, or gh would not say the repository
#   3  the fleet is stopped; nothing goes out
#   5  something went wrong that the next poll should retry
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/fleet/lib.sh

pr="${1:-}"
[ -n "$pr" ] || { echo "usage: after-pr.sh <pr>" >&2; exit 2; }

if fleet_stopped; then
  echo "after-pr.sh: ~/.autofleet/STOP exists; nothing goes out." >&2
  exit 3
fi

mkdir -p "$FLEET_REVIEWING" || {
  echo "after-pr.sh: cannot write $FLEET_REVIEWING" >&2; exit 2; }
REVIEWS="$FLEET_REVIEWING/$pr.reviews"
# ...and the fix session's own ceiling, beside the reviews'. The loop's SHAPE is
# one fix, and that is not the same as a bound: a re-review that exits 8 is
# refunded, so the next poll re-enters with one review spent, gets
# `request-changes` again, and buys a SECOND full-budget fix session. A record
# holds it the way the review ceiling is held. Found by the local /code-review
# pass.
FIXES="$FLEET_REVIEWING/$pr.fixes"

# THE DISPATCHER'S BOOKKEEPING, done here rather than in a wrapper around this
# script. `fleet.sh` has to exec this directly -- a `( ... ) &` subshell keeps
# the dispatcher's own argv, and the liveness probe that reads the lock's pid
# out of `ps` then calls a live loop dead and starts a second one beside it. So
# the two things a wrapper would have done arrive as environment instead.
#
# `.done` ON EXIT 0 ONLY, which is the asymmetry that decides whether a pull
# request is ever looked at again: 0 means FINISHED with this head -- approved,
# or parked with a reason on it -- and anything else is a retry the next poll
# should make. Writing it on both would strand a PR on one `gh` outage; writing
# it on neither is the re-spawn loop armaatus/autofleet#42 exists to remove.
# What bounds the retry is the review ceiling above, which is not refunded for
# a reviewer that actually ran.
LOCK="${AUTOFLEET_PR_MARKER:-}"
LOCK_HEAD="${AUTOFLEET_PR_HEAD:-}"
# THE CHILD CLAIMS ITS OWN LOCK, which is armaatus/autofleet#64's shape and the
# only one that closes the window: the dispatcher cannot claim before it has a
# pid to write, and it cannot have a pid before it has forked. Claiming here
# makes the marker name THIS process from the first moment it exists, so a
# second dispatcher -- or a person running this by hand -- loses the race
# cleanly instead of running beside it. `fleet_lock_publish` from the parent
# afterwards is a no-op when this got there first, and the fallback when it did
# not.
if [ -n "$LOCK" ] && ! fleet_lock_claim "$LOCK" "$LOCK_HEAD"; then
  echo "after-pr.sh: PR #$pr already has a post-PR loop in flight; standing down." >&2
  exit 0
fi
on_exit() {
  local rc=$?
  if [ "$rc" = 0 ] && [ -n "$LOCK" ] && [ -n "$LOCK_HEAD" ]; then
    printf '%s\n' "$LOCK_HEAD" >"$LOCK.done" \
      || echo "after-pr.sh: could not write $LOCK.done; this PR is re-examined every poll" >&2
  fi
  fleet_lock_release "$LOCK"
}
trap on_exit EXIT

# THE MERGE IS QUEUED FIRST, not last.
#
# GitHub refuses to queue auto-merge on a pull request that is ALREADY
# mergeable -- "Pull request is in clean status" -- and nothing here may merge
# directly, so a PR that goes green before anything queued it has nobody left to
# merge it. It sits clean and untouched forever, which is what #90 did. Queued
# now it simply waits, and fires the moment the last required check passes.
#
# Idempotent by asking first: `--auto` on a PR that already has it queued is an
# error, and an error a poll is a log nobody reads.
armed="$(GH_PAGER=cat gh pr view "$pr" --json autoMergeRequest \
           --jq '.autoMergeRequest != null' 2>/dev/null)"
if [ "$armed" != true ]; then
  if GH_PAGER=cat gh pr merge "$pr" --auto --squash >/dev/null 2>&1; then
    echo "after-pr.sh: PR #$pr queued for auto-merge."
  else
    # NOT FATAL. A repository without auto-merge enabled, or a PR GitHub will
    # not queue yet, is a thing a person fixes -- and the review below is worth
    # having either way, on a PR somebody is going to merge by hand.
    echo "after-pr.sh: could not queue auto-merge for PR #$pr; carrying on." >&2
  fi
fi

# `review.sh`, with the ceiling counted first. Prints its own reasoning; this
# only decides what the exit code means.
bought() {
  local n; n="$(cat "$REVIEWS" 2>/dev/null)"
  case "${n:-}" in ''|*[!0-9]*) n=0 ;; esac
  printf '%s\n' "$n"
}
review_once() {
  local n rc; n="$(bought)"
  if [ "$n" -ge 2 ]; then
    echo "after-pr.sh: PR #$pr has had its two reviews; not buying a third." >&2
    return 9
  fi
  # Written BEFORE the run. A reviewer that crashes has still been bought, and
  # the alternative -- counting on success -- is a crash loop that re-reviews
  # every poll at full budget, which is the failure this ceiling exists for.
  printf '%s\n' "$((n + 1))" >"$REVIEWS" \
    || echo "after-pr.sh: could not write $REVIEWS; the review cap is not counting" >&2
  ./scripts/fleet/review.sh "$pr"; rc=$?
  # ...AND REFUNDED WHEN NO MODEL RAN. `review.sh` has four exits that spend
  # nothing: 2 (it could not tell what to review, before any model ran), 3 (the
  # fleet is stopped), 6 (its command is not on PATH) and 8 (a verdict for this
  # head is already posted). Counted, they exhaust the ceiling of two without a
  # single model call -- and 8 is the ORDINARY one: a dispatcher that lost its
  # `<pr>.done` record, a second machine, or a pull request a person reviewed by
  # hand all reach it, twice, and then the next real review is refused with "it
  # has had its two".
  #
  # What is NOT refunded is 5, 7 and 10: a reviewer that ran and produced
  # nothing, one killed at its deadline, and one whose verdict GitHub would not
  # take. All three cost what a review costs, and the whole point of the ceiling
  # is that they cannot be retried forever. 10 exists BECAUSE it used to be a 2:
  # a post that fails after the model has run was refunded like a pre-model
  # failure, so a repository where `gh pr review` cannot work -- a token without
  # PR-write, reviews disabled -- bought a full reviewer every poll, forever.
  # Found by the independent review.
  case "$rc" in
    2|3|6|8) printf '%s\n' "$n" >"$REVIEWS" \
               || echo "after-pr.sh: could not refund $REVIEWS" >&2 ;;
  esac
  return "$rc"
}

# Does the verdict standing on this head ask for changes? Asked of GitHub, and
# through the same marker `merge_gate.py` reads, so "what was decided" has one
# answer rather than one per reader.
verdict_asks_for_changes() {
  # The repository, resolved HERE rather than at the top of this script: this is
  # the only thing in the loop that needs it, and it is reached on one exit out
  # of six. `fleet_pr_payload` reads `$fleet_owner` and `$fleet_repo_name`, and
  # under `set -u` an unresolved pair is not a soft failure -- it is the
  # function erroring out and this answering "no changes asked for", which is
  # the write-off it exists to prevent.
  fleet_owner_repo || {
    echo "after-pr.sh: gh would not say which repository this is, so what the" >&2
    echo "  standing verdict says is unknown; leaving PR #$pr for the next poll." >&2
    return 1; }
  local payload; payload="$(mktemp)"
  fleet_pr_payload "$pr" "$payload" || { rm -f "$payload"; return 1; }
  local head; head="$(GH_PAGER=cat gh pr view "$pr" --json headRefOid \
                        --jq .headRefOid 2>/dev/null)"
  python3 - "$payload" "${head:-}" <<'ASKS'
import json, re, sys
doc = json.load(open(sys.argv[1]))
head = sys.argv[2]
pull = doc["data"]["repository"]["pullRequest"]
marker = re.compile(r"<!--\s*autofleet-verdict:\s*request-changes\s+([0-9a-f]{7,40})\s*-->", re.I)
for review in (pull.get("reviews") or {}).get("nodes") or []:
    on_head = ((review.get("commit") or {}).get("oid") or "") == head
    if on_head and review.get("state") == "CHANGES_REQUESTED":
        raise SystemExit(0)
    found = marker.search(review.get("body") or "")
    if found and head.startswith(found.group(1)):
        raise SystemExit(0)
raise SystemExit(1)
ASKS
  local rc=$?
  rm -f "$payload"
  return "$rc"
}

park() {
  GH_PAGER=cat gh pr comment "$pr" --body "$1" >/dev/null 2>&1 \
    || echo "after-pr.sh: could not comment on PR #$pr." >&2
  echo "after-pr.sh: PR #$pr parked -- $2"
}

review_once; rc=$?
case "$rc" in
  0) echo "after-pr.sh: PR #$pr approved; GitHub decides the rest."; exit 0 ;;
  4) ;;                       # changes requested -- the fix is below
  3) exit 3 ;;
  8) # A VERDICT FOR THIS HEAD ALREADY EXISTS, and which way it went decides
     # whether there is anything left to do. Read, not assumed: exiting 0 here
     # unconditionally wrote `<pr>.done` and the dispatcher then skipped the
     # pull request forever -- correct for an approve, and a write-off for a
     # request-changes this fleet posted and was interrupted before answering.
     # That is an ordinary path, not an edge one: the first review asks for
     # changes, `fix.sh` meets `~/.autofleet/STOP` and exits 3, and on resume
     # the head has not moved, so the next poll sees the standing refusal and
     # calls the PR finished. It never gets the one fix session the loop
     # promises and never gets the comment every other dead end posts. Found by
     # the independent review.
     if verdict_asks_for_changes; then
       echo "after-pr.sh: PR #$pr already carries a request-changes for its"
       echo "  current head; answering it rather than calling it finished."
     else
       echo "after-pr.sh: PR #$pr already judged at its current head."
       exit 0
     fi ;;
  9) park "**autofleet: needs a human.** This pull request has had the two
reviews the loop allows and is still not approved. Nothing further is
automatic: read the reviews above, or close this and re-open the issue with
what the reviews found written into its Scope." "at the review ceiling"
     exit 0 ;;
  *) echo "after-pr.sh: review.sh exited $rc; leaving PR #$pr for the next poll." >&2
     exit 5 ;;
esac

# ONE FIX SESSION, and the record is what says so rather than the shape of this
# script. Written before the run, for the reason the review count is.
fixes="$(cat "$FIXES" 2>/dev/null)"
case "${fixes:-}" in ''|*[!0-9]*) fixes=0 ;; esac
if [ "$fixes" -ge 1 ]; then
  park "**autofleet: needs a human.** This pull request has had the one fix
session the loop allows and its review still asks for changes. Both reviews are
above." "the fix ceiling"
  exit 0
fi
printf '%s\n' "$((fixes + 1))" >"$FIXES" \
  || echo "after-pr.sh: could not write $FIXES; the fix cap is not counting" >&2
./scripts/fleet/fix.sh "$pr"; rc=$?
case "$rc" in
  0) ;;
  3) exit 3 ;;
  5) park "**autofleet: needs a human.** The review asked for changes, the one
fix session this pull request gets ran, and nothing was pushed. The findings
are in the review above." "the fix pushed nothing"
     exit 0 ;;
  *) park "**autofleet: needs a human.** The review asked for changes and the
fix session could not run (\`fix.sh\` exited $rc). The findings are in the
review above." "the fix could not run"
     exit 0 ;;
esac

# THE SECOND REVIEW, AND THE LAST. Its verdict is final in both directions:
# approve and GitHub merges, request-changes and a person takes it.
review_once; rc=$?
case "$rc" in
  0) echo "after-pr.sh: PR #$pr approved after one fix; GitHub decides the rest."
     exit 0 ;;
  3) exit 3 ;;
  4|9) park "**autofleet: needs a human.** The fix answering the first review
was reviewed and still asks for changes. That is the second and last review this
pull request gets -- another lap is not what a disagreement needs. Both reviews
are above." "a second request-changes"
     exit 0 ;;
  *) echo "after-pr.sh: the re-review exited $rc on PR #$pr." >&2; exit 5 ;;
esac
