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
# The exhaustive table lives in the hook itself (`guard.py --selftest`), next to
# the code it constrains, so a guard and its assertion cannot drift into
# separate files. It runs here so `ctest -R agent.config` and CI both cover it.
#
# One deliberate exception, below: the fleet's self-protection is asserted HERE
# rather than in the table, because a fleet-opened worktree cannot write
# `.claude/hooks/` -- which is the very rule being asserted. The table cannot
# grow an assertion that only a hand-opened worktree may add, so `guard.py
# --selftest` alone is no longer the whole record. This file is the rest of it.
if [ -x .claude/hooks/guard.py ]; then
  python3 .claude/hooks/guard.py --selftest 2>&1 | sed 's/^/  /'
  [ "${PIPESTATUS[0]}" = 0 ] || fail "the guard selftest does not hold"

  # ...and it has to hold from INSIDE a fleet worktree too. The stateless cases
  # assert what the guard does for an ordinary developer ("an ordinary push is
  # fine"), which is untrue by design where the fleet's own rules apply. Run
  # against the real fleet directory the selftest passed on a laptop and failed
  # in every agent's worktree -- `ctest -R agent.config` red exactly where the
  # work happens, green only on CI runners that are nobody's fleet.
  guard_tmp="$(mktemp -d)"
  mkdir -p "$guard_tmp/worktrees"
  printf '%s\n' "$REPO_ROOT" >"$guard_tmp/worktrees/999"
  ROMMSYNC_FLEET_DIR="$guard_tmp" python3 .claude/hooks/guard.py --selftest >/dev/null 2>&1 \
    || fail "the guard selftest depends on where it is run: it fails inside a fleet worktree"
  ok "the guard selftest holds from inside a fleet worktree too"

  # ...and the fleet gate has to cover every path in SELF_PROTECTED, not just
  # the hook. `_stateful_checks` drives `.claude/hooks/` and stops there, so
  # dropping `.claude/settings.json` from SELF_PROTECTED leaves every one of its
  # assertions green. settings.local.json is the sharper half -- it is
  # gitignored, so a permission rule written into it appears in no diff, which
  # is exactly why the guard covers it.
  #
  # Nothing stops `_stateful_checks` from asserting this; what stops it is the
  # workflow constraint at the top of this section -- a fleet-opened worktree
  # cannot write `.claude/hooks/`, so the table cannot grow the assertion. Here
  # it can.
  #
  # Absolute and root-relative paths only. The `cd`-relative form is a real hole
  # in the guard rather than a gap in these assertions -- see #139.
  tool_call() {
    python3 -c 'import json, sys; print(json.dumps({"tool_name": sys.argv[1], "tool_input": {sys.argv[2]: sys.argv[3]}}))' \
      "$1" "$2" "$3"
  }
  assert_fleet_blocks() {
    local why
    why="$(printf '%s' "$1" \
      | ROMMSYNC_FLEET_DIR="$guard_tmp" python3 .claude/hooks/guard.py 2>&1 >/dev/null)"
    local got=$?
    # Exit 2 on its own is not proof: the guard also exits 2 for a payload it
    # cannot read (asserted below), so a malformed tool call would report ok for
    # a check that never reached the self-protection branch. Match the reason.
    case "$got:$why" in
      2:*"enforcement layer"*) ok "$2" ;;
      2:*) fail "$2: blocked, but not as the enforcement layer: $why" ;;
      *)   fail "$2: the fleet gate let it through (exit $got)" ;;
    esac
  }

  # The list below is literal on purpose: derived from SELF_PROTECTED, removing
  # an entry would remove its own check. That catches a removal but not an
  # ADDITION -- a fourth marker would get no assertion and the claim above would
  # quietly narrow -- so pin the set as well.
  if sp_drift="$(python3 -c '
import importlib.util, sys
spec = importlib.util.spec_from_file_location("g", ".claude/hooks/guard.py")
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
want = {"/.claude/hooks/", "/.claude/settings.json", "/.claude/settings.local.json"}
if set(g.SELF_PROTECTED) != want:
    print(repr(sorted(g.SELF_PROTECTED)))
    sys.exit(1)
')"; then
    ok "SELF_PROTECTED still names exactly the paths asserted below"
  else
    fail "SELF_PROTECTED is now $sp_drift; give each entry an assertion below and update this list"
  fi

  # guard.py decides ownership from `git rev-parse --show-toplevel`, and with no
  # repo root it correctly allows the write. Without this the six assertions
  # below would go red claiming the fleet gate leaked, when the real cause is
  # git -- dubious-ownership inside a container, a source export with no .git,
  # no git on PATH. `_stateful_checks` guards the same dependency with `if root:`.
  if ! git rev-parse --show-toplevel >/dev/null 2>&1; then
    fail "git cannot resolve this checkout, so the fleet gate cannot be exercised"
  else
    for target in .claude/hooks/guard.py .claude/settings.json .claude/settings.local.json; do
      assert_fleet_blocks "$(tool_call Edit file_path "$REPO_ROOT/$target")" \
        "a fleet worktree cannot edit $target"
      assert_fleet_blocks "$(tool_call Bash command "echo x > $target")" \
        "...nor rewrite $target from the shell"
    done
  fi

  rm -rf "$guard_tmp"
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
assert_hook "$G" 2 'not json at all' \
  "an unreadable payload blocks rather than failing open"

echo "== the plugins the flow depends on"
# mattpocock-skills is not decoration: the brief in issue-command.sh tells every
# agent to run `/mattpocock-skills:code-review` (standards and spec-vs-diff,
# which the built-in review does not cover) and `/mattpocock-skills:tdd`. It is
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

echo "== the flow's own scripts"
# The brief names these by path. A rename that misses the brief turns into an
# agent halfway through a task running a command that does not exist.
for script in fleet.sh stop.sh await-review.sh review-status.sh record-review.sh \
              resolve-thread.sh issue-command.sh agent-autostart.sh; do
  path="scripts/orca/$script"
  [ -x "$path" ] || { fail "$path is missing or not executable"; continue; }
  bash -n "$path" || { fail "$path does not parse"; continue; }
  ok "$script"
done
# ...and the brief must still name them.
brief="$(sed -n "/^sed .*BRIEF/,/^BRIEF$/p" scripts/orca/issue-command.sh)"
for named in record-review.sh await-review.sh review-status.sh resolve-thread.sh; do
  grep -q "$named" <<<"$brief" \
    || fail "the agent brief no longer mentions $named, so the loop stops at that step"
done
ok "the brief still names the review loop"

echo "== every test phase actually runs"
# A phase defined in test_orca_browser.sh and missing from the foreach() list in
# tests/CMakeLists.txt never runs -- not in ctest, not in CI -- and is
# indistinguishable from a phase that passes. It happened: the assertions for a
# whole behaviour shipped green by never being executed. The script and the list
# are two files, so only something reading both can say they agree.
if python3 - <<'PHASES'
import re, sys

# Heredoc bodies are dropped first. The phases write stub `orca` and `gh`
# scripts, and those carry their own two-space `case` arms -- `worktree)`,
# `terminal)` -- which are shell being generated, not phases of this file.
lines, kept, delim = open("tests/test_orca_browser.sh").read().splitlines(), [], None
for line in lines:
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
defined = set(re.findall(r"^  ([a-z0-9_]+)\)$", body, re.M))

cml = open("tests/CMakeLists.txt").read()
listed = set(re.search(r"foreach\(phase\b(.*?)\)", cml, re.S).group(1).split())

bad = False
for name in sorted(defined - listed):
    print(f"{name}: defined in test_orca_browser.sh, never registered in tests/CMakeLists.txt")
    bad = True
for name in sorted(listed - defined):
    print(f"{name}: registered in tests/CMakeLists.txt, but the script has no such phase")
    bad = True
sys.exit(1 if bad else 0)
PHASES
then
  ok "every test_orca_browser.sh phase is registered, and every registration exists"
else
  fail "test_orca_browser.sh and tests/CMakeLists.txt disagree about which phases exist; the ones above never run"
fi

echo "== the workflows parse as GitHub reads them"
# An invalid workflow file does not fail loudly: GitHub creates a run with no
# jobs, named after the file, and the check it was meant to report simply never
# appears. With `merge-gate` as a required check that reads as "pending forever"
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

# claude-code-action authenticates by exchanging a GitHub OIDC token, so a job
# that uses it needs `id-token: write` -- at the job level, or inherited from the
# workflow. Without it the action retries three times, fails, and submits
# nothing. That is invisible in the way that matters: `merge-gate` then blocks
# every PR on a review that will never arrive, and the only clue is a red job
# nobody required. It cost the first real fleet run.
if ls .github/workflows/*.yml >/dev/null 2>&1; then
  if python3 - <<'PY'
import re, sys, glob

bad = []
for path in glob.glob(".github/workflows/*.yml"):
    text = open(path).read()
    if "claude-code-action" not in text:
        continue
    # Cheap and good enough: the token is needed somewhere in scope, and these
    # files declare permissions either at the top or on the job.
    if "id-token: write" not in text:
        bad.append(path)
for path in bad:
    print(path)
sys.exit(1 if bad else 0)
PY
  then
    ok "every workflow using claude-code-action grants id-token: write"
  else
    fail "a workflow uses claude-code-action without 'id-token: write'; the action cannot authenticate and will submit nothing"
  fi
fi

# `track_progress: true` forces the action into TAG mode, which waits for an
# @claude trigger phrase. On an automatic review there is none, so the action
# skips -- and the JOB GOES GREEN while nothing was reviewed. That is the worst
# shape a failure can take here: merge-gate then blocks every PR on a review
# that was never submitted, and the check that should have said so is passing.
# The KEY, not the word: the comment above the removal in claude-review.yml
# explains what track_progress does, and a bare grep flags its own explanation.
if grep -rnE "^[[:space:]]*track_progress:" .github/workflows/*.yml >/dev/null 2>&1; then
  fail "a workflow sets track_progress, which forces tag mode; an automatic review then skips silently while its job reports success"
else
  ok "no workflow forces tag mode on an automatic review"

fi

# Silence is this workflow's failure mode and it is invisible: the action can
# burn 35 turns and real money, decide a verdict, and end without ever running
# `gh pr review` -- is_error false, job green, nothing on the PR. That happened
# on #99 twice on one head, and every watcher downstream then waits forever for
# a review that already came and went.
#
# Two things have to stay true, and both are one careless edit from gone.
review_wf=".github/workflows/claude-review.yml"
if [ -f "$review_wf" ]; then
  grep -q "no verdict was submitted" "$review_wf" \
    || fail "claude-review.yml no longer notices a review that submitted nothing"
  ok "a review that submits nothing is reported"

  # It must stay a COMMENT. A generated review would satisfy merge_gate.py's
  # requirement for an independent review while carrying no judgement at all --
  # worse than the silence it replaces, because it would merge things.
  if sed -n '/say so if no verdict/,/^      - /p' "$review_wf" | grep -q "gh pr review"; then
    fail "the no-verdict notice submits a REVIEW; that would satisfy merge-gate with no judgement"
  fi
  ok "the no-verdict notice is a comment, never a review"
fi

# The reviewer is told to submit its verdict with a command it can actually run.
#
# `claude_args` grants Read, Grep, Glob and a fixed list of gh/git calls -- no
# Write, no generic Bash, no redirection and no mktemp. The prompt nonetheless
# told it to submit with `--body-file <file>`, a file it had no way to create.
# So a run only submitted at all if it improvised away from its instruction, and
# PR #131 got three green review runs and zero reviews out of it. A green job
# that did nothing is the shape nothing else here catches.
if [ -f "$review_wf" ]; then
  # 2>&1 because the checks below report by `sys.exit("...")`, which writes to
  # stderr; without it the failure would print a reason that is an empty string.
  if reason="$(python3 - "$review_wf" 2>&1 <<'REVIEWCMD'
import re, sys

text = open(sys.argv[1]).read()
# From `review:` to the next top-level job key, BY SHAPE. Naming the job that
# follows would hard-code the very thing the comment on the sed below says not
# to, and the two slices of this same file must not disagree.
start = text.find("\n  review:")
if start < 0:
    sys.exit("claude-review.yml has no `review:` job")
after = re.search(r"\n  [a-z][a-z_-]*:\n", text[start + 1:])
job = text[start:start + 1 + after.start()] if after else text[start:]

args = re.search(r"claude_args:\s*(.+)", job)
if not args:
    sys.exit("the review job has no claude_args, so what it may run is unknown")
allowed = args.group(1)
# Anything that could create a file for --body-file to read.
can_write = ("Write" in allowed
             or re.search(r"Bash(?!\()", allowed)
             or "Bash(mktemp" in allowed)
# The COMMAND, not the word: the prompt explains why --body-file is wrong, and a
# bare search flags its own explanation.
told_to = [ln for ln in job.splitlines()
           if "gh pr review" in ln and "--body-file" in ln]
if told_to and not can_write:
    sys.exit("the review prompt submits with `--body-file`, and the job grants "
             "no tool that can create a file: " + told_to[0].strip())
if "gh pr review" not in job:
    sys.exit("the review prompt no longer names `gh pr review`, so nothing tells "
             "it to submit a review at all")
if "Bash(gh pr review:" not in allowed:
    sys.exit("the review job does not allow `gh pr review`, so it cannot submit")
REVIEWCMD
)"
  then
    ok "the reviewer can run the command it is told to submit with"
  else
    # The check's OWN words. A fixed string here reported a renamed job or a
    # missing claude_args as "the reviewer cannot run its submit command",
    # which sends the reader to the wrong line.
    fail "claude-review.yml: $reason"
  fi

  # A review is not a build. Cancelling the run that was producing the verdict
  # leaves that head with none -- and the no-verdict notice is `needs: review`,
  # so it does not fire either. #86's merged head and #81's both show
  # `cancelled` for this job.
  # From `review:` to the next top-level job key, by shape rather than by name:
  # a range hard-coded to `verdict:` would silently swallow the rest of the file
  # the day that job is renamed, and pick up the `mention` job's own
  # cancel-in-progress -- a failure about the wrong job.
  if sed -n '/^  review:/,/^  [a-z][a-z_-]*:$/p' "$review_wf" \
       | grep -qE "^[[:space:]]*cancel-in-progress:[[:space:]]*true"; then
    fail "the review job cancels in progress; a killed review leaves the head with no verdict and nothing that says so"
  fi
  ok "a review in flight is never cancelled by the next event"

  # ...and when a review IS silent, the pipeline asks once more on its own. The
  # comment alone named two remedies and both were manual, so a PR whose review
  # said nothing waited for a person to notice it.
  grep -q 'gh workflow run claude-review.yml' "$review_wf" \
    || fail "a review that submitted nothing no longer asks for another one; the PR waits for a person"
  ok "a silent review asks for exactly one more"
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

echo "== the cross-reference conventions"
# `Closes #N` and `Blocked by #N` are read by three parsers -- GitHub itself,
# unblock.yml, and fleet.sh via merge_gate.py's neighbour -- and every way they
# disagree is silent. The fleet either opens a second worktree for work already
# in flight, or reports "nothing startable" with the backlog wide open.
if [ -x .github/scripts/issue_refs.py ]; then
  python3 .github/scripts/issue_refs.py --selftest 2>&1 | sed 's/^/  /'
  [ "${PIPESTATUS[0]}" = 0 ] || fail "the issue-reference selftest does not hold"
else
  fail ".github/scripts/issue_refs.py is missing or not executable"
fi

# unblock.yml is JavaScript inside YAML and cannot import the module, so the one
# thing keeping the two in step is that they spell the pattern identically.
# Asserted from BOTH ends: change either alone and this goes red.
grep -qF 'blocked\s+by\s+#(\d+)/gi' .github/workflows/unblock.yml \
  || fail "unblock.yml no longer matches blocked\\s+by\\s+#(\\d+)/gi; issue_refs.BLOCKED_BY is now a different rule from the one that maintains the labels"
python3 -c '
import importlib.util, sys
spec = importlib.util.spec_from_file_location("r", ".github/scripts/issue_refs.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
sys.exit(0 if m.BLOCKED_BY.pattern == r"blocked\s+by\s+#(\d+)" else 1)
' || fail "issue_refs.BLOCKED_BY no longer spells unblock.yml's pattern character for character"
ok "the fleet and unblock.yml read the same blockers"

# ...and EVERY function in the fleet that reads one of the two conventions gets
# it from the module. A `grep -q issue_refs` over the whole file would be
# satisfied by one surviving import while the other three regrew patterns of
# their own, which is precisely the drift this section exists to catch -- so
# each function is checked on its own, by name.
fleet_reads_shared=1
for fn in ready_issues has_open_pr issue_is_done count_startable; do
  body="$(sed -n "/^$fn()/,/^}/p" scripts/orca/fleet.sh)"
  [ -n "$body" ] \
    || { fail "scripts/orca/fleet.sh has no $fn(); this check no longer covers what it names"
         fleet_reads_shared=0; continue; }
  grep -q 'from issue_refs import' <<<"$body" \
    || { fail "fleet.sh's $fn() no longer imports issue_refs; it can drift from GitHub and unblock.yml again"
         fleet_reads_shared=0; }
  # Any regex of its own over either convention, in any spelling and either
  # language -- not just the two capitalisations it used to carry.
  if grep -inE '(close[sd]?|fix(e[sd])?|resolve[sd]?|blocked[^"]*by)[^"]*#' <<<"$body" \
     | grep -vi '^ *[0-9]*: *#' | grep -q .; then
    fail "fleet.sh's $fn() spells out a closing or blocker reference again; there is one place for those, .github/scripts/issue_refs.py"
    fleet_reads_shared=0
  fi
done
[ "$fleet_reads_shared" = 1 ] && ok "every fleet function reading a body reads the shared patterns"

echo "== the merge gate"
# The one required check `gh pr merge --auto` waits on. Its decision lives in a
# script rather than in the YAML precisely so it can be tested without a pull
# request -- and so a change to what "may merge" means fails here first.
if [ -x .github/scripts/merge_gate.py ]; then
  python3 .github/scripts/merge_gate.py --selftest 2>&1 | sed 's/^/  /'
  [ "${PIPESTATUS[0]}" = 0 ] || fail "the merge-gate selftest does not hold"
else
  fail ".github/scripts/merge_gate.py is missing or not executable"
fi
grep -q 'merge_gate.py' .github/workflows/merge-gate.yml \
  || fail "merge-gate.yml no longer calls merge_gate.py, so the check decides nothing"
# merge_gate.py imports issue_refs.py, and the gate job checks out
# `.github/scripts` ALONE. Widen that import to anything outside this directory
# and the gate stops with an ImportError -- a required check that can never
# conclude, on every PR.
if ! grep -q 'sparse-checkout: .github/scripts' .github/workflows/merge-gate.yml; then
  fail "merge-gate.yml no longer sparse-checks-out .github/scripts; the paths merge_gate.py imports from are no longer the ones it gets"
fi
sparse_tmp="$(mktemp -d)"
cp -R .github/scripts "$sparse_tmp/scripts"
if ( cd "$sparse_tmp/scripts" && python3 -c 'import merge_gate' ) 2>"$sparse_tmp/err"; then
  ok "the gate still imports from the only directory it is given"
else
  fail "merge_gate.py does not import with only .github/scripts on disk, which is all the gate job checks out: $(tr '\n' ' ' <"$sparse_tmp/err")"
fi
rm -rf "$sparse_tmp"

# One query, paginated, in one file. `reviewThreads(first:100)` is the FIRST
# hundred: past that a PR silently loses its newest threads and the gate reports
# "no review thread is unresolved" from a page it knew was partial. Both readers
# used to carry their own copy of that query, and so their own copy of the bug.
if [ -x .github/scripts/pr_payload.sh ]; then
  bash -n .github/scripts/pr_payload.sh || fail ".github/scripts/pr_payload.sh does not parse"
  for reader in .github/workflows/merge-gate.yml scripts/orca/review-status.sh; do
    grep -q 'pr_payload.sh' "$reader" \
      || fail "$reader does not read the PR through .github/scripts/pr_payload.sh, so it is paging threads on its own again"
    # Comment lines excluded: both files EXPLAIN what `reviewThreads(first:100)`
    # got wrong, and a bare grep flags its own explanation.
    grep -vE '^[[:space:]]*#' "$reader" | grep -q 'reviewThreads(first:' \
      && fail "$reader carries its own reviewThreads query again; the first page is not the list"
  done
  # merge-gate.yml runs BOTH of these out of the BASE branch's checkout, and a
  # base predating either one is a PR a person merges -- which has to be SAID.
  # Before the guard named both, the second one to be added killed the step with
  # `No such file or directory` on exactly the PR introducing it.
  for needed in merge_gate.py pr_payload.sh; do
    sed -n '/agreed rule to judge this by/,/^      - name:/p' \
      .github/workflows/merge-gate.yml | grep -q "$needed" \
      || fail "merge-gate.yml runs $needed from the base checkout without checking the base has it; a base predating it dies with a shell error instead of the human-merge notice"
  done
  ok "a base without the gate's own scripts is told, not crashed into"

  ok "the gate and review-status read one paginated payload"
else
  fail ".github/scripts/pr_payload.sh is missing or not executable"
fi

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
