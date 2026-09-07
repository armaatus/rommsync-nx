#!/usr/bin/env bash
# Covers the two places scripts/orca/fleet.sh used to report a state it had not
# established: the board update, and the worktree removal.
#
#   test_orca_fleet.sh card_says      the Orca CLI refuses -> the dispatcher log
#                                     says the board update failed, and carries
#                                     the CLI's own words. WORKFLOW.md calls the
#                                     board THE status surface, so a silent
#                                     failure freezes it with nothing anywhere
#                                     saying it froze.
#   test_orca_fleet.sh card_quiet     it worked -> nothing is said. A warning per
#                                     poll would be its own kind of noise.
#   test_orca_fleet.sh remove_forces  `worktree rm` fails and `--force` works ->
#                                     the worktree is removed. This repo has a
#                                     submodule and git refuses the plain
#                                     removal outright, so without the retry
#                                     EVERY merged worktree leaks.
#   test_orca_fleet.sh remove_advice  both fail -> reap_merged says how to remove
#                                     it by hand, and does NOT name reap.sh,
#                                     which skips exactly this case and prints
#                                     "nothing to reap".
#
# ...and the three places `needs-human-step` has to be honoured, because an
# issue whose last step is outward and the maintainer's is not a stalled one:
#
#   test_orca_fleet.sh stall_expected an agent `waiting` on a labelled issue ->
#                                     reported as waiting for you AS EXPECTED,
#                                     and never as "nothing should be asking".
#   test_orca_fleet.sh stall_reports  an agent `waiting` on an ordinary issue ->
#                                     still the stall it always was. The whole
#                                     point of the exemption is that the signal
#                                     keeps meaning something.
#   test_orca_fleet.sh timebox_waits  past the box, no PR, labelled -> NOT
#                                     interrupted, no "gave up" comment on the
#                                     issue, said once rather than once a
#                                     minute, and on the board rather than only
#                                     in the log.
#   test_orca_fleet.sh timebox_stops  past the box, no PR, unlabelled -> still
#                                     interrupted and still commented on.
#   test_orca_fleet.sh queue_skips    a labelled issue is not startable: the
#                                     dispatcher must not open a worktree for
#                                     work no agent may finish, or it opens one
#                                     per cycle forever (#148).
#   test_orca_fleet.sh list_declines  ...and `fleet.sh run 148` declines it too,
#                                     dropping it rather than skipping it: an
#                                     issue kept in `wanted` that can never be
#                                     launched is a run loop that never ends.
#   test_orca_fleet.sh timebox_rearms the label comes off -> the box fires. The
#                                     exemption keeps the started marker for
#                                     exactly this: deleting it would leave a
#                                     handed-back agent running uncapped.
#   test_orca_fleet.sh labels_unknown the label lookup FAILS -> the agent is not
#                                     stopped and the stall is not decided.
#                                     Every lookup here has a third answer, and
#                                     an agent is only ever stopped on an answer.
#   test_orca_fleet.sh outage_once    ...and it is said ONCE across a real poll:
#                                     notice_stalled must not clear the marker
#                                     enforce_timebox set moments earlier, which
#                                     is the line-a-minute the markers prevent.
#   test_orca_fleet.sh one_lookup     both watchers in one poll -> one `gh` call
#                                     for one issue's labels, not two.
#
# The Orca CLI and gh are stubbed on PATH; the fleet state dir is a temp dir.
# Nothing here touches a real worktree, docker, or GitHub.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() { echo "FAIL: $*" >&2; exit 1; }

WORK=""
cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; return 0; }
trap cleanup EXIT

# $1 is the CLI stub's mode: ok, set_fails, rm_needs_force, rm_never_works.
make_fixture() {
  WORK="$(mktemp -d)"; WORK="$(cd "$WORK" && pwd -P)"
  mkdir -p "$WORK/repo/scripts/orca" "$WORK/bin"
  cp "$REPO_ROOT/scripts/orca/fleet.sh" "$REPO_ROOT/scripts/orca/lib.sh" \
     "$WORK/repo/scripts/orca/"
  # ready_issues imports it, and $ISSUE_REFS is derived from the repo root.
  mkdir -p "$WORK/repo/.github/scripts"
  cp "$REPO_ROOT/.github/scripts/issue_refs.py" "$WORK/repo/.github/scripts/"

  cat >"$WORK/bin/orca-stub" <<'STUB'
#!/usr/bin/env bash
mode="$(cat "$ORCA_MODE")"
printf '%s\n' "$*" >>"$ORCA_CALLS"
case "$1" in
  --version) echo "orca 0.0.0-test"; exit 0 ;;
esac
# Matched on the pair, because `list` alone is both `worktree list` and
# `terminal list` and the dispatcher asks for both.
case "$1 ${2:-}" in
  "worktree list")   cat "$ORCA_WORKTREES"; exit 0 ;;
  # A create that SUCCEEDS, so the negative case terminates on --max-prs rather
  # than looping on "leaving it in the queue to try again".
  "worktree create") echo "{\"result\":{\"worktree\":{\"path\":\"$WORK_FOR_STUB/created\"}}}"; exit 0 ;;
  "worktree ps")   cat "$ORCA_PS"; exit 0 ;;
  "terminal list") cat "$ORCA_TERMINALS"; exit 0 ;;
  "terminal send") echo '{"ok":true}'; exit 0 ;;
esac
target=""
for arg in "$@"; do
  case "$arg" in path:*) target="${arg#path:}" ;; esac
done
case "$2" in
  set)
    # On STDERR, where a CLI actually reports a failure. The point of the check
    # under test is that the reason reaches the log, and a stub that printed it
    # on stdout would pass a card() that drops stderr on the floor.
    [ "$mode" = set_fails ] && { echo "Unable to determine Orca.app path from symlink" >&2; exit 1; }
    echo '{"ok":true}'; exit 0 ;;
  rm)
    case " $* " in *" --force "*) forced=1 ;; *) forced=0 ;; esac
    if [ "$mode" = rm_never_works ] || { [ "$mode" = rm_needs_force ] && [ "$forced" = 0 ]; }; then
      echo "fatal: working trees containing submodules cannot be moved or removed" >&2
      exit 1
    fi
    rm -rf "$target"; echo '{"ok":true}'; exit 0 ;;
esac
echo '{"ok":true}'
STUB
  chmod +x "$WORK/bin/orca-stub"

  cat >"$WORK/bin/gh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$GH_CALLS"
case "$*" in
  # reap_merged asking whether this branch's PR merged. The other `pr list` is
  # has_open_pr, which parses JSON -- answering `7` there is a parse failure,
  # which the dispatcher reads as "could not tell" rather than as "no PR".
  *"pr list --head"*) echo 7; exit 0 ;;
  *"pr list"*)        cat "$GH_PRS"; exit 0 ;;
  *"issue list"*)     cat "$GH_ISSUES"; exit 0 ;;
  # gh applies --jq itself, so the stub answers what the filter would produce.
  # The literal FAIL stands for a gh that could not answer at all -- the third
  # answer the dispatcher is built around.
  *"issue view"*"--json labels"*)
    [ "$(cat "$GH_LABELS")" = FAIL ] && { echo "gh: could not connect" >&2; exit 1; }
    cat "$GH_LABELS"; exit 0 ;;
  *"issue view"*"--json state"*) echo OPEN; exit 0 ;;
  *"issue comment"*)  exit 0 ;;
esac
echo ""
STUB
  chmod +x "$WORK/bin/gh"

  # cmd_run ends in notify(); a test suite must not put banners on the screen.
  printf '#!/usr/bin/env bash\nexit 0\n' >"$WORK/bin/osascript"
  chmod +x "$WORK/bin/osascript"

  ORCA_CALLS="$WORK/calls"; : >"$ORCA_CALLS"
  ORCA_MODE="$WORK/mode"; printf '%s' "${1:-ok}" >"$ORCA_MODE"
  GH_CALLS="$WORK/gh-calls"; : >"$GH_CALLS"
  # The defaults are the quiet answers: no agent running, no terminal, no open
  # PR, no labels. Each test overrides only the one it is about.
  ORCA_PS="$WORK/ps";               echo '{"result":{"worktrees":[]}}' >"$ORCA_PS"
  ORCA_WORKTREES="$WORK/wtlist";    echo '{"result":{"worktrees":[]}}' >"$ORCA_WORKTREES"
  ORCA_TERMINALS="$WORK/terminals"; echo '{"result":{"terminals":[]}}' >"$ORCA_TERMINALS"
  GH_PRS="$WORK/prs";               echo '[]' >"$GH_PRS"
  GH_ISSUES="$WORK/issues";         echo '[]' >"$GH_ISSUES"
  GH_LABELS="$WORK/labels";         : >"$GH_LABELS"
  WORK_FOR_STUB="$WORK"; mkdir -p "$WORK/created"
  export ORCA_CALLS ORCA_MODE GH_CALLS ORCA_PS ORCA_WORKTREES ORCA_TERMINALS \
         GH_PRS GH_ISSUES GH_LABELS WORK_FOR_STUB
  # cmd_run sleeps between passes; a test that reached one would otherwise sit
  # for a minute before failing.
  export ROMMSYNC_FLEET_POLL=1
  export ORCA_CLI_COMMAND="$WORK/bin/orca-stub"
  export ROMMSYNC_FLEET_DIR="$WORK/fleet"
  PATH="$WORK/bin:$PATH"
  export PATH
}

# A worktree the fleet would own: a git repo with one commit, and an owned-file
# naming it.
make_worktree() {
  mkdir -p "$WORK/wt"
  git -C "$WORK/wt" init -q -b work
  git -C "$WORK/wt" -c user.email=t@t -c user.name=t commit -q --allow-empty -m init
  mkdir -p "$ROMMSYNC_FLEET_DIR/worktrees"
  printf '%s\n' "$WORK/wt" >"$ROMMSYNC_FLEET_DIR/worktrees/42"
}

# The agent Orca reports for the worktree, and the terminal the time-box would
# interrupt. Written as files so the stub answers the same thing every call.
agent_state() {
  python3 -c '
import json, sys
print(json.dumps({"result": {"worktrees": [
    {"path": sys.argv[1], "agents": [{"state": sys.argv[2]}]}]}}))
' "$WORK/wt" "$1" >"$ORCA_PS"
  python3 -c '
import json, sys
print(json.dumps({"result": {"terminals": [
    {"handle": "t1", "worktreePath": sys.argv[1], "agentIdentity": "claude"}]}}))
' "$WORK/wt" >"$ORCA_TERMINALS"
}

# What `gh issue view N --json labels --jq ...` would print.
issue_labels() { printf '%s' "$1" >"$GH_LABELS"; }

# An issue that is past its box with no PR open: the started marker is old, and
# the PR listing is empty.
make_overdue() { mkdir -p "$ROMMSYNC_FLEET_DIR/started"; echo 0 >"$ROMMSYNC_FLEET_DIR/started/42"; }

# fleet.sh returns instead of dispatching when it is sourced, so one function can
# be exercised without starting a dispatcher.
in_fleet() { (cd "$WORK/repo" && . ./scripts/orca/fleet.sh && "$@"); }

# Both watchers in ONE process, which is what a real poll is: they share the
# per-poll answer cache and the state dir, and only there can one of them undo
# what the other just wrote.
in_poll() { (cd "$WORK/repo" && . ./scripts/orca/fleet.sh && "$1"; "$2"); }

case "${1:-}" in
  card_says)
    make_fixture set_fails
    out="$(in_fleet card "/some/worktree" --workspace-status in-progress --comment "#42: building" 2>&1)"
    grep -qi "board update FAILED" <<<"$out" \
      || fail "a refused board update said nothing: $out"
    grep -q "Unable to determine Orca.app path" <<<"$out" \
      || fail "the CLI's own reason was dropped: $out"
    grep -q "in-progress" <<<"$out" \
      || fail "the log does not say which update was lost: $out"
    echo "ok: a board update that failed is in the dispatcher log"
    ;;
  card_quiet)
    make_fixture ok
    out="$(in_fleet card "/some/worktree" --comment "#42: building" 2>&1)"
    grep -qi "fail" <<<"$out" && fail "a successful board update complained: $out"
    echo "ok: a board update that worked says nothing"
    ;;
  remove_forces)
    make_fixture rm_needs_force
    make_worktree
    in_fleet remove_worktree "$WORK/wt" >/dev/null 2>&1 \
      || fail "remove_worktree gave up on a worktree --force would have removed"
    [ -d "$WORK/wt" ] && fail "it reported success and the worktree is still there"
    grep -q -- "--force" "$ORCA_CALLS" \
      || fail "the retry did not pass --force, so the submodule refusal stands"
    echo "ok: a worktree git refuses to remove is removed with --force"
    ;;
  remove_advice)
    make_fixture rm_never_works
    make_worktree
    out="$(in_fleet reap_merged 2>&1)"
    grep -q "could not remove it" <<<"$out" \
      || fail "a failed removal was not reported: $out"
    grep -q "git worktree remove --force" <<<"$out" \
      || fail "it did not say how to remove it by hand: $out"
    grep -q "reap.sh" <<<"$out" \
      && fail "it still points at reap.sh, which skips a worktree that is still there: $out"
    grep -q "containing submodules" <<<"$out" \
      || fail "the reason git gave was dropped; it is the difference between this and a hung CLI: $out"
    echo "ok: a failed removal says something that would actually clean it up"
    ;;
  stall_expected)
    make_fixture ok
    make_worktree
    agent_state waiting
    issue_labels "ready,needs-human-step"
    out="$(in_fleet notice_stalled 2>&1)"
    grep -q "nothing should be asking" <<<"$out" \
      && fail "a correct refusal to act alone was called a stall: $out"
    grep -qi "as expected" <<<"$out" \
      || fail "it did not say the wait was the expected one: $out"
    echo "ok: an issue whose last step is yours is reported as waiting for you"
    ;;
  stall_reports)
    make_fixture ok
    make_worktree
    agent_state waiting
    issue_labels "ready"
    out="$(in_fleet notice_stalled 2>&1)"
    grep -q "nothing should be asking" <<<"$out" \
      || fail "an ordinary agent sitting at a prompt was not reported: $out"
    echo "ok: an ordinary waiting agent is still a stall"
    ;;
  timebox_waits)
    make_fixture ok
    make_worktree
    make_overdue
    agent_state waiting
    issue_labels "ready,needs-human-step"
    out="$(in_fleet enforce_timebox 2>&1)"
    grep -q -- "--interrupt" "$ORCA_CALLS" \
      && fail "the time-box interrupted an agent that was correctly waiting: $out"
    grep -q "issue comment" "$GH_CALLS" \
      && fail "it told the issue the fleet gave up on work that is waiting on a person"
    grep -q "needs-human-step" <<<"$out" \
      || fail "the log does not say why it was left alone: $out"
    grep -q "waiting for you" "$ORCA_CALLS" \
      || fail "nothing reached the board, and one line in fleet.log is not the status surface"
    [ -e "$ROMMSYNC_FLEET_DIR/started/42" ] \
      || fail "the timer was disarmed for good, so removing the label leaves the agent uncapped"
    # Once per exemption, not once per poll: the dispatcher polls every minute.
    again="$(in_fleet enforce_timebox 2>&1)"
    grep -q "needs-human-step" <<<"$again" \
      && fail "it says so again every poll: $again"
    echo "ok: the time-box exempts an issue whose last step is yours"
    ;;
  timebox_stops)
    make_fixture ok
    make_worktree
    make_overdue
    agent_state working
    issue_labels "ready"
    out="$(in_fleet enforce_timebox 2>&1)"
    grep -q -- "--interrupt" "$ORCA_CALLS" \
      || fail "an ordinary overrun was not stopped: $out"
    grep -q "issue comment" "$GH_CALLS" \
      || fail "an ordinary overrun left nothing on the issue: $out"
    echo "ok: an ordinary overrun is still stopped"
    ;;
  queue_skips)
    make_fixture ok
    cat >"$GH_ISSUES" <<'JSON'
[{"number":148,"title":"cut the v1 release","body":"","labels":[{"name":"ready"},{"name":"needs-human-step"}]},
 {"number":151,"title":"an ordinary one","body":"","labels":[{"name":"ready"}]}]
JSON
    out="$(in_fleet ready_issues 2>&1)"
    grep -q "^151" <<<"$out" \
      || fail "an ordinary ready issue stopped being startable: $out"
    grep -q "^148" <<<"$out" \
      && fail "an issue no agent may close is still startable, so the fleet opens a worktree per cycle for it: $out"
    echo "ok: an issue whose last step is yours is not startable"
    ;;
  list_declines)
    make_fixture ok
    issue_labels "ready,needs-human-step"
    # `fleet.sh run 148` with nothing owned and nothing live: one pass, then it
    # runs out of queue. If the decline leaked into `wanted` instead of dropping
    # the issue, this would never return.
    # --max-prs 1 bounds it either way: if the decline ever stops working, the
    # run opens its one worktree and stops, and the assertion below fires --
    # rather than the test hanging, which is a much worse way to fail.
    out="$(in_fleet cmd_run --max-prs 1 148 2>&1)"
    grep -q "is labelled needs-human-step" <<<"$out" \
      || fail "an explicitly named issue whose last step is yours was not declined: $out"
    grep -q "worktree create" "$ORCA_CALLS" \
      && fail "it opened a worktree for work no agent may finish: $(cat "$ORCA_CALLS")"
    grep -q "fleet down" <<<"$out" \
      || fail "the run never terminated, so the declined issue stayed in the queue: $out"
    echo "ok: an explicitly named issue whose last step is yours is declined and dropped"
    ;;
  timebox_rearms)
    make_fixture ok
    make_worktree
    make_overdue
    agent_state waiting
    issue_labels "ready,needs-human-step"
    in_fleet enforce_timebox >/dev/null 2>&1
    # The maintainer takes the label off to hand the worktree back to an agent.
    issue_labels "ready"
    out="$(in_fleet enforce_timebox 2>&1)"
    grep -q -- "--interrupt" "$ORCA_CALLS" \
      || fail "the box stayed disarmed after the label came off, so the agent runs uncapped: $out"
    echo "ok: removing the label re-arms the time-box"
    ;;
  labels_unknown)
    make_fixture ok
    make_worktree
    make_overdue
    agent_state waiting
    issue_labels FAIL
    out="$(in_fleet enforce_timebox 2>&1)"
    grep -q -- "--interrupt" "$ORCA_CALLS" \
      && fail "an agent was stopped on a lookup that failed, not on an answer: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/started/42" ] \
      || fail "the timer was disarmed by a failed lookup, so the box never fires again"
    grep -q "could not read its labels" <<<"$out" \
      || fail "the outage was not reported: $out"
    out="$(in_fleet notice_stalled 2>&1)"
    grep -q "nothing should be asking" <<<"$out" \
      && fail "a failed lookup was frozen as a stall, which is the noise this change removes: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/stalled-42" ] \
      && fail "the once-per-stall marker was written on a non-answer, so it is never re-evaluated"
    echo "ok: a label lookup that failed stops nothing and decides nothing"
    ;;
  outage_once)
    make_fixture ok
    make_worktree
    make_overdue
    # `working`, not `waiting`: the ordinary state of a grinding overrun, and
    # the one that sends notice_stalled down its clearing branch.
    agent_state working
    issue_labels FAIL
    out="$(in_poll enforce_timebox notice_stalled 2>&1)"
    grep -q "could not read its labels" <<<"$out" \
      || fail "the outage was not reported at all: $out"
    again="$(in_poll enforce_timebox notice_stalled 2>&1)"
    grep -q "could not read its labels" <<<"$again" \
      && fail "it says so every poll: one watcher cleared the other watcher marker: $again"
    echo "ok: an unreadable label is reported once, not once a minute"
    ;;
  one_lookup)
    make_fixture ok
    make_worktree
    make_overdue
    agent_state waiting
    issue_labels "ready,needs-human-step"
    in_poll enforce_timebox notice_stalled >/dev/null 2>&1
    n="$(grep -c -- "--json labels" "$GH_CALLS")"
    [ "$n" = 1 ] \
      || fail "asked GitHub $n times for one issue labels in one poll"
    echo "ok: one poll asks for an issue labels once"
    ;;
  *)
    echo "usage: test_orca_fleet.sh card_says|card_quiet|remove_forces|remove_advice|stall_expected|stall_reports|timebox_waits|timebox_stops|queue_skips|list_declines|timebox_rearms|labels_unknown|outage_once|one_lookup" >&2
    exit 2 ;;
esac
