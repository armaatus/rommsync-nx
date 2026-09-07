#!/usr/bin/env bash
# Block until the PR's independent review lands, then print it.
#
# This is the cheap half of the loop. An agent waiting for a review by thinking
# about whether the review has arrived yet burns tokens the whole time and gets
# slower the longer it waits. An agent waiting inside ONE tool call burns
# nothing: the session is suspended in a `Bash` call until this returns.
#
# So: poll `gh`, print the review, exit. No webhook, no ingress, no daemon.
#
#   ./scripts/orca/await-review.sh            # the PR for this worktree's branch
#   ./scripts/orca/await-review.sh 75
#
# Exit codes, so the caller can tell the cases apart:
#   0  a review is in hand
#   2  no PR to wait on
#   3  the fleet was stopped while waiting
#   4  nothing arrived before the deadline -- look at Actions
#   5  the third round is over; stop and say what is unresolved
#   6  the review job failed on this commit; the reason is printed
#   7  the PR's build is red; a review cannot fix that
#   8  the PR conflicts with its base; a review cannot fix that either
#
# The round cap is counted HERE rather than left to the agent to remember. Three
# rounds is more than almost any PR needs, and a fourth is not what a
# disagreement needs -- a person is. The count lives in .orca/review-rounds,
# which is per-worktree and gitignored, and resets when the PR number changes.
#
# WHAT COUNTS AS THE REVIEW IS NOT DECIDED HERE. This imports
# .github/scripts/merge_gate.py and asks it, exactly as review-status.sh does,
# because a wait that ends on a review merge-gate does not count ends it for
# nothing: the agent reads its own thread reply back as "the review", spends one
# of its three rounds on it, and the PR then sits BLOCKED for the reason the
# wait just called satisfied. That was #114, and it needed the GraphQL query
# below -- `gh pr view --json reviews` reports neither the review's commit nor
# enough to tell the author's own record from a reviewer's.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

POLL_SECONDS="${AWAIT_REVIEW_POLL:-30}"
# Long enough for CI plus a review; short enough that a wedged workflow is not
# an overnight wait. The review job itself is capped at 30 minutes.
DEADLINE_SECONDS="${AWAIT_REVIEW_DEADLINE:-2700}"
MAX_ROUNDS="${AWAIT_REVIEW_MAX_ROUNDS:-3}"
ROUNDS_FILE="$REPO_ROOT/.orca/review-rounds"
# Exit 8 is the one failure path with no natural bound: it fires on poll 1, so
# the deadline never applies, and a conflict is not a round of disagreement so
# the round cap must not count it. What CAN be bounded is the case where nothing
# changed -- keyed on the local head, because a real rebase moves it and a
# forgotten force-push does not.
CONFLICT_FILE="$REPO_ROOT/.orca/conflict-head"

pr="${1:-}"
branch="$(git rev-parse --abbrev-ref HEAD)"
head="$(git rev-parse HEAD)"
[ -n "$pr" ] || pr="$(orca_pr_for_branch)" || {
  echo "no open PR for branch $branch" >&2; exit 2; }

mkdir -p "$REPO_ROOT/.orca"
round=0
if [ -r "$ROUNDS_FILE" ]; then
  read -r seen_pr seen_round <"$ROUNDS_FILE" 2>/dev/null || true
  [ "${seen_pr:-}" = "$pr" ] && round="${seen_round:-0}"
fi
round=$((round + 1))

# The count is written only on the path that actually READ a review (exit 0
# below). A round the reviewer never answered is not a round of disagreement --
# three CI timeouts in a row must not exhaust the cap without a single finding
# having been seen.
record_round() { printf '%s %s\n' "$pr" "$round" >"$ROUNDS_FILE"; }

if [ "$round" -gt "$MAX_ROUNDS" ]; then
  cat <<CAP
This is round $round on PR #$pr, and the cap is $MAX_ROUNDS.

Stop here. Comment on the PR saying exactly what is still unresolved and why you
disagree with it, set the worktree comment to "needs you -- $MAX_ROUNDS review
rounds", and stop. Another lap is not what a disagreement needs.
CAP
  exit 5
fi

# Reviews already present are not the answer to the push just made, and what
# separates the two is the COMMIT, not a timestamp. A review cannot be submitted
# against a commit that does not exist yet, so "on this head" is strictly
# stronger than any freshness cut-off this script could compute -- and unlike a
# cut-off it cannot be wrong.
#
# It used to compare `submittedAt` against the HEAD commit's own time, which is
# not when the branch was pushed. Commit at T0, push at T0+3min, and a review
# from the PREVIOUS round submitted at T0+1min passes that test (#114). The
# commit time was never the push time and there is no cheap way to ask GitHub
# for the push time; the head makes the question unnecessary.
# Checked for emptiness as well as for gh's exit status. An owner of "" builds a
# query that can never match, and this loop would then poll it for the full 45
# minutes and report that no review arrived -- the same silent, wrong answer the
# rest of this script exists to stop giving.
owner_repo="$(GH_PAGER=cat gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)"
case "$owner_repo" in
  */*) ;;
  *) echo "could not read this repository's name from gh (got \"$owner_repo\"); nothing here can ask about the reviews" >&2
     exit 2 ;;
esac
owner="${owner_repo%%/*}"; name="${owner_repo##*/}"

echo "round $round of $MAX_ROUNDS -- waiting for a review on PR #$pr, on its current head"
echo "  (polling every ${POLL_SECONDS}s; stop everything with ./scripts/orca/stop.sh)"

payload="$(mktemp)"; reviews_out="$(mktemp)"
trap 'rm -f "$payload" "$reviews_out"' EXIT
waited=0
checks_due=0
broken_before=""
# Set when a poll saw review records on this head that none of the gate rules
# count. Only ever read by the timeout message below.
discounted=""
# Set from the rollup inside the throttled block below; declared here so a poll
# that skips the block still has a value under `set -u`.
review_dead=""
merge_state=""
base_ref=""
while [ "$waited" -lt "$DEADLINE_SECONDS" ]; do
  if orca_fleet_stopped; then
    echo
    echo "STOPPED: $ORCA_FLEET_STOP exists. Put the work down and report where you got to."
    exit 3
  fi

  # A PR whose build is red does not need a review, it needs a fix. Waiting
  # here for one is how #88 sat parked while `host-tests` was failing on its own
  # new test -- the agent watching for a review that could never help. Ignores
  # merge-gate (red until a review exists, by design) and the review job itself
  # (handled just below).
  # Every fourth poll, and acted on only when TWO consecutive checks agree.
  #
  # Every fourth because these are extra `gh` calls on a 30-second loop that can
  # run 45 minutes, and this repo is careful about gh's secondary rate limit
  # (see count_startable in fleet.sh).
  #
  # Twice because `harness.partial` is a known intermittent race (#76, reopened)
  # that runs inside host-tests. A single sighting is not evidence the PR is
  # broken, and sending an agent to fix something that is not theirs costs it a
  # whole cycle. The comparison is between CHECKS, not polls -- an earlier
  # version reset its memory on the polls in between and could never see the
  # same failure twice.
  checks_due=$((checks_due + 1))
  if [ "$((checks_due % 4))" = "1" ]; then
    # `mergeStateStatus` and `baseRefName` ride along on the rollup call rather
    # than costing a second one -- the same reasoning as everything else in this
    # block, and #83's finding was an unthrottled check exactly here. The base
    # comes from the PR because the rebase command below is printed for an agent
    # to run: `origin/main` is right for every PR the fleet opens today and wrong
    # the first time one is stacked on another, and a wrong command is the class
    # of failure this whole exit exists to stop.
    rollup="$(GH_PAGER=cat gh pr view "$pr" --json statusCheckRollup,mergeStateStatus,baseRefName 2>/dev/null \
              | python3 -c "
import json, sys
try:
    doc = json.load(sys.stdin)
    checks = doc.get('statusCheckRollup') or []
except Exception:
    raise SystemExit
skip = ('merge-gate', 'review against REVIEW.md')
dead = ('FAILURE', 'TIMED_OUT', 'ACTION_REQUIRED')
bad = [c.get('name') for c in checks
       if (c.get('conclusion') or '') in dead and c.get('name') not in skip]
review_dead = any(c.get('name') == 'review against REVIEW.md'
                  and (c.get('conclusion') or '') in dead for c in checks)
print(', '.join(n for n in bad if n))
print('REVIEW_FAILED' if review_dead else '')
print((doc.get('mergeStateStatus') or '').upper())
print(doc.get('baseRefName') or '')
" 2>/dev/null)"
    # Four lines out of one capture: the failing check names, the marker saying
    # the review check itself is among the dead, what GitHub makes of the branch
    # against its base, and the base it judged that against. Read from `rollup`
    # rather than from `broken` -- reusing one name as both the here-string
    # source and the first read target works, but reads like a bug.
    #
    # Cleared first, and that is deliberate. `$(...)` strips ALL trailing
    # newlines, so an answer whose tail lines are empty comes back short and the
    # later `read`s hit EOF. bash assigns the empty line it did not get and
    # returns non-zero, so those variables do end up empty (verified on 3.2.57
    # and 5.3.15, the oldest and newest bash this repo can meet). But the
    # recovery path depends entirely on that: if `read` left the variable
    # untouched on EOF, a `REVIEW_FAILED` set on one throttle check would survive
    # every later one, and the `gh run list` below would run on every poll for
    # the rest of the 45-minute wait -- reintroducing exactly the cost the
    # throttle above exists to remove. One assignment makes the recovery explicit
    # instead of a consequence of how `read` handles EOF.
    # `await_stops_paying_once_the_review_recovers` pins it.
    review_dead=""
    merge_state=""
    base_ref=""
    { IFS= read -r broken; IFS= read -r review_dead
      IFS= read -r merge_state; IFS= read -r base_ref; } <<<"$rollup" || true

    # One capture now feeds THREE detections -- the red build, the dead review
    # job and the conflict -- so a rollup that comes back empty turns all three
    # off at once and the wait runs its full deadline saying nothing arrived:
    # #80, #88 and #99 simultaneously, and silently, because `2>/dev/null`
    # swallowed whatever gh said. Nothing here can tell a rejected --json field
    # from a rate limit from a network blip, and none of them is worth aborting
    # a 45-minute wait over. Saying it once is: it turns an invisible failure
    # into one line an agent can act on.
    if [ -z "$rollup" ] && [ -z "${rollup_warned:-}" ]; then
      rollup_warned=1
      echo "  note: the check rollup for PR #$pr came back empty -- the red-build," >&2
      echo "  failed-review and conflict checks are blind while that lasts. If this" >&2
      echo "  persists, run it by hand to see the error this loop discards:" >&2
      echo "    gh pr view $pr --json statusCheckRollup,mergeStateStatus,baseRefName" >&2
    fi

    # A conflict with the base is not something a review can answer, and it
    # blocks every merge -- a person's included. #99 sat here for the full 45
    # minutes, got its review, resolved its threads and still could not merge,
    # because this was the blocker the whole time.
    #
    # Acted on from ONE sighting, unlike the red build below: `DIRTY` is not a
    # flake, and a branch that conflicts has to be rebased whatever else is true.
    # Anything else -- `UNKNOWN` while GitHub is still computing mergeability,
    # `BLOCKED`, `BEHIND` -- is left to review-status.sh, which sees the whole
    # picture; only the state that makes waiting pointless exits here.
    if [ "$merge_state" = "DIRTY" ]; then
      seen_head=""
      [ -r "$CONFLICT_FILE" ] && read -r seen_head <"$CONFLICT_FILE" 2>/dev/null
      printf '%s\n' "$head" >"$CONFLICT_FILE"
      cat <<CONFLICT

GitHub says DIRTY: this PR conflicts with its base.

No review will fix a merge conflict, and a conflicted branch cannot merge at
all. Rebase it:
  git fetch origin && git rebase origin/${base_ref:-main}
resolve the conflicts, re-run the build and the tests, then run
./scripts/orca/record-review.sh for the new head -- the marker is per-commit and
a rebase changes every sha, so the guard refuses the push without a fresh one.
Then push the rewritten branch:
  git push --force-with-lease
and come back here.
CONFLICT
      if [ "$seen_head" = "$head" ]; then
        cat <<AGAIN

This is the SECOND time on commit $head. Nothing about the branch changed
between them, so the rebase either did not happen or was never pushed -- this
script reads GitHub's view of the PR, not your working tree. Check that
\`git status\` is clean and that \`git push --force-with-lease\` actually ran
before coming back; another lap on the same commit gets the same answer.
AGAIN
      fi
      exit 8
    fi
    if [ -n "$broken" ] && [ "$broken" = "$broken_before" ]; then
      cat <<RED

CI is failing on this PR, on two consecutive checks: $broken

No review will fix a red build. Reproduce it locally:
  ctest --test-dir build --output-on-failure
then fix it, re-run the local reviews, ./scripts/orca/record-review.sh for the
new commit, push, and come back here.

One thing to rule out first: if the only failure is harness.partial, that is a
known intermittent race (#76) and NOT yours. Re-run the job rather than
changing code:  gh run rerun <run-id>
RED
      exit 7
    fi
    broken_before="$broken"
  fi

  # A review job that FAILED is not a review that is late. Waiting out the full
  # deadline for one costs 45 minutes and then says only "nothing arrived" --
  # which is what happened on PR #80, where the reviewer had already died on
  # `Reached maximum number of turns (30)` four minutes in. Say the real reason
  # immediately.
  #
  # The rollup fetched just above already knows the check is dead, so this asks
  # it rather than spending a `gh run list` on every poll. That mattered: an
  # earlier version ran two extra calls per 30-second poll for up to 45 minutes,
  # times three worktrees, against the same secondary rate limit the throttle
  # above exists to respect -- and a rate-limited answer is indistinguishable
  # from "nothing yet", which is precisely how #80 defeated the old check.
  # Nothing is spent until there is a failure to explain.
  # `--limit 25`, not 1, and that is the whole check working at all. The review
  # workflow fires on `pull_request_review` and `pull_request_review_comment` as
  # well as on the push, and those runs no-op with `skipped` -- so a run that
  # genuinely FAILED is buried under every skipped run posted since. On this PR
  # that was not an edge case but the shape of every head: each one's newest
  # `claude review` run was a skipped review-event run, with the real one four
  # or five entries down. `--limit 1` would have found nothing, fallen through
  # to "no failed run found", and waited out the full 45 minutes to report that
  # nothing arrived -- #80 exactly, the failure this check was written to end,
  # reintroduced through the list window instead of through a rate limit.
  # The `--jq` already selects on this head AND conclusion == failure, so the
  # only job of the limit is to make sure the matching run is inside the window.
  #
  # Throttled on the SAME poll as the rollup that set `review_dead`, not merely
  # on `review_dead` being true. The usual path exits 6 on the first try and the
  # distinction never shows; it shows when the lookup comes back empty and
  # `review_dead` stays true -- a transient error swallowed by `2>/dev/null`, or
  # a failure sitting further back than the window. Gated on the flag alone this
  # would then re-ask on every poll until the next throttled recheck, and for as
  # long as the run stays unfound, which is the per-poll cost the throttle exists
  # to prevent -- moved one call downstream of the fix rather than removed.
  # A slow-to-appear run now costs one extra call per throttle cycle.
  failed_run=""
  if [ -n "$review_dead" ] && [ "$((checks_due % 4))" = "1" ]; then
    failed_run="$(GH_PAGER=cat gh run list --branch "$branch" --workflow "claude review" \
                    --limit 25 --json conclusion,databaseId,headSha \
                    --jq "[.[] | select(.headSha==\"$head\" and .conclusion==\"failure\")][0].databaseId" \
                  2>/dev/null)"
  fi
  if [ -n "$failed_run" ] && [ "$failed_run" != "null" ]; then
    echo
    echo "The review job FAILED on this commit -- it is not coming. Run $failed_run:"
    GH_PAGER=cat gh run view "$failed_run" --log 2>/dev/null \
      | grep -iE "\[error\]|maximum number of turns|validation|not installed|OIDC" \
      | sed 's/^/    /' | cut -c1-200 | head -5
    echo
    echo "Fix the cause, push, and run this again. Do not wait for it."
    exit 6
  fi

  # GraphQL rather than `gh pr view --json reviews`, which can report neither the
  # commit a review was submitted against nor the PR's own author to compare a
  # reviewer against -- the two things that decide whether this is the review
  # being waited for. `headRefOid` comes from the same answer, so the head judged
  # here is the one merge-gate will judge, not whatever is in the working tree.
  #
  # Written to a file and read back, never spliced into a Python source string:
  # a review body is third-party text.
  if GH_PAGER=cat gh api graphql -F owner="$owner" -F name="$name" -F pr="$pr" -f query='
query($owner:String!,$name:String!,$pr:Int!){
  repository(owner:$owner,name:$name){
    pullRequest(number:$pr){
      headRefOid
      author{login}
      reviews(last:50){ nodes{ state submittedAt commit{oid} author{login}
                               body comments(first:1){ totalCount } } }
    }
  }
}' >"$payload" 2>/dev/null; then
    # Its output goes to a file rather than into `$(...)`, and that is not
    # style. bash 3.2 -- which macOS still ships, and which this repo therefore
    # has to parse under -- tracks single quotes while scanning for the closing
    # paren of a command substitution, THROUGH a quoted heredoc. One apostrophe
    # in a comment inside the block below is enough to make the whole script a
    # syntax error, and the message it gives names neither the line nor the
    # quote. Outside `$(...)` the heredoc is just a heredoc.
    python3 - "$payload" "$head" >"$reviews_out" <<'PY'
import json, sys

sys.path.insert(0, ".github/scripts")
try:
    from merge_gate import independent_reviews, is_substantive
except Exception as exc:  # missing, half-edited, or broken at import time
    # Not ImportError alone: merge_gate.py is a file agents in this repo edit,
    # and a SyntaxError in it must not read as "no review yet" for the rest of a
    # 45-minute wait. Said once per poll on stderr and then waited out, because
    # there is nothing here that could answer instead.
    print(f"  note: could not load .github/scripts/merge_gate.py ({exc}), which "
          "decides what counts as a review. Nothing can end this wait until it "
          "imports.", file=sys.stderr)
    raise SystemExit(1)

path, local_head = sys.argv[1], sys.argv[2]
try:
    pull = json.load(open(path))["data"]["repository"]["pullRequest"] or {}
except Exception:
    raise SystemExit(1)

head = pull.get("headRefOid") or ""
if not head:
    raise SystemExit(1)
if head != local_head:
    # Not fatal: the head GitHub reports is still the one being reviewed and
    # the one merge-gate will judge. But an agent that forgot to push is
    # waiting for a review of code it did not send, and nothing else here
    # would ever say so.
    print(f"  note: this worktree is on {local_head[:8]} and the PR's head is "
          f"{head[:8]} -- there is something unpushed. The review being waited "
          "for is of what GitHub has.", file=sys.stderr)

# The rules the gate itself uses, imported rather than paraphrased: not by the
# PR author, on this head, and carrying something to act on.
on_head = independent_reviews(pull, head)
reviews = [r for r in on_head if is_substantive(r)]
if not reviews:
    # 3 rather than 1 when review RECORDS exist on this head and none of them
    # counted. Nothing to act on either way, so the wait continues -- but a
    # timeout after this has a real reason to give, and "nothing arrived" would
    # be the wrong one. Counted from every record on the head, the PR's own
    # author included, because the record an agent most often mistakes for a
    # review is the one its own thread reply created.
    records = [r for r in ((pull.get("reviews") or {}).get("nodes") or [])
               if ((r.get("commit") or {}).get("oid") == head)]
    raise SystemExit(3 if records else 1)
for r in reviews:
    who = (r.get("author") or {}).get("login", "?")
    print(f"--- {r.get('state')} by {who} at {r.get('submittedAt')}")
    print(r.get("body") or "(no body; see the inline comments)")
    print()
PY
    verdict=$?
    # 3 is "records on this head, none of them a review" -- remembered for the
    # timeout message rather than repeated on all ninety polls.
    [ "$verdict" = 3 ] && discounted=1
    if [ "$verdict" = 0 ]; then
      echo
      cat "$reviews_out"
      echo "--- inline comments"
      GH_PAGER=cat gh api "repos/{owner}/{repo}/pulls/$pr/comments" \
        --jq '.[] | "\(.path):\(.line // .original_line)  \(.user.login)\n\(.body)\n"' \
        2>/dev/null | head -200
      echo
      echo "Fix what is real. Where you disagree, reply on the thread with the reason"
      echo "rather than ignoring it. Resolve every thread, push, and re-request review"
      echo "-- the push itself re-runs the reviewer. Then:"
      echo "  ./scripts/orca/review-status.sh $pr"
      record_round
      exit 0
    fi
  fi

  sleep "$POLL_SECONDS"
  waited=$((waited + POLL_SECONDS))
done

cat <<TIMEOUT

No review arrived in $((DEADLINE_SECONDS / 60)) minutes.

Silence is the failure mode here -- the review job is continue-on-error, so a
broken review looks like a green run with no comments. Check:
  gh run list --branch $(git rev-parse --abbrev-ref HEAD) --limit 5
and whether CLAUDE_CODE_OAUTH_TOKEN is set as a repository secret.
TIMEOUT
if [ -n "$discounted" ]; then
  cat <<DISCOUNTED
Review RECORDS were submitted against this head, and none of them is a review
merge-gate would count: a record by the PR's own author (every reply to a review
thread creates one), or one with no body worth reading and no inline comment.
This waited rather than handing one back, because merge-gate would have refused
the PR straight afterwards for the reason the wait had just called satisfied.
DISCOUNTED
fi
exit 4
