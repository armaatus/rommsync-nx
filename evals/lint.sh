#!/usr/bin/env bash
# Is the agent configuration well-formed and still enforcing what it claims?
#
# Everything under .claude/ steers three parallel worktrees, and every failure
# mode it has is silent: a skill whose frontmatter does not parse never loads, a
# hook whose path is wrong never runs, a guard whose pattern stopped matching
# stops blocking. None of that shows up in a diff review or in a red build --
# it shows up as agents quietly doing the wrong thing for a week.
#
# So this asserts it, deterministically and with no model involved. It runs in
# `.github/workflows/agent-config.yml` as the gate, and as `agent.config` in
# ctest so a worktree sees a break before CI does.
#
#   ./evals/lint.sh
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
ok()   { echo "  ok: $*"; }

# CLAUDE.md is read in full at the start of every session, so its size is a real
# cost paid on every task in every worktree. The cap is a smell test, not a
# formatting rule: past it, the file has stopped being what a new joiner needs on
# day one and started being documentation, which belongs in docs/.
CLAUDE_MD_MAX_LINES=200

echo "== CLAUDE.md"
if [ ! -f CLAUDE.md ]; then
  fail "CLAUDE.md is missing; it is the file every session reads first"
else
  lines="$(wc -l <CLAUDE.md | tr -d ' ')"
  if [ "$lines" -gt "$CLAUDE_MD_MAX_LINES" ]; then
    fail "CLAUDE.md is $lines lines (cap $CLAUDE_MD_MAX_LINES). Move detail into docs/ and link it."
  else
    ok "CLAUDE.md is $lines lines"
  fi
fi

# AGENTS.md is the vendor-neutral name other agent tools read. It is a symlink
# so there is exactly one file to keep true, and a copy would silently rot.
if [ -L AGENTS.md ]; then
  [ "$(readlink AGENTS.md)" = "CLAUDE.md" ] \
    || fail "AGENTS.md points at $(readlink AGENTS.md), not CLAUDE.md"
  ok "AGENTS.md -> CLAUDE.md"
elif [ -e AGENTS.md ]; then
  fail "AGENTS.md is a real file; it must be a symlink to CLAUDE.md so the two cannot drift"
fi

echo "== REVIEW.md"
if [ ! -f REVIEW.md ]; then
  fail "REVIEW.md is missing; /code-review and the PR review workflow both read it"
else
  for needle in "Correctness" "Important vs Nit" "Do not report"; do
    grep -q "$needle" REVIEW.md || fail "REVIEW.md has no '$needle' section"
  done
  ok "REVIEW.md names its passes and its thresholds"
fi

# A skill is a folder with a SKILL.md whose frontmatter says when it triggers.
# Both halves are load-bearing: no frontmatter and it never loads, no description
# and it loads for nothing.
echo "== skills"
shopt -s nullglob
for skill in .claude/skills/*/; do
  name="$(basename "$skill")"
  file="$skill/SKILL.md"
  if [ ! -f "$file" ]; then
    fail "$skill has no SKILL.md"
    continue
  fi
  [ "$(head -1 "$file")" = "---" ] \
    || { fail "$file does not open with a --- frontmatter block"; continue; }
  # The frontmatter is everything up to the second ---.
  fm="$(awk 'NR>1 && /^---$/{exit} NR>1' "$file")"
  grep -q '^name:' <<<"$fm" || fail "$file frontmatter has no name:"
  grep -q '^description:' <<<"$fm" || fail "$file frontmatter has no description:"
  declared="$(grep '^name:' <<<"$fm" | head -1 | sed 's/^name:[[:space:]]*//' | tr -d '"'"'"' ')"
  [ "$declared" = "$name" ] \
    || fail "$file declares name '$declared' but lives in $name/ -- they must match"
  ok "skill $name"
done

echo "== subagents"
for agent in .claude/agents/*.md; do
  name="$(basename "$agent" .md)"
  [ "$(head -1 "$agent")" = "---" ] \
    || { fail "$agent does not open with a --- frontmatter block"; continue; }
  fm="$(awk 'NR>1 && /^---$/{exit} NR>1' "$agent")"
  grep -q '^name:' <<<"$fm" || fail "$agent frontmatter has no name:"
  grep -q '^description:' <<<"$fm" || fail "$agent frontmatter has no description:"
  declared="$(grep '^name:' <<<"$fm" | head -1 | sed 's/^name:[[:space:]]*//' | tr -d '"'"'"' ')"
  [ "$declared" = "$name" ] \
    || fail "$agent declares name '$declared' but the file is $name.md -- they must match"
  ok "subagent $name"
done

echo "== settings.json"
if ! python3 -c 'import json,sys; json.load(open(".claude/settings.json"))' 2>/dev/null; then
  fail ".claude/settings.json is not valid JSON -- every hook in it is silently off"
else
  ok ".claude/settings.json parses"
  # A hook whose command does not exist is not an error anyone sees; it is a
  # guard that stopped guarding.
  while IFS= read -r cmd; do
    [ -n "$cmd" ] || continue
    resolved="${cmd/\$CLAUDE_PROJECT_DIR/$REPO_ROOT}"
    script="${resolved%% *}"
    [ -f "$script" ] || { fail "hook command does not exist: $cmd"; continue; }
    [ -x "$script" ] || { fail "hook command is not executable: $script"; continue; }
    bash -n "$script" || fail "hook does not parse: $script"
    ok "hook $(basename "$script")"
  done < <(python3 - <<'PY'
import json
s = json.load(open(".claude/settings.json"))
for group in s.get("hooks", {}).values():
    for matcher in group:
        for h in matcher.get("hooks", []):
            if h.get("type") == "command" and h.get("command"):
                print(h["command"])
PY
  )
fi

# The guards themselves. A hook that parses is not a hook that blocks: these
# feed it the shape it will see in production and assert the verdict, so a
# pattern that stops matching fails here rather than the day it lets something
# through.
echo "== guards actually guard"
assert_hook() {
  local script="$1" want="$2" payload="$3" what="$4"
  [ -x "$script" ] || { fail "$script is missing; cannot check '$what'"; return; }
  printf '%s' "$payload" | "$script" >/dev/null 2>&1
  local got=$?
  [ "$got" = "$want" ] \
    && ok "$what" \
    || fail "$what: expected exit $want from $(basename "$script"), got $got"
}
GB=.claude/hooks/guard-bash.sh
GE=.claude/hooks/guard-edit.sh
assert_hook "$GB" 2 '{"tool_input":{"command":"gh pr merge 42 --squash"}}' \
  "an agent cannot merge its own PR"
assert_hook "$GB" 0 '{"tool_input":{"command":"ctest --test-dir build --output-on-failure"}}' \
  "running the tests is not blocked"
assert_hook "$GB" 0 '{"tool_input":{"command":"gh pr create --title x --body y"}}' \
  "opening a PR is not blocked"
assert_hook "$GE" 2 '{"tool_input":{"file_path":"/w/rommsync-nx/.env"}}' \
  "secrets are not editable"
assert_hook "$GE" 2 '{"tool_input":{"file_path":"/w/rommsync-nx/server/contract/captures/login.json"}}' \
  "the pinned contract is not hand-edited"
assert_hook "$GE" 2 '{"tool_input":{"file_path":"/w/rommsync-nx/.github/workflows/unblock.yml"}}' \
  "the blocked/ready workflow is not agent-editable"
assert_hook "$GE" 0 '{"tool_input":{"file_path":"/w/rommsync-nx/core/src/sync.cpp"}}' \
  "ordinary source files are editable"

echo "== orca.yaml"
if [ ! -f orca.yaml ]; then
  fail "orca.yaml is missing; new worktrees would provision nothing"
else
  grep -q 'issue-command.sh' orca.yaml \
    || fail "orca.yaml's issueCommand no longer points at scripts/orca/issue-command.sh, so a new worktree's agent starts from a bare URL"
  grep -q 'setupAgentStartupPolicy: wait-for-setup' orca.yaml \
    || fail "orca.yaml no longer holds the agent tab for setup; agents will run ctest against a RomM that is not up"
  ok "orca.yaml still wires the agent to the spec"
fi

echo "== eval cases"
cases=(evals/cases/*.json)
[ "${#cases[@]}" -gt 0 ] || fail "evals/cases/ is empty; the eval job would pass by having nothing to run"
for case_file in "${cases[@]}"; do
  python3 -c "
import json,sys
c = json.load(open('$case_file'))
for key in ('name', 'prompt', 'expect'):
    if key not in c:
        sys.exit('missing key: ' + key)
if not isinstance(c['expect'], dict) or not (c['expect'].get('contains') or c['expect'].get('absent')):
    sys.exit(\"expect must carry 'contains' and/or 'absent'\")
" || fail "$case_file is not a well-formed eval case"
done
ok "${#cases[@]} eval case(s) parse"

echo
if [ "$fails" -gt 0 ]; then
  echo "$fails problem(s) in the agent configuration." >&2
  exit 1
fi
echo "agent configuration is well-formed."
