#!/usr/bin/env bash
# PreToolUse(Edit|Write|NotebookEdit): protected paths.
#
# The companion to guard-bash.sh, for the file-editing tools. Same contract:
# exit 2 blocks and stderr is the explanation Claude sees.
set -uo pipefail

path="$(python3 -c '
import json,sys
try:
    print(json.load(sys.stdin).get("tool_input",{}).get("file_path",""))
except Exception:
    print("")
' 2>/dev/null)"
[ -n "$path" ] || exit 0

deny() { echo "$1" >&2; exit 2; }

case "$path" in
  # "No secrets in the tree." Every one of these is gitignored and holds a real
  # bearer token or key; a session with a reason to write one has a bug.
  */config.ini|*/token.dat*|*/device.dat*|*.env|*/.env)
    deny "Blocked: $path holds per-worktree secrets (CLAUDE.md hard rule 5).
.env is generated -- regenerate it with ./scripts/orca/env.sh instead of editing it." ;;

  */server/contract/captures/*)
    deny "Blocked: server/contract/captures/ is the pinned RomM 5.2.0 contract, and
contract.captures diffs a live probe against it. Editing a capture to match a failing
run silences the only test that notices RomM changing.
Re-capture with server/contract/probe_contract.py and explain the diff in the PR body." ;;

  */.github/workflows/unblock.yml)
    deny "Blocked: unblock.yml derives the blocked/ready labels that decide what other
agents may start (CLAUDE.md, 'Working in parallel'). Changing it changes what three
worktrees are allowed to do. A human edits this one." ;;
esac

exit 0
