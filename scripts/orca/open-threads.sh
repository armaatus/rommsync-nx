#!/usr/bin/env bash
# Which review threads on this PR are still UNRESOLVED?
#
# Step 5 of the fleet's brief used to list feedback with
# `gh api repos/{owner}/{repo}/pulls/<n>/comments`. That REST endpoint cannot
# report whether a thread is resolved -- the same reason review-status.sh:11-13
# gives for going through GraphQL. On any round after the first it returns every
# comment ever left, already-fixed ones mixed in with the live ones and no way
# to tell them apart, which is the exact failure the brief exists to prevent:
# #88 and #89 each sat blocked on one unresolved thread lost in the noise.
#
# So this asks GitHub the question that actually gates the merge, and prints the
# thread ID with each one -- that ID is the argument `resolveReviewThread` wants,
# so the agent never has to go looking for it separately.
#
#   0  the query ran -- unresolved threads printed, nothing printed if none
#   2  could not tell (no PR, gh failed)
#   3  the fleet is stopped
#
# `isOutdated` is deliberately not filtered on: a thread whose lines moved
# because of a later push is still open, and merge-gate still refuses it.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

orca_fleet_stopped && { echo "STOPPED: $ORCA_FLEET_STOP exists."; exit 3; }

pr="${1:-}"
[ -n "$pr" ] || pr="$(orca_pr_for_branch)" || {
  echo "no open PR for branch $(git rev-parse --abbrev-ref HEAD)" >&2; exit 2; }

owner_repo="$(GH_PAGER=cat gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)" || exit 2
owner="${owner_repo%%/*}"; name="${owner_repo##*/}"

payload="$(mktemp)"
trap 'rm -f "$payload"' EXIT

# Written to a file and read back, never spliced into a Python source string:
# comment bodies are third-party text.
GH_PAGER=cat gh api graphql -F owner="$owner" -F name="$name" -F pr="$pr" -f query='
query($owner:String!,$name:String!,$pr:Int!){
  repository(owner:$owner,name:$name){
    pullRequest(number:$pr){
      reviewThreads(first:100){
        nodes{
          id isResolved isOutdated path line
          comments(first:1){ nodes{ author{login} body } }
        }
      }
    }
  }
}' >"$payload" 2>/dev/null || { echo "could not read the PR's review threads" >&2; exit 2; }

python3 - "$pr" "$payload" <<'PY'
import json, sys

pr, payload_path = sys.argv[1:3]
try:
    with open(payload_path) as fh:
        data = json.load(fh)
    threads = data["data"]["repository"]["pullRequest"]["reviewThreads"]["nodes"]
except Exception:
    print("could not read the PR's review threads", file=sys.stderr)
    raise SystemExit(2)

open_threads = [t for t in threads if not t.get("isResolved")]
if not open_threads:
    print(f"PR #{pr}: no unresolved review threads.")
    raise SystemExit(0)

print(f"PR #{pr}: {len(open_threads)} unresolved review thread(s).")
for t in open_threads:
    comments = (t.get("comments") or {}).get("nodes") or [{}]
    first = comments[0]
    who = ((first.get("author") or {}).get("login")) or "?"
    where = f"{t.get('path') or '?'}:{t.get('line') if t.get('line') is not None else '?'}"
    stale = "  (outdated -- still open)" if t.get("isOutdated") else ""
    print(f"\n--- {where}  by {who}{stale}\n    thread: {t.get('id')}")
    for line in (first.get("body") or "").splitlines():
        print(f"    {line}")
PY
