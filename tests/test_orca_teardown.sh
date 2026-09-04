#!/usr/bin/env bash
# Covers scripts/orca/archive.sh and reap.sh -- the removal half of a worktree's
# life, the reverse of scripts/orca/setup.sh.
#
# A stack that survives its worktree is not a cosmetic leak: the fixture restarts
# `unless-stopped`, so it comes back on every docker start and holds two ports
# and four volumes, with no directory left on disk to identify it from.
#
#   test_orca_teardown.sh derives   .env is unreadable -> archive.sh must still
#                                   target the project setup.sh created, not the
#                                   compose default. This is the regression:
#                                   reading .env back yields `rmx-local` and
#                                   removes nothing. Needs no docker.
#   test_orca_teardown.sh reap      reap.sh flags a stack with no worktree and
#                                   never flags one still in use. Skips with 77
#                                   when docker is down, like rig.smoke.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIP=77

fail() { echo "FAIL: $*" >&2; exit 1; }

FIXTURE=""
ORPHAN=""
cleanup() {
  [ -n "$FIXTURE" ] && rm -rf "$FIXTURE"
  # Never leave the fabricated stack behind -- that would be this test
  # committing the exact leak it exists to catch.
  if [ -n "$ORPHAN" ]; then
    docker network rm "${ORPHAN}_default" >/dev/null 2>&1
    docker volume rm "${ORPHAN}_db_data" >/dev/null 2>&1
  fi
  return 0
}
trap cleanup EXIT

case "${1:-}" in
  derives)
    FIXTURE="$(mktemp -d)"
    mkdir -p "$FIXTURE/scripts/orca" "$FIXTURE/server/testing"
    cp "$REPO_ROOT"/scripts/orca/{lib.sh,env.sh,archive.sh,compose.sh} "$FIXTURE/scripts/orca/"
    cp "$REPO_ROOT/server/testing/docker-compose.yml" "$FIXTURE/server/testing/"

    # The creation path defines the truth teardown has to match: whatever
    # setup.sh would have named this worktree's stack is what must be removed.
    expected="$("$FIXTURE/scripts/orca/env.sh" | sed -n 's/.*project=\([^ ]*\).*/\1/p')"
    [ -n "$expected" ] || fail "env.sh printed no project name; fixture is broken"

    # Stand in for docker and record which project each call was aimed at. The
    # name travels in the environment, not the argument list, so that is what
    # gets logged.
    mkdir -p "$FIXTURE/bin"
    cat >"$FIXTURE/bin/docker" <<'FAKE'
#!/usr/bin/env bash
echo "PROJECT=${COMPOSE_PROJECT_NAME:-<unset>} ARGS=$*" >> "$DOCKER_CALL_LOG"
exit 0
FAKE
    chmod +x "$FIXTURE/bin/docker"

    # Two ways setup.sh can leave a worktree with no usable .env: it died before
    # env.sh finished, or it died inside env.sh's write. Both must still tear
    # down, and neither is hypothetical -- .env is generated, gitignored, and
    # written by a hook that is itself allowed to fail.
    for state in truncated missing; do
      log="$FIXTURE/docker-$state.log"
      : > "$log"
      case "$state" in
        truncated) printf '# truncated\n' > "$FIXTURE/.env" ;;
        missing)   rm -f "$FIXTURE/.env" ;;
      esac

      DOCKER_CALL_LOG="$log" PATH="$FIXTURE/bin:$PATH" \
        "$FIXTURE/scripts/orca/archive.sh" >/dev/null 2>&1
      rc=$?

      # Assert on what was torn down before what the script returned: the
      # failure that matters is a stack left running, and reporting the exit
      # status first would name the mechanism instead of the leak.
      grep -q 'ARGS=compose .* down -v --remove-orphans' "$log" \
        || fail "[$state] archive.sh tore nothing down; docker calls: $(cat "$log")"
      grep -q "PROJECT=$expected .*down" "$log" \
        || fail "[$state] teardown targeted the wrong project; expected $expected, calls: $(cat "$log")"
      grep -q 'PROJECT=<unset>' "$log" \
        && fail "[$state] teardown ran with no project name -- compose falls back to rmx-local"
      [ "$rc" -eq 0 ] \
        || fail "[$state] archive.sh exited $rc against a stubbed docker"
    done

    echo "PASS: archive.sh targets $expected with .env truncated and with it missing"
    ;;

  reap)
    docker info >/dev/null 2>&1 || { echo "SKIP: docker is not running"; exit $SKIP; }

    # A stack with no worktree, built by hand out of the pieces compose leaves
    # behind when a worktree is deleted with `rm -rf`.
    ORPHAN="rmx-reap-test-$$"
    docker volume create --label "com.docker.compose.project=$ORPHAN" "${ORPHAN}_db_data" >/dev/null \
      || fail "could not create the fixture volume"
    docker network create --label "com.docker.compose.project=$ORPHAN" "${ORPHAN}_default" >/dev/null \
      || fail "could not create the fixture network"

    out="$("$REPO_ROOT/scripts/orca/reap.sh" 2>&1)" \
      || fail "reap.sh exited non-zero; got: $out"

    grep -q "$ORPHAN" <<<"$out" \
      || fail "reap.sh did not flag a stack with no worktree; got: $out"

    # The dangerous failure is the opposite one. This worktree is live and its
    # database is in use; reap must derive its project name and leave it alone.
    live="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' "$REPO_ROOT/.env")"
    [ -n "$live" ] || fail "no COMPOSE_PROJECT_NAME in .env; run ./scripts/orca/env.sh"
    grep -q "^== $live" <<<"$out" \
      && fail "reap.sh listed the live worktree's own stack ($live) as stale"

    echo "PASS: reap.sh flags $ORPHAN and protects $live"
    ;;

  *)
    echo "usage: $0 derives|reap" >&2
    exit 2
    ;;
esac
