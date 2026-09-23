#!/usr/bin/env bash
# Block until a review verdict exists for this pull request's current head.
#
#   ./scripts/fleet/await-review.sh        # the PR for this branch
#   ./scripts/fleet/await-review.sh 42
#
# ONE QUESTION, asked of GitHub every AUTOFLEET_POLL seconds: is there a review
# of the commit the PR is on right now. Nothing else. The 962 lines this
# replaced also decided whether the build was red, whether the branch was DIRTY
# or BEHIND, which threads were unresolved, whether a validation had superseded
# a review and whether the author's answer had been read -- because the agent
# under review was the one waiting, and every one of those was a thing it had to
# be told to do next. It is not waiting any more: armaatus/autofleet#152 ends
# the agent's job at "PR open with `Closes #N`", and what happens after that is
# the dispatcher's and GitHub's.
#
# So this is for a PERSON, and for a dispatcher restarting into a PR that
# already has a reviewer in flight somewhere. It reads; it never writes.
#
# Exit codes:
#   0  a verdict for the current head is on the PR
#   2  could not tell: no PR, or gh would not say which repository this is
#   3  the fleet is stopped -- nothing is going to arrive
#   4  the head MOVED while waiting; the verdict that matters is not this one's
#   5  nothing arrived inside AUTOFLEET_REVIEW_TIMEOUT
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/fleet/lib.sh

pr="${1:-}"
if [ -z "$pr" ]; then
  pr="$(fleet_pr_for_branch)" || {
    echo "await-review.sh: no open PR for this branch, and none named." >&2
    exit 2; }
fi
fleet_owner_repo || {
  echo "await-review.sh: gh would not say which repository this is." >&2; exit 2; }

head_now() {
  GH_PAGER=cat gh pr view "$pr" --json headRefOid --jq .headRefOid 2>/dev/null
}
head="$(head_now)"
[ -n "$head" ] && [ "$head" != null ] || {
  echo "await-review.sh: could not read the head of PR #$pr." >&2; exit 2; }

payload="$(mktemp)"
trap 'rm -f "$payload"' EXIT

# The SAME question merge_gate.py asks, and asked of the same payload, because
# "is there a verdict" having two answers is how a worktree waits for something
# that already arrived. The gate is imported rather than reimplemented.
verdict_on() {
  fleet_pr_payload "$pr" "$payload" || return 1
  python3 - "$payload" "$1" <<'HAS_VERDICT'
import json, os, re, sys
sys.path.insert(0, os.path.join(os.getcwd(), ".github", "scripts"))
from merge_gate import approved
doc = json.load(open(sys.argv[1]))
pull = doc["data"]["repository"]["pullRequest"]
head = sys.argv[2]
if approved(pull, head):
    raise SystemExit(0)
# ...and a refusal is a verdict too. `approved()` answers the gate's question,
# which is narrower than this one: a review that asked for changes has decided
# something, and a caller waiting for "has anyone judged this commit" must not
# go on waiting through it.
marker = re.compile(r"<!--\s*autofleet-verdict:\s*\S+\s+([0-9a-f]{7,40})\s*-->", re.I)
for review in (pull.get("reviews") or {}).get("nodes") or []:
    if ((review.get("commit") or {}).get("oid") or "") == head and \
            review.get("state") == "CHANGES_REQUESTED":
        raise SystemExit(0)
    found = marker.search(review.get("body") or "")
    if found and head.startswith(found.group(1)):
        raise SystemExit(0)
raise SystemExit(1)
HAS_VERDICT
}

waited=0
while :; do
  if fleet_stopped; then
    echo "await-review.sh: ~/.autofleet/STOP exists; no review is coming." >&2
    exit 3
  fi
  if verdict_on "$head"; then
    echo "await-review.sh: PR #$pr has a verdict for ${head:0:8}."
    exit 0
  fi
  # A HEAD MOVE ENDS THE WAIT rather than resetting it. Whoever moved it knows
  # something this process does not, and a wait that silently re-targets is a
  # wait whose answer is about a commit the caller never named.
  moved="$(head_now)"
  if [ -n "$moved" ] && [ "$moved" != null ] && [ "$moved" != "$head" ]; then
    echo "await-review.sh: PR #$pr moved to ${moved:0:8} while waiting." >&2
    exit 4
  fi
  if [ "$waited" -ge "$AUTOFLEET_REVIEW_TIMEOUT" ]; then
    echo "await-review.sh: nothing judged ${head:0:8} in ${AUTOFLEET_REVIEW_TIMEOUT}s." >&2
    echo "  Run ./scripts/fleet/review.sh $pr, or read why not in its output." >&2
    exit 5
  fi
  sleep "$AUTOFLEET_POLL"
  waited=$((waited + AUTOFLEET_POLL))
done
