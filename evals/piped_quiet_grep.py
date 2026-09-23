#!/usr/bin/env python3
"""Payload scripts that pipe an assertion into `grep -q`.

`-q` exits on the first match, the producer on the left of the pipe then writes
into a closed one, and `set -o pipefail` makes the pipeline 141 -- so a check
that HELD is reported as failed. A race, so green on small input and red on
large. `evals/lint.sh` runs this; CLAUDE.md carries the rule.

A separate file rather than a heredoc inside lint.sh: the pattern this looks for
is `| grep -q`, and a heredoc containing it would have to be written so as not
to match itself, which is how a detector ends up with a hole shaped like its own
source. Prints one line, the offending paths; empty means clean.
"""
import glob
import os
import re
import sys

# The SHELL of the payload. `.github/workflows/*.yml` is payload too and carries
# `run:` blocks with the same hazard -- claude-review.yml has a live one -- but a
# PR that edits a workflow cannot merge itself (merge_gate.py's human-only
# prefixes), so widening this glob here would red the lint on a file this change
# may not fix. It is armaatus/autofleet#90 instead, and CLAUDE.md says what is
# scanned rather than claiming the payload has none. Found by the independent
# review.
#
# `tests/*.sh` is here too, and is not payload: it is where the hazard has most
# recently SHIPPED -- test_docs.sh went in with an assertion whose silent
# direction made a new guard report PASS on the regression it was added to
# catch, and the scan could not say so because it did not look. A suite is
# assertions, which is the one thing this class turns into a lie, and
# `evals/run.sh` already shellchecks the same files. On a host the glob is the
# host's own suite, and a real hazard there is a real one. Found by the
# independent review of #165.
PATTERNS = (
    "scripts/fleet/*.sh",
    "scripts/fleet/runner/*.sh",
    "evals/*.sh",
    "tests/*.sh",
    ".github/scripts/*.sh",
    ".claude/hooks/*.sh",
    "install.sh",
)

# A single `|`, and `-q` in any bundle of short flags (`-q`, `-qE`, `-qxF`) or
# the long spelling. `(?<!\|)\|(?!\|)`: anchored on a bare `|`, this matched
# `cmd || grep -q pat file`, which is a fallback and not a pipe at all. Nothing
# in the tree hit it; found by the independent review before anything did.
HAZARD = re.compile(r"(?<!\|)\|(?!\|)\s*grep\b[^|]*?(?:\s-[A-Za-z]*q|\s--quiet)")
COMMENT = re.compile(r"\s*#")


def hazard_lines(text):
    """1-based line numbers whose (continuation-joined) statement has the bug.

    Continuations are joined first: a `|` ending one line with the grep at the
    start of the next is one pipeline, and a line-oriented scan walks past it.
    The joined line keeps the number of the line it STARTED on, which is the one
    a reader has to open.
    """
    hits, lineno = [], 0
    pending = ""
    start = 0
    for raw in text.splitlines():
        lineno += 1
        if not pending:
            start = lineno
        if raw.endswith("\\"):
            pending += raw[:-1] + " "
            continue
        line, pending = pending + raw, ""
        if COMMENT.match(line):
            continue
        if HAZARD.search(line):
            hits.append(start)
    return hits


def offenders(root="."):
    found = []
    for path in sorted({p for pat in PATTERNS for p in glob.glob(os.path.join(root, pat))}):
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        hits = hazard_lines(text)
        if hits:
            rel = os.path.relpath(path, root)
            # WITH the line numbers. Naming only the file leaves the reader to
            # find it, which on fleet.sh is 2,600 lines of haystack around a
            # comment that quotes the bad form on purpose.
            found.append("%s:%s" % (rel, ",".join(str(n) for n in hits)))
    return found


def selftest():
    """Every row is a shape this detector has already been blind to, or nearly.

    Two of them shipped: the sourced helper with no `pipefail` of its own
    (lib.sh, round 2 of the review) and the flag bundle `-Eq` (fleet.sh, found
    by the scan's own first run). `guard.py`, `merge_gate.py` and `issue_refs.py`
    all carry one of these and `evals/lint.sh` runs it; a detector that has
    shipped blind twice has the least claim to be the exception.
    """
    cases = [
        ("cat f | grep -q x", True, "the plain shape"),
        ("cat f | grep -qE x", True, "...and a flag bundle, which the first rewrite missed"),
        ("cat f | grep -Eq x", True, "...in either order"),
        ("cat f | grep --quiet x", True, "...and the long spelling"),
        ("cat f |\\\n  grep -q x", True, "a continuation is one pipeline"),
        ("cat f | grep x >/dev/null", False, "grep without -q reads its whole input"),
        ("cat f | qgrep x", False, "the safe helper is not the hazard"),
        ("cmd || grep -q x f", False, "a fallback is not a pipe"),
        ("  # cat f | grep -q x", False, "a comment quoting the shape is the record of it"),
        ("cat f | grep -q x | wc -l", True, "still a hazard mid-pipeline"),
    ]
    bad = 0
    for src, want, why in cases:
        got = bool(hazard_lines(src))
        if got != want:
            print("FAIL: %r -> %s, expected %s (%s)" % (src, got, want, why))
            bad += 1
    if bad:
        return 1
    print("  %d piped-grep -q assertions hold" % len(cases))
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    print("; ".join(offenders(sys.argv[1] if len(sys.argv) > 1 else ".")))
