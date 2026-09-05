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
docs/WORKFLOW.md. Work autonomously: do not stop to ask for confirmation on
anything CLAUDE.md already decides. If a question is genuinely open, write it in
the PR body and carry on with the rest of the scope.

**1. Plan.** Stay in plan mode until the plan is right: Files that change, Order
of work, Risks, Proof. The bar is that someone who never saw this conversation
could implement it from the plan alone. It goes in the PR body under `## Plan`,
not in a file.

**2. Build it, test-first where the issue is a bug.** For a bug fix, reproduce it
as a failing test, watch it fail for the reason you expect, commit that test, and
only then fix the code -- `/mattpocock-skills:tdd` is the loop for this.
`ctest --test-dir build --output-on-failure` green, with a test that would have
failed before your change. Run it and read the output.

**3. Review it yourself, before anything leaves this worktree.** Two passes,
because they look for different things and this machine has the time:

    /code-review high                  # defects: correctness, efficiency, reuse
    /mattpocock-skills:code-review     # conformance: standards, and spec-vs-diff

REVIEW.md is the policy: three passes, Important before Nit, at most five nits,
and a list of what not to report. Fix what is real. Re-run the tests. Then:

    ./scripts/orca/record-review.sh findings.md

Until that marker exists for the exact commit you are pushing, the guard hook
refuses `git push` and `gh pr create` from this worktree. That is deliberate: a
PR from the fleet arrives already reviewed or it does not arrive.

**4. Push and open the PR.** Body carries `## Plan`, both sets of findings and
what you did about them, any issue you edited and why, and `Closes #__ISSUE__` --
a workflow reads that line to unblock dependent issues, so the wording matters.

**5. Wait for the independent review.** One blocking call, which costs nothing
while it waits:

    ./scripts/orca/await-review.sh

It returns when the review lands. Fix what is real; where you disagree, reply on
the thread with the reason rather than silently ignoring it. Reply to and resolve
every thread, push, and re-request review. Then:

    ./scripts/orca/review-status.sh

Exit 0 means every thread is resolved and every check is green. Anything else,
go back to `await-review.sh`. Loop until it says ready.

**6. Stop.** Say the PR is waiting on a human and stop. Do not merge it; the hook
will not let you, and that is the one review control this project has.

At any point, if `~/.rommsync-fleet/STOP` exists, put the work down: say where you
got to and do nothing further. Nothing can go out while it exists.
BRIEF
