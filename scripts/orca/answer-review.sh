#!/usr/bin/env bash
# Say what was done about a review's findings -- and ask the gate again, which
# is the half nothing else does.
#
#   ./scripts/orca/answer-review.sh "reworded the comment; the retry already backs off"
#   ./scripts/orca/answer-review.sh --stdin  <notes.md
#
# WHY THIS EXISTS. `gh pr merge --auto` is armed the moment the PR is created,
# deliberately: that is what stops a finished PR sitting green with nobody left
# to merge it (#90). The independent review only runs afterwards, so between the
# review landing and the author's next push every gate is satisfied and the
# branch merges while the fixes are still being written. Four PRs went in that
# way -- #146, #154, #159, #168 -- and the window is not the life of the PR, it
# is one `ctest` run: 20 to 30 minutes, which is exactly what CLAUDE.md's loop
# does between reading the findings and pushing.
#
# `merge_gate.py` closes it by requiring an answer to any review that reports
# findings. This writes that answer. It is a comment on the PR, so a human
# reading the conversation sees the disposition in the same place as the
# findings.
#
# IT DOES NOT CLAIM THE FINDINGS WERE FIXED. "I did this" and "I am not doing
# this, because" are both answers, and the gate cannot tell them apart. What it
# asserts is that somebody read them and decided before the branch went in.
#
# Exits 0 when the answer is posted and the gate has been re-asked (or did not
# need to be), 1 when it could not be posted, 2 when it could not tell -- which
# includes the answer having gone up and the re-ask having failed, in which case
# it prints the `gh run rerun` to run by hand -- and 3 when the fleet is stopped.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

usage() {
  echo "usage: answer-review.sh \"<what you did about the findings>\"" >&2
  echo "       answer-review.sh --stdin  <notes.md" >&2
}

case "${1:-}" in
  ""|-h|--help) usage; exit 2 ;;
  --stdin) text="$(cat)" ;;
  *) text="$*" ;;
esac

# The stop is a stop. This writes to a pull request, which is exactly what
# nothing may do while the file exists.
orca_fleet_stopped && { echo "STOPPED: $ORCA_FLEET_STOP exists."; exit 3; }

# Everything this script needs to agree with the gate about -- what counts as an
# answer, and what an answer looks like -- comes FROM the gate, through one
# helper rather than through a number and a format copied here. An answer the
# gate will not count is worse than no answer: it reads as done and the PR stays
# red for a reason nothing on the PR states.
gate_py() {
  python3 -c '
import sys
sys.path.insert(0, ".github/scripts")
import merge_gate
exec(sys.argv[1])' "$@" 2>/dev/null
}

# Substance measured the gate's way, not by trimming here: it used to count
# non-whitespace characters, which is a different number, so this could refuse
# an answer the gate would have taken. Found in review of this PR.
short="$(printf '%s' "$text" | gate_py '
text = sys.stdin.read()
print("" if len(merge_gate.answer_substance(text)) >= merge_gate.MIN_ANSWER_BODY
      else merge_gate.MIN_ANSWER_BODY)')" || {
  echo "could not load .github/scripts/merge_gate.py, which decides what counts as an answer" >&2
  exit 2; }
if [ -n "$short" ]; then
  echo "that is too short to be an answer ($short characters of substance, minimum)." >&2
  echo "Say what you did about each finding, or why you are not doing it. A human" >&2
  echo "reading this PR has nothing else to check the disposition against." >&2
  exit 2
fi

orca_owner_repo || { echo "could not read this repository's name from gh" >&2; exit 2; }

pr="$(orca_pr_for_branch)" || {
  echo "no open PR for branch $(git rev-parse --abbrev-ref HEAD)" >&2; exit 2; }

head="$(GH_PAGER=cat gh pr view "$pr" --json headRefOid --jq .headRefOid 2>/dev/null)"
[ -n "$head" ] || { echo "could not read PR #$pr's head" >&2; exit 2; }

# The answer is per-head, so answering a commit this worktree has already moved
# past is answering the wrong review. Refused rather than warned: the push
# re-runs the reviewer, so what is on the way is a NEW review of the code that
# actually exists, and an answer written now would stand over its findings.
local_head="$(git rev-parse HEAD 2>/dev/null || echo "")"
if [ -n "$local_head" ] && [ "$local_head" != "$head" ]; then
  echo "this worktree is on ${local_head:0:8} and PR #$pr is on ${head:0:8}." >&2
  # WHICH WAY they diverge decides the advice, and getting it wrong sends an
  # agent to run a `git push` GitHub will reject. Found in review of this PR.
  if git merge-base --is-ancestor "$head" "$local_head" 2>/dev/null; then
    echo "There is work here that GitHub has not seen. Push it first -- the push" >&2
    echo "re-runs the reviewer, and the review to answer is the one of what you sent." >&2
  else
    echo "The PR is ahead of this worktree, or the two have diverged. Fetch and" >&2
    echo "rebase before answering; what you would be answering for is not what is there." >&2
  fi
  exit 2
fi

# ...and there has to BE a review to answer. `answered()` requires the answer to
# post AFTER the review it answers, so an answer written before one exists on
# this head is discarded the moment the reviewer submits -- the agent is told it
# is done, the gate holds again, and nothing says why. That is reachable by
# following the loop as written: push a fix, then answer. Found in review of
# this PR, which also reordered the brief.
payload="$(mktemp)"
trap 'rm -f "$payload"' EXIT
orca_pr_payload "$pr" "$payload" || {
  echo "could not read PR #$pr's reviews, so whether there is one to answer cannot be told" >&2
  exit 2; }
reviewed="$(gate_py '
import json
pull = json.load(open(sys.argv[2]))["data"]["repository"]["pullRequest"]
print("yes" if any(merge_gate.is_substantive(r)
                   for r in merge_gate.independent_reviews(pull, sys.argv[3]))
      else "no")' "$payload" "$head")" || reviewed=""
if [ "$reviewed" != yes ]; then
  echo "no independent review has been submitted against ${head:0:8} yet, so there is" >&2
  echo "nothing here to answer -- and an answer written now would be discarded by the" >&2
  echo "review that follows it. Wait for it:  ./scripts/orca/await-review.sh $pr" >&2
  exit 2
fi

marker="$(gate_py 'print(merge_gate.answer_marker(sys.argv[2]))' "$head")" || marker=""
[ -n "$marker" ] || {
  echo "could not build the answer marker from .github/scripts/merge_gate.py" >&2
  exit 2; }

# The marker and NOTHING ELSE in front of the answer. The gate measures what is
# left after stripping the marker, so a preamble written here -- "answering the
# review on abc1234", which the marker already says -- would be substance the
# author did not supply, and the length check would be measuring this script.
body="$(mktemp)"
trap 'rm -f "$body" "$payload"' EXIT
{
  printf '%s\n' "$marker"
  printf '%s\n' "$text"
} >"$body"

if ! GH_PAGER=cat gh pr comment "$pr" --body-file "$body" >/dev/null 2>&1; then
  echo "could not post the answer on PR #$pr" >&2
  exit 1
fi
echo "answered the review on ${head:0:8} of PR #$pr"

# An issue comment is not one of merge-gate.yml's triggers and cannot be: an
# `issue_comment` run attaches its check to the default branch, not to this PR's
# head. So the gate has to be asked again by hand, exactly as after resolving
# the last thread.
orca_reask_gate "$head" "the review is answered"
