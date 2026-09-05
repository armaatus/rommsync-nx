# evals/

Regression tests for the configuration that steers the agents.

`CLAUDE.md`, `REVIEW.md`, `.claude/skills`, `.claude/agents` and `.claude/hooks`
decide what three parallel worktrees do all day, and every way they break is
silent. A skill whose frontmatter does not parse never loads. A hook whose path
is wrong never runs. A guard whose pattern stopped matching stops blocking. None
of that appears in a diff review or turns a build red — it appears as agents
quietly doing the wrong thing for a week.

So the configuration gets the regression testing the code gets.

## Two layers

**`./evals/lint.sh`** — deterministic, free, no model. Skills and subagents have
well-formed frontmatter whose declared name matches its path; `settings.json`
parses and every hook it registers exists, is executable and parses; the guards
still return exit 2 for the things they exist to block and exit 0 for ordinary
work; `orca.yaml` still points a new worktree's agent at the spec; `CLAUDE.md` is
still short enough to be read at the start of every session. This is the gate,
in CI and in `ctest -R agent.config`.

**`./evals/run.sh`** — one headless session per case in `cases/`, scoring what
the agent actually answers. This is where you find out that the `save-safety`
skill stopped firing, or that the finishing conditions are no longer known.
Needs `CLAUDE_CODE_OAUTH_TOKEN` — mint one with `claude setup-token` and add it
under **Settings → Secrets and variables → Actions**. Without the secret, CI
reports the job skipped rather than failing.

```bash
./evals/lint.sh                 # always runnable
./evals/run.sh                  # every case
./evals/run.sh save-safety      # one
```

## Writing a case

A case is one JSON file in `cases/`:

```json
{
  "name": "save-safety",
  "why": "Hard rule 2 ... if the skill stops firing, this is where it shows.",
  "prompt": "…a question whose right answer is unambiguous…",
  "expect": {
    "contains": ["backup", "atomic|rename|temporary|tmp"],
    "absent":   ["truncat"]
  }
}
```

`contains` and `absent` are extended regexes matched case-insensitively against
the session's answer; `a|b` means either will do.

Three rules that keep the suite honest:

1. **The prompt asks the agent to describe or decide, never to change
   anything.** `run.sh` allows only read-only tools, so a case that needs an
   edit has already failed — and nothing here can mutate the checkout it runs in.
2. **Keep the checks coarse.** A keyword the right answer cannot avoid, and a
   keyword the wrong answer cannot avoid. A case that needs cleverness to score
   will start lying as models change. Prefer adding a case to sharpening one.
3. **`why` is not decoration.** It says which rule the case defends, so whoever
   sees it go red knows whether they have found a real regression or a case that
   has stopped discriminating. Delete cases that no longer discriminate.

## Where cases come from

Two places, and both matter more than anything invented up front:

- **A rule that got broken.** If a review finding, a PR discussion or a bug
  traces back to the agent not knowing something `CLAUDE.md` or a skill says,
  the fix goes in that file *and* a case goes in here, so it stays fixed.
- **An incident.** Anything that reached `main` and had to be reverted earns a
  case, written by whoever handled it.
