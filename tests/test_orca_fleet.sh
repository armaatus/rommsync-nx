#!/usr/bin/env bash
# Covers the two places scripts/orca/fleet.sh used to report a state it had not
# established: the board update, and the worktree removal.
#
#   test_orca_fleet.sh card_says      the Orca CLI refuses -> the dispatcher log
#                                     says the board update failed, and carries
#                                     the CLI's own words. WORKFLOW.md calls the
#                                     board THE status surface, so a silent
#                                     failure freezes it with nothing anywhere
#                                     saying it froze.
#   test_orca_fleet.sh card_quiet     it worked -> nothing is said. A warning per
#                                     poll would be its own kind of noise.
#   test_orca_fleet.sh remove_forces  `worktree rm` fails and `--force` works ->
#                                     the worktree is removed. This repo has a
#                                     submodule and git refuses the plain
#                                     removal outright, so without the retry
#                                     EVERY merged worktree leaks.
#   test_orca_fleet.sh remove_advice  both fail -> reap_merged says how to remove
#                                     it by hand, and does NOT name reap.sh,
#                                     which skips exactly this case and prints
#                                     "nothing to reap".
#
# The Orca CLI and gh are stubbed on PATH; the fleet state dir is a temp dir.
# Nothing here touches a real worktree, docker, or GitHub.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() { echo "FAIL: $*" >&2; exit 1; }

WORK=""
cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; return 0; }
trap cleanup EXIT

# $1 is the CLI stub's mode: ok, set_fails, rm_needs_force, rm_never_works.
make_fixture() {
  WORK="$(mktemp -d)"; WORK="$(cd "$WORK" && pwd -P)"
  mkdir -p "$WORK/repo/scripts/orca" "$WORK/bin"
  cp "$REPO_ROOT/scripts/orca/fleet.sh" "$REPO_ROOT/scripts/orca/lib.sh" \
     "$WORK/repo/scripts/orca/"

  cat >"$WORK/bin/orca-stub" <<'STUB'
#!/usr/bin/env bash
mode="$(cat "$ORCA_MODE")"
printf '%s\n' "$*" >>"$ORCA_CALLS"
case "$1" in
  --version) echo "orca 0.0.0-test"; exit 0 ;;
esac
target=""
for arg in "$@"; do
  case "$arg" in path:*) target="${arg#path:}" ;; esac
done
case "$2" in
  set)
    # On STDERR, where a CLI actually reports a failure. The point of the check
    # under test is that the reason reaches the log, and a stub that printed it
    # on stdout would pass a card() that drops stderr on the floor.
    [ "$mode" = set_fails ] && { echo "Unable to determine Orca.app path from symlink" >&2; exit 1; }
    echo '{"ok":true}'; exit 0 ;;
  rm)
    case " $* " in *" --force "*) forced=1 ;; *) forced=0 ;; esac
    if [ "$mode" = rm_never_works ] || { [ "$mode" = rm_needs_force ] && [ "$forced" = 0 ]; }; then
      echo "fatal: working trees containing submodules cannot be moved or removed" >&2
      exit 1
    fi
    rm -rf "$target"; echo '{"ok":true}'; exit 0 ;;
esac
echo '{"ok":true}'
STUB
  chmod +x "$WORK/bin/orca-stub"

  cat >"$WORK/bin/gh" <<'STUB'
#!/usr/bin/env bash
case "$*" in
  *"pr list"*) echo 7; exit 0 ;;
esac
echo ""
STUB
  chmod +x "$WORK/bin/gh"

  ORCA_CALLS="$WORK/calls"; : >"$ORCA_CALLS"
  ORCA_MODE="$WORK/mode"; printf '%s' "${1:-ok}" >"$ORCA_MODE"
  export ORCA_CALLS ORCA_MODE
  export ORCA_CLI_COMMAND="$WORK/bin/orca-stub"
  export ROMMSYNC_FLEET_DIR="$WORK/fleet"
  PATH="$WORK/bin:$PATH"
  export PATH
}

# A worktree the fleet would own: a git repo with one commit, and an owned-file
# naming it.
make_worktree() {
  mkdir -p "$WORK/wt"
  git -C "$WORK/wt" init -q -b work
  git -C "$WORK/wt" -c user.email=t@t -c user.name=t commit -q --allow-empty -m init
  mkdir -p "$ROMMSYNC_FLEET_DIR/worktrees"
  printf '%s\n' "$WORK/wt" >"$ROMMSYNC_FLEET_DIR/worktrees/42"
}

# fleet.sh returns instead of dispatching when it is sourced, so one function can
# be exercised without starting a dispatcher.
in_fleet() { (cd "$WORK/repo" && . ./scripts/orca/fleet.sh && "$@"); }

case "${1:-}" in
  card_says)
    make_fixture set_fails
    out="$(in_fleet card "/some/worktree" --workspace-status in-progress --comment "#42: building" 2>&1)"
    grep -qi "board update FAILED" <<<"$out" \
      || fail "a refused board update said nothing: $out"
    grep -q "Unable to determine Orca.app path" <<<"$out" \
      || fail "the CLI's own reason was dropped: $out"
    grep -q "in-progress" <<<"$out" \
      || fail "the log does not say which update was lost: $out"
    echo "ok: a board update that failed is in the dispatcher log"
    ;;
  card_quiet)
    make_fixture ok
    out="$(in_fleet card "/some/worktree" --comment "#42: building" 2>&1)"
    grep -qi "fail" <<<"$out" && fail "a successful board update complained: $out"
    echo "ok: a board update that worked says nothing"
    ;;
  remove_forces)
    make_fixture rm_needs_force
    make_worktree
    in_fleet remove_worktree "$WORK/wt" >/dev/null 2>&1 \
      || fail "remove_worktree gave up on a worktree --force would have removed"
    [ -d "$WORK/wt" ] && fail "it reported success and the worktree is still there"
    grep -q -- "--force" "$ORCA_CALLS" \
      || fail "the retry did not pass --force, so the submodule refusal stands"
    echo "ok: a worktree git refuses to remove is removed with --force"
    ;;
  remove_advice)
    make_fixture rm_never_works
    make_worktree
    out="$(in_fleet reap_merged 2>&1)"
    grep -q "could not remove it" <<<"$out" \
      || fail "a failed removal was not reported: $out"
    grep -q "git worktree remove --force" <<<"$out" \
      || fail "it did not say how to remove it by hand: $out"
    grep -q "reap.sh" <<<"$out" \
      && fail "it still points at reap.sh, which skips a worktree that is still there: $out"
    grep -q "containing submodules" <<<"$out" \
      || fail "the reason git gave was dropped; it is the difference between this and a hung CLI: $out"
    echo "ok: a failed removal says something that would actually clean it up"
    ;;
  *)
    echo "usage: test_orca_fleet.sh card_says|card_quiet|remove_forces|remove_advice" >&2
    exit 2 ;;
esac
