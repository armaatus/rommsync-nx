#!/usr/bin/env bash
# Shared helpers for the Orca hooks. Sourced, never executed.

# A worktree's identity -- its compose project name and its two ports -- is a
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

# Run an `orca` CLI call with a hard deadline, capturing its stdout in $2.
#
# Every hook that talks to the Orca runtime needs this. The runtime can accept a
# connection and then never answer, and `setupAgentStartupPolicy: wait-for-setup`
# in orca.yaml holds the agent's tab until setup.sh returns -- so a hook that
# blocks forever on the CLI costs the whole worktree, which is strictly worse
# than whatever it was trying to arrange. macOS ships no `timeout`, hence the
# manual watchdog.
#
# Returns the command's status, or 124 when the deadline was hit.
orca_run_with_deadline() {
  local seconds="$1" out="$2"; shift 2
  "$@" >"$out" 2>/dev/null &
  local cli=$! waited=0
  while kill -0 "$cli" 2>/dev/null; do
    if [ "$waited" -ge "$seconds" ]; then
      kill "$cli" 2>/dev/null
      wait "$cli" 2>/dev/null
      return 124
    fi
    sleep 1
    waited=$((waited + 1))
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
#
# Prints the interpreter on stdout; returns non-zero when nothing on PATH is new
# enough, so the caller can say so in one line instead of leaving pip to.
ORCA_PYTHON_MIN_MAJOR=3
ORCA_PYTHON_MIN_MINOR=10

orca_python_is_new_enough() {
  "$1" -c "import sys; raise SystemExit(0 if sys.version_info >= ($ORCA_PYTHON_MIN_MAJOR, $ORCA_PYTHON_MIN_MINOR) else 1)" \
    2>/dev/null
}

# Newest first, then bare `python3` last: a machine with only a current python3
# and no versioned name still works, while one whose `python3` is Apple's 3.9
# finds a real one rather than stopping at the first thing on PATH.
orca_pick_python() {
  local candidate
  for candidate in python3.14 python3.13 python3.12 python3.11 python3.10 python3; do
    command -v "$candidate" >/dev/null 2>&1 || continue
    if orca_python_is_new_enough "$candidate"; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}
