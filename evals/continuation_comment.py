#!/usr/bin/env python3
"""Payload scripts with a comment between a `\\` and the line it continues.

A backslash at end of line removes the newline, so

    gh api --paginate "$url" \\
    # `previous_filename` as well, because a rename reports only the new path
      --jq '.[] | .filename' >"$files" || exit 2

is not a command and a comment. It is ONE line -- `gh api --paginate "$url" #
...` -- in which the `#` comments out everything after it, including the `--jq`
the next line was meant to add. The line under the comment then runs as a
command of its own: `--jq` is not a command, rc 127, and the `|| exit 2` fires.

`bash -n` is happy with all of it, which is why this needs a scanner. It cost
`scripts/fleet/review-status.sh` its verdict: from armaatus/autofleet#121 until
armaatus/autofleet#69 it answered every invocation with 97 KB of unfiltered
JSON on stdout and exit 2, and exit 2 is "could not read" -- the step of the
brief where an agent finds out whether its threads are resolved.

A comment ABOVE the command says the same thing and survives. The fix is always
to move it there; there is no correct in-continuation comment, which is what
makes this a scan rather than a judgement.

`evals/lint.sh` runs this; CLAUDE.md carries the rule.

A separate file rather than a heredoc inside lint.sh, for the reason
`piped_quiet_grep.py` states: a detector whose source has to be written so as
not to match itself ends up with a hole shaped like its own source. Prints one
line, the offending paths; empty means clean.
"""
import glob
import os
import sys

# The same payload shell `piped_quiet_grep.py` and `late_stderr_silence.py`
# scan, and the same exclusion: `.github/workflows/*.yml` carries `run:` blocks
# that are payload too, but a PR editing a workflow cannot merge itself, so
# widening the glob here would red the lint on a file the change may not fix.
PATTERNS = (
    "scripts/fleet/*.sh",
    "scripts/fleet/runner/*.sh",
    "evals/*.sh",
    ".github/scripts/*.sh",
    ".claude/hooks/*.sh",
    "install.sh",
)


def hazard_lines(text):
    """1-based line numbers of continuations whose next line opens a comment.

    Two subtleties, and the rest is "does the next line start with `#`":

    A line ending in an EVEN number of backslashes does not continue -- the last
    two are an escaped backslash, and the newline stands.

    And the backslash has to be the LAST character. `cmd \ ` escapes the space,
    not the newline, so it is not a continuation and the comment under it is an
    ordinary comment. Only `\r` is stripped, for a file with CRLF endings;
    stripping whitespace here would report a line that is not a hazard, and a
    scan that cries wolf is one somebody deletes (hard rule 3).
    """
    lines = text.split("\n")
    hits = []
    for i, line in enumerate(lines[:-1]):
        stripped = line.rstrip("\r")
        if not stripped.endswith("\\"):
            continue
        trailing = len(stripped) - len(stripped.rstrip("\\"))
        if trailing % 2 == 0:
            continue
        if lines[i + 1].lstrip().startswith("#"):
            hits.append(i + 1)
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
            found.append("%s:%s" % (rel, ",".join(str(n) for n in hits)))
    return found


def selftest():
    """Every row is a shape this detector has to tell apart from the hazard."""
    cases = [
        ('gh api "$u" \\\n# a comment\n  --jq .x', True, "armaatus/autofleet#121 itself"),
        ('gh api "$u" \\\n    # indented, which is how it usually reads\n  --jq .x', True,
         "the indent is what makes it look like part of the block"),
        ('gh api "$u" \\\n  --jq .x\n# a comment after the statement', False,
         "a comment BELOW the finished command is fine"),
        ('# a comment above it\ngh api "$u" \\\n  --jq .x', False, "...and so is one above"),
        ('gh api "$u" \\\n\n# a comment after a blank line', False,
         "a blank line ends the continuation, so the comment is its own line"),
        ('printf \'a\\\\\\\\\' \\\n# still a continuation, odd count after the escaped pair',
         True, "an escaped backslash then a real one still continues"),
        ('printf \'x\\\\\\\\\'\n# an even count does not continue, so this is just a comment',
         False, "two backslashes are an escaped one; the newline stands"),
        # ...and that row ends in a QUOTE, so `endswith` short-circuits and the
        # even/odd count is never reached. These two are what assert it: delete
        # the `trailing % 2` guard and the first of them reports a hazard on a
        # line bash does not continue. Found by the local review, which deleted
        # the guard and watched all eleven other rows stay green (hard rule 3).
        ('echo foo\\\\\n# the line ends in an escaped backslash, not a continuation', False,
         "an even count at the very end of the line"),
        ('echo foo\\\\\\\n# ...and three is odd again, so this one does continue', True,
         "the guard counts; it does not merely look for a pair"),
        ('echo no continuation here\n# a comment', False, "nothing to swallow"),
        ('cmd \\ \n# the backslash escaped the space, so the newline stands', False,
         "a trailing space means this is not a continuation at all"),
        ('cmd \\\r\n# a CRLF file continues just the same', True,
         "...but a carriage return is line ending, not content"),
        ('cmd \\\n  --flag \\\n# swallowed on the SECOND continuation\n  --other', True,
         "the hazard is per continuation, not per statement"),
    ]
    bad = 0
    for src, want, why in cases:
        got = bool(hazard_lines(src))
        if got != want:
            print("FAIL: %r -> %s, expected %s (%s)" % (src, got, want, why))
            bad += 1
    if bad:
        return 1
    print("  %d continuation-comment assertions hold" % len(cases))
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    print("; ".join(offenders(sys.argv[1] if len(sys.argv) > 1 else ".")))
