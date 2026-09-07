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

# The head this worktree's fixture PR sits on, in every phase that describes one.
RS_HEAD=deadbeefdeadbeefdeadbeefdeadbeefdeadbeef

# A worktree holding what review-status.sh reads: the script, lib.sh, and the
# gate it defers to for who counts as a reviewer. `gh` answers from two files
# the phase writes, so a phase describes a pull request rather than a protocol.
make_review_status_fixture() {
  make_fixture
  mkdir -p "$TMPDIR_FIXTURE/.github/scripts" "$TMPDIR_FIXTURE/stub-bin"
  cp "$REPO_ROOT"/scripts/orca/{review-status.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
  cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
  cat >"$TMPDIR_FIXTURE/stub-bin/gh" <<'GHSTUB'
#!/usr/bin/env bash
case "$*" in
  *"repo view"*) printf 'armaatus/rommsync-nx\n' ;;
  *graphql*)     cat "$RS_FIXTURE/graphql.json" ;;
  *"pr view"*)   cat "$RS_FIXTURE/prview.json" ;;
  *"/files"*)    cat "$RS_FIXTURE/files.txt" ;;
  *)             printf '\n' ;;
esac
GHSTUB
  chmod +x "$TMPDIR_FIXTURE/stub-bin/gh"
  ( cd "$TMPDIR_FIXTURE" && git init -q . && git commit -q --allow-empty -m fixture ) 2>/dev/null
}

# The reviews/threads half of the answer. $1 is the reviews array, $2 the
# threads array, $3 the PR body.
write_pr_reviews() {
  cat >"$TMPDIR_FIXTURE/graphql.json" <<JSON
{"data":{"repository":{"pullRequest":{
  "body": $3,
  "author":{"login":"armaatus"},
  "reviews":{"nodes":[$1]},
  "reviewThreads":{"nodes":[$2]}
}}}}
JSON
}

# The checks/mergeability half. $1 is the statusCheckRollup array, $2 the
# mergeStateStatus, $3 the changed files one per line -- the same shape
# `gh api --paginate .../files --jq .[].filename` hands back in CI.
write_pr_checks() {
  cat >"$TMPDIR_FIXTURE/prview.json" <<JSON
{
  "headRefOid":"$RS_HEAD",
  "mergeStateStatus":"$2",
  "autoMergeRequest":{"enabledAt":"2026-09-07T00:00:00Z"},
  "statusCheckRollup":[$1]
}
JSON
  printf '%s\n' "$3" >"$TMPDIR_FIXTURE/files.txt"
}

run_review_status() {
  ( cd "$TMPDIR_FIXTURE" &&
    PATH="$TMPDIR_FIXTURE/stub-bin:$PATH" \
    RS_FIXTURE="$TMPDIR_FIXTURE" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" \
    bash "$TMPDIR_FIXTURE/scripts/orca/review-status.sh" 84 2>&1 )
}

# A worktree holding what await-review.sh reads. Like the review-status fixture
# it carries the gate as well as the script, because await-review.sh asks
# merge_gate.py who counts as a reviewer rather than deciding it again.
#
# The reviews come back through GraphQL, which is the only shape that can report
# the commit a review was submitted against and the PR's own author to compare a
# reviewer with. The `gh pr view` the stub still answers is the check rollup, not
# the reviews.
AW_PR=114
make_await_fixture() {
  make_fixture
  mkdir -p "$TMPDIR_FIXTURE/.github/scripts" "$TMPDIR_FIXTURE/stub-bin"
  cp "$REPO_ROOT"/scripts/orca/{await-review.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
  cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
  cat >"$TMPDIR_FIXTURE/stub-bin/gh" <<'GHSTUB'
#!/usr/bin/env bash
case "$*" in
  *"pr list"*)         printf '[{"number":114}]\n' ;;
  *"repo view"*)       printf 'armaatus/rommsync-nx\n' ;;
  # Everything green and no conflict: whatever ends this wait, it is the reviews.
  *statusCheckRollup*) printf '{"statusCheckRollup":[{"name":"host-tests","conclusion":"SUCCESS"}],"mergeStateStatus":"CLEAN","baseRefName":"main"}\n' ;;
  *graphql*)           cat "$AW_FIXTURE/graphql.json" ;;
  *"run list"*)        printf '\n' ;;
  *"/comments"*)       printf '[]\n' ;;
  *)                   printf '\n' ;;
esac
GHSTUB
  chmod +x "$TMPDIR_FIXTURE/stub-bin/gh"
  ( cd "$TMPDIR_FIXTURE" && git init -q . && git commit -q --allow-empty -m fixture ) 2>/dev/null
  # The head the fixture PR sits on -- the one `headRefOid` reports and the one
  # reviews are attributed to. Started equal to the worktree HEAD because that is
  # the ordinary case; a phase that wants them to DIFFER (something committed and
  # not pushed) reassigns it before describing the reviews.
  AW_PR_HEAD="$(cd "$TMPDIR_FIXTURE" && git rev-parse HEAD)"
}

# The reviews on the fixture PR, in $1, as GraphQL review nodes. The PR's own
# author is `armaatus`, as it is on every PR the fleet opens.
write_await_reviews() {
  cat >"$TMPDIR_FIXTURE/graphql.json" <<JSON
{"data":{"repository":{"pullRequest":{
  "headRefOid":"$AW_PR_HEAD",
  "author":{"login":"armaatus"},
  "reviews":{"nodes":[$1]}
}}}}
JSON
}

run_await_review() {
  ( cd "$TMPDIR_FIXTURE" &&
    PATH="$TMPDIR_FIXTURE/stub-bin:$PATH" \
    AW_FIXTURE="$TMPDIR_FIXTURE" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" \
    AWAIT_REVIEW_DEADLINE="${1:-6}" AWAIT_REVIEW_POLL=1 \
    bash "$TMPDIR_FIXTURE/scripts/orca/await-review.sh" "$AW_PR" 2>&1 )
}

# ---------------------------------------------------------------------------
# The fleet's queue parsing: `Closes #N` and `Blocked by #N`, read out of issue
# and PR bodies. A worktree holding fleet.sh, lib.sh and the shared reference
# patterns, plus a `gh` that answers from files instead of GitHub -- so a phase
# describes a backlog rather than a protocol.
make_fleet_parse_fixture() {
  TMPDIR_FIXTURE="$(mktemp -d)"
  TMPDIR_FIXTURE="$(cd "$TMPDIR_FIXTURE" && pwd -P)"
  mkdir -p "$TMPDIR_FIXTURE/scripts/orca" "$TMPDIR_FIXTURE/.github/scripts" \
           "$TMPDIR_FIXTURE/stub-bin"
  cp "$REPO_ROOT"/scripts/orca/{fleet.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
  cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
  FLEET_ISSUES="$TMPDIR_FIXTURE/issues.json"
  FLEET_OPEN_PRS="$TMPDIR_FIXTURE/open-prs.json"
  FLEET_MERGED_PRS="$TMPDIR_FIXTURE/merged-prs.json"
  FLEET_ISSUE_STATE=OPEN
  FLEET_GH_FAIL=""
  FLEET_LIVE=""
  printf '[]\n' >"$FLEET_ISSUES"
  printf '[]\n' >"$FLEET_OPEN_PRS"
  printf '[]\n' >"$FLEET_MERGED_PRS"
  cat >"$TMPDIR_FIXTURE/stub-bin/gh" <<'GHSTUB'
#!/usr/bin/env bash
# The four reads fleet.sh makes of GitHub, and nothing else.
[ -n "${FLEET_GH_FAIL:-}" ] && exit 1
case "$*" in
  *"issue list"*)               cat "$FLEET_ISSUES" ;;
  *"issue view"*)               printf '%s\n' "${FLEET_ISSUE_STATE:-OPEN}" ;;
  *"pr list"*"--state merged"*) cat "$FLEET_MERGED_PRS" ;;
  *"pr list"*)                  cat "$FLEET_OPEN_PRS" ;;
  *)                            printf '[]\n' ;;
esac
GHSTUB
  chmod +x "$TMPDIR_FIXTURE/stub-bin/gh"
}

# Run ONE of fleet.sh's queue functions against that fixture. The functions are
# lifted out rather than the dispatcher run, the same way
# fleet_notices_a_stalled_agent does it: what is under test is how a body is
# read, not the loop around it.
run_fleet_fn() {
  local fn="$1"; shift
  local src
  # The `ISSUE_REFS=` line comes out of fleet.sh too, rather than being written
  # here: it is how the snippets find issue_refs at all, and a wrong path in it
  # would otherwise stay green through every phase below.
  src="$(sed -n '/^ISSUE_REFS=/p; /^has_open_pr()/,/^}/p; /^in_flight()/,/^}/p;
                 /^ready_issues()/,/^}/p; /^issue_is_done()/,/^}/p;
                 /^count_startable()/,/^}/p' \
         "$TMPDIR_FIXTURE/scripts/orca/fleet.sh")"
  ( cd "$TMPDIR_FIXTURE" || exit 99
    export PATH="$TMPDIR_FIXTURE/stub-bin:$PATH"
    export FLEET_ISSUES FLEET_OPEN_PRS FLEET_MERGED_PRS FLEET_ISSUE_STATE
    export FLEET_GH_FAIL
    REPO_ROOT="$TMPDIR_FIXTURE"
    # count_startable reads the live worktree list from Orca, which is not what
    # these assert; $FLEET_LIVE stands in for it.
    live_worktrees() { printf '%s' "$FLEET_LIVE"; }
    eval "$src"
    "$fn" "$@" )
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

  await_reports_failed_review)
    # A review job that FAILED is not a review that is late. Waiting out the
    # 45-minute deadline for one and then saying "nothing arrived" is what
    # happened on PR #80, where the reviewer had already died on "Reached
    # maximum number of turns" four minutes in.
    make_fixture
    # The script under test goes INTO the fixture, like every other phase here.
    # await-review.sh derives its own repo root from its location, so running
    # the checkout's copy makes it cd to the real working tree -- and then
    # `orca_fleet_stopped` reads the developer's own ~/.rommsync-fleet/STOP. If
    # that file happens to exist the script exits 3 before it ever reaches the
    # branch under test, and the failure blames the feature rather than the
    # fixture. Found in review of this PR.
    cp "$REPO_ROOT"/scripts/orca/{await-review.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
    # The gate comes too: await-review.sh imports it to decide what counts as a
    # review, and a worktree without it is one where nothing can, which is its
    # own exit. A fixture missing it would test that instead of the branch named.
    mkdir -p "$TMPDIR_FIXTURE/.github/scripts"
    cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
    stub="$TMPDIR_FIXTURE/stub-bin"
    mkdir -p "$stub"
    cat >"$stub/gh" <<'GHSTUB'
#!/usr/bin/env bash
case "$*" in
  *"pr list"*)  printf '[{"number":80}]\n' ;;
  *"repo view"*) printf 'armaatus/rommsync-nx\n' ;;
  *"run list"*) printf '4242\n' ;;
  *"run view"*) printf '##[error] Execution failed: Reached maximum number of turns (30)\n' ;;
  # The rollup is what the script asks first: the review CHECK is dead, so it
  # goes looking for the run behind it. Nothing else here is failing, so the
  # red-build branch must not fire -- this has to exit 6, never 7.
  *statusCheckRollup*)
    printf '{"statusCheckRollup":[{"name":"review against REVIEW.md","conclusion":"FAILURE"},{"name":"host-tests","conclusion":"SUCCESS"}]}\n' ;;
  *"pr view"*)  printf '{"reviews":[]}\n' ;;
  *)            printf '\n' ;;
esac
GHSTUB
    chmod +x "$stub/gh"
    ( cd "$TMPDIR_FIXTURE" && git init -q . && git commit -q --allow-empty -m fixture ) 2>/dev/null
    out="$(cd "$TMPDIR_FIXTURE" &&
           PATH="$stub:$PATH" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" \
           AWAIT_REVIEW_DEADLINE=5 AWAIT_REVIEW_POLL=1 \
           bash "$TMPDIR_FIXTURE/scripts/orca/await-review.sh" 80 2>&1)"
    rc=$?
    grep -q "maximum number of turns" <<<"$out" \
      || fail "did not report why the review job failed; got: $out"
    [ "$rc" = 6 ] \
      || fail "expected exit 6 for a failed review job, got $rc: $out"
    echo "PASS: a failed review job is reported at once, not waited out"
    ;;

  await_finds_the_failed_run_under_newer_skipped_ones)
    # The phase above proves the report happens; this one proves it is still
    # reachable on a real PR. `claude review` fires on pull_request_review and
    # pull_request_review_comment as well as on the push, and those runs no-op
    # with `skipped`. So the FAILED run sinks: on this PR every single head had
    # a skipped review-event run as its newest `claude review` run, with the
    # real one four or five entries below. Asking for `--limit 1` therefore
    # returns a skipped run forever, the failure is never seen, and the wait
    # runs its full 45 minutes to report that nothing arrived -- which is #80,
    # the exact failure this check exists to end.
    #
    # The stub models GitHub's ordering rather than answering a constant: it
    # builds five skipped runs ahead of the failure, truncates to whatever
    # --limit the script asked for, and then applies the same select the real
    # --jq applies. A window too small to reach the failure yields nothing,
    # exactly as gh would.
    make_fixture
    cp "$REPO_ROOT"/scripts/orca/{await-review.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
    # The gate comes too: await-review.sh imports it to decide what counts as a
    # review, and a worktree without it is one where nothing can, which is its
    # own exit. A fixture missing it would test that instead of the branch named.
    mkdir -p "$TMPDIR_FIXTURE/.github/scripts"
    cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
    stub="$TMPDIR_FIXTURE/stub-bin"
    mkdir -p "$stub"
    cat >"$stub/gh" <<'GHSTUB'
#!/usr/bin/env bash
case "$*" in
  *"pr list"*)  printf '[{"number":80}]\n' ;;
  *"repo view"*) printf 'armaatus/rommsync-nx\n' ;;
  *"run list"*)
    limit=1
    for a in "$@"; do
      [ "$prev" = "--limit" ] && limit="$a"
      prev="$a"
    done
    echo "$limit" >>"$LIMIT_LOG"
    # Five skipped runs sit ahead of the failure, as they do on a real PR.
    if [ "$limit" -gt 5 ]; then printf '4242\n'; else printf '\n'; fi ;;
  *"run view"*) printf '##[error] Execution failed: Reached maximum number of turns (30)\n' ;;
  *statusCheckRollup*)
    printf '{"statusCheckRollup":[{"name":"review against REVIEW.md","conclusion":"FAILURE"},{"name":"host-tests","conclusion":"SUCCESS"}]}\n' ;;
  *"pr view"*)  printf '{"reviews":[]}\n' ;;
  *)            printf '\n' ;;
esac
GHSTUB
    chmod +x "$stub/gh"
    ( cd "$TMPDIR_FIXTURE" && git init -q . && git commit -q --allow-empty -m fixture ) 2>/dev/null
    limitlog="$TMPDIR_FIXTURE/limits.log"; : >"$limitlog"
    out="$(cd "$TMPDIR_FIXTURE" &&
           PATH="$stub:$PATH" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" \
           LIMIT_LOG="$limitlog" \
           AWAIT_REVIEW_DEADLINE=5 AWAIT_REVIEW_POLL=1 \
           bash "$TMPDIR_FIXTURE/scripts/orca/await-review.sh" 80 2>&1)"
    rc=$?
    [ -s "$limitlog" ] || fail "never asked for the run list at all; the fixture proves nothing"
    [ "$rc" = 6 ] \
      || fail "a failed review run buried under newer skipped runs was never found (exit $rc, asked for --limit $(head -1 "$limitlog")); the wait would run its full deadline and blame no one: $out"
    grep -q "maximum number of turns" <<<"$out" \
      || fail "found the run but did not say why it failed; got: $out"
    echo "PASS: the failed run is found even under newer skipped runs"
    ;;

  await_throttles_the_lookup_when_the_run_is_never_found)
    # The third way this same call has now been made expensive, and the subtlest.
    # The review check is dead, so `review_dead` is correctly true -- but the run
    # behind it is never found: a transient error swallowed by `2>/dev/null`, or
    # a failure further back than the window. The loop therefore does NOT exit 6,
    # and `review_dead` stays true until the next throttled recheck.
    #
    # Gated on `review_dead` alone, the lookup then re-fires on every poll for as
    # long as that lasts -- the per-poll cost the throttle exists to prevent,
    # moved one call downstream of the fix rather than removed. Gated on the
    # throttle too, an unfound run costs one call per cycle.
    #
    # The other two await phases cannot see this: one exits 6 on the first try
    # so the loop never repeats, and the recovery phase only counts calls made
    # AFTER the rollup reports recovery, not the ones made while review_dead is
    # correctly still true.
    make_fixture
    cp "$REPO_ROOT"/scripts/orca/{await-review.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
    # The gate comes too: await-review.sh imports it to decide what counts as a
    # review, and a worktree without it is one where nothing can, which is its
    # own exit. A fixture missing it would test that instead of the branch named.
    mkdir -p "$TMPDIR_FIXTURE/.github/scripts"
    cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
    stub="$TMPDIR_FIXTURE/stub-bin"
    mkdir -p "$stub"
    cat >"$stub/gh" <<'GHSTUB'
#!/usr/bin/env bash
case "$*" in
  *"pr list"*)  printf '[{"number":80}]\n' ;;
  *"repo view"*) printf 'armaatus/rommsync-nx\n' ;;
  # The review check stays dead all the way through, so review_dead is true on
  # every poll after the first throttled check.
  *statusCheckRollup*)
    printf '{"statusCheckRollup":[{"name":"review against REVIEW.md","conclusion":"FAILURE"},{"name":"host-tests","conclusion":"SUCCESS"}]}\n' ;;
  # ...and the run behind it is never found, so the loop never exits 6.
  *"run list"*) echo x >>"$RUNLIST_LOG"; printf '\n' ;;
  *"pr view"*)  printf '{"reviews":[]}\n' ;;
  *)            printf '\n' ;;
esac
GHSTUB
    chmod +x "$stub/gh"
    ( cd "$TMPDIR_FIXTURE" && git init -q . && git commit -q --allow-empty -m fixture ) 2>/dev/null
    runlog="$TMPDIR_FIXTURE/runlist.log"; : >"$runlog"
    # 12 polls at one second: three throttled checks (polls 1, 5, 9), so a
    # correctly throttled lookup spends three calls and a per-poll one spends
    # about twelve.
    ( cd "$TMPDIR_FIXTURE" &&
      PATH="$stub:$PATH" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" RUNLIST_LOG="$runlog" \
      AWAIT_REVIEW_DEADLINE=12 AWAIT_REVIEW_POLL=1 \
      bash "$TMPDIR_FIXTURE/scripts/orca/await-review.sh" 80 >/dev/null 2>&1 )
    calls="$(wc -l <"$runlog" | tr -d ' ')"
    [ "${calls:-0}" -ge 1 ] \
      || fail "never looked for the failed run at all; the fixture proves nothing"
    [ "${calls:-0}" -le 4 ] \
      || fail "spent $calls run-list call(s) over 12 polls while the run stayed unfound; the lookup is running per poll, not per throttle cycle"
    echo "PASS: an unfound run costs one lookup per throttle cycle, not one per poll"
    ;;

  await_reports_a_red_build)
    # #88 sat in await-review.sh while host-tests failed on its own new test.
    # A review cannot fix a red build, and waiting for one costs 45 minutes and
    # then reports the wrong thing.
    make_fixture
    cp "$REPO_ROOT"/scripts/orca/{await-review.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
    # The gate comes too: await-review.sh imports it to decide what counts as a
    # review, and a worktree without it is one where nothing can, which is its
    # own exit. A fixture missing it would test that instead of the branch named.
    mkdir -p "$TMPDIR_FIXTURE/.github/scripts"
    cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
    stub="$TMPDIR_FIXTURE/stub-bin"; mkdir -p "$stub"
    cat >"$stub/gh" <<'GHSTUB'
#!/usr/bin/env bash
case "$*" in
  *"pr list"*)              printf '[{"number":88}]\n' ;;
  *"repo view"*)            printf 'armaatus/rommsync-nx\n' ;;
  *statusCheckRollup*)      printf '{"statusCheckRollup":[{"name":"host-tests","conclusion":"FAILURE"},{"name":"merge-gate","conclusion":"FAILURE"}]}\n' ;;
  *"pr view"*)              printf '{"reviews":[]}\n' ;;
  *"run list"*)             printf '\n' ;;
  *)                        printf '\n' ;;
esac
GHSTUB
    chmod +x "$stub/gh"
    ( cd "$TMPDIR_FIXTURE" && git init -q . && git commit -q --allow-empty -m fixture ) 2>/dev/null
    out="$(cd "$TMPDIR_FIXTURE" &&
           PATH="$stub:$PATH" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" \
           AWAIT_REVIEW_DEADLINE=12 AWAIT_REVIEW_POLL=1 \
           bash "$TMPDIR_FIXTURE/scripts/orca/await-review.sh" 88 2>&1)"
    rc=$?
    grep -q "host-tests" <<<"$out" || fail "did not name the failing check: $out"
    grep -q "two consecutive checks" <<<"$out" \
      || fail "acted on a single sighting; a known flake would send an agent chasing it: $out"
    grep -q "#76" <<<"$out" \
      || fail "did not point at the known flake as the first thing to rule out: $out"
    grep -q "merge-gate" <<<"$out" \
      && fail "reported merge-gate, which is red by design until a review exists: $out"
    [ "$rc" = 7 ] || fail "expected exit 7 for a red build, got $rc: $out"
    echo "PASS: a red build is reported at once, and merge-gate is not mistaken for one"
    ;;

  await_reports_a_conflicted_pr)
    # #99 sat in await-review.sh for the full 45 minutes while the PR was DIRTY:
    # something had merged underneath it, and no review can fix a merge conflict.
    # It then got its review, resolved its threads, and still could not merge,
    # because the conflict was the blocker the whole time.
    #
    # Same shape as the red build above, and reported on the FIRST throttled
    # check rather than on two consecutive ones: a conflict is not a flake, and
    # the branch cannot merge until it is rebased whatever else is true. So the
    # stub keeps every check green -- if this exits, it is mergeStateStatus that
    # made it exit.
    make_fixture
    cp "$REPO_ROOT"/scripts/orca/{await-review.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
    # The gate comes too: await-review.sh imports it to decide what counts as a
    # review, and a worktree without it is one where nothing can, which is its
    # own exit. A fixture missing it would test that instead of the branch named.
    mkdir -p "$TMPDIR_FIXTURE/.github/scripts"
    cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
    stub="$TMPDIR_FIXTURE/stub-bin"; mkdir -p "$stub"
    cat >"$stub/gh" <<'GHSTUB'
#!/usr/bin/env bash
case "$*" in
  *"pr list"*)         printf '[{"number":99}]\n' ;;
  *"repo view"*)       printf 'armaatus/rommsync-nx\n' ;;
  *statusCheckRollup*)
    echo x >>"$ROLLUP_LOG"
    printf '{"statusCheckRollup":[{"name":"host-tests","conclusion":"SUCCESS"}],"mergeStateStatus":"DIRTY","baseRefName":"release/v1"}\n' ;;
  *"pr view"*)         printf '{"reviews":[]}\n' ;;
  *"run list"*)        printf '\n' ;;
  *)                   printf '\n' ;;
esac
GHSTUB
    chmod +x "$stub/gh"
    ( cd "$TMPDIR_FIXTURE" && git init -q . && git commit -q --allow-empty -m fixture ) 2>/dev/null
    rolluplog="$TMPDIR_FIXTURE/rollup.log"; : >"$rolluplog"
    out="$(cd "$TMPDIR_FIXTURE" &&
           PATH="$stub:$PATH" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" \
           ROLLUP_LOG="$rolluplog" \
           AWAIT_REVIEW_DEADLINE=12 AWAIT_REVIEW_POLL=1 \
           bash "$TMPDIR_FIXTURE/scripts/orca/await-review.sh" 99 2>&1)"
    rc=$?
    [ "$rc" = 8 ] \
      || fail "expected exit 8 for a conflicted PR, got $rc; the wait would run its full deadline and then ask for a review that cannot help: $out"
    # One rollup fetched, so it went on the FIRST throttled check. Counting the
    # stub's calls rather than the wall clock: the property is "one sighting is
    # enough", not "it was quick", and a loaded CI box makes a timing assert
    # flake on a script that behaved correctly.
    [ "$(wc -l <"$rolluplog" | tr -d ' ')" = 1 ] \
      || fail "fetched the rollup $(wc -l <"$rolluplog" | tr -d ' ') time(s) before reporting a conflict; it must go on the first sighting, not on two consecutive ones: $out"
    grep -q "DIRTY" <<<"$out" \
      || fail "did not name the state GitHub reports, so the agent cannot match it to review-status.sh: $out"
    grep -q -- "--force-with-lease" <<<"$out" \
      || fail "told the agent to rebase without saying how to push the rewritten branch: $out"
    grep -q "record-review.sh" <<<"$out" \
      || fail "a rebase changes every sha and the review marker is per-commit; the push will be refused without a new one: $out"
    # The base is the PR's own, not a hardcoded origin/main -- which is right for
    # every PR the fleet opens today and wrong the first time one is stacked.
    grep -q "origin/release/v1" <<<"$out" \
      || fail "printed a rebase against a base this PR does not have; the command it hands the agent has to be runnable: $out"

    # Exit 8 fires on poll 1, so neither the deadline nor the round cap bounds
    # it, and an agent whose force-push never ran would bounce here forever
    # getting the same four instructions. A second visit on the SAME commit is
    # proof nothing changed -- this reads GitHub's view, not the working tree --
    # and it has to say so.
    : >"$rolluplog"
    out2="$(cd "$TMPDIR_FIXTURE" &&
            PATH="$stub:$PATH" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" \
            ROLLUP_LOG="$rolluplog" \
            AWAIT_REVIEW_DEADLINE=12 AWAIT_REVIEW_POLL=1 \
            bash "$TMPDIR_FIXTURE/scripts/orca/await-review.sh" 99 2>&1)"
    [ "$?" = 8 ] || fail "the second visit did not still exit 8: $out2"
    grep -q "SECOND time" <<<"$out2" \
      || fail "came back on the same commit and repeated itself; nothing bounds this path, so an unpushed rebase loops forever: $out2"
    grep -q "SECOND time" <<<"$out" \
      && fail "called the first visit a repeat; the marker is not being keyed on the head at all: $out"

    echo "PASS: a conflicted PR is reported at once, with the rebase it needs"
    ;;

  fleet_notices_a_stalled_agent)
    # The other half of this PR, which shipped untested and should not have.
    # An agent in `waiting` is at a prompt, not working -- the board still reads
    # `in-progress` and the time-box has hours to run. The fleet has to say so,
    # once, rather than once per poll.
    make_fixture
    cp "$REPO_ROOT"/scripts/orca/{fleet.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
    stub="$TMPDIR_FIXTURE/stub-bin"; mkdir -p "$stub"
    state="$TMPDIR_FIXTURE/fleet"; mkdir -p "$state/worktrees"
    printf '%s\n' "$TMPDIR_FIXTURE" >"$state/worktrees/29"
    cat >"$stub/orca" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"$TMPDIR_FIXTURE/orca-calls.log"
case "\$1" in
  --version) exit 0 ;;
  worktree)
    printf '{"ok":true,"result":{"worktrees":[{"path":"$TMPDIR_FIXTURE","linkedIssue":29,"agents":[{"state":"waiting"}]}]}}\n' ;;
  *) printf '{"ok":true,"result":{}}\n' ;;
esac
STUB
    chmod +x "$stub/orca"
    : >"$TMPDIR_FIXTURE/orca-calls.log"
    out="$(PATH="$stub:$PATH" ROMMSYNC_FLEET_DIR="$state" \
           bash -c '. '"$TMPDIR_FIXTURE"'/scripts/orca/lib.sh
                    orca_cli_resolve
                    STATE_DIR="'"$state"'"; OWNED_DIR="$STATE_DIR/worktrees"
                    say() { echo "$*"; }
                    notify() { echo "NOTIFY: $*"; }
                    orca_json() { local o; o="$(mktemp)"; orca_run_with_deadline 10 "$o" "$ORCA_CLI" "$@" --json; cat "$o"; rm -f "$o"; }
                    card() { echo "CARD: $*"; }
                    '"$(sed -n '/^notice_stalled()/,/^}/p' "$TMPDIR_FIXTURE/scripts/orca/fleet.sh")"'
                    notice_stalled
                    echo "--- second pass ---"
                    notice_stalled' 2>&1)"
    grep -q "waiting for input" <<<"$out" \
      || fail "the fleet did not notice an agent sitting at a prompt: $out"
    grep -q "NOTIFY:" <<<"$out" || fail "no notification for a stalled agent: $out"
    grep -q "CARD:" <<<"$out" || fail "the card was not updated for a stalled agent: $out"
    # Count the LOG line, not the phrase: the card comment repeats it, so
    # matching "waiting for input" counts two for a single notice.
    [ "$(grep -c "in auto mode nothing should be asking" <<<"$out")" -eq 1 ] \
      || fail "said it more than once; it must be once per stall, not per poll: $out"
    echo "PASS: a stalled agent is reported once, on the card and in a notification"
    ;;

  review_status_shows_the_open_thread)
    # The brief used to list feedback with the REST comments endpoint, which
    # cannot say whether a thread is resolved. On round two that returns every
    # comment ever left -- the fixed ones mixed in with the live one -- and a
    # thread lost in that noise is exactly what left #88 and #89 blocked.
    make_review_status_fixture
    write_pr_reviews \
      '{"state":"COMMENTED","submittedAt":"2026-09-06T00:00:00Z",
        "commit":{"oid":"'"$RS_HEAD"'"},"author":{"login":"claude"},
        "body":"A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY.",
        "comments":{"totalCount":0}}' \
      '{"id":"T_settled","isResolved":true,"isOutdated":false,
        "path":"core/src/old.cpp","line":10,
        "comments":{"nodes":[{"author":{"login":"claude"},"body":"ALREADY FIXED LAST ROUND"}]}},
       {"id":"T_live","isResolved":false,"isOutdated":true,
        "path":"core/src/sync.cpp","line":288,
        "comments":{"nodes":[{"author":{"login":"claude"},"body":"THE ONE STILL OPEN"}]}}' \
      '"## Plan\nCloses #84\n/code-review high\nmattpocock-skills:code-review\n"'
    write_pr_checks \
      '{"name":"host-tests","status":"COMPLETED","conclusion":"SUCCESS",
        "startedAt":"2026-09-06T09:00:00Z","completedAt":"2026-09-06T09:30:00Z"}' \
      BLOCKED core/src/sync.cpp
    out="$(run_review_status)"; rc=$?
    [ "$rc" = 1 ] || fail "expected exit 1 (an unresolved thread is not ready), got $rc: $out"
    grep -q "THE ONE STILL OPEN" <<<"$out" \
      || fail "did not print the unresolved thread; got: $out"
    grep -q "ALREADY FIXED LAST ROUND" <<<"$out" \
      && fail "printed a RESOLVED thread -- that is the REST behaviour this replaces: $out"
    grep -q "T_live" <<<"$out" \
      || fail "did not print the thread ID resolveReviewThread needs: $out"
    grep -q "core/src/sync.cpp:288" <<<"$out" \
      || fail "did not say where the thread is: $out"
    # An outdated thread still blocks the merge, so it must still be listed.
    grep -q "outdated -- still open" <<<"$out" \
      || fail "an outdated-but-unresolved thread must be shown as still open: $out"
    echo "PASS: only unresolved threads are listed, with the ID needed to resolve them"
    ;;

  review_status_ignores_superseded_checks)
    # #84: merge-gate runs several times on one head by design -- it re-evaluates
    # when a review lands -- and every run before the review is an honest
    # failure. GitHub resolves a required check from the NEWEST run of that name;
    # `statusCheckRollup` hands back all of them un-deduplicated. Judging the raw
    # list reports "check failed: merge-gate" on a PR GitHub calls CLEAN, and the
    # fleet loop is told to keep going until this exits 0 -- so it loops forever
    # against a green PR. Seen on #100 and #108.
    make_review_status_fixture
    write_pr_reviews \
      '{"state":"COMMENTED","submittedAt":"2026-09-06T10:00:00Z",
        "commit":{"oid":"'"$RS_HEAD"'"},"author":{"login":"claude"},
        "body":"A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY.",
        "comments":{"totalCount":0}}' \
      '' '"## Plan\nCloses #84\n/code-review high\nmattpocock-skills:code-review\n"'
    write_pr_checks \
      '{"name":"merge-gate","status":"COMPLETED","conclusion":"FAILURE",
        "startedAt":"2026-09-06T09:00:00Z","completedAt":"2026-09-06T09:00:10Z",
        "detailsUrl":"https://github.com/armaatus/rommsync-nx/actions/runs/1/job/11"},
       {"name":"merge-gate","status":"COMPLETED","conclusion":"SUCCESS",
        "startedAt":"2026-09-06T11:00:00Z","completedAt":"2026-09-06T11:00:10Z",
        "detailsUrl":"https://github.com/armaatus/rommsync-nx/actions/runs/2/job/22"},
       {"name":"host-tests","status":"COMPLETED","conclusion":"SUCCESS",
        "startedAt":"2026-09-06T09:00:00Z","completedAt":"2026-09-06T09:30:00Z"}' \
      CLEAN core/src/sync.cpp
    out="$(run_review_status)"; rc=$?
    [ "$rc" = 0 ] || fail "a superseded merge-gate failure must not count -- GitHub reads the newest run of a name; got $rc: $out"
    grep -q "check failed" <<<"$out" \
      && fail "reported a check that its own newer run superseded: $out"
    echo "PASS: only the newest run of each check name is judged"
    ;;

  review_status_mirrors_the_gate_on_who_reviewed)
    # The local answer and the required check must not be able to disagree: a
    # review-status that says "ready" on a PR merge-gate refuses is how a PR
    # sits quietly BLOCKED, which is the failure mode #84 is about. The two
    # rules the gate has and prose alone would drift on: a review by the PR's
    # own author is not independent, and an empty review record is not a review
    # (PR #95 merged on one whose whole body was the word "test").
    make_review_status_fixture
    write_pr_reviews \
      '{"state":"COMMENTED","submittedAt":"2026-09-06T10:00:00Z",
        "commit":{"oid":"'"$RS_HEAD"'"},"author":{"login":"armaatus"},
        "body":"The author reviewing itself, at length, which is not independence.",
        "comments":{"totalCount":0}},
       {"state":"COMMENTED","submittedAt":"2026-09-06T10:05:00Z",
        "commit":{"oid":"'"$RS_HEAD"'"},"author":{"login":"claude"},
        "body":"test","comments":{"totalCount":0}}' \
      '' '"## Plan\nCloses #84\n/code-review high\nmattpocock-skills:code-review\n"'
    write_pr_checks \
      '{"name":"host-tests","status":"COMPLETED","conclusion":"SUCCESS",
        "startedAt":"2026-09-06T09:00:00Z","completedAt":"2026-09-06T09:30:00Z"}' \
      BLOCKED core/src/sync.cpp
    out="$(run_review_status)"; rc=$?
    [ "$rc" = 1 ] || fail "a self-review plus an empty record is not a reviewed PR; got $rc: $out"
    grep -qi "empty" <<<"$out" \
      || fail "did not say the only independent review is empty, which is what merge-gate will say: $out"
    echo "PASS: review-status answers who reviewed with merge_gate.py's own rule"
    ;;

  review_status_says_a_human_merges_this_one)
    # merge-gate refuses the enforcement layer on purpose, so its FAILURE on such
    # a PR is the gate working. Reported as a plain "check failed" it reads as
    # something to fix, and the agent spends its three review rounds trying to
    # turn green a gate that never will (#96). Distinct exit code, so the brief's
    # loop can stop rather than lap.
    make_review_status_fixture
    write_pr_reviews \
      '{"state":"COMMENTED","submittedAt":"2026-09-06T10:00:00Z",
        "commit":{"oid":"'"$RS_HEAD"'"},"author":{"login":"claude"},
        "body":"A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY.",
        "comments":{"totalCount":0}}' \
      '' '"## Plan\nCloses #84\n/code-review high\nmattpocock-skills:code-review\n"'
    write_pr_checks \
      '{"name":"merge-gate","status":"COMPLETED","conclusion":"FAILURE",
        "startedAt":"2026-09-06T11:00:00Z","completedAt":"2026-09-06T11:00:10Z",
        "detailsUrl":"https://github.com/armaatus/rommsync-nx/actions/runs/2/job/22"},
       {"name":"host-tests","status":"COMPLETED","conclusion":"SUCCESS",
        "startedAt":"2026-09-06T09:00:00Z","completedAt":"2026-09-06T09:30:00Z"}' \
      BLOCKED '.github/workflows/merge-gate.yml
core/src/sync.cpp'
    out="$(run_review_status)"; rc=$?
    [ "$rc" = 4 ] || fail "a PR touching the enforcement layer needs its own answer, not 'not ready'; got $rc: $out"
    grep -qi "human" <<<"$out" \
      || fail "did not say a person merges this one: $out"
    grep -q ".github/workflows/merge-gate.yml" <<<"$out" \
      || fail "did not name the path that makes it human-merge-only: $out"
    # And before the gate has concluded, which is where an agent asks first. A
    # gate still running on such a PR is a gate that is going to fail, so
    # reporting it as "still running" makes exit 4 unreachable on the first ask
    # and sends the agent round again for an answer that cannot change.
    write_pr_checks \
      '{"name":"merge-gate","status":"IN_PROGRESS","conclusion":null,
        "startedAt":"2026-09-06T11:00:00Z","completedAt":null,
        "detailsUrl":"https://github.com/armaatus/rommsync-nx/actions/runs/2/job/22"},
       {"name":"host-tests","status":"COMPLETED","conclusion":"SUCCESS",
        "startedAt":"2026-09-06T09:00:00Z","completedAt":"2026-09-06T09:30:00Z"}' \
      BLOCKED '.github/workflows/merge-gate.yml
core/src/sync.cpp'
    out="$(run_review_status)"; rc=$?
    [ "$rc" = 4 ] || fail "the gate had not concluded yet, and its answer on this PR is not in doubt; got $rc: $out"
    grep -q "check still running: merge-gate" <<<"$out" \
      && fail "waited on a gate whose verdict on this PR is already decided: $out"
    echo "PASS: an enforcement-layer PR is reported as human-merge, not as a failure to fix"
    ;;

  review_status_names_the_wedge)
    # The #84 wedge itself: every latest check green, every thread resolved, and
    # GitHub still says BLOCKED, because branch protection is still counting a
    # stale run. Whatever else happens, this must not be silent -- the script
    # says which run is holding it and the command that clears it, rather than
    # printing "ready" at an agent whose PR will never merge.
    make_review_status_fixture
    write_pr_reviews \
      '{"state":"COMMENTED","submittedAt":"2026-09-06T10:00:00Z",
        "commit":{"oid":"'"$RS_HEAD"'"},"author":{"login":"claude"},
        "body":"A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY.",
        "comments":{"totalCount":0}}' \
      '' '"## Plan\nCloses #84\n/code-review high\nmattpocock-skills:code-review\n"'
    write_pr_checks \
      '{"name":"merge-gate","status":"COMPLETED","conclusion":"FAILURE",
        "startedAt":"2026-09-06T09:00:00Z","completedAt":"2026-09-06T09:00:10Z",
        "detailsUrl":"https://github.com/armaatus/rommsync-nx/actions/runs/34061892166/job/11"},
       {"name":"merge-gate","status":"COMPLETED","conclusion":"SUCCESS",
        "startedAt":"2026-09-06T11:00:00Z","completedAt":"2026-09-06T11:00:10Z",
        "detailsUrl":"https://github.com/armaatus/rommsync-nx/actions/runs/2/job/22"}' \
      BLOCKED core/src/sync.cpp
    out="$(run_review_status)"; rc=$?
    [ "$rc" = 1 ] || fail "GitHub says BLOCKED, so this PR is not ready however green it looks; got $rc: $out"
    grep -q "BLOCKED" <<<"$out" \
      || fail "did not report GitHub's own verdict on the PR: $out"
    grep -q "34061892166" <<<"$out" \
      || fail "did not name the stale run still being counted: $out"
    grep -q "gh run rerun" <<<"$out" \
      || fail "did not print the command that clears it: $out"
    echo "PASS: a PR wedged on a stale check run says so, and says what clears it"
    ;;

  review_status_waits_for_the_run_in_flight)
    # The gate job is `cancel-in-progress`, so the ordinary sequence is: a run
    # starts, the review lands, a second run starts and CANCELS the first. The
    # cancelled run therefore FINISHES a few seconds after the live one STARTED.
    # Ordering by completion time then picks the cancelled run as the newest of
    # its name and reports "check failed" while the real gate is still running --
    # the misreport this whole change exists to remove, reintroduced through the
    # clock. Newest is by start, and a run with no conclusion yet is in flight.
    make_review_status_fixture
    write_pr_reviews \
      '{"state":"COMMENTED","submittedAt":"2026-09-06T10:00:00Z",
        "commit":{"oid":"'"$RS_HEAD"'"},"author":{"login":"claude"},
        "body":"A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY.",
        "comments":{"totalCount":0}}' \
      '' '"## Plan\nCloses #84\n/code-review high\nmattpocock-skills:code-review\n"'
    write_pr_checks \
      '{"name":"merge-gate","status":"COMPLETED","conclusion":"CANCELLED",
        "startedAt":"2026-09-06T09:00:00Z","completedAt":"2026-09-06T09:10:05Z",
        "detailsUrl":"https://github.com/armaatus/rommsync-nx/actions/runs/1/job/11"},
       {"name":"merge-gate","status":"IN_PROGRESS","conclusion":null,
        "startedAt":"2026-09-06T09:10:00Z","completedAt":null,
        "detailsUrl":"https://github.com/armaatus/rommsync-nx/actions/runs/2/job/22"}' \
      BLOCKED core/src/sync.cpp
    out="$(run_review_status)"; rc=$?
    [ "$rc" = 1 ] || fail "a gate still in flight is not ready yet; got $rc: $out"
    grep -q "check still running: merge-gate" <<<"$out" \
      || fail "did not report the run in flight as running: $out"
    grep -q "check failed" <<<"$out" \
      && fail "reported the run the live one cancelled as the current answer: $out"
    echo "PASS: the newest run of a name is the one that started last, not the one that ended last"
    ;;

  review_status_reports_a_conflict_before_the_human_merge)
    # Exit 4 tells the agent to stop, so it must not be reachable while something
    # is still wrong. A conflict with the base blocks EVERY merge, a person's
    # included -- announcing "nothing here is left to fix" on one leaves the PR
    # parked with the one thing that had to be said unsaid.
    make_review_status_fixture
    write_pr_reviews \
      '{"state":"COMMENTED","submittedAt":"2026-09-06T10:00:00Z",
        "commit":{"oid":"'"$RS_HEAD"'"},"author":{"login":"claude"},
        "body":"A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY.",
        "comments":{"totalCount":0}}' \
      '' '"## Plan\nCloses #84\n/code-review high\nmattpocock-skills:code-review\n"'
    write_pr_checks \
      '{"name":"host-tests","status":"COMPLETED","conclusion":"SUCCESS",
        "startedAt":"2026-09-06T09:00:00Z","completedAt":"2026-09-06T09:30:00Z"}' \
      DIRTY '.claude/hooks/guard.py'
    out="$(run_review_status)"; rc=$?
    [ "$rc" = 1 ] || fail "a conflicted PR is not finished, whoever merges it; got $rc: $out"
    grep -qi "conflict" <<<"$out" \
      || fail "did not say the branch conflicts with its base: $out"
    echo "PASS: a conflict is reported even on a PR only a human can merge"
    ;;

  brief_never_lists_threads_over_rest)
    # The brief IS the fleet's operating instruction; a wrong command in it is a
    # defect in the same sense a wrong line of C++ is. This pins the endpoint it
    # must not go back to.
    brief="$REPO_ROOT/scripts/orca/issue-command.sh"
    grep -q "review-status.sh" "$brief" \
      || fail "step 5 no longer points at review-status.sh"
    # The REST endpoint may be NAMED (the brief warns against it) but never as
    # the command to run: the warning line is the only place it may appear.
    while IFS= read -r line; do
      case "$line" in
        *"pulls/<n>/comments"*)
          case "$line" in
            *"Do NOT"*|*"cannot"*) ;;
            *) fail "the brief still tells the agent to run: $line" ;;
          esac ;;
      esac
    done <"$brief"
    echo "PASS: the brief lists threads through review-status.sh, not the REST comments list"
    ;;

  brief_queues_the_merge_at_step_four)
    # #90 went green with nothing queued to merge it: GitHub refuses auto-merge
    # on an ALREADY-mergeable PR ("Pull request is in clean status"), and the
    # guard forbids merging by hand, so the PR sat clean and untouched. The brief
    # is the fleet's operating instruction -- a forward reference ("see step 6")
    # is not an instruction to run anything, and an agent reading in order would
    # queue it late and reproduce the bug. The command has to BE in step 4.
    brief="$REPO_ROOT/scripts/orca/issue-command.sh"
    step4="$(awk '/^\*\*4\. /{on=1} /^\*\*5\. /{on=0} on' "$brief")"
    [ -n "$step4" ] || fail "could not find step 4 in the brief"
    grep -q -- "--auto" <<<"$step4" \
      || fail "step 4 does not carry the auto-merge command itself:
$step4"
    grep -q "gh pr merge" <<<"$step4" \
      || fail "step 4 never names gh pr merge:
$step4"
    echo "PASS: the brief queues auto-merge in step 4, not by forward reference"
    ;;

  await_costs_nothing_while_the_review_is_healthy)
    # The failed-review check used to spend a `gh run list` on EVERY poll -- up
    # to 90 extra calls per waiting worktree over a 45-minute wait, times three
    # worktrees, against the same secondary rate limit the red-build check is
    # throttled to respect. Worse, a rate-limited answer is indistinguishable
    # from "nothing failed", which is how #80 defeated the previous check. It now
    # reads the rollup it already fetched, and asks about runs only once that
    # rollup says the review check is dead.
    make_fixture
    cp "$REPO_ROOT"/scripts/orca/{await-review.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
    # The gate comes too: await-review.sh imports it to decide what counts as a
    # review, and a worktree without it is one where nothing can, which is its
    # own exit. A fixture missing it would test that instead of the branch named.
    mkdir -p "$TMPDIR_FIXTURE/.github/scripts"
    cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
    stub="$TMPDIR_FIXTURE/stub-bin"
    mkdir -p "$stub"
    cat >"$stub/gh" <<'GHSTUB'
#!/usr/bin/env bash
# Everything green and no review yet: the healthy wait.
case "$*" in
  *"pr list"*)          printf '[{"number":80}]
' ;;
  *"repo view"*)        printf 'armaatus/rommsync-nx
' ;;
  *"run list"*)         echo "$*" >>"$RUNLIST_LOG"; printf '
' ;;
  *statusCheckRollup*)  printf '{"statusCheckRollup":[{"name":"host-tests","conclusion":"SUCCESS"}]}
' ;;
  *"pr view"*)          printf '{"reviews":[]}
' ;;
  *)                    printf '
' ;;
esac
GHSTUB
    chmod +x "$stub/gh"
    ( cd "$TMPDIR_FIXTURE" && git init -q . && git commit -q --allow-empty -m fixture ) 2>/dev/null
    runlog="$TMPDIR_FIXTURE/runlist.log"; : >"$runlog"
    ( cd "$TMPDIR_FIXTURE" &&
      PATH="$stub:$PATH" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" RUNLIST_LOG="$runlog" \
      AWAIT_REVIEW_DEADLINE=6 AWAIT_REVIEW_POLL=1 \
      bash "$TMPDIR_FIXTURE/scripts/orca/await-review.sh" 80 >/dev/null 2>&1 )
    calls="$(grep -c "claude review" "$runlog" || true)"
    [ "${calls:-0}" = 0 ] \
      || fail "spent $calls run-list call(s) while the review check was green; the rollup already said nothing failed"
    echo "PASS: no run-list calls while the review check is healthy"
    ;;

  await_stops_paying_once_the_review_recovers)
    # The other half of the throttle: not "never dead" but dead and then ALIVE
    # again. A review job that failed can be re-run, and when it comes back the
    # `gh run list` behind it has to stop -- otherwise a single dead check early
    # in the wait buys an unthrottled call on every poll for the remaining 45
    # minutes, which is the cost the throttle exists to remove.
    #
    # This is the transition the other four await tests do not cross: they cover
    # dead-from-the-first-check, never-dead, and the red-build path. In each of
    # those the value carried between checks happens to equal the correct one,
    # so a `review_dead` that never cleared would pass all of them.
    #
    # The stub answers FAILURE on the first rollup and SUCCESS on every one
    # after it, and stamps each `gh run list` with the number of rollups that
    # preceded it. Any call stamped 2 or higher is one made after the rollup
    # said the review had recovered.
    make_fixture
    cp "$REPO_ROOT"/scripts/orca/{await-review.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
    # The gate comes too: await-review.sh imports it to decide what counts as a
    # review, and a worktree without it is one where nothing can, which is its
    # own exit. A fixture missing it would test that instead of the branch named.
    mkdir -p "$TMPDIR_FIXTURE/.github/scripts"
    cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
    stub="$TMPDIR_FIXTURE/stub-bin"
    mkdir -p "$stub"
    cat >"$stub/gh" <<'GHSTUB'
#!/usr/bin/env bash
case "$*" in
  *"pr list"*)
    printf '[{"number":80}]\n' ;;
  *"repo view"*)
    printf 'armaatus/rommsync-nx\n' ;;
  *"run list"*)
    # Stamped with how many rollups have been served, so the assertion can tell
    # a call made while the check was dead from one made after it recovered.
    printf '%s\n' "$(cat "$ROLLUP_COUNT")" >>"$RUNLIST_LOG"
    printf '\n' ;;
  *statusCheckRollup*)
    n=$(( $(cat "$ROLLUP_COUNT") + 1 )); printf '%s' "$n" >"$ROLLUP_COUNT"
    if [ "$n" = 1 ]; then
      # The review job died. Nothing else is failing, so the red-build path
      # (which needs the same non-review check twice running) never fires and
      # cannot end the wait early.
      printf '{"statusCheckRollup":[{"name":"review against REVIEW.md","conclusion":"FAILURE"}]}\n'
    else
      printf '{"statusCheckRollup":[{"name":"review against REVIEW.md","conclusion":"SUCCESS"}]}\n'
    fi ;;
  *"pr view"*)
    printf '{"reviews":[]}\n' ;;
  *)
    printf '\n' ;;
esac
GHSTUB
    chmod +x "$stub/gh"
    ( cd "$TMPDIR_FIXTURE" && git init -q . && git commit -q --allow-empty -m fixture ) 2>/dev/null
    runlog="$TMPDIR_FIXTURE/runlist.log"; : >"$runlog"
    counter="$TMPDIR_FIXTURE/rollups"; printf '0' >"$counter"
    # Long enough to cross at least two throttle checks (they fire on polls
    # 1, 5, 9 ...), so the recovery at check 2 is actually reached.
    ( cd "$TMPDIR_FIXTURE" &&
      PATH="$stub:$PATH" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" \
      RUNLIST_LOG="$runlog" ROLLUP_COUNT="$counter" \
      AWAIT_REVIEW_DEADLINE=9 AWAIT_REVIEW_POLL=1 \
      bash "$TMPDIR_FIXTURE/scripts/orca/await-review.sh" 80 >/dev/null 2>&1 )
    [ "$(cat "$counter")" -ge 2 ] \
      || fail "the wait never reached a second throttle check ($(cat "$counter")) -- the fixture proves nothing"
    late="$(awk '$1 >= 2' "$runlog" | wc -l | tr -d ' ')"
    [ "${late:-0}" = 0 ] \
      || fail "spent $late run-list call(s) after the rollup said the review check had recovered; review_dead never cleared"
    echo "PASS: the run-list stops once the review check recovers"
    ;;

  await_ignores_what_the_gate_would_not_count)
    # #114: await-review.sh said "the review is in" on three shapes merge-gate
    # then refused, and every one of them cost a round out of three.
    #
    #   the PR author's own record -- replying to a review THREAD submits a
    #     COMMENTED review attributed to the replier, so an agent answering
    #     findings manufactured its own independent review. Verified on real
    #     data: PR #108 had one at commit.oid == headRefOid, PR #95 had four;
    #   a real review on an OLDER head -- the answer to the previous push, which
    #     the fix being waited on has already invalidated;
    #   an empty record on this head -- a review record is not a review (#95).
    #
    # All three are dated well into the future, so the freshness cut-off this
    # script used to rely on -- the HEAD commit's own time -- accepts every one
    # of them. Nothing but the gate's rules can turn this into a wait.
    make_await_fixture
    write_await_reviews \
      '{"state":"COMMENTED","submittedAt":"2099-01-01T00:00:00Z",
        "commit":{"oid":"'"$AW_PR_HEAD"'"},"author":{"login":"armaatus"},
        "body":"","comments":{"totalCount":0}},
       {"state":"CHANGES_REQUESTED","submittedAt":"2099-01-01T01:00:00Z",
        "commit":{"oid":"0000000000000000000000000000000000000000"},
        "author":{"login":"claude"},
        "body":"A real review of the commit before this one, long enough to clear MIN_REVIEW_BODY.",
        "comments":{"totalCount":3}},
       {"state":"COMMENTED","submittedAt":"2099-01-01T02:00:00Z",
        "commit":{"oid":"'"$AW_PR_HEAD"'"},"author":{"login":"claude"},
        "body":"test","comments":{"totalCount":0}}'
    out="$(run_await_review 6)"; rc=$?
    [ "$rc" = 4 ] \
      || fail "handed back a review merge-gate does not count (exit $rc); the PR would then sit BLOCKED for the reason the wait just called satisfied: $out"
    grep -q "no body; see the inline comments" <<<"$out" \
      && fail "printed the author's own empty thread reply as the review: $out"
    grep -q "commit before this one" <<<"$out" \
      && fail "printed a review of an older head as the answer to this push: $out"
    # And it cost nothing. The round cap is three, and a round spent on a review
    # that was never a review is a round the real disagreement does not get.
    [ ! -f "$TMPDIR_FIXTURE/.orca/review-rounds" ] \
      || fail "burned a review round on a review the gate does not count: $(cat "$TMPDIR_FIXTURE/.orca/review-rounds")"
    # And it says so, rather than reporting plain silence -- three records did
    # arrive, and an agent told only "no review arrived" goes looking at the
    # review workflow instead of at what was discounted.
    grep -q "none of them is a review" <<<"$out" \
      || fail "discounted three records and reported plain silence: $out"
    echo "PASS: await-review counts a review the way merge_gate.py does"
    ;;

  await_reports_the_independent_review_on_this_head)
    # The other half: the filters above must not have made the wait unsatisfiable.
    # Same three records the phase above rejects, plus the real one, and this
    # has to end at once with that review in hand and the round recorded.
    make_await_fixture
    write_await_reviews \
      '{"state":"COMMENTED","submittedAt":"2099-01-01T00:00:00Z",
        "commit":{"oid":"'"$AW_PR_HEAD"'"},"author":{"login":"armaatus"},
        "body":"","comments":{"totalCount":0}},
       {"state":"CHANGES_REQUESTED","submittedAt":"2099-01-01T03:00:00Z",
        "commit":{"oid":"'"$AW_PR_HEAD"'"},"author":{"login":"claude"},
        "body":"IMPORTANT: the backup is written after the overwrite, not before.",
        "comments":{"totalCount":1}}'
    out="$(run_await_review 6)"; rc=$?
    [ "$rc" = 0 ] \
      || fail "waited out the deadline with a real review on this head in hand (exit $rc): $out"
    grep -q "the backup is written after the overwrite" <<<"$out" \
      || fail "did not print the review it was waiting for: $out"
    grep -q "CHANGES_REQUESTED by claude" <<<"$out" \
      || fail "did not say who reviewed and what their verdict was: $out"
    grep -q "armaatus" <<<"$out" \
      && fail "printed the author's own empty record alongside the review: $out"
    grep -q "114 1" "$TMPDIR_FIXTURE/.orca/review-rounds" \
      || fail "read a review without counting the round; the cap of three stops bounding anything: $(cat "$TMPDIR_FIXTURE/.orca/review-rounds" 2>&1)"
    echo "PASS: a real independent review on this head still ends the wait"
    ;;

  await_stops_when_the_gate_will_not_import)
    # Now that merge_gate.py decides what counts, a merge_gate.py that will not
    # import decides that NOTHING counts -- and that is indistinguishable from
    # "no review yet" unless it is said. merge_gate.py is a file agents in this
    # repo edit; a half-finished edit or a rename of one of these two functions
    # would otherwise cost the full 45-minute deadline and then blame the review
    # job, which is a GitHub Actions problem the agent would go and look for.
    #
    # review-status.sh answers the same condition with exit 2, "could not tell".
    # So does this, immediately.
    make_await_fixture
    write_await_reviews \
      '{"state":"CHANGES_REQUESTED","submittedAt":"2099-01-01T03:00:00Z",
        "commit":{"oid":"'"$AW_PR_HEAD"'"},"author":{"login":"claude"},
        "body":"A real review that will never be seen while the gate is broken.",
        "comments":{"totalCount":1}}'
    printf 'def independent_reviews(\n' >"$TMPDIR_FIXTURE/.github/scripts/merge_gate.py"
    out="$(run_await_review 12)"; rc=$?
    [ "$rc" = 2 ] \
      || fail "a merge_gate.py that will not import read as 'no review yet' (exit $rc); the wait would cost its whole deadline and then blame the review job: $out"
    grep -q "merge_gate.py" <<<"$out" \
      || fail "did not name the file that has to import before anything here can answer: $out"
    echo "PASS: a gate that will not import is 'could not tell', not silence"
    ;;

  await_never_hands_back_the_same_review_twice)
    # The round cap is three, and a round is spent whenever a review is read. So
    # a review must be read once.
    #
    # A second visit on an UNCHANGED head is a real flow, not a mistake:
    # claude-review.yml fires on `review_requested` as well as on `synchronize`,
    # so an agent that re-requests without pushing is waiting for a SECOND review
    # of the same commit. Neither the commit time this used to compare against
    # nor the push time #114 asked for tells that apart from the review already
    # acted on -- both are older than both reviews. What does is the newest one
    # handed back, which is why record_round now writes it down.
    make_await_fixture
    write_await_reviews \
      '{"state":"CHANGES_REQUESTED","submittedAt":"2099-01-01T03:00:00Z",
        "commit":{"oid":"'"$AW_PR_HEAD"'"},"author":{"login":"claude"},
        "body":"IMPORTANT: the backup is written after the overwrite, not before.",
        "comments":{"totalCount":1}}'
    out="$(run_await_review 6)"; rc=$?
    [ "$rc" = 0 ] || fail "the first visit did not read the review at all (exit $rc): $out"

    # Nothing pushed, nothing re-reviewed: the same payload, second visit.
    out2="$(run_await_review 6)"; rc2=$?
    [ "$rc2" != 0 ] \
      || fail "handed the same review back a second time and burned round 2 of 3 on findings already in hand: $out2"
    grep -q "already handed back" <<<"$out2" \
      || fail "waited without saying why, which reads as the reviewer never running: $out2"
    grep -q "^114 1 " "$TMPDIR_FIXTURE/.orca/review-rounds" \
      || fail "the round count moved past 1 without a new review being read: $(cat "$TMPDIR_FIXTURE/.orca/review-rounds")"

    # ...and a genuine re-review on the same head IS read. This is the half a
    # blunt "same head, already answered" rule would break.
    write_await_reviews \
      '{"state":"CHANGES_REQUESTED","submittedAt":"2099-01-01T03:00:00Z",
        "commit":{"oid":"'"$AW_PR_HEAD"'"},"author":{"login":"claude"},
        "body":"IMPORTANT: the backup is written after the overwrite, not before.",
        "comments":{"totalCount":1}},
       {"state":"COMMENTED","submittedAt":"2099-01-01T09:00:00Z",
        "commit":{"oid":"'"$AW_PR_HEAD"'"},"author":{"login":"claude"},
        "body":"Re-reviewed on the same head after the request: the backup ordering is fixed.",
        "comments":{"totalCount":0}}'
    out3="$(run_await_review 6)"; rc3=$?
    [ "$rc3" = 0 ] \
      || fail "a re-review on the same head was never read, so a review_requested round can never end (exit $rc3): $out3"
    grep -q "Re-reviewed on the same head" <<<"$out3" \
      || fail "did not print the new review: $out3"
    grep -q "the backup is written after the overwrite" <<<"$out3" \
      && fail "reprinted the review already acted on alongside the new one: $out3"
    echo "PASS: a review is handed back once, and a re-review still lands"
    ;;

  await_judges_the_head_github_has)
    # "This head" is the head on the PULL REQUEST, not the one in the worktree,
    # because that is the head merge-gate judges. They differ exactly when
    # something was committed and not pushed -- and then an agent is waiting on
    # a review of code it never sent, which nothing else in this loop would say.
    #
    # So: the review is on the PR head, the worktree is somewhere else, and this
    # has to end with the review in hand AND with the divergence named.
    make_await_fixture
    # The PR is on a commit this worktree does not have.
    AW_PR_HEAD=1111111111111111111111111111111111111111
    write_await_reviews \
      '{"state":"COMMENTED","submittedAt":"2099-01-01T03:00:00Z",
        "commit":{"oid":"'"$AW_PR_HEAD"'"},"author":{"login":"claude"},
        "body":"A real review of what GitHub actually has, long enough to clear MIN_REVIEW_BODY.",
        "comments":{"totalCount":0}}'
    out="$(run_await_review 6)"; rc=$?
    [ "$rc" = 0 ] \
      || fail "judged the worktree head rather than the PR head, so a review merge-gate counts was invisible (exit $rc): $out"
    grep -q "something unpushed" <<<"$out" \
      || fail "the worktree is on a commit the PR does not have and nothing said so: $out"
    # And it must not spend a round. The review answers code this worktree has
    # already moved past; the round is the answer to what gets pushed next, and
    # there are only three.
    [ ! -f "$TMPDIR_FIXTURE/.orca/review-rounds" ] \
      || fail "spent a review round on a review of a commit this worktree has already superseded: $(cat "$TMPDIR_FIXTURE/.orca/review-rounds")"
    grep -q "Not counted as one of the" <<<"$out" \
      || fail "did not say the round was not counted, so the agent cannot tell how many it has left: $out"
    echo "PASS: the head judged is the PR head, a divergence is named, and no round is spent"
    ;;

  review_status_matches_the_gate_on_a_thread_reply)
    # #114's acceptance, stated as the two answers being the same one. A PR
    # whose only on-head review is the author's own reply to a review thread is
    # the shape that made this diverge: GitHub attributes a COMMENTED review to
    # the replier, so the PR looks reviewed to anything that does not ask who
    # wrote it.
    #
    # The assertion runs merge_gate.py over the very same payload rather than
    # hardcoding what it would say. That is the property worth pinning -- not
    # "review-status exits 1 here", which a future rule change could make wrong
    # in both places at once without this noticing.
    make_review_status_fixture
    write_pr_reviews \
      '{"state":"COMMENTED","submittedAt":"2026-09-06T10:00:00Z",
        "commit":{"oid":"'"$RS_HEAD"'"},"author":{"login":"armaatus"},
        "body":"","comments":{"totalCount":0}}' \
      '' '"## Plan\n/code-review high\nmattpocock-skills:code-review\n"'
    write_pr_checks \
      '{"name":"host-tests","status":"COMPLETED","conclusion":"SUCCESS",
        "startedAt":"2026-09-06T09:00:00Z","completedAt":"2026-09-06T09:30:00Z"}' \
      BLOCKED core/src/sync.cpp
    out="$(run_review_status)"; rs_rc=$?
    gate="$(cd "$TMPDIR_FIXTURE" &&
            python3 .github/scripts/merge_gate.py "$RS_HEAD" graphql.json files.txt 2>&1)"
    gate_rc=$?
    [ "$gate_rc" = 1 ] \
      || fail "the gate itself accepted the author's own thread reply as an independent review; the fixture proves nothing: $gate"
    [ "$rs_rc" = "$gate_rc" ] \
      || fail "review-status said $rs_rc where merge_gate.py says $gate_rc -- the local answer and the required check disagree, which is how a PR sits quietly BLOCKED: $out"
    # The exit codes alone would be a weak assertion: these are different spaces
    # (0-4 here, 0/1 there) and review-status also exits 1 for BLOCKED and for
    # DIRTY, so 1 == 1 can match for the wrong reason. The REASON is the thing
    # that has to be the same one, so the gate's own sentence has to appear
    # verbatim in what review-status printed.
    reason="$(grep -F "no independent review" <<<"$gate" | sed 's/^ *//')"
    [ -n "$reason" ] \
      || fail "the gate refused for some reason other than independence; the fixture no longer describes the acceptance shape: $gate"
    grep -qF "$reason" <<<"$out" \
      || fail "review-status reached the same exit code by a different route -- it never gave the gate's reason, so the agent is sent to fix the wrong thing: $out"
    echo "PASS: review-status and merge_gate.py agree on a thread reply"
    ;;

  reap_judges_removal_by_the_directory)
    # #27's worktree removal exited non-zero and the fleet logged "could not
    # remove it" -- while the same command by hand removed it, warning only that
    # git would not delete the branch. The slot stayed held, and the fleet ran at
    # two worktrees instead of three. Removal is judged by whether the directory
    # is gone, and retried once before it is given up on.
    make_fixture
    cp "$REPO_ROOT"/scripts/orca/{fleet.sh,lib.sh} "$TMPDIR_FIXTURE/scripts/orca/"
    stub="$TMPDIR_FIXTURE/stub-bin"; mkdir -p "$stub"
    target="$TMPDIR_FIXTURE/wt-27"; mkdir -p "$target"
    # Removes the worktree and THEN exits non-zero, exactly as the real one did.
    cat >"$stub/orca" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"$TMPDIR_FIXTURE/orca-calls.log"
case "\$1" in
  --version) exit 0 ;;
  worktree)  rm -rf "$target"; echo 'warning: local branch was kept' >&2; exit 1 ;;
  *)         printf '{"ok":true}\n' ;;
esac
STUB
    chmod +x "$stub/orca"
    : >"$TMPDIR_FIXTURE/orca-calls.log"
    run_remove() {
      PATH="$stub:$PATH" ROMMSYNC_FLEET_DIR="$TMPDIR_FIXTURE/fleet" \
      bash -c '. '"$TMPDIR_FIXTURE"'/scripts/orca/lib.sh
               orca_cli_resolve
               '"$(sed -n '/^remove_worktree()/,/^}/p' "$TMPDIR_FIXTURE/scripts/orca/fleet.sh")"'
               remove_worktree "'"$1"'"'
    }
    run_remove "$target"; rc=$?
    [ "$rc" = 0 ] \
      || fail "reported failure for a worktree that IS gone (exit $rc) -- that is the bug that held #27's slot"
    [ "$(grep -c 'worktree rm' "$TMPDIR_FIXTURE/orca-calls.log")" = 1 ] \
      || fail "retried a removal that had already succeeded"

    # And a removal that genuinely does nothing must still fail -- after a retry.
    stuck="$TMPDIR_FIXTURE/wt-stuck"; mkdir -p "$stuck"
    cat >"$stub/orca" <<'STUB'
#!/usr/bin/env bash
case "$1" in
  --version) exit 0 ;;
  *)         exit 1 ;;
esac
STUB
    chmod +x "$stub/orca"
    run_remove "$stuck"; rc=$?
    [ "$rc" != 0 ] \
      || fail "claimed success while the worktree is still on disk"
    [ -d "$stuck" ] || fail "fixture removed itself; the test proves nothing"
    echo "PASS: removal is judged by the directory, retried once, and still fails honestly"
    ;;

  brief_warns_about_human_merge_paths)
    # #96 is issue #34, "Versioning + Releases from CI" -- its scope IS
    # .github/workflows/ci.yml, which merge_gate.py refuses by design. The agent
    # did nothing wrong and cannot merge, and without being told so it burns its
    # three review rounds trying to turn a gate green that never will be. The
    # brief has to name the paths and say what "done" looks like for them.
    brief="$REPO_ROOT/scripts/orca/issue-command.sh"
    for path in ".github/workflows/" ".github/scripts/" ".claude/"; do
      grep -q -- "$path" "$brief" \
        || fail "the brief never mentions $path, which merge-gate refuses"
    done
    grep -qi "needs a human merge" "$brief" \
      || fail "the brief does not say what done looks like for a human-merge PR"
    # The gate and the brief must name the SAME paths -- a brief that warns about
    # a different set than the gate enforces is worse than no warning.
    gate="$REPO_ROOT/.github/scripts/merge_gate.py"
    if [ -f "$gate" ]; then
      # EVERY place that makes the claim, not just one of them. The brief says
      # it twice -- once in step 4 and once in the closing notes -- and CLAUDE.md
      # says it a third time. Checking only that each path appears SOMEWHERE
      # passes while a second, narrower list sits a few lines further down
      # contradicting the first, which is exactly what was found here: the
      # closing note named two of the three paths, so an agent whose PR touched
      # only .github/scripts/ read the correct warning and then the one that
      # said it was fine, with nothing to say which governs.
      python3 - "$gate" "$brief" "$REPO_ROOT/CLAUDE.md" <<'PYCHECK' || fail "a human-merge-path claim names fewer paths than merge_gate.py refuses"
import re, sys
gate = open(sys.argv[1]).read()
block = re.search(r"HUMAN_ONLY_PREFIXES\s*=\s*\((.*?)\)", gate, re.S)
paths = re.findall(r'"([^"]+)"', block.group(1)) if block else []
if not paths:
    print("could not read HUMAN_ONLY_PREFIXES from the gate", file=sys.stderr)
    raise SystemExit(1)

# A paragraph that both names one of the refused paths and asserts something
# about merging is making the claim, and has to make it in full.
claim = re.compile(r"auto-merge|merges? itself|never merges", re.I)
bad = False
for doc in sys.argv[2:]:
    text = open(doc).read()
    for para in re.split(r"\n\s*\n", text):
        if not claim.search(para):
            continue
        named = [p for p in paths if p in para]
        if not named:
            continue
        missing = [p for p in paths if p not in para]
        if missing:
            print(f"{doc}: a paragraph claiming {named} never auto-merges "
                  f"omits {missing}:\n    " + "\n    ".join(para.strip().splitlines()),
                  file=sys.stderr)
            bad = True
raise SystemExit(1 if bad else 0)
PYCHECK
    fi
    echo "PASS: every human-merge-path claim names the full set merge-gate refuses"
    ;;

  fleet_reads_every_closing_keyword)
    # GitHub links a PR to an issue on any of NINE keywords, case-insensitively
    # and with any run of whitespace between. The fleet used to match one exact
    # spelling, `Closes #N`, so a PR saying `Fixes #12` closed the issue and
    # unblocked its dependants while `has_open_pr` still reported #12 free --
    # and the dispatcher opened a second worktree for work already in flight,
    # burning one of only three slots.
    make_fleet_parse_fixture
    python3 - "$FLEET_OPEN_PRS" <<'PY'
import json, sys
json.dump([
    {"number": 201, "body": "Fixes #12\n"},
    {"number": 202, "body": "closes #13\n"},
    {"number": 203, "body": "Closes  #14\n"},
    {"number": 204, "body": "RESOLVED #15\n"},
    {"number": 205, "body": "Closes #160\n"},
    {"number": 206, "body": "prose that merely mentions #17, closing nothing\n"},
], open(sys.argv[1], "w"))
PY
    for n in 12 13 14 15; do
      run_fleet_fn has_open_pr "$n" \
        || fail "a PR closing #$n was not seen; GitHub accepts that spelling and so must the fleet"
    done
    # `Closes #160` is not a PR for #16. The old pattern ended in \b, which the
    # digits themselves satisfy, so this has to stay asserted.
    run_fleet_fn has_open_pr 16 \
      && fail "#160 answered for #16; a wider keyword set must not widen the NUMBER"
    run_fleet_fn has_open_pr 17 \
      && fail "a bare mention of #17 was read as a closing line; the fleet would then never start it"
    echo "PASS: every spelling GitHub closes on is a PR the fleet can see"
    ;;

  fleet_reads_blocked_by_case_insensitively)
    # unblock.yml matches /blocked\s+by\s+#(\d+)/gi; the fleet matched
    # `Blocked by #` exactly. A body written `blocked by #7` was a blocker to
    # the workflow and invisible here, so #7 -- the issue that frees two others
    # -- scored zero and lost the ordering to a lower-numbered issue that frees
    # nothing. Most-unblocking-first is the whole point of the queue.
    make_fleet_parse_fixture
    python3 - "$FLEET_ISSUES" <<'PY'
import json, sys
json.dump([
    {"number": 6, "title": "frees nothing", "body": "",
     "labels": [{"name": "ready"}]},
    {"number": 7, "title": "the foundation", "body": "",
     "labels": [{"name": "ready"}]},
    {"number": 9, "title": "lowercase", "body": "blocked by #7",
     "labels": [{"name": "blocked"}]},
    {"number": 10, "title": "odd spacing", "body": "Blocked  By  #7",
     "labels": [{"name": "blocked"}]},
], open(sys.argv[1], "w"))
PY
    out="$(run_fleet_fn ready_issues)"
    first="$(printf '%s\n' "$out" | head -1)"
    [ "$(printf '%s' "$first" | cut -f1)" = "7" ] \
      || fail "the most-unblocking issue did not sort first; got: $out"
    [ "$(printf '%s' "$first" | cut -f2)" = "2" ] \
      || fail "#7 was not credited with both issues naming it; got: $out"
    echo "PASS: the fleet counts the same blockers unblock.yml does"
    ;;

  fleet_counts_startable_by_the_same_rule)
    # `count_startable` is what the run loop calls "nothing to wait for". Read
    # with the narrow pattern it counts an issue whose PR is already open, and
    # the loop keeps a slot warm for work that is finished.
    make_fleet_parse_fixture
    python3 - "$FLEET_ISSUES" <<'PY'
import json, sys
json.dump([
    {"number": 6, "title": "genuinely free", "body": "", "labels": [{"name": "ready"}]},
    {"number": 7, "title": "already in flight", "body": "", "labels": [{"name": "ready"}]},
], open(sys.argv[1], "w"))
PY
    python3 - "$FLEET_OPEN_PRS" <<'PY'
import json, sys
json.dump([{"number": 301, "body": "Fixes #7\n"}], open(sys.argv[1], "w"))
PY
    n="$(run_fleet_fn count_startable)"
    [ "$n" = "1" ] \
      || fail "counted $n startable; #7 has an open PR saying Fixes, so only #6 is"

    # ...and each body is read on its own. Flattened into one string, a body
    # ending in the word "fixes" ahead of one opening `#6` matches across the
    # join -- the count then hides an issue nobody is working on, and the
    # dispatcher waits for a slot that is already free.
    python3 - "$FLEET_OPEN_PRS" <<'PRS'
import json, sys
json.dump([
    {"number": 302, "body": "nothing here closes anything, but it ends in fixes"},
    {"number": 303, "body": "#6 is mentioned first thing, and closed by nobody"},
], open(sys.argv[1], "w"))
PRS
    n="$(run_fleet_fn count_startable)"
    [ "$n" = "2" ] \
      || fail "counted $n startable; neither PR closes anything, so both #6 and #7 are"
    echo "PASS: an open PR hides its issue from the startable count whatever it says"
    ;;

  fleet_sees_a_merged_pr_that_says_fixes)
    # `issue_is_done` is what takes an issue off `fleet.sh run 11 12 13`. Miss
    # the closing line and the list never empties: the dispatcher relaunches a
    # worktree for work that merged hours ago.
    make_fleet_parse_fixture
    python3 - "$FLEET_MERGED_PRS" <<'PY'
import json, sys
json.dump([{"body": "Fixes #21\n"}, {"body": "resolve #22\n"}],
          open(sys.argv[1], "w"))
PY
    run_fleet_fn issue_is_done 21 \
      || fail "a merged PR saying 'Fixes #21' did not mark #21 landed"
    run_fleet_fn issue_is_done 22 \
      || fail "a merged PR saying 'resolve #22' did not mark #22 landed"
    run_fleet_fn issue_is_done 23 \
      && fail "#23 is neither closed nor merged, and was reported landed anyway"
    echo "PASS: a merged PR is read the way GitHub read it"
    ;;

  fleet_cannot_tell_when_the_pr_lookup_fails)
    # "Nothing printed" is what no-PR looks like, and it is also what a failed
    # `gh` call or a broken import looks like. Read as "free" they are the same
    # answer, and the fleet opens a second worktree for work already in flight
    # -- the exact failure the shared pattern exists to prevent, arriving by a
    # different door. `in_flight` has always documented a third answer; until
    # now `has_open_pr` could not produce it.
    make_fleet_parse_fixture
    FLEET_GH_FAIL=1
    run_fleet_fn has_open_pr 12; rc=$?
    [ "$rc" = 2 ] \
      || fail "has_open_pr answered $rc when the lookup failed; 1 means 'free', which is a guess"
    run_fleet_fn in_flight 12; rc=$?
    [ "$rc" = 2 ] \
      || fail "in_flight answered $rc; its own contract reserves 2 for 'could not tell'"
    # ...and a healthy lookup still answers plainly, so the guard above cannot
    # be satisfied by returning 2 for everything.
    FLEET_GH_FAIL=""
    run_fleet_fn has_open_pr 12; rc=$?
    [ "$rc" = 1 ] \
      || fail "has_open_pr answered $rc for an issue with no PR; that must stay a plain 'free'"
    echo "PASS: a failed PR lookup is 'could not tell', never 'free'"
    ;;

  fleet_leaves_a_timed_out_agent_when_it_cannot_tell)
    # The time-box is the one branch that ENDS an agent: it interrupts the
    # terminal, comments on the issue and drops the started marker. Reading a
    # failed `gh` call as "no PR" there costs three hours of finished work,
    # because the agent it stops is as likely to be the one waiting on a review
    # as the one grinding. `has_open_pr` answers 2 for "could not tell";
    # enforce_timebox has to spend that answer on doing nothing, and say so.
    make_fixture
    mkdir -p "$TMPDIR_FIXTURE/.github/scripts"
    cp "$REPO_ROOT"/scripts/orca/fleet.sh "$TMPDIR_FIXTURE/scripts/orca/"
    cp "$REPO_ROOT"/.github/scripts/*.py "$TMPDIR_FIXTURE/.github/scripts/"
    stub="$TMPDIR_FIXTURE/stub-bin"; mkdir -p "$stub"
    state="$TMPDIR_FIXTURE/fleet"; mkdir -p "$state/worktrees" "$state/started"
    printf '%s\n' "$TMPDIR_FIXTURE" >"$state/worktrees/29"
    # Started at the epoch: expired under any time-box the fleet could be given.
    printf '0\n' >"$state/started/29"
    printf '[]\n' >"$TMPDIR_FIXTURE/open-prs.json"
    cat >"$stub/gh" <<GHSTUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"$TMPDIR_FIXTURE/gh-calls.log"
[ -n "\${FLEET_GH_FAIL:-}" ] && exit 1
case "\$*" in
  *"pr list"*) cat "$TMPDIR_FIXTURE/open-prs.json" ;;
  *)           printf '\n' ;;
esac
GHSTUB
    chmod +x "$stub/gh"
    cat >"$stub/orca" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"$TMPDIR_FIXTURE/orca-calls.log"
case "\$*" in
  --version) exit 0 ;;
  *"terminal list"*)
    printf '{"ok":true,"result":{"terminals":[{"handle":"t-29","worktreePath":"$TMPDIR_FIXTURE","agentIdentity":"claude"}]}}\n' ;;
  *) printf '{"ok":true,"result":{}}\n' ;;
esac
STUB
    chmod +x "$stub/orca"

    # One expired issue through the real enforce_timebox, with $1 deciding
    # whether `gh` answers at all. Only the surfaces that reach outside the
    # function are stood in for; the decision itself is the code under test.
    run_timebox() {
      : >"$TMPDIR_FIXTURE/orca-calls.log"; : >"$TMPDIR_FIXTURE/gh-calls.log"
      PATH="$stub:$PATH" FLEET_GH_FAIL="$1" \
      bash -c '. '"$TMPDIR_FIXTURE"'/scripts/orca/lib.sh
               orca_cli_resolve
               REPO_ROOT="'"$TMPDIR_FIXTURE"'"
               STATE_DIR="'"$state"'"; OWNED_DIR="$STATE_DIR/worktrees"
               STARTED_DIR="$STATE_DIR/started"; TIMEBOX_SECONDS=10800
               say() { echo "$*"; }
               notify() { echo "NOTIFY: $*"; }
               card() { echo "CARD: $*"; }
               orca_json() { local o; o="$(mktemp)"; orca_run_with_deadline 10 "$o" "$ORCA_CLI" "$@" --json; cat "$o"; rm -f "$o"; }
               '"$(sed -n '/^ISSUE_REFS=/p; /^owned_path()/p;
                             /^has_open_pr()/,/^}/p; /^agent_terminal_in()/,/^}/p;
                             /^enforce_timebox()/,/^}/p' \
                    "$TMPDIR_FIXTURE/scripts/orca/fleet.sh")"'
               enforce_timebox' 2>&1
    }

    # 1. `gh` is down. The agent may well have a PR up; nothing here knows.
    out="$(run_timebox 1)"
    grep -q "could not tell" <<<"$out" \
      || fail "the log does not say it could not tell; a silent skip reads as 'nothing expired': $out"
    grep -q -- "--interrupt" "$TMPDIR_FIXTURE/orca-calls.log" \
      && fail "interrupted an agent on a lookup that never answered: $(cat "$TMPDIR_FIXTURE/orca-calls.log")"
    grep -q "issue comment" "$TMPDIR_FIXTURE/gh-calls.log" \
      && fail "told the issue the fleet gave up, on the strength of a failed lookup"
    [ -e "$state/started/29" ] \
      || fail "dropped the started marker, so the next pass can never time this issue out at all"

    # 2. ...and `gh` answering plainly still ends a genuinely stuck agent. Without
    # this the guard above is satisfied by a time-box that never fires.
    out="$(run_timebox "")"
    grep -q -- "--interrupt" "$TMPDIR_FIXTURE/orca-calls.log" \
      || fail "3h with a working lookup and no PR is exactly what the time-box is for: $out"
    grep -q "issue comment" "$TMPDIR_FIXTURE/gh-calls.log" \
      || fail "stopped the agent without saying so on the issue: $out"
    [ -e "$state/started/29" ] \
      && fail "kept the started marker after stopping the agent; it would be stopped again every poll"

    # 3. A PR that is up is not a time-out, however long it took to open.
    printf '0\n' >"$state/started/29"
    printf '[{"number":401,"body":"Closes #29\\n"}]\n' >"$TMPDIR_FIXTURE/open-prs.json"
    out="$(run_timebox "")"
    grep -q -- "--interrupt" "$TMPDIR_FIXTURE/orca-calls.log" \
      && fail "interrupted an agent whose PR is open and waiting on review: $out"
    [ -e "$state/started/29" ] \
      && fail "an issue that got where it was going should stop being timed"
    echo "PASS: a failed lookup leaves the time-boxed agent alone, and says so"
    ;;

  *)
    echo "usage: $0 opens|reuses|foreign|no_romm|submits|no_draft|unstable" >&2
    echo "       watch_needs_issue|watch_late_draft|watch_grace|watch_submits|watch_single" >&2
    echo "       watch_bare_url|watch_full_draft_untouched|cli_broken" >&2
    echo "       await_reports_failed_review|await_reports_a_red_build" >&2
    echo "       await_reports_a_conflicted_pr" >&2
    echo "       await_finds_the_failed_run_under_newer_skipped_ones" >&2
    echo "       await_throttles_the_lookup_when_the_run_is_never_found" >&2
    echo "       fleet_notices_a_stalled_agent" >&2
    echo "       review_status_shows_the_open_thread|brief_never_lists_threads_over_rest" >&2
    echo "       brief_queues_the_merge_at_step_four" >&2
    echo "       await_costs_nothing_while_the_review_is_healthy" >&2
    echo "       await_stops_paying_once_the_review_recovers" >&2
    echo "       reap_judges_removal_by_the_directory" >&2
    echo "       brief_warns_about_human_merge_paths" >&2
    echo "       review_status_ignores_superseded_checks" >&2
    echo "       review_status_mirrors_the_gate_on_who_reviewed" >&2
    echo "       review_status_says_a_human_merges_this_one" >&2
    echo "       review_status_names_the_wedge" >&2
    echo "       review_status_waits_for_the_run_in_flight" >&2
    echo "       review_status_reports_a_conflict_before_the_human_merge" >&2
    echo "       fleet_reads_every_closing_keyword" >&2
    echo "       fleet_reads_blocked_by_case_insensitively" >&2
    echo "       fleet_counts_startable_by_the_same_rule" >&2
    echo "       fleet_sees_a_merged_pr_that_says_fixes" >&2
    echo "       fleet_cannot_tell_when_the_pr_lookup_fails" >&2
    echo "       fleet_leaves_a_timed_out_agent_when_it_cannot_tell" >&2
    exit 2
    ;;
esac
