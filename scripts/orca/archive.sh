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
# ports and four volumes forever, with no worktree left to find it from. So this
# derives the project name rather than trusting .env, and verifies the stack is
# actually gone rather than assuming `down` succeeded.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

orca_derive_env "$REPO_ROOT"
# Exported, not written to .env: compose.sh sources .env over the top of this,
# which is a no-op when .env is intact (same deterministic value) and the whole
# point when it is missing a COMPOSE_PROJECT_NAME line.
export COMPOSE_PROJECT_NAME="$orca_project"

echo "==> removing RomM stack $COMPOSE_PROJECT_NAME and its volumes"
if ! ./scripts/orca/compose.sh down -v --remove-orphans; then
  echo "!! compose down failed (docker not running?)"
fi

remnants="$(orca_project_remnants "$COMPOSE_PROJECT_NAME")"
if [ -n "$remnants" ]; then
  echo "!! $COMPOSE_PROJECT_NAME is not fully gone:"
  echo "$remnants" | sed 's/^/     /'
  echo "!! reap it once docker is healthy:  ./scripts/orca/reap.sh"
  # Deliberately not a failure. Orca is removing this worktree either way, and
  # exiting non-zero over a stopped docker daemon would make every removal look
  # broken. reap.sh is the recovery path, and it needs no worktree to run.
  exit 0
fi

echo "worktree stack removed."
