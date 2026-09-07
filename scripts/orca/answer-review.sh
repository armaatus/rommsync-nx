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
# Exits 0 when the answer is posted (whether or not the gate needed re-asking),
# 1 when it could not be posted, 2 when it could not tell, 3 when the fleet is
# stopped.
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

# The bar comes from the gate rather than from a number here. An answer the gate
# will not count is worse than no answer: it looks done and the PR stays red for
# a reason nothing on the PR states.
min="$(python3 -c '
import sys
sys.path.insert(0, ".github/scripts")
import merge_gate
print(merge_gate.MIN_ANSWER_BODY)' 2>/dev/null)" || min=""
[ -n "$min" ] || {
  echo "could not load .github/scripts/merge_gate.py, which decides what counts as an answer" >&2
  exit 2; }

trimmed="$(printf '%s' "$text" | tr -d '[:space:]')"
if [ "${#trimmed}" -lt "$min" ]; then
  echo "that is too short to be an answer ($min characters of substance, minimum)." >&2
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
  echo "this worktree is on ${local_head:0:8} and PR #$pr is on ${head:0:8}, so there is" >&2
  echo "work here that GitHub has not seen. Push it first -- the push re-runs the" >&2
  echo "reviewer, and the review to answer is the one of what you actually sent." >&2
  exit 2
fi

marker="$(python3 -c '
import sys
sys.path.insert(0, ".github/scripts")
import merge_gate
print(merge_gate.answer_marker(sys.argv[1]))' "$head" 2>/dev/null)" || marker=""
[ -n "$marker" ] || {
  echo "could not build the answer marker from .github/scripts/merge_gate.py" >&2
  exit 2; }

body="$(mktemp)"
trap 'rm -f "$body"' EXIT
{
  printf '%s\n\n' "$marker"
  printf '**Answering the review on `%s`.**\n\n' "${head:0:8}"
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
orca_reask_gate "$pr" "$head" "the review is answered"
