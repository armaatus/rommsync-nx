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
#   test_orca_env.sh venv         a .venv whose interpreter is missing, dangling
#                                 or too old is replaced, and a good one kept.
#                                 The dangling case is the sharp one: `[ -x ]`
#                                 is false for a dangling symlink, so a guard
#                                 written that way leaves the venv in place and
#                                 `python -m venv` over it exits 1 -- every
#                                 re-run, forever.
#   test_orca_env.sh setup_fails_fast
#                                 setup.sh itself stops on the interpreter
#                                 before seeding or building, and says why.
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

# A PATH that has the ordinary tools but NO real python, so the only
# interpreters discoverable are the fakes.
#
# Stripping PATH down to the fakes alone does not work: orca_python_candidates
# sorts, orca_python_is_usable tails, and setup.sh's own preamble needs
# `dirname` -- with those missing the picker collapses to bare `python3` and the
# phase would pass for the wrong reason. Only python is withheld.
fake_only_path() {
  local tool
  mkdir -p "$FIXTURE/realbin"
  for tool in bash sh dirname basename sort tail head sed grep cat mkdir rm ln chmod cut tr uname; do
    [ -e "$FIXTURE/realbin/$tool" ] && continue
    tool_path="$(command -v "$tool" 2>/dev/null)" || continue
    ln -s "$tool_path" "$FIXTURE/realbin/$tool"
  done
  echo "$FIXTURE/fakebin:$FIXTURE/realbin"
}

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

    # `PATH=x var=$(...)` would be two assignments to the CURRENT shell, not a
    # prefix for one command -- it overwrites PATH for the rest of the phase and
    # every later `mkdir` fails. Saved and restored instead.
    REAL_PATH="$PATH"
    FAKE_PATH=""
    with_fake_path() { [ -n "$FAKE_PATH" ] || FAKE_PATH="$(fake_only_path)"; PATH="$FAKE_PATH"; }
    restore_path() { PATH="$REAL_PATH"; }

    # Only an old interpreter on PATH. Picking it is the bug: pip then fails
    # with a wall of version numbers instead of anything actionable.
    fake_python python3 3.9
    # Proved to work first, and to report what it claims. Without this the
    # assertion below passes for any broken fake -- a bad shebang, a missing
    # real python to delegate to -- and never shows a 3.9 was turned down FOR
    # BEING 3.9.
    [ "$("$FIXTURE/fakebin/python3" -V 2>&1)" = "Python 3.9" ] \
      || fail "the fake interpreter is broken, so rejecting it would prove nothing: $("$FIXTURE/fakebin/python3" -V 2>&1)"
    with_fake_path
    orca_python_is_usable python3; usable=$?
    picked="$(orca_pick_python)"; found=$?
    restore_path
    [ "$usable" -eq 0 ] && fail "a 3.9 was accepted as usable for requirements.txt"
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

    # Discovered, not listed: a version past whatever was current when this was
    # written must still be found, or the message tells someone to install a
    # Python they already have.
    fake_python python3.99 3.99
    with_fake_path
    picked="$(orca_pick_python)"; restore_path
    [ "$picked" = python3.99 ] \
      || fail "picked $picked; a newer interpreter than the code knew about was not considered"

    # An interpreter that cannot run at all is not "too old" -- saying so
    # contradicts the version setup.sh prints beside it.
    printf '#!/bin/bash\nexit 9\n' >"$FIXTURE/fakebin/python3.98"
    chmod +x "$FIXTURE/fakebin/python3.98"
    with_fake_path
    orca_python_is_usable python3.98
    reason="$orca_python_reject"
    restore_path
    [ -n "$reason" ] || fail "an unrunnable interpreter was rejected with no reason given"
    case "$reason" in *"too old"*) fail "an unrunnable interpreter was reported as too old: $reason" ;; esac

    # And the real one this machine has, so the phase is not only about fakes.
    picked="$(orca_pick_python)" || fail "no usable interpreter on this machine's PATH"
    orca_python_is_usable "$picked" \
      || fail "orca_pick_python returned $picked, which its own check rejects"
    echo "PASS: the venv interpreter is chosen and probed, not inherited"
    ;;

  venv)
    # The regression that makes a worktree unrecoverable. A venv's bin/python is
    # a SYMLINK to the interpreter that built it; a Homebrew minor upgrade
    # leaves it dangling. `[ -x ]` is false for a dangling link, so a guard
    # written that way reads the venv as absent, leaves it in place, and
    # `python -m venv` over it exits 1 on the broken link -- forever, on every
    # re-run.
    FIXTURE="$(mktemp -d)"
    . "$REPO_ROOT/scripts/orca/lib.sh"

    mkdir -p "$FIXTURE/dangling/bin"
    ln -s "$FIXTURE/no-such-interpreter" "$FIXTURE/dangling/bin/python"
    [ -x "$FIXTURE/dangling/bin/python" ] \
      && fail "fixture is wrong: the symlink is not dangling, so this proves nothing"
    orca_venv_is_usable "$FIXTURE/dangling" \
      && fail "a venv whose interpreter is gone was called usable; setup.sh would leave it and die"

    # Absent entirely.
    mkdir -p "$FIXTURE/empty"
    orca_venv_is_usable "$FIXTURE/empty" \
      && fail "a venv with no interpreter at all was called usable"

    # Too old, the case the guard was originally written for.
    mkdir -p "$FIXTURE/old/bin"
    fake_python python3 3.9
    cp "$FIXTURE/fakebin/python3" "$FIXTURE/old/bin/python"
    orca_venv_is_usable "$FIXTURE/old" \
      && fail "a venv built by a too-old interpreter was called usable"

    # A real, current one is kept -- or every setup run throws away a good venv.
    real="$(orca_pick_python)" || fail "no usable interpreter to build a real venv with"
    "$real" -m venv "$FIXTURE/good" >/dev/null 2>&1 \
      || fail "could not build a venv with $real, so the positive case is untested"
    orca_venv_is_usable "$FIXTURE/good" \
      || fail "a freshly built venv was called unusable; setup.sh would delete it every run"
    echo "PASS: a missing, dangling or too-old venv is replaced, and a good one is kept"
    ;;

  setup_fails_fast)
    # setup.sh's own half. Without running it, losing the `. lib.sh` line or the
    # check itself goes unnoticed -- the picker can be perfect and the hook
    # still broken.
    FIXTURE="$(mktemp -d)"
    mkdir -p "$FIXTURE/scripts/orca"
    cp "$REPO_ROOT"/scripts/orca/{setup.sh,lib.sh,env.sh,compose.sh} "$FIXTURE/scripts/orca/"
    fake_python python3 3.9

    # PATH holds nothing but the old interpreter. If the check is not first,
    # setup.sh reaches env.sh or seed.sh and fails over a missing tool instead.
    # Invoked through bash by absolute path: setup.sh's `#!/usr/bin/env bash`
    # would need `env` and `bash` resolvable on the stripped PATH, and failing
    # to exec is not the failure this phase is about.
    fake_path="$(fake_only_path)"
    out="$(cd "$FIXTURE" && PATH="$fake_path" "$BASH" ./scripts/orca/setup.sh 2>&1)"
    rc=$?
    [ "$rc" -ne 0 ] || fail "setup.sh carried on with an interpreter too old to install requirements.txt"
    grep -qi "python" <<<"$out" \
      || fail "setup.sh failed without mentioning Python, which is the whole diagnosis: $out"
    # Nothing expensive may have happened first: no .env means it stopped before
    # env.sh, which is what makes the failure fast as well as clear.
    [ -e "$FIXTURE/.env" ] \
      && fail "setup.sh got as far as deriving .env before checking the interpreter"
    echo "PASS: setup.sh stops on the interpreter before doing any work, and says why"
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
    echo "usage: $0 concurrent|readable|python|venv|setup_fails_fast" >&2
    exit 2
    ;;
esac
