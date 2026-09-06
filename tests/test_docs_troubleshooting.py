#!/usr/bin/env python3
"""docs/TROUBLESHOOTING.md, pinned to the log the code actually writes.

A troubleshooting guide rots in one particular way: it keeps telling people to
look for a line the code stopped writing. Hard rule 1 means nobody can check
this page by following it -- M8-2 is where a person does that on a console -- so
what it *can* be held to is that every line it quotes is one this build can
produce, and that every line this build can produce is answered somewhere on the
page.

That is possible because the log has a closed set of **event tags**. They are an
enum with `kAllEvents` as its extent (core/include/rommsync/log.hpp) and their
spellings are one switch in core/src/log.cpp, so both directions can be read off
the code rather than restated here. It is the arrangement `auth.scopes` uses to
pin the scope block in docs/API_CONTRACT.md to `MinimumScopes()`.

Five phases, one CTest entry each so a red run names what broke:

  events    the guide's event table is exactly `log::kAllEvents`, and every tag
            appears in a quoted log line -- while every tag quoted in a log line
            is one the code writes.
  emitted   every tag has a call site outside the log module itself. A tag
            nobody writes is a section of this guide about nothing.
  links     every relative link resolves to a file in the tree, and every anchor
            to a heading in it -- this page's own included.
  safety    no step weakens TLS, and the bug-report section still tells the user
            not to attach the files that hold credentials.
  structure the sections #38 lists, in order; the hardware note naming M8-1 and
            M8-2; the log's path and cap as log.hpp states them; and a link to
            the guide from README.md and from docs/INSTALL.md.
"""
import argparse
import os
import re
import sys

# The sections #38's Scope asks for, in the order a user meets them: read the
# log, then the failure modes, then what to send.
SECTIONS = [
    "Reading the log",
    "Nothing syncs, and nothing is reachable",
    "The token expired or was revoked",
    "The server URL is wrong or missing",
    "TLS fails",
    "Saves are skipped: nothing matches",
    "The folder map switched saves off",
    "The SD card is full",
    "`config.ini` complaints",
    "The server refused the sync",
    "What to attach to a bug report",
    "What this page cannot tell you yet",
]

# The files that define and implement the log. A tag mentioned only in these is
# a tag nothing emits, which is what `emitted` is for.
LOG_SOURCES = ("core/include/rommsync/log.hpp", "core/src/log.cpp")

# Where a call site may live. `core/` deliberately holds none today -- it hands
# its log-safe strings up and the sysmodule writes them (log.hpp) -- but it is
# allowed to, so both trees are swept.
CALL_SITE_DIRS = ("core/src", "sysmodule/source", "overlay/source", "host/src")

SHORTCUT_PATTERNS = (
    r"(?:disabl\w*|turn\w*\s+off|skip\w*|ignor\w*|bypass\w*)[^.]{0,40}"
    r"\b(?:certificate|cert|tls|ssl|verification)\b",
    r"\b(?:certificate|cert|tls|ssl)\s+verification[^.]{0,20}\boff\b",
)
NEGATIONS = (" no ", " not ", " never ", " without ", " cannot ", " nothing ", " none ")
CLAUSE_BREAK = re.compile(r"[,;:]|\b(?:but|then|and|or|so|if|when|unless)\b")

# Never acceptable, negated or otherwise -- the same three docs.install_safety
# refuses, and for the same reason: a user copies a line out of a code block
# without reading the paragraph above it.
FORBIDDEN_LITERALS = ("verify_peer = false", "SslOptionType_SkipDefaultVerify",
                      "SetVerifyOption(0)")

FENCE = re.compile(r"^```[^\n]*\n(.*?)^```", re.MULTILINE | re.DOTALL)

# `3 warn net.offline GET /api/roms: ...` -- the line shape log.cpp renders.
LOG_LINE = re.compile(r"^(\d+) (error|warn|info) (\S+)(?: (.*))?$")


def fail(msg: str) -> int:
    print(f"FAIL: {msg}", file=sys.stderr)
    return 1


def read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def guide_path(repo: str) -> str:
    return os.path.join(repo, "docs", "TROUBLESHOOTING.md")


def events(repo: str) -> dict:
    """`{tag: enumerator}` for every event, read off `ToString(Event)`.

    The switch is the single spelling of the tags (log.cpp), and `kAllEvents` in
    the header is the extent -- so both are parsed and cross-checked here rather
    than one being trusted. A tag in the switch that is not in `kAllEvents` is
    an event the guide would never be asked about; the reverse is an event with
    no name.
    """
    source = read(os.path.join(repo, "core", "src", "log.cpp"))
    body = re.search(r"const char\* ToString\(Event event\) \{(.*?)\n\}", source, re.DOTALL)
    if not body:
        raise SystemExit(fail("cannot find `ToString(Event)` in core/src/log.cpp"))
    found = dict(re.findall(r"case Event::(k\w+):\s*\n\s*return \"([^\"]+)\";", body.group(1)))
    if not found:
        raise SystemExit(fail("`ToString(Event)` parsed as empty"))

    header = read(os.path.join(repo, "core", "include", "rommsync", "log.hpp"))
    extent = re.search(r"inline constexpr std::array kAllEvents = \{(.*?)\};", header, re.DOTALL)
    if not extent:
        raise SystemExit(fail("cannot find `kAllEvents` in core/include/rommsync/log.hpp"))
    listed = set(re.findall(r"Event::(k\w+)", extent.group(1)))

    named = set(found)
    if listed != named:
        raise SystemExit(fail(
            "`kAllEvents` and `ToString(Event)` disagree about which events exist: "
            f"only in kAllEvents {sorted(listed - named)}, "
            f"only in ToString {sorted(named - listed)}"))
    return {tag: name for name, tag in found.items()}


# The heading the event table lives under. Scoped rather than swept, because the
# guide has a second single-column-code table -- the three levels -- and a sweep
# over the whole page would read `info` as an event.
EVENT_TABLE = "### The events, and where each one is answered"


def table_tags(text: str) -> list:
    """The tags in the guide's event table: `| `net.offline` | ... |`."""
    if EVENT_TABLE not in text:
        raise SystemExit(fail(f"docs/TROUBLESHOOTING.md has no {EVENT_TABLE!r} heading; "
                              "that table is what this test reads the guide's events out of"))
    section = text[text.index(EVENT_TABLE) + len(EVENT_TABLE):]
    end = re.search(r"^#{1,3} ", section, re.MULTILINE)
    if end:
        section = section[: end.start()]
    return [m.group(1) for m in
            re.finditer(r"^\|\s*`([a-z][a-z_.]*)`\s*\|", section, re.MULTILINE)]


def quoted_lines(text: str) -> list:
    """Every line inside a fenced block that has the shape of a log line."""
    lines = []
    for block in FENCE.findall(text):
        for line in block.splitlines():
            match = LOG_LINE.match(line.strip())
            if match:
                lines.append(match)
    return lines


def slug(heading: str) -> str:
    """GitHub's heading anchor. docs/INSTALL.md's test explains the rule; this
    is the same function, deliberately -- two spellings of an anchor is how one
    of these two tests starts passing links the other would refuse."""
    s = heading.strip().lower()
    s = re.sub(r"[^\w\s-]", "", s)
    return re.sub(r"\s", "-", s)


def headings(text: str) -> set:
    body = FENCE.sub("", text)
    return {slug(m) for m in re.findall(r"^#{1,6}\s+(.*?)\s*$", body, re.MULTILINE)}


def sentences(text: str) -> list:
    flat = re.sub(r"\s+", " ", text)
    return [s for s in re.split(r"(?<=[.!?:])\s+|\s*\|\s*", flat) if s.strip()]


# --- events -------------------------------------------------------------------

def phase_events(args) -> int:
    repo = args.repo
    text = read(guide_path(repo))
    known = events(repo)

    documented = table_tags(text)
    if len(documented) != len(set(documented)):
        return fail("the guide's event table lists a tag twice")
    if set(documented) != set(known):
        return fail(
            "the guide's event table is not `log::kAllEvents`.\n"
            f"  undocumented: {sorted(set(known) - set(documented))}\n"
            f"  not an event: {sorted(set(documented) - set(known))}")

    # The acceptance criterion in #38's words: every log line quoted in the doc
    # is one the code can actually emit. Both directions, because a guide that
    # documented a tag in its table and quoted a different one in the section
    # below would pass a check that only ran one way.
    lines = quoted_lines(text)
    if not lines:
        return fail("the guide quotes no log lines at all; it is supposed to show "
                    "the line each failure produces, not describe it")
    for match in lines:
        if match.group(3) not in known:
            return fail(f"the guide quotes a log line whose event is {match.group(3)!r}:\n"
                        f"  {match.group(0)}\n"
                        f"this build writes {', '.join(sorted(known))}")

    shown = {match.group(3) for match in lines}
    missing = sorted(set(known) - shown)
    if missing:
        return fail(f"these events are in the table and never shown as a line: {missing}. "
                    "A user matches what they see in the file against this page, so every "
                    "tag needs at least one quoted example.")

    print(f"{len(known)} events, each in the table and each shown as a line; "
          f"{len(lines)} quoted lines, every one of them writable")
    return 0


# --- emitted ------------------------------------------------------------------

def phase_emitted(args) -> int:
    repo = args.repo
    known = events(repo)

    sources = []
    for directory in CALL_SITE_DIRS:
        root = os.path.join(repo, directory)
        for base, _, names in os.walk(root):
            # The vendored overlay library is not ours and never logs.
            if "lib" + os.sep in os.path.relpath(base, repo) + os.sep:
                continue
            for name in names:
                if name.endswith((".cpp", ".hpp")):
                    path = os.path.join(base, name)
                    if os.path.relpath(path, repo).replace(os.sep, "/") not in LOG_SOURCES:
                        sources.append(path)
    if not sources:
        return fail(f"no sources found under {CALL_SITE_DIRS}")
    # Comments stripped first. These files explain themselves at length and name
    # `log::Event::kNoServer` and friends in prose constantly, so a sweep over
    # the raw text would stay green after somebody deleted a call and left the
    # paragraph above it -- exactly the rot this phase exists to catch. `///`
    # doc comments are covered by the line rule.
    body = "\n".join(read(path) for path in sources)
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.DOTALL)
    body = re.sub(r"//[^\n]*", "", body)

    # A *call*, not a mention: one of the five entry points, an open paren, and
    # the event within the same statement. The level argument in between may be a
    # literal or a call (`LevelFor(note.severity)`), so it is matched loosely and
    # bounded by the statement's own semicolon.
    orphans = sorted(
        tag for tag, name in known.items()
        if not re.search(rf"\b(?:Error|Warn|Info|Write|WriteEach)\s*\([^;]{{0,80}}?"
                         rf"Event::{name}\b", body))
    if orphans:
        return fail(f"docs/TROUBLESHOOTING.md documents {orphans}, and nothing outside the "
                    "log module ever *writes* them -- a mention in a comment does not count. "
                    "A section about a line the code cannot produce is worse than no section.")

    print(f"every one of {len(known)} events has a call site across {len(sources)} sources")
    return 0


# --- links --------------------------------------------------------------------

def phase_links(args) -> int:
    repo = args.repo
    guide = guide_path(repo)
    text = read(guide)
    checked = 0

    for target in re.findall(r"\]\(([^)\s]+)\)", text):
        if target.startswith(("http://", "https://", "mailto:")):
            continue
        path, _, anchor = target.partition("#")
        if path:
            resolved = os.path.normpath(os.path.join(os.path.dirname(guide), path))
            if not os.path.exists(resolved):
                return fail(f"docs/TROUBLESHOOTING.md links {target!r}; there is no such file")
        else:
            resolved = guide
        if anchor and anchor not in headings(read(resolved)):
            return fail(f"docs/TROUBLESHOOTING.md links {target!r}; "
                        f"{os.path.relpath(resolved, repo)} has no such heading")
        checked += 1

    if checked == 0:
        return fail("docs/TROUBLESHOOTING.md has no relative links at all; it is supposed to "
                    "point at CONFIG.md, SYNC_PROTOCOL.md and SECURITY.md rather than "
                    "restate them")
    print(f"{checked} relative links resolve")
    return 0


# --- safety -------------------------------------------------------------------

def phase_safety(args) -> int:
    text = read(guide_path(args.repo))

    for literal in FORBIDDEN_LITERALS:
        if literal in text:
            return fail(f"docs/TROUBLESHOOTING.md contains {literal!r}. This is the page a "
                        "frustrated user reads, which is exactly where a way to skip "
                        "certificate verification must not be (docs/SECURITY.md).")

    for sentence in sentences(text):
        for pattern in SHORTCUT_PATTERNS:
            m = re.search(pattern, sentence, re.IGNORECASE)
            if not m:
                continue
            breaks = [b.end() for b in CLAUSE_BREAK.finditer(sentence.lower(), 0, m.start())]
            clause = f" {sentence.lower()[breaks[-1] if breaks else 0:m.start()]} "
            if any(neg in clause for neg in NEGATIONS):
                continue
            return fail(
                "docs/TROUBLESHOOTING.md appears to tell the user to weaken TLS:\n"
                f"  {sentence.strip()}\n"
                "There is no setting in this client that turns verification off, and this "
                "page may not suggest one (docs/SECURITY.md).")

    # The other half of the bug-report section's job. It tells a user to paste
    # the log *because* the log holds no credential -- so it also has to say
    # which files do.
    for name in ("token.dat", "device.dat"):
        if name not in text:
            return fail(f"the bug-report section never names {name}; it tells the user to "
                        "attach a log and has to say which files not to attach")
    if not re.search(r"do\s+\*\*not\*\*\s+attach|never attach|do not attach", text, re.I):
        return fail("the bug-report section never says not to attach the credential files")

    print("no step weakens TLS, and the bug-report section names what not to send")
    return 0


# --- structure ----------------------------------------------------------------

def constant(header: str, name: str) -> str:
    """The value of an `inline constexpr` in log.hpp, as written."""
    m = re.search(rf"inline constexpr [\w:<>*& ]*\b{name} = ([^;]+);", header)
    if not m:
        raise SystemExit(fail(f"cannot find `{name}` in core/include/rommsync/log.hpp"))
    return m.group(1).strip()


def phase_structure(args) -> int:
    repo = args.repo
    text = read(guide_path(repo))

    found = [h.strip() for h in re.findall(r"^##\s+(.*?)\s*$", text, re.MULTILINE)]
    if found != SECTIONS:
        return fail("docs/TROUBLESHOOTING.md's sections are not #38's, in order.\n"
                    "  expected: " + " / ".join(SECTIONS) + "\n"
                    "  found:    " + " / ".join(found))

    # The guide quotes whole log lines, and only the first three fields of one
    # are a contract -- `phase_events` pins those and deliberately pins nothing
    # after the tag (core/include/rommsync/log.hpp says why). A page that quoted
    # a detail without saying so would be promising more than any test here
    # keeps, so it has to say so.
    reading = text[text.index("## " + SECTIONS[0]): text.index("## " + SECTIONS[1])]
    if "first three fields" not in reading:
        return fail("the 'Reading the log' section does not say that only the ordinal, the "
                    "level and the event tag are promised. The detail after the tag is not "
                    "pinned by any test, and a guide that quotes one without saying so is "
                    "promising a wording that will move.")

    head = text[: text.index("## " + SECTIONS[0])]
    if not re.search(r"not yet validated on hardware", head, re.IGNORECASE):
        return fail("docs/TROUBLESHOOTING.md does not open with the "
                    "'not yet validated on hardware' note (hard rule 1)")
    for milestone in ("M8-1", "M8-2"):
        if milestone not in head:
            return fail(f"the hardware note does not name {milestone} -- one is the gate and "
                        "the other is what removes the note")

    # The file the guide sends a user to, and the ceiling it promises, read off
    # the header that decides them rather than typed here.
    header = read(os.path.join(repo, "core", "include", "rommsync", "log.hpp"))
    file_name = constant(header, "kLogFileName").strip('"')
    if file_name not in text:
        return fail(f"log.hpp writes {file_name!r} and the guide never names it")
    if f"{file_name}.old" not in text:
        return fail("the guide never names the rotated file, which is half of what a user "
                    "is asked to attach")
    cap = constant(header, "kMaxFileBytes")
    written = re.fullmatch(r"(\d+)\s*\*\s*1024", cap)
    if not written:
        return fail(f"log.hpp writes `kMaxFileBytes` as `{cap}`, which this test cannot read "
                    "as a number of KiB -- keep it as `<n> * 1024` or teach this phase the "
                    "new spelling")
    kib = int(written.group(1))
    if f"{kib} KiB" not in text:
        return fail(f"log.hpp caps the file at {kib} KiB; the guide does not say so, "
                    "and a user cannot tell how much card this costs")
    if f"{2 * kib} KiB" not in text:
        return fail(f"the guide does not say the pair costs {2 * kib} KiB -- there are two "
                    "files and the ceiling a user cares about is both of them")

    readme = read(os.path.join(repo, "README.md"))
    if "docs/TROUBLESHOOTING.md" not in readme:
        return fail("README.md does not link docs/TROUBLESHOOTING.md")

    # #38: the install guide's "When it does not work" section links this page by
    # relative path rather than by issue number. `docs.install_links` checks the
    # target resolves; this checks the paragraph was actually replaced.
    install = read(os.path.join(repo, "docs", "INSTALL.md"))
    if "TROUBLESHOOTING.md" not in install:
        return fail("docs/INSTALL.md still does not link the troubleshooting guide")
    tail = install[install.index("## When it does not work"):]
    if "issues/38" in tail:
        return fail("docs/INSTALL.md's 'When it does not work' still points at issue #38 "
                    "rather than at the page that now exists")

    print(f"{len(found)} sections in order, the hardware note names M8-1 and M8-2, "
          f"{file_name} capped at {kib} KiB, README and INSTALL both link it")
    return 0


PHASES = {
    "events": phase_events,
    "emitted": phase_emitted,
    "links": phase_links,
    "safety": phase_safety,
    "structure": phase_structure,
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("phase", choices=sorted(PHASES))
    ap.add_argument("--repo", required=True)
    args = ap.parse_args()
    return PHASES[args.phase](args)


if __name__ == "__main__":
    sys.exit(main())
