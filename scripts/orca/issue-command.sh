#!/usr/bin/env bash
# Resolves a linked GitHub issue into the brief an agent starts from: the issue
# body, then what to do with it. orca.yaml's issueCommand points here, so the
# brief is written in one place and the prompt cannot drift from it.
#
#   ./scripts/orca/issue-command.sh 277
#   ./scripts/orca/issue-command.sh https://github.com/<owner>/<repo>/issues/277
#
# The brief sends the agent to no other document: everything it needs beyond
# CLAUDE.md is the issue itself, and every extra page it is told to read rides
# in the prompt prefix of every request for the rest of the session.
set -euo pipefail

ref="${1:-}"
# A bare number, `#N`, or any .../issues/<n>[...] URL.
case "$ref" in
  *"/issues/"*) num="${ref##*/issues/}"; num="${num%%[!0-9]*}" ;;
  *)            num="${ref#\#}" ;;
esac
case "$num" in
  ''|*[!0-9]*) echo "issue-command: could not resolve an issue from '${ref}'" >&2; exit 1 ;;
esac

# GH_PAGER: on a TTY `gh` pages through less, which waits for a keypress nobody
# will press -- the prompt never arrives and the agent sits on a bare URL.
GH_PAGER=cat gh issue view "$num" --json number,title,body,labels,milestone,url \
  --template '{{printf "# %v: %v" .number .title}}
{{.url}}
Milestone: {{if .milestone}}{{.milestone.title}}{{else}}none{{end}}
Labels: {{range $i, $l := .labels}}{{if $i}}, {{end}}{{$l.name}}{{end}}

{{.body}}
'

# Quoted heredoc: the block is full of backticks, and an unquoted one would run
# every one of them -- the test command included.
sed -e "s/__ISSUE__/$num/g" <<'BRIEF'

---

Implement the issue above, end to end, in this worktree. Work autonomously:
CLAUDE.md and this brief carry your instructions, and a genuinely open question
goes in your final commit message while you carry on with the rest of the scope.

**1. Build it.** The issue above IS the plan -- Goal, Scope, Design notes and
Acceptance are meant to be sufficient, and there is no planning phase before you
edit. Type this and nothing else, naming the issue above as the input:

    /implement

It drives `/mattpocock-skills:tdd` at the seams, runs
`ctest --test-dir build --output-on-failure` once at the end, and commits. For a
bug the failing test is committed before the fix. Read the suite output: a test
reporting **Skipped** judged nothing.

**2. Commit on this worktree's branch, and STOP THERE.** The final commit's
message ends with `## Plan` -- what the issue asked for, where you departed from
it and why, any issue you edited -- and `Closes #__ISSUE__` on a line of its own.
Departing is normal; departing silently is not.

Do not merge, and do not open a pull request. The maintainer's session merges
this branch into the milestone's integration branch (`fleet/mN`), which goes up
as one pull request that the maintainer reviews and merges.

BRIEF
