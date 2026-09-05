#!/usr/bin/env bash
# PostToolUse(Edit|Write): a shell script that no longer PARSES, caught now.
#
# `bash -n`, not shellcheck -- the name said otherwise for a while and the
# name was wrong. CI runs the same check across scripts/orca and
# server/testing; this is that check, one push earlier.
#
# scripts/orca/*.sh provision every worktree on this project, and a syntax error
# in one does not fail where it was typed -- it fails in the next worktree Orca
# creates, during setup, as a hook that exits non-zero for a reason nothing on
# screen explains. CI checks the same thing (`Shell scripts parse`), but CI is
# minutes and a PR away.
#
# Never blocks: the edit has already happened, so exit 2 would only add noise to
# something that is now on disk. It tells the SESSION instead, which is the part
# that can fix it -- and that has to go through the hook's JSON output. Plain
# stdout from a PostToolUse hook is shown to the user in transcript mode and
# never reaches Claude, so a message printed that way is a message nobody acts
# on.
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
  python3 - "$path" "$out" <<'PY'
import json, sys
path, out = sys.argv[1], sys.argv[2]
print(json.dumps({
    "hookSpecificOutput": {
        "hookEventName": "PostToolUse",
        "additionalContext": (
            f"{path} no longer parses. CI's 'Shell scripts parse' step will fail on "
            f"this, and if it is one of scripts/orca/*.sh the next worktree Orca "
            f"creates will fail to provision. Fix it before moving on:\n{out}"
        ),
    }
}))
PY
fi
exit 0
