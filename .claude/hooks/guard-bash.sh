#!/usr/bin/env bash
# PreToolUse(Bash): the deterministic layer behind the rules CLAUDE.md states.
#
# CLAUDE.md and the skills beside this file are advisory -- a session can read
# them and still do the thing. Everything here is a rule where "rare" is not
# good enough, so it is enforced rather than asked for. Blocking is exit 2, and
# the message on stderr is what Claude is told, so every block says what it
# stopped and where the approval route is.
#
# Deliberately short. A hook runs on every matching action, so anything slow or
# clever here is paid on every command in every parallel session.
set -uo pipefail

cmd="$(python3 -c '
import json,sys
try:
    print(json.load(sys.stdin).get("tool_input",{}).get("command",""))
except Exception:
    print("")
' 2>/dev/null)"
[ -n "$cmd" ] || exit 0

deny() { echo "$1" >&2; exit 2; }

# "A human merges. Do not merge your own PR." -- CLAUDE.md. Separation of duties
# is the one review control this project has, and an agent that can merge is not
# separated from anything.
case "$cmd" in
  *"gh pr merge"*)
    deny "Blocked: agents do not merge PRs on this repo (CLAUDE.md, 'Finishing a task').
A human merges. Open the PR, put the /code-review findings in its body, and stop there." ;;
esac

# A force-push to a shared branch destroys the audit trail the whole workflow is
# built on -- the commit chain is the record of who asked for what.
case "$cmd" in
  *"push"*"--force"*|*"push"*" -f "*)
    case "$cmd" in
      *main*|*origin\ main*)
        deny "Blocked: force-pushing main rewrites the commit chain that is this project's audit trail.
Push to your branch instead, or ask a human to do it deliberately." ;;
    esac ;;
esac

# The pinned RomM snapshot. contract.captures diffs a live probe against these
# files, so editing them to match a failing run turns the one test that can
# notice a server change into a test that agrees with whatever happened.
case "$cmd" in
  *server/contract/captures/*)
    case "$cmd" in
      *rm\ *|*mv\ *|*">"*|*tee\ *|*sed\ -i*)
        deny "Blocked: server/contract/captures/ is the pinned RomM 5.2.0 contract.
Re-capture it with server/contract/probe_contract.py and say in the PR body what changed and why." ;;
    esac ;;
esac

exit 0
