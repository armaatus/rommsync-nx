#!/usr/bin/env python3
"""The two cross-reference conventions this repo runs on, spelled once.

Three parsers read the same two line-shapes out of issue and pull request
bodies, and they used to disagree:

  `Closes #N`     GitHub links and closes on any of NINE keywords, matched
                  case-insensitively with any run of whitespace before the `#`.
                  `fleet.sh` matched the single spelling `Closes #N`, so a PR
                  saying `Fixes #12` closed the issue and unblocked its
                  dependants while `has_open_pr` still reported #12 free -- and
                  the dispatcher opened a second worktree for work already in
                  flight, out of only three slots.

  `Blocked by #N` `.github/workflows/unblock.yml` matches
                  `/blocked\\s+by\\s+#(\\d+)/gi`; `fleet.sh` matched
                  `Blocked by #` exactly. A body written `blocked by #7` was a
                  blocker to the workflow and invisible to the fleet, so the
                  issue that frees the most work scored zero and lost the
                  ordering the queue exists to produce.

WHY IT LIVES IN `.github/scripts/` rather than `scripts/orca/`, which is where
its busiest reader is: `.github/workflows/merge-gate.yml` runs `merge_gate.py`
from a SPARSE CHECKOUT of `.github/scripts` alone, taken from the base branch.
A module anywhere else is simply not on disk when the gate runs. `fleet.sh` is
in a full worktree and can reach here; the gate cannot reach out.

`unblock.yml` is JavaScript inside YAML and cannot import this at all. What
keeps it in step is that it spells `BLOCKED_BY` identically -- asserted in
`evals/lint.sh`, which fails if either side is edited alone.

    python3 .github/scripts/issue_refs.py --selftest
"""

import re
import sys

# GitHub's closing keywords, verbatim from its documentation on linking a pull
# request to an issue. Nine, not one: an agent that writes `Fixes #12` gets
# exactly the same behaviour out of GitHub as one that writes `Closes #12`, so
# every reader of a body here has to agree with that.
CLOSING_KEYWORDS = (
    "close", "closes", "closed",
    "fix", "fixes", "fixed",
    "resolve", "resolves", "resolved",
)

# Longest first so the alternation reads in the obvious order. It would match
# either way -- Python backtracks into the shorter branches -- but a pattern
# whose correctness depends on backtracking is a pattern nobody edits safely.
_KEYWORDS = "|".join(sorted(CLOSING_KEYWORDS, key=len, reverse=True))

# `\b` at both ends: `Uncloses #12` is not a closing line, and `Closes #160` is
# not one for #16.
#
# Deliberately NOT accepting GitHub's `owner/repo#N` or full-URL forms. They
# close an issue in ANOTHER repository, and reading one as a local reference
# would tell the fleet that its own issue N is in flight when nothing is --
# "nothing startable" with the backlog wide open. CLAUDE.md asks for `Closes
# #N`, and that is the form every reader here agrees on.
CLOSES = re.compile(r"\b(?:" + _KEYWORDS + r")\s+#(\d+)\b", re.IGNORECASE)

# Character for character what unblock.yml matches, including the absence of a
# leading `\b`. Parity is the point: an issue this disagrees with the workflow
# about is one the fleet may start while the labels call it blocked.
BLOCKED_BY = re.compile(r"blocked\s+by\s+#(\d+)", re.IGNORECASE)


def closes(body):
    """Every issue number `body` says it closes, in order, as ints."""
    return [int(m.group(1)) for m in CLOSES.finditer(body or "")]


def closes_issue(body, number):
    """Does `body` carry a closing line for this one issue?"""
    return int(number) in closes(body)


def blocked_by(body):
    """Every issue number `body` names as a blocker, in order, as ints."""
    return [int(m.group(1)) for m in BLOCKED_BY.finditer(body or "")]


SELFTEST = [
    # Every keyword GitHub closes on, in the case an agent is likely to use.
    ("Closes #12", [12], []),
    ("closes #12", [12], []),
    ("CLOSED #12", [12], []),
    ("Close #12", [12], []),
    ("Fixes #12", [12], []),
    ("fix #12", [12], []),
    ("Fixed #12", [12], []),
    ("Resolves #12", [12], []),
    ("resolve #12", [12], []),
    ("RESOLVED #12", [12], []),
    # Any whitespace, because GitHub takes any whitespace.
    ("Closes  #12", [12], []),
    ("Closes\t#12", [12], []),
    ("Closes\n#12", [12], []),
    # ...but the keyword has to be the whole word, and the number the whole
    # number.
    ("Uncloses #12", [], []),
    ("Closes#12", [], []),
    ("Closes #12abc", [], []),
    ("this closes nothing and mentions #12 in passing", [], []),
    # Several, in the order written, is a PR that closes two issues.
    ("Closes #12\nFixes #13\n", [12, 13], []),
    # The blocker convention, in the spellings unblock.yml already accepts.
    ("Blocked by #7", [], [7]),
    ("blocked by #7", [], [7]),
    ("Blocked  By  #7", [], [7]),
    ("Blocked by\n#7", [], [7]),
    ("<!-- blockers -->\nBlocked by #7\nBlocked by #8\n", [], [7, 8]),
    ("nothing blocks this", [], []),
    # A real body, carrying both.
    ("## Plan\nCloses #115\n\n<!-- blockers -->\nBlocked by #40\n", [115], [40]),
    # Neither reader may fall over on an absent body: `gh` returns null for one.
    (None, [], []),
]


def selftest():
    failures = 0
    for body, want_closes, want_blocked in SELFTEST:
        shown = repr(body)
        got_closes, got_blocked = closes(body), blocked_by(body)
        if got_closes != want_closes or got_blocked != want_blocked:
            print(f"FAIL: {shown}: closes {got_closes} (want {want_closes}), "
                  f"blocked by {got_blocked} (want {want_blocked})",
                  file=sys.stderr)
            failures += 1
        else:
            print(f"  ok: {shown}")
    if not closes_issue("Fixes #12", 12) or closes_issue("Fixes #120", 12):
        print("FAIL: closes_issue does not agree with closes()", file=sys.stderr)
        failures += 1
    if failures:
        print(f"{failures} issue-reference assertion(s) failed", file=sys.stderr)
        return 1
    print(f"{len(SELFTEST)} issue-reference assertions hold")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    print(__doc__.strip(), file=sys.stderr)
    sys.exit(2)
