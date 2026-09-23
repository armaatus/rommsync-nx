#!/usr/bin/env bash
# The Orca driver: how autofleet creates a worktree and hosts a build in a
# terminal a person can watch. Sourced by lib.sh when AUTOFLEET_RUNNER=orca,
# which is NO LONGER THE DEFAULT -- `headless` is, and it needs no app at all.
# This one exists for the desk: it runs the identical command line in a tab.
# armaatus/autofleet#151.
#
# THE CONTRACT A SECOND DRIVER HAS TO MEET is in docs/RUNNERS.md, and this file
# is now the whole of it: no code outside this directory CALLS the CLI, reads
# $ORCA_CLI, or parses a line of Orca's JSON. A tmux + `git worktree` driver is
# a second file here and nothing else. Orca is still NAMED outside it -- in
# orca.yaml, and in the comments of the hooks orca.yaml points at -- and
# docs/RUNNERS.md lists what those hooks still assume about the runtime.
#
# Two properties hold for every function below, and they are the reason the
# seam is worth having rather than a convention:
#
#   1. EVERY CALL HAS A DEADLINE. The runtime can accept a connection and then
#      never answer, and orca.yaml's `setupAgentStartupPolicy: wait-for-setup`
#      holds the agent's tab until setup.sh returns -- a hook that blocks
#      forever costs the whole worktree.
#   2. "I COULD NOT TELL" IS NOT "NOTHING". Each function distinguishes a
#      negative answer from a failed question, because conflating them is how a
#      dispatcher waits out its whole time-box and then reports that nothing
#      arrived. Where that needs more than two codes it is spelled out on the
#      function.
#
# The deadlines are the ones each callsite used before the move.
ORCA_DEADLINE=30
ORCA_SEND_DEADLINE=20
ORCA_CREATE_DEADLINE=240

# `runner_set_deadline <seconds>` -- part of the contract, because a caller that
# needs a shorter one has to be able to ask WITHOUT knowing which driver it has.
# The watcher that polled every three seconds was that caller, and one
# that can block for thirty of them inside one poll has stopped watching. It
# used to say so by exporting a variable only this file reads, which is a caller
# outside runner/ assuming how the driver works -- the exact thing the seam
# exists to stop. Found by the independent review.
#
# Creating a worktree keeps its own, deliberately: nobody polls a create, and a
# 20-second deadline on a call that legitimately takes minutes is not a shorter
# wait, it is a failed launch.
runner_set_deadline() {
  ORCA_DEADLINE="$1"
  ORCA_SEND_DEADLINE="$1"
}

# The Orca CLI this machine can actually run, in $ORCA_CLI.
#
# `orca` on PATH is a wrapper that locates Orca.app by reading its own symlink.
# A macOS install has shipped that symlink mode 0700 root:wheel, so the readlink
# fails for the user Orca runs these hooks as and every call dies with "Unable to
# determine Orca.app path from symlink". Nothing here notices a broken CLI as
# such -- the JSON never parses -- so it surfaces one layer up as an answer:
# a hook reports "this worktree has no linked issue", stops
# watching, and leaves a fully provisioned worktree whose agent sits on an unsent
# prompt forever. That is how three worktrees went idle on 2026-09-05.
#
# So the wrapper is verified rather than assumed, and the app's own binary is the
# fallback. `--version` is the probe because it is the one call that needs no
# runtime: a wrapper that cannot find Orca.app fails it, and a reachable CLI
# answers it whether or not the app is running.
#
# The probe goes through fleet_run_with_deadline like every other CLI call. It is
# the FIRST call each hook makes, and `setupAgentStartupPolicy: wait-for-setup`
# holds the agent's tab until setup.sh returns -- so a wrapper that connects and
# then never answers would hang worktree provisioning at the one point where
# nothing has printed a reason yet.
#
# Orca-named and Orca-private. The broken symlink is Orca's own install, not any
# project's -- armaatus/rommsync-nx is only where the fleet first ran into it --
# so a second driver inherits none of this.
#
# Returns non-zero when nothing answers, so a caller can say so in one line
# instead of making its first real call and reading the silence as data.
ORCA_CLI_PROBE_SECONDS="${ORCA_CLI_PROBE_SECONDS:-10}"

# The candidates, in the order they are tried, one per line.
#
# A function rather than a list inline in `orca_cli_resolve`, which is its only
# caller, because the list is what a test has to be able to REPLACE: whether
# this machine has an orca CLI is a property of the machine, and
# `tests/test_fleet.sh runner_missing` overrides this to pin the refusal's
# wording against candidates it controls. The earlier version of this comment
# claimed `orca_unavailable_says` read it too; it reads $ORCA_CLI_REJECTS, and a
# why a reader can falsify in one grep costs more than none. Found by the local
# review.
orca_cli_candidates() {
  [ -n "${ORCA_CLI_COMMAND:-}" ] && printf '%s\n' "$ORCA_CLI_COMMAND"
  printf '%s\n' orca orca-dev orca-ide \
    /Applications/Orca.app/Contents/Resources/bin/orca
}

# What was tried and why each was turned down, as one comma-joined line.
#
# Collected DURING the resolve rather than re-derived after it, which is where
# this departs from `fleet_python_rejections`: re-probing costs a second
# `--version` per candidate, each with its own $ORCA_CLI_PROBE_SECONDS, and this
# is the first call `setup.sh` makes while the runner holds the agent's tab. Five
# candidates that all time out would be a hundred seconds of silence to explain
# the fifty that came before it.
ORCA_CLI_REJECTS=""
orca_cli_reject() { ORCA_CLI_REJECTS="${ORCA_CLI_REJECTS:+$ORCA_CLI_REJECTS, }$1"; }

orca_cli_resolve() {
  [ -n "${ORCA_CLI:-}" ] && return 0
  local candidate where probe_rc
  # Reset per resolve, not per source: a resolve that fails, then succeeds after
  # the app starts, must not leave the first attempt's reasons behind for a
  # later failure to print as if they were its own.
  ORCA_CLI_REJECTS=""
  # /dev/null, NOT A TEMP FILE. Nothing ever reads the probe's output -- only
  # its exit status is the answer -- so the file existed solely to be written to
  # and deleted.
  #
  # It cost three rounds of the local review to notice, and each of those rounds
  # was a bug in the code that made it: the machine with no `mktemp` needed a
  # third refusal shape of its own, a retry that SUCCEEDED had its file deleted
  # and was refused anyway, and the diagnostic call that replaced it was itself
  # unchecked. `tests/test_env.sh setup_fails_fast` caught the first, because
  # its PATH holds one interpreter and nothing else. Sixty lines, a global, a
  # branch in `orca_unavailable_says` and a test part went with it.
  #
  # WHAT WENT WITH THEM IS THE `mktemp` BINARY, not every temp file, and the
  # wider claim stood here until the independent review took it down. The
  # candidate list at the bottom of this function is a here-document, and bash
  # 3.2 -- the `/bin/bash` every macOS ships, which this repo targets -- backs
  # one with a real file: `stat -f %HT /dev/fd/3` inside the loop says `Regular
  # File` on 3.2.57 and `Fifo File` on 5.1+, which is where bash started using a
  # pipe for small ones. An unwritable `$TMPDIR` does not reach it, because bash
  # falls back to `/tmp` when `$TMPDIR` is not a writable directory (checked
  # both ways on 3.2.57). A machine that can write a temp file NOWHERE loses the
  # list instead: the loop body never runs, and the refusal reads `tried:
  # nothing was probed` -- which names no candidate rather than naming the wrong
  # one, so it is still not the confident lie about the machine that #13 exists
  # to remove.
  # ON FD 3, and the list is what a candidate CLI must not be able to drain: it
  # would eat the rest of the heredoc, `read` would hit EOF, and the loop would
  # end after that one candidate -- the refusal then naming it as everything
  # that was tried while the /Applications fallback the 0700 install needs sits
  # last and untried. Fd 3 does NOT give that on its own, which is the whole
  # point: `fleet_run_with_deadline` forks with `&` and, with job control off,
  # bash points only STDIN at /dev/null, so the stdin form this replaced was
  # protected by the shell and fd 3 is inherited untouched. The `3<&-` on the
  # probe call below is what closes it, and without that line the rewrite was
  # strictly worse than what it replaced. Raised by the local review, which
  # caught the rationale pointing the wrong way; stated once here by the
  # self-review, which found it stated wrongly and then corrected 25 lines
  # further down.
  while IFS= read -r -u 3 candidate; do
    [ -n "$candidate" ] || continue
    if ! where="$(command -v "$candidate" 2>/dev/null)"; then
      # NOT "not on PATH", twice over. `command -v` turns down a file that is
      # right there and not executable by this user exactly as it turns down one
      # that is absent -- and the 0700-root:wheel wrapper CLAUDE.md names as the
      # reason this probe exists at all is precisely the first case, so telling
      # that person their PATH is wrong sends them to check something correct
      # and to distrust the rest of the message. For a BARE NAME the two are not
      # distinguishable from here without walking PATH, so the line says both.
      # For an ABSOLUTE PATH -- the /Applications fallback below, and an
      # absolute ORCA_CLI_COMMAND -- there was never a PATH lookup to fail, and
      # on the common case of a Mac with no Orca the `tried:` line ended with a
      # sentence about PATH for a path. Both halves found by the local review.
      # `*/*`, not `/*`: `command -v` skips PATH for ANY name containing a
      # slash, so a relative `bin/orca` was never a PATH lookup either and was
      # being reported as one. Found by the local review.
      case "$candidate" in
        */*) orca_cli_reject "$candidate (no such file, or not executable)" ;;
        *)   orca_cli_reject "$candidate (not found on PATH, or found and not executable)" ;;
      esac
      continue
    fi
    # `3<&-` closes the candidate list for the child -- see the top of the loop
    # for why fd 3 needs it and stdin did not.
    # 124 IS THE WRAPPER'S DEADLINE, and the two failures a reader has to tell
    # apart no longer share a sentence: a CLI that
    # HANGS is an app mid-start or wedged and is worth waiting out, while one
    # that is there and exits non-zero has already answered. "did not answer"
    # for an instant `exit 3` sent the reader to look for a wedged app. Captured
    # into a variable because `if ! cmd` sets `$?` to the negation, so the
    # branch that wants the status cannot read it. Raised by the independent
    # review.
    #
    # It is not a status only the wrapper can produce: `fleet_run_with_deadline`
    # ends in `wait "$child"`, so a candidate that is itself a `timeout` wrapper
    # propagating 124 reads here as a hang. That misreads a CLI which did answer
    # as one that did not -- the same class as before, one candidate wide
    # instead of all of them, and the remedy line is the same either way.
    # Distinguishing them needs the wrapper to report the deadline out of band,
    # which is a change to a function eleven callers share and is not this
    # issue. Found by the self-review.
    probe_rc=0
    fleet_run_with_deadline "$ORCA_CLI_PROBE_SECONDS" /dev/null \
      "$candidate" --version 3<&- || probe_rc=$?
    if [ "$probe_rc" != 0 ]; then
      case "$probe_rc" in
        124) orca_cli_reject "$where (no --version answer in ${ORCA_CLI_PROBE_SECONDS}s)" ;;
        *)   orca_cli_reject "$where (--version exited $probe_rc)" ;;
      esac
      continue
    fi
    ORCA_CLI="$candidate"
    return 0
  done 3<<EOF
$(orca_cli_candidates)
EOF
  return 1
}

# Every call goes through one of these two, and BOTH resolve first.
#
# A driver function is allowed to be called before `runner_available` -- the
# fleet's own scripts all probe, but the contract does not oblige a caller to,
# and `orca_cli_resolve` returns instantly once $ORCA_CLI is set. Without this
# the failure was `ORCA_CLI: unbound variable` from inside the driver on any
# script running `set -u`, which is the least useful sentence available: it
# names a variable rather than saying the app is not answering. Found by
# smoke-testing the driver from a bare shell.
orca_cli() {
  orca_cli_resolve || return 1
  fleet_run_with_deadline "$1" "$2" "$ORCA_CLI" "${@:3}"
}

# One `orca ... --json` call, its stdout in $1, on the ordinary deadline.
# Private: a caller outside this file passing its own subcommand would be the
# seam leaking through a hole in the middle of it.
orca_json() {
  local out="$1"; shift
  orca_cli "$ORCA_DEADLINE" "$out" "$@" --json
}

# ------------------------------------------------------------ the contract ---

# 0 if this runner can be used at all.
#
# It SAYS why on stderr when it cannot, because only the driver knows what to
# check next -- "is the Orca app running?" is not a sentence the dispatcher can
# write for an arbitrary runner. The caller adds the consequence.
# The sentence, once. It was written three times in three separate review rounds
# -- and on two different streams, which is precisely what the last of those
# rounds had to fix -- so the third copy was a fourth chance to pick the wrong
# one. WHICH STREAM stays with the callsite, because that is genuinely per-caller:
# `runner_available` is a probe nobody captures, while `runner_worktree_create`
# and `runner_worktree_set` print where their callers read. Found by the
# independent review.
#
# THREE LINES, and that is a ceiling rather than a coincidence: docs/RUNNERS.md
# caps a relay at three, `runner_worktree_create` prints this one on the stream
# `launch` logs, and `launch` retries every pass. So what was tried is one
# comma-joined line however many candidates there were, not one line each.
# The three carry the three things a person needs: which runner, what was tried,
# and what to do about it. Today's line named the runtime and stopped there, so
# the answer to "and now what" was a file nobody reads twice.
# armaatus/autofleet#13.
orca_unavailable_says() {
  printf 'no orca CLI answers here; is the Orca app running?\n'
  printf '     tried: %s\n' "${ORCA_CLI_REJECTS:-nothing was probed}"
  printf '     install Orca (https://orca.computer) and start it, or set ORCA_CLI_COMMAND to a CLI that answers --version\n'
}

runner_available() {
  orca_cli_resolve && return 0
  orca_unavailable_says >&2
  return 1
}

# How a person starts the dispatcher somewhere it stays visible. One line, and
# the only human-facing string that is runner-specific: `fleet.sh --help` prints
# it rather than hardcoding a command that is wrong for every other driver.
runner_dispatcher_hint() {
  printf 'orca terminal create --worktree active --title fleet --command "%s"\n' \
    "./scripts/fleet/fleet.sh run --auto"
}

# Open a worktree for one issue, printing its path.
#
# On failure, prints the runtime's own words instead -- capped at three lines
# like every other relay -- because "could not create it" with nothing after it
# is the same message whether the app is down or the branch already exists.
#
# stderr goes to its OWN file, not merged into $out. This is the one driver
# function that both relays a failure and parses a success, and merging broke the
# parse for any CLI that succeeded while writing anything at all to stderr: the
# worktree existed, counted against the cap, and nothing owned it -- no card, no
# time-box, and neither reap iterates it, because both walk OWNED_DIR. A slot
# held forever by a worktree the fleet cannot see. Found by the independent
# review; lib.sh states the rule this broke, in this change's own words.
# NO --agent AND NO --prompt any more, which is the shape armaatus/autofleet#151
# changed here. Orca used to start the agent itself, from a worktree-creation
# hook, with the brief DRAFTED in its composer and a watcher of ours pressing
# Return -- so the dispatcher never saw the agent start, and a hook that failed
# left a fully provisioned worktree sitting on an unsent prompt until a person
# noticed. This call now creates a worktree and nothing else; `runner_build_start`
# is what puts a build in it, and it runs the same command the headless driver
# runs.
runner_worktree_create() {
  local repo="$1" name="$2" issue="$3"
  # Resolved explicitly, and SAID, because `orca_cli` answers a failed resolve
  # with `orca_cli_resolve || return 1` BEFORE anything is written to $err or
  # $out -- so the relay below had nothing to relay and `launch` printed
  # "  could not create it:" followed by nothing. Word for word the message
  # `create_says` exists to prevent, on the one path that phase did not cover.
  # Both neighbours already do this: `runner_worktree_remove` resolves for its
  # own three-way answer, `runner_available` says why on stderr. docs/RUNNERS.md
  # is explicit that a caller is not obliged to probe first. Found by the
  # independent review.
  orca_cli_resolve || {
    # STDOUT, not stderr, and that distinction is the whole of the fix. `launch`
    # captures stdout only -- `fleet.sh:702` is `runner_worktree_create ... >"$out"`
    # -- and it has to, because stdout is where the worktree path comes back. A
    # reason on stderr goes to the terminal and never into `$out`, so
    # "  could not create it:" printed nothing, which is what round five thought
    # it had fixed. docs/RUNNERS.md says this function prints "the runtime's own
    # words on failure", and the CLI-failure branch below already relays them on
    # stdout. Found by the review OF the round-five fix.
    orca_unavailable_says
    return 1
  }
  local out err rc; out="$(mktemp)"; err="$(mktemp)"
  FLEET_RUN_STDERR="$err" orca_cli "$ORCA_CREATE_DEADLINE" "$out" worktree create \
    --repo "$(orca_resolve_repo_selector "$repo")" \
    --name "$name" \
    --issue "$issue" \
    --no-parent \
    --comment "starting #$issue" \
    --json
  rc=$?
  if [ "$rc" != 0 ]; then
    # stderr FIRST. The three-line budget is the contract's, written for drivers
    # that do not exist yet, and a runtime that prints an error object on stdout
    # would push the actual reason off the end -- reintroducing the empty
    # "could not create it:" against a driver that is obeying the page. Orca
    # writes nothing to stdout on a failed --json call, so this costs nothing
    # here and is the whole fix elsewhere. Found by the independent review.
    cat "$err" "$out" | sed -n '1,3p'
    rm -f "$out" "$err"
    return "$rc"
  fi
  python3 -c '
import json,sys
try:
    print(json.load(sys.stdin)["result"]["worktree"]["path"])
except Exception:
    print("")
' <"$out"
  rm -f "$out" "$err"
  return 0
}

# WHICH REPOSITORY THIS DRIVER IS SCOPED TO, resolved once at source time.
#
# `--repo path:<p>` names a repository ROOT, and half of scripts/fleet/ runs from
# a WORKTREE -- so a naive `path:$REPO_ROOT` names the worktree, the CLI answers
# `repo_not_found`, every listing fails, and the dispatcher skips every pass
# forever. That is the same permanent stall armaatus/autofleet#31 describes,
# arriving through its own fix.
#
# `git rev-parse --git-common-dir` is the root even from inside a worktree: a
# worktree's `.git` points at `<root>/.git`, and this resolves it.
#
# THE GUARD IS ON WHAT `dirname` PRODUCED, not on the string built from it. git
# before 2.31 does not know `--path-format`: it echoes the unrecognised argument
# back and still exits 0, so `common` comes back as `--path-format=absolute` plus
# `.git`, `dirname` refuses the leading `--`, and a guard reading the assembled
# `path:$(dirname ...)` sees a non-empty literal `path:` and lets it through.
#
# ...and it is an ALLOWLIST -- a leading slash -- because that is the whole
# requirement: `path:` names an absolute ROOT. The denylist it replaced (empty,
# or a leading dash) enumerated one way the pre-2.31 answer goes wrong, and
# depended on the local `dirname` refusing `--path-format=absolute` rather than
# reading a slashless string as a path and answering `.`. It also had nothing at
# all to say about the case that reaches this by a different door: a git that
# prints the common dir RELATIVELY, where `dirname` answers `.` and `path:.`
# ships -- the same permanent `repo_not_found` stall this function exists to
# prevent, arriving through the guard against it. armaatus/autofleet#71.
orca_resolve_repo_selector() {
  # `$1` is the checkout to resolve FROM, defaulting to this one. It exists so
  # `runner_worktree_create` can pass the `<repo>` its contract gives it rather
  # than having the parameter go dead -- the caller names the repository, the
  # driver turns that into whatever selector its runtime speaks.
  local from="${1:-$REPO_ROOT}" common root
  common="$(git -C "$from" rev-parse --path-format=absolute \
              --git-common-dir 2>/dev/null)" || common=""
  if [ -n "$common" ]; then
    root="$(dirname "$common" 2>/dev/null)" || root=""
    case "$root" in /*) ;; *) root="" ;; esac
    [ -n "$root" ] && { printf 'path:%s\n' "$root"; return 0; }
  fi
  printf 'path:%s\n' "$from"
}
ORCA_REPO_SELECTOR="$(orca_resolve_repo_selector)"

# Every worktree the runner is managing, as `path<TAB>branch<TAB>issue` lines.
# `-` where the runner has no answer for a field; nothing here may be JSON,
# because a caller that parses JSON has hardcoded this runner.
#
# The main worktree and archived ones are left out: the fleet counts these to
# decide whether it may launch, and neither of those is a slot.
#
# NON-ZERO WHEN THE ANSWER COULD NOT BE READ, which is not the same as "nothing
# is running". Reading a failed call as zero live worktrees is how one transient
# hiccup turns into three duplicate worktrees for issues that already have one.
runner_worktree_list() {
  local out rc; out="$(mktemp)"
  # SCOPED. `orca worktree list` is machine-wide, and every caller resolves the
  # issue numbers against THIS repository -- so one foreign worktree inflated
  # `live`, and `foundation_in_flight` asked about an issue number that does not
  # exist here, could not read its labels, and held the fleet indefinitely after
  # a single line in the log. armaatus/autofleet#31.
  orca_json "$out" worktree list --repo "$ORCA_REPO_SELECTOR" || {
    # NAMES THE SELECTOR IT ASKED WITH, on stderr, which the dispatcher's log
    # captures. A refused selector and an app that is down both surface as "could
    # not read the worktree list", and they need opposite responses -- one is
    # `repo_not_found` on a path, the other is a runtime to restart. The pre-#1
    # branch named the selector in `fleet.sh`'s own message; that string is a
    # runner's, so hard rule 2 moved it here rather than losing it. Found by the
    # independent review of the rebase that lost it.
    echo "orca: could not list worktrees for $ORCA_REPO_SELECTOR" >&2
    rm -f "$out"; return 1; }
  python3 -c '
import json, sys
try:
    worktrees = json.load(open(sys.argv[1]))["result"]["worktrees"]
except Exception:
    raise SystemExit(1)
for w in worktrees:
    if w.get("isMainWorktree") or w.get("isArchived"):
        continue
    print(w["path"], w.get("branch") or "-", w.get("linkedIssue") or "-", sep="\t")
' "$out"
  rc=$?
  rm -f "$out"
  return $rc
}

# This worktree's linked issue, if it has one.
#
# THREE answers, and the middle one is the whole reason this function is not a
# boolean: 0 with the issue on stdout, 2 for "there is no linked issue", 1 for
# "the runtime would not say". A hook that reads 1 as 2 announces that a
# worktree the fleet linked to an issue has none -- see orca_cli_resolve, which
# exists because that is exactly what happened.
runner_worktree_issue() {
  local out rc; out="$(mktemp)"
  orca_json "$out" worktree current || { rm -f "$out"; return 1; }
  python3 -c '
import json, sys
try:
    wt = json.load(open(sys.argv[1]))["result"]["worktree"]
except Exception:
    raise SystemExit(1)
for key in ("linkedIssue", "linkedLinearIssue", "linkedWorkItem"):
    if wt.get(key):
        print(wt[key] if isinstance(wt[key], (str, int)) else "linked")
        raise SystemExit(0)
raise SystemExit(2)
' "$out"
  rc=$?
  rm -f "$out"
  # A body that would not parse is the runtime failing to answer, not a worktree
  # without an issue: SystemExit(1) above and a dead CLI have to land together.
  return $rc
}

# Set metadata on one worktree, as `key value` PAIRS.
#
# Pairs rather than a single key because every caller sets a status and the
# comment explaining it, and two calls would put a new status on the board
# beside the previous line for as long as the second call takes -- or forever,
# if it is the one that fails. The keys the fleet uses are `workspace-status`
# and `comment`.
#
# Prints the runtime's own words on failure and nothing at all on success. The
# caller decides how loud that is; on this board it is said rather than
# swallowed, because a card that did not update is a status surface showing
# something that is not true.
#
# Any non-zero means the card was not updated. 2 specifically means the CALLER
# is malformed rather than the runtime unreachable -- see below.
#
# An odd number of arguments is REFUSED rather than rounded down. A caller that
# means `comment "..."` and passes `comment` alone would otherwise get a board
# update that silently did less than it was asked for -- and on a status surface
# that is the same failure as not updating at all, minus the message. It is also
# what keeps the expansion below safe on bash 3.2, where `"${args[@]}"` on an
# empty array under `set -u` is a fatal "unbound variable" rather than nothing.
runner_worktree_set() {
  local path="$1"; shift
  local args=() rc out
  if [ $# -lt 2 ] || [ $(($# % 2)) != 0 ]; then
    echo "runner_worktree_set: expected key/value pairs, got: $*" >&2
    return 2
  fi
  while [ $# -ge 2 ]; do
    args+=("--$1" "$2"); shift 2
  done
  # Same reason as create, one function up: `card` logs
  # "board update FAILED (rc $rc)" and then whatever this printed, so a failed
  # resolve that says nothing is a refusal with no cause under it. `orca_cli`
  # returns before it writes $out, so the relay below has nothing to relay.
  orca_cli_resolve || {
    orca_unavailable_says
    return 1
  }
  # STDERR TO ITS OWN FILE, and relayed FIRST -- the same shape
  # `runner_worktree_create` was given, for the same reason. With
  # `FLEET_RUN_CAPTURE_STDERR=1` the two streams arrive merged in the CLI's own
  # order, so a runtime that prints an error object on stdout pushes the real
  # reason past the three-line budget `docs/RUNNERS.md` states as an obligation
  # on drivers that do not exist yet. Orca writes nothing to stdout on a failed
  # `--json` call, so this costs nothing here -- and it means the page's two
  # relays behave the same way rather than being parallel only on paper. Found
  # by the independent review, which noted this was the one relay that could not
  # honour the ordering half of its own contract.
  local err; err="$(mktemp)"
  out="$(mktemp)"
  FLEET_RUN_STDERR="$err" orca_cli "$ORCA_DEADLINE" "$out" \
    worktree set --worktree "path:$path" "${args[@]}" --json
  rc=$?
  [ "$rc" = 0 ] || cat "$err" "$out" | sed -n '1,3p'
  rm -f "$err"
  rm -f "$out"
  return $rc
}

# Remove one worktree. 0 only if it is REALLY gone, 1 if the runtime answered
# and refused, 2 if it never answered at all. The caller acts on the difference:
# a refusal is a decision about THIS worktree and is not worth retrying, while a
# deadline is Orca.app restarting and says nothing about the worktree.
#
# ## Why it forces on the second attempt
#
# A repository with a real submodule makes `git worktree remove` refuse outright:
#
#   fatal: working trees containing submodules cannot be moved or removed
#
# So the plain call fails on every worktree the fleet has ever created, every
# time, and it is not intermittent. Three accumulated in about eighteen hours on
# 2026-09-07, each holding four containers, two ports and four volumes that come
# back on every `docker start` under `restart: unless-stopped`.
#
# Forcing is safe HERE specifically: every caller checks the worktree holds
# nothing first. --force forces the worktree removal, not the branch deletion.
#
# ## Why the archive hook does not run through the CLI (armaatus/rommsync-nx#163)
#
# `orca worktree rm --run-hooks` runs orca.yaml's archive hook -- archive.sh,
# which takes the stack and its volumes down -- and it runs it BEFORE Orca
# decides whether it will remove the worktree at all. Orca then refuses (a dirty
# working tree, the submodule), and "could not remove it" has already destroyed
# the thing the worktree could not be worked in without:
#
#   16:49:15  #122: PR #159 is merged; marking it done and removing the worktree
#   16:49:32    could not remove it; sweep later with ./scripts/fleet/reap.sh
#
# That agent was mid-test-run. A suite failed after 90s with ~130 tests skipped
# behind it, and the fixture had lost its scan and its token -- none of which the
# log above suggests. So the removal is attempted with no hooks at all, and only
# a worktree that is genuinely GONE gets its stack swept, by the caller. A
# refusal now changes nothing.
#
# The second attempt is skipped after a deadline rather than after a refusal: a
# first call that hit it means nothing is answering, and a second 180s spent
# proving that doubles what a poll costs while Orca.app restarts.
runner_worktree_remove() {
  local path="$1" deadline="${2:-180}" out rc
  # A worktree that is ALREADY gone is gone, and saying so needs no runtime at
  # all. Asked before the resolve, or `reap_merged` could not release a slot with
  # nothing left in it while Orca was down -- which is the same held slot this
  # function's three-way answer exists to avoid.
  [ -d "$path" ] || return 0
  # Then resolved HERE rather than left to orca_cli, because this is the one
  # function whose rc 1 means something specific -- "answered and refused" -- and
  # orca_cli answers a failed resolve with 1 like everything else. Without this
  # line an Orca that is not running came back as a decision about THIS worktree,
  # with `the removal refused:` and nothing after it, and the dispatcher parked a
  # slot that only needed retrying. Design note 2 inverted inside the function
  # added to stop exactly that. Both found by the independent review.
  orca_cli_resolve || return 2
  out="$(mktemp)"
  FLEET_RUN_CAPTURE_STDERR=1 orca_cli "$deadline" "$out" \
    worktree rm --worktree "path:$path" --json
  rc=$?
  if [ ! -d "$path" ]; then rm -f "$out"; return 0; fi
  if [ "$rc" != 124 ]; then
    FLEET_RUN_CAPTURE_STDERR=1 orca_cli "$deadline" "$out" \
      worktree rm --worktree "path:$path" --force --json
    rc=$?
    if [ ! -d "$path" ]; then rm -f "$out"; return 0; fi
  fi
  if [ "$rc" = 124 ]; then rm -f "$out"; return 2; fi
  sed -n '1,3p' "$out"
  rm -f "$out"
  return 1
}

# ------------------------------------------------------------------ the build
#
# THE SAME COMMAND, IN A TAB. `fleet_build_command_line` composes it once in
# lib.sh and neither driver gets to assemble its own -- that is what makes
# "headless is the CI path, Orca is the desk path" a statement about WHERE a
# build runs rather than about what it does.
#
# What this driver no longer does is drive a session. It does not type, does not
# press Return, does not read a composer draft and does not classify a tab as
# working or waiting. Those six contract functions are gone with the five
# subsystems that used them.

# The live terminal in one worktree, if there is one. PRIVATE to this driver --
# it is not a contract function, and nothing outside this file may call it.
orca_terminal_for_path() {
  local out; out="$(mktemp)"
  orca_json "$out" terminal list || { rm -f "$out"; return 1; }
    # MATCHED ON THE TITLE AS WELL AS THE PATH. `runner_build_start` names the tab
  # `#<issue>`, and a worktree can hold several tabs -- a shell, a log, an agent
  # a person opened. On the path alone a stop interrupted and CLOSED whichever
  # came back first, which is somebody else's tab and leaves the build writing
  # into a worktree that is being removed. Found by the local `/code-review`
  # pass.
  ORCA_TERMINAL_PATH="$1" ORCA_TERMINAL_TITLE="${2:-}" python3 -c '
import json, os, sys
try:
    terminals = json.load(open(sys.argv[1]))["result"]["terminals"]
except Exception:
    raise SystemExit(1)
want = os.environ["ORCA_TERMINAL_PATH"]
title = os.environ.get("ORCA_TERMINAL_TITLE") or ""
for t in terminals:
    if t.get("worktreePath") != want or t.get("orphaned"):
        continue
    if title and (t.get("title") or "") != title:
        continue
    print(t["handle"])
    break
' "$out"
  local rc=$?
  rm -f "$out"
  return $rc
}

# Start the build in $1 for issue $2, in a terminal tab.
#
# `--command` is what makes the tab the build rather than a shell beside it: the
# terminal starts, runs the line, and the maintainer watches the same stdout the
# headless driver sends to /dev/null. The line writes its own exit status, so
# `runner_build_state` needs nothing from the app -- which matters, because a
# terminal is not a process this driver can wait on.
runner_build_start() {
  local path="$1" issue="$2" dir
  dir="$(fleet_build_dir "$issue")"
  [ -r "$dir/prompt" ] && [ -r "$dir/system.md" ] || {
    echo "orca: no prompt for #$issue in $dir" >&2; return 1; }
  [ "$(runner_build_state "$path" 2>/dev/null)" = running ] && return 0
  orca_cli_resolve || { orca_unavailable_says >&2; return 1; }
  fleet_build_started "$dir" "$path" "$issue" || return 1
  local out rc; out="$(mktemp)"
  FLEET_RUN_CAPTURE_STDERR=1 orca_cli "$ORCA_DEADLINE" "$out" terminal create \
    --worktree "path:$path" \
    --title "#$issue" \
    --command "$(fleet_build_command_line "$dir")" \
    --json
  rc=$?
  if [ "$rc" != 0 ]; then
    # AT MOST THREE LINES of the runtime's own words, the driver's bound.
    sed -n '1,3p' "$out" >&2
    rm -f "$out"
    # ...and the index goes with it, or `runner_build_state` would answer
    # "running" for a build that was never started -- the file-only reading
    # `fleet_build_state_of` falls back to when there is no pid.
    fleet_build_forget_path "$path"
    return 1
  fi
  rm -f "$out"
  return 0
}

runner_build_state() { fleet_build_state_of "$1"; }

# Stop the build by interrupting the terminal hosting it, then closing the tab.
#
# The interrupt FIRST and the close after: closing a tab mid-write leaves the
# build's own files half-written, and the interrupt is what lets the line reach
# the `printf` that records its exit status. Silent and always 0 -- every caller
# reaches here on a path where the worktree is going away regardless.
runner_build_stop() {
  local handle issue
  orca_cli_resolve || return 0
  # The tab this driver named, by the title it gave it. An issue it cannot
  # resolve falls back to the worktree, which is the pre-title behaviour and
  # still better than nothing.
  issue="$(fleet_build_dir_for_path "$1" 2>/dev/null)" && issue="#${issue##*/}" || issue=""
  handle="$(orca_terminal_for_path "$1" "$issue")" || return 0
  [ -n "$handle" ] || return 0
  orca_cli "$ORCA_SEND_DEADLINE" /dev/null \
    terminal send --terminal "$handle" --interrupt --json >/dev/null 2>&1
  orca_cli "$ORCA_SEND_DEADLINE" /dev/null \
    terminal close --terminal "$handle" --json >/dev/null 2>&1
  return 0
}
