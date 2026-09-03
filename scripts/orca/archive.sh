#!/usr/bin/env bash
# Orca archive hook — runs when a worktree is removed (orca.yaml scripts.archive).
#
# Tears down this worktree's RomM stack and its volumes. The shared ROM cache
# and ccache are deliberately left alone; they are shared across worktrees.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

if [ -f .env ]; then
  set -a; . ./.env; set +a
  echo "==> removing RomM stack $COMPOSE_PROJECT_NAME and its volumes"
  docker compose -f server/testing/docker-compose.yml down -v --remove-orphans || true
else
  echo "no .env; nothing to tear down"
fi
