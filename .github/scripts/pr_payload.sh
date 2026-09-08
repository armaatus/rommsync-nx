#!/usr/bin/env bash
# One read of a pull request, in the shape .github/scripts/merge_gate.py judges.
#
#   .github/scripts/pr_payload.sh <owner> <name> <pr> >pr.json
#
# Shared by `.github/workflows/merge-gate.yml` and `scripts/orca/review-status.sh`
# for the reason merge_gate.py's own helpers are shared: the two used to carry
# the same GraphQL query twice, and the copies drifted (#114). They also carried
# the same bug -- `reviewThreads(first:100)` is the FIRST hundred, so a PR past
# that silently loses its newest threads and the gate reports "no review thread
# is unresolved" from a page it knew was partial.
#
# So this PAGES. `reviewThreads.pageInfo.hasNextPage` in the output is true only
# when the page cap below was reached before the list ran out, and merge_gate.py
# treats that as a refusal to answer rather than as a clean result.
#
# `reviews(last:50)` needs no paging: it takes the LATEST fifty, which is what
# the gate reads. Only the thread list is truncated from the wrong end.
#
# `comments` is the PR's own conversation, not its review threads, and it is
# here for one thing: the author's answer to a review that reported findings
# (`merge_gate.py`, `answered()`). LAST fifty for the same reason as the
# reviews -- an answer is written after the review it answers, so the newest end
# is the only end it can be at.
set -uo pipefail

[ $# -eq 3 ] || { echo "usage: pr_payload.sh <owner> <name> <pr>" >&2; exit 2; }
owner="$1"; name="$2"; pr="$3"

# 2000 threads. Far past anything a real PR has, and a bound rather than a
# `while true` against an API whose cursor could in principle never end.
MAX_PAGES="${PR_PAYLOAD_MAX_PAGES:-20}"

# Single-quoted on purpose: $owner, $name, $pr and $after are GraphQL variables
# bound by the -F/-f flags, and must reach the server unexpanded.
# shellcheck disable=SC2016
QUERY='
query($owner:String!,$name:String!,$pr:Int!,$after:String){
  repository(owner:$owner,name:$name){
    pullRequest(number:$pr){
      body
      author{login}
      reviews(last:50){ nodes{ state submittedAt commit{oid} author{login}
                               body comments(first:1){ totalCount } } }
      comments(last:50){ nodes{ author{login} createdAt body } }
      reviewThreads(first:100, after:$after){
        pageInfo{ hasNextPage endCursor }
        nodes{ id isResolved isOutdated path line
               comments(first:1){ nodes{ author{login} body } } }
      }
    }
  }
}'

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

after=""
page=0
pages=()
while [ "$page" -lt "$MAX_PAGES" ]; do
  page=$((page + 1))
  out="$work/page-$page.json"
  pages+=("$out")
  if [ -z "$after" ]; then
    GH_PAGER=cat gh api graphql -F owner="$owner" -F name="$name" -F pr="$pr" \
      -f query="$QUERY" >"$out" || { echo "could not read PR #$pr" >&2; exit 1; }
  else
    GH_PAGER=cat gh api graphql -F owner="$owner" -F name="$name" -F pr="$pr" \
      -f after="$after" -f query="$QUERY" >"$out" \
      || { echo "could not read page $page of PR #$pr's review threads" >&2; exit 1; }
  fi
  # The cursor, read back from the page rather than tracked here, so a shape
  # this script did not expect stops the loop instead of paging forever. Two
  # lines and not two fields: a cursor is opaque, and splitting it on whitespace
  # would be a guess about someone else's format.
  info="$(python3 -c '
import json, sys
doc = json.load(open(sys.argv[1]))
pull = ((doc.get("data") or {}).get("repository") or {}).get("pullRequest")
if not pull:
    raise SystemExit(1)
page = (pull.get("reviewThreads") or {}).get("pageInfo") or {}
print("1" if page.get("hasNextPage") else "0")
print(page.get("endCursor") or "")
' "$out")" || { echo "PR #$pr came back in a shape this cannot read" >&2; exit 1; }
  more="$(printf '%s\n' "$info" | sed -n 1p)"
  after="$(printf '%s\n' "$info" | sed -n 2p)"
  [ "$more" = 1 ] && [ -n "$after" ] || break
done

# Page one carries everything that is not a thread; the threads are every page's
# nodes end to end. `hasNextPage` survives only if the cap stopped us: that is
# the one case where the answer really is partial, and merge_gate.py refuses it.
python3 -c '
import json, sys

pages = [json.load(open(p)) for p in sys.argv[1:]]
doc = pages[0]
pull = doc["data"]["repository"]["pullRequest"]
nodes = []
for page in pages:
    threads = page["data"]["repository"]["pullRequest"]["reviewThreads"]
    nodes.extend(threads.get("nodes") or [])
last = pages[-1]["data"]["repository"]["pullRequest"]["reviewThreads"]
pull["reviewThreads"] = {
    "pageInfo": {"hasNextPage":
                 bool(((last.get("pageInfo") or {}).get("hasNextPage")))},
    "nodes": nodes,
}
json.dump(doc, sys.stdout)
sys.stdout.write("\n")
' "${pages[@]}"
