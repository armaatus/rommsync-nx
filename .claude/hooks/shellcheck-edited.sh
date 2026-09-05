#!/usr/bin/env bash
# PostToolUse(Edit|Write): a shell script that no longer parses, caught now.
#
# scripts/orca/*.sh provision every worktree on this project, and a syntax error
# in one of them does not fail where it was typed -- it fails in the next
# worktree Orca creates, during setup, as a hook that exits non-zero for a reason
# nothing on screen explains. CI checks the same thing (`Shell scripts parse`),
# but CI is minutes and a PR away.
#
# Never blocks: this runs after the edit, so exit 2 would only add noise to an
# edit that already happened. It reports, and the session fixes it.
set -uo pipefail

path="$(python3 -c '
import json,sys
try:
    print(json.load(sys.stdin).get("tool_input",{}).get("file_path",""))
except Exception:
    print("")
' 2>/dev/null)"

case "$path" in
  *.sh) ;;
  *) exit 0 ;;
esac
[ -f "$path" ] || exit 0

if ! out="$(bash -n "$path" 2>&1)"; then
  echo "$path no longer parses -- CI's 'Shell scripts parse' step will fail on this:"
  echo "$out"
fi
exit 0
