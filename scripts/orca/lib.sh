#!/usr/bin/env bash
# Shared helpers for the Orca hooks. Sourced, never executed.

# A worktree's identity -- its compose project name and its two ports -- is a
# pure function of its absolute path. env.sh writes the result to .env when the
# worktree is created; teardown recomputes it instead of reading .env back.
#
# That asymmetry is deliberate. .env is generated, gitignored, and written by a
# hook that can itself fail half way, so teardown must not depend on it: an
# absent or truncated COMPOSE_PROJECT_NAME sends `docker compose down` to the
# compose default (`rmx-local`) and leaves this worktree's three containers, four
# volumes and network running under `restart: unless-stopped`, holding two ports,
# with nothing left on disk to identify them by.
orca_derive_env() {
  local root="$1"

  orca_slug="$(basename "$root" | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9' '-' | sed 's/^-//;s/-$//')"
  orca_offset="$(python3 -c "
import hashlib,sys
h=hashlib.sha1(sys.argv[1].encode()).digest()
print(int.from_bytes(h[:2],'big') % 2000)
" "$root")"

  orca_romm_port=$((21000 + orca_offset))
  orca_proxy_port=$((23000 + orca_offset))
  # The offset is part of the name, not just the ports: two worktrees whose leaf
  # directory names collide (a checkout and a worktree both called
  # "rommsync-nx") would otherwise share a compose project, and the second
  # `up -d` would adopt and recreate the first's containers and database.
  orca_project="rmx-${orca_slug}-${orca_offset}"
}

# Everything docker holds for one compose project, as `kind<TAB>name` lines.
# Empty output means the project is fully gone -- the postcondition teardown
# checks rather than assumes.
orca_project_remnants() {
  local project="$1" filter="label=com.docker.compose.project=$1"
  docker ps -a     --filter "$filter" --format 'container'"$(printf '\t')"'{{.Names}}' 2>/dev/null || true
  docker volume ls --filter "$filter" --format 'volume'"$(printf '\t')"'{{.Name}}'    2>/dev/null || true
  docker network ls --filter "$filter" --format 'network'"$(printf '\t')"'{{.Name}}'  2>/dev/null || true
}
