#!/usr/bin/env bash
# Does the agent still do the work? One headless session per case in evals/cases/.
#
# `evals/lint.sh` proves the configuration is well-formed. This proves it still
# has the effect it was written to have -- that the save-safety skill still
# fires on a save-writing question, that an agent asked to point the rig at real
# hardware still refuses, that the finishing conditions are still known. Those
# are the failures a diff review cannot see and a well-formedness check cannot
# either.
#
# Needs CLAUDE_CODE_OAUTH_TOKEN (mint one with `claude setup-token`). CI runs
# this from .github/workflows/agent-config.yml, which skips it when the secret
# is absent rather than failing.
#
# Pass and fail come from the RESPONSE TEXT, never from the CLI's exit code:
# `claude --print` exits 0 even when it produced nothing useful.
#
# The checks are deliberately coarse: a keyword the right answer cannot avoid,
# and a keyword the wrong answer cannot avoid. A case that needs cleverness to
# score is a case that will start lying as models change. Prefer adding a case
# to sharpening one.
#
#   ./evals/run.sh                    # every case
#   ./evals/run.sh save-safety        # one
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

command -v claude >/dev/null 2>&1 || {
  echo "no claude CLI on PATH; install it with: npm install -g @anthropic-ai/claude-code" >&2
  exit 1
}
[ -n "${CLAUDE_CODE_OAUTH_TOKEN:-}${ANTHROPIC_API_KEY:-}" ] || {
  echo "no CLAUDE_CODE_OAUTH_TOKEN; mint one with 'claude setup-token'" >&2
  exit 1
}

shopt -s nullglob
if [ "$#" -gt 0 ]; then
  cases=()
  for name in "$@"; do cases+=("evals/cases/$name.json"); done
else
  cases=(evals/cases/*.json)
fi
[ "${#cases[@]}" -gt 0 ] || { echo "no cases to run" >&2; exit 1; }

OUT="${EVAL_OUT_DIR:-$(mktemp -d)}"
mkdir -p "$OUT"
passed=0
failed=0

for case_file in "${cases[@]}"; do
  [ -f "$case_file" ] || { echo "no such case: $case_file" >&2; failed=$((failed + 1)); continue; }
  name="$(python3 -c "import json;print(json.load(open('$case_file'))['name'])")"
  prompt="$(python3 -c "import json;print(json.load(open('$case_file'))['prompt'])")"

  echo "== $name"
  # Read-only tools only. Every case asks the agent to describe or decide, never
  # to change anything, so a case that edits a file has already failed -- and
  # this is what stops an eval run from mutating the checkout it runs in.
  #
  # The prompt goes in on STDIN, not as a trailing argument: `--allowed-tools` is
  # variadic and swallows a positional after it, which produces an empty run that
  # still exits 0.
  printf '%s' "$prompt" | claude --print \
    --allowed-tools "Read,Grep,Glob" \
    --permission-mode plan \
    --output-format json \
    >"$OUT/$name.json" 2>"$OUT/$name.err" || {
      echo "  FAIL: the session errored"
      sed 's/^/    /' "$OUT/$name.err" | head -5
      failed=$((failed + 1))
      continue
    }

  answer="$(python3 - "$OUT/$name.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
# --output-format json is either the result envelope or a list of messages,
# depending on version. Take the text either way rather than pinning a shape.
if isinstance(d, dict) and "result" in d:
    print(str(d["result"]))
else:
    print(json.dumps(d))
PY
)"

  verdict=0
  # Each expectation is an extended regex; "a|b" means either will do.
  for needle in $(python3 -c "
import json
print('\n'.join(json.load(open('$case_file'))['expect'].get('contains', [])))"); do
    grep -Eiq -- "$needle" <<<"$answer" || {
      echo "  FAIL: the answer never mentions /$needle/"
      verdict=1
    }
  done
  for needle in $(python3 -c "
import json
print('\n'.join(json.load(open('$case_file'))['expect'].get('absent', [])))"); do
    grep -Eiq -- "$needle" <<<"$answer" && {
      echo "  FAIL: the answer contains /$needle/, which it should not"
      verdict=1
    }
  done

  if [ "$verdict" = 0 ]; then
    echo "  pass"
    passed=$((passed + 1))
  else
    echo "  ---- what it actually said ----"
    sed 's/^/  | /' <<<"$answer" | head -25
    failed=$((failed + 1))
  fi
done

echo
echo "$passed passed, $failed failed. Transcripts in $OUT"
[ "$failed" = 0 ]
