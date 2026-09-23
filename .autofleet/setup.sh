#!/usr/bin/env bash
# What a worktree of rommsync-nx needs before an agent can work in it.
#
# The project half of the setup hook: scripts/fleet/setup.sh has already derived
# the worktree's identity and exported .env by the time this runs, and its exit
# status is provisioning's. This is the body of what scripts/orca/setup.sh did
# under Orca, minus the Orca-only parts (the browser tab, the log tab, the
# autostart watcher): ROM fixtures, the build, the venv the server tooling
# needs, this worktree's own RomM, and the fixture provisioned inside it. By the
# time this returns, `ctest` is a meaningful command.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

# FIRST, before anything expensive: the interpreter. This hook runs in whatever
# environment the dispatcher hands it, and that is not an interactive shell --
# on macOS `python3` can resolve to Apple's 3.9.6 while a terminal finds
# Homebrew's. server/requirements.txt needs >= 3.10, and pip's report of that is
# two hundred lines naming neither Python nor the reason. Say it here, in one.
if ! PYTHON="$(orca_pick_python)"; then
  echo "!! server/requirements.txt needs Python >= ${ORCA_PYTHON_MIN_MAJOR}.${ORCA_PYTHON_MIN_MINOR}" >&2
  echo "   and nothing usable was found. Tried:" >&2
  orca_python_rejections >&2
  echo "   PATH=$PATH" >&2
  echo "   Install one (brew install python@3.13 / apt install python3.13 python3.13-venv)" >&2
  echo "   and make sure it is on the PATH the fleet runs hooks with, then re-run this script." >&2
  exit 1
fi

# The fleet's env.sh wrote the ports; this project's wrapper adds the BASE_URLs
# and the shared caches the tests and the build read. Re-derived here rather
# than trusted, so a hand-run of this script from a plain clone works too.
echo "==> deriving this worktree's environment"
./scripts/orca/env.sh
set -a; . ./.env; set +a

# A git worktree does not inherit initialised submodules from the checkout it
# was made from, and CI checks out with `submodules: recursive` -- so a worktree
# whose issue touches overlay/ would otherwise start by finding the dependency
# absent and reaching for a clone. Harmless and instant when there are none.
# scripts/fleet/setup.sh does this too; twice is idempotent and a hand-run of
# this script alone still gets it.
if [ -f .gitmodules ]; then
  echo "==> initialising submodules"
  git submodule update --init --recursive
fi

echo "==> seeding ROM fixtures (shared cache: $ROM_CACHE)"
./server/testing/seed.sh

# Build, not just configure. A configured-but-unbuilt tree makes the agent's
# first `ctest` fail with "Not Run" and no explanation. It also warms the shared
# ccache.
echo "==> building"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build --parallel >/dev/null

# The server tooling (fixture provisioner, contract probe) is python and needs
# third-party clients; the C++ build needs none of this. A per-worktree venv
# keeps those out of the user's system python and out of other worktrees.
#
# An existing .venv is reused only if its OWN interpreter still answers and
# still satisfies the floor -- see orca_venv_is_usable in lib.sh for why testing
# the file with `-x` is the wrong question and leaves a worktree permanently
# unprovisionable. Replacing it is what makes a worktree that failed here
# recoverable by re-running this script rather than by hand.
if [ -d .venv ] && ! orca_venv_is_usable .venv; then
  echo "==> replacing .venv (its interpreter is missing, broken or too old)"
  rm -rf .venv
fi
if [ -d .venv ]; then
  echo "==> installing server tooling into the existing .venv ($(./.venv/bin/python -V 2>&1))"
else
  echo "==> installing server tooling into .venv ($("$PYTHON" -V 2>&1))"
  "$PYTHON" -m venv .venv >/dev/null
fi
./.venv/bin/pip install -q --disable-pip-version-check -r server/requirements.txt

echo "==> starting RomM ($COMPOSE_PROJECT_NAME on :$ROMM_PORT)"
./scripts/orca/compose.sh up -d

# Files on disk are not a library: RomM reports zero roms until something scans
# them, and the scan is socket.io-driven rather than a REST call. Provisioning
# also creates the fixture admin and mints a client token through the real
# device-code flow, so tests authenticate without a human approving anything.
echo "==> provisioning the fixture (scan, collection, client token)"
./.venv/bin/python server/testing/provision.py --base-url "$ROMM_BASE_URL"

echo "==> ready: RomM at $ROMM_BASE_URL, proxy at $PROXY_BASE_URL; run ctest --test-dir build"
