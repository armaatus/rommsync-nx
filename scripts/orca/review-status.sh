#!/usr/bin/env bash
# Is this PR waiting only on GitHub's auto-merge?
#
# The exit code is the answer, so an agent can loop on it without reading prose:
#
#   0  ready -- every thread resolved, every check green, no standing objection
#   1  not ready -- the reasons are printed
#   2  could not tell (no PR, gh failed, no gate to judge by)
#   3  the fleet is stopped
#   4  ready, but a person merges it -- the PR touches the enforcement layer,
#      which merge-gate refuses by design. Nothing here is left to fix.
#
# "Resolved" is read from GitHub's own state. The REST endpoint for PR comments
# cannot report it, so this goes through GraphQL -- `isResolved` on the thread,
# which is the thing a human looks at. `isOutdated` is not `isResolved`: a thread
# whose lines moved is still open.
#
# WHO COUNTS AS A REVIEWER IS NOT DECIDED HERE. This imports
# .github/scripts/merge_gate.py and asks it, because a review-status that says
# "ready" on a PR merge-gate refuses is precisely how a PR sits quietly BLOCKED.
# The rules that mattered when this was a paraphrase -- the latest review per
# author rather than the sticky `reviewDecision`, a self-review not counting, an
# empty review record not counting -- now have one implementation and one place
# to change them.
#
# With one gap that cannot be closed from here: CI runs the BASE branch's copy of
# that script (merge-gate.yml checks out `base.sha`, so a PR cannot pass its own
# gate by rewriting it), while this runs the copy in the worktree. On a PR
# editing merge_gate.py the two therefore judge by different rules -- which is
# also a PR only a person can merge, so the exit-4 answer below says so.
#
# Checks are judged as GitHub judges them: the NEWEST run of each check name.
# `merge-gate` runs several times on one head by design, and every run before the
# review lands is an honest failure that its own later run supersedes;
# `statusCheckRollup` returns all of them. Reading the raw list reported
# "check failed: merge-gate" on PRs that were mergeable and merged seconds later
# (#100, #108), against a loop told to repeat until this exits 0.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/orca/lib.sh

orca_fleet_stopped && { echo "STOPPED: $ORCA_FLEET_STOP exists."; exit 3; }

pr="${1:-}"
[ -n "$pr" ] || pr="$(orca_pr_for_branch)" || {
  echo "no open PR for branch $(git rev-parse --abbrev-ref HEAD)" >&2; exit 2; }

owner_repo="$(GH_PAGER=cat gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)" || exit 2
owner="${owner_repo%%/*}"; name="${owner_repo##*/}"

payload="$(mktemp)"; checks="$(mktemp)"; files="$(mktemp)"
trap 'rm -f "$payload" "$checks" "$files"' EXIT

# Written to files and read back, never spliced into a Python source string.
# Review bodies are third-party text: one containing a quote sequence would
# otherwise break the parse, or worse.
#
# `body`, `author` and each review's own body and inline-comment count are here
# because merge_gate.evaluate() reads them -- this is the same shape the gate
# gathers in CI, so the two see the same pull request.
GH_PAGER=cat gh api graphql -F owner="$owner" -F name="$name" -F pr="$pr" -f query='
query($owner:String!,$name:String!,$pr:Int!){
  repository(owner:$owner,name:$name){
    pullRequest(number:$pr){
      body
      author{login}
      reviews(last:50){ nodes{ state submittedAt commit{oid} author{login}
                               body comments(first:1){ totalCount } } }
      reviewThreads(first:100){
        nodes{ id isResolved isOutdated path line
               comments(first:1){ nodes{ author{login} body } } }
      }
    }
  }
}' >"$payload" 2>/dev/null || { echo "could not read the PR's reviews" >&2; exit 2; }

# `mergeStateStatus` is the only thing that can see a stale check run branch
# protection is still counting -- the wedge in #84, which is invisible from the
# rollup alone because the newest run of every name is green.
GH_PAGER=cat gh pr view "$pr" \
  --json headRefOid,statusCheckRollup,mergeStateStatus,autoMergeRequest \
  >"$checks" 2>/dev/null || exit 2

# Paginated, because `gh pr view --json files` stops at 100 and the gate's own
# Gather step paginates. A PR whose only enforcement-layer path sorts past the
# 100th file would otherwise be told to fix a merge-gate failure that is the gate
# working as designed.
GH_PAGER=cat gh api --paginate "repos/$owner/$name/pulls/$pr/files" \
  --jq '.[].filename' >"$files" 2>/dev/null || exit 2

python3 - "$pr" "$payload" "$checks" "$files" <<'PY'
import json, sys

sys.path.insert(0, ".github/scripts")
try:
    from merge_gate import HUMAN_ONLY_PREFIXES, evaluate
except Exception as exc:  # missing, half-edited, or broken at import time
    # Deliberately not ImportError alone. merge_gate.py is a file agents in this
    # repo edit, and a SyntaxError in it would otherwise reach the caller as an
    # exit 1 with a traceback -- "not ready", which sends an agent back to
    # await-review.sh for feedback that cannot exist. This is exit 2, "could not
    # tell", which is what it is.
    print(f"could not load .github/scripts/merge_gate.py ({exc}), which is the "
          "rule this PR will actually be judged by. Nothing here can answer "
          "without it.", file=sys.stderr)
    raise SystemExit(2)

pr, payload_path, checks_path, files_path = sys.argv[1:5]
pull = json.load(open(payload_path))["data"]["repository"]["pullRequest"]
view = json.load(open(checks_path))
head = view.get("headRefOid") or ""
checks = view.get("statusCheckRollup") or []
merge_state = (view.get("mergeStateStatus") or "UNKNOWN").upper()
queued = view.get("autoMergeRequest") is not None
with open(files_path) as fh:
    files = [line.strip() for line in fh if line.strip()]

problems = []

# The enforcement layer, judged before anything else: merge-gate refuses these
# paths on purpose, so its FAILURE on such a PR is the gate working, not a defect
# to chase. Reported as a plain failed check it costs an agent its three review
# rounds trying to turn green a gate that never will (#96).
protected = sorted(f for f in files if f.startswith(HUMAN_ONLY_PREFIXES))

# Everything about reviews comes from the gate itself, on the same pull request
# minus the two things answered better here: threads (listed in full below) and
# the protected paths (their own verdict, not a problem to fix).
gate_view = dict(pull, reviewThreads={"nodes": []})
ok, lines = evaluate(head, gate_view, [f for f in files if f not in protected])
if not ok:
    # evaluate() returns a heading plus two-space-indented problems; this prints
    # its own heading, and un-indents defensively rather than by slicing blind --
    # the gate is a separate file, and a change to how it formats must not be
    # able to swallow a character of the reason it gives.
    problems.extend(line[2:] if line.startswith("  ") else line
                    for line in lines[1:])

unresolved = [t for t in pull["reviewThreads"]["nodes"] if not t["isResolved"]]
if unresolved:
    # The whole thread, not just where it is. An agent that has to go and fetch
    # each body separately reaches for
    # `gh api repos/{owner}/{repo}/pulls/<n>/comments`, which cannot report
    # isResolved and hands back every comment ever left -- the already-fixed
    # ones mixed in with the live one. #88 and #89 each sat blocked on exactly
    # one unresolved thread lost in that noise. The id is here because it is
    # what `resolveReviewThread` takes, so nothing has to be looked up twice.
    problems.append(f"{len(unresolved)} unresolved review thread(s):")
    for t in unresolved:
        first = ((t.get("comments") or {}).get("nodes") or [{}])[0]
        who = ((first.get("author") or {}).get("login")) or "?"
        stale = "  (outdated -- still open)" if t.get("isOutdated") else ""
        problems.append(f"    {t.get('path')}:{t.get('line')}  by {who}{stale}")
        problems.append(f"      thread: {t.get('id')}")
        for line in (first.get("body") or "").splitlines():
            problems.append(f"      {line}")


def when(check):
    """When GitHub would consider this run the newest of its name.

    By start, never by completion. The gate job is `cancel-in-progress`, so the
    ordinary sequence on any PR is: a run starts, the review lands, a second run
    starts and cancels the first -- which means the CANCELLED run finishes
    seconds AFTER the live one began. Ordered by completion, that cancelled run
    is the newest of its name, and this reports "check failed: merge-gate" while
    the real gate is still running: the misreport this script exists to remove,
    arriving through the clock instead of through the rollup.
    """
    return (check.get("startedAt") or check.get("createdAt")
            or check.get("completedAt") or "")


# Newest run per check NAME, which is how branch protection resolves a required
# check and how `gh pr checks` reports one. The index breaks ties in list order,
# so two runs stamped the same second still resolve to one answer rather than to
# whichever dict happened to come first.
newest = {}
superseded = []
for i, c in enumerate(checks):
    key = c.get("name") or c.get("context") or "?"
    if key not in newest or (when(c), i) >= (when(newest[key]), newest[key]["_i"]):
        if key in newest:
            superseded.append(newest[key])
        newest[key] = dict(c, _i=i)
    else:
        superseded.append(c)

DEAD = ("FAILURE", "TIMED_OUT", "CANCELLED", "ACTION_REQUIRED", "ERROR")
# A check that has not finished is not a failure, but it is not ready either.
bad = [c for c in newest.values() if (c.get("conclusion") or c.get("state")) in DEAD]
# `status` is a CheckRun field; a legacy commit status carries `state` instead,
# so PENDING has to be looked for in both or it counts as green.
pending = [c for c in newest.values()
           if c.get("status") in ("IN_PROGRESS", "QUEUED", "PENDING")
           or (c.get("state") or "").upper() in ("PENDING", "EXPECTED")
           or not (c.get("conclusion") or c.get("state"))]
def is_the_expected_gate_failure(check):
    """A merge-gate on a protected-path PR, whichever state it is in.

    Answered below as the human-merge verdict rather than here as a problem --
    and that has to include the run still IN_PROGRESS, not only the concluded
    one. A gate that has not landed yet on such a PR is a gate that is going to
    fail, so reporting "check still running" makes exit 4 unreachable on the
    first ask and sends the agent round again for an answer that will not change.
    """
    return bool(protected) and (check.get("name") or check.get("context")) == "merge-gate"


for c in bad:
    if is_the_expected_gate_failure(c):
        continue
    problems.append(f"check failed: {c.get('name') or c.get('context')}")
for c in pending:
    if is_the_expected_gate_failure(c):
        continue
    problems.append(f"check still running: {c.get('name') or c.get('context')}")

if problems:
    print(f"PR #{pr} is NOT ready:")
    for p in problems:
        print("  " + p)
    raise SystemExit(1)

# Before the human-merge verdict, because exit 4 tells the agent to stop and a
# conflict blocks EVERY merge, a person's included. Announcing "nothing here is
# left to fix" on a conflicted branch parks the PR with the one thing that had to
# be said unsaid.
if merge_state in ("DIRTY", "BEHIND"):
    print(f"PR #{pr} is not ready, and GitHub says {merge_state}:")
    if merge_state == "DIRTY":
        print("  the branch conflicts with its base. Rebase, re-run "
              "record-review.sh for the")
        print("  new head -- the marker is per-commit -- and push with "
              "--force-with-lease.")
    else:
        print("  the branch is behind its base and the base requires being up to "
              "date. Update it.")
    print("  Do NOT wait for another review; no review can fix this.")
    raise SystemExit(1)

if protected:
    print(f"PR #{pr} is ready, and a human merges it.")
    print("  It touches the enforcement layer, which never merges itself:")
    for f in protected:
        print(f"    {f}")
    print("  merge-gate fails this PR by design -- a change that can rewrite the "
          "rules judging")
    print("  PRs is not merged by the machinery those rules govern. Nothing here "
          "is left to fix.")
    if ".github/scripts/merge_gate.py" in protected:
        print("  Note that CI judged this PR by the BASE branch's copy of that "
              "script and this")
        print("  read the one in the worktree, so the two answered by different "
              "rules.")
    print("  Set the worktree comment to \"ready, needs a human merge -- touches "
          f"{protected[0]}\" and stop.")
    raise SystemExit(4)

# Everything visible from here says yes, so if GitHub still says no, the reason
# is one this script cannot see from the rollup: a stale run of a name whose
# newest run is green, still counted by branch protection. That is #84, and it
# is silent -- the PR sits at BLOCKED with auto-merge queued and never fires.
if merge_state == "BLOCKED":
    print(f"PR #{pr} looks ready here, and GitHub says BLOCKED:")
    # Newest first, so the run most likely to be the one still counted is the
    # one read first. Which of these branch protection actually requires is not
    # knowable without admin on the branch rules, so they are all offered rather
    # than one of them asserted -- re-running a job that was not the problem
    # costs a minute; naming the wrong one as the answer costs a review round.
    left = sorted((c for c in superseded
                   if (c.get("conclusion") or c.get("state")) in DEAD),
                  key=when, reverse=True)
    if left:
        print("  a stale run whose own newer run passed is still on this commit, "
              "and if it is")
        print("  a required check then branch protection is still counting it:")
        for c in left:
            print(f"    {c.get('name') or c.get('context')}  "
                  f"{c.get('conclusion') or c.get('state')}  "
                  f"{c.get('detailsUrl') or ''}")
        print("  Re-running one updates its check in place, which is what clears "
              "it:")
        print("    gh run rerun --job <the job id at the end of that URL>")
        print("  then run this again. Do NOT wait for another review -- there is "
              "nothing to review.")
    else:
        print("  and nothing here can see why. Look at the PR's checks in the "
              "browser; a required")
        print("  check may never have run on this head. Do NOT wait for another "
              "review.")
    raise SystemExit(1)

print(f"PR #{pr} is ready: every thread resolved, every check green, no standing "
      "objection.")
if merge_state == "UNKNOWN":
    # GitHub computes mergeability lazily and answers UNKNOWN until it has. That
    # is usually transient and usually harmless -- but it is also the one state
    # in which the BLOCKED check above cannot run, so say so rather than let a
    # wedge pass as a clean bill of health.
    print("GitHub had not computed this PR's mergeability yet, so a stale check "
          "run could not be")
    print("ruled out. If it has not merged in a few minutes, run this again.")
if queued:
    print("Auto-merge is queued; GitHub merges it when the last required check "
          "passes. Stop here.")
else:
    print(f"Queue the merge and stop:  gh pr merge {pr} --auto --squash")
    print("That does not merge -- it asks GitHub to, once merge-gate passes.")
PY
