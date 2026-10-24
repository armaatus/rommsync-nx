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


# A skill is a folder with a SKILL.md whose frontmatter says when it triggers.
# Both halves are load-bearing: no frontmatter and it never loads, no description
# and it loads for nothing.
# A skill and a subagent are the same shape: a markdown file whose frontmatter
# says what it is and when it applies. Both halves are load-bearing -- no
# frontmatter and it never loads, no description and it loads for nothing -- and
# checking them twice in two places is how the two checks drift apart.
check_frontmatter() {
  local file="$1" expect_name="$2" what="$3" fm declared
  [ -f "$file" ] || { fail "$what: $file is missing"; return; }
  [ "$(head -1 "$file")" = "---" ] \
    || { fail "$file does not open with a --- frontmatter block"; return; }
  fm="$(awk 'NR>1 && /^---$/{exit} NR>1' "$file")"
  grep -q '^name:' <<<"$fm" || fail "$file frontmatter has no name:"
  grep -q '^description:' <<<"$fm" || fail "$file frontmatter has no description:"
  declared="$(grep '^name:' <<<"$fm" | head -1 | sed 's/^name:[[:space:]]*//' \
              | tr -d '"'"'"' ')"
  [ "$declared" = "$expect_name" ] \
    || fail "$file declares name '$declared' but is filed as '$expect_name' -- they must match"
  ok "$what $expect_name"
}

echo "== skills"
shopt -s nullglob
for skill in .claude/skills/*/; do
  check_frontmatter "$skill/SKILL.md" "$(basename "$skill")" skill
done

echo "== subagents"
for agent in .claude/agents/*.md; do
  check_frontmatter "$agent" "$(basename "$agent" .md)" subagent
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
    # The whole command is treated as one path, not split at the first space:
    # every hook here IS a single script, and a checkout under "~/my worktrees/"
    # would otherwise report all of them missing on a perfectly correct tree.
    script="${cmd/\$CLAUDE_PROJECT_DIR/$REPO_ROOT}"
    [ -f "$script" ] || { fail "hook command does not exist: $cmd"; continue; }
    [ -x "$script" ] || { fail "hook command is not executable: $script"; continue; }
    case "$script" in
      *.py) python3 -m py_compile "$script" || fail "hook does not parse: $script" ;;
      *)    bash -n "$script" || fail "hook does not parse: $script" ;;
    esac
    ok "hook $(basename "$script")"
  done < <(python3 -c '
import json
s = json.load(open(".claude/settings.json"))
for group in s.get("hooks", {}).values():
    for matcher in group:
        for h in matcher.get("hooks", []):
            if h.get("type") == "command" and h.get("command"):
                print(h["command"])
')
fi

# The guards themselves. A hook that parses is not a hook that blocks: these
# feed it the shape it will see in production and assert the verdict, so a
# pattern that stops matching fails here rather than the day it lets something
# through.
echo "== guards actually guard"
# The exhaustive table lives in the hook itself (`guard.py --selftest`): every
# rule in it has a row, and one without is not shipped.
# What this project asks it to protect on top -- the pinned RomM contract, the
# fixture credentials -- is .claude/hooks/guard.json, beside it.
if [ -x .claude/hooks/guard.py ]; then
  python3 .claude/hooks/guard.py --selftest 2>&1 | sed 's/^/  /'
  [ "${PIPESTATUS[0]}" = 0 ] || fail "the guard selftest does not hold"
else
  fail ".claude/hooks/guard.py is missing or not executable"
fi

# And the wiring, end to end. The selftest calls the module's functions, which
# proves the rules; this proves the process -- stdin in, exit code out -- which
# is what settings.json actually depends on.
assert_hook() {
  local script="$1" want="$2" payload="$3" what="$4"
  [ -x "$script" ] || { fail "$script is missing; cannot check '$what'"; return; }
  printf '%s' "$payload" | "$script" >/dev/null 2>&1
  local got=$?
  [ "$got" = "$want" ] \
    && ok "$what" \
    || fail "$what: expected exit $want from $(basename "$script"), got $got"
}
G=.claude/hooks/guard.py
assert_hook "$G" 2 '{"tool_name":"Bash","tool_input":{"command":"gh pr merge 42"}}' \
  "the guard blocks over stdin, not just in-process"
assert_hook "$G" 0 '{"tool_name":"Bash","tool_input":{"command":"ctest --test-dir build"}}' \
  "...and allows ordinary work the same way"
assert_hook "$G" 2 '{"tool_name":"Bash","cwd":"/w","tool_input":{"command":"cp x server/contract/captures/5.4.0/y.json"}}' \
  "a shell write into the pinned contract blocks over stdin, against this repo's guard.json"
assert_hook "$G" 2 'not json at all' \
  "an unreadable payload blocks rather than failing open"

echo "== the plugins the flow depends on"
# mattpocock-skills is not decoration: the brief in scripts/orca/issue-command.sh
# tells every agent to run `/implement`, which drives `/mattpocock-skills:tdd`
# and `/mattpocock-skills:code-review` (standards and spec-vs-diff, which the
# built-in review does not cover). It is
# enabled in the COMMITTED settings.json, which is what makes it present in every
# worktree; drop the entry and the brief silently asks for a skill nobody has.
if python3 -c '
import json, sys
s = json.load(open(".claude/settings.json"))
sys.exit(0 if s.get("enabledPlugins", {}).get("mattpocock-skills@claude-plugins-official") else 1)
'; then
  ok "mattpocock-skills is enabled for every worktree"
else
  fail "mattpocock-skills is not enabled in .claude/settings.json, but the agent brief calls its skills"
fi

echo "== every test phase actually runs"
# A phase defined in one of the phase-dispatching test scripts and missing from
# its foreach() list in tests/CMakeLists.txt never runs -- not in ctest, not in
# CI -- and is indistinguishable from a phase that passes. It happened: the
# assertions for a whole behaviour shipped green by never being executed. The
# script and the list are two files, so only something reading both can say they
# agree.
#
# Every such script, not one named here: this check used to cover
# test_orca_browser.sh alone, and the file that then grew ten phases in one
# change was test_orca_fleet.sh. The pairing comes out of the CMake itself --
# each `foreach(phase ...)` names a `*_TEST` variable, and each of those is
# `set()` to a script -- so a new phase-dispatching suite is covered the day it
# is registered, with nothing here to remember to update.
if python3 - <<'PHASES'
import re, sys

cml = open("tests/CMakeLists.txt").read()
scripts = dict(re.findall(r'set\((\w+_TEST)\s+"[^"]*/([\w.]+)"\)', cml))

def phases_in(path):
    # Heredoc bodies are dropped first. The phases write stub `orca` and `gh`
    # scripts, and those carry their own two-space `case` arms -- `worktree)`,
    # `terminal)` -- which are shell being generated, not phases of this file.
    kept, delim = [], None
    for line in open(path).read().splitlines():
        if delim is not None:
            if line.strip() == delim:
                delim = None
            continue
        here = re.search(r"<<-?\s*[\'\"]?([A-Za-z_][A-Za-z0-9_]*)[\'\"]?\s*$", line)
        if here:
            delim = here.group(1)
        kept.append(line)
    body = "\n".join(kept).split('case "${1:-}" in', 1)[1]
    # What is left: the phase labels are the only ones at exactly two spaces, and
    # `*)` is the usage fallback.
    return set(re.findall(r"^  ([a-z0-9_]+)\)$", body, re.M))

bad = covered = 0
for names, body in re.findall(r"foreach\(phase\b(.*?)\)(.*?)endforeach\(\)", cml, re.S):
    used = set(re.findall(r"\$\{(\w+_TEST)\}", body))
    if len(used) != 1:
        print(f"a foreach(phase ...) block names {len(used)} test scripts; this check cannot pair it")
        bad = 1
        continue
    script = scripts.get(used.pop())
    if script is None:
        print("a foreach(phase ...) block runs a test variable that is never set()")
        bad = 1
        continue
    listed, defined = set(names.split()), phases_in(f"tests/{script}")
    for name in sorted(defined - listed):
        print(f"{name}: defined in {script}, never registered in tests/CMakeLists.txt")
        bad = 1
    for name in sorted(listed - defined):
        print(f"{name}: registered in tests/CMakeLists.txt, but {script} has no such phase")
        bad = 1
    covered += 1
if covered == 0:
    # No phase-dispatching suite is registered this way any more (the v1 fleet
    # suites were). Nothing to pair is not a broken pairing: say so and keep the
    # check for the day one comes back.
    if "foreach(phase" in cml:
        print("no foreach(phase ...) block was paired with a script; this check now asserts nothing")
        bad = 1
    else:
        print("no foreach(phase ...) block in tests/CMakeLists.txt; nothing to pair")
sys.exit(bad)
PHASES
then
  ok "every phase-dispatching suite agrees with tests/CMakeLists.txt"
else
  fail "a phase test script and tests/CMakeLists.txt disagree about which phases exist; the ones above never run"
fi

echo "== the workflows parse as GitHub reads them"
# An invalid workflow file does not fail loudly: GitHub creates a run with no
# jobs, named after the file, and the check it was meant to report simply never
# appears. For a required check that reads as "pending forever"
# and nothing can merge. It happened once already --
# `pull_request_review_thread` is a webhook event, not a workflow trigger, and
# putting it in `on:` invalidated the whole file.
#
# Optional, because actionlint is not everywhere. CI has it, and says so.
if command -v actionlint >/dev/null 2>&1; then
  actionlint .github/workflows/*.yml || fail "actionlint rejects a workflow"
  ok "actionlint accepts every workflow"
else
  echo "  --: actionlint is not installed (brew install actionlint); CI still checks this"
fi

# A cancelled run is not a green run. `cancel-in-progress` is right on a branch,
# where only the newest push matters, and wrong on main, where every commit is
# one somebody has to be able to trust: two merges close together cancelled the
# earlier commit's build outright, and nothing re-ran it. #105 and #102 merged
# seconds apart and #105's main build died mid-flight.
if [ -f .github/workflows/ci.yml ]; then
  if grep -qE "^[[:space:]]*cancel-in-progress:[[:space:]]*true[[:space:]]*$" .github/workflows/ci.yml; then
    fail "ci.yml cancels in progress unconditionally, so a merge can cancel main's own build and that commit is never verified"
  fi
  ok "main's builds are never cancelled by the next merge"
fi

# A required check is matched by the name a job REPORTS, and a matrix job does
# not report its `name:` when that name holds no matrix expression: GitHub
# appends the matrix values, so `name: host-tests` over `matrix: romm: [5.4.0]`
# reports `host-tests (5.4.0)`. Branch protection requires `host-tests`, which
# then never reports, and every pull request waits on it forever (#235 shipped
# exactly that). So: no workflow job pairs a literal name with a matrix, and the
# job behind the required `host-tests` check is not a matrix at all and reports
# that name and no other -- a second RomM runs as a job of its own beside it.
#
# One line per job: id, name (comment and quotes stripped, empty if none), and
# 1 when it has a block-style matrix. Block-style job keys at two spaces is all
# these workflows use; a flow-style `strategy: {matrix: ...}` is not seen.
workflow_jobs() {
  awk '
    function flush() { if (id != "") printf "%s\t%s\t%d\n", id, name, matrix; id = "" }
    /^jobs:/ { in_jobs = 1; next }
    in_jobs && /^[^ #]/ { flush(); in_jobs = 0 }
    !in_jobs { next }
    /^  [^ #][^:]*:[[:space:]]*(#.*)?$/ {
      flush(); id = $1; sub(/:.*$/, "", id); name = ""; matrix = 0; next
    }
    /^    name:/ {
      name = $0
      sub(/^    name:[[:space:]]*/, "", name)
      if (match(name, /^"[^"]*"/) || match(name, /^\047[^\047]*\047/))
        name = substr(name, 2, RLENGTH - 2)
      else {
        sub(/[[:space:]]+#.*$/, "", name)
        sub(/[[:space:]]+$/, "", name)
      }
      next
    }
    /^      matrix:/ { matrix = 1 }
    END { flush() }' "$1"
}
for wf in .github/workflows/*.yml; do
  [ -f "$wf" ] || continue
  bad="$(workflow_jobs "$wf" |
    awk -F '\t' '$3 == 1 && $2 != "" && $2 !~ /\$\{\{.*matrix\./ { print $1 " (name: " $2 ")" }')"
  [ -z "$bad" ] ||
    fail "$wf: a matrix job with a fixed name reports as '<name> (<values>)', not its name, so a required check on it never reports: $bad"
done
if [ -f .github/workflows/ci.yml ]; then
  host_tests="$(workflow_jobs .github/workflows/ci.yml | awk -F '\t' '$1 == "host-tests"')"
  if [ -z "$host_tests" ]; then
    fail "ci.yml has no host-tests job; branch protection requires a check of that name"
  elif [ "$(cut -f3 <<<"$host_tests")" = 1 ]; then
    fail "ci.yml's host-tests job is a matrix, so what it reports depends on the matrix rather than being 'host-tests'; run a second RomM as a job of its own"
  elif ! case "$(cut -f2 <<<"$host_tests")" in "" | host-tests) true ;; *) false ;; esac; then
    fail "ci.yml's host-tests job reports under another name; branch protection requires 'host-tests'"
  else
    ok "the job behind the required host-tests check reports exactly that name"
  fi
fi


echo "== orca.yaml"
if [ ! -f orca.yaml ]; then
  fail "orca.yaml is missing; new worktrees would provision nothing"
else
  grep -q 'issue-command.sh' orca.yaml \
    || fail "orca.yaml's issueCommand no longer points at scripts/orca/issue-command.sh, so a new worktree's agent starts from a bare URL"
  grep -q 'setupAgentStartupPolicy: wait-for-setup' orca.yaml \
    || fail "orca.yaml no longer holds the agent tab for setup; agents will run ctest against a RomM that is not up"
  # Every script it names has to exist and run. Orca reports a missing hook as
  # a failed setup in a log nobody opens, and the worktree arrives with no RomM
  # -- which is what a move of the provisioning scripts (#277) would leave.
  named="$(grep -oE '\./scripts/[A-Za-z0-9_./-]+\.sh' orca.yaml | sort -u)"
  [ -n "$named" ] || fail "orca.yaml names no script at all; this check would assert nothing"
  for script in $named; do
    [ -x "$script" ] || fail "orca.yaml names $script, which does not exist or is not executable"
  done
  ok "orca.yaml still wires the agent to the spec, through scripts that exist"
fi


echo "== the scripts and hooks"
# The provisioning scripts, the hooks, the suite's shell and the selftests:
# bash -n, shellcheck, the pipefail and stderr traps. Run here too, so a
# worktree sees a break before CI does.
if [ -x evals/run.sh ]; then
  ./evals/run.sh 2>&1 | sed 's/^/  /'
  [ "${PIPESTATUS[0]}" = 0 ] || fail "evals/run.sh: a script or hook is not well-formed"
else
  fail "evals/run.sh is missing; the scripts and hooks are never checked"
fi

echo "== eval cases"
cases=(evals/cases/*.json)
[ "${#cases[@]}" -gt 0 ] || fail "evals/cases/ is empty; the eval job would pass by having nothing to run"
for case_file in "${cases[@]}"; do
  python3 -c '
import json, sys
c = json.load(open(sys.argv[1]))
for key in ("name", "prompt", "expect"):
    if key not in c:
        sys.exit("missing key: " + key)
e = c["expect"]
if not isinstance(e, dict) or not (e.get("contains") or e.get("absent")):
    sys.exit("expect must carry contains and/or absent")
' "$case_file" || fail "$case_file is not a well-formed eval case"
  ok "$(basename "$case_file")"
done

echo
if [ "$fails" -gt 0 ]; then
  echo "$fails problem(s) in the agent configuration." >&2
  exit 1
fi
echo "agent configuration is well-formed."
