#!/usr/bin/env bash
# Remove this project's docker stacks whose worktree no longer exists.
#
# archive.sh is the normal path, but it only runs when the runner removes a
# worktree and docker is healthy at that moment. A worktree deleted with
# `rm -rf`, or removed while Docker Desktop was down, leaves a stack behind --
# and a service that restarts `unless-stopped` comes back on every docker start,
# holding its ports and volumes, with no directory left to find it from. This is
# the sweep that catches those.
#
#   ./scripts/fleet/reap.sh          # list what is stale, change nothing
#   ./scripts/fleet/reap.sh --yes    # tear those stacks down, volumes included
#   ./scripts/fleet/reap.sh --yes --only af-foo-123    # ...that one, if it is stale
#
# This deletes databases, so every uncertainty resolves towards keeping a stack:
# anything it cannot positively establish is stale is left alone, and it refuses
# to run at all rather than sweep with an incomplete idea of what is live.
#
# `--only` narrows the result and can never widen it: the stale set is computed
# exactly as it always was, and the named projects are then intersected with it.
# It exists for fleet.sh, which removes worktrees with no runner hooks and
# sweeps afterwards (armaatus/rommsync-nx#163) -- a dispatcher releasing ONE worktree should not also delete
# the database of an orphan somebody is still looking at. Repeatable, because a
# renamed worktree runs its stack under a name that no longer matches its path.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/fleet/lib.sh

APPLY=false
ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --yes|-y)  APPLY=true ;;
    --dry-run) ;;
    # The `-*` check is not pedantry: `--only --yes` would otherwise scope the
    # sweep to a project that cannot exist AND silently leave APPLY false, and
    # the run would print "nothing to reap", which reads like success.
    --only)    shift
               case "${1:-}" in
                 ""|-*) echo "usage: $0 [--yes] [--only <project>]..." >&2; exit 2 ;;
               esac
               ONLY="$ONLY $1" ;;
    *) echo "usage: $0 [--yes] [--only <project>]..." >&2; exit 2 ;;
  esac
  shift
done

# Every query below reports "nothing found" when the daemon is unreachable, so
# without this the tool documented as the recovery path for "docker was down"
# would print a clean bill of health precisely when docker is down.
fleet_docker_ready \
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
  fleet_derive_env "$wt" \
    || { echo "cannot derive a project name for $wt; refusing to sweep" >&2; exit 1; }
  protect "$fleet_project"
done <<EOF
$worktrees
EOF

# `git worktree list` only knows this repository. A second clone of the repo
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
fleet_derive_env "$REPO_ROOT" \
  || { echo "cannot derive this worktree's own project name" >&2; exit 1; }
case " $protected " in
  *" $fleet_project "*) ;;
  *) echo "sanity check failed: this worktree ($fleet_project) is not in the protected set" >&2
     echo "refusing to remove anything" >&2
     exit 1 ;;
esac

# Every project docker knows about under this repo's prefix, across all three
# resource kinds -- a half-removed stack may survive as volumes alone.
found="$(
  {
    docker ps -a      --format '{{.Label "com.docker.compose.project"}}'
    docker volume ls  --format '{{.Label "com.docker.compose.project"}}'
    docker network ls --format '{{.Label "com.docker.compose.project"}}'
  } 2>/dev/null | grep "^${AUTOFLEET_PROJECT_PREFIX}-" | sort -u || true
)"

stale=""
for project in $found; do
  case " $protected " in *" $project "*) continue ;; esac
  # Applied here, after the protected set has already had its say, so a name
  # passed to --only can only ever be dropped from the sweep and never added to
  # it. A live worktree's project is not removable by asking for it.
  if [ -n "${ONLY// /}" ]; then
    case " $ONLY " in *" $project "*) ;; *) continue ;; esac
  fi
  stale="$stale $project"
done

if [ -z "${stale// /}" ]; then
  if [ -n "${ONLY// /}" ]; then
    echo "nothing to reap;$ONLY has no stack left, or still has a worktree"
  else
    echo "nothing to reap; every ${AUTOFLEET_PROJECT_PREFIX}-* stack belongs to a live worktree"
  fi
  exit 0
fi

failed=false
for project in $stale; do
  echo "== $project (no worktree)"
  fleet_project_remnants "$project" | sed 's/^/     /'
  if ! $APPLY; then continue; fi

  # This repo's compose file describes every one of its stacks -- only the
  # project name differs -- so it can tear down a stack whose own worktree is
  # long gone. AUTOFLEET_COMPOSE_DOWN_ARGS for the same reason as archive.sh: a
  # service whose profile is not active is invisible to `down`, and comes back
  # under `restart: unless-stopped`.
  if [ -n "${AUTOFLEET_COMPOSE_FILE:-}" ]; then
    # shellcheck disable=SC2086 # the down args are a deliberate word list
    docker compose -p "$project" -f "$AUTOFLEET_COMPOSE_FILE" \
      ${AUTOFLEET_COMPOSE_DOWN_ARGS:-} down -v --remove-orphans \
      || echo "!! down failed for $project"
  fi

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
  done < <(fleet_project_remnants "$project")

  remnants="$(fleet_project_remnants "$project")"
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
