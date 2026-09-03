#!/usr/bin/env bash
# Orca setup hook — runs once when a worktree is created (orca.yaml scripts.setup).
#
# Prepares an isolated, ready-to-work environment so the agent's first `ctest`
# run means something. `setupAgentStartupPolicy: wait-for-setup` in orca.yaml
# makes the agent's tab wait for this to finish, so nothing here needs to be
# fast — it needs to be complete.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

echo "==> deriving isolated worktree environment"
./scripts/orca/env.sh
set -a; . ./.env; set +a

echo "==> seeding ROM fixtures (shared cache: $ROM_CACHE)"
./server/testing/seed.sh

# Build, not just configure. A configured-but-unbuilt tree makes the agent's
# first `ctest` fail with "Not Run" and no explanation, which is exactly the
# confusion `wait-for-setup` exists to avoid. It also warms the shared ccache.
echo "==> building"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build --parallel >/dev/null

echo "==> starting RomM ($COMPOSE_PROJECT_NAME on :$ROMM_PORT)"
./scripts/orca/compose.sh up -d

echo
echo "worktree ready."
echo "  RomM        $ROMM_BASE_URL"
echo "  fault proxy $PROXY_BASE_URL"
echo "  tests       ctest --test-dir build --output-on-failure"
