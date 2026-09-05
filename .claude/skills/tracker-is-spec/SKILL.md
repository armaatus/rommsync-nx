---
name: tracker-is-spec
description: >-
  Apply when work turns up something the issue did not know -- an endpoint that
  differs from the pinned contract, scope another issue already shipped, a
  constraint the code imposes, a dependency that is genuinely missing -- or when
  writing a PR body, closing an issue, or reading a spec. Triggers on issue,
  tracker, spec, acceptance, Blocked by, ready, blocked, milestone, PR body.
---

# The issue is the spec, and you maintain it

Every issue on this repo carries **Goal / Scope / Design notes / Acceptance**
(#5 and #40 are the standard). Three agents work in three worktrees and cannot
see each other. These issue bodies are the only channel between them. A stale one
makes the next agent re-derive a decision that was already made, or contradict it.

## The rule

**Edit the affected issue as you find it** -- including issues that are not
yours. Finding out that an issue is wrong and leaving it wrong is the failure
mode this exists to prevent.

Then say in your PR body which issues you edited and why. The reviewer needs to
see the spec change alongside the code change that caused it.

## How to edit one

```bash
GH_PAGER=cat gh issue view <n> --json number,title,body,labels,milestone
GH_PAGER=cat gh issue edit <n> --body-file <file>
```

Keep the Goal / Scope / Design notes / Acceptance shape. Add what you learned to
**Design notes**, and correct **Acceptance** if what you found changes what
"done" means.

## The one thing you must not touch

`blocked` and `ready` are **derived labels**. `.github/workflows/unblock.yml`
recomputes them from the `Blocked by #N` lines below the `<!-- blockers -->`
marker in each body. Hand-editing a label puts it back the moment the workflow
next runs, and in the meantime it can tell another agent to start work that is
not startable.

The `Blocked by #N` lines themselves *are* editable, and a genuinely missing
dependency should be added. But that changes what other agents may start, so:
do it deliberately, do it alone, and say so in the PR body. Never as a side
effect of rewording a body.

See [ISSUES.md](../../../ISSUES.md) and [docs/WORKFLOW.md](../../../docs/WORKFLOW.md).
