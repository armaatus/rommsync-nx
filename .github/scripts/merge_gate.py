#!/usr/bin/env python3
"""Decide whether a pull request may merge itself.

`.github/workflows/merge-gate.yml` runs this as the one required check that
`gh pr merge --auto` waits on. Everything the flow promises about a PR is
asserted here, in one deterministic place, rather than trusted to the agent that
produced it:

  1. it was reviewed LOCALLY before it was pushed -- both passes, findings in
     the body;
  2. an independent review exists on the CURRENT head, so pushing a fix
     invalidates it and a re-review is required;
  3. that review's latest word is not "changes requested";
  4. no review thread is still open -- and the thread list it read was
     complete, rather than the first page of one;
  5. it says which issue it closes, in a form GitHub will act on;
  6. a review that reports findings has been ANSWERED by the author, so the
     branch cannot merge out from under the fixes it asked for;
  7. it does not touch the enforcement layer, which never merges itself.

Point 6 exists because points 2 and 3 are not enough on their own. `gh pr merge
--auto` is armed the moment the PR is created -- deliberately, since that is
what stops a finished PR sitting green and unmerged (#90) -- and the independent
review only runs afterwards. `--request-changes` is caught by point 3, and an
inline finding is caught by point 4, but a review that returns nits as a
COMMENTED verdict in its BODY satisfies every one of them, so auto-merge fires
while the author is still editing. Four PRs went in that way -- #146, #154,
#159, #168 -- and the window is not the life of the PR but the minutes between
the review landing and the author's next push, which is exactly a full `ctest`
run. See `answered()` for what an answer is.

Point 3 reads the LATEST review per author rather than GitHub's
`reviewDecision`, and that is the whole trick. `reviewDecision` is sticky: once
a reviewer requests changes it stays CHANGES_REQUESTED until dismissed or until
that reviewer approves -- and this reviewer never approves, by design. A PR
whose findings were all addressed would sit blocked forever. Taking the latest
review instead lets a clean re-review supersede the old verdict on its own, with
no dismissal step and nothing waiting on a person.

    python3 .github/scripts/merge_gate.py <head-sha> <pr.json> <files.txt>

Exits 0 when the PR may merge, 1 when it may not, and prints why either way.
`--selftest` runs it against recorded shapes and needs no network.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from issue_refs import CLOSING_KEYWORDS, closes  # noqa: E402

# Paths that never merge themselves. `.claude/**` is the enforcement layer --
# the guards and the settings that register them -- and `.github/workflows/**`
# can turn off the very checks gating this PR. Both change rarely, and a person
# merging them is the point. enforce_admins is off, so an admin can merge these
# by hand with this check red; nothing else can.
# `.github/scripts/` is in here for the same reason as the other two: this file
# IS the gate, so a PR that changes what "may merge" means must not be able to
# merge itself on the strength of its own new rules.
# Shorter than any real review and longer than "test", "lgtm" or a stray
# newline. A number rather than a heuristic, because the alternative is judging
# review quality, which this script cannot do and should not pretend to.
MIN_REVIEW_BODY = 40

# The same bar for the author's answer, and lower: an answer is a disposition,
# not a review. "Reworded it" is a real answer; "ok" is the acknowledgement
# equivalent of PR #95's one-word review, and it leaves a human reading the PR
# with nothing to check the disposition against.
MIN_ANSWER_BODY = 20

# What the reviewer says it found, and what the author says about it. Both are
# HTML comments so neither shows up in the rendered text a person reads.
#
# A trailer rather than a verdict type, because the verdict type cannot carry
# this: REVIEW.md sends anything Important to `--request-changes` and everything
# else to `--comment`, so "a COMMENTED review" spans both "five nits" and
# "nothing at all" -- the two cases that have to be told apart here. Asking the
# reviewer to state a number is one instruction; inferring it from prose is a
# heuristic over third-party text.
REVIEW_FINDINGS_RE = re.compile(r"<!--\s*review-findings:\s*(\d+)\s*-->", re.I)
ANSWER_RE = re.compile(r"<!--\s*review-answered\s+([0-9a-f]{6,40})\s*-->", re.I)

HUMAN_ONLY_PREFIXES = (".claude/", ".github/workflows/", ".github/scripts/")

# What the PR body has to show. These are the two local passes CLAUDE.md
# requires before anything leaves a worktree. The `.orca/reviewed-<sha>` marker
# that gated the push is per-worktree and invisible from CI, so the body is what
# can actually be checked here.
LOCAL_PASSES = (
    ("/code-review", "a local /code-review pass"),
    ("mattpocock-skills:code-review",
     "a local /mattpocock-skills:code-review pass (standards, and spec-vs-diff)"),
)


def independent_reviews(pull_request, head_sha):
    """The reviews on `head_sha` that could be an independent review, oldest first.

    Two of the gate's conditions on WHO reviewed, in one place the fleet's own
    scripts can import. `scripts/orca/review-status.sh` and
    `scripts/orca/await-review.sh` both answer "has this PR been reviewed yet?",
    and both used to paraphrase this -- every paraphrase drifted the same way,
    more permissive than the gate, so the local answer said yes on a PR
    `merge-gate` then refused (#114).

    A review by the PR's own author is not an independent review. GitHub refuses
    a self-`--approve` but permits a self-`--comment`, and `gh pr review` is on
    the agent's allowlist -- so without this the author could satisfy the
    independence requirement by reviewing itself. Replying to a review THREAD
    creates one of these too, with an empty body and the replier as its author,
    which is how an agent answering findings manufactured its own review.

    The review also has to be on this head, because pushing a fix invalidates
    the review of the commit before it. For a caller waiting on a review that
    condition is strictly stronger than any freshness cut-off it could compute:
    a review cannot be submitted against a commit that does not exist yet.
    """
    pr_author = ((pull_request.get("author") or {}).get("login") or "").lower()
    return sorted(
        (r for r in ((pull_request.get("reviews") or {}).get("nodes") or [])
         if ((r.get("author") or {}).get("login") or "").lower() != pr_author
         and ((r.get("commit") or {}).get("oid") == head_sha)),
        key=lambda r: r.get("submittedAt") or "",
    )


def is_substantive(review):
    """Whether a review record carries anything a person could act on.

    A review RECORD is not a review. PR #95 merged on one whose entire body was
    the word "test": the reviewer posted it at 02:45:34, the gate went green 13
    seconds later, and the real review -- 3812 characters, CHANGES_REQUESTED --
    arrived at 03:01, six minutes after the code was already on main.

    So a review has to carry a body with substance, or at least one inline
    comment. Empty COMMENTED records are routine and harmless in themselves --
    every thread reply creates one -- but they must not be what satisfies the
    independence requirement, nor what a waiting agent is handed as "the review".

    The bar is deliberately low. It is here to catch nothing-at-all, not to judge
    quality, which no substring test can do.
    """
    return (len((review.get("body") or "").strip()) >= MIN_REVIEW_BODY
            or ((review.get("comments") or {}).get("totalCount") or 0) > 0)


def declared_findings(review):
    """How many findings the review says it left, or None if it did not say.

    `.github/workflows/claude-review.yml` instructs the reviewer to end every
    body with `<!-- review-findings: N -->` and REVIEW.md documents it, so this
    is a promise the flow makes rather than a guess about phrasing. A review
    without one has NOT said it is clean, and `answered()`'s caller reads it
    that way -- see the asymmetry there.
    """
    # The LAST one, not the first. REVIEW.md and the review prompt both put the
    # trailer at the END of the body, and a review of THIS repository quotes
    # fixtures full of the thing -- `merge_gate.py`'s own selftest carries five
    # of them. Reading the first match would let a quoted `0` stand in for a
    # real count of 3, which fails OPEN: straight back into the race this
    # condition exists to close. Found in review of this PR.
    matches = REVIEW_FINDINGS_RE.findall(review.get("body") or "")
    return int(matches[-1]) if matches else None


def answer_marker(head_sha):
    """The marker an answer carries, in the one place that also matches it.

    `scripts/orca/answer-review.sh` asks this script for it rather than spelling
    it out, so the writer and the reader cannot drift into two formats -- which
    would be silent in the direction that matters: answers that satisfy nothing.
    """
    return f"<!-- review-answered {head_sha} -->"


def answer_substance(body):
    """What is left of an answer once its marker is stripped.

    `MIN_ANSWER_BODY` measures THIS, and `scripts/orca/answer-review.sh` calls it
    rather than trimming its own way -- it used to count non-whitespace
    characters, which is a different number, so the writer could refuse an
    answer the reader would have taken. Found in review of this PR.
    """
    return ANSWER_RE.sub("", body or "").strip()


def answered(pull_request, head_sha, review):
    """Whether the PR's author has answered this review, on this head, since it.

    An answer is an issue comment carrying `<!-- review-answered <head-sha> -->`
    and something a person can read, written by the PR's author after the review
    was submitted. `scripts/orca/answer-review.sh` posts them; the marker is not
    meant to be typed by hand.

    Three conditions, each for its own failure:

    BY THE AUTHOR, because the review job holds `pull-requests: write` and can
    comment -- the same hole `independent_reviews()` closes at the other end,
    where a reply to a thread was manufacturing the review it was replying to.

    ON THIS HEAD, because an answer is invalidated by a push for the same reason
    the review is: round one's disposition is not round two's.

    AFTER THE REVIEW, because one head can legitimately collect two reviews --
    claude-review.yml fires on `review_requested` as well as on `synchronize` --
    and an answer written before the second one existed cannot be about it.
    Without this the sha alone would let round one's answer stand over findings
    that arrived later on the same commit.

    It does not, and cannot, check that the findings were FIXED. "Addressed
    them" and "said it will not" are the same answer here: what this asserts is
    that somebody read them and decided before the branch went in.
    """
    author = ((pull_request.get("author") or {}).get("login") or "").lower()
    if not author:
        # No author, no answer. Reachable only from a payload that did not ask
        # for one -- and treating "cannot tell who the author is" as "anyone may
        # answer" would make the marker satisfiable by the reviewer itself.
        return False
    since = review.get("submittedAt") or ""
    for comment in ((pull_request.get("comments") or {}).get("nodes") or []):
        if ((comment.get("author") or {}).get("login") or "").lower() != author:
            continue
        body = comment.get("body") or ""
        match = ANSWER_RE.search(body)
        # A PREFIX, because `scripts/orca/answer-review.sh` writes the full sha but
        # a person answering by hand writes the short one they were shown. Six
        # hex digits of a named PR's head is not a collision anybody can reach.
        if not match or not head_sha.lower().startswith(match.group(1).lower()):
            continue
        if (comment.get("createdAt") or "") <= since:
            continue
        if len(answer_substance(body)) < MIN_ANSWER_BODY:
            continue
        return True
    return False


def thread_list_is_complete(pull_request):
    """Whether this payload's review threads are ALL of the PR's review threads.

    A page is not the list. Both readers ask for `reviewThreads(first:100)`, so
    on a longer PR the newest threads -- the ones most likely to still be open --
    fall off the end, and every thread that did come back can be resolved while
    an open one sits on page two. `.github/scripts/pr_payload.sh` pages through
    them so this is normally true; it is false when the paging ran out.

    Here rather than in each caller for the same reason the query is:
    `resolve-thread.sh` also has to decide whether "no thread is left" is a
    thing it may say, and a second copy of that judgement is a second place for
    it to drift.
    """
    threads = pull_request.get("reviewThreads") or {}
    page = threads.get("pageInfo")
    # A MISSING pageInfo is "cannot tell", not "complete". Defaulting the other
    # way is fail-open on the one property this exists to make fail closed: a
    # caller that fetched the threads without asking whether there were more
    # would be told its half-list was whole. Nothing reaches this today --
    # pr_payload.sh always sets it -- and that is exactly when a default is
    # cheap to get right.
    if not page:
        return False
    return not page.get("hasNextPage")


def unresolved_threads(pull_request):
    """The open review threads. Only meaningful if the list is complete."""
    return [t for t in ((pull_request.get("reviewThreads") or {}).get("nodes") or [])
            if not t.get("isResolved")]


def evaluate(head_sha, pull_request, changed_files):
    """Returns (ok, [lines to print])."""
    problems = []
    body = pull_request.get("body") or ""

    for needle, what in LOCAL_PASSES:
        if needle not in body:
            problems.append(
                f"the PR body does not show {what}. Nothing leaves a fleet worktree "
                "unreviewed, so its findings belong in the body where a human can "
                "read them."
            )

    # A PR that merges without a closing line closes nothing, so unblock.yml
    # relabels nothing and the fleet reports "nothing startable" with the work
    # available. CLAUDE.md's finishing steps have always said the wording
    # matters; until now nothing checked it. Any of GitHub's nine keywords
    # counts, because any of them is what GitHub itself acts on.
    if not closes(body):
        problems.append(
            "the PR body names no issue it closes. Add `Closes #N` -- so "
            "merging it unblocks whatever was waiting on that issue. GitHub "
            "accepts any of " + ", ".join(CLOSING_KEYWORDS) + ", in any case."
        )

    # Who counts, and what counts as a review -- both from the functions above,
    # which is what `review-status.sh` and `await-review.sh` import so that the
    # three answers cannot drift apart again.
    on_head = independent_reviews(pull_request, head_sha)
    substantive = [r for r in on_head if is_substantive(r)]
    if on_head and not substantive:
        problems.append(
            f"the only reviews on {head_sha[:8]} are empty -- no body worth "
            "reading and no inline comment. A review record is not a review: "
            "PR #95 merged on one whose body was the word \"test\". Wait for the "
            "review job to actually submit its findings."
        )
    if not on_head:
        problems.append(
            f"no independent review has been submitted against the current head "
            f"({head_sha[:8]}). Pushing a fix invalidates the previous one -- "
            "re-request review. A review by this PR's own author does not count."
        )
    else:
        # From `substantive`, NOT from `on_head`. The same reviewer filing a real
        # CHANGES_REQUESTED and then, later on the same head, an empty COMMENTED
        # record -- which is exactly what a review job that runs and submits
        # nothing produces -- would otherwise make the empty one the review of
        # record, and `blocking` would come back empty with the findings never
        # addressed. That is PR #95's bug with the ordering reversed: an empty
        # record standing in for a review, arriving after instead of before.
        # Found in review of this PR.
        #
        # Oldest first, so the last write per author wins -- which is what
        # independent_reviews() already returns and `substantive` preserves,
        # being a filter over it. Sorting again here would be a second pass over
        # the same data for the same order.
        latest = {}
        for r in substantive:
            if r.get("state") in ("APPROVED", "CHANGES_REQUESTED", "COMMENTED"):
                latest[(r.get("author") or {}).get("login") or "?"] = r
        blocking = sorted(w for w, r in latest.items()
                          if r.get("state") == "CHANGES_REQUESTED")
        if blocking:
            problems.append(
                "the latest review from " + ", ".join(blocking) + " still requests "
                "changes. Address it and re-request review; a clean re-review "
                "supersedes it."
            )

        # ...and the same for a review that asked for nothing in particular but
        # still found something. A CHANGES_REQUESTED is caught above and an
        # inline finding is caught by the thread list; a nit in the BODY of a
        # COMMENTED review is caught by neither, and it merged four PRs out from
        # under their authors. See the module docstring.
        #
        # Only the reviews still standing -- one already superseded by a later
        # review on the same head has been answered by that review's existence,
        # and requiring an answer to it would deadlock a PR whose second review
        # was clean.
        for who, review in sorted(latest.items()):
            if review.get("state") == "CHANGES_REQUESTED":
                continue  # said above, with the remedy that belongs to it
            found = declared_findings(review)
            if found == 0:
                # #90's property: a review that reports nothing needs no answer,
                # so the PR still merges with nobody watching. This is the whole
                # reason the requirement is conditional rather than an
                # unconditional "the agent declares done", which would put a
                # finished PR back to waiting on an agent that may be gone.
                continue
            if answered(pull_request, head_sha, review):
                continue
            problems.append(
                (f"the review from {who} reports {found} finding(s)"
                 if found is not None else
                 f"the review from {who} does not say what it found -- no "
                 "`<!-- review-findings: N -->` trailer, so it is not read as "
                 "clean")
                + ", and this PR's author has not said what was done about "
                "them. Auto-merge is armed before the review runs, so without "
                "this the branch merges while the fixes are still being "
                "written. Address the findings, or say why you will not, and "
                "then:  ./scripts/orca/answer-review.sh \"<what you did>\""
            )

    if not thread_list_is_complete(pull_request):
        problems.append(
            "the review threads came back truncated, so whether any are still "
            "open cannot be answered from them. This is a refusal, not a "
            "failure: re-run this check, and if it persists the PR has more "
            "threads than the gather pages through."
        )
    unresolved = unresolved_threads(pull_request)
    if unresolved:
        problems.append(f"{len(unresolved)} review thread(s) are unresolved:")
        for t in unresolved[:10]:
            problems.append(f"    {t.get('path')}:{t.get('line')}")

    protected = sorted(
        f for f in changed_files if f.startswith(HUMAN_ONLY_PREFIXES)
    )
    if protected:
        problems.append(
            "this PR touches the enforcement layer, which never merges itself:"
        )
        for f in protected[:10]:
            problems.append(f"    {f}")
        problems.append(
            "    A repository admin merges it by hand -- enforce_admins is off, so "
            "that works with this check red."
        )

    if problems:
        return False, ["NOT READY TO MERGE:"] + ["  " + p for p in problems]
    return True, [
        "every gate holds: reviewed locally, reviewed independently on this head, "
        "no changes requested, no open threads."
    ]


SELFTEST = [
    (
        "a clean PR merges",
        "abc123",
        {
            "body": "Closes #7\n## Review findings\n/code-review high\nmattpocock-skills:code-review\n",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        True,
    ),
    (
        "no local review in the body",
        "abc123",
        {
            "body": "just some prose",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        "the review is against an older commit",
        "def456",
        {
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        "changes requested, and nothing since",
        "abc123",
        {
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "CHANGES_REQUESTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        # The wedge `reviewDecision` would create, and the reason for reading the
        # latest review per author instead.
        "a clean re-review supersedes an earlier changes-requested",
        "abc123",
        {
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "CHANGES_REQUESTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
                {"state": "COMMENTED", "submittedAt": "2026-09-05T11:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        True,
    ),
    (
        "an unresolved thread holds it",
        "abc123",
        {
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": [
                {"isResolved": False, "path": "core/src/sync.cpp", "line": 42},
            ]},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        "a self-review does not count as independent",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "armaatus"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        "...and a review from someone else does",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        True,
    ),
    (
        "the enforcement layer never merges itself",
        "abc123",
        {
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        [".claude/hooks/guard.py"],
        False,
    ),
    (
        "...nor a change to this gate itself",
        "abc123",
        {
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        [".github/scripts/merge_gate.py"],
        False,
    ),
    (
        "...and neither does a workflow change",
        "abc123",
        {
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        [".github/workflows/ci.yml"],
        False,
    ),
    (
        "a review with nothing in it does not count",
        "abc123",
        {
            "body": "Closes #7\n## Review findings\n/code-review high\nmattpocock-skills:code-review\n",
            "reviews": {"nodes": [
                # PR #95 merged on exactly this: body "test", no inline comments.
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:45:34Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "test"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        "...but an empty body with an inline comment does",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n## Review findings\n/code-review high\nmattpocock-skills:code-review\n",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:45:34Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "", "comments": {"totalCount": 1}},
            ]},
            # An empty body cannot carry a findings trailer, so this review is
            # answered instead -- which is the ordinary shape of one that put
            # everything inline.
            "comments": {"nodes": [
                {"author": {"login": "armaatus"},
                 "createdAt": "2026-09-06T03:00:00Z",
                 "body": "<!-- review-answered abc123 -->\n"
                         "Took the inline suggestion; the thread is resolved."},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        True,
    ),
    (
        "an empty review does not supersede a real CHANGES_REQUESTED",
        "abc123",
        {
            "body": "Closes #7\n## Review findings\n/code-review high\nmattpocock-skills:code-review\n",
            "reviews": {"nodes": [
                {"state": "CHANGES_REQUESTED", "submittedAt": "2026-09-06T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "A real review, long enough to clear MIN_REVIEW_BODY, "
                         "asking for changes that were never made."},
                # The review job re-ran and submitted nothing. Later, so it wins
                # on submittedAt -- and it must not.
                {"state": "COMMENTED", "submittedAt": "2026-09-06T10:30:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": ""},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        # The closing line is what unblock.yml acts on. Without it a PR merges,
        # closes nothing, and the fleet reports "nothing startable" while the
        # work it just finished sits waiting to free three more issues.
        "no closing line, so merging it would unblock nothing",
        "abc123",
        {
            "body": "## Review findings\n/code-review high\nmattpocock-skills:code-review\n",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        # Any of GitHub's nine, in any case: what the gate requires and what
        # GitHub acts on have to be the same set, or the gate blocks a PR that
        # would have worked.
        "...and any keyword GitHub closes on satisfies it",
        "abc123",
        {
            "body": "resolved #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        True,
    ),
    (
        # The gate asked for the FIRST hundred threads and there are more. Every
        # thread it did get back is resolved, so without this it prints "no open
        # threads" from a page it knows is partial -- a verdict about a state it
        # never established.
        "a truncated thread page is not an answer",
        "abc123",
        {
            "body": "Closes #1\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": True},
                              "nodes": [{"isResolved": True, "path": "a.cpp", "line": 1}]},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        "...and a complete one still is",
        "abc123",
        {
            "body": "Closes #1\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False},
                              "nodes": [{"isResolved": True, "path": "a.cpp", "line": 1}]},
        },
        ["core/src/sync.cpp"],
        True,
    ),
    (
        # A payload that never said whether there were more threads. Nothing in
        # this repo produces one -- pr_payload.sh always sets pageInfo -- which
        # is why the old default read it as "complete" for sixteen fixtures
        # without anyone noticing. Fail closed: not knowing is not the same as
        # knowing there are none.
        "a thread list that does not say whether it is complete is not an answer",
        "abc123",
        {
            "body": "Closes #1\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."
                 "\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        # THE WINDOW THIS CONDITION EXISTS TO CLOSE. Auto-merge is armed when
        # the PR is created -- deliberately, because that is what stops a
        # finished PR sitting green and unmerged (#90) -- and the independent
        # review only runs afterwards. A review that returns nits as a
        # COMMENTED verdict satisfied every other gate here, so the branch
        # merged while its author was still editing. Four times: #146, #154,
        # #159, #168, and #154's took a real defect into main with it.
        "a review that found something is not merged until the author answers it",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "Nit: the comment above sync_tick() says what, not why.\n"
                         "<!-- review-findings: 1 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        # "...or said it will not" is the same answer as "addressed them": both
        # are the author having read the findings and decided. The gate cannot
        # tell those apart and does not try -- what it requires is that somebody
        # answered before the branch went in.
        "...and merges once they have",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "Nit: the comment above sync_tick() says what, not why.\n"
                         "<!-- review-findings: 1 -->"},
            ]},
            "comments": {"nodes": [
                {"author": {"login": "armaatus"},
                 "createdAt": "2026-09-06T02:06:00Z",
                 "body": "<!-- review-answered abc123 -->\n"
                         "Reworded the comment to say why. Nothing else was actionable."},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        True,
    ),
    (
        # #90'S PROPERTY, KEPT. A review that says it found nothing needs no
        # answer, so the PR still merges with nobody watching -- which is the
        # whole reason auto-merge is armed early and the reason this condition
        # is conditional rather than an unconditional "the agent must declare
        # done".
        "a review that reports nothing still merges unattended",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "Correctness: nothing. Portability: nothing. Spec: matches "
                         "the plan.\n<!-- review-findings: 0 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        True,
    ),
    (
        # Fail CLOSED on a review that did not say. A reviewer that drops the
        # trailer is the ordinary way this degrades, and the cost of guessing
        # wrong is asymmetric: guessing "clean" re-opens the race the four PRs
        # above were lost to, guessing "found something" costs one command.
        "a review that does not say what it found is not assumed clean",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        # The answer is per-head for the same reason the review is: pushing a
        # fix invalidates both. Without the sha in it, the answer given to
        # round one would still be standing over round two's findings.
        "an answer to an earlier head does not answer this review",
        "def456",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T03:00:00Z",
                 "commit": {"oid": "def456"}, "author": {"login": "claude[bot]"},
                 "body": "Important: the retry has no backoff.\n"
                         "<!-- review-findings: 1 -->"},
            ]},
            "comments": {"nodes": [
                {"author": {"login": "armaatus"},
                 "createdAt": "2026-09-06T03:10:00Z",
                 "body": "<!-- review-answered abc123 -->\n"
                         "Answered round one's findings on the previous commit."},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        # Same head, and still not an answer: claude-review.yml fires on
        # `review_requested` as well as on `synchronize`, so one commit can
        # legitimately collect a second review. An answer written before that
        # review existed cannot be about it.
        "an answer written before the review does not answer it",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T04:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "Important: the retry has no backoff.\n"
                         "<!-- review-findings: 1 -->"},
            ]},
            "comments": {"nodes": [
                {"author": {"login": "armaatus"},
                 "createdAt": "2026-09-06T03:00:00Z",
                 "body": "<!-- review-answered abc123 -->\n"
                         "Answered the first review of this commit."},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        # The reviewer answering itself is the same hole `independent_reviews`
        # closes at the other end -- and it is reachable, because the review job
        # holds `pull-requests: write` and can comment.
        "an answer from anyone but the PR's author is not the author answering",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "Important: the retry has no backoff.\n"
                         "<!-- review-findings: 1 -->"},
            ]},
            "comments": {"nodes": [
                {"author": {"login": "claude[bot]"},
                 "createdAt": "2026-09-06T02:06:00Z",
                 "body": "<!-- review-answered abc123 -->\n"
                         "Marking my own findings as dealt with."},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        # A review of THIS repository quotes fixtures carrying the trailer --
        # every case in this list has one. Read from the front, a quoted `0`
        # stands in for the real count at the end, and the PR merges unanswered:
        # the race, reintroduced through the parser. Found in review of #170.
        "a trailer quoted inside a review body does not become its verdict",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "The new fixture says `<!-- review-findings: 0 -->`, which "
                         "is right for what it stands for.\nImportant: the retry has "
                         "no backoff.\n<!-- review-findings: 1 -->"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        # PR #95's lesson, one layer out: a record is not the thing. An answer
        # whose entire content is the marker says nothing a human reading the
        # PR could check the disposition against.
        "an answer that says nothing is not an answer",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "Closes #7\n/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "Important: the retry has no backoff.\n"
                         "<!-- review-findings: 1 -->"},
            ]},
            "comments": {"nodes": [
                {"author": {"login": "armaatus"},
                 "createdAt": "2026-09-06T02:06:00Z",
                 "body": "<!-- review-answered abc123 -->\nok"},
            ]},
            "reviewThreads": {"pageInfo": {"hasNextPage": False}, "nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
]


def selftest():
    failures = 0
    for what, head, pr, files, want in SELFTEST:
        got, _ = evaluate(head, pr, files)
        if got != want:
            print(f"FAIL: {what} (expected {want}, got {got})", file=sys.stderr)
            failures += 1
        else:
            print(f"  ok: {what}")
    if failures:
        print(f"{failures} merge-gate assertion(s) failed", file=sys.stderr)
        return 1
    print(f"{len(SELFTEST)} merge-gate assertions hold")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    if len(sys.argv) != 4:
        print("usage: merge_gate.py <head-sha> <pr.json> <files.txt>  [--selftest]",
              file=sys.stderr)
        sys.exit(2)
    head_sha = sys.argv[1]
    payload = json.load(open(sys.argv[2]))
    pull_request = payload["data"]["repository"]["pullRequest"]
    with open(sys.argv[3]) as fh:
        files = [line.strip() for line in fh if line.strip()]
    ok, lines = evaluate(head_sha, pull_request, files)
    for line in lines:
        print(line)
    sys.exit(0 if ok else 1)
