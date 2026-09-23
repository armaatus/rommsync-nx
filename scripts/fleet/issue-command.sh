#!/usr/bin/env bash
# Resolves a linked GitHub issue into the brief an agent starts from: the issue
# body, then the marching orders. This file is the only place the brief is
# written -- orca.yaml and the dispatcher both point at it, so neither can drift
# from it.
#
# THE BRIEF SENDS THE AGENT TO NO OTHER DOCUMENT. Its first sentence once read
# "following this repo's CLAUDE.md and the loop in docs/WORKFLOW.md", and that
# page was 10,036 words -- a longer retelling of the brief the agent had just
# been handed, carried in the prompt prefix of every request for the rest of the
# session (armaatus/autofleet#54).
#
# There was a second stage too, fetched with `--after-pr`: 1,272 words of review
# protocol. armaatus/autofleet#152 deleted it. The agent's job ends at an open
# pull request carrying `Closes #N`; everything after that is the dispatcher's.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$REPO_ROOT/scripts/fleet/lib.sh"

# `--after-pr` IS AN ERROR, NOT A NO-OP: an agent resumed in a worktree opened
# before the change still has the old stage 1 in its context telling it to type
# this, and printing stage 1 again would answer "what next" with what it has
# already done.
for arg in "$@"; do
  [ "$arg" = --after-pr ] || continue
  echo "issue-command: there is no --after-pr brief any more. Your job ends at" >&2
  echo "  an open pull request carrying 'Closes #N'. Nothing is waiting on you." >&2
  exit 2
done

ref="${1:-}"
# Neither "no runner" nor "no linked issue" may kill the script under `set -e`:
# the argument form is what the agent uses, and this fallback is for a person
# running it by hand.
if [ -z "$ref" ] && runner_available 2>/dev/null; then
  ref="$(runner_worktree_issue)" || issue_rc=$?
  case "${issue_rc:-0}" in
    0|2) ;;
    *)   echo "issue-command: the runner would not say whether this worktree has" >&2
         echo "  a linked issue -- which is not the same as it having none. Pass" >&2
         echo "  the issue number or URL as an argument." >&2 ;;
  esac
fi

# A bare number or any .../issues/<n>[...] URL. lib.sh holds the parse: the copy
# that lived here carried a BSD-sed defect that turned `/issues/42` into `4242`.
num="$(fleet_issue_number "$ref")" \
  || { echo "issue-command: could not resolve an issue from '${ref}'" >&2; exit 1; }

# GH_PAGER: on a TTY `gh` pages through less, which waits for a keypress nobody
# will press -- the hook never exits and the agent sits on a bare URL forever.
GH_PAGER=cat gh issue view "$num" --json number,title,body,labels,milestone,url \
  --template '{{printf "# %v: %v" .number .title}}
{{.url}}
Milestone: {{if .milestone}}{{.milestone.title}}{{else}}none{{end}}
Labels: {{range $i, $l := .labels}}{{if $i}}, {{end}}{{$l.name}}{{end}}

{{.body}}
'

# Quoted heredoc, and the substitutions applied after: the block is full of
# backticks, and in an unquoted heredoc the shell runs every one of them -- the
# project's own test command included, which is a whole test run in the middle
# of printing a prompt.
#
# The test command does NOT go through `sed`: it comes from `.autofleet/config`
# and may hold any character a `s###` delimiter could be. `awk`, and through
# ENVIRON rather than `-v`, because `-v` interprets escape sequences in what it
# assigns. `__ISSUE__` stays in the `sed`: it is digits.
brief_filter='
  BEGIN { cmd = "`" ENVIRON["tc"] "`" }
  { i = index($0, "__TEST_COMMAND__")
    if (i) $0 = substr($0, 1, i - 1) cmd substr($0, i + length("__TEST_COMMAND__"))
    print }
'
tc="${AUTOFLEET_TEST_COMMAND:-the full test suite}"
sed -e "s/__ISSUE__/$num/" <<'BRIEF' | tc="$tc" awk "$brief_filter"

---

Implement the issue above, end to end. Work autonomously: CLAUDE.md and this
brief carry your instructions, and a genuinely open question goes in the PR body
while you carry on with the rest of the scope.

**1. Build it.** The issue above IS the plan -- Goal, Scope, Design notes and
Acceptance are meant to be sufficient, and there is no planning phase before you
edit. Type this and nothing else, naming the issue above as the input:

    /implement

It drives `/mattpocock-skills:tdd` at the seams, runs __TEST_COMMAND__ once at
the end, and commits. For a bug the failing test is committed before the fix.
Read the suite output: a phase reporting `skip` judged nothing.

**2. Push, open the pull request, and STOP THERE.** The body carries `## Plan`
-- what the issue asked for, and where you departed from it and why -- any issue
you edited, and `Closes #__ISSUE__` on a line of its own. Departing is normal;
departing silently is not, and the review checks that section against the diff.

Do not queue the merge, wait for a review, or answer one. The dispatcher arms
auto-merge, runs one review, buys one fix session if it asks for changes, and
then GitHub merges or a person is told why not. A PR touching the rules --
`.github/`, `.claude/`, `.autofleet/` -- never merges itself, and that is not a
failure.

**Commit as you go.** If this run stops at its limit a second one starts in the
same worktree, from the branch and the pull request. Nothing else crosses.

**You are bounded by turns and dollars**: `gh pr diff --stat` before
`gh pr diff`, `sed -n '120,180p'` not a whole file, the `researcher` subagent
before a wide search.

If `~/.autofleet/STOP` exists, stop: say where you got to and do nothing
further. Nothing can go out while it exists.

BRIEF
