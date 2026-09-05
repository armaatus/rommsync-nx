#!/usr/bin/env bash
# Is this PR waiting only on GitHub's auto-merge?
#
# The exit code is the answer, so an agent can loop on it without reading prose:
#
#   0  ready -- every thread resolved, every check green, no standing objection
#   1  not ready -- the reasons are printed
#   2  could not tell (no PR, gh failed)
#   3  the fleet is stopped
#
# "Resolved" is read from GitHub's own state. The REST endpoint for PR comments
# cannot report it, so this goes through GraphQL -- `isResolved` on the thread,
# which is the thing a human looks at. `isOutdated` is not `isResolved`: a thread
# whose lines moved is still open.
#
# The verdict is read as the LATEST review per author, never as
# `reviewDecision`. That field is sticky: once a reviewer requests changes it
# stays CHANGES_REQUESTED until dismissed or until that reviewer approves -- and
# this repo's reviewer never approves, by design. Reading it would report a PR
# whose findings were all addressed as blocked forever. This mirrors
# .github/scripts/merge_gate.py exactly, on purpose: the local answer and the
# required check must not be able to disagree.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

orca_fleet_stopped && { echo "STOPPED: $ORCA_FLEET_STOP exists."; exit 3; }

pr="${1:-}"
[ -n "$pr" ] || pr="$(orca_pr_for_branch)" || {
  echo "no open PR for branch $(git rev-parse --abbrev-ref HEAD)" >&2; exit 2; }

head_sha="$(GH_PAGER=cat gh pr view "$pr" --json headRefOid --jq .headRefOid 2>/dev/null)" || exit 2
owner_repo="$(GH_PAGER=cat gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)" || exit 2
owner="${owner_repo%%/*}"; name="${owner_repo##*/}"

payload="$(mktemp)"; checks="$(mktemp)"
trap 'rm -f "$payload" "$checks"' EXIT

# Written to files and read back, never spliced into a Python source string.
# Review bodies are third-party text: one containing a quote sequence would
# otherwise break the parse, or worse.
GH_PAGER=cat gh api graphql -F owner="$owner" -F name="$name" -F pr="$pr" -f query='
query($owner:String!,$name:String!,$pr:Int!){
  repository(owner:$owner,name:$name){
    pullRequest(number:$pr){
      reviews(last:50){ nodes{ state submittedAt commit{oid} author{login} } }
      reviewThreads(first:100){ nodes{ isResolved path line } }
    }
  }
}' >"$payload" 2>/dev/null || { echo "could not read the PR's reviews" >&2; exit 2; }

GH_PAGER=cat gh pr view "$pr" --json statusCheckRollup >"$checks" 2>/dev/null || exit 2

python3 - "$pr" "$head_sha" "$payload" "$checks" <<'PY'
import json, sys

pr, head, payload_path, checks_path = sys.argv[1:5]
pull = json.load(open(payload_path))["data"]["repository"]["pullRequest"]
checks = json.load(open(checks_path)).get("statusCheckRollup") or []

problems = []

unresolved = [t for t in pull["reviewThreads"]["nodes"] if not t["isResolved"]]
if unresolved:
    problems.append(f"{len(unresolved)} unresolved review thread(s):")
    for t in unresolved:
        problems.append(f"    {t.get('path')}:{t.get('line')}")

on_head = [r for r in pull["reviews"]["nodes"]
           if (r.get("commit") or {}).get("oid") == head]
if not on_head:
    problems.append(
        f"no review has been submitted against the current head ({head[:8]}). "
        "Pushing a fix invalidates the previous one -- re-request review.")
else:
    latest = {}
    for r in sorted(on_head, key=lambda r: r.get("submittedAt") or ""):
        if r.get("state") in ("APPROVED", "CHANGES_REQUESTED", "COMMENTED"):
            latest[(r.get("author") or {}).get("login") or "?"] = r
    blocking = sorted(w for w, r in latest.items()
                      if r.get("state") == "CHANGES_REQUESTED")
    if blocking:
        problems.append(
            "the latest review from " + ", ".join(blocking) + " still requests "
            "changes. Address it and re-request review; a clean re-review "
            "supersedes it.")

# A check that has not finished is not a failure, but it is not ready either.
bad = [c for c in checks
       if (c.get("conclusion") or c.get("state")) in
       ("FAILURE", "TIMED_OUT", "CANCELLED", "ACTION_REQUIRED", "ERROR")]
pending = [c for c in checks
           if c.get("status") in ("IN_PROGRESS", "QUEUED", "PENDING")
           or not (c.get("conclusion") or c.get("state"))]
for c in bad:
    problems.append(f"check failed: {c.get('name') or c.get('context')}")
for c in pending:
    problems.append(f"check still running: {c.get('name') or c.get('context')}")

if problems:
    print(f"PR #{pr} is NOT ready:")
    for p in problems:
        print("  " + p)
    raise SystemExit(1)

print(f"PR #{pr} is ready: every thread resolved, every check green, no standing "
      "objection.")
print(f"Queue the merge and stop:  gh pr merge {pr} --auto --squash")
print("That does not merge -- it asks GitHub to, once merge-gate passes.")
PY
