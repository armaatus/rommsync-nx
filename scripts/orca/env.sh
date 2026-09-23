#!/usr/bin/env bash
# Derive this worktree's isolated environment and write it to .env (gitignored).
#
# The derivation is the fleet's: scripts/fleet/env.sh names the compose project
# `rmx-<slug>-<offset>` from the worktree's absolute path and hands out one port
# per entry in AUTOFLEET_PORTS (.autofleet/config) -- stable across restarts,
# never colliding between worktrees. This wrapper adds what this project's
# tests and build read on top of the ports: the three BASE_URLs and the two
# shared caches. Every generate-if-missing (compose.sh, seed.sh, tls-fixture.sh)
# calls THIS script, never the fleet's directly, or the BASE_URL lines vanish.
# See docs/TESTING.md and the Q8 decision in docs/DEVELOPMENT.md#worktree-isolation.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# The fleet writes .env atomically and prints the derivation; its output is
# repeated below with the URLs added, so keep it quiet here.
./scripts/fleet/env.sh >/dev/null

# Shared across worktrees on purpose: immutable, expensive to refetch. Mutable
# state (RomM's database volume, build output) stays per-worktree.
ROM_CACHE="$REPO_ROOT/.cache/roms"
CCACHE_DIR="$REPO_ROOT/.cache/ccache"
mkdir -p "$ROM_CACHE" "$CCACHE_DIR"

# Read back what the fleet derived. `set -a` in a subshell-free way is not
# needed: these are only read here.
romm_port="$(sed -n 's/^ROMM_PORT=//p' .env)"
proxy_port="$(sed -n 's/^PROXY_PORT=//p' .env)"
tls_port="$(sed -n 's/^TLS_PORT=//p' .env)"
project="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' .env)"
[ -n "$romm_port" ] && [ -n "$proxy_port" ] && [ -n "$tls_port" ] \
  || { echo "scripts/fleet/env.sh wrote no ports; is AUTOFLEET_PORTS set in .autofleet/config?" >&2; exit 1; }

# A private temp file per run, not a shared `.env.tmp`: compose.sh generates a
# missing .env itself, so there can be two writers at the same moment, and
# sharing one temp path let the first `mv` carry off the second's source file.
# The fleet's own lines are kept verbatim; this script's lines are REPLACED, not
# appended, so a re-run publishes the same file and never a growing one.
ORCA_ENV_TMP="$(mktemp "$REPO_ROOT/.env.tmp.XXXXXX")"
trap 'rm -f "$ORCA_ENV_TMP"' EXIT
{
  grep -v '^\(ROMM_BASE_URL\|PROXY_BASE_URL\|TLS_BASE_URL\|ROM_CACHE\|CCACHE_DIR\)=' .env \
    | grep -v '^# Regenerate with:'
  echo "# Regenerate with: ./scripts/orca/env.sh (the fleet's ports, plus this project's URLs and caches)"
  echo "ROMM_BASE_URL=http://127.0.0.1:$romm_port"
  echo "PROXY_BASE_URL=http://127.0.0.1:$proxy_port"
  echo "TLS_BASE_URL=https://127.0.0.1:$tls_port"
  echo "ROM_CACHE=$ROM_CACHE"
  echo "CCACHE_DIR=$CCACHE_DIR"
} > "$ORCA_ENV_TMP"
# Atomically, because compose.sh sources .env unguarded and may be reading at
# this moment; a read landing inside a truncating rewrite sees no
# COMPOSE_PROJECT_NAME and docker-compose.yml falls back to `rmx-local`.
mv -f "$ORCA_ENV_TMP" "$REPO_ROOT/.env"

echo "worktree env: project=$project romm=$romm_port proxy=$proxy_port tls=$tls_port"
