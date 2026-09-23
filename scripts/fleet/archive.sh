#!/usr/bin/env bash
# Worktree archive hook -- runs when a worktree is removed.
#
# The reverse of setup.sh, for the one thing setup.sh creates that does NOT go
# away with the worktree directory: the docker stack.
# Everything else setup.sh writes -- .env, build output, fixtures -- lives
# inside the worktree and is deleted along with it. Caches a project shares
# between worktrees are deliberately left alone.
#
# Failing quietly here is the expensive failure: a service that restarts
# `unless-stopped` comes back on every docker start and holds its ports and
# volumes, with no worktree left to find it from. So this derives the project
# name rather than trusting .env, and reports what it could not remove rather
# than assuming `down` worked.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/fleet/lib.sh

# The project's own teardown, before the stack: it may need the containers it
# is about to lose. Never fatal -- the runner is removing this worktree either
# way, and a hook that exits non-zero must not stop the stack removal below,
# which is the part that leaks if it is skipped.
if [ -n "${AUTOFLEET_TEARDOWN_HOOK:-}" ] && [ -x "$AUTOFLEET_TEARDOWN_HOOK" ]; then
  echo "==> $AUTOFLEET_TEARDOWN_HOOK"
  "./$AUTOFLEET_TEARDOWN_HOOK" || echo "!! $AUTOFLEET_TEARDOWN_HOOK failed; continuing with the stack removal"
fi

# A project with no compose file has no stack to remove, and everything below
# would report docker problems that cannot matter to it.
if [ -z "${AUTOFLEET_COMPOSE_FILE:-}" ]; then
  echo "worktree archived (no compose file configured; nothing to tear down)."
  exit 0
fi

env_project=""
[ -f .env ] && env_project="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' .env | tail -1)"

# Two names, because two things can each be wrong. The derived name is what
# setup.sh would have called this worktree, and is the only one available when
# .env is missing or lost its COMPOSE_PROJECT_NAME line. The name in .env is
# what compose will actually use when .env is intact -- and after a worktree
# directory is renamed the two disagree, with the running stack still under the
# name it was created with. Removing both is the only way neither leaks.
projects=""
if fleet_derive_env "$REPO_ROOT"; then
  projects="$fleet_project"
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

if ! fleet_docker_ready; then
  echo "!! docker is not reachable; this worktree's stack is being left behind:"
  echo "     $projects"
  echo "!! sweep it up later with:  ./scripts/fleet/reap.sh"
  # Not a failure: Orca is removing this worktree either way, and exiting
  # non-zero over a stopped docker daemon would make every removal look broken.
  # reap.sh is the recovery path, and it needs no worktree to run.
  exit 0
fi

leaked=false
for project in $projects; do
  echo "==> removing stack $project and its volumes"
  # `-p` names the stack outright rather than letting .env decide, which is what
  # teardown needs and what reap.sh does too: both passes below must address the
  # same project even when .env disagrees with the path.
  #
  # AUTOFLEET_COMPOSE_DOWN_ARGS is where a project puts its `--profile` flags,
  # and getting them wrong is expensive: `down` only touches services whose
  # profile is ACTIVE, so an inactive one survives teardown, `restart:
  # unless-stopped` brings it back on every Docker start, and it holds this
  # worktree's port with no directory left to identify it by -- the exact orphan
  # this hook exists to prevent. It also blocks the network removal, so the rest
  # of the teardown fails behind it.
  # shellcheck disable=SC2086 # the down args are a deliberate word list
  if ! docker compose -p "$project" -f "$AUTOFLEET_COMPOSE_FILE" \
        ${AUTOFLEET_COMPOSE_DOWN_ARGS:-} down -v --remove-orphans; then
    echo "!! compose down failed for $project"
  fi

  remnants="$(fleet_project_remnants "$project")"
  if [ -n "$remnants" ]; then
    echo "!! $project is not fully gone:"
    echo "$remnants" | sed 's/^/     /'
    leaked=true
  fi
done

if $leaked; then
  echo "!! reap it once docker is healthy:  ./scripts/fleet/reap.sh"
  exit 0
fi

echo "worktree stack removed."
