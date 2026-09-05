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
  worktree)
    # `worktree current` is how --watch decides whether a draft is even
    # expected here; ORCA_WORKTREE is the file holding the answer.
    cat "${ORCA_WORKTREE:-/dev/null}" 2>/dev/null \
      || printf '{"ok":true,"result":{"worktree":{}}}\n' ;;
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
  ORCA_WORKTREE="$TMPDIR_FIXTURE/worktree.json" \
  AGENT_AUTOSTART_PIDFILE="$TMPDIR_FIXTURE/autostart.pid" \
  AGENT_AUTOSTART_POLL_SECONDS="${AGENT_AUTOSTART_POLL_SECONDS:-1}" \
  AGENT_AUTOSTART_DEADLINE_SECONDS="${AGENT_AUTOSTART_DEADLINE_SECONDS:-6}" \
    bash "$TMPDIR_FIXTURE/scripts/orca/agent-autostart.sh" "$@" 2>&1
}

# `orca worktree current` answering with ($1=issue) or without ($1=none) a link.
write_worktree() {
  if [ "$1" = issue ]; then
    echo '{"ok":true,"result":{"worktree":{"linkedIssue":4}}}' >"$TMPDIR_FIXTURE/worktree.json"
  else
    echo '{"ok":true,"result":{"worktree":{"linkedIssue":null}}}' >"$TMPDIR_FIXTURE/worktree.json"
  fi
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

  watch_needs_issue)
    # Without a linked issue Orca drafts nothing, so anything in that composer
    # was typed by a person. Watching there is how a half-written prompt gets
    # submitted for them, which is far worse than the Return being replaced.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    write_worktree none
    cat >"$TMPDIR_FIXTURE/read.json" <<'JSON'
{"ok":true,"result":{"terminal":{"draft":"a prompt a human is halfway through typ"}}}
JSON

    out="$(run_autostart --watch)"
    calls="$(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q "terminal send" <<<"$calls" \
      && fail "submitted a human's typing on a worktree with no issue: $calls"
    grep -q "no linked issue" <<<"$out" || fail "did not say why it is not watching; got: $out"
    [ -e "$TMPDIR_FIXTURE/autostart.pid" ] \
      && fail "left a pidfile behind for a watcher that never ran"
    echo "PASS: no linked issue means no watching, so no typing is submitted"
    ;;

  watch_late_draft)
    # The grace window has closed on an empty composer, so Orca's paste is not
    # coming -- text appearing after that is someone typing into it. Grace is
    # forced to one poll here so the phase is about what happens AFTER it, not
    # about how long it is.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    write_worktree issue
    cat >"$TMPDIR_FIXTURE/stub-bin/orca" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$ORCA_CALLS"
case "$1" in
  worktree) cat "$ORCA_WORKTREE" ;;
  terminal)
    case "${2:-}" in
      list) cat "$ORCA_TERMINALS" ;;
      read)
        n=$(( $(cat "$ORCA_READ_COUNT" 2>/dev/null || echo 0) + 1 ))
        echo "$n" >"$ORCA_READ_COUNT"
        # Empty at first -- the agent came up with nothing drafted -- then a
        # steady string, exactly as a person typing and pausing would look.
        if [ "$n" -le 2 ]; then
          printf '{"ok":true,"result":{"terminal":{}}}\n'
        else
          printf '{"ok":true,"result":{"terminal":{"draft":"typed by a human"}}}\n'
        fi ;;
      *) printf '{"ok":true,"result":{}}\n' ;;
    esac ;;
  *) printf '{"ok":true,"result":{}}\n' ;;
esac
STUB
    chmod +x "$TMPDIR_FIXTURE/stub-bin/orca"

    out="$(ORCA_READ_COUNT="$TMPDIR_FIXTURE/read-count" \
           AGENT_AUTOSTART_GRACE_SECONDS=1 run_autostart --watch)"
    grep -q "terminal send" "$TMPDIR_FIXTURE/orca-calls.log" \
      && fail "submitted text that appeared after the grace window closed: $(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q "empty composer" <<<"$out" || fail "did not stop at the empty baseline; got: $out"
    echo "PASS: a draft appearing after the grace window is left alone"
    ;;

  watch_grace)
    # The other side of it. Orca delivers the draft either as a launch argument
    # or by pasting once the agent is ready, and the second of those lands after
    # the watcher's first poll -- a zero-length window would miss every one of
    # them and quietly hand the Return back to a human.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    write_worktree issue
    cat >"$TMPDIR_FIXTURE/stub-bin/orca" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$ORCA_CALLS"
case "$1" in
  worktree) cat "$ORCA_WORKTREE" ;;
  terminal)
    case "${2:-}" in
      list) cat "$ORCA_TERMINALS" ;;
      read)
        n=$(( $(cat "$ORCA_READ_COUNT" 2>/dev/null || echo 0) + 1 ))
        echo "$n" >"$ORCA_READ_COUNT"
        # Empty on the first look, pasted by the second: the agent was up
        # before Orca finished handing it the spec.
        if [ "$n" -le 1 ]; then
          printf '{"ok":true,"result":{"terminal":{}}}\n'
        else
          printf '{"ok":true,"result":{"terminal":{"draft":"# 43: v1 gate"}}}\n'
        fi ;;
      *) printf '{"ok":true,"result":{}}\n' ;;
    esac ;;
  *) printf '{"ok":true,"result":{}}\n' ;;
esac
STUB
    chmod +x "$TMPDIR_FIXTURE/stub-bin/orca"

    out="$(ORCA_READ_COUNT="$TMPDIR_FIXTURE/read-count" run_autostart --watch)"
    grep -q -- "terminal send --terminal term_agent --enter" "$TMPDIR_FIXTURE/orca-calls.log" \
      || fail "gave up before Orca had pasted the draft; got: $out"
    echo "PASS: a draft that lands just after the agent starts is still sent"
    ;;

  watch_submits)
    # The case the whole thing exists for: an issue-linked worktree whose agent
    # comes up with Orca's paste already in the composer.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    write_worktree issue
    cat >"$TMPDIR_FIXTURE/read.json" <<'JSON'
{"ok":true,"result":{"terminal":{"draft":"# 4: Capture real RomM auth shapes"}}}
JSON

    out="$(run_autostart --watch)"
    calls="$(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q -- "terminal send --terminal term_agent --enter" <<<"$calls" \
      || fail "did not submit the drafted issue: ${calls:-<nothing>} / $out"
    [ "$(grep -c "terminal send" <<<"$calls")" -eq 1 ] \
      || fail "pressed Return more than once: $calls"
    # The watcher owns a pidfile while it runs and takes it away when it stops,
    # or archive.sh has nothing to signal and the next setup run stacks a second
    # watcher on the first.
    [ -e "$TMPDIR_FIXTURE/autostart.pid" ] \
      && fail "pidfile outlived the watcher; teardown would signal a stale pid"
    echo "PASS: an issue-linked worktree gets its drafted prompt sent, once"
    ;;

  watch_single)
    # Two watchers racing into one composer. The guard has to survive a pid
    # being recycled, or a watcher never starts again after one is killed -9.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    write_worktree issue
    echo '{"ok":true,"result":{"terminal":{}}}' >"$TMPDIR_FIXTURE/read.json"

    # A live pid that is not a watcher, exactly as a recycled one would look.
    sleep 30 & impostor=$!
    echo "$impostor" >"$TMPDIR_FIXTURE/autostart.pid"
    out="$(run_autostart --watch)"
    kill "$impostor" 2>/dev/null; wait "$impostor" 2>/dev/null
    grep -q "already running" <<<"$out" \
      && fail "a recycled pid stopped the watcher from ever starting again; got: $out"

    # And a real one is respected.
    #
    # `exec -a`, so this is ONE process and not a wrapper around a sleep. The
    # identity check reads the process's own command line, so a plain `exec
    # sleep 30` would show as `sleep` -- what a recycled pid looks like, not
    # what a live watcher does -- and `-a` is what keeps the name while still
    # leaving nothing behind the kill below cannot reach.
    #
    # A wrapper with a background sleep is what this used to be, and the sleep
    # survived: `kill` reaches the wrapper only, so the child was reparented to
    # init and went on holding the stdout ctest reads to decide the test has
    # finished. A TERM trap does not close it either -- the kill races the trap
    # being installed, which leaked on 3 runs in 6.
    printf '#!/usr/bin/env bash\nexec -a agent-autostart-fake sleep 30\n' \
      >"$TMPDIR_FIXTURE/agent-autostart-fake"
    chmod +x "$TMPDIR_FIXTURE/agent-autostart-fake"
    "$TMPDIR_FIXTURE/agent-autostart-fake" & real=$!
    echo "$real" >"$TMPDIR_FIXTURE/autostart.pid"
    out="$(run_autostart --watch)"
    kill "$real" 2>/dev/null; wait "$real" 2>/dev/null
    grep -q "already running" <<<"$out" \
      || fail "started a second watcher beside a live one; got: $out"
    echo "PASS: one watcher per worktree, and a recycled pid does not lock it out"
    ;;

  watch_bare_url)
    # Orca will not run an orca.yaml `issueCommand` it has not been trusted
    # with, and the prompt it drafts then falls back to the bare issue URL. An
    # agent handed a link and nothing else reads a title and invents the scope
    # the issue already specifies -- which is what happened to all three
    # worktrees opened on 2026-09-05. The draft has to be completed before it is
    # submitted, and completed from issue-command.sh, where the brief lives.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    write_worktree issue
    cat >"$TMPDIR_FIXTURE/read.json" <<'JSON'
{"ok":true,"result":{"terminal":{"draft":"https://github.com/armaatus/rommsync-nx/issues/13"}}}
JSON

    out="$(run_autostart --watch)"
    calls="$(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q -- "issue-command.sh 13" <<<"$calls" \
      || fail "submitted a bare URL as the whole brief: ${calls:-<nothing>} / $out"
    # Order matters: the text has to be in the composer before Return.
    [ "$(grep -n "issue-command.sh 13" <<<"$calls" | cut -d: -f1)" \
      -lt "$(grep -n -- "--enter" <<<"$calls" | cut -d: -f1)" ] \
      || fail "pressed Return before the instruction was added: $calls"
    echo "PASS: a draft that is only a link is pointed at the spec before it is sent"
    ;;

  watch_full_draft_untouched)
    # And the trusted path is left exactly as it was: when Orca did draft the
    # template, appending to it would be noise at best.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    write_worktree issue
    cat >"$TMPDIR_FIXTURE/read.json" <<'JSON'
{"ok":true,"result":{"terminal":{"draft":"Run ./scripts/orca/issue-command.sh 13 first and follow everything it prints."}}}
JSON

    out="$(run_autostart --watch)"
    calls="$(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q -- "--text" <<<"$calls" \
      && fail "rewrote a prompt that was already complete: $calls"
    grep -q -- "terminal send --terminal term_agent --enter" <<<"$calls" \
      || fail "did not submit the drafted prompt: ${calls:-<nothing>} / $out"
    echo "PASS: a complete draft is submitted unchanged"
    ;;

  cli_broken)
    # `orca` on PATH is a wrapper that finds Orca.app by reading its own
    # symlink, and a macOS install has shipped that symlink readable only by
    # root. Every call then fails, no JSON parses, and the watcher concludes the
    # worktree has no linked issue and stops -- which is how a provisioned
    # worktree ends up with an agent sitting on an unsent prompt. The wrapper is
    # probed rather than trusted, so a second CLI that answers is used instead.
    make_fixture
    stub_orca "$TMPDIR_FIXTURE" >/dev/null
    write_terminals "$TMPDIR_FIXTURE"
    write_worktree issue
    echo '{"ok":true,"result":{"terminal":{"draft":"# 4: Capture real RomM auth shapes"}}}' \
      >"$TMPDIR_FIXTURE/read.json"

    # The working stub, moved aside under the name the resolver tries next.
    mv "$TMPDIR_FIXTURE/stub-bin/orca" "$TMPDIR_FIXTURE/stub-bin/orca-dev"
    printf '#!/usr/bin/env bash\necho "Unable to determine Orca.app path from symlink" >&2\nexit 1\n' \
      >"$TMPDIR_FIXTURE/stub-bin/orca"
    chmod +x "$TMPDIR_FIXTURE/stub-bin/orca"

    out="$(run_autostart --watch)"
    grep -q "nothing to start" <<<"$out" \
      && fail "a broken \`orca\` on PATH stopped the watcher outright; got: $out"
    grep -q -- "terminal send --terminal term_agent --enter" "$TMPDIR_FIXTURE/orca-calls.log" \
      || fail "never reached the agent past the broken CLI; got: $out"
    echo "PASS: a broken orca wrapper on PATH is skipped for one that answers"
    ;;

  *)
    echo "usage: $0 opens|reuses|foreign|no_romm|submits|no_draft|unstable" >&2
    echo "       watch_needs_issue|watch_late_draft|watch_grace|watch_submits|watch_single" >&2
    echo "       watch_bare_url|watch_full_draft_untouched|cli_broken" >&2
    exit 2
    ;;
esac
