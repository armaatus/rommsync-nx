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

**2. Build it, test-first where the issue is a bug.** Reproduce the bug as a
failing test, watch it fail for the reason you expect, commit that test, and only
then fix the code -- `/mattpocock-skills:tdd` is that loop.
`ctest --test-dir build --output-on-failure` green, with a test that would have
failed before your change. Run it and read the output.

**3. Review it yourself, before anything leaves this worktree.** Two passes,
because they look for different things and this machine has the time:

    /code-review high                  # defects: correctness, efficiency, reuse
    /mattpocock-skills:code-review     # conformance: standards, and spec-vs-diff

REVIEW.md is the policy. Fix what is real, re-run the tests, then:

    ./scripts/orca/record-review.sh findings.md

Until that marker exists for the exact commit you are pushing, the guard hook
refuses `git push` and `gh pr create` here. A PR from the fleet arrives already
reviewed or it does not arrive.

**4. Push, open the PR, and queue the merge -- in that order, now.**

    gh pr merge <n> --auto --squash

Run it the moment the PR exists. Not at the end, not after the review: GitHub
refuses to queue auto-merge on a pull request that is ALREADY mergeable (`Pull
request is in clean status`), and you are forbidden from merging directly, so a
PR that goes green before anything queued it has nobody left to merge it. It sits
clean and untouched forever, which is what #90 did. Queued here it simply waits,
and fires the moment the last required check passes. Step 6 is only the check
that you did it.

The body must carry `## Plan`, BOTH sets of findings
and what you did about them, any issue you edited and why, and `Closes #__ISSUE__`.
The `merge-gate` check reads that body: it looks for the words `/code-review` and
`mattpocock-skills:code-review`, and without them the PR cannot merge. Then tell
the Orca board where the work is:

    orca worktree set --worktree active --workspace-status in-review \
      --comment "#__ISSUE__: PR #<n>, waiting on review"

**If your issue's scope is `.github/workflows/`, `.github/scripts/` or
`.claude/`, this PR will never merge itself, and that is not a failure.**
`merge-gate` refuses those paths on purpose: a PR that could rewrite the rules
judging PRs is not merged by the machinery those rules govern. Take it to a
reviewed, green PR with every thread resolved, set the board comment to
"#__ISSUE__: ready, needs a human merge -- touches <path>", and stop there.
Do not spend review rounds trying to turn that gate green.

**5. Wait for the independent review.** One blocking call, which costs nothing
while it waits:

    ./scripts/orca/await-review.sh

It returns when the review lands. **A clean verdict is not the same as no
findings.** A review can come back COMMENTED and still carry inline comments,
each of which is a THREAD, and `merge-gate` refuses to merge while any thread is
unresolved. #88 and #89 both sat blocked on exactly one unresolved thread with
every check green.

So after every review, whatever its verdict:

    ./scripts/orca/review-status.sh

It prints every thread that is still UNRESOLVED -- where it is, what it says,
and the thread ID `resolveReviewThread` wants -- along with anything else
keeping the PR from merging. Do NOT reach for
`gh api repos/{owner}/{repo}/pulls/<n>/comments` instead: that endpoint cannot
say whether a thread is resolved, so on round two it hands you every comment
ever left with the live ones buried among them.

Fix what is real; where you disagree, reply on the thread with your reasoning --
both are acceptable, silence is not. Then RESOLVE each thread (the
`resolveReviewThread` GraphQL mutation), push if you changed anything, and run
it again:

    ./scripts/orca/review-status.sh

Exit 0 means every thread is resolved and every check is green. Anything else,
go back to `await-review.sh`.

**At most THREE rounds of this.** If a third round still leaves something
unresolved, stop: comment on the PR saying exactly what is unresolved and why you
disagree, set the board comment to "#__ISSUE__: needs you -- 3 review rounds", and
stop. Another lap is not what a disagreement needs.

**6. Confirm the merge is queued.** You ran `gh pr merge <n> --auto --squash`
back at step 4; this is only the check that it took:

    gh pr view <n> --json autoMergeRequest --jq '.autoMergeRequest != null'

`true` and you are done -- GitHub merges it when the last required check passes.
`false` means the queue did not take: read what `gh pr merge <n> --auto --squash`
says now rather than merging by hand, which the guard hook refuses anyway.

That does NOT merge. It asks GitHub to merge once the required checks pass, and
`merge-gate` is one of them -- so the rules decide, not you. You may not merge
directly; the hook will not let you, and that is the one review control this
project has. Say the PR is queued and stop.

A PR that touches `.claude/` or `.github/workflows/` never auto-merges: those are
the paths that can disable the checks gating their own PR, and a person merges
them. `merge-gate` will say so.

At any point, if `~/.rommsync-fleet/STOP` exists, put the work down: say where you
got to and do nothing further. Nothing can go out while it exists.
BRIEF
