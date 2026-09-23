#!/usr/bin/env python3
"""Decide whether a pull request may merge itself.

`.github/workflows/merge-gate.yml` runs this as the one required check that
`gh pr merge --auto --squash` waits on. It asks three things and nothing else:

  1. the body says which issue it closes, in a form GitHub will act on;
  2. the change does not touch the enforcement layer, which never merges itself;
  3. a review on the CURRENT head said approve.

EVERYTHING ELSE IS BRANCH PROTECTION. CI green, every review thread resolved,
and an approval dismissed when the head moves are rules GitHub already has;
`install.sh` sets them through `gh api` and docs/CONFIGURATION.md says so. The
3,162-line script this replaced re-implemented all three in Python, on a payload
it had to page itself, and then added a fourth layer -- "has the author ANSWERED
the review" -- that could only be decided by reading prose the agent wrote. Two
validation passes existed to judge that prose. armaatus/autofleet#152 removed
the prose and with it the passes: the reviewer's verdict is a field in a JSON
schema, and this file reads a state, a path list and a regex.

WHY A MARKER IS ACCEPTED ALONGSIDE `APPROVED`. GitHub refuses `--approve` and
`--request-changes` on your own pull request, and in `local` review mode the
reviewer signs in as whoever `gh` is -- normally the account that opened the PR.
Every review this repository has ever received is therefore COMMENTED
(PRs #133, #135, #146), and a gate that only read the state could never pass one.
So `review.sh` writes its verdict as a marker naming the head it judged, and
falls back to a COMMENTED review carrying it when GitHub refuses the state. The
marker is written by the SCRIPT from a schema-validated field, never by the
model, and it names a sha, so it cannot be inherited by a later head.

    python3 .github/scripts/merge_gate.py <head-sha> <pr.json> <files.txt>

Exits 0 when the PR may merge, 1 when it may not, and prints why either way.
`--selftest` runs it against recorded shapes and needs no network.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from issue_refs import closes  # noqa: E402

# Paths that never merge themselves. `.claude/**` is the enforcement layer,
# `.github/workflows/**` can turn off the very checks gating this PR, and
# `.github/scripts/` holds THIS file -- a PR that changes what "may merge" means
# must not merge itself on the strength of its own new rules. enforce_admins is
# off, so an admin merges these by hand with this check red; nothing else can.
HUMAN_ONLY_PREFIXES = (".claude/", ".github/workflows/", ".github/scripts/")

# The same rule for the project-owned half, by exact name rather than by prefix.
# These three decide what the rules ARE. Beside them sit `setup.sh` and
# `teardown.sh`, ordinary project code an agent is meant to change -- taking the
# directory wholesale would put a person in the loop of routine work, and a rule
# that costs that gets removed rather than obeyed. armaatus/autofleet#38.
HUMAN_ONLY_FILES = (
    ".autofleet/guard.json", ".autofleet/config", ".autofleet/review.md",
)

# `review.sh` writes this, from the `verdict` field of the reviewer's JSON.
# The sha is part of the match on purpose: an approval is of ONE commit, and
# without it a fix pushed after the approval would inherit it.
VERDICT_RE = re.compile(r"<!--\s*autofleet-verdict:\s*approve\s+([0-9a-f]{7,40})\s*-->", re.I)


def human_only(changed_files):
    """The paths in this change that a person has to merge, sorted."""
    return sorted(f for f in changed_files
                  if f.startswith(HUMAN_ONLY_PREFIXES) or f in HUMAN_ONLY_FILES)


def approved(pull_request, head_sha):
    """Did a review of THIS commit say approve?

    Both halves are bound to the head: the state is read off the review's own
    `commit.oid`, and the marker names the sha it judged. A review of an earlier
    commit says nothing about this one, which is the whole reason a fix has to
    be re-reviewed.
    """
    for review in (pull_request.get("reviews") or {}).get("nodes") or []:
        review = review or {}
        on_head = ((review.get("commit") or {}).get("oid") or "") == head_sha
        if on_head and review.get("state") == "APPROVED":
            return True
        found = VERDICT_RE.search(review.get("body") or "")
        if found and head_sha.startswith(found.group(1)):
            return True
    return False


def evaluate(head_sha, pull_request, changed_files):
    """(may it merge, the lines saying why)."""
    ok, lines = True, []

    protected = human_only(changed_files)
    if protected:
        ok = False
        lines.append("Human merge required: this PR changes "
                     + ", ".join(protected) + ".")
        lines.append("  Those paths are the rules themselves. An admin merges "
                     "this by hand; nothing else can.")

    if closes(pull_request.get("body") or ""):
        lines.append("The body says which issue it closes.")
    else:
        ok = False
        lines.append("The body does not say which issue this closes. Add a "
                     "`Closes #N` line; GitHub only acts on that spelling.")

    if approved(pull_request, head_sha):
        lines.append("A review of %s said approve." % head_sha[:8])
    else:
        ok = False
        lines.append("No review of %s said approve. The reviewer decides that, "
                     "not the author; pushing a fix invalidates the last one."
                     % head_sha[:8])

    lines.append("MAY MERGE" if ok else "MAY NOT MERGE")
    return ok, lines


def _review(state="COMMENTED", oid="abc1234", body=""):
    return {"state": state, "commit": {"oid": oid}, "body": body}


def _pr(body="Closes #7", reviews=()):
    return {"body": body, "reviews": {"nodes": list(reviews)}}


OK = "<!-- autofleet-verdict: approve abc1234 -->"

# Eleven was the previous table's count of RULES; this is the count of rows.
# Every arm of every branch above has one, which is hard rule 3 -- a rule with
# no assertion is not shipped -- and the whole table fits on a screen, which the
# 1,600-line one it replaced never did.
SELFTEST = [
    ("an approved, closing, ordinary PR merges",
     "abc1234", _pr(reviews=[_review("APPROVED")]), ["scripts/fleet/lib.sh"], True),
    ("...and so does one whose verdict is the marker, which is all GitHub "
     "allows on your own PR",
     "abc1234", _pr(reviews=[_review(body="findings\n" + OK)]),
     ["scripts/fleet/lib.sh"], True),
    ("no review at all does not merge",
     "abc1234", _pr(), ["scripts/fleet/lib.sh"], False),
    ("an approval of an EARLIER head does not merge -- the fix it approved is "
     "not what is being merged",
     "def5678", _pr(reviews=[_review("APPROVED")]), ["scripts/fleet/lib.sh"], False),
    ("...and neither does a marker naming an earlier head",
     "def5678", _pr(reviews=[_review(body=OK)]), ["scripts/fleet/lib.sh"], False),
    ("a review that asked for changes is not an approval",
     "abc1234", _pr(reviews=[_review("CHANGES_REQUESTED")]),
     ["scripts/fleet/lib.sh"], False),
    ("a body with no closing line does not merge",
     "abc1234", _pr("fixes it", [_review("APPROVED")]),
     ["scripts/fleet/lib.sh"], False),
    ("the enforcement layer is never merged by the machinery it governs",
     "abc1234", _pr(reviews=[_review("APPROVED")]), [".claude/hooks/guard.py"], False),
    ("...nor are the three files that say what the rules are",
     "abc1234", _pr(reviews=[_review("APPROVED")]), [".autofleet/guard.json"], False),
    ("...but the project hooks beside them are ordinary code",
     "abc1234", _pr(reviews=[_review("APPROVED")]), [".autofleet/setup.sh"], True),
]


def selftest():
    failures = 0
    for what, head, pull_request, files, want in SELFTEST:
        got, lines = evaluate(head, pull_request, files)
        if got != want:
            print("FAIL: %s (expected %s, got %s)" % (what, want, got),
                  file=sys.stderr)
            print("\n".join("      " + line for line in lines), file=sys.stderr)
            failures += 1
        else:
            print("  ok: %s" % what)
    if failures:
        print("%d merge-gate assertion(s) failed" % failures, file=sys.stderr)
        return 1
    print("%d merge-gate assertions hold" % len(SELFTEST))
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    if len(sys.argv) != 4:
        print("usage: merge_gate.py <head-sha> <pr.json> <files.txt>  [--selftest]",
              file=sys.stderr)
        sys.exit(2)
    payload = json.load(open(sys.argv[2]))
    with open(sys.argv[3]) as fh:
        changed = [line.strip() for line in fh if line.strip()]
    passed, why = evaluate(sys.argv[1],
                           payload["data"]["repository"]["pullRequest"], changed)
    for line in why:
        print(line)
    sys.exit(0 if passed else 1)
