#!/usr/bin/env bash
# The fixture's own configuration -- the handful of settings that make a REAL
# RomM deterministic enough to measure a client against.
#
# `server/testing/docker-compose.yml` already turns off the two sources of
# run-to-run variation that were obvious from the outside (metadata providers,
# parallel scan workers). This file holds the one that was not: gunicorn recycles
# each worker after `--max-requests` requests, and a suite that drives thousands
# of calls through one stack meets that recycle as a 502 out of nowhere (#155).
#
#   test_rig_fixture.sh recycling       the compose file disables it. Needs no
#                                       docker, so the regression stays checkable
#                                       with the daemon stopped.
#   test_rig_fixture.sh recycling_live  ...and THIS worktree's RomM is really
#                                       running with it, which a stack started
#                                       before the setting was added is not.
#                                       Skips with 77 when docker is down, like
#                                       rig.smoke.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIP=77
COMPOSE_FILE="$REPO_ROOT/server/testing/docker-compose.yml"

fail() { echo "FAIL: $*" >&2; exit 1; }

# What the fixture must run with. Named once: both phases below ask the same
# question of the file and of the daemon, and a value changed in one place only
# would leave one of them agreeing with nothing.
readonly MAX_REQUESTS=0

phase_recycling() {
  grep -qE "^[[:space:]]*WEB_SERVER_MAX_REQUESTS:[[:space:]]*\"$MAX_REQUESTS\"[[:space:]]*$" \
      "$COMPOSE_FILE" \
    || fail "server/testing/docker-compose.yml does not set WEB_SERVER_MAX_REQUESTS: \"$MAX_REQUESTS\"
  gunicorn then recycles a worker every ~1000 requests, and a request in flight
  when it does comes back 502 from RomM's own nginx -- see #155."
  echo "rig.recycling ok"
}

phase_recycling_live() {
  . "$REPO_ROOT/scripts/orca/lib.sh"

  orca_docker_ready || { echo "docker is not answering -- skipping"; exit "$SKIP"; }
  orca_derive_env "$REPO_ROOT" || fail "could not derive this worktree's compose project"

  local container
  container="$(docker ps \
    --filter "label=com.docker.compose.project=$orca_project" \
    --filter "label=com.docker.compose.service=romm" \
    --format '{{.Names}}' 2>/dev/null | head -1)"
  [ -n "$container" ] || { echo "this worktree's RomM is not running -- skipping"; exit "$SKIP"; }

  # The effective argument, not the environment variable that produced it: /init
  # turns WEB_SERVER_MAX_REQUESTS into `--max-requests`, and it is the flag
  # gunicorn reads. Read from /proc rather than `ps`, whose output width is a
  # busybox build option.
  local args
  args="$(docker exec "$container" sh -c '
    for proc in /proc/[0-9]*/cmdline; do
      pid="${proc#/proc/}"; pid="${pid%/cmdline}"
      # This shell is a process in that container too, and the pattern it is
      # looking for is in its own argv -- so it matches itself first.
      [ "$pid" = "$$" ] && continue
      line="$(tr "\0" " " < "$proc" 2>/dev/null)" || continue
      case "$line" in *gunicorn*--max-requests*) printf "%s\n" "$line"; break ;; esac
    done' 2>/dev/null)"
  [ -n "$args" ] || fail "no gunicorn process in $container"

  local setting
  setting="$(printf '%s\n' "$args" | grep -oE -- '--max-requests [0-9]+' | head -1)"
  [ "$setting" = "--max-requests $MAX_REQUESTS" ] || fail \
"$container runs gunicorn with '$setting', not '--max-requests $MAX_REQUESTS'
  This stack predates the setting, so it still recycles a worker every ~1000
  requests and answers 502 to whatever was in flight (#155). Pick it up with:
    ./scripts/orca/compose.sh up -d romm"
  echo "rig.recycling_live ok"
}

case "${1:-}" in
  recycling) phase_recycling ;;
  recycling_live) phase_recycling_live ;;
  *) fail "usage: $0 recycling|recycling_live" ;;
esac
