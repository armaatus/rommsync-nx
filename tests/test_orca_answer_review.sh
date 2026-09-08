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
#                                        refused, told to push. The review to
#                                        answer is the one of the code GitHub
#                                        actually has.
#   test_orca_answer_review.sh behind    ...and the other way -> refused, told to
#                                        fetch. "Push it first" on a branch that
#                                        is behind is advice GitHub rejects.
#   test_orca_answer_review.sh no_review no review has been submitted against this
#                                        head yet -> refused. An answer written
#                                        before the review it answers is
#                                        discarded by it, so the agent would be
#                                        told it is done with the gate still red.
#   test_orca_answer_review.sh flight    a gate run is still in flight -> waited
#                                        out and re-asked, not read as nothing
#                                        to do. That run may have read the PR
#                                        before the answer existed.
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
  cp "$REPO_ROOT"/.github/scripts/*.py \
     "$REPO_ROOT/.github/scripts/pr_payload.sh" "$WORK/repo/.github/scripts/"
  git -C "$WORK/repo" init -q -b work
  git() { command git -C "$WORK/repo" -c user.email=t@t -c user.name=t "$@"; }
  git commit -q --allow-empty -m init
  first="$(git rev-parse HEAD)"
  git commit -q --allow-empty -m second

  # The PR's head, as `gh pr view` reports it. Equal to the worktree's own HEAD
  # except in the two phases about them diverging -- and WHICH WAY they diverge
  # is the point of having both: the advice is "push" one way and "fetch" the
  # other, and a script that assumes the first sends an agent to run a push
  # GitHub rejects.
  PR_HEAD="$(git rev-parse HEAD)"
  case "${1:-}" in
    unpushed) PR_HEAD="$first" ;;                    # the worktree is ahead
    behind)   git reset -q --hard "$first" ;;        # ...and here, behind
  esac
  unset -f git

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
    # The `flight` phase's first answer is a run still going: whoever queued it
    # may have read the PR before this comment existed, so "it will evaluate
    # with the new state anyway" is not something this can assume.
    if [ -s "$GH_FLIGHT" ]; then
      : >"$GH_FLIGHT"
      echo '[{"databaseId":9,"conclusion":null,"headSha":"'"$(cat "$GH_HEAD")"'"}]'
    else
      echo '[{"databaseId":9,"conclusion":"failure","headSha":"'"$(cat "$GH_HEAD")"'"}]'
    fi
    exit 0 ;;
  *"actions/runs/9/jobs"*) echo 4242; exit 0 ;;
  *"run rerun"*) exit 0 ;;
  *graphql*)
    # The paginated PR read, in the shape merge_gate.py judges. `$GH_REVIEWED`
    # holds the sha the review is against -- empty for the phase where none has
    # been submitted yet.
    python3 - "$(cat "$GH_REVIEWED")" <<'PY'
import json, sys
oid = sys.argv[1]
reviews = [{"state": "COMMENTED", "submittedAt": "2026-09-06T02:00:00Z",
            "commit": {"oid": oid}, "author": {"login": "claude"},
            "body": "Nit: the comment says what, not why.\n"
                    "<!-- review-findings: 1 -->",
            "comments": {"totalCount": 0}}] if oid else []
print(json.dumps({"data": {"repository": {"pullRequest": {
    "body": "Closes #170\n/code-review\nmattpocock-skills:code-review",
    "author": {"login": "armaatus"},
    "reviews": {"nodes": reviews},
    "comments": {"nodes": []},
    "reviewThreads": {"pageInfo": {"hasNextPage": False, "endCursor": None},
                      "nodes": []}}}}}))
PY
    exit 0 ;;
esac
exit 0
STUB
  chmod +x "$WORK/bin/gh"
  GH_CALLS="$WORK/calls"; : >"$GH_CALLS"
  GH_HEAD="$WORK/head"; printf '%s' "$PR_HEAD" >"$GH_HEAD"
  GH_POSTED="$WORK/posted"; : >"$GH_POSTED"
  # The head the independent review is against -- the PR's, except in the phase
  # where no review has been submitted yet.
  GH_REVIEWED="$WORK/reviewed"
  if [ "${1:-}" = no_review ]; then : >"$GH_REVIEWED"; else printf '%s' "$PR_HEAD" >"$GH_REVIEWED"; fi
  # Non-empty means the next `run list` reports a gate run still in flight.
  GH_FLIGHT="$WORK/flight"; : >"$GH_FLIGHT"
  [ "${1:-}" = flight ] && printf 1 >"$GH_FLIGHT"
  export GH_CALLS GH_HEAD GH_POSTED GH_REVIEWED GH_FLIGHT
  export ORCA_GATE_WAIT_SECONDS=5 ORCA_GATE_POLL_SECONDS=1
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
  behind)
    make_fixture behind
    out="$(run_it "$ANSWER" 2>&1)"
    [ "$?" = 2 ] || fail "answering from a worktree behind the PR was not refused: $out"
    grep -qi 'push it first' <<<"$out" \
      && fail "it told an agent to push a branch that is behind; GitHub rejects that"
    grep -qi 'fetch' <<<"$out" || fail "it did not say what to do instead: $out"
    echo "ok: a worktree behind the PR is told to fetch, not to push"
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
  no_review)
    make_fixture no_review
    out="$(run_it "$ANSWER" 2>&1)"
    [ "$?" = 2 ] || fail "answering a head with no review on it was not refused: $out"
    grep -q 'pr comment' "$GH_CALLS" \
      && fail "it posted an answer to a review that does not exist; the reviewer would discard it"
    grep -q 'await-review.sh' <<<"$out" || fail "it did not say what to do instead: $out"
    echo "ok: an answer is refused while there is no review on this head to answer"
    ;;
  flight)
    # A gate run already going is NOT nothing to do. It may have read the PR
    # before this answer was posted, so it concludes failure on a condition that
    # is now satisfied -- and an issue comment is not a merge-gate trigger, so
    # nothing asks again and the PR sits red with the answer already on it.
    # Reachable straight from the brief: resolve-thread.sh re-runs the gate, and
    # answer-review.sh follows it seconds later.
    make_fixture flight
    out="$(run_it "$ANSWER" 2>&1)" || fail "answer-review.sh exited non-zero: $out"
    grep -q 'run rerun --job 4242' "$GH_CALLS" \
      || { echo "$out" >&2; fail "it took a run still in flight for nothing to do, and never re-asked the gate"; }
    echo "ok: a gate run in flight is waited out, not read as nothing to do"
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
    echo "usage: test_orca_answer_review.sh posts|thin|unpushed|behind|no_review|flight|stopped|gate" >&2
    exit 2 ;;
esac
