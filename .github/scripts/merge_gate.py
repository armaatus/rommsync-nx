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
  4. no review thread is still open;
  5. it does not touch the enforcement layer, which never merges itself.

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
import sys

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

    # A review by the PR's own author is not an independent review. GitHub
    # refuses a self-`--approve` but permits a self-`--comment`, and
    # `gh pr review` is on the agent's allowlist -- so without this the author
    # could satisfy the independence requirement by reviewing itself.
    pr_author = ((pull_request.get("author") or {}).get("login") or "").lower()
    reviews = [
        r for r in ((pull_request.get("reviews") or {}).get("nodes") or [])
        if ((r.get("author") or {}).get("login") or "").lower() != pr_author
    ]
    on_head = [r for r in reviews if ((r.get("commit") or {}).get("oid") == head_sha)]

    # A review RECORD is not a review. PR #95 merged on one whose entire body was
    # the word "test": the reviewer posted it at 02:45:34, this gate went green
    # 13 seconds later, and the real review -- 3812 characters, CHANGES_REQUESTED
    # -- arrived at 03:01, six minutes after the code was already on main.
    #
    # So a review has to carry something a person could act on: a body with
    # substance, or at least one inline comment. Empty COMMENTED records are
    # routine and harmless in themselves -- every thread reply creates one -- but
    # they must not be what satisfies the independence requirement.
    #
    # The bar is deliberately low. It is here to catch nothing-at-all, not to
    # judge quality, which no substring test can do.
    substantive = [
        r for r in on_head
        if len((r.get("body") or "").strip()) >= MIN_REVIEW_BODY
        or ((r.get("comments") or {}).get("totalCount") or 0) > 0
    ]
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
        latest = {}
        for r in sorted(on_head, key=lambda r: r.get("submittedAt") or ""):
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

    threads = (pull_request.get("reviewThreads") or {}).get("nodes") or []
    unresolved = [t for t in threads if not t.get("isResolved")]
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
            "body": "## Review findings\n/code-review high\nmattpocock-skills:code-review\n",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": []},
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
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        "the review is against an older commit",
        "def456",
        {
            "body": "/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        "changes requested, and nothing since",
        "abc123",
        {
            "body": "/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "CHANGES_REQUESTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": []},
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
            "body": "/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "CHANGES_REQUESTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
                {"state": "COMMENTED", "submittedAt": "2026-09-05T11:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": []},
        },
        ["core/src/sync.cpp"],
        True,
    ),
    (
        "an unresolved thread holds it",
        "abc123",
        {
            "body": "/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": [
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
            "body": "/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "armaatus"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        "...and a review from someone else does",
        "abc123",
        {
            "author": {"login": "armaatus"},
            "body": "/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": []},
        },
        ["core/src/sync.cpp"],
        True,
    ),
    (
        "the enforcement layer never merges itself",
        "abc123",
        {
            "body": "/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": []},
        },
        [".claude/hooks/guard.py"],
        False,
    ),
    (
        "...nor a change to this gate itself",
        "abc123",
        {
            "body": "/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": []},
        },
        [".github/scripts/merge_gate.py"],
        False,
    ),
    (
        "...and neither does a workflow change",
        "abc123",
        {
            "body": "/code-review\nmattpocock-skills:code-review",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-05T10:00:00Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"}, "body": "A real review body, long enough to be worth reading and to clear MIN_REVIEW_BODY."},
            ]},
            "reviewThreads": {"nodes": []},
        },
        [".github/workflows/ci.yml"],
        False,
    ),
    (
        "a review with nothing in it does not count",
        "abc123",
        {
            "body": "## Review findings\n/code-review high\nmattpocock-skills:code-review\n",
            "reviews": {"nodes": [
                # PR #95 merged on exactly this: body "test", no inline comments.
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:45:34Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "test"},
            ]},
            "reviewThreads": {"nodes": []},
        },
        ["core/src/sync.cpp"],
        False,
    ),
    (
        "...but an empty body with an inline comment does",
        "abc123",
        {
            "body": "## Review findings\n/code-review high\nmattpocock-skills:code-review\n",
            "reviews": {"nodes": [
                {"state": "COMMENTED", "submittedAt": "2026-09-06T02:45:34Z",
                 "commit": {"oid": "abc123"}, "author": {"login": "claude[bot]"},
                 "body": "", "comments": {"totalCount": 1}},
            ]},
            "reviewThreads": {"nodes": []},
        },
        ["core/src/sync.cpp"],
        True,
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
