#!/usr/bin/env bash
# Orca archive hook — runs when a worktree is removed (orca.yaml scripts.archive).
#
# The reverse of setup.sh, for the one thing setup.sh creates that does NOT go
# away with the worktree directory: the docker stack. Everything else setup.sh
# writes -- .env, build/, server/testing/library/ -- lives inside the worktree
# and is deleted along with it. The shared ROM cache and ccache are deliberately
# left alone; they are shared across worktrees and expensive to rebuild.
#
# Failing quietly here is the expensive failure: the stack restarts
# `unless-stopped`, so an orphan comes back on every docker start and holds two
# ports and four volumes, with no worktree left to find it from. So this derives
# the project name rather than trusting .env, and reports what it could not
# remove rather than assuming `down` worked.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

env_project=""
[ -f .env ] && env_project="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' .env | tail -1)"

# Two names, because two things can each be wrong. The derived name is what
# setup.sh would have called this worktree, and is the only one available when
# .env is missing or lost its COMPOSE_PROJECT_NAME line. The name in .env is
# what compose.sh will actually use when .env is intact -- and after a worktree
# directory is renamed the two disagree, with the running stack still under the
# name it was created with. Removing both is the only way neither leaks.
projects=""
if orca_derive_env "$REPO_ROOT"; then
  projects="$orca_project"
else
  echo "!! could not derive the project name (no sha1 tool); falling back to .env"
fi
case " $projects " in
  *" $env_project "*) ;;
  *) [ -n "$env_project" ] && projects="$projects $env_project" ;;
esac

if [ -z "${projects// /}" ]; then
  echo "!! no project name from either the worktree path or .env; nothing can be torn down safely" >&2
  exit 1
fi

# Before docker, because it needs nothing from it and the paths below all end in
# an `exit`. The watcher is bounded and would expire on its own, but it polls the
# Orca runtime from a working directory that is about to be deleted, and a
# process reading a worktree Orca is removing is worth not having.
# `|| true` because this runs under `set -e` and a missing pidfile -- the normal
# case -- would otherwise abort teardown before it removed anything.
watcher="$(cat .orca/agent-autostart.pid 2>/dev/null || true)"
if [ -n "$watcher" ] && kill -0 "$watcher" 2>/dev/null; then
  echo "==> stopping the agent autostart watcher (pid $watcher)"
  kill "$watcher" 2>/dev/null || true
fi

if ! orca_docker_ready; then
  echo "!! docker is not reachable; this worktree's stack is being left behind:"
  echo "     $projects"
  echo "!! sweep it up later with:  ./scripts/orca/reap.sh"
  # Not a failure: Orca is removing this worktree either way, and exiting
  # non-zero over a stopped docker daemon would make every removal look broken.
  # reap.sh is the recovery path, and it needs no worktree to run.
  exit 0
fi

leaked=false
for project in $projects; do
  echo "==> removing RomM stack $project and its volumes"
  # Deliberately not through compose.sh: the wrapper sources .env over the
  # caller's environment, which is right for every other command but would
  # resolve both passes below to whatever .env happens to say. `-p` names the
  # stack outright, which is what teardown needs and what reap.sh does too.
  if ! docker compose -p "$project" -f server/testing/docker-compose.yml \
        down -v --remove-orphans; then
    echo "!! compose down failed for $project"
  fi

  remnants="$(orca_project_remnants "$project")"
  if [ -n "$remnants" ]; then
    echo "!! $project is not fully gone:"
    echo "$remnants" | sed 's/^/     /'
    leaked=true
  fi
done

if $leaked; then
  echo "!! reap it once docker is healthy:  ./scripts/orca/reap.sh"
  exit 0
fi

echo "worktree stack removed."
