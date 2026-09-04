#!/usr/bin/env bash
# Covers scripts/orca/env.sh -- the first thing setup.sh runs, and the one step
# whose failure costs the whole worktree.
#
# env.sh is not the only caller of itself: compose.sh generates a missing .env
# before it does anything, so the `romm` tab -- which polls compose.sh from the
# moment Orca opens it -- writes .env too, at the same moment setup.sh does.
#
#   test_orca_env.sh concurrent   two writers at once both succeed and agree.
#                                 This is the regression: sharing one `.env.tmp`
#                                 let the first `mv` take the second's source
#                                 away, and the loser's ENOENT aborted setup.sh
#                                 at its first step under `set -e` -- a worktree
#                                 with no build, no venv and no RomM, observed
#                                 on a real worktree on 2026-09-04.
#   test_orca_env.sh readable     a reader never observes a partial .env, which
#                                 is what the atomic rename was for originally.
#
# Neither phase needs Docker or Orca.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() { echo "FAIL: $*" >&2; exit 1; }

FIXTURE=""
cleanup() { [ -n "$FIXTURE" ] && rm -rf "$FIXTURE"; return 0; }
trap cleanup EXIT

# env.sh derives everything from its own location, so a copy of the scripts in a
# throwaway directory derives that directory's ports and touches nothing real.
make_fixture() {
  FIXTURE="$(mktemp -d)"
  mkdir -p "$FIXTURE/scripts/orca"
  cp "$REPO_ROOT"/scripts/orca/{env.sh,lib.sh,compose.sh} "$FIXTURE/scripts/orca/"
}

WRITERS=6

case "${1:-}" in
  concurrent)
    make_fixture
    pids=""
    for i in $(seq 1 "$WRITERS"); do
      "$FIXTURE/scripts/orca/env.sh" >"$FIXTURE/out.$i" 2>&1 &
      pids="$pids $!"
    done
    failed=0
    for pid in $pids; do wait "$pid" || failed=$((failed + 1)); done
    [ "$failed" -eq 0 ] \
      || fail "$failed of $WRITERS concurrent writers failed; setup.sh dies here: $(cat "$FIXTURE"/out.* | grep -i 'mv:\|error' | head -3)"

    [ -f "$FIXTURE/.env" ] || fail "no .env was published at all"
    grep -q '^COMPOSE_PROJECT_NAME=rmx-' "$FIXTURE/.env" \
      || fail "published .env names no stack: $(cat "$FIXTURE/.env")"

    # Every writer derives from the same path, so disagreement would mean the
    # published file was spliced together from two of them.
    want="$(grep -c . "$FIXTURE/.env")"
    for i in $(seq 1 "$WRITERS"); do
      grep -q 'project=rmx-' "$FIXTURE/out.$i" \
        || fail "writer $i reported no derivation: $(cat "$FIXTURE/out.$i")"
    done
    [ "$want" -ge 8 ] || fail ".env looks truncated ($want lines): $(cat "$FIXTURE/.env")"

    # A temp file left behind is the next run's confusion, and .gitignore only
    # covers the names this script actually uses.
    leftovers="$(find "$FIXTURE" -maxdepth 1 -name '.env.tmp*' | wc -l | tr -d ' ')"
    [ "$leftovers" = "0" ] || fail "$leftovers temp files left behind"
    echo "PASS: $WRITERS concurrent writers all succeed and publish one whole .env"
    ;;

  readable)
    make_fixture
    "$FIXTURE/scripts/orca/env.sh" >/dev/null 2>&1 || fail "first run failed"

    # Rewrite in a loop while reading: a non-atomic publish shows up as a read
    # that finds no COMPOSE_PROJECT_NAME, which is what silently retargets
    # docker compose at the wrong stack.
    ( for _ in $(seq 1 25); do "$FIXTURE/scripts/orca/env.sh" >/dev/null 2>&1; done ) &
    writer=$!
    bad=0
    while kill -0 "$writer" 2>/dev/null; do
      grep -q '^COMPOSE_PROJECT_NAME=rmx-' "$FIXTURE/.env" 2>/dev/null || bad=$((bad + 1))
    done
    wait "$writer" 2>/dev/null
    [ "$bad" -eq 0 ] || fail "$bad reads saw a .env with no stack name"
    echo "PASS: .env is never observed partially written"
    ;;

  *)
    echo "usage: $0 concurrent|readable" >&2
    exit 2
    ;;
esac
