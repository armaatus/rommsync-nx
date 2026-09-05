#!/usr/bin/env bash
# Resolves a linked GitHub issue into the full brief an agent starts from.
#
# orca.yaml's `issueCommand` points the agent's opening prompt at this script, so
# an issue picked from the Tasks tab is read as its complete body rather than as
# the bare URL Orca prefills by default. Orca substitutes {{issue}} into that
# prompt, so the agent invokes this with the number; the no-argument form falls
# back to this worktree's linked issue, which is what a human running it by hand
# will want.
#
# It prints the spec AND the marching orders, so this file is the single place
# the opening brief is written. orca.yaml and agent-autostart.sh both only point
# at it -- neither restates the workflow, so neither can drift from it.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$REPO_ROOT/scripts/orca/lib.sh"

ref="${1:-}"
if [ -z "$ref" ] && orca_cli_resolve; then
  ref="$("$ORCA_CLI" worktree current --json 2>/dev/null \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["result"]["worktree"].get("linkedIssue") or "")' 2>/dev/null || true)"
fi

# Accept a bare number or any .../issues/<n>[...] URL.
num="$(printf '%s' "$ref" | sed -nE 's#.*/issues/([0-9]+).*#\1#p; s#^([0-9]+)$#\1#p' | head -1)"
[ -n "$num" ] || { echo "issue-command: could not resolve an issue from '${ref}'" >&2; exit 1; }

# GH_PAGER: Orca runs this hook on a TTY, and `gh` pages TTY output through
# less, which then waits for a keypress no one will press -- the hook never
# exits, Orca never gets the spec, and the agent tab sits on a bare URL forever.
GH_PAGER=cat gh issue view "$num" --json number,title,body,labels,milestone,url \
  --template '{{printf "# %v: %v" .number .title}}
{{.url}}
Milestone: {{if .milestone}}{{.milestone.title}}{{else}}none{{end}}
Labels: {{range $i, $l := .labels}}{{if $i}}, {{end}}{{$l.name}}{{end}}

{{.body}}
'

# Quoted heredoc, and the number substituted afterwards: this block is full of
# backticks, and in an unquoted heredoc the shell runs every one of them --
# `ctest --test-dir build` included, which is a three-minute test run in the
# middle of printing a prompt.
#
# CLAUDE.md carries all of this in full; repeating the checkable part here is
# what makes the opening prompt self-contained, so an agent cannot start work
# having read only a title.
sed "s/__ISSUE__/$num/" <<'BRIEF'

---

Implement the issue above, end to end, following CLAUDE.md and the loop in
docs/WORKFLOW.md.

Plan first. Stay in plan mode until the plan is right, then commit it as
`plans/__ISSUE__-<slug>.md` with four headings: Files that change, Order of work,
Risks, Proof. The bar is that an engineer who has never seen this conversation
could implement the change from it alone. If the implementation later departs
from the plan, update the plan in the same commit.

Then build it. Before you finish:

1. `ctest --test-dir build --output-on-failure` is green, and your change has a
   test that would have failed before it. Run it and read the output. For a bug
   fix, write the failing test first and commit it before the fix.
2. Run `/code-review` on your own branch and put the findings in the PR body.
   REVIEW.md is the policy it follows.
3. Edit any issue your findings invalidated -- yours or another -- and say in the
   PR body which and why.
4. Open a PR whose body carries `Closes #__ISSUE__`. A workflow reads that line to
   unblock dependent issues, so the wording matters.
5. Do not merge it. A human does that.

Work autonomously: do not stop to ask for confirmation on anything CLAUDE.md
already decides. If a question is genuinely open, write it in the PR body and
carry on with the rest of the scope.
BRIEF
