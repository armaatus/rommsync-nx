#!/usr/bin/env bash
# Tear down this worktree's RomM stack -- orca.yaml runs it when Orca removes a
# worktree, and it is the same command by hand before a `git worktree remove`.
#
# The reverse of setup.sh, for the one thing setup.sh creates that does NOT go
# away with the worktree directory: the docker stack. Everything else -- .env,
# build output, the venv -- lives inside the worktree and is deleted along with
# it. The shared caches under .cache/ are deliberately left alone.
#
# Failing quietly here is the expensive failure: a service that restarts
# `unless-stopped` comes back on every docker start and holds its ports and
# volumes, with no worktree left to find it from. So this derives the project
# name rather than trusting .env, and reports what it could not remove rather
# than assuming `down` worked.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

env_project=""
[ -f .env ] && env_project="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' .env | tail -1)"

# Two names, because two things can each be wrong. The derived name is what
# env.sh would have called this worktree, and is the only one available when
# .env is missing or lost its COMPOSE_PROJECT_NAME line. The name in .env is
# what compose will actually use when .env is intact -- and after a worktree
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

if ! orca_docker_ready; then
  echo "!! docker is not reachable; this worktree's stack is being left behind:"
  echo "     $projects"
  echo "!! sweep it up later with:  ./scripts/orca/reap.sh --yes"
  # Not a failure: Orca is removing this worktree either way, and exiting
  # non-zero over a stopped docker daemon would make every removal look broken.
  # reap.sh is the recovery path, and it needs no worktree to run.
  exit 0
fi

leaked=false
for project in $projects; do
  echo "==> removing stack $project and its volumes"
  # `-p` names the stack outright rather than letting .env decide: both passes
  # must address the project they were given even when .env disagrees.
  #
  # `--profile tls` because `down` only touches services whose profile is
  # ACTIVE: the TLS terminator tls-fixture.sh starts would otherwise survive,
  # come back on every docker start under `restart: unless-stopped`, hold this
  # worktree's TLS_PORT, and block the network removal behind it (#122).
  # `orca.teardown_profiles` fails the day a profile is added and not named here.
  if ! docker compose -p "$project" -f server/testing/docker-compose.yml \
        --profile tls down -v --remove-orphans; then
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
  echo "!! reap it once docker is healthy:  ./scripts/orca/reap.sh --yes"
  exit 0
fi

echo "worktree stack removed."
