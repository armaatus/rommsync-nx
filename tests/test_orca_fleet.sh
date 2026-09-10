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
#   test_orca_fleet.sh status_behind_revert
#                                         ...but a commit and its revert leave
#                                         `origin/main` byte-identical to what
#                                         the dispatcher parsed -> NOT behind.
#                                         The commits are how the report names
#                                         what changed; the bytes are what
#                                         decides that anything did.
#   test_orca_fleet.sh status_unreadable  the dispatcher's own fleet.sh cannot be
#                                         READ -> "cannot say", not STALE. A hash
#                                         nobody could take compares unequal to
#                                         every recorded one, so a permission
#                                         error would otherwise be reported as a
#                                         change.
#   test_orca_fleet.sh status_names_root  the restart it prints names the
#                                         dispatcher's OWN checkout. This report
#                                         is read from a fleet worktree, and a
#                                         relative `run --auto` there starts a
#                                         dispatcher in a directory the fleet
#                                         removes when that PR merges.
#
# ...and the other half of that: nothing stopped the restart it advises from
# landing on top of a dispatcher that never went away. `MAX_WORKTREES` is
# enforced per PROCESS, so two dispatchers open twice the cap between them,
# reap, card and interrupt the same worktrees, and say so nowhere (#179).
#
#   test_orca_fleet.sh run_refuses        a dispatcher is up -> `run` refuses,
#                                         NAMES its pid, and says how to take
#                                         over. The pidfile is left naming the
#                                         one that is running: a refusal that
#                                         claimed it would leave the live
#                                         dispatcher's own exit unable to
#                                         release it.
#   test_orca_fleet.sh run_stale_recycled the pid is alive and is somebody
#                                         else's -- a `kill -9` left the pidfile
#                                         and the OS wrapped round -> it starts.
#                                         `kill -0` alone would refuse to start
#                                         the fleet at all, forever, on the
#                                         strength of a stranger.
#   test_orca_fleet.sh run_stale_gone     the process is simply gone -> it
#                                         starts.
#
# ...and the same question asked in the two other places this file reads that
# pidfile, because a `status` that says `running` while `run` starts anyway is
# two screens disagreeing about whether the fleet is up -- and `stop --now` is
# the one place the fleet SIGNALS a pid it read out of a file.
#
#   test_orca_fleet.sh status_recycled    the pid is a stranger's -> `idle`.
#   test_orca_fleet.sh stop_spares_stranger
#                                         ...and `stop --now` does not signal
#                                         it, and says why.
#   test_orca_fleet.sh stop_stops_dispatcher
#                                         the control: a real one still goes.
#
# ...and the THIRD answer, which is not "no": a `ps` that will not answer at all
# -- no procps in the container, a launch shape whose argv never carried the
# script path. The two callers want opposite things from it, and collapsing it
# into "no" rebuilds #179 inside the check for it: `--now` would interrupt every
# agent, announce there was no dispatcher, leave it polling, and let the next
# `run` start a second one.
#
#   test_orca_fleet.sh run_blind_ps       it starts -- one stale file may not
#                                         hold the fleet down -- and WARNS,
#                                         naming the pid.
#   test_orca_fleet.sh status_blind_ps    ...and `status` says so rather than
#                                         `idle`, which is the line that sends
#                                         somebody to start the second one.
#   test_orca_fleet.sh stop_blind_ps      ...and `--now` signals it anyway,
#                                         because it promises the dispatcher is
#                                         down when it returns.
#
# ...and the two states a stop can be in, which used to be one (#183). A drain
# waits for the PRs in flight to merge, and the file it wrote was the same file
# guard.py reads before it lets an agent push -- so it waited for PRs it had
# itself forbidden, and ended only when the time-box gave three worktrees up.
# `DRAIN` and `STOP` are now different files with different readers.
#
#   test_orca_fleet.sh drain_ends_on_merge
#                                         the acceptance, end to end and against
#                                         a real dispatcher: one worktree past
#                                         its time-box, kept because its PR is
#                                         open, a drain set WHILE it runs, and
#                                         then that PR merges -> the run ends on
#                                         the work landing, nothing is given up,
#                                         and no agent is interrupted. It used to
#                                         `break 2` out of the whole loop the
#                                         moment the file appeared mid-pass,
#                                         leaving the worktrees it held unreaped.
#   test_orca_fleet.sh drain_after_stop   a drain asked for while STOP is still
#                                         set changes nothing about the agents,
#                                         and says so. It used to print the full
#                                         "they are NOT frozen" reassurance over
#                                         a stop that had them frozen.
#   test_orca_fleet.sh stop_writes_drain  a drain writes DRAIN and NOT STOP.
#   test_orca_fleet.sh stop_now_writes_both
#                                         `--now` writes both: a hard stop is a
#                                         drain plus a freeze.
#   test_orca_fleet.sh drain_lets_agents_finish
#                                         THE issue, asserted against the real
#                                         .claude/hooks/guard.py: after a drain
#                                         that hook allows `git push` and
#                                         `gh pr create`, so the PRs the drain is
#                                         waiting on can actually land.
#   test_orca_fleet.sh stop_freezes_agents
#                                         ...and after `--now` it still refuses
#                                         both. The escape hatch is unchanged.
#   test_orca_fleet.sh drain_launches_nothing
#                                         the half a drain must KEEP: `run`
#                                         refuses with only DRAIN set.
#   test_orca_fleet.sh status_stopped     a hard stop says STOPPED, and does not
#                                         call itself a drain.
#   test_orca_fleet.sh resume_clears_both a resume that cleared one of the two
#                                         would leave a fleet nothing can start.
#   test_orca_fleet.sh stop_drain_blind_dispatcher
#                                         a dispatcher that predates DRAIN cannot
#                                         see one -> the drain WARNS and names
#                                         the pid, rather than looking set while
#                                         that dispatcher keeps launching.
#
# The Orca CLI and gh are stubbed on PATH; the fleet state dir is a temp dir.
# Nothing here touches a real worktree, docker, or GitHub.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() { echo "FAIL: $*" >&2; exit 1; }

WORK=""
# Whatever process the pidfile points at in the run_* phases -- a stand-in
# dispatcher, or the unrelated process a recycled pid lands on. Killed here so a
# failed assertion does not leave a `sleep` behind for half a minute.
HELD_PID=""
cleanup() {
  [ -n "$HELD_PID" ] && kill "$HELD_PID" 2>/dev/null
  [ -n "$WORK" ] && rm -rf "$WORK"
  return 0
}
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
  "worktree list")
    # The real CLI takes `--repo <selector>` and answers only that repo's
    # worktrees; without it the answer is machine-wide (#212). The fixture
    # carries a `repoPath` per entry so a phase can say "this one belongs to
    # some other repo", and this reproduces the filter rather than restating
    # its outcome. An entry with no `repoPath` belongs to whatever was asked
    # for, so every fixture written before the scope existed still answers.
    sel=""
    for arg in "$@"; do
      case "$arg" in path:*) sel="${arg#path:}" ;; esac
    done
    ORCA_REPO_SELECTOR="$sel" python3 -c '
import json, os, sys
doc = json.load(open(sys.argv[1]))
sel = os.environ.get("ORCA_REPO_SELECTOR", "")
if sel:
    doc["result"]["worktrees"] = [
        w for w in doc["result"]["worktrees"] if w.get("repoPath", sel) == sel]
print(json.dumps(doc))
' "$ORCA_WORKTREES"
    exit 0 ;;
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

  # A `ps` that can be blinded. Not "ps says this is not a dispatcher" -- ps says
  # NOTHING, which is what a container without procps, or a launch shape whose
  # argv never carried the script path, actually looks like. dispatcher_alive
  # has to keep that apart from a definite no, because `run` and `stop --now`
  # want opposite things from it.
  cat >"$WORK/bin/ps" <<'STUB'
#!/usr/bin/env bash
[ -s "$PS_BLIND" ] && exit 1
exec /bin/ps "$@"
STUB
  chmod +x "$WORK/bin/ps"
  PS_BLIND="$WORK/ps-blind"; : >"$PS_BLIND"
  export PS_BLIND

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

# What .claude/hooks/guard.py answers about one command, against THIS phase's
# fleet dir: 0 allowed, 2 blocked.
#
# The real hook, not a copy of its rule. The whole of #183 is what TWO programs
# do with the same directory -- fleet.sh writes, guard.py reads -- and a test
# that restated guard.py's rule here would have gone on passing through the
# entire bug. Run from $WORK, which is not a git repo and so is not a worktree
# the fleet owns: the review-marker gate is a different rule and would otherwise
# answer for this one.
guard_says() {
  local payload
  payload="$(python3 -c '
import json, sys
print(json.dumps({"tool_name": "Bash", "tool_input": {"command": sys.argv[1]}}))
' "$1")"
  ( cd "$WORK" && printf '%s' "$payload" \
      | ROMMSYNC_FLEET_DIR="$ROMMSYNC_FLEET_DIR" \
        python3 "$REPO_ROOT/.claude/hooks/guard.py" >/dev/null 2>&1 )
  printf '%s' "$?"
}

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

# The pidfile, naming whatever pid the phase wants it to name.
hold_pidfile() { mkdir -p "$ROMMSYNC_FLEET_DIR"; printf '%s\n' "$1" >"$ROMMSYNC_FLEET_DIR/fleet.pid"; }

# A live process that LOOKS like a dispatcher, and a pidfile naming it. The
# fleet asks `ps -o command=` as well as `kill -0` before it believes a pidfile
# -- before it refuses a second `run`, before `status` says `running`, and
# before `stop --now` SIGNALs -- so a stand-in needs a real command line and not
# just a pid. `bash <path>/fleet.sh run --auto` is what ps prints for the
# dispatcher this machine is running now.
hold_pidfile_with_dispatcher() {
  mkdir -p "$WORK/other/scripts/orca"
  # A loop of short sleeps rather than one long one: cleanup kills this bash,
  # and whatever it is blocked in is orphaned rather than killed with it. A
  # `sleep 30` left holding the inherited stdout is 30 seconds ctest spends
  # waiting for a test that finished -- which is also why it is redirected.
  printf '#!/usr/bin/env bash\nfor _ in $(seq 60); do sleep 1; done\n' \
    >"$WORK/other/scripts/orca/fleet.sh"
  chmod +x "$WORK/other/scripts/orca/fleet.sh"
  bash "$WORK/other/scripts/orca/fleet.sh" run --auto >/dev/null 2>&1 &
  HELD_PID=$!
  # Off the job table, or bash announces "Terminated" on stderr when cleanup
  # kills it and a passing test looks like a broken one.
  disown "$HELD_PID" 2>/dev/null
  hold_pidfile "$HELD_PID"
  # A stand-in ps would not recognise makes every assertion after it vacuous --
  # the fleet would answer "no dispatcher" for the ordinary reason and the
  # phases that assert a refusal would pass without one.
  ps -o command= -p "$HELD_PID" 2>/dev/null | grep -q 'fleet\.sh run' \
    || fail "the stand-in dispatcher does not look like one to ps; every assertion below would be vacuous"
}

# What the status_ phases mean by "a dispatcher is up". It is the real thing
# now, and it has to be: `status` no longer believes a pid it cannot also see
# running a dispatcher, so a bare `echo $$` here would report `idle` and every
# staleness assertion would be about a screen that never printed.
dispatcher_running() { hold_pidfile_with_dispatcher; }

# A pid that is alive and is NOT a dispatcher: what a pidfile a `kill -9` left
# behind turns into once the OS wraps round and hands the number to somebody
# else. Killing THIS is the thing `stop --now` must not do.
hold_pidfile_with_stranger() {
  sleep 30 &
  HELD_PID=$!
  disown "$HELD_PID" 2>/dev/null
  hold_pidfile "$HELD_PID"
  ps -o command= -p "$HELD_PID" 2>/dev/null | grep -q 'fleet\.sh' \
    && fail "the stranger looks like a dispatcher; the phase would assert nothing"
  return 0
}

# ...and a pid nothing holds at all: started, reaped, and gone before it is used.
hold_pidfile_with_ghost() {
  sleep 0 &
  local gone=$!
  wait "$gone" 2>/dev/null
  kill -0 "$gone" 2>/dev/null && fail "the ghost pid is still alive; the phase would assert nothing"
  hold_pidfile "$gone"
}

# `cmd_run` in list mode against an issue that has already landed: one pass, no
# sleep, and it comes back. Every run_ phase goes through this, the refusing one
# included -- `--auto` would sit polling until CTest's timeout if the refusal
# ever regressed, and a phase that fails by hanging says far less than one that
# fails by returning.
run_one_pass() { issue_state CLOSED; in_fleet cmd_run 42 2>&1; }

# ...from here on. Armed AFTER the stand-in dispatcher is made, because making
# one asserts that ps can see it.
blind_ps() { printf '1\n' >"$PS_BLIND"; }

# `kill` returns once the signal is delivered, not once the target is gone, so
# the assertion that it went waits rather than races it.
pid_gone() {
  local i=0
  while [ "$i" -lt 50 ]; do
    kill -0 "$1" 2>/dev/null || return 0
    sleep 0.1; i=$((i + 1))
  done
  return 1
}

# A line the running dispatcher has printed, waited for rather than raced. Ten
# seconds against a one-second poll: the phases that use it drive a real
# dispatcher through two passes, and a fixed sleep would either be flaky or be
# most of the suite's runtime.
wait_for_log() {
  local i=0
  while [ "$i" -lt 100 ]; do
    grep -q "$1" "$WORK/run.log" 2>/dev/null && return 0
    sleep 0.1; i=$((i + 1))
  done
  fail "the dispatcher never said '$1': $(cat "$WORK/run.log" 2>/dev/null)"
}

# ...and its exit, which is the thing a drain is supposed to reach on its own.
run_ended() {
  local i=0
  while [ "$i" -lt 200 ]; do
    kill -0 "$1" 2>/dev/null || return 0
    sleep 0.1; i=$((i + 1))
  done
  return 1
}

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

# ...and then taken back out again, so `origin/main` holds two commits and the
# same bytes the dispatcher parsed.
revert_on_origin() {
  git -C "$WORK/repo" reset -q --hard origin/main
  git -C "$WORK/repo" -c user.email=t@t -c user.name=t revert --no-edit HEAD >/dev/null
  git -C "$WORK/repo" push -q origin HEAD:main
  git -C "$WORK/repo" reset -q --hard "$1"
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
  live_scoped)
    make_fixture ok
    # One worktree of ours, one belonging to a different repository entirely --
    # which is the ordinary state of a machine running more than one fleet.
    python3 -c '
import json, sys
print(json.dumps({"result": {"worktrees": [
    {"path": sys.argv[1] + "/wt", "linkedIssue": 42, "repoPath": sys.argv[1] + "/repo",
     "isMainWorktree": False, "isArchived": False},
    {"path": sys.argv[1] + "/foreign", "linkedIssue": 7, "repoPath": sys.argv[1] + "/other",
     "isMainWorktree": False, "isArchived": False}]}}))
' "$WORK" >"$ORCA_WORKTREES"
    n="$(in_fleet live_count 2>&1)"
    [ "$n" = 1 ] \
      || fail "counted $n live worktree(s); another repo's worktree is taking a slot from MAX_WORKTREES"
    grep -q -- "--repo path:$WORK/repo" "$ORCA_CALLS" \
      || fail "it asked for every worktree on the machine, not this repo's: $(cat "$ORCA_CALLS")"
    # The same list answers `in_flight`, which matches on an issue NUMBER, so an
    # unrelated repo's #7 must not answer for ours. 0 = in flight, 1 = free.
    in_fleet in_flight 7; rc=$?
    [ "$rc" = 1 ] \
      || fail "another repo's issue 7 reads as in flight here (in_flight said $rc)"
    echo "ok: the count, and what is in flight, are this repository's"
    ;;
  foundation_foreign)
    make_fixture ok
    # A foundation issue lands alone, so the dispatcher holds it until `live`
    # reaches 0 (the gate is `is_foundation && [ "$live" -gt 0 ]`). With the
    # count unscoped that never happened: a worktree on another repo held every
    # foundation issue forever, because nothing this fleet does can close one.
    #
    # This asserts the gate's two inputs rather than driving `cmd_run`. A
    # dispatcher that HAS launched its foundation issue then polls until the
    # work lands, by design -- so a phase that ran one would have to kill it,
    # and an orphan holding the inherited stdout is time ctest spends waiting
    # for a test that already finished (see hold_pidfile_with_dispatcher).
    cat >"$GH_ISSUES" <<'JSON'
[{"number":196,"title":"the foundation one","body":"","labels":[{"name":"ready"},{"name":"foundation"}]}]
JSON
    python3 -c '
import json, sys
print(json.dumps({"result": {"worktrees": [
    {"path": sys.argv[1] + "/foreign", "linkedIssue": 1, "repoPath": sys.argv[1] + "/other",
     "isMainWorktree": False, "isArchived": False}]}}))
' "$WORK" >"$ORCA_WORKTREES"
    n="$(in_fleet live_count 2>&1)"
    [ "$n" = 0 ] \
      || fail "counted $n live worktree(s) with only another repo's open, so the foundation gate never opens"
    out="$(in_fleet ready_issues 2>&1)"
    grep -q "^196" <<<"$out" \
      || fail "the foundation issue stopped being startable for some other reason, so the count above proves nothing: $out"
    grep -q "foundation" <<<"$out" \
      || fail "the fixture issue is not labelled foundation, so this phase asserts the wrong gate: $out"
    echo "ok: another repo's worktree does not hold a foundation issue"
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
    : >"$ROMMSYNC_FLEET_DIR/DRAIN"
    out="$(in_fleet cmd_status 2>&1)"
    grep -q "DRAINING" <<<"$out" || fail "a drain stopped being reported: $out"
    grep -qi "not live" <<<"$out" \
      || fail "a draining dispatcher hid its staleness behind the stop, and a drain is exactly when it keeps running: $out"
    echo "ok: draining and still up says both"
    ;;
  status_stopped)
    make_fixture ok
    make_repo_git
    mkdir -p "$ROMMSYNC_FLEET_DIR"; : >"$ROMMSYNC_FLEET_DIR/STOP"
    out="$(in_fleet cmd_status 2>&1)"
    grep -q "STOPPED" <<<"$out" || fail "a hard stop stopped being reported: $out"
    # The two are not the same state and the screen may not blur them: a drain
    # lets agents finish and a stop freezes them, and the person reading this is
    # deciding whether to wait.
    grep -q "DRAINING" <<<"$out" \
      && fail "it called a hard stop a drain, so nothing on the screen says whether the agents are frozen: $out"
    echo "ok: status keeps the two states apart"
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
    # The steps in docs/WORKFLOW.md's order, which is the section this output
    # tells the reader to go and read. A screen that contradicts the page it
    # cites is worse than either alone.
    order="$(printf '%s\n' "$out" \
      | sed -n 's/.*stop\.sh.*/stop/p; s/.*pull --ff-only.*/pull/p; s/.*fleet\.sh resume.*/resume/p' \
      | tr '\n' ' ')"
    [ "$order" = "stop pull resume " ] \
      || fail "the steps are not in the order WORKFLOW.md gives them ($order): $out"
    echo "ok: merged-but-not-pulled is reported, and the pull sits where the doc puts it"
    ;;
  status_behind_revert)
    make_fixture ok
    make_repo_git
    dispatcher_running
    in_fleet record_dispatcher
    parked="$(git -C "$WORK/repo" rev-parse HEAD)"
    merged_but_not_pulled "a fix that merged while the dispatcher ran"
    revert_on_origin "$parked"
    out="$(in_fleet cmd_status 2>&1)"
    grep -qi "not live" <<<"$out" \
      && fail "it asked for a pull and a restart for bytes already running: $out"
    grep -q "up since" <<<"$out" \
      || fail "it stopped saying when the dispatcher started: $out"
    echo "ok: commits that cancel out are not something to pull for"
    ;;
  status_unreadable)
    make_fixture ok
    make_repo_git
    dispatcher_running
    in_fleet record_dispatcher
    # Asked from a second checkout, because a fleet.sh nothing can read is also
    # a fleet.sh nothing can source.
    older_checkout "$(git -C "$WORK/repo" rev-parse HEAD)"
    chmod 000 "$WORK/repo/scripts/orca/fleet.sh"
    out="$(in_fleet_at "$WORK/wt2" cmd_status 2>&1)"
    chmod 644 "$WORK/repo/scripts/orca/fleet.sh"
    grep -qi "cannot say" <<<"$out" \
      || fail "a file it could not read was reported as an answer: $out"
    grep -qi "STALE" <<<"$out" \
      && fail "it read a permission error as a change to the file: $out"
    echo "ok: a hash nobody could take is 'cannot say', not 'stale'"
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
  status_recycled)
    make_fixture ok
    make_repo_git
    hold_pidfile_with_stranger
    out="$(in_fleet cmd_status 2>&1)"
    grep -q "running   (pid $HELD_PID)" <<<"$out" \
      && fail "a pidfile whose pid the OS recycled onto a stranger was reported as a running dispatcher: $out"
    grep -q "^idle" <<<"$out" \
      || fail "it said neither running nor idle about a pid that is not a dispatcher: $out"
    echo "ok: status asks the same question run does, so the two cannot disagree"
    ;;
  stop_spares_stranger)
    make_fixture ok
    hold_pidfile_with_stranger
    out="$(in_fleet cmd_stop --now 2>&1)"
    kill -0 "$HELD_PID" 2>/dev/null \
      || fail "stop --now signalled a pid out of a stale pidfile, and it was somebody else's process: $out"
    grep -q "dispatcher stopped" <<<"$out" \
      && fail "it reported stopping a dispatcher it never found: $out"
    grep -q "not one" <<<"$out" \
      || fail "it said nothing about a pidfile naming something that is not a dispatcher: $out"
    echo "ok: stop --now checks what it is about to signal, the way lib.sh does"
    ;;
  stop_stops_dispatcher)
    make_fixture ok
    hold_pidfile_with_dispatcher
    out="$(in_fleet cmd_stop --now 2>&1)"
    pid_gone "$HELD_PID" \
      || fail "stop --now left the dispatcher running; the check it gained is refusing everything: $out"
    grep -q "dispatcher stopped" <<<"$out" \
      || fail "it stopped the dispatcher and did not say so: $out"
    echo "ok: ...and still stops the one that is really there"
    ;;
  run_blind_ps)
    make_fixture ok
    hold_pidfile_with_dispatcher
    blind_ps
    out="$(run_one_pass)"; rc=$?
    [ "$rc" = 0 ] \
      || fail "a ps that would not answer held the fleet down, which is the failure the check exists to avoid: $out"
    grep -q "WARNING" <<<"$out" \
      || fail "it started over an unverifiable pid in silence, and that silence IS the two-dispatcher case: $out"
    grep -q "ps -p $HELD_PID" <<<"$out" \
      || fail "the warning does not name the pid it could not identify, nor how to find out: $out"
    # Not a bare `kill`: the whole reason this branch exists is that nothing
    # established what that pid is, and WORKFLOW.md now says the same.
    grep -q "kill $HELD_PID" <<<"$out" \
      && fail "it handed out a kill for a pid it had just said it could not identify: $out"
    echo "ok: an unanswerable ps starts the fleet, and says it could not tell"
    ;;
  status_blind_ps)
    make_fixture ok
    make_repo_git
    hold_pidfile_with_dispatcher
    blind_ps
    out="$(in_fleet cmd_status 2>&1)"
    # The idle LINE, not the word: restart_advice prints "until it says idle" in
    # the instructions right below, and grepping for that would pass on anything.
    grep -q "^idle" <<<"$out" \
      && fail "a live dispatcher ps would not name was reported as idle, which sends you to start a second: $out"
    grep -q "$HELD_PID" <<<"$out" \
      || fail "it did not name the pid it could not identify: $out"
    echo "ok: alive-but-unidentifiable is not idle"
    ;;
  stop_blind_ps)
    make_fixture ok
    hold_pidfile_with_dispatcher
    blind_ps
    out="$(in_fleet cmd_stop --now 2>&1)"
    pid_gone "$HELD_PID" \
      || fail "--now interrupted the agents, said nothing was there, and left the dispatcher polling: $out"
    grep -q "dispatcher stopped" <<<"$out" \
      || fail "it stopped the dispatcher and did not say so: $out"
    grep -q "not one" <<<"$out" \
      && fail "it called a dispatcher it had just signalled 'not one': $out"
    echo "ok: --now keeps its promise when ps will not answer"
    ;;
  run_refuses)
    make_fixture ok
    hold_pidfile_with_dispatcher
    out="$(run_one_pass)"; rc=$?
    [ "$rc" = 0 ] \
      && fail "a second dispatcher started while one was already running: $out"
    grep -q "$HELD_PID" <<<"$out" \
      || fail "it refused without naming the pid that holds it, which is the one thing you need: $out"
    grep -q "kill $HELD_PID" <<<"$out" \
      || fail "it did not say how to take over; a person restarting a stale dispatcher is doing the right thing: $out"
    # Both takeovers, because they cost different things: a drain is safe with
    # agents mid-work (#183) but waits hours for their PRs, and a bare kill is
    # immediate and gives up the reaping. A refusal that named only one sends
    # somebody to the wrong one.
    grep -q "stop.sh" <<<"$out" \
      || fail "it never mentions the stop, so nothing warns that a restart via stop.sh freezes every agent: $out"
    [ "$(cat "$ROMMSYNC_FLEET_DIR/fleet.pid")" = "$HELD_PID" ] \
      || fail "the refusal took the pidfile anyway, so the running dispatcher's own exit would no longer release it"
    echo "ok: a second dispatcher is refused, by pid, and told how to take over"
    ;;
  run_stale_recycled)
    make_fixture ok
    hold_pidfile_with_stranger
    out="$(run_one_pass)"; rc=$?
    [ "$rc" = 0 ] \
      || fail "a pidfile whose pid the OS recycled onto a stranger refused to start the fleet at all: $out"
    grep -q "fleet up" <<<"$out" \
      || fail "it never started: $out"
    [ "$(cat "$ROMMSYNC_FLEET_DIR/fleet.pid" 2>/dev/null)" = "$HELD_PID" ] \
      && fail "it started and left the stranger's pid in the pidfile"
    echo "ok: a recycled pid is not a dispatcher, and does not hold the fleet down"
    ;;
  run_stale_gone)
    make_fixture ok
    hold_pidfile_with_ghost
    out="$(run_one_pass)"; rc=$?
    [ "$rc" = 0 ] \
      || fail "a pidfile left by a killed dispatcher refused to start the fleet at all: $out"
    grep -q "fleet up" <<<"$out" \
      || fail "it never started: $out"
    echo "ok: a pidfile whose process is gone does not refuse"
    ;;
  drain_ends_on_merge)
    make_fixture ok
    make_worktree
    add_origin
    # Past its time-box, and kept only because its PR is open -- which is the
    # state every worktree in a real drain is in.
    make_overdue
    agent_state working
    issue_state OPEN; issue_labels "ready"
    echo '[{"number":9,"body":"Closes #42"}]' >"$GH_PRS"
    : >"$GH_MERGED"
    ( in_fleet cmd_run --auto >"$WORK/run.log" 2>&1 ) &
    HELD_PID=$!
    wait_for_log "fleet up"
    in_fleet cmd_stop >/dev/null 2>&1
    # Either line will do: the drain is noticed at the top of a pass, or in the
    # launch loop when it appears mid-pass, and which one a real `stop.sh` hits
    # is a race this phase must not depend on.
    wait_for_log "draining"
    # ...and now the PR the drain is waiting on merges, which is what a drain
    # that also froze the agents made impossible.
    echo 7 >"$GH_MERGED"
    run_ended "$HELD_PID" \
      || fail "the drain never ended; it is waiting for something only the time-box will now resolve: $(cat "$WORK/run.log")"
    HELD_PID=""
    grep -q "everything in flight has landed" "$WORK/run.log" \
      || fail "it exited for some other reason than the work landing: $(cat "$WORK/run.log")"
    [ -e "$ROMMSYNC_FLEET_DIR/gaveup-42" ] \
      && fail "the time-box gave the worktree up; that is the three-hours-each ending #183 is about: $(cat "$WORK/run.log")"
    grep -q -- "--interrupt" "$ORCA_CALLS" \
      && fail "a drain interrupted an agent, which is --now's job and not this one"
    echo "ok: a drain ends when the work in flight lands, not at the time-box"
    ;;
  drain_after_stop)
    make_fixture ok
    in_fleet cmd_stop --now >/dev/null 2>&1
    out="$(in_fleet cmd_stop 2>&1)"
    grep -q "STILL SET" <<<"$out" \
      || fail "a drain over a stop said nothing about the stop, and the agents it promises are finishing are frozen: $out"
    grep -q "NOT frozen" <<<"$out" \
      && fail "it told you the agents were free to push while $ROMMSYNC_FLEET_DIR/STOP was still there: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/STOP" ] \
      || fail "the drain cleared the stop; lifting one is resume's job and nothing here asked for it: $out"
    echo "ok: a drain does not lift a stop, and does not pretend it did"
    ;;
  stop_writes_drain)
    make_fixture ok
    out="$(in_fleet cmd_stop 2>&1)"
    [ -e "$ROMMSYNC_FLEET_DIR/DRAIN" ] \
      || fail "a drain wrote no DRAIN file, so nothing tells the dispatcher to stop launching: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/STOP" ] \
      && fail "a drain wrote the STOP file, which is what guard.py reads -- the PRs it now waits for cannot be opened (#183): $out"
    grep -q "finish" <<<"$out" \
      || fail "it never says the agents may finish, which is the whole difference from --now: $out"
    echo "ok: a drain sets the drain and nothing else"
    ;;
  stop_now_writes_both)
    make_fixture ok
    out="$(in_fleet cmd_stop --now 2>&1)"
    [ -e "$ROMMSYNC_FLEET_DIR/STOP" ] \
      || fail "--now let the agents keep pushing; it is the escape hatch and #183 must not have widened it: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/DRAIN" ] \
      || fail "--now froze the agents and left the dispatcher free to launch more: $out"
    echo "ok: a hard stop is a drain plus a freeze"
    ;;
  drain_lets_agents_finish)
    make_fixture ok
    mkdir -p "$ROMMSYNC_FLEET_DIR"
    # The control first, so the allow below is not the guard failing to see this
    # fleet dir at all: with the STOP file set it must refuse.
    : >"$ROMMSYNC_FLEET_DIR/STOP"
    [ "$(guard_says 'git push')" = 2 ] \
      || fail "guard.py did not refuse a push under STOP, so this phase cannot tell an allow from a hook it never reached"
    rm -f "$ROMMSYNC_FLEET_DIR/STOP"
    out="$(in_fleet cmd_stop 2>&1)"
    [ "$(guard_says 'git push')" = 0 ] \
      || fail "a drain still blocks the push, so it waits for PRs it has itself forbidden and ends only at the time-box (#183): $out"
    [ "$(guard_says 'gh pr create --fill')" = 0 ] \
      || fail "a drain still blocks opening the PR, and a merged PR is what releases the worktree it is waiting on: $out"
    echo "ok: a drain lets the work in flight finish"
    ;;
  stop_freezes_agents)
    make_fixture ok
    out="$(in_fleet cmd_stop --now 2>&1)"
    [ "$(guard_says 'git push')" = 2 ] \
      || fail "a hard stop no longer stops a push: $out"
    [ "$(guard_says 'gh pr create --fill')" = 2 ] \
      || fail "a hard stop no longer stops a PR: $out"
    [ "$(guard_says 'ctest --test-dir build')" = 0 ] \
      || fail "it froze the machine rather than what leaves it; reading, building and testing stay open: $out"
    echo "ok: --now still stops every outward effect"
    ;;
  drain_launches_nothing)
    make_fixture ok
    in_fleet cmd_stop >/dev/null 2>&1
    out="$(run_one_pass)"; rc=$?
    [ "$rc" = 0 ] \
      && fail "a drain let a new dispatcher start, so the half a drain must keep is gone: $out"
    grep -qi "drain" <<<"$out" \
      || fail "it refused without saying which of the two states it is in: $out"
    echo "ok: a drain still starts nothing new"
    ;;
  resume_clears_both)
    make_fixture ok
    in_fleet cmd_stop --now >/dev/null 2>&1
    # Both, before: a `resume` asserted against files that were never there
    # passes on a fleet that writes neither.
    for f in STOP DRAIN; do
      [ -e "$ROMMSYNC_FLEET_DIR/$f" ] \
        || fail "--now did not set $f, so the resume below clears nothing and asserts nothing"
    done
    out="$(in_fleet cmd_resume 2>&1)"
    [ -e "$ROMMSYNC_FLEET_DIR/STOP" ] \
      && fail "resume left the STOP file, so no agent can push and nothing says why: $out"
    [ -e "$ROMMSYNC_FLEET_DIR/DRAIN" ] \
      && fail "resume left the DRAIN file, so run goes on refusing with the stop apparently cleared: $out"
    echo "ok: resume clears both files"
    ;;
  stop_drain_blind_dispatcher)
    make_fixture ok
    make_repo_git
    dispatcher_running
    # A dispatcher that recorded what it parsed, and knows the drain file.
    in_fleet record_dispatcher
    out="$(in_fleet cmd_stop 2>&1)"
    grep -q "WARNING" <<<"$out" \
      && fail "it warned about a dispatcher that reads the drain file perfectly well: $out"
    # ...and one from before #183. It leaves a FULL record that simply lacks the
    # drain= line -- record_dispatcher has written root, started, commit and
    # hash since #173 -- so removing the file instead would also empty `root`
    # and only ever exercise the warning's fallback half.
    grep -v '^drain=' "$ROMMSYNC_FLEET_DIR/dispatcher" >"$WORK/old-record"
    mv "$WORK/old-record" "$ROMMSYNC_FLEET_DIR/dispatcher"
    out="$(in_fleet cmd_stop 2>&1)"
    grep -q "WARNING" <<<"$out" \
      || fail "a drain that pid $HELD_PID cannot see looked exactly like one it can, while it kept launching worktrees: $out"
    grep -q "kill $HELD_PID" <<<"$out" \
      || fail "the warning does not say how to take that dispatcher over: $out"
    # Its OWN checkout, the way every other restart this file prints does: a
    # relative `run --auto` starts a dispatcher in whatever worktree you typed it.
    grep -q "cd $WORK/repo && ./scripts/orca/fleet.sh run --auto" <<<"$out" \
      || fail "the takeover does not name the dispatcher's own checkout: $out"
    echo "ok: a dispatcher too old to see the drain is not drained in silence"
    ;;
  *)
    echo "usage: test_orca_fleet.sh card_says|card_quiet|remove_forces|remove_advice|remove_keeps_stack|remove_sweeps_stack|merged_keeps_dirty|merged_keeps_owned|merged_unknown_git|merged_cli_silent|remove_scoped_sweep|stall_expected|stall_reports|timebox_waits|timebox_stops|queue_skips|list_declines|timebox_rearms|labels_unknown|outage_once|one_lookup|timebox_clears|stop_clears|own_clears|one_card|abandon_blocked|abandon_closed|abandon_human_step|abandon_keeps_dirty|abandon_keeps_commits|abandon_unknown_git|abandon_leaves_working|abandon_timebox|gaveup_not_restarted|gaveup_retry|abandon_warns_first|abandon_warned_saved|abandon_two_keeps|gaveup_pruned|list_says_declined|abandon_reason_flickers|abandon_lookup_blind|status_stale|status_current|status_unrecorded|status_from_worktree|status_draining|status_stopped|status_drained|status_behind|status_behind_revert|status_unreadable|status_names_root|run_refuses|run_stale_recycled|run_stale_gone|status_recycled|stop_spares_stranger|stop_stops_dispatcher|run_blind_ps|status_blind_ps|stop_blind_ps|drain_ends_on_merge|drain_after_stop|stop_writes_drain|stop_now_writes_both|drain_lets_agents_finish|stop_freezes_agents|drain_launches_nothing|resume_clears_both|stop_drain_blind_dispatcher" >&2
    exit 2 ;;
esac
