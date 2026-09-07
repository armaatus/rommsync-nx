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
  # Inside the `romm` service, not just somewhere in the file: the same line
  # under `romm-db` or `fault-proxy` would satisfy a whole-file grep while RomM
  # went on recycling, and this is the half that has to hold when docker is
  # stopped and recycling_live can only skip.
  local block
  block="$(awk '
    /^  [a-z0-9_-]+:$/ { inside = ($0 == "  romm:") ; next }
    inside { print }
  ' "$COMPOSE_FILE")"

  printf '%s\n' "$block" \
    | grep -qE "^[[:space:]]*WEB_SERVER_MAX_REQUESTS:[[:space:]]*\"$MAX_REQUESTS\"[[:space:]]*$" \
    || fail "the romm service in server/testing/docker-compose.yml does not set
  WEB_SERVER_MAX_REQUESTS: \"$MAX_REQUESTS\"
  gunicorn then recycles a worker every ~1000 requests, and a request in flight
  when it does comes back 502 from RomM's own nginx -- see #155."
  echo "rig.recycling ok"
}

phase_recycling_live() {
  . "$REPO_ROOT/scripts/orca/lib.sh"

  orca_docker_ready || { echo "docker is not answering -- skipping"; exit "$SKIP"; }

  # The stack the tests are actually talking to. .env is what compose.sh reads,
  # so it is the authority here -- unlike teardown, which recomputes precisely
  # because it must work when .env is gone (scripts/orca/lib.sh). The derivation
  # is the fallback for a checkout that has not generated one yet.
  local project=""
  if [ -f "$REPO_ROOT/.env" ]; then
    project="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' "$REPO_ROOT/.env" | head -1)"
  fi
  if [ -z "$project" ]; then
    orca_derive_env "$REPO_ROOT" || fail "no COMPOSE_PROJECT_NAME in .env, and none derivable"
    project="$orca_project"
  fi

  local container
  container="$(docker ps \
    --filter "label=com.docker.compose.project=$project" \
    --filter "label=com.docker.compose.service=romm" \
    --format '{{.Names}}' 2>/dev/null | head -1)"
  [ -n "$container" ] || { echo "this worktree's RomM is not running -- skipping"; exit "$SKIP"; }

  # The effective argument, not the environment variable that produced it: /init
  # turns WEB_SERVER_MAX_REQUESTS into `--max-requests`, and it is the flag
  # gunicorn reads. Read from /proc rather than `ps`, whose output width is a
  # busybox build option.
  # Selected on `gunicorn` alone, deliberately. Matching the flag too would make
  # a gunicorn that carries no `--max-requests` -- a RomM that renamed the env
  # var, or dropped the flag -- indistinguishable from a container that has not
  # started one, and this would then skip forever with the wrong reason.
  local args
  args="$(docker exec "$container" sh -c '
    for proc in /proc/[0-9]*/cmdline; do
      pid="${proc#/proc/}"; pid="${pid%/cmdline}"
      # This shell is a process in that container too, and the word it is
      # looking for is in its own argv -- so it matches itself first.
      [ "$pid" = "$$" ] && continue
      line="$(tr "\0" " " < "$proc" 2>/dev/null)" || continue
      case "$line" in
        *bin/gunicorn\ *) printf "%s\n" "$line"; break ;;
      esac
    done' 2>/dev/null)"
  # A container that is up but has not reached gunicorn yet is a stack still
  # starting, not a stack misconfigured -- the same condition every rig test
  # skips on, and one -DROMMSYNC_REQUIRE_RIG=ON turns back into a failure.
  [ -n "$args" ] || { echo "$container has not started gunicorn yet -- skipping"; exit "$SKIP"; }

  # Both spellings: `--max-requests 0` is what /init emits today, and a
  # `--max-requests=0` that read as "flag absent" would fail a correctly
  # configured stack with a message naming no value at all.
  local setting
  setting="$(printf '%s\n' "$args" | grep -oE -- '--max-requests[= ][0-9]+' | head -1)"
  [ -n "$setting" ] || fail \
"$container runs gunicorn with no --max-requests at all:
  $args
  Nothing is holding the fixture to the setting #155 needs, so read /init and
  find what replaced it rather than deleting this test."
  [ "${setting#--max-requests?}" = "$MAX_REQUESTS" ] || fail \
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
