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
#                                     it by hand, BOTH halves and in order: the
#                                     directory first, because reap.sh alone
#                                     skips a worktree that is still there and
#                                     prints "nothing to reap"; then reap.sh,
#                                     because once the directory goes the next
#                                     pass disowns the issue and nothing ever
#                                     tears that stack down (#163).
#
# ...and the removal's ORDER, because "could not remove it" used to mean the
# stack had already been torn down anyway (#163): `--run-hooks` ran archive.sh
# BEFORE Orca refused the removal, so a worktree that was still there, still
# owned and still being worked in lost its RomM mid-ctest.
#
#   test_orca_fleet.sh remove_keeps_stack   the removal refuses -> the hook was
#                                           never asked for, the sweep never ran,
#                                           and it says the stack is still up.
#   test_orca_fleet.sh remove_sweeps_stack  it worked -> the stack is swept only
#                                           NOW. Dropping --run-hooks without
#                                           this would leak two ports and four
#                                           volumes per merged worktree.
#   test_orca_fleet.sh merged_keeps_dirty   reap_merged leaves a dirty worktree
#                                           and says what it holds, exactly as it
#                                           already does for unpushed commits --
#                                           #122's held two review fixes.
#   test_orca_fleet.sh merged_keeps_owned   a refused removal keeps the issue
#                                           OWNED and is said once, the way
#                                           reap_abandoned already does. Disowned,
#                                           nothing ever looks at that worktree
#                                           again.
#   test_orca_fleet.sh merged_unknown_git   ...and a git that cannot say what is
#                                           in there is "could not tell", not
#                                           "clean". A guard that fails open on
#                                           its own error is not a guard.
#   test_orca_fleet.sh merged_cli_silent    a CLI that never ANSWERED is not a
#                                           refusal: retried next pass rather
#                                           than parked, or Orca.app restarting
#                                           costs the fleet a slot for good.
#   test_orca_fleet.sh remove_scoped_sweep  the sweep names THIS worktree's stack.
#                                           Unscoped, releasing one worktree also
#                                           deletes the database of an orphan
#                                           somebody is still looking at.
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
#   test_orca_fleet.sh one_lookup     every watcher in one poll -> one `gh` call
#                                     for that issue's state and labels, not one
#                                     each.
#   test_orca_fleet.sh own_clears     a fresh worktree for an issue that had one
#                                     before starts with NO markers. They only
#                                     ever throttle a message to once, so an
#                                     inherited one silences the new worktree --
#                                     a `stalled-42` left behind makes
#                                     notice_stalled say nothing at all.
#   test_orca_fleet.sh timebox_clears both exits from enforce_timebox clear
#   test_orca_fleet.sh stop_clears    every marker it owns, not only the ones it
#                                     set on the way in.
#   test_orca_fleet.sh one_card       exempt, waiting and past the box -> ONE
#                                     board comment in the poll, not two, and it
#                                     still says both things a person needs.
#
# ...and the other half of the reap: a worktree whose issue will never produce a
# merged PR held one of three slots forever, because `reap_merged` is keyed on a
# PR that merged and nothing else ever removed one (#153).
#
#   test_orca_fleet.sh abandon_blocked      the issue went `blocked` -> released.
#   test_orca_fleet.sh abandon_closed       the issue closed with no merged PR
#                                           for this branch -> released.
#   test_orca_fleet.sh abandon_human_step   labelled `needs-human-step` -> released.
#                                           #139's agent correctly produced no PR
#                                           and stopped; reap_merged would wait
#                                           for a merged PR forever.
#   test_orca_fleet.sh abandon_keeps_dirty  ...unless the working tree is dirty,
#   test_orca_fleet.sh abandon_keeps_commits or carries commits that are not in
#                                           origin/main. Kept, and it SAYS what
#                                           is in there -- that pair is the whole
#                                           safety argument, verified by hand
#                                           before every removal on 2026-09-07.
#   test_orca_fleet.sh abandon_unknown_git  ...and a git that cannot answer is
#                                           "could not tell", not "nothing".
#   test_orca_fleet.sh abandon_leaves_working the control: an open issue with no
#                                           reason to release -> untouched. An
#                                           agent mid-task must not lose its
#                                           worktree.
#   test_orca_fleet.sh abandon_timebox      the box stopped the agent -> released,
#                                           and the record of it OUTLIVES the
#                                           worktree.
#   test_orca_fleet.sh gaveup_not_restarted ...so the freed slot does not go
#                                           straight back to the same three
#                                           hours, which is what releasing it
#                                           without the record would do.
#   test_orca_fleet.sh gaveup_retry         `fleet.sh retry 42` is how it comes
#                                           back, and it is the only way.
#   test_orca_fleet.sh abandon_warns_first  the first pass that finds a reason
#                                           WARNS and removes nothing. An agent
#                                           plans before it edits (CLAUDE.md), so
#                                           a worktree forty minutes into real
#                                           work is legitimately empty -- and
#                                           `blocked` is re-derived by
#                                           unblock.yml on every merge, so it
#                                           lands under one that is mid-plan.
#   test_orca_fleet.sh abandon_warned_saved ...and a minute is enough: something
#                                           committed after the warning keeps the
#                                           worktree.
#   test_orca_fleet.sh abandon_two_keeps    the two keeps do not share one
#                                           marker. A transient git failure must
#                                           not silence the line that says what
#                                           is actually in there.
#   test_orca_fleet.sh gaveup_pruned        the record is dropped once the issue
#                                           lands, or `fleet.sh status` lists
#                                           finished work forever and the files
#                                           never go away.
#   test_orca_fleet.sh list_says_declined   a run that declined every issue it was
#                                           given does not report that they landed.
#   test_orca_fleet.sh abandon_reason_flickers
#                                           the reason goes away and comes back ->
#                                           a FRESH pass of notice. unblock.yml
#                                           re-derives `blocked` on every merge,
#                                           so a label that flickers is the
#                                           ordinary case, and a warning spent on
#                                           the first occurrence must not be
#                                           inherited by the second -- that is a
#                                           worktree removed with no notice at all.
#   test_orca_fleet.sh abandon_lookup_blind a lookup that FAILED is not "no
#                                           reason". Folded into one, it wipes a
#                                           warning still owed to a reason nobody
#                                           could read, and the notice starts over
#                                           every time GitHub hiccups -- silently.
#
# ...and the one thing the dispatcher cannot re-read: itself. `fleet.sh run`
# parses its functions once, at start, so a fix merged to `main` is live in the
# worktree and NOT live in the dispatcher that is running -- for 27 hours, over
# four PRs, with nothing anywhere saying so (#173).
#
#   test_orca_fleet.sh status_stale       fleet.sh moved on since the dispatcher
#                                         started -> status says when it started,
#                                         NAMES the commits that are not live in
#                                         it, and says a restart is what fixes
#                                         it, the cap included.
#   test_orca_fleet.sh status_current     it is running the file on disk ->
#                                         still says when it started, and does
#                                         not cry stale. A warning every poll on
#                                         a current dispatcher teaches you to
#                                         ignore the one that matters.
#   test_orca_fleet.sh status_unrecorded  a dispatcher from before this check ->
#                                         "cannot say", not "current". A staleness
#                                         report that fails open is the silence
#                                         #173 already was.
#   test_orca_fleet.sh status_from_worktree
#                                         `status` run from a FLEET worktree,
#                                         branched before the fix, about the
#                                         dispatcher in the main one -> still
#                                         stale. This is the case that actually
#                                         happens: CLAUDE.md points agents in a
#                                         worktree at `fleet.sh status`, and
#                                         comparing the caller's own copy makes
#                                         two old files agree and reports
#                                         "current" -- #173 rebuilt inside the
#                                         check for it.
#   test_orca_fleet.sh status_draining    stopped but still up -> BOTH lines. A
#                                         drain leaves the dispatcher running on
#                                         purpose, and the code it is draining
#                                         with is the stale code.
#   test_orca_fleet.sh status_drained     ...and once it exits, `idle` prints
#                                         WHILE stopped. That line is what the
#                                         documented restart waits for, and the
#                                         stop file used to swallow it.
#   test_orca_fleet.sh status_behind      the checkout it started from never
#                                         pulled the fix -> BEHIND, naming it and
#                                         the pull. Nothing in the fleet updates
#                                         that checkout, so "the bytes on disk
#                                         are the bytes it parsed" is true of the
#                                         exact 27 hours #173 is about.
#   test_orca_fleet.sh status_names_root  the restart it prints names the
#                                         dispatcher's OWN checkout. This report
#                                         is read from a fleet worktree, and a
#                                         relative `run --auto` there starts a
#                                         dispatcher in a directory the fleet
#                                         removes when that PR merges.
#
# The Orca CLI and gh are stubbed on PATH; the fleet state dir is a temp dir.
# Nothing here touches a real worktree, docker, or GitHub.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() { echo "FAIL: $*" >&2; exit 1; }

WORK=""
cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; return 0; }
trap cleanup EXIT

# $1 is the CLI stub's mode: ok, set_fails, rm_needs_force, rm_never_works,
# rm_hangs.
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
    # A CLI that is there but never answers -- Orca.app restarting. The
    # dispatcher's deadline is what ends this, and the result is "could not ask",
    # which is not a decision about the worktree.
    [ "$mode" = rm_hangs ] && { sleep 30; exit 1; }
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
  *"pr list --head"*) cat "$GH_MERGED"; exit 0 ;;
  *"pr list"*)        cat "$GH_PRS"; exit 0 ;;
  *"issue list"*)     cat "$GH_ISSUES"; exit 0 ;;
  # gh applies --jq itself, so the stub answers what the filter would produce.
  # The literal FAIL stands for a gh that could not answer at all -- the third
  # answer the dispatcher is built around.
  #
  # State and labels come back from ONE call, so this branch has to sit above the
  # `--json state` one below: `--json state,labels` matches both patterns, and
  # the wrong one would answer a bare OPEN with no labels in it at all.
  *"issue view"*"--json state,labels"*)
    [ "$(cat "$GH_LABELS")" = FAIL ] && { echo "gh: could not connect" >&2; exit 1; }
    printf '%s\t%s\n' "$(cat "$GH_STATE")" "$(cat "$GH_LABELS")"; exit 0 ;;
  *"issue view"*"--json state"*) cat "$GH_STATE"; exit 0 ;;
  *"issue comment"*)  exit 0 ;;
esac
echo ""
STUB
  chmod +x "$WORK/bin/gh"

  # cmd_run ends in notify(); a test suite must not put banners on the screen.
  printf '#!/usr/bin/env bash\nexit 0\n' >"$WORK/bin/osascript"
  chmod +x "$WORK/bin/osascript"

  # The sweep the removal now runs ITSELF, because it no longer asks the Orca CLI
  # to run the archive hook (#163). Stubbed rather than real: the real one talks
  # to docker, and what these tests are about is WHEN it is called, not what it
  # tears down.
  cat >"$WORK/repo/scripts/orca/reap.sh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$REAP_CALLS"
exit 0
STUB
  chmod +x "$WORK/repo/scripts/orca/reap.sh"

  ORCA_CALLS="$WORK/calls"; : >"$ORCA_CALLS"
  REAP_CALLS="$WORK/reap-calls"; : >"$REAP_CALLS"
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
  GH_STATE="$WORK/state";           echo OPEN >"$GH_STATE"
  # What `gh pr list --head <branch> --state merged` finds. `7` is the answer
  # reap_merged acts on, and the default the removal tests are written against;
  # a test about the OTHER reap empties it, or reap_merged gets there first.
  GH_MERGED="$WORK/merged";         echo 7 >"$GH_MERGED"
  WORK_FOR_STUB="$WORK"; mkdir -p "$WORK/created"
  export ORCA_CALLS ORCA_MODE GH_CALLS ORCA_PS ORCA_WORKTREES ORCA_TERMINALS \
         GH_PRS GH_ISSUES GH_LABELS GH_STATE GH_MERGED WORK_FOR_STUB REAP_CALLS
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

# The two halves of what one `gh issue view N --json state,labels` would print.
issue_labels() { printf '%s' "$1" >"$GH_LABELS"; }
issue_state()  { printf '%s' "$1" >"$GH_STATE"; }

# An `origin` whose `main` is this worktree's HEAD, and a `work` branch tracking
# it. The release check asks what the worktree holds that origin/main does not,
# so a worktree with no origin at all is the "could not tell" case rather than
# the empty one -- which is why every test that expects a removal sets this up.
add_origin() {
  git init -q --bare "$WORK/origin.git"
  git -C "$WORK/wt" remote add origin "$WORK/origin.git"
  git -C "$WORK/wt" push -q origin work:main
  git -C "$WORK/wt" push -q -u origin work
}

# Nothing this dispatcher may throw away: an untracked file, or a commit that is
# nowhere but here.
dirty_worktree()  { echo scratch >"$WORK/wt/notes.txt"; }
commit_ahead()    { git -C "$WORK/wt" -c user.email=t@t -c user.name=t \
                        commit -q --allow-empty -m "work in progress"; }

# No reason to release, and the reap must find none: an open issue, no merged PR
# for the branch, and nothing the fleet has given up on.
quiet_issue() { issue_state OPEN; issue_labels "ready"; : >"$GH_MERGED"; }

# A release takes TWO passes on purpose: the first warns and interrupts, the
# second re-asks what the worktree holds and only then removes it. Tests about
# the removal drive both; tests about the warning, or about a worktree that is
# kept, drive one.
release_pass()      { in_fleet reap_abandoned 2>&1; }
warn_then_release() { release_pass >/dev/null 2>&1; release_pass; }

# An issue that is past its box with no PR open: the started marker is old, and
# the PR listing is empty.
make_overdue() { mkdir -p "$ROMMSYNC_FLEET_DIR/started"; echo 0 >"$ROMMSYNC_FLEET_DIR/started/42"; }

# All three markers enforce_timebox owns, set the way the dispatcher sets them
# -- by driving it through the polls that write each one. Setting them by hand
# would assert against a state the code may never produce.
BOX_MARKERS="unreachable box-labels human-step"
# The order is forced: the exemption branch clears `unreachable-` and
# `box-labels-` on its way past, so `human-step-` has to be armed before them.
arm_box_markers() {
  # No PR, and the label says the last step is a person's -> `human-step-`.
  echo '[]' >"$GH_PRS"; issue_labels "ready,needs-human-step"
  in_fleet enforce_timebox >/dev/null 2>&1
  # A PR lookup that cannot answer -> `unreachable-`.
  echo 'not json' >"$GH_PRS"
  in_fleet enforce_timebox >/dev/null 2>&1
  # It answers again, and now the LABEL lookup cannot -> `box-labels-`.
  echo '[]' >"$GH_PRS"; issue_labels FAIL
  in_fleet enforce_timebox >/dev/null 2>&1
  local m
  for m in $BOX_MARKERS; do
    [ -e "$ROMMSYNC_FLEET_DIR/$m-42" ] \
      || fail "could not arm $m-42, so the assertion that follows would be vacuous"
  done
}
assert_no_box_markers() {
  local m
  for m in $BOX_MARKERS; do
    [ -e "$ROMMSYNC_FLEET_DIR/$m-42" ] \
      && fail "$m-42 survives $1, and it silences the next worktree for this issue"
  done
  return 0
}

# fleet.sh returns instead of dispatching when it is sourced, so one function can
# be exercised without starting a dispatcher.
in_fleet() { (cd "$WORK/repo" && . ./scripts/orca/fleet.sh && "$@"); }

# Both watchers in ONE process, which is what a real poll is: they share the
# per-poll answer cache and the state dir, and only there can one of them undo
# what the other just wrote.
in_poll() { (cd "$WORK/repo" && . ./scripts/orca/fleet.sh; for fn in "$@"; do "$fn"; done); }

# The fixture repo under git, because "which fixes are not live in the running
# dispatcher" is answered in commits and cannot be faked with a hash alone.
make_repo_git() {
  git -C "$WORK/repo" init -q -b main
  git -C "$WORK/repo" -c user.email=t@t -c user.name=t add -A
  git -C "$WORK/repo" -c user.email=t@t -c user.name=t commit -q -m "the fleet as the dispatcher parsed it"
}

# A commit that changes fleet.sh under a dispatcher that is already up. A
# trailing comment, so the file the test itself sources still behaves.
merge_fleet_fix() {
  printf '# %s\n' "$1" >>"$WORK/repo/scripts/orca/fleet.sh"
  git -C "$WORK/repo" -c user.email=t@t -c user.name=t commit -q -am "$1"
}

# A dispatcher this shell can answer `kill -0` for.
dispatcher_running() { mkdir -p "$ROMMSYNC_FLEET_DIR"; echo $$ >"$ROMMSYNC_FLEET_DIR/fleet.pid"; }

# fleet.sh sourced from somewhere OTHER than the dispatcher's own checkout --
# what an agent in a fleet worktree runs.
in_fleet_at() { local at="$1"; shift; (cd "$at" && . ./scripts/orca/fleet.sh && "$@"); }

# A second checkout of the fixture repo, pinned to the commit the dispatcher
# started from: a worktree branched before the fix landed.
older_checkout() {
  git -C "$WORK/repo" worktree add -q --detach "$WORK/wt2" "$1"
}

# The fix merged and pushed, with the dispatcher's own checkout left exactly
# where it was: `origin/main` moves, the file on disk does not. Nothing in the
# fleet pulls it, so this is what a merge during a run actually looks like.
merged_but_not_pulled() {
  local parked; parked="$(git -C "$WORK/repo" rev-parse HEAD)"
  git -C "$WORK/repo" init -q --bare "$WORK/repo-origin.git" 2>/dev/null
  git -C "$WORK/repo" remote add origin "$WORK/repo-origin.git" 2>/dev/null
  git -C "$WORK/repo" push -q origin HEAD:main
  merge_fleet_fix "$1"
  git -C "$WORK/repo" push -q origin HEAD:main
  git -C "$WORK/repo" reset -q --hard "$parked"
}

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
    grep -q "git worktree remove --force.*&&.*reap.sh --yes" <<<"$out" \
      || fail "the advice is not both halves in order -- reap.sh alone skips a worktree that is still there, and the directory alone leaks its stack: $out"
    grep -q "containing submodules" <<<"$out" \
      || fail "the reason git gave was dropped; it is the difference between this and a hung CLI: $out"
    echo "ok: a failed removal says something that would actually clean it up"
    ;;
  remove_keeps_stack)
    make_fixture rm_never_works
    make_worktree
    out="$(in_fleet remove_worktree "$WORK/wt" 2>&1)"
    grep -q -- "--run-hooks" "$ORCA_CALLS" \
      && fail "the removal still asks Orca to run the archive hook, so a refusal tears the stack down anyway: $(cat "$ORCA_CALLS")"
    [ -s "$REAP_CALLS" ] \
      && fail "it swept the stack of a worktree it did not remove: $(cat "$REAP_CALLS")"
    grep -q "still up" <<<"$out" \
      || fail "it did not say the stack survived, which is the whole difference: $out"
    echo "ok: a refused removal leaves that worktree's stack running"
    ;;
  remove_sweeps_stack)
    make_fixture rm_needs_force
    make_worktree
    out="$(in_fleet remove_worktree "$WORK/wt" 2>&1)" \
      || fail "remove_worktree gave up on a worktree --force would have removed: $out"
    [ -d "$WORK/wt" ] && fail "it reported success and the worktree is still there"
    grep -q -- "--yes" "$REAP_CALLS" \
      || fail "the stack was never swept, so every merged worktree leaks it: $(cat "$REAP_CALLS")"
    echo "ok: the stack is torn down once the worktree is actually gone"
    ;;
  merged_keeps_dirty)
    make_fixture ok
    make_worktree
    add_origin
    dirty_worktree
    out="$(in_fleet reap_merged 2>&1)"
    grep -q "worktree rm" "$ORCA_CALLS" \
      && fail "a dirty worktree reached the removal: $(cat "$ORCA_CALLS")"
    [ -d "$WORK/wt" ] || fail "it removed a worktree holding uncommitted work: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/worktrees/42" ] \
      || fail "it disowned a worktree it kept, so nothing looks at it again: $out"
    grep -q "uncommitted" <<<"$out" \
      || fail "it did not say WHAT the worktree holds: $out"
    # Once per worktree, not once per poll: the dispatcher polls every minute.
    again="$(in_fleet reap_merged 2>&1)"
    grep -q "uncommitted" <<<"$again" && fail "it says so every poll: $again"
    echo "ok: reap_merged leaves a dirty worktree and says what is in it"
    ;;
  merged_unknown_git)
    make_fixture ok
    make_worktree
    add_origin
    # HEAD still reads, so the branch and the merged-PR lookup both answer; the
    # index does not, so `git status` cannot say whether anything is uncommitted.
    printf 'not an index' >"$WORK/wt/.git/index"
    out="$(in_fleet reap_merged 2>&1)"
    grep -q "worktree rm" "$ORCA_CALLS" \
      && fail "it removed a worktree on a git that never answered: $(cat "$ORCA_CALLS")"
    [ -d "$WORK/wt" ] || fail "the worktree is gone: $out"
    grep -q "could not say" <<<"$out" \
      || fail "it treated a git error as a clean working tree, in silence: $out"
    again="$(in_fleet reap_merged 2>&1)"
    grep -q "could not say" <<<"$again" && fail "it says so every poll: $again"
    echo "ok: a git that cannot answer keeps the worktree"
    ;;
  merged_cli_silent)
    make_fixture rm_hangs
    make_worktree
    add_origin
    # The dispatcher's own deadline, shortened so the phase fits its 60s timeout.
    out="$(ROMMSYNC_FLEET_RM_DEADLINE=1 in_fleet reap_merged 2>&1)"
    [ -d "$WORK/wt" ] || fail "the worktree went on a call that never answered: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/stuck-42" ] \
      && fail "a CLI that never answered was recorded as a refusal, so the slot is held for good: $out"
    grep -q "did not answer" <<<"$out" || fail "it did not say what actually happened: $out"
    # ONE attempt, not two: a second 180s proving nothing answers doubles what a
    # poll costs while Orca.app restarts.
    [ "$(grep -c "worktree rm" "$ORCA_CALLS")" = 1 ] \
      || fail "it spent a second deadline on a CLI that had already timed out: $(cat "$ORCA_CALLS")"
    # The COUNT, not its presence: $ORCA_CALLS is never truncated between the two
    # passes, so `grep -q` would find the first call's line and pass even if the
    # second pass made no call at all -- which is the regression this pins.
    again="$(ROMMSYNC_FLEET_RM_DEADLINE=1 in_fleet reap_merged 2>&1)"
    [ "$(grep -c "worktree rm" "$ORCA_CALLS")" = 2 ] \
      || fail "the next pass did not retry, so an Orca restart costs the slot permanently: $again"
    echo "ok: a CLI that never answered is retried, not parked"
    ;;
  remove_scoped_sweep)
    make_fixture rm_needs_force
    make_worktree
    # The name the stack was really created under, which after a directory rename
    # is NOT the one the path derives -- archive.sh removed both, so must this.
    echo "COMPOSE_PROJECT_NAME=rmx-renamed-1" >"$WORK/wt/.env"
    in_fleet remove_worktree "$WORK/wt" >/dev/null 2>&1 \
      || fail "the worktree was not removed, so the sweep assertion is vacuous"
    grep -q -- "--only" "$REAP_CALLS" \
      || fail "the sweep was unscoped, so releasing one worktree reaps every orphan on the machine: $(cat "$REAP_CALLS")"
    grep -q -- "--only rmx-renamed-1" "$REAP_CALLS" \
      || fail "the name the stack was actually created under was not swept: $(cat "$REAP_CALLS")"
    echo "ok: the sweep names the stack of the worktree it removed"
    ;;
  merged_keeps_owned)
    make_fixture rm_never_works
    make_worktree
    add_origin
    out="$(in_fleet reap_merged 2>&1)"
    grep -q "could not remove it" <<<"$out" \
      || fail "a failed removal was not reported: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/worktrees/42" ] \
      || fail "the issue was disowned on a removal that refused, so that worktree is one nothing ever looks at again: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/stuck-42" ] \
      || fail "nothing recorded the refusal, so the next poll retries it: $out"
    again="$(in_fleet reap_merged 2>&1)"
    grep -q "could not remove it" <<<"$again" \
      && fail "it says so again every poll, and retries the removal with it: $again"
    echo "ok: a refused removal keeps its issue owned, and is said once"
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
    add_origin
    make_overdue
    agent_state waiting
    issue_labels "ready,needs-human-step"
    : >"$GH_MERGED"
    # All THREE watchers, in one process, which is what a poll is. reap_abandoned
    # asks the same question as the other two, so a third round-trip is exactly
    # the drift the shared answer exists to stop.
    in_poll reap_abandoned enforce_timebox notice_stalled >/dev/null 2>&1
    n="$(grep -c -- "--json state,labels" "$GH_CALLS")"
    [ "$n" = 1 ] \
      || fail "asked GitHub $n times for one issue's state and labels in one poll"
    echo "ok: one poll asks for an issue's state and labels once"
    ;;
  timebox_clears)
    make_fixture ok
    make_worktree
    make_overdue
    agent_state working
    arm_box_markers
    # A PR opens: the time-box is done with this worktree.
    echo '[{"number":9,"body":"Closes #42"}]' >"$GH_PRS"
    in_fleet enforce_timebox >/dev/null 2>&1
    assert_no_box_markers "the PR-opened exit"
    echo "ok: the time-box leaves nothing behind when it lets an issue go"
    ;;
  stop_clears)
    make_fixture ok
    make_worktree
    make_overdue
    agent_state working
    arm_box_markers
    # Nothing exempts it any more, so this pass takes the stop.
    echo '[]' >"$GH_PRS"; issue_labels "ready"
    out="$(in_fleet enforce_timebox 2>&1)"
    grep -q -- "--interrupt" "$ORCA_CALLS" \
      || fail "it did not reach the stop, so this asserts nothing: $out"
    assert_no_box_markers "the stop"
    echo "ok: the stop leaves nothing behind either"
    ;;
  own_clears)
    make_fixture ok
    make_worktree
    make_overdue
    agent_state working
    # Everything a previous worktree for this issue leaves standing, armed by
    # driving the dispatcher through the poll that writes each one. `stalled-`
    # and `stall-labels-` are alternatives -- notice_stalled clears one when it
    # writes the other -- so they need two arrangements, not one.
    arm_box_markers
    agent_state waiting; issue_labels "ready"
    in_fleet notice_stalled >/dev/null 2>&1
    for m in $BOX_MARKERS stalled; do
      [ -e "$ROMMSYNC_FLEET_DIR/$m-42" ] \
        || fail "could not arm $m-42, so the assertion that follows would be vacuous"
    done
    # `live_worktrees` skips an ARCHIVED worktree, so `in_flight` reads free and
    # the dispatcher opens a SECOND worktree for an issue whose markers are all
    # still set. Each of them throttles a message to once, so the new worktree
    # inherits silence: a standing `stalled-42` makes notice_stalled say nothing
    # at all for an agent that is genuinely stuck.
    in_fleet own 42 "$WORK/wt" >/dev/null 2>&1
    for m in $BOX_MARKERS stalled; do
      [ -e "$ROMMSYNC_FLEET_DIR/$m-42" ] \
        && fail "$m-42 survived into a fresh worktree, which is silenced by it"
    done
    # ...and the other half of that pair.
    issue_labels FAIL
    in_fleet notice_stalled >/dev/null 2>&1
    [ -e "$ROMMSYNC_FLEET_DIR/stall-labels-42" ] \
      || fail "could not arm stall-labels-42, so the assertion that follows would be vacuous"
    in_fleet own 42 "$WORK/wt" >/dev/null 2>&1
    [ -e "$ROMMSYNC_FLEET_DIR/stall-labels-42" ] \
      && fail "stall-labels-42 survived into a fresh worktree, which is silenced by it"
    echo "ok: a fresh worktree starts with nothing already said on its behalf"
    ;;
  one_card)
    make_fixture ok
    make_worktree
    make_overdue
    agent_state waiting
    issue_labels "ready,needs-human-step"
    in_poll enforce_timebox notice_stalled >/dev/null 2>&1
    n="$(grep -c "waiting for you" "$ORCA_CALLS")"
    [ "$n" = 1 ] \
      || fail "put $n near-duplicate comments on the board in one poll"
    grep -q "past the time-box" "$ORCA_CALLS" \
      || fail "the comment that survived does not say it is past the box: $(cat "$ORCA_CALLS")"
    grep -q "not a stall" "$ORCA_CALLS" \
      || fail "the comment that survived does not say it is not a stall: $(cat "$ORCA_CALLS")"
    echo "ok: one poll leaves one board comment, and it says both things"
    ;;
  abandon_blocked)
    make_fixture ok
    make_worktree
    add_origin
    quiet_issue
    issue_labels "blocked"
    out="$(warn_then_release)"
    [ -d "$WORK/wt" ] && fail "a blocked issue's clean worktree still holds a slot: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/worktrees/42" ] \
      && fail "the worktree is gone but the issue is still owned, so the cap still counts it"
    grep -q "went blocked" <<<"$out" || fail "it did not say why it released it: $out"
    echo "ok: a worktree whose issue went blocked is released"
    ;;
  abandon_closed)
    make_fixture ok
    make_worktree
    add_origin
    quiet_issue
    issue_state CLOSED
    out="$(warn_then_release)"
    [ -d "$WORK/wt" ] && fail "a closed issue's clean worktree still holds a slot: $out"
    grep -q "closed" <<<"$out" || fail "it did not say why it released it: $out"
    echo "ok: a worktree whose issue closed with no merged PR is released"
    ;;
  abandon_human_step)
    make_fixture ok
    make_worktree
    add_origin
    quiet_issue
    # #139: the write its issue needs is one guard.py refuses from a fleet
    # worktree, so its agent correctly produced no PR, labelled the issue and
    # stopped. reap_merged waits for a merged PR that can never exist.
    issue_labels "ready,needs-human-step"
    out="$(warn_then_release)"
    [ -d "$WORK/wt" ] && fail "an issue no agent may close still holds a slot: $out"
    grep -q "needs-human-step" <<<"$out" || fail "it did not say why it released it: $out"
    echo "ok: a worktree whose last step is yours is released once it holds nothing"
    ;;
  abandon_keeps_dirty)
    make_fixture ok
    make_worktree
    add_origin
    quiet_issue
    issue_labels "blocked"
    dirty_worktree
    out="$(release_pass)"
    [ -d "$WORK/wt" ] || fail "it deleted uncommitted work on the strength of a label: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/worktrees/42" ] \
      || fail "it kept the directory but disowned the issue, so nothing looks at it again"
    grep -q "uncommitted" <<<"$out" || fail "it did not say what is in there: $out"
    grep -q "worktree rm" "$ORCA_CALLS" \
      && fail "it attempted the removal anyway, on a worktree holding uncommitted work"
    # Once per worktree, not once per poll.
    again="$(release_pass)"
    grep -q "uncommitted" <<<"$again" && fail "it says so every poll: $again"
    echo "ok: a dirty worktree is kept, and it says what it holds"
    ;;
  abandon_keeps_commits)
    make_fixture ok
    make_worktree
    add_origin
    quiet_issue
    issue_labels "blocked"
    commit_ahead
    out="$(release_pass)"
    [ -d "$WORK/wt" ] || fail "it deleted the only copy of a commit: $out"
    grep -q "not in origin/main" <<<"$out" || fail "it did not say what is in there: $out"
    grep -q "worktree rm" "$ORCA_CALLS" && fail "it attempted the removal anyway: $out"
    echo "ok: a worktree carrying commits that are nowhere else is kept"
    ;;
  abandon_unknown_git)
    make_fixture ok
    make_worktree
    quiet_issue
    issue_labels "blocked"
    # No origin at all, so there is nothing to compare against. That is the third
    # answer, and reading it as "holds nothing" is how a fix like this destroys
    # the work it was written to protect.
    out="$(release_pass)"
    [ -d "$WORK/wt" ] || fail "it removed a worktree it could not read: $out"
    grep -q "could not be read" <<<"$out" || fail "it did not say it could not tell: $out"
    echo "ok: a git that cannot answer is not read as an empty worktree"
    ;;
  abandon_leaves_working)
    make_fixture ok
    make_worktree
    add_origin
    quiet_issue
    agent_state working
    out="$(release_pass)"
    [ -d "$WORK/wt" ] || fail "it removed the worktree of an agent that is still working: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/worktrees/42" ] \
      || fail "it disowned an issue that is still being worked, so the cap stops counting it"
    grep -q "worktree rm" "$ORCA_CALLS" && fail "it attempted a removal with no reason to: $out"
    [ -n "$out" ] && fail "it had nothing to say and said it anyway: $out"
    echo "ok: an ordinary in-flight worktree is left alone"
    ;;
  abandon_timebox)
    make_fixture ok
    make_worktree
    add_origin
    make_overdue
    quiet_issue
    agent_state working
    in_fleet enforce_timebox >/dev/null 2>&1
    [ -e "$ROMMSYNC_FLEET_DIR/gaveup-42" ] \
      || fail "the box stopped the agent without recording it, so nothing else can act on it"
    out="$(warn_then_release)"
    [ -d "$WORK/wt" ] && fail "#44's case again: three hours, nothing produced, slot held: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/gaveup-42" ] \
      || fail "the record died with the worktree, so the queue hands the issue straight back"
    echo "ok: a timed-out worktree holding nothing is released, and the record outlives it"
    ;;
  gaveup_not_restarted)
    make_fixture ok
    make_worktree
    add_origin
    make_overdue
    quiet_issue
    agent_state working
    cat >"$GH_ISSUES" <<'JSON'
[{"number":42,"title":"the one that ground for three hours","body":"","labels":[{"name":"ready"}]}]
JSON
    in_fleet enforce_timebox >/dev/null 2>&1
    # --max-prs 1 bounds this either way: if the decline stops working the run
    # opens its one worktree and stops, and the assertion below fires -- rather
    # than the test hanging, which is a much worse way to fail.
    out="$(in_fleet cmd_run --auto --max-prs 1 2>&1)"
    grep -q "worktree create" "$ORCA_CALLS" \
      && fail "it released the slot and handed it straight back to the same issue: $(cat "$ORCA_CALLS")"
    [ -d "$WORK/wt" ] \
      && fail "the poll never released the worktree, so the decline below asserts nothing"
    grep -q "fleet down" <<<"$out" \
      || fail "the run never terminated: a gave-up issue is still being counted as startable: $out"
    echo "ok: the slot a gave-up issue frees does not go straight back to it"
    ;;
  gaveup_retry)
    make_fixture ok
    make_worktree
    add_origin
    make_overdue
    quiet_issue
    agent_state working
    in_fleet enforce_timebox >/dev/null 2>&1
    [ -e "$ROMMSYNC_FLEET_DIR/gaveup-42" ] \
      || fail "could not arm gaveup-42, so the assertion that follows would be vacuous"
    out="$(in_fleet cmd_retry 42 2>&1)"
    [ -e "$ROMMSYNC_FLEET_DIR/gaveup-42" ] \
      && fail "retry left the record standing, so the issue is still declined: $out"
    grep -q "startable again" <<<"$out" || fail "retry said nothing useful: $out"
    echo "ok: fleet.sh retry hands a gave-up issue back to the queue"
    ;;
  abandon_warns_first)
    make_fixture ok
    make_worktree
    add_origin
    quiet_issue
    issue_labels "blocked"
    agent_state working
    out="$(release_pass)"
    [ -d "$WORK/wt" ] \
      || fail "it removed the worktree on the pass that found the reason, with no notice: $out"
    grep -q "next pass" <<<"$out" || fail "it did not say what happens next: $out"
    grep -q -- "--interrupt" "$ORCA_CALLS" \
      || fail "it warned the board and left the agent working against a rig that is going"
    grep -q "worktree rm" "$ORCA_CALLS" && fail "it removed it anyway: $(cat "$ORCA_CALLS")"
    echo "ok: the pass that finds a reason warns, and removes nothing"
    ;;
  abandon_warned_saved)
    make_fixture ok
    make_worktree
    add_origin
    quiet_issue
    issue_labels "blocked"
    release_pass >/dev/null 2>&1
    # The minute of notice is only worth having if it is really re-asked.
    commit_ahead
    out="$(release_pass)"
    [ -d "$WORK/wt" ] \
      || fail "it removed a worktree that had work in it by the time it acted: $out"
    grep -q "not in origin/main" <<<"$out" || fail "it did not say what saved it: $out"
    echo "ok: work that lands during the warning keeps the worktree"
    ;;
  abandon_two_keeps)
    make_fixture ok
    make_worktree
    quiet_issue
    issue_labels "blocked"
    # No origin yet: git cannot answer, and that is said once.
    first="$(release_pass)"
    grep -q "could not be read" <<<"$first" || fail "the unreadable case was not reported: $first"
    # Now it can answer, and there is something in there. One shared marker would
    # swallow this, and the board would go on claiming git was unreadable.
    add_origin
    dirty_worktree
    out="$(release_pass)"
    grep -q "uncommitted" <<<"$out" \
      || fail "the second keep was silenced by the first one's marker: $out"
    echo "ok: an unreadable git does not silence the line that says what is in there"
    ;;
  gaveup_pruned)
    make_fixture ok
    make_worktree
    add_origin
    make_overdue
    quiet_issue
    agent_state working
    in_fleet enforce_timebox >/dev/null 2>&1
    [ -e "$ROMMSYNC_FLEET_DIR/gaveup-42" ] \
      || fail "could not arm gaveup-42, so the assertion that follows would be vacuous"
    # A person picks it up by hand and its PR lands.
    issue_state CLOSED
    out="$(in_fleet prune_gaveup 2>&1)"
    [ -e "$ROMMSYNC_FLEET_DIR/gaveup-42" ] \
      && fail "the record outlived the work: status lists a finished issue forever, and the file never goes"
    grep -q "dropping the record" <<<"$out" || fail "it dropped it silently: $out"
    echo "ok: a give-up record is dropped once its issue lands"
    ;;
  list_says_declined)
    make_fixture ok
    issue_labels "ready,needs-human-step"
    out="$(in_fleet cmd_run --max-prs 1 148 2>&1)"
    grep -q "fleet down" <<<"$out" || fail "the run never terminated: $out"
    grep -q "every issue it was given has landed$" <<<"$out" \
      && fail "it reported an issue it declined as landed: $out"
    grep -q "declined" <<<"$out" || fail "the last line does not say what really happened: $out"
    echo "ok: a run that declined its issues does not report that they landed"
    ;;
  abandon_reason_flickers)
    make_fixture ok
    make_worktree
    add_origin
    quiet_issue
    agent_state working
    # 1. It goes blocked, and the first pass warns.
    issue_labels "blocked"
    release_pass >/dev/null 2>&1
    [ -e "$ROMMSYNC_FLEET_DIR/warned-42" ] \
      || fail "could not arm warned-42, so the assertion that follows would be vacuous"
    # 2. unblock.yml relabels it on somebody else's merge and the reason is gone.
    issue_labels "ready"
    out="$(release_pass)"
    [ -n "$out" ] && fail "it had nothing to say about a worktree with no reason to release: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/warned-42" ] \
      && fail "the spent warning outlived the reason that bought it"
    # 3. A second dependency is added and it goes blocked again. This pass must
    #    warn, not remove: the notice is per occasion, not once per worktree.
    issue_labels "blocked"
    out="$(release_pass)"
    [ -d "$WORK/wt" ] \
      || fail "it removed the worktree with no notice at all, on a warning spent for an earlier occurrence: $out"
    grep -q "next pass" <<<"$out" || fail "it did not warn the second time: $out"
    # 4. ...and then it goes, so this is a delay rather than a deadlock.
    out="$(release_pass)"
    [ -d "$WORK/wt" ] && fail "the second warning never turned into a release: $out"
    echo "ok: a reason that comes back buys a fresh pass of notice"
    ;;
  abandon_lookup_blind)
    make_fixture ok
    make_worktree
    add_origin
    quiet_issue
    agent_state working
    # 1. Blocked, and warned.
    issue_labels "blocked"
    release_pass >/dev/null 2>&1
    [ -e "$ROMMSYNC_FLEET_DIR/warned-42" ] \
      || fail "could not arm warned-42, so the assertion that follows would be vacuous"
    # 2. `gh` cannot answer. That is the third answer, not "it stopped being
    #    blocked", and the difference is a warning that is still owed.
    issue_labels FAIL
    out="$(release_pass)"
    [ -e "$ROMMSYNC_FLEET_DIR/warned-42" ] \
      || fail "a failed lookup wiped a warning still owed to a reason nobody could read: $out"
    grep -q "could not read" <<<"$out" || fail "it discarded the pass in silence: $out"
    [ -d "$WORK/wt" ] || fail "it removed the worktree on a lookup that never answered: $out"
    # 3. The outage lasts. Once per outage, not once per poll.
    again="$(release_pass)"
    grep -q "could not read" <<<"$again" && fail "it says so every poll: $again"
    # 4. It ends, still blocked. The warning was never lost, so this pass
    #    releases rather than starting the notice over.
    issue_labels "blocked"
    out="$(release_pass)"
    [ -d "$WORK/wt" ] \
      && fail "the outage restarted the notice; under a flaky gh the release never happens: $out"
    echo "ok: a lookup that failed leaves the markers, and the notice, where they were"
    ;;
  status_stale)
    make_fixture ok
    make_repo_git
    dispatcher_running
    # What a real dispatcher records at start: the fleet.sh it actually parsed.
    in_fleet record_dispatcher
    merge_fleet_fix "a fix the running dispatcher never parsed"
    out="$(in_fleet cmd_status 2>&1)"
    grep -q "up since" <<<"$out" \
      || fail "status does not say when the dispatcher started: $out"
    grep -qi "not live" <<<"$out" \
      || fail "a dispatcher older than fleet.sh was not reported as stale: $out"
    grep -q "a fix the running dispatcher never parsed" <<<"$out" \
      || fail "it did not name the commit that is missing from it: $out"
    grep -qi "restart" <<<"$out" \
      || fail "it did not say a restart is what makes the fix live: $out"
    grep -q "ROMMSYNC_FLEET_MAX" <<<"$out" \
      || fail "it did not say the cap is read at start too, so it changes only across a restart: $out"
    echo "ok: a dispatcher older than fleet.sh is reported stale, by commit"
    ;;
  status_current)
    make_fixture ok
    make_repo_git
    dispatcher_running
    in_fleet record_dispatcher
    out="$(in_fleet cmd_status 2>&1)"
    grep -q "up since" <<<"$out" \
      || fail "status does not say when the dispatcher started: $out"
    grep -qi "not live" <<<"$out" \
      && fail "a dispatcher running the file on disk was reported stale: $out"
    echo "ok: a current dispatcher says when it started and nothing more"
    ;;
  status_unrecorded)
    make_fixture ok
    make_repo_git
    dispatcher_running
    # No record at all -- a dispatcher started before this check existed.
    out="$(in_fleet cmd_status 2>&1)"
    grep -qi "cannot say" <<<"$out" \
      || fail "a dispatcher whose fleet.sh nothing recorded was not reported as unknown: $out"
    grep -qi "restart" <<<"$out" \
      || fail "it did not say what to do about it: $out"
    echo "ok: a dispatcher that recorded nothing is 'cannot say', not 'current'"
    ;;
  status_from_worktree)
    make_fixture ok
    make_repo_git
    dispatcher_running
    in_fleet record_dispatcher
    parked="$(git -C "$WORK/repo" rev-parse HEAD)"
    merge_fleet_fix "a fix the running dispatcher never parsed"
    # The agent's worktree still holds exactly the bytes the dispatcher parsed,
    # so a check that hashed the CALLER's copy would find them equal and say the
    # dispatcher is current.
    older_checkout "$parked"
    out="$(in_fleet_at "$WORK/wt2" cmd_status 2>&1)"
    grep -qi "not live" <<<"$out" \
      || fail "asked from a worktree branched before the fix, it called a stale dispatcher current: $out"
    grep -q "a fix the running dispatcher never parsed" <<<"$out" \
      || fail "it did not name the commit that is missing from the dispatcher: $out"
    echo "ok: staleness is about the dispatcher's checkout, not the caller's"
    ;;
  status_draining)
    make_fixture ok
    make_repo_git
    dispatcher_running
    in_fleet record_dispatcher
    merge_fleet_fix "a fix the running dispatcher never parsed"
    : >"$ROMMSYNC_FLEET_DIR/STOP"
    out="$(in_fleet cmd_status 2>&1)"
    grep -q "STOPPED" <<<"$out" || fail "a stop stopped being reported: $out"
    grep -qi "not live" <<<"$out" \
      || fail "a draining dispatcher hid its staleness behind the stop, and a drain is exactly when it keeps running: $out"
    echo "ok: stopped and still up says both"
    ;;
  status_drained)
    make_fixture ok
    make_repo_git
    mkdir -p "$ROMMSYNC_FLEET_DIR"; : >"$ROMMSYNC_FLEET_DIR/STOP"
    out="$(in_fleet cmd_status 2>&1)"
    grep -q "STOPPED" <<<"$out" || fail "a stop stopped being reported: $out"
    grep -q "idle" <<<"$out" \
      || fail "the stop swallowed the line the documented restart waits for: $out"
    echo "ok: a drained fleet says idle while stopped"
    ;;
  status_behind)
    make_fixture ok
    make_repo_git
    dispatcher_running
    in_fleet record_dispatcher
    merged_but_not_pulled "a fix that merged while the dispatcher ran"
    out="$(in_fleet cmd_status 2>&1)"
    grep -qi "not live" <<<"$out" \
      || fail "a fix merged under a dispatcher whose checkout never pulled was reported as live: $out"
    grep -q "a fix that merged while the dispatcher ran" <<<"$out" \
      || fail "it did not name the merged commit: $out"
    grep -q "pull --ff-only" <<<"$out" \
      || fail "it advised a restart that on its own would change nothing: $out"
    echo "ok: merged-but-not-pulled is reported, and the pull is said first"
    ;;
  status_names_root)
    make_fixture ok
    make_repo_git
    dispatcher_running
    in_fleet record_dispatcher
    parked="$(git -C "$WORK/repo" rev-parse HEAD)"
    merge_fleet_fix "a fix the running dispatcher never parsed"
    older_checkout "$parked"
    out="$(in_fleet_at "$WORK/wt2" cmd_status 2>&1)"
    grep -q "cd $WORK/repo && ./scripts/orca/fleet.sh run --auto" <<<"$out" \
      || fail "read from a worktree, it told you to start a dispatcher in that worktree: $out"
    grep -q "cd $WORK/wt2" <<<"$out" \
      && fail "it named the caller's worktree, which the fleet removes when its PR merges: $out"
    echo "ok: the restart it prints names the dispatcher's own checkout"
    ;;
  *)
    echo "usage: test_orca_fleet.sh card_says|card_quiet|remove_forces|remove_advice|remove_keeps_stack|remove_sweeps_stack|merged_keeps_dirty|merged_keeps_owned|merged_unknown_git|merged_cli_silent|remove_scoped_sweep|stall_expected|stall_reports|timebox_waits|timebox_stops|queue_skips|list_declines|timebox_rearms|labels_unknown|outage_once|one_lookup|timebox_clears|stop_clears|own_clears|one_card|abandon_blocked|abandon_closed|abandon_human_step|abandon_keeps_dirty|abandon_keeps_commits|abandon_unknown_git|abandon_leaves_working|abandon_timebox|gaveup_not_restarted|gaveup_retry|abandon_warns_first|abandon_warned_saved|abandon_two_keeps|gaveup_pruned|list_says_declined|abandon_reason_flickers|abandon_lookup_blind|status_stale|status_current|status_unrecorded|status_from_worktree|status_draining|status_drained|status_behind|status_names_root" >&2
    exit 2 ;;
esac
