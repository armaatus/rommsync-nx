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

# The server tooling (fixture provisioner, contract probe) is python and needs
# third-party clients; the C++ build needs none of this. A per-worktree venv
# keeps those out of the user's system python and out of other worktrees.
echo "==> installing server tooling into .venv"
python3 -m venv .venv >/dev/null
./.venv/bin/pip install -q --disable-pip-version-check -r server/requirements.txt

# Before the stack, not after it. The steps below are the ones that fail on a bad
# day -- an image pull with no network, a scan that never finishes -- and `set -e`
# means a failure there would skip this and leave exactly the case the tab exists
# for: no RomM, and nothing on screen saying why. romm-logs.sh waits for a stack
# that is not up yet, so there is nothing to gain by running it later.
./scripts/orca/ensure-romm-tab.sh

echo "==> starting RomM ($COMPOSE_PROJECT_NAME on :$ROMM_PORT)"
./scripts/orca/compose.sh up -d

# Files on disk are not a library: RomM reports zero roms until something scans
# them, and the scan is socket.io-driven rather than a REST call. Provisioning
# also creates the fixture admin and mints a client token through the real
# device-code flow, so tests authenticate without a human approving anything.
echo "==> provisioning the fixture (scan, collection, client token)"
./.venv/bin/python server/testing/provision.py --base-url "$ROMM_BASE_URL"

echo
echo "worktree ready."
echo "  RomM        $ROMM_BASE_URL"
echo "  fault proxy $PROXY_BASE_URL"
echo "  fixture     server/testing/fixture-auth.env"
echo "  tests       ctest --test-dir build --output-on-failure"
