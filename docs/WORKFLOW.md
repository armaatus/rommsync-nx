# How work happens here

This project is built by agents working in parallel, one per Orca worktree, with
one human deciding what gets merged. This file is how that works end to end: what
each stage produces, what starts the next one, and where a person is required.

New here? Read [CLAUDE.md](../CLAUDE.md) first — it is the working agreement and
it is short. This file is the longer explanation behind it.

The shape is adapted from Anthropic's
[AI-native SDLC playbook](https://claude.com/blog/the-ai-native-sdlc-playbook).
The idea it turns on: **each stage ends by committing an artifact, and the next
stage begins by reading it.** The chain of commits is the audit trail — who asked
for what, what the agent produced, and who approved it. Nothing here is a
handoff meeting; everything is a file.

## The loop at a glance

| Stage | Artifact it commits | What starts the next stage | Who decides |
|---|---|---|---|
| 1. Intent | an `intent`-labelled issue | the issue being given Goal/Scope/Acceptance | anyone can file |
| 2. Spec | the issue body, `ready` | Orca worktree created from it | the maintainer |
| 3. Plan | `plans/<n>-<slug>.md` | the plan being accepted | the agent, out loud in the PR |
| 4. Build & test | the diff and its tests | `ctest` green | enforced by CI |
| 5. Review | `/code-review` findings in the PR body | a PR with `Closes #N` | **a human merges** |
| 6. Maintain | a new issue | back to stage 1 | the maintainer |

Two rules run through all of it:

- **The tracker is the spec.** Design intent lives in issue bodies, nowhere else.
  Three worktrees cannot see each other; those bodies are the only channel.
- **A human merges.** No agent merges its own work. This is enforced, not asked
  for — see [Guardrails](#guardrails).

---

## Stage 1 — Intent

An idea, a bug, a rough observation. It does not have to be a spec yet.

File it with the **Intent** issue template
([`.github/ISSUE_TEMPLATE/intent.yml`](../.github/ISSUE_TEMPLATE/intent.yml)):
problem, proposed outcome, affected systems, constraints, open questions. In
your own words. The point is that an idea does not have to wait until someone
has time to write a proper issue.

If you already know the scope and what "done" means, skip this and open the
issue in the stage 2 shape directly.

**Committed artifact:** the issue.

## Stage 2 — Spec

An intent becomes workable when its issue carries the four sections every issue
on this repo carries — #5 and #40 are the standard:

- **Goal** — what is true afterwards.
- **Scope** — what is in, and explicitly what is not.
- **Design notes** — the decisions already made, and why. This is where a
  constraint the code imposes, or an endpoint that differs from the pinned
  contract, gets written down.
- **Acceptance** — what a reviewer checks. Each item is something a test or a
  command can demonstrate.

Below a `<!-- blockers -->` marker, `Blocked by #N` lines name its dependencies.
[`.github/workflows/unblock.yml`](../.github/workflows/unblock.yml) derives the
`blocked` and `ready` labels from those lines on every merge. **Never hand-edit
those labels.** The lines themselves are editable, but changing one changes what
other agents may start — do it deliberately, alone, and say so in the PR body.

An issue is startable when it is `ready`. One exception the labels cannot
express: **a foundation issue lands alone.** When an issue defines an interface
later issues include (M0-2's `HttpClient` is the standing example) it merges
before anything depending on it starts, even if several things read as ready.

**Committed artifact:** the issue body. **Gate:** the maintainer decides what
gets a worktree.

## Stage 3 — Plan

Create the worktree in Orca from the linked issue. Everything below then happens
without you:

1. `orca.yaml`'s `setup` hook runs
   [`scripts/orca/setup.sh`](../scripts/orca/setup.sh): derives isolated ports,
   seeds ROM fixtures, builds, installs the server tooling, starts this
   worktree's own RomM, provisions the fixture, and opens a browser tab signed
   in as the fixture admin.
2. `setupAgentStartupPolicy: wait-for-setup` holds the agent's tab until that
   finishes, so the agent's first `ctest` means something.
3. `orca.yaml`'s `issueCommand` puts a prompt in the agent's composer pointing at
   [`scripts/orca/issue-command.sh`](../scripts/orca/issue-command.sh), which
   resolves the issue to its full body, labels and milestone, and then states the
   finishing conditions.
4. [`scripts/orca/agent-autostart.sh`](../scripts/orca/agent-autostart.sh)
   submits that prompt. Orca drafts it rather than sending it, so without this
   the worktree comes up perfectly provisioned with an agent that has read
   nothing.

The agent then plans before it edits. Claude Code starts in plan mode: it can
read the whole tree and change none of it. The plan is committed as
`plans/<issue-number>-<slug>.md` with four headings — **Files that change**,
**Order of work**, **Risks**, **Proof**. See [`plans/README.md`](../plans/README.md).

The bar: *an engineer who has never seen the conversation could implement the
change from the plan alone.*

**Committed artifact:** `plans/<n>-<slug>.md`. **Gate:** the plan is accepted
before any file is edited, and if the implementation departs from it, the plan is
updated in the same commit.

## Stage 4 — Build and test

Ordinary work, with three things that are not negotiable.

**There is no mock RomM.** Tests run against a real RomM 5.2.0 in Docker, per
worktree, on its own port. A passing test means the behaviour is genuinely real.
Failure modes a healthy RomM will not produce on demand — 401 mid-sync, a
truncated body, a dropped connection, a stall — are forced with the fault proxy
in front of it. See [TESTING.md](TESTING.md).

**Every change carries a test that would have failed before it.** Not "the suite
still passes". For a bug fix, write the failing test first, watch it fail for the
reason you expect, commit it, and only then fix the code. A test that existed
before the fix is proof the bug is gone.

**Verification is part of "done".** Run `ctest --test-dir build --output-on-failure`
and read the output before reporting anything complete. If `rig.smoke` reports
**Skipped**, RomM is not running and most of the suite is meaningless — start it
rather than working around it.

The [`verifier`](../.claude/agents/verifier.md) subagent is the packaged form of
the final check: a fresh context that builds, runs the suite, looks for the test
that would have failed, and reports `READY` or `NOT READY`. It fixes nothing,
which is why its verdict is worth having.

**Committed artifact:** the diff and its tests.

## Stage 5 — Review and merge

Three passes, on the way to one human decision.

1. **`/code-review` on your own branch.** Required by CLAUDE.md, not optional.
   The findings go in the PR body. This is what makes a human review tractable.
2. **CI.** [`ci.yml`](../.github/workflows/ci.yml) builds the host harness and
   runs the whole suite against a real RomM, builds all three Switch targets and
   checks the artifacts are what they claim to be, and enforces the mechanical
   half of the portability rule. [`agent-config.yml`](../.github/workflows/agent-config.yml)
   regression-tests the agent configuration itself whenever it changes.
3. **The independent PR review.**
   [`claude-review.yml`](../.github/workflows/claude-review.yml) reviews the PR
   against [REVIEW.md](../REVIEW.md) from a context that has not seen the
   conversation which produced the diff. An author reviewing their own work
   shares its blind spots; this is the pass that does not.

[REVIEW.md](../REVIEW.md) is the review policy: three passes (correctness;
portability and platform rules; compliance with the spec), what counts as
**Important** rather than a **Nit**, a cap on nits, and an explicit list of what
not to report at all — starting with anything CI already enforces.

The PR body carries `Closes #N`. A workflow reads that line to unblock dependent
issues, so the wording matters.

**Gate: a human merges.** Findings inform; they do not approve and they do not
block. No agent merges its own work — this is enforced by a hook, not asked for.

### Talking to Claude on a PR

Comment `@claude …` on a PR or a review comment and
[`claude-review.yml`](../.github/workflows/claude-review.yml) picks it up: it
reads the thread, makes the change, and pushes to the PR branch. It cannot reach
`main` and it cannot merge.

Both Claude workflows need a `CLAUDE_CODE_OAUTH_TOKEN` secret — mint one with
`claude setup-token` and add it under **Settings → Secrets and variables →
Actions**. Without it they no-op with a notice instead of failing, so a fork or a
plain clone still works.

## Stage 6 — Maintain

What comes back from a merged change re-enters at stage 1.

- A review finding that shows up **twice** stops being a review finding: the
  correction goes into [CLAUDE.md](../CLAUDE.md), or into a skill, as part of
  that review. That is how the next session already knows it.
- Anything that reached `main` and had to be reverted earns an eval case in
  [`evals/cases/`](../evals/cases), written by whoever handled it, so it stays
  fixed.
- Anything the work invalidated in the tracker gets edited as it is found —
  including issues that are not yours. The PR body says which and why.

---

## Guardrails

Three layers, in increasing order of how hard they are to ignore.

**CLAUDE.md** — read at the start of every session. Conventions, commands, the
five hard rules. Kept short on purpose: it is a cost paid on every task in every
worktree, and `evals/lint.sh` fails if it grows past what a session can hold.

**Skills** ([`.claude/skills/`](../.claude/skills)) — institutional knowledge
that must be applied consistently, loaded when it becomes relevant rather than
read every time. [`save-safety`](../.claude/skills/save-safety/SKILL.md) fires on
anything that writes a save; [`core-portability`](../.claude/skills/core-portability/SKILL.md)
on anything that reaches for a platform facility inside `core/`;
[`tracker-is-spec`](../.claude/skills/tracker-is-spec/SKILL.md) on anything that
finds an issue to be wrong. A skill is a real control but an *advisory* one —
nothing forces a session to comply.

**Hooks** ([`.claude/hooks/`](../.claude/hooks)) — the deterministic layer behind
the skills, registered in [`.claude/settings.json`](../.claude/settings.json).
They run on every matching action and they block, with an explanation:

| Hook | Blocks |
|---|---|
| `guard-bash.sh` | merging a PR; force-pushing `main`; rewriting the pinned contract from the shell |
| `guard-edit.sh` | editing secrets or `.env`; hand-editing `server/contract/captures/`; editing `unblock.yml` |
| `shellcheck-edited.sh` | *(reports, never blocks)* a shell script that no longer parses |

The skill makes a violation rare; the hook makes it close to impossible.

Agents run in **auto** permission mode (`permissions.defaultMode` in
`.claude/settings.json`) so a worktree does not sit waiting for someone to approve
a `cmake` invocation. That is only safe because the guardrails above are what
actually decide what an agent may do — not a prompt for each command.

### Guardrails on the GitHub side

The two Claude workflows are wired defensively, and the reasons are worth knowing
before you change them.

- **The `@claude` job is gated on author association** (`OWNER`, `MEMBER`,
  `COLLABORATOR`). It is the job that can write, and anyone can leave a comment.
  Only people who already have write access get to drive an agent that has it too.
- **Everything a PR contains is untrusted data.** Titles, descriptions, commit
  messages and diffs are the *subject* of the work, never a source of
  instructions. Both jobs carry that framing — the review job in its prompt, the
  mention job through `--append-system-prompt`, so it layers on top of the
  triggering comment rather than being replaced by it.
- **`pull_request`, never `pull_request_target`.** The latter would run a fork's
  code with write credentials.
- **The token-bearing eval job does not run on pull requests.** What it evaluates
  is the instructions an agent loads — `CLAUDE.md`, the skills, the hooks — and it
  evaluates them by handing them to an agent that has the token in its
  environment. On a PR trigger those files are whatever the branch says they are,
  and a hook is plain command execution. So evals run on `push` to `main`, after
  review; a PR gets the token-less `lint` job.
- **Actions are pinned to a commit SHA**, with the tag in a trailing comment. A
  mutable tag is a supply-chain hole: whoever can move `v1` can change what runs.
- **Both Claude jobs are `continue-on-error`.** They inform; they never block a
  merge. The consequence is that *silence* is the failure mode — a review that
  stops happening shows up as missing comments inside a green run, not as a red
  check. Watch for the absence.
- **The eval suite warns rather than fails.** A gate that cries wolf gets ignored,
  and the useful output is the responses in the job summary.

## Working in parallel

At most **three worktrees** at once. The ceiling is not machine capacity; it is
how many streams one person can review properly. Add a fourth only when review is
comfortably keeping up with three.

Each worktree is fully isolated: its own ports, its own compose project, its own
RomM database, its own `build/`. Only the immutable, expensive things are shared
(`.cache/roms`, `.cache/ccache`). No agent can corrupt another's fixtures.

Tasks that touch the same files run in one worktree, one after another. A
committed plan is the earliest point at which a collision between two branches is
visible, which is one of the reasons the plan is a file.

Removing a worktree from the **Orca UI** runs the teardown hook. Removing it with
`orca worktree rm` does **not** unless you pass `--run-hooks`, and the stack it
leaves behind restarts `unless-stopped` and holds two ports forever. Either pass
the flag or sweep afterwards with `./scripts/orca/reap.sh --yes`.

## What to check when the loop stalls

| Symptom | Cause | Fix |
|---|---|---|
| Worktree provisioned, agent idle, nothing in the composer | Orca drafts the issue prompt instead of sending it | `./scripts/orca/agent-autostart.sh` — the `--watch` form is started by `setup.sh` |
| The agent started from a bare issue URL and invented the scope | Orca will not run an `orca.yaml` `issueCommand` it has not been trusted with | Trust `orca.yaml` in Orca's repository-hooks settings. `agent-autostart.sh` completes the draft anyway |
| Every hook says "this worktree has no linked issue" | the `orca` CLI on `PATH` cannot find `Orca.app` | nothing — the hooks probe the wrapper and fall back to the app's own binary. If it persists, `orca skills get orca-cli` and check the install |
| `ctest` reports `rig.smoke` **Skipped** | RomM is not running for this worktree | `./scripts/orca/compose.sh up -d` |
| The whole suite fails on connection errors | the fixture never provisioned | re-run `./scripts/orca/setup.sh`; read `.orca/agent-autostart.log` and the `romm` tab |
| An agent is waiting for permission on something safe | auto mode is not on for that session | check `permissions.defaultMode` in `.claude/settings.json`, or start it with `claude --permission-mode auto` |
