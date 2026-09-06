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
#
# The round cap is counted HERE rather than left to the agent to remember. Three
# rounds is more than almost any PR needs, and a fourth is not what a
# disagreement needs -- a person is. The count lives in .orca/review-rounds,
# which is per-worktree and gitignored, and resets when the PR number changes.
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

# Reviews already present are not the answer to the push just made. The cut-off
# is the current head's own commit time rather than "now": a review submitted
# while this script was starting still belongs to this round.
#
# In UTC, and with a Z, because the comparison below is a STRING comparison
# against GitHub's `submittedAt`, which is always UTC. A local-offset stamp
# (`...T14:00:00+02:00`) compares wrongly against `...T12:05:00Z` -- the review
# that arrived five minutes later sorts earlier, `fresh` stays empty, and every
# worktree on a machine east of UTC waits out the full deadline and exits 4.
since="$(TZ=UTC0 git log -1 --format=%cd --date=format:%Y-%m-%dT%H:%M:%SZ HEAD)"

echo "round $round of $MAX_ROUNDS -- waiting for a review on PR #$pr after $since"
echo "  (polling every ${POLL_SECONDS}s; stop everything with ./scripts/orca/stop.sh)"

payload="$(mktemp)"; trap 'rm -f "$payload"' EXIT
waited=0
checks_due=0
broken_before=""
# Set from the rollup inside the throttled block below; declared here so a poll
# that skips the block still has a value under `set -u`.
review_dead=""
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
    rollup="$(GH_PAGER=cat gh pr view "$pr" --json statusCheckRollup 2>/dev/null \
              | python3 -c "
import json, sys
try:
    checks = json.load(sys.stdin).get('statusCheckRollup') or []
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
" 2>/dev/null)"
    # Two lines out of one capture: the failing check names, then the marker
    # saying the review check itself is among the dead. Read from `rollup` rather
    # than from `broken` -- reusing one name as both the here-string source and
    # the first read target works, but reads like a bug.
    { IFS= read -r broken; IFS= read -r review_dead; } <<<"$rollup" || true
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
  failed_run=""
  if [ -n "$review_dead" ]; then
    failed_run="$(GH_PAGER=cat gh run list --branch "$branch" --workflow "claude review" \
                    --limit 1 --json conclusion,databaseId,headSha \
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

  # Written to a file and read back, never spliced into a Python source string:
  # a review body is third-party text.
  if GH_PAGER=cat gh pr view "$pr" --json reviews,reviewDecision >"$payload" 2>/dev/null; then
    if body="$(python3 - "$since" "$payload" <<'PY'
import json, sys
since, path = sys.argv[1], sys.argv[2]
d = json.load(open(path))
fresh = [r for r in d.get("reviews") or [] if (r.get("submittedAt") or "") > since]
if not fresh:
    raise SystemExit(1)
for r in fresh:
    who = (r.get("author") or {}).get("login", "?")
    print(f"--- {r.get('state')} by {who} at {r.get('submittedAt')}")
    print(r.get("body") or "(no body; see the inline comments)")
    print()
PY
)"; then
      echo
      echo "$body"
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
exit 4
