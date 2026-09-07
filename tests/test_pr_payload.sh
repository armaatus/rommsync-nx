#!/usr/bin/env bash
# Covers .github/scripts/pr_payload.sh -- the one paginated read of a pull
# request that both `merge-gate.yml` and `review-status.sh` judge from.
#
#   test_pr_payload.sh pages    a PR with more threads than one page -> every
#                               thread comes back, the second call carries the
#                               first page's cursor, and the result does not
#                               claim to be partial.
#   test_pr_payload.sh capped   the page cap is reached before the list ends ->
#                               hasNextPage stays true, and merge_gate.py refuses
#                               to answer from it. This is the bug the whole
#                               script exists for: 101 threads, the hundred it
#                               got all resolved, and the gate reporting "no
#                               review thread is unresolved".
#   test_pr_payload.sh fails    gh cannot answer -> non-zero, not an empty PR.
#
# `gh` is stubbed on PATH, so none of this needs a network or a pull request.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() { echo "FAIL: $*" >&2; exit 1; }

WORK=""
cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; return 0; }
trap cleanup EXIT

# A gh stub whose behaviour is a mode file, so one stub covers every phase. It
# records each invocation's arguments, which is how the cursor is asserted.
make_stub() {
  WORK="$(mktemp -d)"
  mkdir -p "$WORK/bin"
  cat >"$WORK/bin/gh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$GH_CALLS"
mode="$(cat "$GH_MODE")"
[ "$mode" = fail ] && { echo "gh: nope" >&2; exit 1; }
after=""
for arg in "$@"; do
  case "$arg" in after=*) after="${arg#after=}" ;; esac
done
page=1
[ -n "$after" ] && page=2
python3 - "$page" "$mode" <<'PY'
import json, sys
page, mode = int(sys.argv[1]), sys.argv[2]
if mode == "capped":
    nodes = [{"id": f"T{page}-{i}", "isResolved": True, "path": "a.cpp", "line": i}
             for i in range(100)]
    info = {"hasNextPage": True, "endCursor": f"cursor-{page}"}
elif page == 1:
    nodes = [{"id": f"T1-{i}", "isResolved": True, "path": "a.cpp", "line": i}
             for i in range(100)]
    info = {"hasNextPage": True, "endCursor": "cursor-one"}
else:
    nodes = [{"id": "T2-0", "isResolved": False, "path": "b.cpp", "line": 7}]
    info = {"hasNextPage": False, "endCursor": None}
print(json.dumps({"data": {"repository": {"pullRequest": {
    "body": "/code-review\nmattpocock-skills:code-review",
    "author": {"login": "armaatus"},
    "reviews": {"nodes": [{
        "state": "COMMENTED", "submittedAt": "2026-09-06T10:00:00Z",
        "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
        "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."}]},
    "reviewThreads": {"pageInfo": info, "nodes": nodes},
}}}}))
PY
STUB
  chmod +x "$WORK/bin/gh"
  GH_CALLS="$WORK/calls"; : >"$GH_CALLS"
  GH_MODE="$WORK/mode"; printf '%s' "${1:-pages}" >"$GH_MODE"
  export GH_CALLS GH_MODE
  PATH="$WORK/bin:$PATH"
}

count_nodes() {
  python3 -c '
import json, sys
doc = json.load(open(sys.argv[1]))
print(len(doc["data"]["repository"]["pullRequest"]["reviewThreads"]["nodes"]))' "$1"
}
has_next() {
  python3 -c '
import json, sys
doc = json.load(open(sys.argv[1]))
info = doc["data"]["repository"]["pullRequest"]["reviewThreads"]["pageInfo"]
print("yes" if info.get("hasNextPage") else "no")' "$1"
}

case "${1:-}" in
  pages)
    make_stub pages
    out="$WORK/pr.json"
    "$REPO_ROOT/.github/scripts/pr_payload.sh" armaatus rommsync-nx 131 >"$out" \
      || fail "pr_payload.sh exited non-zero on a two-page PR"
    [ "$(count_nodes "$out")" = 101 ] \
      || fail "expected 101 threads across two pages, got $(count_nodes "$out")"
    [ "$(has_next "$out")" = no ] \
      || fail "the list was paged to its end but the result still claims to be partial"
    grep -q 'after=cursor-one' "$GH_CALLS" \
      || fail "the second call did not carry the first page's cursor"
    # The unresolved thread lives on page two: the exact one the old query lost.
    python3 -c '
import json, sys
threads = json.load(open(sys.argv[1]))["data"]["repository"]["pullRequest"]["reviewThreads"]["nodes"]
sys.exit(0 if any(not t["isResolved"] for t in threads) else 1)' "$out" \
      || fail "the open thread on page two is missing from the merged result"
    echo "ok: every page of threads comes back, and the result is not marked partial"
    ;;
  capped)
    make_stub capped
    out="$WORK/pr.json"
    PR_PAYLOAD_MAX_PAGES=2 "$REPO_ROOT/.github/scripts/pr_payload.sh" \
      armaatus rommsync-nx 131 >"$out" || fail "pr_payload.sh exited non-zero at the cap"
    [ "$(has_next "$out")" = yes ] \
      || fail "the page cap was reached and the result does not say the list is partial"
    # ...and the gate refuses it, rather than reporting a clean thread list.
    files="$WORK/files.txt"; echo "core/src/sync.cpp" >"$files"
    if python3 "$REPO_ROOT/.github/scripts/merge_gate.py" abc123 "$out" "$files" >"$WORK/out" 2>&1; then
      fail "merge_gate.py passed a PR whose thread list it only half read"
    fi
    grep -qi "truncated" "$WORK/out" \
      || { cat "$WORK/out" >&2; fail "the gate refused but did not say the threads were truncated"; }
    echo "ok: a truncated thread list is a refusal, not a clean answer"
    ;;
  fails)
    make_stub fail
    if "$REPO_ROOT/.github/scripts/pr_payload.sh" armaatus rommsync-nx 131 >/dev/null 2>&1; then
      fail "pr_payload.sh reported success when gh could not answer"
    fi
    echo "ok: a PR that could not be read is an error, not an empty one"
    ;;
  *)
    echo "usage: test_pr_payload.sh pages|capped|fails" >&2; exit 2 ;;
esac
