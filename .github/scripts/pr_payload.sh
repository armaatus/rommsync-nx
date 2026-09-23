#!/usr/bin/env bash
# One read of a pull request, in the shape .github/scripts/merge_gate.py judges.
#
#   .github/scripts/pr_payload.sh <owner> <name> <pr> >pr.json
#
# Shared by `.github/workflows/merge-gate.yml` and `scripts/fleet/await-review.sh`
# for the reason merge_gate.py's own helpers are shared: the two used to carry
# the same GraphQL query twice, and the copies drifted (#114).
#
# GraphQL rather than `gh pr view --json reviews`, for one field: `commit{oid}`.
# A review is of ONE commit, both readers are deciding whether a review judged
# THIS head, and the REST shape does not carry the commit a review was left on.
#
# IT NO LONGER PAGES, because there is no longer anything to page. This used to
# fetch every review thread and every PR comment so the gate could decide
# whether each thread was resolved and whether the author had answered the
# review in prose. `required_conversation_resolution` is branch protection now
# and the answer is a re-review, so the whole of that is gone with
# armaatus/autofleet#152 -- and with it the truncation bug that made a partial
# thread list read as a clean one. `reviews(last:50)` takes the LATEST fifty,
# which is the end a verdict on the current head can only be at.
set -uo pipefail

[ $# -eq 3 ] || { echo "usage: pr_payload.sh <owner> <name> <pr>" >&2; exit 2; }

# Single-quoted on purpose: $owner, $name and $pr are GraphQL variables bound by
# the -F flags, and must reach the server unexpanded.
# shellcheck disable=SC2016
QUERY='
query($owner:String!,$name:String!,$pr:Int!){
  repository(owner:$owner,name:$name){
    pullRequest(number:$pr){
      body
      reviews(last:50){ nodes{ state submittedAt commit{oid} author{login} body } }
    }
  }
}'

GH_PAGER=cat gh api graphql -F owner="$1" -F name="$2" -F pr="$3" -f query="$QUERY" \
  || { echo "could not read PR #$3" >&2; exit 1; }
