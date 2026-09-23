#!/usr/bin/env python3
"""Payload scripts that silence stderr AFTER the redirection that fails.

Redirections are applied left to right. In

    read -r head n <"$marker.tries" 2>/dev/null || true

the open of a missing file fails while stderr is still the caller's, so the
shell prints `.../42.tries: No such file or directory` and only then points
stderr at /dev/null. The `|| true` hides the status, not the diagnostic. Written
the other way round -- `2>/dev/null <"$marker.tries"` -- the diagnostic goes
where it was meant to.

It reads as harmless and is not: armaatus/autofleet#87 is this line printing a
shell error into fleet.log on the first poll of every new PR head, and by the
time anybody scanned for it there were thirteen copies. A guard rather than
thirteen fixes, because the copies came from re-typing a working line -- and the
next one will too.

`evals/lint.sh` runs this; CLAUDE.md carries the rule.

A separate file rather than a heredoc inside lint.sh, for the reason
`piped_quiet_grep.py` states: a detector whose source has to be written so as
not to match itself ends up with a hole shaped like its own source. Prints one
line, the offending paths; empty means clean.
"""
import glob
import os
import re
import sys

# The same payload shell `piped_quiet_grep.py` scans, and the same exclusion:
# `.github/workflows/*.yml` carries `run:` blocks that are payload too, but a PR
# editing a workflow cannot merge itself, so widening the glob here would red
# the lint on a file the change may not fix.
PATTERNS = (
    "scripts/fleet/*.sh",
    "scripts/fleet/runner/*.sh",
    "evals/*.sh",
    ".github/scripts/*.sh",
    ".claude/hooks/*.sh",
    "install.sh",
)

# An input redirection whose open can FAIL. `<<` and `<<<` are excluded because
# a heredoc and a herestring are written by the shell into a temporary it just
# created -- there is no name to be missing -- and `< <(` because a process
# substitution opens a pipe the shell owns. What is left is `<word`, which is
# the only form that can name a file that is not there.
#
# The negative lookahead on `<` and `(` is what keeps the three apart; without
# it every `<<<"$out"` in the suite read as a hazard, which is 200-odd lines
# that were never at risk.
OPEN = re.compile(r"(?<![<\d])<(?![<(])\s*[^\s<>&|;)]")
# Silencing stderr: /dev/null, or closing the descriptor outright. `2>&1` is not
# silencing -- it merges, and the diagnostic still arrives somewhere a reader
# sees -- so it is not the hazard this is about.
QUIET = re.compile(r"2>\s*(?:/dev/null\b|&-)")
COMMENT = re.compile(r"\s*#")


def hazard_lines(text):
    """1-based line numbers whose statement silences stderr too late.

    Continuations are joined first, and the joined statement keeps the number of
    the line it STARTED on -- that is the line a reader has to open. Same shape
    as `piped_quiet_grep.hazard_lines`, and deliberately so: two scans of the
    same tree that disagree about what one statement is are two scans with
    different blind spots.
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
        # PER COMMAND, not per line. Comparing the first `<` with the first
        # `2>` on the whole joined statement reads `cmd 2>/dev/null; read -r h
        # n <"$f" 2>/dev/null` as clean -- the silencing it finds belongs to the
        # command before the hazard. Nothing in the tree is written that way
        # today; the premise of this whole rule is that the shape gets re-typed,
        # and a compound line is the obvious next spelling. Found by the
        # self-review.
        #
        # `;`, `&&`, `||` and `|` -- and NOT a bare `&`, which would cut
        # `2>&-` in half and lose the descriptor-closing spelling this scan
        # already had a row for. A backgrounding `&` at the end of a command is
        # a miss, not a false positive, and the selftest keeps `2>&-`.
        #
        # A `;` inside a quoted string splits a segment that then matches
        # nothing, which costs a miss in the same safe direction.
        for seg in re.split(r'(?:;|&&|\|\||\|)', line):
            opened = OPEN.search(seg)
            if not opened:
                continue
            quiet = QUIET.search(seg)
            # ORDER IS THE WHOLE QUESTION. A `2>/dev/null` before the open is
            # the correct spelling of exactly this line, so a scan that only
            # asked whether both appear would fail every fix it asked for.
            if quiet and quiet.start() > opened.start():
                hits.append(start)
                break
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
    """Every row is a shape this detector has to tell apart from the hazard.

    The herestring and the heredoc rows are the ones that matter: this tree has
    hundreds of `<<<"$out"` assertions and a scan that read them as hazards
    would have been deleted rather than fixed, which is how a guard stops
    guarding (hard rule 3).
    """
    cases = [
        ('read -r h n <"$m.tries" 2>/dev/null || true', True, "armaatus/autofleet#87 itself"),
        ('read -r h n < "$m.tries" 2>/dev/null', True, "...with a space after the <"),
        ('{ read -r a; read -r b; } <"$f" 2>/dev/null', True, "a group has one set of redirections"),
        ('read -r h n <"$m.tries" 2>&-', True, "closing the descriptor silences it just as late"),
        ('read -r h \\\n  n <"$m" 2>/dev/null', True, "a continuation is one statement"),
        ('read -r h n 2>/dev/null <"$m.tries"', False, "the fix: silenced before the open"),
        ('read -r h n <"$m.tries"', False, "unsilenced is a different bug, and not this one"),
        ('grep -c x <<<"$out" 2>/dev/null', False, "a herestring cannot fail to open"),
        ('cat <<EOF 2>/dev/null', False, "...nor a heredoc"),
        ('while read -r l; do :; done < <(gen) 2>/dev/null', False, "...nor a process substitution"),
        ('cmd >"$out" 2>/dev/null', False, "an output redirection is not this hazard"),
        ('cmd <"$f" 2>&1', False, "merging is not silencing -- the diagnostic still lands"),
        ('  # read -r h n <"$m" 2>/dev/null', False, "a comment quoting the shape is the record of it"),
        ('printf x >&2 2>/dev/null', False, "no input redirection at all"),
        ('cmd 2>/dev/null; read -r h n <"$m" 2>/dev/null', True,
         "a compound line: the silencing before the hazard is another command's"),
        ('cmd 2>/dev/null && read -r h n 2>/dev/null <"$m"', False,
         "...and the fix is still the fix on the second command"),
    ]
    bad = 0
    for src, want, why in cases:
        got = bool(hazard_lines(src))
        if got != want:
            print("FAIL: %r -> %s, expected %s (%s)" % (src, got, want, why))
            bad += 1
    if bad:
        return 1
    print("  %d late-stderr assertions hold" % len(cases))
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    print("; ".join(offenders(sys.argv[1] if len(sys.argv) > 1 else ".")))
