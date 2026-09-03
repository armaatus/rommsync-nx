#!/usr/bin/env bash
# Run `docker compose` against THIS worktree's stack.
#
# Docker Compose reads `.env` from the project directory -- which is the
# directory holding the compose file, i.e. server/testing/ -- not from the repo
# root and not from the caller's cwd. So a bare
# `docker compose -f server/testing/docker-compose.yml ...` silently resolves
# COMPOSE_PROJECT_NAME to its default and operates on the WRONG stack.
#
# Always go through this wrapper rather than calling docker compose directly.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

[ -f .env ] || ./scripts/orca/env.sh >/dev/null
set -a; . ./.env; set +a

exec docker compose -f server/testing/docker-compose.yml "$@"
