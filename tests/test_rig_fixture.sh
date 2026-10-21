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
#   test_rig_fixture.sh slot_cap        the compose file pins MAX_SAVES_PER_SLOT,
#                                       so pruning is the fixture's choice and
#                                       not whatever a RomM release defaults it
#                                       to (#227).
#   test_rig_fixture.sh version_live    THIS worktree's RomM runs the image .env
#                                       names. ROMM_VERSION selects the RomM the
#                                       suite tests (#227), and a stack started
#                                       before it changed answers for the old
#                                       one -- every result green or red for the
#                                       wrong version. Skips like recycling_live.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIP=77
COMPOSE_FILE="$REPO_ROOT/server/testing/docker-compose.yml"

fail() { echo "FAIL: $*" >&2; exit 1; }

# What the fixture must run with. Named once: both phases below ask the same
# question of the file and of the daemon, and a value changed in one place only
# would leave one of them agreeing with nothing.
readonly MAX_REQUESTS=0
# ...and the slot cap. server/probe_contract.py proves pruning against this
# number, so a compose file that moved it alone would turn contract.captures red
# with nothing here saying why.
readonly SLOT_CAP=10

# The romm service's block of the compose file, by indentation: see
# phase_recycling for why that and not `docker compose config`.
romm_block() {
  awk '
    /^  [a-z0-9_-]+:$/ { inside = ($0 == "  romm:") ; next }
    inside { print }
  ' "$COMPOSE_FILE"
}

# Sets `container` to the romm container of the stack the tests are actually
# talking to, or to nothing. Called directly, never in $(...): the skip below has
# to leave the script, not a subshell.
find_romm_container() {
  . "$REPO_ROOT/scripts/orca/lib.sh"
  orca_docker_ready || { echo "docker is not answering -- skipping"; exit "$SKIP"; }

  # .env is what compose.sh reads, so it is the authority here -- unlike
  # teardown, which recomputes precisely because it must work when .env is gone
  # (scripts/orca/lib.sh). The derivation is the fallback for a checkout that
  # has not generated one yet.
  local project=""
  if [ -f "$REPO_ROOT/.env" ]; then
    project="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' "$REPO_ROOT/.env" | head -1)"
  fi
  if [ -z "$project" ]; then
    orca_derive_env "$REPO_ROOT" || fail "no COMPOSE_PROJECT_NAME in .env, and none derivable"
    project="$orca_project"
  fi

  container="$(docker ps \
    --filter "label=com.docker.compose.project=$project" \
    --filter "label=com.docker.compose.service=romm" \
    --format '{{.Names}}' 2>/dev/null | head -1)"
}

phase_recycling() {
  # Inside the `romm` service, not just somewhere in the file: the same line
  # under `romm-db` or `fault-proxy` would satisfy a whole-file grep while RomM
  # went on recycling, and this is the half that has to hold when docker is
  # stopped and recycling_live can only skip.
  #
  # By indentation, because the alternative is `docker compose config` and the
  # point of this phase is that it needs no daemon. A service header is the only
  # thing at exactly two spaces, so everything printed between `  romm:` and the
  # next one is that service's block. Reindent the file and this stops finding
  # anything, which is a failure, not a false pass.
  local block
  block="$(romm_block)"

  printf '%s\n' "$block" \
    | grep -qE "^[[:space:]]*WEB_SERVER_MAX_REQUESTS:[[:space:]]*\"$MAX_REQUESTS\"[[:space:]]*$" \
    || fail "the romm service in server/testing/docker-compose.yml does not set
  WEB_SERVER_MAX_REQUESTS: \"$MAX_REQUESTS\"
  gunicorn then recycles a worker every ~1000 requests, and a request in flight
  when it does comes back 502 from RomM's own nginx -- see #155."
  echo "rig.recycling ok"
}

phase_recycling_live() {
  local container
  find_romm_container
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
  # `?` is a glob, matching whichever of `=` or a space the spelling used, so
  # what is left is the value on its own.
  [ "${setting#--max-requests?}" = "$MAX_REQUESTS" ] || fail \
"$container runs gunicorn with '$setting', not '--max-requests $MAX_REQUESTS'
  This stack predates the setting, so it still recycles a worker every ~1000
  requests and answers 502 to whatever was in flight (#155). Pick it up with:
    ./scripts/orca/compose.sh up -d romm"
  echo "rig.recycling_live ok"
}

phase_slot_cap() {
  # RomM 5.3 prunes every slot to MAX_SAVES_PER_SLOT versions on upload. Left to
  # its default, a test that counts versions is testing RomM's release notes.
  romm_block \
    | grep -qE "^[[:space:]]*MAX_SAVES_PER_SLOT:[[:space:]]*\"$SLOT_CAP\"[[:space:]]*$" \
    || fail "the romm service in server/testing/docker-compose.yml does not set
  MAX_SAVES_PER_SLOT: \"$SLOT_CAP\"
  RomM 5.3+ prunes each slot to it, and a default that moves between releases
  makes every pruning test depend on which RomM it ran against (#227). Moving it
  means moving the 13-upload pruning scenario in server/probe_contract.py too."
  echo "rig.slot_cap ok"
}

phase_version_live() {
  local want container image
  # The container first: with no stack running there is nothing to be wrong
  # about, and this skips like every other rig test rather than failing a
  # checkout that has not generated .env yet.
  find_romm_container
  [ -n "$container" ] || { echo "this worktree's RomM is not running -- skipping"; exit "$SKIP"; }

  # The environment first, as contract.captures reads it: `ROMM_VERSION=5.4.0
  # ctest` is a claim about the server, and this is where it is checked.
  want="${ROMM_VERSION:-$(sed -n 's/^ROMM_VERSION=//p' "$REPO_ROOT/.env" 2>/dev/null | head -1)}"
  [ -n "$want" ] || fail "no ROMM_VERSION in .env -- regenerate it with ./scripts/orca/env.sh"

  # And not below the default, which is also the floor (#235): the suites
  # assert 5.4.0's behaviour, and a .env written before the flip names 5.2.0
  # only because that was the default then -- env.sh keeps it, by design, so
  # nothing else would ever say so. Asked before the image, so the answer is
  # the move rather than "recreate what you have".
  local floor
  floor="$(sed -n 's/^ROMM_VERSION_DEFAULT=//p' "$REPO_ROOT/scripts/orca/env.sh" | head -1)"
  [ -n "$floor" ] || fail "scripts/orca/env.sh names no ROMM_VERSION_DEFAULT"
  [ "$(printf '%s\n%s\n' "$want" "$floor" | sort -V | head -1)" = "$floor" ] || fail \
"ROMM_VERSION is $want, below $floor, the version the suites now assert.
  Move this worktree -- emptied first, since RomM never migrates a database
  back and a 5.2.0 volume is not one to keep -- with:
    ROMM_VERSION=$floor ./scripts/orca/env.sh
    ./scripts/orca/compose.sh down -v && ./scripts/orca/compose.sh up -d
    ./.venv/bin/python server/testing/provision.py
  (docs/TESTING.md#choosing-the-romm-version)"

  image="$(docker inspect --format '{{.Config.Image}}' "$container" 2>/dev/null)"
  [ "$image" = "rommapp/romm:$want" ] || fail \
"$container runs $image, but ROMM_VERSION asks for rommapp/romm:$want.
  Every rig test is answering for the wrong RomM. Recreate it -- emptied
  first, since RomM never migrates a database back -- with:
    ./scripts/orca/compose.sh down -v && ./scripts/orca/compose.sh up -d
    ./.venv/bin/python server/testing/provision.py
  (docs/TESTING.md#choosing-the-romm-version)"
  echo "rig.version_live ok ($image)"
}

case "${1:-}" in
  recycling) phase_recycling ;;
  recycling_live) phase_recycling_live ;;
  slot_cap) phase_slot_cap ;;
  version_live) phase_version_live ;;
  *) fail "usage: $0 recycling|recycling_live|slot_cap|version_live" ;;
esac
