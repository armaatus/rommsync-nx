#!/usr/bin/env bash
# Covers scripts/orca/romm-browser.sh and scripts/orca/agent-autostart.sh --
# the two things that make a new worktree land ready to look at and ready to work.
#
#   test_orca_browser.sh opens     no tab yet -> ask the CLI for one, on this
#                                  worktree, at this worktree's port, and log the
#                                  page in through /api/login.
#   test_orca_browser.sh reuses    a tab for this worktree is already on this
#                                  RomM -> re-authenticate it, do not open a
#                                  second one. This is what keeps a hand-run of
#                                  the script from piling up tabs.
#   test_orca_browser.sh foreign   a tab on the same port belonging to ANOTHER
#                                  worktree is not adopted. Three worktrees run
#                                  three RomMs and `orca tab list` sees them all.
#   test_orca_browser.sh no_romm   RomM is not answering -> say so and open
#                                  nothing, rather than a tab onto a connection
#                                  error.
#   test_orca_browser.sh submits   the agent has a drafted issue prompt -> send
#                                  Return, once, after reading it twice the same.
#                                  This is the manual keypress being removed.
#   test_orca_browser.sh no_draft  nothing drafted -> send nothing. A worktree
#                                  created without an issue must not have a bare
#                                  Return pushed into its agent.
#   test_orca_browser.sh unstable  the draft is still being pasted -> wait for it
#                                  to settle rather than submitting a truncated
#                                  spec.
#
# Every phase stubs the `orca` CLI on PATH, so they assert what the scripts would
# do to a real workspace without touching one. None of them need Docker, so the
# guarantees stay checked in CI.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() { echo "FAIL: $*" >&2; exit 1; }

TMPDIR_FIXTURE=""
cleanup() { [ -n "$TMPDIR_FIXTURE" ] && rm -rf "$TMPDIR_FIXTURE"; return 0; }
trap cleanup EXIT

PORT=21999
BASE="http://127.0.0.1:$PORT"

# A worktree root holding just enough of the repo to run the orca scripts.
make_fixture() {
  TMPDIR_FIXTURE="$(mktemp -d)"
  # macOS hands out /var/... which is a symlink to /private/var; the scripts
  # resolve their own root with `cd -P`-equivalent semantics, so the fixture must
  # be compared by the resolved path or every worktree match below misses.
  TMPDIR_FIXTURE="$(cd "$TMPDIR_FIXTURE" && pwd -P)"
  mkdir -p "$TMPDIR_FIXTURE/scripts/orca" "$TMPDIR_FIXTURE/server/testing"
  cp "$REPO_ROOT"/scripts/orca/{romm-browser.sh,agent-autostart.sh,lib.sh,env.sh,compose.sh} \
     "$TMPDIR_FIXTURE/scripts/orca/"
  cat >"$TMPDIR_FIXTURE/.env" <<ENV
COMPOSE_PROJECT_NAME=rmx-test-browser-$$
ROMM_PORT=$PORT
PROXY_PORT=23999
ROMM_BASE_URL=$BASE
PROXY_BASE_URL=http://127.0.0.1:23999
ENV
  cat >"$TMPDIR_FIXTURE/server/testing/fixture-auth.env" <<AUTH
ROMM_FIXTURE_USER=rommsync
ROMM_FIXTURE_PASSWORD=rommsync-test-only
AUTH
}

# An `orca` on PATH that records its arguments and answers with the runtime's
# JSON shape. $1 is the fixture root; $2..$n are `key=value` behaviour switches
# read from the environment by the stub.
stub_orca() {
  local dir="$1/stub-bin"
  mkdir -p "$dir"
  cat >"$dir/orca" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$ORCA_CALLS"
# `orca eval --page X` and `orca tab list` are one- and two-word commands, so
# the verb is matched first and the subcommand only where there is one.
case "$1" in
  eval) printf '{"ok":true,"result":{"result":"%s"}}\n' "${ORCA_EVAL_RESULT:-ok:rommsync}" ;;
  tab)
    case "${2:-}" in
      list)   cat "$ORCA_TABS" ;;
      create) printf '{"ok":true,"result":{"browserPageId":"page-created"}}\n' ;;
      *)      printf '{"ok":true,"result":{}}\n' ;;
    esac ;;
  terminal)
    case "${2:-}" in
      list) cat "$ORCA_TERMINALS" ;;
      read) cat "$ORCA_READ" ;;
      *)    printf '{"ok":true,"result":{}}\n' ;;
    esac ;;
  *) printf '{"ok":true,"result":{}}\n' ;;
esac
STUB
  chmod +x "$dir/orca"
  : >"$1/orca-calls.log"
  echo "$dir"
}

# curl on PATH that reports RomM up ($1=up) or unreachable ($1=down), so no
# phase depends on a listening port.
stub_curl() {
  local dir="$1/stub-bin"
  mkdir -p "$dir"
  cat >"$dir/curl" <<STUB
#!/usr/bin/env bash
exit $([ "$2" = up ] && echo 0 || echo 7)
STUB
  chmod +x "$dir/curl"
}

run_browser() {
  PATH="$TMPDIR_FIXTURE/stub-bin:$PATH" \
  ORCA_CALLS="$TMPDIR_FIXTURE/orca-calls.log" \
  ORCA_TABS="$TMPDIR_FIXTURE/tabs.json" \
  ORCA_EVAL_RESULT="${ORCA_EVAL_RESULT:-ok:rommsync}" \
  ROMM_BROWSER_WAIT_SECONDS=2 \
    bash "$TMPDIR_FIXTURE/scripts/orca/romm-browser.sh" 2>&1
}

run_autostart() {
  PATH="$TMPDIR_FIXTURE/stub-bin:$PATH" \
  ORCA_CALLS="$TMPDIR_FIXTURE/orca-calls.log" \
  ORCA_TERMINALS="$TMPDIR_FIXTURE/terminals.json" \
  ORCA_READ="$TMPDIR_FIXTURE/read.json" \
  AGENT_AUTOSTART_PIDFILE="$TMPDIR_FIXTURE/autostart.pid" \
    bash "$TMPDIR_FIXTURE/scripts/orca/agent-autostart.sh" "$@" 2>&1
}

# `orca terminal list` with one agent terminal in $1 and one in another worktree.
write_terminals() {
  cat >"$TMPDIR_FIXTURE/terminals.json" <<JSON
{"ok":true,"result":{"terminals":[
  {"handle":"term_logs","worktreePath":"$1","agentIdentity":null,"orphaned":false},
  {"handle":"term_other","worktreePath":"/somewhere/else","agentIdentity":"claude","orphaned":false},
  {"handle":"term_agent","worktreePath":"$1","agentIdentity":"claude","orphaned":false}
]}}
JSON
}

case "${1:-}" in
  opens)
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    stub_curl "$TMPDIR_FIXTURE" up
    echo '{"ok":true,"result":{"tabs":[]}}' >"$TMPDIR_FIXTURE/tabs.json"

    out="$(run_browser)"
    calls="$(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q "tab create" <<<"$calls" || fail "opened no tab; got: ${calls:-<nothing>} / $out"
    grep -q -- "--url $BASE/" <<<"$calls" \
      || fail "tab would open on the wrong RomM (ports are per worktree): $calls"
    grep -q -- "path:$TMPDIR_FIXTURE" <<<"$calls" \
      || fail "tab would land in the wrong worktree: $calls"
    # The whole point of the script: a tab nobody is logged into is the state it
    # exists to remove, and opening one is the easy half.
    grep -q "^eval " <<<"$calls" || fail "tab was opened but never signed in: $calls"
    grep -q -- "--page page-created" <<<"$calls" \
      || fail "login was not pinned to the tab it opened -- it could drive another worktree's: $calls"
    grep -q "/api/login" <<<"$calls" || fail "signed in by some route other than the pinned API: $calls"
    grep -q "signed in as rommsync" <<<"$out" || fail "did not report a signed-in tab; got: $out"
    echo "PASS: a worktree with no tab gets one, on its own RomM, signed in"
    ;;

  reuses)
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    stub_curl "$TMPDIR_FIXTURE" up
    cat >"$TMPDIR_FIXTURE/tabs.json" <<JSON
{"ok":true,"result":{"tabs":[
  {"browserPageId":"page-existing","url":"$BASE/","worktreeId":"repo1::$TMPDIR_FIXTURE"}
]}}
JSON

    out="$(run_browser)"
    calls="$(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q "tab create" <<<"$calls" \
      && fail "opened a second tab onto the same RomM: $calls"
    grep -q -- "--page page-existing" <<<"$calls" \
      || fail "did not drive the tab it found: $calls"
    grep -q "signed in as rommsync" <<<"$out" \
      || fail "reused the tab without re-authenticating it; got: $out"
    echo "PASS: an existing tab is reused and re-authenticated, not duplicated"
    ;;

  foreign)
    # Same port, different worktree. Matching on the URL alone would hand this
    # script another agent's tab to log in and navigate.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    stub_curl "$TMPDIR_FIXTURE" up
    cat >"$TMPDIR_FIXTURE/tabs.json" <<JSON
{"ok":true,"result":{"tabs":[
  {"browserPageId":"page-elsewhere","url":"$BASE/","worktreeId":"repo1::/some/other/worktree"}
]}}
JSON

    out="$(run_browser)"
    calls="$(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q -- "--page page-elsewhere" <<<"$calls" \
      && fail "adopted another worktree's browser tab: $calls"
    grep -q "tab create" <<<"$calls" \
      || fail "neither adopted nor opened a tab; got: ${calls:-<nothing>} / $out"
    echo "PASS: another worktree's tab on the same port is left alone"
    ;;

  no_romm)
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    stub_curl "$TMPDIR_FIXTURE" down
    echo '{"ok":true,"result":{"tabs":[]}}' >"$TMPDIR_FIXTURE/tabs.json"

    out="$(run_browser)"
    rc=$?
    [ "$rc" -eq 0 ] || fail "failed the caller over a browser tab (setup.sh would abort): rc=$rc"
    grep -q "tab create" "$TMPDIR_FIXTURE/orca-calls.log" \
      && fail "opened a tab onto a RomM that is not answering: $(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q "not answering" <<<"$out" || fail "did not say why there is no tab; got: $out"
    echo "PASS: no RomM means no tab, and setup is not failed over it"
    ;;

  submits)
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    cat >"$TMPDIR_FIXTURE/read.json" <<'JSON'
{"ok":true,"result":{"terminal":{"handle":"term_agent","draft":"# 12: Do the thing\nBody of the issue."}}}
JSON

    out="$(run_autostart)"
    calls="$(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q "terminal send" <<<"$calls" \
      || fail "left the agent sitting on an unsent prompt (this is the bug): ${calls:-<nothing>} / $out"
    grep -q -- "--terminal term_agent" <<<"$calls" \
      || fail "sent Return to the wrong terminal -- the log or shell tab: $calls"
    grep -q -- "--enter" <<<"$calls" || fail "sent something other than Return: $calls"
    # Once. A second Return lands in an agent that is already working.
    [ "$(grep -c "terminal send" <<<"$calls")" -eq 1 ] \
      || fail "pressed Return more than once: $calls"
    echo "PASS: a drafted issue prompt is submitted, once, to the agent terminal"
    ;;

  no_draft)
    # A worktree created without a linked issue. A bare Return here would land in
    # whatever the agent is doing.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    echo '{"ok":true,"result":{"terminal":{"handle":"term_agent"}}}' >"$TMPDIR_FIXTURE/read.json"

    out="$(run_autostart)"
    grep -q "terminal send" "$TMPDIR_FIXTURE/orca-calls.log" \
      && fail "pressed Return into an agent with nothing drafted: $(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q "nothing drafted" <<<"$out" || fail "did not report an empty composer; got: $out"

    # And a draft that is only whitespace is not a draft.
    : >"$TMPDIR_FIXTURE/orca-calls.log"
    echo '{"ok":true,"result":{"terminal":{"draft":"   \n  "}}}' >"$TMPDIR_FIXTURE/read.json"
    run_autostart >/dev/null
    grep -q "terminal send" "$TMPDIR_FIXTURE/orca-calls.log" \
      && fail "treated a whitespace draft as a prompt: $(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    echo "PASS: nothing drafted means nothing sent"
    ;;

  unstable)
    # Orca pastes the spec into the composer; a reading taken part way through is
    # a shorter string. Submitting there sends the agent a truncated issue, which
    # is worse than the keypress this replaces.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    # A read that returns something different every call.
    cat >"$TMPDIR_FIXTURE/stub-bin/orca" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$ORCA_CALLS"
case "$1 ${2:-}" in
  "terminal list") cat "$ORCA_TERMINALS" ;;
  "terminal read")
    n=$(( $(cat "$ORCA_READ_COUNT" 2>/dev/null || echo 0) + 1 ))
    echo "$n" >"$ORCA_READ_COUNT"
    printf '{"ok":true,"result":{"terminal":{"draft":"# 12: partial %s"}}}\n' "$n" ;;
  *) printf '{"ok":true,"result":{}}\n' ;;
esac
STUB
    chmod +x "$TMPDIR_FIXTURE/stub-bin/orca"

    out="$(ORCA_READ_COUNT="$TMPDIR_FIXTURE/read-count" run_autostart)"
    grep -q "terminal send" "$TMPDIR_FIXTURE/orca-calls.log" \
      && fail "submitted a draft that was still changing: $(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    echo "PASS: a draft still being pasted is left to settle"
    ;;

  *)
    echo "usage: $0 opens|reuses|foreign|no_romm|submits|no_draft|unstable" >&2
    exit 2
    ;;
esac
