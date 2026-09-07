#!/usr/bin/env bash
# Covers scripts/orca/answer-review.sh -- the answer that keeps a review's
# findings from being outrun by the auto-merge armed before the review ran.
#
#   test_orca_answer_review.sh posts     an ordinary answer -> a comment
#                                        carrying the marker merge_gate.py
#                                        matches, and the gate's failed run on
#                                        this head is re-run. No GitHub event
#                                        does that: an `issue_comment` run's
#                                        check attaches to the default branch,
#                                        not to this PR's head.
#   test_orca_answer_review.sh thin      "ok" -> refused, nothing posted. An
#                                        answer the gate will not count is worse
#                                        than none: it looks done and the PR
#                                        stays red for a reason nothing states.
#   test_orca_answer_review.sh unpushed  the worktree is ahead of the PR ->
#                                        refused. The review to answer is the
#                                        one of the code GitHub actually has.
#   test_orca_answer_review.sh stopped   the fleet stop file exists -> exit 3 and
#                                        write nothing.
#   test_orca_answer_review.sh gate      the posted answer satisfies the gate it
#                                        was written for -- the two halves are
#                                        checked against each other rather than
#                                        each against its own idea of the marker.
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
# branch and head lookups answer.
make_fixture() {
  WORK="$(mktemp -d)"; WORK="$(cd "$WORK" && pwd -P)"
  mkdir -p "$WORK/repo/scripts/orca" "$WORK/repo/.github/scripts" "$WORK/bin"
  cp "$REPO_ROOT/scripts/orca/lib.sh" "$REPO_ROOT/scripts/orca/answer-review.sh" \
     "$WORK/repo/scripts/orca/"
  # EVERY .py: answer-review.sh asks merge_gate.py for the marker and for the
  # length that counts as an answer, and merge_gate.py imports its siblings.
  cp "$REPO_ROOT"/.github/scripts/*.py "$WORK/repo/.github/scripts/"
  git -C "$WORK/repo" init -q -b work
  git -C "$WORK/repo" -c user.email=t@t -c user.name=t commit -q --allow-empty -m init

  # The PR's head, as `gh pr view` reports it. Equal to the worktree's own HEAD
  # except in the `unpushed` phase, which is the whole point of that phase.
  PR_HEAD="$(git -C "$WORK/repo" rev-parse HEAD)"
  [ "${1:-}" = unpushed ] && PR_HEAD="0000000000000000000000000000000000000000"

  cat >"$WORK/bin/gh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$GH_CALLS"
case "$*" in
  *"repo view"*) echo "armaatus/rommsync-nx"; exit 0 ;;
  *"pr list"*)   echo 131; exit 0 ;;
  *"pr view"*)   cat "$GH_HEAD"; exit 0 ;;
  *"pr comment"*)
    for arg in "$@"; do
      [ -f "$arg" ] && [ "${arg#--}" = "$arg" ] && cp "$arg" "$GH_POSTED"
    done
    exit 0 ;;
  *"run list"*)
    echo '[{"databaseId":9,"conclusion":"failure","headSha":"'"$(cat "$GH_HEAD")"'"}]'
    exit 0 ;;
  *"actions/runs/9/jobs"*) echo 4242; exit 0 ;;
  *"run rerun"*) exit 0 ;;
esac
exit 0
STUB
  chmod +x "$WORK/bin/gh"
  GH_CALLS="$WORK/calls"; : >"$GH_CALLS"
  GH_HEAD="$WORK/head"; printf '%s' "$PR_HEAD" >"$GH_HEAD"
  GH_POSTED="$WORK/posted"; : >"$GH_POSTED"
  export GH_CALLS GH_HEAD GH_POSTED
  export ROMMSYNC_FLEET_DIR="$WORK/fleet"
  mkdir -p "$ROMMSYNC_FLEET_DIR"
  PATH="$WORK/bin:$PATH"
}

run_it() { (cd "$WORK/repo" && ./scripts/orca/answer-review.sh "$@"); }

ANSWER="Reworded the comment above sync_tick(); the retry already backs off."

case "${1:-}" in
  posts)
    make_fixture
    out="$(run_it "$ANSWER" 2>&1)" || fail "answer-review.sh exited non-zero: $out"
    grep -q 'pr comment' "$GH_CALLS" || fail "no comment was posted: $out"
    grep -q 'review-answered' "$GH_POSTED" \
      || fail "the posted comment carries no marker, so merge-gate cannot see it"
    grep -qF "$ANSWER" "$GH_POSTED" \
      || fail "the posted comment does not carry what the author actually said"
    grep -q 'run rerun --job 4242' "$GH_CALLS" \
      || { echo "$out" >&2; fail "the gate job was not re-run, so nothing asks merge-gate again"; }
    echo "ok: the answer is posted with its marker, and the gate is asked again"
    ;;
  thin)
    make_fixture
    run_it "ok" >/dev/null 2>&1
    [ "$?" = 2 ] || fail "a one-word answer was not refused"
    grep -q 'pr comment' "$GH_CALLS" \
      && fail "it posted an answer the gate will not count, which reads as done and is not"
    echo "ok: an answer with nothing in it is refused before it is posted"
    ;;
  unpushed)
    make_fixture unpushed
    out="$(run_it "$ANSWER" 2>&1)"
    [ "$?" = 2 ] || fail "answering a head GitHub does not have was not refused: $out"
    grep -q 'pr comment' "$GH_CALLS" \
      && fail "it answered a review of code that was never pushed"
    grep -qi 'push it first' <<<"$out" || fail "it did not say what to do instead: $out"
    echo "ok: an answer is refused while the worktree is ahead of the PR"
    ;;
  stopped)
    make_fixture
    : >"$ROMMSYNC_FLEET_DIR/STOP"
    run_it "$ANSWER" >/dev/null 2>&1
    [ "$?" = 3 ] || fail "a stopped fleet did not exit 3"
    grep -q 'pr comment' "$GH_CALLS" \
      && fail "it wrote to a pull request while the fleet was stopped"
    echo "ok: a stopped fleet answers nothing"
    ;;
  gate)
    # The two halves against each other. Both sides read the marker from
    # merge_gate.py, so a format change cannot break them apart -- but a change
    # to what `answered()` REQUIRES (the author, the head, the ordering) can,
    # and it would be silent: answers that satisfy nothing.
    make_fixture
    out="$(run_it "$ANSWER" 2>&1)" || fail "answer-review.sh exited non-zero: $out"
    head="$(cat "$GH_HEAD")"
    python3 - "$GH_POSTED" "$head" "$REPO_ROOT" <<'PY' || fail "the answer this script posts does not satisfy the gate it was written for"
import json, sys
posted, head, root = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, root + "/.github/scripts")
from merge_gate import evaluate

review = {"state": "COMMENTED", "submittedAt": "2026-09-06T02:00:00Z",
          "commit": {"oid": head}, "author": {"login": "claude[bot]"},
          "body": "Nit: the comment says what, not why.\n"
                  "<!-- review-findings: 1 -->"}
pull = {
    "author": {"login": "armaatus"},
    "body": "Closes #170\n/code-review\nmattpocock-skills:code-review",
    "reviews": {"nodes": [review]},
    "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
    "comments": {"nodes": [{"author": {"login": "armaatus"},
                            "createdAt": "2026-09-06T02:30:00Z",
                            "body": open(posted).read()}]},
}
ok, lines = evaluate(head, pull, ["core/src/sync.cpp"])
if not ok:
    print("\n".join(lines), file=sys.stderr)
sys.exit(0 if ok else 1)
PY
    echo "ok: what this script writes is what the gate accepts as an answer"
    ;;
  *)
    echo "usage: test_orca_answer_review.sh posts|thin|unpushed|stopped|gate" >&2
    exit 2 ;;
esac
