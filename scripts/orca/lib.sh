#!/usr/bin/env bash
# Shared helpers for the Orca hooks. Sourced, never executed.

# The agent autostart watcher, which outlives setup.sh and is the one thing a
# worktree removal has to stop that is not a container. Two callers with opposite
# timing: archive.sh runs INSIDE the worktree and reads the pidfile as it goes,
# while fleet.sh has to read it before a removal it may not get and signal only
# after one it did -- so reading and stopping are separate.
#
# The identity check is why this is shared rather than copied: a pidfile outlives
# a `kill -9` and a reboot, and signalling a recycled pid means signalling an
# unrelated process of the user's. One copy of that reasoning, not two.
#
# Non-zero means nothing was signalled, so a caller can report only a real stop.
orca_read_autostart_watcher() {
  cat "$1/.orca/agent-autostart.pid" 2>/dev/null || true
}
orca_stop_autostart_watcher() {
  local pid="${1:-}"
  [ -n "$pid" ] || return 1
  kill -0 "$pid" 2>/dev/null || return 1
  ps -o command= -p "$pid" 2>/dev/null | grep -q 'agent-autostart' || return 1
  kill "$pid" 2>/dev/null || true
  return 0
}

# A worktree's identity -- its compose project name and its ports -- is a
# pure function of its absolute path. env.sh writes the result to .env when the
# worktree is created; teardown recomputes it instead of reading .env back.
#
# That asymmetry is deliberate. .env is generated, gitignored, and written by a
# hook that can itself fail half way, so teardown must not depend on it: an
# absent or truncated COMPOSE_PROJECT_NAME sends `docker compose down` to the
# compose default (`rmx-local`) and leaves this worktree's three containers, four
# volumes and network running under `restart: unless-stopped`, holding two ports,
# with nothing left on disk to identify them by.
#
# Returns non-zero rather than exiting if no SHA-1 tool is available: under
# `set -e` an aborting derivation would guarantee the very leak it exists to
# prevent, so callers get to fall back on .env instead.
orca_derive_env() {
  local root="$1" hex=""

  # sha1 of the path, first two bytes, mod 2000 -- kept bit-for-bit compatible
  # with the python implementation this replaces, because changing it would
  # rename every existing stack out from under the worktree that created it.
  # Shelling out to a hash tool rather than python3 keeps teardown working on a
  # machine where the interpreter has gone away.
  if command -v shasum >/dev/null 2>&1; then
    hex="$(printf %s "$root" | shasum -a 1 | cut -c1-4)"
  elif command -v sha1sum >/dev/null 2>&1; then
    hex="$(printf %s "$root" | sha1sum | cut -c1-4)"
  elif command -v python3 >/dev/null 2>&1; then
    hex="$(python3 -c "
import hashlib,sys
print(hashlib.sha1(sys.argv[1].encode()).hexdigest()[:4])
" "$root")"
  fi
  case "$hex" in [0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;; *) return 1 ;; esac

  orca_slug="$(basename "$root" | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9' '-' | sed 's/^-//;s/-$//')"
  orca_offset=$(( 0x$hex % 2000 ))
  orca_romm_port=$((21000 + orca_offset))
  orca_proxy_port=$((23000 + orca_offset))
  # The TLS terminator in front of RomM (server/testing/docker-compose.yml's
  # `tls` profile). Derived like the other two rather than picked at run time,
  # for the same reason: teardown recomputes ports instead of reading .env back,
  # and a port nothing can re-derive is a port nothing can release.
  orca_tls_port=$((25000 + orca_offset))
  # The offset is part of the name, not just the ports: two worktrees whose leaf
  # directory names collide (a checkout and a worktree both called
  # "rommsync-nx") would otherwise share a compose project, and the second
  # `up -d` would adopt and recreate the first's containers and database.
  orca_project="rmx-${orca_slug}-${orca_offset}"
}

# Whether the daemon is actually answering.
#
# Every query below reports "nothing found" when it cannot reach docker, which
# reads identically to "nothing is left". Teardown must not confuse the two: a
# stack that survived because the daemon was down is exactly the one that needs
# reporting, and calling it removed is worse than not checking at all.
orca_docker_ready() { docker info >/dev/null 2>&1; }

# Everything docker holds for one compose project, as `kind<TAB>name` lines.
# Only meaningful once orca_docker_ready succeeds.
orca_project_remnants() {
  local filter="label=com.docker.compose.project=$1" tab
  tab="$(printf '\t')"
  docker ps -a      --filter "$filter" --format "container${tab}{{.Names}}" 2>/dev/null || true
  docker volume ls  --filter "$filter" --format "volume${tab}{{.Name}}"     2>/dev/null || true
  docker network ls --filter "$filter" --format "network${tab}{{.Name}}"    2>/dev/null || true
}

# Where the fleet keeps its state, and the file that stops it.
#
# Here rather than in each script because three of them need the same two paths
# and a stop that only some of them can see is not a stop. See
# scripts/orca/fleet.sh for what a stop actually does.
ORCA_FLEET_DIR="${ROMMSYNC_FLEET_DIR:-$HOME/.rommsync-fleet}"
ORCA_FLEET_STOP="$ORCA_FLEET_DIR/STOP"
ORCA_FLEET_OWNED="$ORCA_FLEET_DIR/worktrees"

orca_fleet_stopped() { [ -e "$ORCA_FLEET_STOP" ]; }

# The open PR for a branch, or nothing. Also here rather than in each script:
# two of them resolved it slightly differently, which is how "no PR for this
# branch" and "gh failed" end up looking the same in one of them.
orca_pr_for_branch() {
  local branch="${1:-$(git rev-parse --abbrev-ref HEAD)}" pr
  pr="$(GH_PAGER=cat gh pr list --head "$branch" --state open --json number \
          --jq '.[0].number' 2>/dev/null)" || return 1
  [ -n "$pr" ] && [ "$pr" != "null" ] || return 1
  printf '%s\n' "$pr"
}

# The `owner/name` this worktree's PRs live in, split into $orca_owner and
# $orca_repo_name.
#
# Non-zero when gh cannot answer, and ALSO when it answers something that is not
# `owner/name`. That second case is the one worth a function: an empty owner
# builds a GraphQL query that matches nothing rather than one that errors, so a
# caller polling on it waits out its whole deadline and then reports that nothing
# arrived -- indistinguishable from the silence await-review.sh exists to
# explain.
orca_owner_repo() {
  local answer
  answer="$(GH_PAGER=cat gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)" \
    || return 1
  case "$answer" in */*) ;; *) return 1 ;; esac
  orca_owner="${answer%%/*}"
  orca_repo_name="${answer##*/}"
}

# The pull request, in the shape .github/scripts/merge_gate.py judges, written
# to $2. Needs orca_owner_repo to have run.
#
# Here rather than in each caller for the reason the query itself is one file:
# `review-status.sh` and `resolve-thread.sh` both want exactly this, and "resolve
# the repo, then run the gather" written twice is two places for it to drift --
# which is how the local answer and the gate's answer came apart in #114.
# stderr is NOT swallowed. pr_payload.sh distinguishes "could not read PR #N"
# from "came back in a shape this cannot read", and a caller that turns both
# into its own one-line "could not read the PR's reviews" has thrown away the
# half that says which. That is the same silent-failure shape fleet.sh's card()
# and remove_worktree() are being fixed for in this very change; it reached one
# more place than the issue listed.
orca_pr_payload() {
  local pr="$1" out="$2"
  bash .github/scripts/pr_payload.sh "$orca_owner" "$orca_repo_name" "$pr" >"$out"
}

# Ask merge-gate again about $1 at head $2, and say so with $3 in front of it.
#
# NOTHING ON GITHUB DOES THIS. `merge-gate.yml` re-runs on the events that can
# change its answer, and two of the things that change it are not events it can
# subscribe to: `pull_request_review_thread` is a webhook event and not a
# workflow trigger (putting it in `on:` invalidates the whole file), and an
# issue comment -- which is what an answer to a review is -- would run against
# the default branch, so its check run would not attach to this PR's head at
# all. So the gate stays red on a condition that is already satisfied and
# `--auto` never fires.
#
# Re-running the gate's own earlier run is what asks it again, because a re-run
# updates that check run IN PLACE, which is the thing branch protection counts.
# `merge-gate.yml`'s own `clear-stale` job relies on the same mechanism.
#
# Here rather than in each caller because `resolve-thread.sh` and
# `answer-review.sh` need exactly this, down to the reasons not to do it -- and
# the one that matters is a rule, not a detail: only the NEWEST gate run on the
# head, and only if it failed or was cancelled. Re-running an older failure that
# a newer success already superseded enters merge-gate's cancel-in-progress
# group and can kill a live run, which is #84's wedge inflicted from here.
#
# Exits 0 whether or not a re-run was needed, 2 when it could not tell.
orca_reask_gate() {
  local pr="$1" head="$2" said="$3" listing run job
  listing="$(GH_PAGER=cat gh run list --repo "$orca_owner/$orca_repo_name" \
               --workflow merge-gate.yml --limit 40 \
               --json databaseId,conclusion,headSha 2>/dev/null)" || {
    echo "$said, but could not list merge-gate runs; ask for the gate again with a push" >&2
    return 2; }

  run="$(printf '%s' "$listing" | python3 -c '
import json, sys
head = sys.argv[1]
on_head = [r for r in json.load(sys.stdin) if r.get("headSha") == head]
newest = on_head[0] if on_head else None
print(newest["databaseId"]
      if newest and newest.get("conclusion") in ("failure", "cancelled") else "")
' "$head" 2>/dev/null)"

  if [ -z "$run" ]; then
    echo "$said; the newest merge-gate run on ${head:0:8} is not one to re-ask"
    return 0
  fi

  job="$(GH_PAGER=cat gh api \
           "repos/$orca_owner/$orca_repo_name/actions/runs/$run/jobs" \
           --jq '[.jobs[] | select(.name == "merge-gate") | .id][0]' 2>/dev/null || echo "")"
  if [ -z "$job" ] || [ "$job" = "null" ]; then
    # The gate JOB, never the whole run: `clear-stale` is `needs: gate` and
    # would run a second copy of its own rerun loop, which is the wedge it
    # exists to clear reproduced one level down (merge-gate.yml says the same).
    echo "$said, but the gate job of run $run could not be found." >&2
    echo "Ask the gate again with: gh run rerun --job <gate job id of run $run>" >&2
    return 2
  fi

  if GH_PAGER=cat gh run rerun --job "$job" --repo "$orca_owner/$orca_repo_name" \
       >/dev/null 2>&1; then
    echo "$said; re-ran the gate job ($job) so merge-gate is asked again"
    return 0
  fi
  echo "$said, but re-running the gate job failed." >&2
  echo "Ask it again with: gh run rerun --job $job" >&2
  return 2
}

# The Orca CLI this machine can actually run, in $ORCA_CLI.
#
# `orca` on PATH is a wrapper that locates Orca.app by reading its own symlink.
# A macOS install has shipped that symlink mode 0700 root:wheel, so the readlink
# fails for the user Orca runs these hooks as and every call dies with "Unable to
# determine Orca.app path from symlink". Nothing here notices a broken CLI as
# such -- the JSON never parses -- so it surfaces one layer up as an answer:
# `agent-autostart.sh` reports "this worktree has no linked issue", stops
# watching, and leaves a fully provisioned worktree whose agent sits on an unsent
# prompt forever. That is how three worktrees went idle on 2026-09-05.
#
# So the wrapper is verified rather than assumed, and the app's own binary is the
# fallback. `--version` is the probe because it is the one call that needs no
# runtime: a wrapper that cannot find Orca.app fails it, and a reachable CLI
# answers it whether or not the app is running.
#
# The probe goes through orca_run_with_deadline like every other CLI call. It is
# the FIRST call each hook makes, and `setupAgentStartupPolicy: wait-for-setup`
# holds the agent's tab until setup.sh returns -- so a wrapper that connects and
# then never answers would hang worktree provisioning at the one point where
# nothing has printed a reason yet.
#
# Returns non-zero when nothing answers, so a caller can say so in one line
# instead of making its first real call and reading the silence as data.
ORCA_CLI_PROBE_SECONDS="${ORCA_CLI_PROBE_SECONDS:-10}"
orca_cli_resolve() {
  [ -n "${ORCA_CLI:-}" ] && return 0
  local candidate probe_out
  probe_out="$(mktemp)"
  for candidate in ${ORCA_CLI_COMMAND:-} orca orca-dev orca-ide \
      /Applications/Orca.app/Contents/Resources/bin/orca; do
    command -v "$candidate" >/dev/null 2>&1 || continue
    orca_run_with_deadline "$ORCA_CLI_PROBE_SECONDS" "$probe_out" \
      "$candidate" --version || continue
    ORCA_CLI="$candidate"
    rm -f "$probe_out"
    return 0
  done
  rm -f "$probe_out"
  return 1
}

# Run an `orca` CLI call with a hard deadline, capturing its stdout in $2.
#
# Every hook that talks to the Orca runtime needs this. The runtime can accept a
# connection and then never answer, and `setupAgentStartupPolicy: wait-for-setup`
# in orca.yaml holds the agent's tab until setup.sh returns -- so a hook that
# blocks forever on the CLI costs the whole worktree, which is strictly worse
# than whatever it was trying to arrange. macOS ships no `timeout`, hence the
# manual watchdog.
#
# Polled in 50ms ticks rather than whole seconds, which is not a micro-
# optimisation: a child that has already exited is a zombie until bash reaps it,
# and `kill -0` succeeds on a zombie. With a one-second sleep every call
# therefore cost a full second even when the CLI answered instantly. setup.sh
# makes dozens of these, and `agent-autostart.sh --watch` alone makes two per
# poll -- which is what turned a test of it into a 21-second one, close enough
# to its 60s ctest timeout to go red on a loaded machine.
#
# Returns the command's status, or 124 when the deadline was hit.
# $2 gets stdout only, unless ORCA_RUN_CAPTURE_STDERR=1 is set for the call --
# `VAR=1 orca_run_with_deadline ...`, which bash scopes to that call alone.
#
# Off by default because most callers parse $2 as JSON, and a warning landing in
# it is a parse error. On for the two that report a FAILURE: a CLI says why on
# stderr, so dropping it leaves "it failed" with nothing after it -- and "the
# app is not running" and "that worktree is gone" then look identical.
orca_run_with_deadline() {
  local seconds="$1" out="$2"; shift 2
  if [ "${ORCA_RUN_CAPTURE_STDERR:-0}" = 1 ]; then
    "$@" >"$out" 2>&1 &
  else
    "$@" >"$out" 2>/dev/null &
  fi
  local cli=$! ticks=0 limit=$((seconds * 20))
  while kill -0 "$cli" 2>/dev/null; do
    if [ "$ticks" -ge "$limit" ]; then
      kill "$cli" 2>/dev/null
      wait "$cli" 2>/dev/null
      return 124
    fi
    sleep 0.05
    ticks=$((ticks + 1))
  done
  wait "$cli"
}

# The Python that server/requirements.txt can actually be installed with.
#
# Not simply `python3`. This hook runs in whatever environment Orca hands it,
# and that is not an interactive shell: creating a worktree from the Orca UI on
# macOS resolves `python3` to Apple's Command Line Tools build (3.9.6), while the
# same command typed into a terminal finds Homebrew's. The difference was
# invisible until `requests` was pinned at 2.33.0, which declares
# `requires-python >=3.10` -- pip then filters out every candidate and reports
# "no matching distribution", two hundred lines long, naming neither Python nor
# the reason.
ORCA_PYTHON_MIN_MAJOR=3
ORCA_PYTHON_MIN_MINOR=10

# Usable means three things, not one: it runs, it is new enough, and it can
# actually build a venv. On Debian and Ubuntu `python3.13` and `python3.13-venv`
# are separate packages, so a version check alone can pick an interpreter whose
# `-m venv` then dies with "ensurepip is not available" -- which would be this
# same class of opaque failure, moved one step later.
#
# Sets orca_python_reject to a one-line reason when it returns non-zero, so a
# caller can say which interpreters it turned down and why, rather than
# reporting "nothing is new enough" about one that will not start at all.
orca_python_reject=""
orca_python_is_usable() {
  local out
  orca_python_reject=""
  out="$("$1" -c "
import sys
if sys.version_info < ($ORCA_PYTHON_MIN_MAJOR, $ORCA_PYTHON_MIN_MINOR):
    raise SystemExit('too old: %d.%d' % sys.version_info[:2])
import ensurepip, venv
" 2>&1)" && return 0
  # Last line only: an ImportError arrives as a traceback whose final line is
  # the part worth showing.
  orca_python_reject="$(printf '%s' "$out" | tail -1)"
  [ -n "$orca_python_reject" ] || orca_python_reject="will not run"
  return 1
}

# Every python3.N on PATH, newest first, then bare `python3` as the fallback for
# a machine that has only that.
#
# Discovered rather than hardcoded: a written-out list ends at whatever was
# current the day it was written, and would tell a machine whose only new-enough
# interpreter is python3.15 to go and install a Python it already has.
orca_python_candidates() {
  local dir name
  {
    # Split on ":" only, so a PATH entry containing a space still resolves.
    local IFS=:
    for dir in $PATH; do
      [ -n "$dir" ] || continue
      for name in "$dir"/python3.[0-9] "$dir"/python3.[0-9][0-9]; do
        [ -x "$name" ] && printf '%s\n' "${name##*/}"
      done
    done
  } | sort -t. -k2 -nr -u
  printf 'python3\n'
}

# The first usable candidate, on stdout. Non-zero when there is none, so the
# caller can say so in one line instead of leaving pip to.
orca_pick_python() {
  local candidate
  while IFS= read -r candidate; do
    [ -n "$candidate" ] || continue
    command -v "$candidate" >/dev/null 2>&1 || continue
    if orca_python_is_usable "$candidate"; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done <<EOF
$(orca_python_candidates)
EOF
  return 1
}

# What was tried and why each was turned down, one per line. Only ever called on
# the failure path -- orca_pick_python runs in a command substitution, so the
# reasons it collects cannot escape the subshell, and re-deriving them here
# costs nothing when the alternative is an unexplained stop.
orca_python_rejections() {
  local candidate
  while IFS= read -r candidate; do
    [ -n "$candidate" ] || continue
    command -v "$candidate" >/dev/null 2>&1 || continue
    orca_python_is_usable "$candidate" \
      || printf '     %s (%s): %s\n' "$candidate" "$(command -v "$candidate")" "$orca_python_reject"
  done <<EOF
$(orca_python_candidates)
EOF
}

# Whether an existing .venv can still be used.
#
# `[ -x .venv/bin/python ]` is NOT this test. A venv's bin/python is a symlink to
# the interpreter that built it, and a Homebrew minor upgrade -- or picking a
# different interpreter than last time -- leaves it dangling. `-x` is false for a
# dangling link, so the venv reads as absent, and `python -m venv` over it then
# exits 1 on the broken link rather than repairing it: setup.sh dies there under
# `set -e` and every re-run does the same thing forever. Asking the venv's own
# interpreter to answer covers missing, dangling and too-old in one question.
orca_venv_is_usable() {
  orca_python_is_usable "$1/bin/python" 2>/dev/null
}
