#!/usr/bin/env bash
# Covers scripts/orca/resolve-thread.sh -- resolving a review thread AND asking
# merge-gate again, which is the half no GitHub event does.
#
#   test_orca_resolve_thread.sh last     the last open thread is resolved -> the
#                                        mutation goes out for every id and the
#                                        gate's failed run on this head is
#                                        re-run, so the check re-evaluates with
#                                        no push and no person.
#   test_orca_resolve_thread.sh more     one thread is still open -> resolve, and
#                                        do NOT re-run: the gate would fail again
#                                        for the same honest reason.
#   test_orca_resolve_thread.sh partial  the thread list came back truncated ->
#                                        no re-run. "None left" read off a first
#                                        page is the bug this repo is fixing, not
#                                        a reason to act.
#   test_orca_resolve_thread.sh stopped  the fleet stop file exists -> exit 3 and
#                                        mutate nothing.
#
# `gh` is stubbed on PATH and the fleet state dir is a temp dir, so nothing here
# touches a pull request or the machine's fleet.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() { echo "FAIL: $*" >&2; exit 1; }

WORK=""
cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; return 0; }
trap cleanup EXIT

# A worktree holding just the scripts under test, as its own git repo so the
# branch lookup answers.
make_fixture() {
  WORK="$(mktemp -d)"; WORK="$(cd "$WORK" && pwd -P)"
  mkdir -p "$WORK/repo/scripts/orca" "$WORK/repo/.github/scripts" "$WORK/bin"
  cp "$REPO_ROOT/scripts/orca/lib.sh" "$REPO_ROOT/scripts/orca/resolve-thread.sh" \
     "$WORK/repo/scripts/orca/"
  # merge_gate.py as well as the gather: resolve-thread.sh asks the gate whether
  # a thread list may be read as "none left" rather than deciding that itself.
  cp "$REPO_ROOT/.github/scripts/pr_payload.sh" \
     "$REPO_ROOT/.github/scripts/merge_gate.py" "$WORK/repo/.github/scripts/"
  git -C "$WORK/repo" init -q -b work
  git -C "$WORK/repo" -c user.email=t@t -c user.name=t commit -q --allow-empty -m init

  cat >"$WORK/bin/gh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$GH_CALLS"
case "$*" in
  *"repo view"*)  echo "armaatus/rommsync-nx"; exit 0 ;;
  *"pr list"*)    echo 131; exit 0 ;;
  *"pr view"*)    echo "deadbeefcafe1234"; exit 0 ;;
  *"run list"*)
    if [ "$(cat "$GH_MODE")" = green ]; then
      # Newest first, as gh returns them: a fresh gate already passed on this
      # head, and an older failure sits under it.
      echo '[{"databaseId":11,"conclusion":"success","headSha":"deadbeefcafe1234"},
             {"databaseId":9,"conclusion":"failure","headSha":"deadbeefcafe1234"}]'
    else
      echo '[{"databaseId":9,"conclusion":"failure","headSha":"deadbeefcafe1234"}]'
    fi
    exit 0 ;;
  *"actions/runs/9/jobs"*) echo 4242; exit 0 ;;
  *"run rerun"*)  exit 0 ;;
esac
case "$*" in
  *mutation*) exit 0 ;;
esac
# The paginated PR read. One page, contents per mode.
python3 - "$GH_MODE" <<'PY'
import json, sys
mode = open(sys.argv[1]).read().strip()
nodes = [{"id": "T1", "isResolved": True, "path": "a.cpp", "line": 1}]
if mode == "more":
    nodes.append({"id": "T2", "isResolved": False, "path": "b.cpp", "line": 2})
info = {"hasNextPage": mode == "partial", "endCursor": None}
print(json.dumps({"data": {"repository": {"pullRequest": {
    "body": "", "author": {"login": "armaatus"}, "reviews": {"nodes": []},
    "reviewThreads": {"pageInfo": info, "nodes": nodes}}}}}))
PY
STUB
  chmod +x "$WORK/bin/gh"
  GH_CALLS="$WORK/calls"; : >"$GH_CALLS"
  GH_MODE="$WORK/mode"; printf '%s' "${1:-last}" >"$GH_MODE"
  export GH_CALLS GH_MODE
  export ROMMSYNC_FLEET_DIR="$WORK/fleet"
  mkdir -p "$ROMMSYNC_FLEET_DIR"
  PATH="$WORK/bin:$PATH"
}

run_it() { (cd "$WORK/repo" && ./scripts/orca/resolve-thread.sh "$@"); }

case "${1:-}" in
  last)
    make_fixture last
    out="$(run_it T1 T2 2>&1)" || fail "resolve-thread.sh exited non-zero: $out"
    grep -q 'mutation' "$GH_CALLS" || fail "no resolveReviewThread mutation was sent"
    [ "$(grep -c 'mutation' "$GH_CALLS")" = 2 ] \
      || fail "expected one mutation per thread id, got $(grep -c 'mutation' "$GH_CALLS")"
    grep -q 'run rerun --job 4242' "$GH_CALLS" \
      || { echo "$out" >&2; fail "the gate job was not re-run, so nothing asks merge-gate again"; }
    echo "ok: the last thread resolved re-asks the gate with no push and no person"
    ;;
  more)
    make_fixture more
    out="$(run_it T1 2>&1)" || fail "resolve-thread.sh exited non-zero: $out"
    grep -q 'run rerun' "$GH_CALLS" \
      && fail "the gate was re-run while a thread is still open; it would fail again"
    grep -qi 'still open' <<<"$out" || fail "it did not say why it left the gate alone: $out"
    echo "ok: a thread still open leaves the gate alone, and says so"
    ;;
  partial)
    make_fixture partial
    out="$(run_it T1 2>&1)" || fail "resolve-thread.sh exited non-zero: $out"
    grep -q 'run rerun' "$GH_CALLS" \
      && fail "the gate was re-run on the strength of a truncated thread list"
    grep -qi 'pages through' <<<"$out" \
      || fail "it did not say the list was partial: $out"
    echo "ok: a truncated thread list is not a reason to declare the PR clean"
    ;;
  green)
    # The newest gate run on this head already passed, so there is nothing to
    # re-ask. Re-running the older failure under it would enter merge-gate's
    # `cancel-in-progress` group and could kill a live run -- the wedge that
    # workflow's own clear-stale job is careful to avoid.
    make_fixture green
    out="$(run_it T1 2>&1)" || fail "resolve-thread.sh exited non-zero: $out"
    grep -q 'run rerun' "$GH_CALLS" \
      && fail "it re-ran an older failed gate while a newer one had already passed"
    grep -q 'not one to re-ask' <<<"$out" || fail "it did not say why it stopped: $out"
    echo "ok: a gate that already passed on this head is left alone"
    ;;

  stopped)
    make_fixture last
    : >"$ROMMSYNC_FLEET_DIR/STOP"
    run_it T1 >/dev/null 2>&1
    [ "$?" = 3 ] || fail "a stopped fleet did not exit 3"
    grep -q 'mutation' "$GH_CALLS" \
      && fail "it mutated a pull request while the fleet was stopped"
    echo "ok: a stopped fleet resolves nothing"
    ;;
  *)
    echo "usage: test_orca_resolve_thread.sh last|more|partial|green|stopped" >&2; exit 2 ;;
esac
