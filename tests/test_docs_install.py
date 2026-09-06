#!/usr/bin/env python3
"""docs/INSTALL.md, pinned to the archive the packaging script actually builds.

The install guide is the one page in this repo nobody can verify by following
it: hard rule 1 says no hardware until the M8-1 gate, so "does this procedure
work" is a question M8-2 answers and this file cannot. What it can do is stop
the guide from telling a user to look in a folder that no longer exists.

The three ways this page rots are a renamed artifact, a moved title id, and a
screen that changed. The first two are the same fact -- both are entries in
`scripts/package.sh --list`, which prints the archive's paths and builds
nothing, so the check is cheap enough to run in a docs test and needs no
cross-compiler. The third is `PairingState` in core/include/rommsync/pairing.hpp.
Nothing here is typed from memory; every path, the title id and every state name
is read off the thing that produces it.

Five phases, one CTest entry each so a red run names what broke:

  paths     every SD path the guide names is one the archive ships, one of the
            directories it ships into, or a runtime file that lives beside one
            -- and every entry the archive ships is named by the guide.
  links     every relative link resolves to a file in the tree, and every
            anchor to a heading in it.
  safety    no step tells the user to type an account secret on the console or
            to turn certificate verification off. Those are the two ways a
            written-down shortcut undoes M1 and SECURITY.md respectively, and
            both are one sentence away at all times.
  pairing   the pairing section's state table is exactly the states
            `PairingState` exposes, with three DISTINCT sentences for the three
            terminal failures -- pairing.hpp's own requirement.
  structure the ten sections #35 specifies, in order; the hardware note with
            M8-2 named as what removes it; and a link to the guide from
            README.md.
"""
import argparse
import json
import os
import re
import subprocess
import sys

# The prefixes an install path starts with. A guide sentence about
# `/tico/roms/snes` is naming a folder the *user* mapped (docs/CONFIG.md), not
# one the archive ships, so the sweep is anchored to the archive's own top
# level rather than to "anything with a slash in it".
SD_PREFIXES = ("atmosphere/", "switch/", "config/")

# The one directory a shipped path may grow that the archive does not contain:
# `flags/`, which ovl-sysmodules creates to write boot2.flag into. The archive
# deliberately holds no flags/ entry (#33), so this cannot be derived from the
# manifest -- but the title id directory ABOVE it still is, which is what keeps
# a moved title id red here too.
RUNTIME_SUBDIRS = ("flags",)

# Runtime state that lives beside the shipped config, and is named by the guide
# precisely because it is NOT in the archive: an upgrade must leave it alone.
RUNTIME_FILES = ("config.ini", "token.dat", "device.dat", "state.db", "queue.json")

# The ten sections of #35, in the order a user meets them.
SECTIONS = [
    "Before you start",
    "1. Unpack the zip",
    "2. Enable the sysmodule",
    "3. Point it at your server",
    "4. Pair the console",
    "5. The first sync",
    "6. Upgrading",
    "7. Uninstalling and un-pairing",
    "Security",
    "When it does not work",
]

# Naming one of these is talking about a shortcut. The guide is allowed to --
# it has to be able to say that no account secret is typed on the console and
# that the client has no setting to weaken TLS -- but only in the negative, and
# only where the negation governs the phrase itself. "If it will not connect,
# disable certificate verification" is an instruction with a negation somewhere
# else in it, which is why the window below starts at the enclosing clause and
# not at the sentence.
SHORTCUT_PATTERNS = (
    r"\bpasswords?\b",
    r"\bpassphrases?\b",
    r"\bcredentials\b",
    r"(?:disabl\w*|turn\w*\s+off|skip\w*|ignor\w*|bypass\w*)[^.]{0,40}"
    r"\b(?:certificate|cert|tls|ssl|verification)\b",
    r"\b(?:certificate|cert|tls|ssl)\s+verification[^.]{0,20}\boff\b",
)
NEGATIONS = (" no ", " not ", " never ", " without ", " cannot ", " nothing ", " none ")
# What ends the clause a negation has to be inside. A negation on the far side
# of one of these is governing a different statement.
CLAUSE_BREAK = re.compile(r"[,;:]|\b(?:but|then|and|or|so|if|when|unless)\b")

# Never acceptable, negated or otherwise: these are not sentences, they are the
# thing itself, and a user copies one out of a code block without reading the
# paragraph above it. Both are what M0-1 found the console's `ssl` service can
# be talked into (docs/SECURITY.md#a-self-signed-certificate-on-a-home-server);
# a curl flag is not, which is why none is listed.
FORBIDDEN_LITERALS = ("SSL_VERIFY_NONE", "verify_peer = false")


def fail(msg: str) -> int:
    print(f"FAIL: {msg}", file=sys.stderr)
    return 1


def read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def guide_path(repo: str) -> str:
    return os.path.join(repo, "docs", "INSTALL.md")


def manifest(repo: str) -> list:
    """The archive's entry paths, from the script that writes the archive."""
    script = os.path.join(repo, "scripts", "package.sh")
    out = subprocess.run(["bash", script, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(fail(f"scripts/package.sh --list failed: {out.stderr.strip()}"))
    entries = [line.strip() for line in out.stdout.splitlines() if line.strip()]
    if not entries:
        raise SystemExit(fail("scripts/package.sh --list printed nothing"))
    return entries


def title_id(repo: str) -> str:
    """Read from the NPDM config by a JSON reader, where package.sh uses a regex."""
    with open(os.path.join(repo, "sysmodule", "sys-rommsync.json"), encoding="utf-8") as f:
        tid = json.load(f)["title_id"]
    return (tid[2:] if tid[:2].lower() == "0x" else tid).upper()


# One expression for a fenced block, used to collect the fences and then to
# remove them: two copies drift, and the second copy is the one that silently
# starts letting a path through.
FENCE = re.compile(r"^```[^\n]*\n(.*?)^```", re.MULTILINE | re.DOTALL)


def code_spans(text: str) -> list:
    """Every inline code span, plus every line of every fenced block.

    The guide names paths in both, and a path that only ever appears as prose is
    a path this test cannot see -- which is why the guide writes them as code.
    """
    spans = []
    for block in FENCE.findall(text):
        spans.extend(block.splitlines())
    spans.extend(re.findall(r"`([^`\n]+)`", FENCE.sub("", text)))
    return spans


def named_paths(text: str) -> set:
    """Every SD install path the guide names, normalised without a trailing slash."""
    found = set()
    for span in code_spans(text):
        for token in re.findall(r"[A-Za-z0-9_.\-/]+", span):
            if token.startswith(SD_PREFIXES) or token in ("atmosphere", "switch", "config"):
                found.add(token.rstrip("/"))
    return found


def ancestors(entry: str) -> set:
    """Every directory an archive entry passes through, `atmosphere` included."""
    parts = entry.split("/")[:-1]
    return {"/".join(parts[: i + 1]) for i in range(len(parts))}


def slug(heading: str) -> str:
    """GitHub's heading anchor: lowercased, punctuation dropped, spaces hyphenated.

    One space becomes one hyphen and runs are NOT collapsed, which is why
    `## Re-pairing / revocation` is reached as `#re-pairing--revocation` -- the
    dropped `/` leaves the second hyphen behind.
    """
    s = heading.strip().lower()
    s = re.sub(r"[^\w\s-]", "", s)
    return re.sub(r"\s", "-", s)


def headings(text: str) -> set:
    """Every anchor a link may aim at. Fenced blocks are stripped first: a shell
    comment inside one is not a heading, and accepting it would let a link
    resolve here that resolves to nothing on GitHub."""
    body = FENCE.sub("", text)
    return {slug(m) for m in re.findall(r"^#{1,6}\s+(.*?)\s*$", body, re.MULTILINE)}


def sentences(text: str) -> list:
    """Rough sentence split. A table cell is its own sentence: a four-column row
    is four statements on one line, and joining them would let a negation in one
    cell excuse an instruction in the next."""
    flat = re.sub(r"\s+", " ", text)
    return [s for s in re.split(r"(?<=[.!?:])\s+|\s*\|\s*", flat) if s.strip()]


# --- paths --------------------------------------------------------------------

def phase_paths(args) -> int:
    repo = args.repo
    text = read(guide_path(repo))
    entries = manifest(repo)
    tid = title_id(repo)

    allowed_dirs = set()
    for entry in entries:
        allowed_dirs |= ancestors(entry)

    for path in sorted(named_paths(text)):
        if path in entries or path in allowed_dirs:
            continue
        parent = os.path.dirname(path)
        if parent in allowed_dirs and os.path.basename(path) in RUNTIME_FILES:
            continue
        if (os.path.dirname(parent) in allowed_dirs
                and os.path.basename(parent) in RUNTIME_SUBDIRS):
            continue
        return fail(
            f"docs/INSTALL.md names {path!r}, which the release zip neither ships "
            f"nor ships a folder for. `scripts/package.sh --list` prints:\n  "
            + "\n  ".join(entries))

    spans = set(code_spans(text))
    named = named_paths(text)
    for entry in entries:
        # Asked of the code spans, not of the document: the sweep above only
        # sees a path the guide wrote as code, so accepting a prose mention here
        # would let an entry be "named" in a way the other half cannot check.
        if entry in named or any(entry in span for span in spans):
            continue
        return fail(f"the archive ships {entry!r} and docs/INSTALL.md never names it "
                    "in a code span")

    # Redundant with the paths above -- the title id is a path segment in one of
    # them -- and asserted anyway, because #35 asks for it by name and a future
    # rewrite of the guide could drop the path while keeping a bare id in prose.
    if tid not in text:
        return fail(f"the title id is {tid} in sysmodule/sys-rommsync.json; "
                    "docs/INSTALL.md does not carry it")
    for other in re.findall(rf"\b[0-9A-Fa-f]{{{len(tid)}}}\b", text):
        if other.upper() != tid:
            return fail(f"docs/INSTALL.md names title id {other}, but "
                        f"sysmodule/sys-rommsync.json says {tid}")

    print(f"{len(entries)} archive entries, every path in the guide accounted for, tid {tid}")
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
                return fail(f"docs/INSTALL.md links {target!r}; there is no such file")
        else:
            resolved = guide
        if anchor and anchor not in headings(read(resolved)):
            return fail(f"docs/INSTALL.md links {target!r}; "
                        f"{os.path.relpath(resolved, repo)} has no such heading")
        checked += 1

    if checked == 0:
        return fail("docs/INSTALL.md has no relative links at all; it is supposed to "
                    "point at CONFIG.md, AUTH.md and SECURITY.md rather than restate them")
    print(f"{checked} relative links resolve")
    return 0


# --- safety -------------------------------------------------------------------

def phase_safety(args) -> int:
    text = read(guide_path(args.repo))

    for literal in FORBIDDEN_LITERALS:
        if literal in text:
            return fail(f"docs/INSTALL.md contains {literal!r}. Nothing in this guide "
                        "may hand a user a way to skip certificate verification "
                        "(docs/SECURITY.md).")

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
                "docs/INSTALL.md appears to instruct the user to do the thing the "
                "device-code flow exists to avoid, or to weaken TLS:\n"
                f"  {sentence.strip()}\n"
                "No step may ask for an account secret on the console (M1), and none "
                "may turn certificate verification off (docs/SECURITY.md).")

    print("no step asks for an account secret on the console or weakens TLS")
    return 0


# --- pairing ------------------------------------------------------------------

def phase_pairing(args) -> int:
    repo = args.repo
    text = read(guide_path(repo))
    header = read(os.path.join(repo, "core", "include", "rommsync", "pairing.hpp"))

    body = re.search(r"enum class PairingState \{(.*?)\};", header, re.DOTALL)
    if not body:
        return fail("cannot find `enum class PairingState` in "
                    "core/include/rommsync/pairing.hpp")
    # The trailing comma is optional: an enumerator added as the LAST entry
    # without one would otherwise be silently absent from `states`, and both the
    # missing and the extra check below would pass with the guide never
    # mentioning it -- a hole in the exact thing this phase is for.
    states = [m[1:].lower()
              for m in re.findall(r"^\s*(k[A-Za-z]+)\s*(?:,|/{2,}|\}|$)",
                                  body.group(1), re.MULTILINE)]
    if not states:
        return fail("PairingState parsed as empty")

    rows = {}
    for line in text.splitlines():
        m = re.match(r"^\|([^|]+)\|\s*`([a-z]+)`\s*\|([^|]+)\|\s*$", line)
        if m:
            rows[m.group(2)] = (m.group(1).strip(), m.group(3).strip())

    missing = [s for s in states if s not in rows]
    if missing:
        return fail(f"docs/INSTALL.md's pairing table is missing {', '.join(missing)}; "
                    f"PairingState exposes {', '.join(states)}")
    extra = [s for s in rows if s not in states]
    if extra:
        return fail(f"docs/INSTALL.md's pairing table names {', '.join(extra)}, "
                    f"which PairingState does not expose")

    # pairing.hpp: "A human who refused the code, a code that ran out of time,
    # and a server that answered something this client cannot act on need three
    # different sentences on the overlay". A table that explains two of them the
    # same way has collapsed the distinction the enum exists to keep.
    failures = ("denied", "expired", "failed")
    meanings = {s: rows[s][1] for s in failures}
    if len(set(meanings.values())) != len(failures):
        return fail("the three terminal failures need three different sentences; "
                    f"the guide gives: {meanings}")
    headlines = {s: rows[s][0] for s in failures}
    if len(set(headlines.values())) != len(failures):
        return fail(f"the three terminal failures share a headline: {headlines}")

    # The third way this page rots is a screen that changed, and unlike the
    # other two it is not in the archive -- it is a string literal in the view.
    # Every headline the table promises, and every screen line the prose quotes,
    # is asserted against the source that draws it. The guide's convention is
    # that **"..."** is verbatim overlay text and nothing else is; italics are
    # for a word being discussed, not for a line being quoted.
    views = "\n".join(read(os.path.join(repo, "core", "src", name))
                      for name in ("overlay_pairing_view.cpp", "overlay_status_view.cpp"))
    quoted = {row[0] for row in rows.values()}
    quoted |= set(re.findall(r'\*\*"([^"]+)"\*\*', text))
    for line in sorted(quoted):
        if f'"{line}"' not in views:
            return fail(f"docs/INSTALL.md quotes the overlay as saying {line!r}; "
                        "no such string is drawn by core/src/overlay_pairing_view.cpp "
                        "or core/src/overlay_status_view.cpp")

    print(f"the pairing table is exactly {', '.join(states)}, three failures apart; "
          f"{len(quoted)} quoted screen lines are drawn by the views")
    return 0


# --- structure ----------------------------------------------------------------

def phase_structure(args) -> int:
    repo = args.repo
    text = read(guide_path(repo))

    found = [h.strip() for h in re.findall(r"^##\s+(.*?)\s*$", text, re.MULTILINE)]
    if found != SECTIONS:
        return fail("docs/INSTALL.md's sections are not #35's ten, in order.\n"
                    "  expected: " + " / ".join(SECTIONS) + "\n"
                    "  found:    " + " / ".join(found))

    # The note is a promise about scope, not decoration: the guide is reviewed
    # on paper and nothing has run it. Naming M8-2 is what tells whoever passes
    # that gate that removing the note is part of it.
    head = text[: text.index("## " + SECTIONS[0])]
    if not re.search(r"not yet validated on hardware", head, re.IGNORECASE):
        return fail("docs/INSTALL.md does not open with the "
                    "'not yet validated on hardware' note (hard rule 1)")
    if "M8-2" not in head:
        return fail("the hardware note does not name M8-2 as what removes it")

    readme = read(os.path.join(repo, "README.md"))
    if "docs/INSTALL.md" not in readme:
        return fail("README.md does not link docs/INSTALL.md")

    print(f"{len(found)} sections in order, the hardware note names M8-2, README links it")
    return 0


PHASES = {
    "paths": phase_paths,
    "links": phase_links,
    "safety": phase_safety,
    "pairing": phase_pairing,
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
