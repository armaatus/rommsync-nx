#!/usr/bin/env bash
# The headless runner: a plain `git worktree` and one non-interactive
# `claude -p` per build. No app, no terminal, no tab, nothing that can sit
# waiting for input.
#
# This is the default driver (scripts/fleet/config.sh) and the one CI runs. It
# needs `git`, `gh` and the build command on PATH and nothing else, which is the
# whole point of armaatus/autofleet#151: the fleet had never run anywhere but
# one macOS desk because the runtime it drove only exists there.
#
# WHAT IT REPLACED, so the shape below reads as a decision rather than a
# reduction. The interactive runner needed five subsystems to keep one session
# alive -- an autostart watcher that pressed Return on a drafted prompt, a
# "waiting for input" detector, a time-box that interrupted with a turn, a
# context recycle, and a handoff note the agent wrote before being cleared.
# Every one of them existed because a session that is never allowed to END has
# to be managed. `claude -p` ends: at the PR, at `--max-turns`, or at
# `--max-budget-usd`. The branch and the PR are the state, so a resume is a
# second `claude -p` in the same worktree and there is nothing to hand off.
#
# WHERE THE STATE LIVES. Under `$(fleet_build_dir <issue>)`, NOT inside the
# worktree: `self-review.sh` refuses a dirty tree, and a build log written next
# to the code is either a dirty tree or a .gitignore entry every host repo would
# have to vendor. It also has to outlive the worktree -- `cost.sh` reports on
# issues whose worktree was removed hours ago.

# The per-call deadline, for the git and gh calls below.
#
# Not for the BUILD: a build runs for hours and is not a call anyone waits on.
# `runner_build_start` returns as soon as the child is spawned, and
# `runner_build_state` answers from a pid file, so neither of them can block.
# That is the contract's "every call has a deadline" honoured by having no call
# that could take one.
runner_set_deadline() { HEADLESS_DEADLINE="$1"; }
: "${HEADLESS_DEADLINE:=60}"

# Where worktrees are created. One directory per repository leaf, so two host
# projects on one machine do not collide, and under $FLEET_DIR rather than
# beside the checkout because a sibling directory full of worktrees is a thing
# people commit by accident.
headless_tree_root() {
  printf '%s\n' "${AUTOFLEET_WORKTREE_ROOT:-$FLEET_DIR/trees}"
}

# THE ISSUE A WORKTREE BELONGS TO, kept by the driver.
#
# docs/RUNNERS.md names this as one of the two things a git-worktree driver has
# to answer for itself: the app-backed runner recorded the issue on the worktree
# object, and git has nowhere to put it. Not `git config --worktree` -- that
# needs `extensions.worktreeConfig`, which is a repository-wide flag this driver
# would be turning on in somebody else's repo. Not a file inside the worktree:
# see the header on why nothing is written in there.
#
# So: one file per worktree, named by the path with `/` folded to `%`, holding
# the issue number. `%` because it cannot appear in a path component that git
# would accept as a branch name and, unlike a hash, the directory is readable
# when something has gone wrong.
headless_link_dir() { printf '%s\n' "$FLEET_DIR/headless/links"; }
headless_link_file() {
  # RESOLVED FIRST. `git worktree list` and `rev-parse --show-toplevel` both
  # report a path with every symlink resolved; a caller that hands us the
  # unresolved one -- which is what `$AUTOFLEET_DIR` under a symlinked `$HOME`
  # gives, and what macOS `/tmp` gives for free -- writes its link under one
  # name and reads it under another. Every worktree then lists with issue `-`,
  # the dispatcher believes nothing is in flight, and it launches a second
  # worktree for an issue already running. Found by the local `/code-review`
  # pass, which could not run the probe and marked it plausible; it reproduces.
  local path="$1" dir base
  dir="$(dirname "$path")"; base="$(basename "$path")"
  if dir="$(cd "$dir" 2>/dev/null && pwd -P)"; then path="$dir/$base"; fi
  printf '%s/%s\n' "$(headless_link_dir)" "$(printf '%s' "$path" | tr '/' '%')"
}

# Is this driver usable at all, and SAY WHY when it is not.
#
# GIT AND GH, AND NOT THE BUILD COMMAND. This asked for all three for one round
# and the CI suite went red across eleven `review_mode` phases with
# `headless: not on PATH: claude`: `fleet.sh` probes the driver at SOURCE time
# and dies on a no, so on a runner with no agent CLI installed EVERY fleet
# command stopped -- the reviewer, the validator, `cost`, the whole review
# pipeline, none of which builds anything. It passed locally for the one reason
# that makes this class of bug ship: the machine it was written on has `claude`.
#
# The question this function answers is "can this driver create and remove
# worktrees". Whether a build can RUN is `start_build`'s question, asked once
# per launch and answered loudly there -- which is also the only place that
# knows a build is about to happen. Found by the suite in CI.
runner_available() {
  local missing=""
  for tool in git gh; do
    command -v "$tool" >/dev/null 2>&1 || missing="${missing:+$missing, }$tool"
  done
  [ -z "$missing" ] && return 0
  echo "headless: not on PATH: $missing" >&2
  return 1
}

runner_dispatcher_hint() {
  printf 'run `./scripts/fleet/fleet.sh run --auto` in a terminal, or under nohup\n'
}

# Create the worktree. Prints its path on success, the reason on STDOUT on
# failure -- stdout because `launch` captures stdout only, and a reason on
# stderr never reaches fleet.log. Same rule as the app-backed driver, and the
# same reason: "could not create it" with nothing after it reads identically
# whether the branch already exists or the disk is full.
runner_worktree_create() {
  local repo="$1" name="$2" issue="$3"
  local root path out rc
  root="$(headless_tree_root)/$(basename "$repo")"
  path="$root/$name"
  mkdir -p "$root" "$(headless_link_dir)" || { echo "could not create $root"; return 1; }
  [ -e "$path" ] && { echo "$path already exists"; return 1; }
  # A BRANCH NAME NOTHING ELSE HAS. `launch` derives it from the issue number
  # and title, so it is the same name every time that issue is started -- and
  # `git worktree remove` does not delete the branch it made. So
  # `fleet.sh retry 42`, and any relaunch after a release, died on `a branch
  # named '42-foo' already exists`, every poll, forever. Suffixed rather than
  # forced: `-B` would reset a branch that may hold the previous attempt's
  # commits, which is the one thing the reap is careful not to throw away.
  # Found by the local `/code-review` pass.
  local base="$name" n=2
  while git -C "$repo" show-ref --verify --quiet "refs/heads/$name"; do
    name="$base-$n"; n=$((n + 1))
    [ "$n" -gt 99 ] && { echo "a hundred branches are already named $base-*"; return 1; }
  done

  out="$(mktemp)"
  # FROM THE REMOTE TIP WHEN THERE IS ONE, and from HEAD when there is not. The
  # dispatcher runs for hours and a build that branched off a stale local main
  # re-does merged work -- but a repository with no `origin`, which is what
  # every fixture and every offline host is, must still get a worktree. The
  # fetch is inside the deadline with everything else. The comment here promised
  # the fetch while the code passed `HEAD`; found by both local review passes.
  local from=HEAD upstream
  if upstream="$(git -C "$repo" symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>/dev/null)" \
     && [ -n "$upstream" ]; then
    fleet_run_with_deadline "$HEADLESS_DEADLINE" /dev/null \
      git -C "$repo" fetch --quiet origin && from="$upstream"
  fi
  FLEET_RUN_CAPTURE_STDERR=1 fleet_run_with_deadline "$HEADLESS_DEADLINE" "$out" \
    git -C "$repo" worktree add -b "$name" "$path" "$from"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    # AT MOST THREE LINES of the runtime's own words -- the driver's bound, not
    # the caller's, so a verbose git cannot flood fleet.log.
    head -3 "$out"; rm -f "$out"; return 1
  fi
  rm -f "$out"
  printf '%s\n' "$issue" >"$(headless_link_file "$path")"
  printf '%s\n' "$path"
}

# `path<TAB>branch<TAB>issue` for every worktree of THIS repository.
#
# SCOPED to the repo, like the app-backed driver's listing, for the reason
# recorded there: a machine-wide listing put a foreign worktree in `live`, and
# the foundation hold then waited on an issue number that does not exist here.
# `git worktree list` is already repo-scoped, which is one whole class of bug
# this driver does not have.
runner_worktree_list() {
  local out rc; out="$(mktemp)"
  fleet_run_with_deadline "$HEADLESS_DEADLINE" "$out" \
    git -C "$REPO_ROOT" worktree list --porcelain
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "headless: could not list worktrees in $REPO_ROOT" >&2
    rm -f "$out"; return 1
  fi
  # The main worktree is skipped: the fleet owns the ones it opened, and the
  # checkout the dispatcher itself runs in is not one of them.
  # THE MAIN WORKTREE, and `--path-format` is GUARDED. git before 2.31 does not
  # know the option, echoes it back and still exits 0 -- so `main` came back as
  # the flag itself, the comparison below never matched, and the dispatcher's
  # own checkout was emitted as a fleet-owned worktree with no issue. The other
  # driver carries the same guard and the reason (armaatus/autofleet#71); found
  # here by `/mattpocock-skills:code-review`.
  local main; main="$(git -C "$REPO_ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
  case "$main" in
    --*|'') main="$(cd "$REPO_ROOT" && cd "$(git rev-parse --git-common-dir 2>/dev/null)" && pwd -P)" ;;
  esac
  main="${main%/.git}"
  local path="" branch=""
  while IFS= read -r line; do
    case "$line" in
      "worktree "*)
        path="${line#worktree }"; branch="" ;;
      "branch "*)
        branch="${line#branch }"; branch="${branch#refs/heads/}" ;;
      "")
        headless_emit_row "$path" "$branch" "$main"; path=""; branch="" ;;
    esac
  done <"$out"
  headless_emit_row "$path" "$branch" "$main"
  rm -f "$out"
}

headless_emit_row() {
  local path="$1" branch="$2" main="$3" issue
  [ -n "$path" ] || return 0
  [ "$path" = "$main" ] && return 0
  issue="$(cat "$(headless_link_file "$path")" 2>/dev/null)"
  printf '%s\t%s\t%s\n' "$path" "${branch:--}" "${issue:--}"
}

# THIS worktree's linked issue. Three answers, and the middle one is why this is
# not a boolean: 0 with the number, 2 for "this worktree has no linked issue",
# 1 for "could not tell". A caller that reads 1 as 2 announces that a worktree
# the fleet linked has none, which is how a provisioned worktree sat idle.
runner_worktree_issue() {
  local top issue
  top="$(git rev-parse --show-toplevel 2>/dev/null)" || return 1
  issue="$(cat "$(headless_link_file "$top")" 2>/dev/null)" || return 2
  [ -n "$issue" ] || return 2
  printf '%s\n' "$issue"
}

# The app-backed driver writes a card; there is no card here. A no-op that
# SUCCEEDS is the honest answer -- the contract's callers use this to show a
# person what is happening, and failing it would make the dispatcher log a
# problem that does not exist.
runner_worktree_set() { return 0; }

# Remove the worktree. Three answers, per the contract: 0 gone, 1 the runtime
# refused, 2 the runtime could not be reached at all. There is no runtime here,
# so 2 is unreachable and that is stated rather than left to be inferred.
runner_worktree_remove() {
  local path="$1" deadline="${2:-$HEADLESS_DEADLINE}" out rc
  # A worktree that is ALREADY gone is gone, and saying so needs nothing else
  # to work: `reap_merged` could otherwise fail to release a slot with nothing
  # left in it, which is the same held slot this three-way answer avoids.
  # ...and the registry goes WITH it. Returning 0 here without pruning left
  # `git worktree list --porcelain` still emitting the stale entry and the link
  # file still naming its issue, so the row came back with an issue number on
  # it: a phantom slot, counted against AUTOFLEET_MAX forever, and `in_flight`
  # answering yes for an issue nobody is working on. Found by
  # `/mattpocock-skills:code-review`.
  if [ ! -d "$path" ]; then
    rm -f "$(headless_link_file "$path")"
    fleet_build_forget_path "$path"
    git -C "$REPO_ROOT" worktree prune >/dev/null 2>&1
    return 0
  fi
  # The build first: `git worktree remove` on a tree a `claude -p` is still
  # writing to races the agent, and the loser is the worktree.
  runner_build_stop "$path" >/dev/null
  out="$(mktemp)"
  FLEET_RUN_CAPTURE_STDERR=1 fleet_run_with_deadline "$deadline" "$out" \
    git -C "$REPO_ROOT" worktree remove --force "$path"
  rc=$?
  if [ "$rc" -ne 0 ] && [ -e "$path" ]; then
    head -3 "$out" >&2; rm -f "$out"; return 1
  fi
  rm -f "$out" "$(headless_link_file "$path")"
  fleet_build_forget_path "$path"
  git -C "$REPO_ROOT" worktree prune >/dev/null 2>&1
  return 0
}

# ------------------------------------------------------------------ the build

# Start the build in $1 for issue $2, and return as soon as it is running.
#
# `set -m` before the spawn, so the child gets its own PROCESS GROUP: the build
# command is advertised as a wrapper seam (AUTOFLEET_BUILD_CMD), so the process
# actually holding the credentials is routinely a CHILD of what we forked, and
# signalling the direct child orphans a full-budget run nobody is counting.
# Same shape, same reason, as review.sh and self-review.sh -- see
# `fleet_kill_group` in lib.sh.
runner_build_start() {
  local path="$1" issue="$2" dir
  dir="$(fleet_build_dir "$issue")"
  [ -r "$dir/prompt" ] && [ -r "$dir/system.md" ] || {
    echo "headless: no prompt for #$issue in $dir" >&2; return 1; }

  # Already running is SUCCESS, not an error: the dispatcher calls this to make
  # sure a build is up, and a second `claude -p` in one worktree is two agents
  # editing one tree.
  [ "$(runner_build_state "$path" 2>/dev/null)" = running ] && return 0

  fleet_build_started "$dir" "$path" "$issue" || return 1
  local line; line="$(fleet_build_command_line "$dir")"
  set -m
  (
    cd "$path" || exit 127
    # The line itself writes result.json, build.log and rc -- see
    # `fleet_build_command_line`. Stdout is discarded HERE and nowhere else:
    # the app-backed driver runs the same line in a terminal, where that same
    # stdout is what the maintainer watches.
    # `</dev/null`, and it is not decoration. The spawn is under `set -m`, so
    # the child gets its own process group and INHERITS the dispatcher's
    # terminal; a background process group that reads from that terminal is sent
    # SIGTTIN and STOPS. The pid then stays alive, `kill -0` keeps answering
    # yes, and `fleet_build_state_of` reports `running` for a build that will
    # never move again. `self-review.sh` documents this exact trap and guards
    # against it; this spawn did not. Found by the local `/code-review` pass.
    # `exec`, and it is the pid file's whole correctness. Without it the
    # subshell stays alive as the parent of the build, and `$!` -- the only pid
    # this driver records -- names a process whose command line is the
    # DISPATCHER's (`fleet.sh run --auto`), not the build's. `headless_pid_is_ours`
    # then answered no for every build the dispatcher ever started, the kill was
    # skipped, the pid file was removed, and `stop --now` reported a stop over a
    # `claude -p` that was still running (#163). Exec replaces the subshell in
    # place: same pid, same process group, and a command line that names this
    # fleet's build directory, which is what the identity check reads.
    exec bash -c "$line" </dev/null >/dev/null 2>&1
  ) &
  local pid=$!
  set +m
  printf '%s\n' "$pid" >"$dir/pid"
  return 0
}

# `running`, or `exited <rc>`, from lib.sh's one reader. The driver adds
# nothing: a background child and a terminal-hosted command are described by
# the same two files, and the pid this driver writes is what makes a KILLED
# build report as exited rather than as running forever.
runner_build_state() { fleet_build_state_of "$1"; }

# Stop the build in $1, if there is one. Idempotent, and silent about a build
# that is already gone: every caller reaches here on a path where the worktree
# is going away regardless.
#
# 0 WHEN THERE IS NOTHING LEFT RUNNING, and non-zero with the surviving pid on
# stdout when there is. It returned 0 unconditionally, so `cmd_stop` printed
# `stopped the build for #N` whether or not anything had been stopped -- and the
# case it printed it in was the only case that mattered (#163). The pid goes on
# stdout rather than into a message because the caller is driver-agnostic: it
# knows there is a build, not where the driver keeps its handle on it.
runner_build_stop() {
  local dir pid rc=0
  dir="$(fleet_build_dir_for_path "$1")" || return 0
  pid="$(cat "$dir/pid" 2>/dev/null)" || return 0
  [ -n "$pid" ] || return 0
  if headless_pid_alive "$pid" && headless_pid_is_ours "$pid"; then
    fleet_kill_group "$pid"
    # A GRACE AFTER THE KILL. `fleet_kill_group` returns when SIGKILL is SENT,
    # not when it has been delivered, and a process in uninterruptible sleep
    # dies a moment later -- read at once, it still answers `kill -0` and is not
    # yet a zombie, and the stop would report a failure over a build that is
    # dying. Three seconds is the bound the test uses for the same question.
    local waited=0
    while headless_pid_alive "$pid" && [ "$waited" -lt 15 ]; do
      sleep 0.2; waited=$((waited + 1))
    done
    if headless_pid_alive "$pid"; then
      # THE PID FILE STAYS. It has just been confirmed alive AND ours, which is
      # the one case the reuse precaution below was never about -- and without
      # it the next `stop --now` finds no pid, returns 0 at the top of this
      # function, and prints "stopped" over the process this call just named
      # as surviving (#163's lie, one command later).
      printf '%s\n' "$pid"
      return 1
    else
      # A RECORD THAT THE BUILD WAS KILLED. The command line writes its own exit
      # status last and a killed one never reaches that line, so on files alone
      # the state reader sees a worktree, no rc and no pid and answers `running`
      # -- forever, which is a restarted dispatcher waiting out its budget on a
      # build that was killed hours ago.
      #
      # The marker rather than an `rc` of 143 written here: the state reader
      # renders it as `exited 143` either way, but an `rc` alone would make a
      # deliberate stop indistinguishable from a build that died at its budget,
      # and `build_exited` acts on that difference -- it would comment on the
      # issue of every worktree `reap_abandoned` stopped on purpose. See
      # `fleet_build_mark_stopped` in lib.sh.
      fleet_build_mark_stopped "$dir"
    fi
  fi
  # THE PID FILE GOES EITHER WAY. It outlives a `kill -9` and a reboot, and the
  # number in it is then whatever the system reused it for -- so a file left
  # behind is a TERM and a KILL aimed at an unrelated process group of the
  # user's, the next time anything stops this worktree. The watcher stop this
  # replaced carried exactly this precaution and it was not carried over. Found
  # by the local `/code-review` pass.
  rm -f "$dir/pid"
  return "$rc"
}

# Is this pid a process that is still doing something?
#
# `kill -0` alone is not that question. A build the dispatcher forked is a job
# of the dispatcher shell, which never waits on it, so a killed one stays a
# ZOMBIE until its parent exits -- and `kill -0` goes on answering yes about it.
# A stop that read that as "still running" would report failure for every build
# it successfully killed, which is the same lie as the one above with the sign
# flipped. `ps` is asked for the state and a `Z` is read as gone; a `ps` that
# cannot answer at all leaves `kill -0`'s answer standing.
headless_pid_alive() {
  kill -0 "$1" 2>/dev/null || return 1
  case "$(ps -o state= -p "$1" 2>/dev/null)" in
    *Z*) return 1 ;;
  esac
  return 0
}

# Is this pid the build we forked, rather than whatever the system has since
# reused the number for?
#
# The command line is what says so: the pid recorded by `runner_build_start` IS
# the `bash -c` running the generated line -- see the `exec` there -- and that
# line always names this fleet's build directory. `ps` rather than a stored
# start time, because `ps` is what every machine has. A `ps` that cannot answer is read as "not ours", which errs
# towards not signalling -- the direction where the cost is a build that outlives
# its worktree rather than a stranger's process group killed.
#
# `grep` without `-q`: every caller sources lib.sh under `pipefail`, and `-q`
# exits on the first match, so `ps` can write into a closed pipe and the
# pipeline is 141 for a probe that MATCHED. CLAUDE.md carries the rule.
# `-ww`: BSD `ps` truncates a command line to the terminal width, and the build
# line names the build directory some forty characters in -- so under a fleet
# directory with a long path the identity check would read a truncated line, find
# nothing, and decline to kill a build that is ours.
headless_pid_is_ours() {
  ps -ww -o command= -p "$1" 2>/dev/null | grep -F "$FLEET_BUILDS" >/dev/null
}
