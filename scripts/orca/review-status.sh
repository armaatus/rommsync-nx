#!/usr/bin/env bash
# Is this PR waiting only on a human?
#
# The exit code is the answer, so an agent can loop on it without reading prose:
#
#   0  ready -- every review thread resolved, every check green, no changes requested
#   1  not ready -- the reasons are printed
#   2  could not tell (no PR, gh failed)
#   3  the fleet is stopped
#
# "Resolved" is read from GitHub's own state, not from whether a reply exists.
# The REST endpoint for PR comments cannot report it, so this goes through
# GraphQL -- `isResolved` on the thread, which is the thing a human looks at.
# `isOutdated` is not `isResolved`: a thread whose lines moved is still open.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

STATE_DIR="${ROMMSYNC_FLEET_DIR:-$HOME/.rommsync-fleet}"
[ -e "$STATE_DIR/STOP" ] && { echo "STOPPED: $STATE_DIR/STOP exists."; exit 3; }

pr="${1:-}"
if [ -z "$pr" ]; then
  branch="$(git rev-parse --abbrev-ref HEAD)"
  pr="$(GH_PAGER=cat gh pr list --head "$branch" --state open --json number \
          --jq '.[0].number' 2>/dev/null)"
  [ -n "$pr" ] && [ "$pr" != "null" ] \
    || { echo "no open PR for branch $branch" >&2; exit 2; }
fi

owner_repo="$(GH_PAGER=cat gh repo view --json nameWithOwner --jq .nameWithOwner)" || exit 2
owner="${owner_repo%%/*}"; name="${owner_repo##*/}"

threads="$(GH_PAGER=cat gh api graphql -f query='
query($owner:String!,$name:String!,$pr:Int!){
  repository(owner:$owner,name:$name){
    pullRequest(number:$pr){
      reviewDecision
      reviewThreads(first:100){
        nodes{ isResolved path line comments(first:1){ nodes{ author{login} body } } }
      }
    }
  }
}' -f owner="$owner" -f name="$name" -F pr="$pr" 2>/dev/null)" || {
  echo "could not read the PR's review threads" >&2; exit 2; }

checks="$(GH_PAGER=cat gh pr view "$pr" --json statusCheckRollup 2>/dev/null)" || exit 2

python3 - "$pr" <<PY
import json, sys
pr = sys.argv[1]
threads = json.loads('''$threads''')["data"]["repository"]["pullRequest"]
checks = json.loads('''$checks''').get("statusCheckRollup") or []

problems = []

open_threads = [t for t in threads["reviewThreads"]["nodes"] if not t["isResolved"]]
if open_threads:
    problems.append(f"{len(open_threads)} unresolved review thread(s):")
    for t in open_threads:
        first = (t["comments"]["nodes"] or [{}])[0]
        head = (first.get("body") or "").strip().splitlines()
        problems.append(
            f"    {t.get('path')}:{t.get('line')}  {head[0][:90] if head else '(no body)'}"
        )

decision = threads.get("reviewDecision")
if decision == "CHANGES_REQUESTED":
    problems.append("the review still says CHANGES_REQUESTED -- re-request it once the fixes are pushed")

# A check that has not finished is not a failure, but it is not ready either.
bad = [c for c in checks if (c.get("conclusion") or c.get("state")) in
       ("FAILURE", "TIMED_OUT", "CANCELLED", "ACTION_REQUIRED", "ERROR")]
pending = [c for c in checks if not (c.get("conclusion") or c.get("state"))
           or (c.get("status") in ("IN_PROGRESS", "QUEUED", "PENDING"))]
for c in bad:
    problems.append(f"check failed: {c.get('name') or c.get('context')}")
for c in pending:
    problems.append(f"check still running: {c.get('name') or c.get('context')}")

if problems:
    print(f"PR #{pr} is NOT ready:")
    for p in problems:
        print("  " + p)
    raise SystemExit(1)

print(f"PR #{pr} is ready: every review thread resolved, every check green.")
print("Stop here. A human merges.")
PY
