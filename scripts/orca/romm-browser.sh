#!/usr/bin/env bash
# Put this worktree's RomM on screen, already logged in -- setup.sh's last step.
#
# The fixture is the environment we are building against, and until now the only
# window onto it was a log stream. Reading logs tells you the server is alive; it
# does not tell you the library scanned, which platforms RomM decided the seeded
# folders are, or what a rom's metadata actually looks like. Those are exactly
# the questions that come up while implementing against the API, and they are one
# glance away in the web UI -- if you are logged in.
#
# So this opens an Orca browser tab on this worktree's RomM and authenticates the
# tab's session through the real `/api/login`, with the same fixture credentials
# the test suite uses. No password prompt, no copy-pasting a port.
#
# Logging in is done from inside the page rather than by driving the login form:
# the form is a Vue app whose markup is RomM's to change, while `/api/login` is
# the pinned contract in docs/API_CONTRACT.md. One `fetch` that RomM's own client
# also makes leaves the tab holding a genuine `romm_session` cookie.
#
# Idempotent, and never fails setup: a browser tab is a convenience, and a
# worktree that refuses to provision because it could not open one is not.
#
#   ./scripts/orca/romm-browser.sh          # open (or re-authenticate) the tab
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

[ -f .env ] || ./scripts/orca/env.sh >/dev/null 2>&1
set -a; . ./.env; set +a

# These MUST match provision.py's FIXTURE_USER/FIXTURE_PASSWORD and rig::kUser/
# kPassword in tests/rig.hpp. fixture-auth.env is the provisioner's own record of
# what it created, so prefer it and treat these as the fallback for a worktree
# whose provisioning has not run yet.
ROMM_FIXTURE_USER="rommsync"
ROMM_FIXTURE_PASSWORD="rommsync-test-only"
# shellcheck disable=SC1091
[ -f server/testing/fixture-auth.env ] && . ./server/testing/fixture-auth.env

CLI_SECONDS="${ROMM_BROWSER_CLI_SECONDS:-25}"
# How long to wait for RomM to answer before opening a tab at all. setup.sh calls
# this after provisioning, so the server is already up and this is slack, not a
# real wait; run by hand against a stopped stack it is what keeps the tab from
# opening onto a connection error.
UP_SECONDS="${ROMM_BROWSER_WAIT_SECONDS:-60}"
HINT="    open it yourself: $ROMM_BASE_URL  ($ROMM_FIXTURE_USER / $ROMM_FIXTURE_PASSWORD)"

CLI_OUT="$(mktemp)"
trap 'rm -f "$CLI_OUT"' EXIT

romm_is_up() {
  curl -fsS --max-time 3 "$ROMM_BASE_URL/api/heartbeat" >/dev/null 2>&1
}

# Picks this worktree's RomM tab out of `orca tab list` (see existing_page).
MATCH_TAB_PY='
import json, sys
root, base = sys.argv[1], sys.argv[2]
try:
    tabs = json.load(sys.stdin)["result"]["tabs"]
except Exception:
    sys.exit(1)
for tab in tabs:
    # worktreeId is "<repo-id>::<path>"; compare the path, which is what this
    # script knows about itself.
    if str(tab.get("worktreeId", "")).split("::", 1)[-1] != root:
        continue
    if str(tab.get("url", "")).startswith(base):
        print(tab["browserPageId"])
        sys.exit(0)
sys.exit(1)
'

# The page id of a tab in THIS worktree already pointing at THIS worktree's RomM.
#
# Scoped both ways on purpose. Three worktrees run three RomMs and `orca tab
# list` sees all of them, so matching on the port alone would hand this script
# another worktree's tab to drive, and matching on the worktree alone would
# adopt a tab someone opened on unrelated documentation.
existing_page() {
  command -v python3 >/dev/null 2>&1 || return 1
  orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" \
    orca tab list --worktree all --json || return 1
  # -c rather than a heredoc on stdin: stdin is the CLI's reply, and a program
  # fed the same way would consume it before the parse ever ran.
  python3 -c "$MATCH_TAB_PY" "$REPO_ROOT" "$ROMM_BASE_URL" <"$CLI_OUT"
}

# Authenticate the tab's own session, then return to the library.
#
# Base64 rather than the password inline: the expression is assembled by the
# shell and evaluated as JavaScript, so a credential carrying a quote or a
# backslash would otherwise be a syntax error at best. Base64 is alphanumerics
# and `+/=`, which is inert in both languages.
authenticate() {
  local page="$1" creds js
  creds="$(printf '%s:%s' "$ROMM_FIXTURE_USER" "$ROMM_FIXTURE_PASSWORD" | base64 | tr -d '\n')"
  # RomM rejects unsafe methods without the romm_csrftoken cookie echoed in an
  # X-CSRFToken header -- the same rule provision.py's session obeys. Any GET
  # mints the cookie, hence the heartbeat first.
  js='(async () => {
    await fetch("/api/heartbeat", {credentials: "include"});
    const m = document.cookie.match(/(?:^|; )romm_csrftoken=([^;]*)/);
    const r = await fetch("/api/login", {
      method: "POST",
      credentials: "include",
      headers: {
        "Authorization": "Basic '"$creds"'",
        "X-CSRFToken": m ? decodeURIComponent(m[1]) : ""
      }
    });
    if (!r.ok) return "login-failed:" + r.status;
    // Asking who we are, rather than trusting the 200: a login that set no
    // usable session cookie is the failure worth catching here, and it is
    // invisible in the login response itself.
    const me = await fetch("/api/users/me", {credentials: "include"});
    if (!me.ok) return "session-failed:" + me.status;
    return "ok:" + ((await me.json()).username || "?");
  })()'
  orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" \
    orca eval --page "$page" --expression "$js" --json || return 1
  grep -q '"ok": *true' "$CLI_OUT" || return 1
  grep -q '"result": *"ok:' "$CLI_OUT"
}

if ! command -v orca >/dev/null 2>&1; then
  # A plain clone or CI: no Orca, no tabs, nothing to do and nothing wrong.
  echo "==> no orca CLI; not opening a browser tab"
  echo "$HINT"
  exit 0
fi

waited=0
while ! romm_is_up; do
  if [ "$waited" -ge "$UP_SECONDS" ]; then
    echo "==> RomM is not answering on $ROMM_BASE_URL; not opening a browser tab"
    echo "    start it with: ./scripts/orca/compose.sh up -d"
    exit 0
  fi
  sleep 2
  waited=$((waited + 2))
done

page="$(existing_page)"
if [ -n "$page" ]; then
  echo "==> reusing the RomM tab ($ROMM_BASE_URL)"
  # Navigate before authenticating: the tab may be sitting on /login from an
  # expired session, and the login fetch has to run against RomM's own origin
  # for the relative paths and the cookie jar to be the right ones.
  orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" \
    orca goto --page "$page" --url "$ROMM_BASE_URL/" --json >/dev/null 2>&1
else
  echo "==> opening RomM ($ROMM_BASE_URL)"
  if ! orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" \
         orca tab create --worktree "path:$REPO_ROOT" --url "$ROMM_BASE_URL/" --json; then
    echo "    the orca CLI did not open a tab -- is the runtime reachable?"
    echo "$HINT"
    exit 0
  fi
  page="$(sed -n 's/.*"browserPageId"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CLI_OUT" | head -1)"
  if [ -z "$page" ]; then
    echo "    the CLI accepted the tab but named no page to log in"
    echo "$HINT"
    exit 0
  fi
fi

if authenticate "$page"; then
  # Back to the library rather than leaving the tab on whatever it was: the
  # login happened in the background of the current document, so the page on
  # screen is still the logged-out one until it reloads.
  orca_run_with_deadline "$CLI_SECONDS" "$CLI_OUT" \
    orca goto --page "$page" --url "$ROMM_BASE_URL/" --json >/dev/null 2>&1
  echo "    signed in as $ROMM_FIXTURE_USER"
else
  echo "    the tab is open but could not be signed in"
  echo "$HINT"
fi

exit 0
