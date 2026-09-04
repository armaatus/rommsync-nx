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
#
# This deletes databases, so every uncertainty resolves towards keeping a stack:
# anything it cannot positively establish is stale is left alone, and it refuses
# to run at all rather than sweep with an incomplete idea of what is live.
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

# Every query below reports "nothing found" when the daemon is unreachable, so
# without this the tool documented as the recovery path for "docker was down"
# would print a clean bill of health precisely when docker is down.
orca_docker_ready \
  || { echo "docker is not reachable; cannot tell which stacks are stale" >&2; exit 1; }

protected=""
protect() {
  case " $protected " in *" $1 "*) ;; *) protected="$protected $1" ;; esac
}

# Live worktrees of this repo define what must not be touched, and they are the
# authority rather than the containers' labels: a stack whose containers were
# already removed by hand has no label left, but its volumes still hold a
# database a live worktree is using.
#
# Captured before the loop so a git failure is a failure. Inside a process
# substitution its exit status is discarded, and an empty list then reads as
# "nothing is protected" -- which would make every live stack a target.
worktrees="$(git worktree list --porcelain)" \
  || { echo "cannot list worktrees; refusing to guess what is live" >&2; exit 1; }

while IFS= read -r line; do
  case "$line" in "worktree "*) ;; *) continue ;; esac
  # Not `awk '{print $2}'`: --porcelain does not quote, so a worktree path
  # containing a space would be truncated, derive a different name, and leave
  # the live stack it belongs to unprotected.
  wt="${line#worktree }"
  # git keeps listing a worktree deleted with `rm -rf` until someone prunes it,
  # marked `prunable`. Trusting the list alone would protect exactly the orphans
  # this tool exists to sweep, so require the directory to still be there.
  [ -d "$wt" ] || continue
  orca_derive_env "$wt" \
    || { echo "cannot derive a project name for $wt; refusing to sweep" >&2; exit 1; }
  protect "$orca_project"
done <<EOF
$worktrees
EOF

# `git worktree list` only knows this repository. A second clone of rommsync-nx
# elsewhere on the machine is not a worktree of this one, and its running stack
# would otherwise look stale. Its containers still point at the directory that
# holds its compose file, so protect any project whose directory is still there.
while IFS= read -r line; do
  [ -n "$line" ] || continue
  project="${line%%	*}"
  workdir="${line#*	}"
  [ -n "$project" ] && [ -d "$workdir" ] && protect "$project"
done < <(docker ps -a --format \
  '{{.Label "com.docker.compose.project"}}	{{.Label "com.docker.compose.project.working_dir"}}' \
  2>/dev/null || true)

# This checkout is itself a worktree of this repo, so its own project must have
# come out of the loop above. If it did not, the protected set is not to be
# trusted and neither is anything derived from it.
orca_derive_env "$REPO_ROOT" \
  || { echo "cannot derive this worktree's own project name" >&2; exit 1; }
case " $protected " in
  *" $orca_project "*) ;;
  *) echo "sanity check failed: this worktree ($orca_project) is not in the protected set" >&2
     echo "refusing to remove anything" >&2
     exit 1 ;;
esac

# Every rmx-* project docker knows about, across all three resource kinds -- a
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

failed=false
for project in $stale; do
  echo "== $project (no worktree)"
  orca_project_remnants "$project" | sed 's/^/     /'
  if ! $APPLY; then continue; fi

  # This repo's compose file describes every rmx-* stack -- only the project
  # name differs -- so it can tear down a stack whose own worktree is long gone.
  # --profile tls for the same reason as archive.sh: an inactive profile's
  # containers are invisible to `down`, and the TLS terminator restarts itself.
  docker compose -p "$project" -f server/testing/docker-compose.yml \
    --profile tls down -v --remove-orphans || echo "!! down failed for $project"

  # `down` only removes what it recognises as its own: a stack that was
  # half-dismantled by hand keeps its network, and sometimes a stray container,
  # because compose no longer associates them with the project. Everything left
  # carrying the label of a project already established to have no worktree is
  # safe to remove outright, so finish by label.
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
    failed=true
  else
    echo "   removed."
  fi
done

if ! $APPLY; then
  echo
  echo "re-run with --yes to remove these."
  exit 0
fi

# Non-zero when the sweep did not finish, so a caller scripting this can tell.
$failed && exit 1
exit 0
