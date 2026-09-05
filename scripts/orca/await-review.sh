#!/usr/bin/env bash
# Block until the PR's independent review lands, then print it.
#
# This is the cheap half of the loop. An agent waiting for a review by thinking
# about whether the review has arrived yet burns tokens the whole time and gets
# slower the longer it waits. An agent waiting inside ONE tool call burns
# nothing: the session is suspended in a `Bash` call until this script returns.
#
# So: poll `gh`, print the review, exit. No webhook, no ingress, no daemon.
#
#   ./scripts/orca/await-review.sh            # the PR for this worktree's branch
#   ./scripts/orca/await-review.sh 75         # a specific PR
#   ./scripts/orca/await-review.sh 75 --since 2026-09-05T12:00:00Z
#
# Exits 0 when a review is in hand, 3 when the fleet was stopped while waiting,
# 4 on timeout. The caller can tell those apart, which matters: a timeout means
# look at Actions, a stop means put the work down.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

STATE_DIR="${ROMMSYNC_FLEET_DIR:-$HOME/.rommsync-fleet}"
STOP_FILE="$STATE_DIR/STOP"
POLL_SECONDS="${AWAIT_REVIEW_POLL:-30}"
# Long enough for CI plus a review; short enough that a wedged workflow is not
# an overnight wait. The review job itself is capped at 30 minutes.
DEADLINE_SECONDS="${AWAIT_REVIEW_DEADLINE:-2700}"

pr="${1:-}"
since=""
[ "${1:-}" = "--since" ] && { pr=""; }
while [ "$#" -gt 0 ]; do
  case "$1" in
    --since) since="$2"; shift 2 ;;
    [0-9]*)  pr="$1"; shift ;;
    *)       echo "usage: $0 [PR] [--since <iso8601>]" >&2; exit 2 ;;
  esac
done

if [ -z "$pr" ]; then
  branch="$(git rev-parse --abbrev-ref HEAD)"
  pr="$(GH_PAGER=cat gh pr list --head "$branch" --state open --json number \
          --jq '.[0].number' 2>/dev/null)"
  [ -n "$pr" ] && [ "$pr" != "null" ] \
    || { echo "no open PR for branch $branch" >&2; exit 2; }
fi

# Reviews already present when we start are not the answer to the push we just
# made. Default the cut-off to now unless the caller names an earlier one.
[ -n "$since" ] || since="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

echo "waiting for a review on PR #$pr submitted after $since"
echo "  (polling every ${POLL_SECONDS}s; stop the whole fleet with ./scripts/orca/fleet.sh stop)"

waited=0
while [ "$waited" -lt "$DEADLINE_SECONDS" ]; do
  if [ -e "$STOP_FILE" ]; then
    echo
    echo "STOPPED: $STOP_FILE exists. Put the work down and report where you got to."
    exit 3
  fi

  # Reviews first: the workflow submits a real review, so `reviewDecision` is
  # the state to read rather than a comment body to grep.
  body="$(GH_PAGER=cat gh pr view "$pr" \
            --json reviews,reviewDecision,statusCheckRollup 2>/dev/null \
          | python3 - "$since" <<'PY'
import json, sys
since = sys.argv[1]
d = json.load(sys.stdin)
fresh = [r for r in d.get("reviews") or [] if (r.get("submittedAt") or "") > since]
if not fresh:
    sys.exit(1)
print(f"review decision: {d.get('reviewDecision') or 'none recorded'}")
print()
for r in fresh:
    who = (r.get("author") or {}).get("login", "?")
    print(f"--- {r.get('state')} by {who} at {r.get('submittedAt')}")
    print(r.get("body") or "(no body; see the inline comments)")
    print()
PY
)"
  if [ -n "$body" ]; then
    echo
    echo "$body"
    echo "--- inline comments"
    GH_PAGER=cat gh api "repos/{owner}/{repo}/pulls/$pr/comments" \
      --jq '.[] | "\(.path):\(.line // .original_line)  \(.user.login)\n\(.body)\n"' \
      2>/dev/null | head -200
    echo
    echo "Now: fix what is real, push back on what is not (with a reason, in a reply),"
    echo "reply to and resolve every thread, then push. Check with:"
    echo "  ./scripts/orca/review-status.sh $pr"
    exit 0
  fi

  sleep "$POLL_SECONDS"
  waited=$((waited + POLL_SECONDS))
done

echo
echo "no review arrived in $((DEADLINE_SECONDS / 60)) minutes."
echo "Silence is the failure mode here -- the review job is continue-on-error, so a"
echo "broken review looks like a green run with no comments. Check:"
echo "  gh run list --branch $(git rev-parse --abbrev-ref HEAD) --limit 5"
echo "and whether CLAUDE_CODE_OAUTH_TOKEN is set as a repository secret."
exit 4
