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
#   test_orca_env.sh python       setup.sh installs server/requirements.txt with
#                                 an interpreter new enough for it, and says so
#                                 in one line when there is none. This is the
#                                 regression: a worktree created from the Orca
#                                 UI got macOS's 3.9.6 as `python3`, pip filtered
#                                 out every candidate for `requests==2.33.0`
#                                 (requires-python >=3.10) and reported "no
#                                 matching distribution" over two hundred lines,
#                                 naming neither Python nor the reason. setup.sh
#                                 died under `set -e` and the worktree arrived
#                                 with no RomM and no agent -- observed on a real
#                                 worktree on 2026-09-04.
#
# No phase needs Docker or Orca.
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

# A directory holding fake interpreters: $1 is the name, $2 the version it
# reports. Enough of `python3 -V` and the version_info check for orca_pick_python
# to make the same decision it would about a real one.
fake_python() {
  local name="$1" major="${2%%.*}" minor="${2#*.}"
  mkdir -p "$FIXTURE/fakebin"
  # An absolute shebang, not `/usr/bin/env bash`: these run with PATH stripped
  # down to this directory, and `env` would find no bash to hand them to -- the
  # fake would fail to execute and read as "too old" whatever it claims to be.
  cat >"$FIXTURE/fakebin/$name" <<FAKE
#!/bin/bash
case "\${1:-}" in
  -V|--version) echo "Python $2" ;;
  -c) exec "$(command -v python3)" -c "import sys; sys.version_info=($major, $minor, 0); \$2" ;;
esac
FAKE
  chmod +x "$FIXTURE/fakebin/$name"
}

case "${1:-}" in
  python)
    FIXTURE="$(mktemp -d)"
    . "$REPO_ROOT/scripts/orca/lib.sh"

    # Only an old interpreter on PATH. Picking it is the bug: pip then fails
    # with a wall of version numbers instead of anything actionable.
    # `PATH=x var=$(...)` would be two assignments to the CURRENT shell, not a
    # prefix for one command -- it overwrites PATH for the rest of the phase and
    # every later `mkdir` fails. Saved and restored instead.
    REAL_PATH="$PATH"
    with_fake_path() { PATH="$FIXTURE/fakebin"; }
    restore_path() { PATH="$REAL_PATH"; }

    fake_python python3 3.9
    with_fake_path
    picked="$(orca_pick_python)"; found=$?
    restore_path
    [ "$found" -eq 0 ] \
      && fail "picked $picked, an interpreter too old to install requirements.txt"

    # A versioned one beside it is found even though bare `python3` is first on
    # PATH and is the one an inherited environment would have used.
    fake_python python3.12 3.12
    with_fake_path
    picked="$(orca_pick_python)"; found=$?
    restore_path
    [ "$found" -eq 0 ] || fail "found nothing usable while a 3.12 was on PATH"
    [ "$picked" = python3.12 ] \
      || fail "picked $picked instead of the new-enough python3.12"

    # And the real one this machine has, so the phase is not only about fakes.
    picked="$(orca_pick_python)" || fail "no interpreter >= 3.10 on this machine's PATH"
    orca_python_is_new_enough "$picked" \
      || fail "orca_pick_python returned $picked, which its own check rejects"

    # The failure has to be one readable line, not pip's. setup.sh is the only
    # place that can say so, because it is the only one that knows why.
    grep -q 'needs Python >=' "$REPO_ROOT/scripts/orca/setup.sh" \
      || fail "setup.sh does not explain the requirement when no interpreter is new enough"
    echo "PASS: the venv interpreter is chosen, not inherited, and a miss is explained"
    ;;

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
    echo "usage: $0 concurrent|readable|python" >&2
    exit 2
    ;;
esac
