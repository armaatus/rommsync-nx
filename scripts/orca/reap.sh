#!/usr/bin/env bash
# Remove RomM stacks whose worktree no longer exists.
#
# archive.sh is the normal path, but it only runs when Orca removes a worktree
# and docker is healthy at that moment. A worktree deleted with `rm -rf`, or
# removed while Docker Desktop was down, leaves a stack behind -- and because
# the fixture restarts `unless-stopped` it comes back on every docker start,
# holding two ports and four volumes, with no directory left to find it from.
# This is the sweep that catches those.
#
#   ./scripts/orca/reap.sh          # list what is stale, change nothing
#   ./scripts/orca/reap.sh --yes    # tear those stacks down, volumes included
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

APPLY=false
case "${1:-}" in
  --yes|-y) APPLY=true ;;
  ""|--dry-run) ;;
  *) echo "usage: $0 [--yes]" >&2; exit 2 ;;
esac

# Live worktrees define what must NOT be touched, and they are the authority
# rather than the container's working_dir label: a stack whose containers were
# already removed by hand has no label left, but its volumes still hold a
# database a live worktree is using. Derive each live worktree's project name
# through the same function that named it in the first place.
protected=""
while read -r wt; do
  [ -n "$wt" ] || continue
  orca_derive_env "$wt"
  protected="$protected $orca_project"
done < <(git worktree list --porcelain | awk '/^worktree /{print $2}')

# Every rmx-* project docker knows about, from all three resource kinds -- a
# half-removed stack may survive as volumes alone.
found="$(
  {
    docker ps -a      --format '{{.Label "com.docker.compose.project"}}'
    docker volume ls  --format '{{.Label "com.docker.compose.project"}}'
    docker network ls --format '{{.Label "com.docker.compose.project"}}'
  } 2>/dev/null | grep '^rmx-' | sort -u || true
)"

stale=""
for project in $found; do
  case " $protected " in *" $project "*) continue ;; esac
  stale="$stale $project"
done

if [ -z "${stale// /}" ]; then
  echo "nothing to reap; every rmx-* stack belongs to a live worktree"
  exit 0
fi

for project in $stale; do
  echo "== $project (no worktree)"
  orca_project_remnants "$project" | sed 's/^/     /'
  if ! $APPLY; then continue; fi
  # This repo's compose file describes every rmx-* stack -- only the project
  # name differs -- so it can tear down a stack whose own worktree is long gone.
  COMPOSE_PROJECT_NAME="$project" \
    docker compose -p "$project" -f server/testing/docker-compose.yml \
      down -v --remove-orphans || echo "!! down failed for $project"

  # `down` only removes what it recognises as its own: a stack that was
  # half-dismantled by hand keeps its network, and sometimes a stray container,
  # because compose no longer associates them with the project. Everything left
  # carrying the label of a project we have already established has no worktree
  # is safe to remove outright, so finish by label.
  while IFS="$(printf '\t')" read -r kind name; do
    [ -n "${name:-}" ] || continue
    case "$kind" in
      container) docker rm -f "$name" >/dev/null 2>&1 || true ;;
      volume)    docker volume rm "$name" >/dev/null 2>&1 || true ;;
      network)   docker network rm "$name" >/dev/null 2>&1 || true ;;
    esac
  done < <(orca_project_remnants "$project")

  remnants="$(orca_project_remnants "$project")"
  if [ -n "$remnants" ]; then
    echo "!! $project still has:"
    echo "$remnants" | sed 's/^/     /'
  else
    echo "   removed."
  fi
done

$APPLY || echo; $APPLY || echo "re-run with --yes to remove these."
