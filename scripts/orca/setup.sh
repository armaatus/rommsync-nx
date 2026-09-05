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
. ./scripts/orca/lib.sh

# First, before the ROM seeding and the full build below. This check is free and
# its failure is fatal, so paying for a download and a compile before reporting
# it would only mean waiting minutes to be told about a one-line problem.
if ! PYTHON="$(orca_pick_python)"; then
  echo "!! server/requirements.txt needs Python >= ${ORCA_PYTHON_MIN_MAJOR}.${ORCA_PYTHON_MIN_MINOR}" >&2
  echo "   and nothing usable was found. Tried:" >&2
  orca_python_rejections >&2
  echo "   PATH=$PATH" >&2
  echo "   Install one (brew install python@3.13 / apt install python3.13 python3.13-venv)" >&2
  echo "   and make sure it is on the PATH Orca runs hooks with, then re-run this script." >&2
  exit 1
fi

echo "==> deriving isolated worktree environment"
./scripts/orca/env.sh
set -a; . ./.env; set +a
# The hooks' own scratch space -- pidfiles and the autostart log. Gitignored, and
# created here so nothing later has to check whether it exists.
mkdir -p "$REPO_ROOT/.orca"

# A git worktree does not inherit initialised submodules from the checkout it
# was made from, and CI checks out with `submodules: recursive` -- so a worktree
# whose issue touches overlay/ would otherwise start by finding the dependency
# absent and reaching for a clone. Harmless and instant when there are none.
if [ -f .gitmodules ]; then
  echo "==> initialising submodules"
  git submodule update --init --recursive
fi

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
#
# $PYTHON, resolved at the top of this script, rather than bare `python3`.
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
  # Reported from the venv, not from $PYTHON: `venv` leaves an existing
  # bin/python alone, so announcing the interpreter we picked would describe a
  # venv that is not the one about to be installed into.
  echo "==> installing server tooling into the existing .venv ($(./.venv/bin/python -V 2>&1))"
else
  echo "==> installing server tooling into .venv ($("$PYTHON" -V 2>&1))"
  "$PYTHON" -m venv .venv >/dev/null
fi
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

# Only now, because the tab logs in as the fixture admin and provisioning is what
# creates it. A log stream says the server is alive; the web UI is where you can
# see what it actually scanned, which is the question that comes up while
# implementing against the API.
./scripts/orca/romm-browser.sh

# Detached, and last: `setupAgentStartupPolicy: wait-for-setup` in orca.yaml
# holds the agent's tab until this script returns, so the draft this watches for
# cannot exist yet. `nohup` with HUP ignored because the watcher has to outlive
# the hook that started it -- that is the whole point of it.
if [ "${ROMMSYNC_AGENT_AUTOSTART:-1}" != "0" ]; then
  ( trap "" HUP
    nohup ./scripts/orca/agent-autostart.sh --watch \
      >>"$REPO_ROOT/.orca/agent-autostart.log" 2>&1 & ) &
  echo "==> watching for the agent's issue prompt to submit it"
fi

# For the summary below only: provision.py wrote the account it created here.
set -a; . ./server/testing/fixture-auth.env; set +a

# Only claimed when it happened. romm-browser.sh always exits 0 -- a browser tab
# must not fail a worktree -- so it reports through this file instead, and a
# plain clone or an unreachable runtime gets an honest summary.
if [ -s "$REPO_ROOT/.orca/romm-browser.state" ]; then
  romm_note="  (browser tab, signed in as $ROMM_FIXTURE_USER)"
else
  romm_note="  ($ROMM_FIXTURE_USER / see server/testing/fixture-auth.env)"
fi

echo
echo "worktree ready."
echo "  RomM        $ROMM_BASE_URL$romm_note"
echo "  fault proxy $PROXY_BASE_URL"
echo "  fixture     server/testing/fixture-auth.env"
echo "  tests       ctest --test-dir build --output-on-failure"
