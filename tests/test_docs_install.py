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

docs/INTEROP.md (#243) is the install guide's second half -- what to set on the
*other* devices -- and is held here for the same reason and in the same way:
nobody may follow it before M8-2, so every number, version, key and name it
gives a user is read off the code that decides it.

  interop        its sections in order, the hardware note, the README and the
                 install guide linking it, the README's status line no longer
                 saying nothing is built, and CONFIG.md / CONFIGURATION.md each
                 pointing at the other (two pages called "configuration").
  interop_facts  the oldest RomM, the limits (saves, states, roms, folder
                 entries), the conflict copy's name, the RetroArch keys, the
                 Cloud Sync path and every quoted screen line, each against the
                 code.
  interop_table  the per-system table both ways against the Tico module table
                 in core/src/save_source.cpp, and the RetroArch core folders
                 against RomM's table pinned in the same file.

`links` covers both pages.
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

# The one directory whose contents the guide names and the archive does not
# ship: `flags/`, which the archive carries empty (#211) and ovl-sysmodules
# writes boot2.flag into. What may sit in it is not derivable from the
# manifest -- but the title id directory ABOVE it still is, which is what keeps
# a moved title id red here too.
RUNTIME_SUBDIRS = ("flags",)

# Runtime state that lives beside the shipped config, and is named by the guide
# precisely because it is NOT in the archive: an upgrade must leave it alone.
RUNTIME_FILES = ("config.ini", "token.dat", "device.dat", "state.db", "queue.json",
                 "rommsync.log", "rommsync.log.old")

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
# paragraph above it. Each is a real switch on one side or the other of the
# boundary docs/SECURITY.md#a-self-signed-certificate-on-a-home-server draws:
# `verify_peer` is this client's own `ClientOptions` field, and the other two
# are the Horizon `ssl` bypass M0-1 measured. A curl flag is neither, which is
# why none is listed.
FORBIDDEN_LITERALS = ("verify_peer = false", "SslOptionType_SkipDefaultVerify",
                      "SetVerifyOption(0)")


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


def program_id(repo: str) -> str:
    """Read from the NPDM config by a JSON reader, where package.sh uses a regex."""
    with open(os.path.join(repo, "sysmodule", "sys-rommsync.json"), encoding="utf-8") as f:
        tid = json.load(f)["program_id"]
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
    tid = program_id(repo)

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
    checked = 0

    for guide in (guide_path(repo), interop_path(repo)):
        name = os.path.relpath(guide, repo)
        text = read(guide)
        page_links = 0
        for target in re.findall(r"\]\(([^)\s]+)\)", text):
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            path, _, anchor = target.partition("#")
            if path:
                resolved = os.path.normpath(os.path.join(os.path.dirname(guide), path))
                if not os.path.exists(resolved):
                    return fail(f"{name} links {target!r}; there is no such file")
            else:
                resolved = guide
            if anchor and anchor not in headings(read(resolved)):
                return fail(f"{name} links {target!r}; "
                            f"{os.path.relpath(resolved, repo)} has no such heading")
            page_links += 1
        if page_links == 0:
            return fail(f"{name} has no relative links at all; it is supposed to point at "
                        "CONFIG.md, SYNC_PROTOCOL.md and SECURITY.md rather than restate them")
        checked += page_links

    print(f"{checked} relative links resolve across INSTALL.md and INTEROP.md")
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


# --- interop: docs/INTEROP.md (#243) -------------------------------------------

def interop_path(repo: str) -> str:
    path = os.path.join(repo, "docs", "INTEROP.md")
    if not os.path.exists(path):
        raise SystemExit(fail("there is no docs/INTEROP.md: the page that says how the Switch "
                              "meets RetroArch, the web player, Argosy and Grout (#243)"))
    return path


# The sections #243's Scope asks for, in the order a user setting up a second
# device meets them: is my server new enough, what has to agree, which systems,
# then each kind of other device, then what goes wrong and where it stops.
INTEROP_SECTIONS = [
    "Which RomM",
    "What has to agree",
    "Which systems sync with what",
    "Setting up RetroArch on your other devices",
    "RetroArch on the Switch: Cloud Sync or rommsync-nx, not both",
    "The web player, Argosy and Grout",
    "Tico 0.8 accounts",
    "When two devices disagree",
    "Known limits",
]


def section(text: str, heading: str) -> str:
    """The body under `## heading` exactly (not a `###` that ends the same way),
    up to the next `## `."""
    found = re.search(rf"^## {re.escape(heading)}\s*$", text, re.MULTILINE)
    if not found:
        raise SystemExit(fail(f"docs/INTEROP.md has no section '## {heading}'"))
    end = re.search(r"^## ", text[found.end():], re.MULTILINE)
    return text[found.start(): found.end() + end.start()] if end else text[found.start():]


def headers_text(repo: str) -> str:
    folder = os.path.join(repo, "core", "include", "rommsync")
    return "\n".join(read(os.path.join(folder, name)) for name in sorted(os.listdir(folder))
                     if name.endswith(".hpp"))


def constant(repo: str, name: str, seen=()) -> int:
    """An integer `inline constexpr` from core/'s headers, its expression resolved.

    `kMaxStates` is written `state::kMaxRecords / 4` rather than as a number, so
    a reader that only took literals would pin the page to nothing at all."""
    m = re.search(rf"inline constexpr [\w:<>]+ {name} = ([^;]+);", headers_text(repo))
    if not m:
        raise SystemExit(fail(f"cannot find `{name}` in core/include/rommsync"))
    expression = m.group(1)
    for qualified in set(re.findall(r"(?:\w+::)*(k\w+)", expression)):
        if qualified in seen:
            raise SystemExit(fail(f"`{name}` is defined in terms of itself"))
        expression = re.sub(rf"(?:\w+::)*\b{qualified}\b",
                            str(constant(repo, qualified, seen + (name,))), expression)
    if not re.fullmatch(r"[\d\s+\-*/()]+", expression):
        raise SystemExit(fail(f"`{name}` is `{m.group(1)}`, which this test cannot evaluate"))
    return int(eval(expression.replace("/", "//")))  # digits and operators only


def string_literals(paths) -> list:
    literals = []
    for path in paths:
        literals.extend(re.findall(r'"((?:[^"\\\n]|\\.)*)"', read(path)))
    return literals


def phase_interop(args) -> int:
    repo = args.repo
    text = read(interop_path(repo))

    found = [h.strip() for h in re.findall(r"^##\s+(.*?)\s*$", text, re.MULTILINE)]
    if found != INTEROP_SECTIONS:
        return fail("docs/INTEROP.md's sections are not #243's, in order.\n"
                    "  expected: " + " / ".join(INTEROP_SECTIONS) + "\n"
                    "  found:    " + " / ".join(found))
    head = text[: text.index("## " + INTEROP_SECTIONS[0])]
    if not re.search(r"not yet validated on hardware", head, re.IGNORECASE) or "M8-2" not in head:
        return fail("docs/INTEROP.md does not open with the 'not yet validated on hardware' "
                    "note naming M8-2 (hard rule 1)")

    readme = read(os.path.join(repo, "README.md"))
    if "docs/INTEROP.md" not in readme:
        return fail("README.md does not link docs/INTEROP.md")
    # #243: the status line said nothing on the Switch was built, while M0-M10
    # built all of it. A user reads that line first and the rest not at all.
    for stale in ("not yet implemented", "server-side groundwork", "does nothing yet",
                  "draws nothing yet"):
        if stale in readme:
            return fail(f"README.md still says {stale!r}; the sysmodule and the overlay are "
                        "built and tested off-console")
    if "INTEROP.md" not in read(guide_path(repo)):
        return fail("docs/INSTALL.md never points at docs/INTEROP.md, and a user who has "
                    "installed the client is the one who next asks about other devices")

    # Two pages called "configuration": the user's config.ini and autofleet's
    # project config. Each says, before its first section, that it is not the
    # other one.
    for page, other in (("CONFIG.md", "CONFIGURATION.md"), ("CONFIGURATION.md", "CONFIG.md")):
        body = read(os.path.join(repo, "docs", page))
        opening = body[: re.search(r"^## ", body, re.MULTILINE).start()]
        if f"]({other})" not in opening:
            return fail(f"docs/{page} does not point at docs/{other} before its first section")

    print(f"{len(found)} sections in order, the hardware note names M8-2, README and INSTALL "
          "link it, and the two configuration pages point at each other")
    return 0


def phase_interop_facts(args) -> int:
    repo = args.repo
    text = read(interop_path(repo))

    # The oldest RomM, from the constant the client refuses below.
    caps = read(os.path.join(repo, "core", "include", "rommsync", "server_capabilities.hpp"))
    m = re.search(r"kMinimumVersion\{(\d+),\s*(\d+),\s*(\d+)\}", caps)
    if not m:
        return fail("cannot find `kMinimumVersion` in server_capabilities.hpp")
    minimum = ".".join(m.groups())
    if f"RomM {minimum} or later" not in section(text, "Which RomM"):
        return fail(f"'Which RomM' does not say RomM {minimum} or later, the floor "
                    "`server::kMinimumVersion` holds")

    # Every limit a user can hit, as the number the code stops at. Any other
    # "<n> saves" on the page is a stale one.
    bounds = {
        "saves": {constant(repo, "kMaxRecords"), constant(repo, "kMaxSaves"),
                  constant(repo, "kMaxPlanOperations")},
        "states": {constant(repo, "kMaxStates")},
        "roms": {constant(repo, "kMaxIndexRoms")},
        "entries": {constant(repo, "kMaxDirectoryEntries")},
    }
    limits = section(text, "Known limits")
    for noun, values in bounds.items():
        for value in values:
            if f"{value:,} {noun}" not in limits:
                return fail(f"'Known limits' does not give the {value:,} {noun} the code stops at")
    for number, noun in re.findall(r"\b(\d[\d,]*) (saves|states|roms|entries)\b", text):
        if int(number.replace(",", "")) not in bounds[noun]:
            return fail(f"docs/INTEROP.md says {number} {noun}; the code's bound is "
                        f"{', '.join(str(v) for v in sorted(bounds[noun]))}")

    # The name the losing side of a conflict is kept under on the server (#240).
    execute = read(os.path.join(repo, "core", "src", "sync_execute.cpp"))
    if '" [switch-conflict "' not in execute:
        return fail("sync::ArchiveFileName no longer writes ' [switch-conflict '; "
                    "docs/INTEROP.md names that copy by it")
    if "[switch-conflict " not in section(text, "When two devices disagree"):
        return fail("'When two devices disagree' does not name the archival copy RomM keeps")

    # The shared slot, as config.hpp defaults it.
    slot = re.search(r'kDefaultSlot = "([^"]+)"',
                     read(os.path.join(repo, "core", "include", "rommsync", "config.hpp")))
    if not slot or f"`{slot.group(1)}`" not in section(text, "What has to agree"):
        return fail("'What has to agree' does not name the default slot `kDefaultSlot` holds")

    # RetroArch's own keys, as retroarch_cfg.cpp reads them.
    read_keys = set(re.findall(r'\{"([a-z_]+)", Key::',
                               read(os.path.join(repo, "core", "src", "retroarch_cfg.cpp"))))
    named = {token for span in code_spans(text)
             for token in re.findall(r"\b(?:sort|savefiles|savestates|cloud_sync|savefile|"
                                     r"savestate)_[a-z_]+\b", span)}
    if not named:
        return fail("docs/INTEROP.md names no retroarch.cfg key; the RetroArch section is "
                    "supposed to say which settings to change")
    unknown = sorted(named - read_keys)
    if unknown:
        return fail(f"docs/INTEROP.md names {unknown}, which core/src/retroarch_cfg.cpp does "
                    "not read")

    # RetroArch's Cloud Sync endpoint, as the rig test that talks to it spells it.
    path = re.search(r'"(/api/sync/retroarch/)"',
                     read(os.path.join(repo, "tests", "test_engine.cpp")))
    if not path or path.group(1) not in text:
        return fail("docs/INTEROP.md does not give the Cloud Sync path `AsRetroArch` "
                    "(tests/test_engine.cpp) exercises against the real RomM")

    # The "tested with" table is #244's matrix: a row per client the matrix
    # runs, so a client added to it is a row the page is missing.
    cmake = read(os.path.join(repo, "tests", "CMakeLists.txt"))
    matrix = re.search(r"foreach\(client ([a-z ]+)\)\s*\n\s*foreach\(cell IN LISTS INTEROP_CELLS\)",
                       cmake)
    if not matrix:
        return fail("cannot find the interop matrix's clients in tests/CMakeLists.txt")
    clients = matrix.group(1).split()
    tested = table_rows(section(text, "The web player, Argosy and Grout"),
                        "| Client | Plays, in the test |")
    if len(tested) != len(clients):
        return fail(f"the 'Tested with' table has {len(tested)} rows; the interop matrix "
                    f"(tests/CMakeLists.txt) runs {len(clients)} clients: {', '.join(clients)}")

    # **"..."** is verbatim screen text, as in INSTALL.md: each is in a literal
    # the client draws or reports.
    sources = [os.path.join(repo, "core", "src", n)
               for n in sorted(os.listdir(os.path.join(repo, "core", "src")))
               if n.endswith(".cpp")]
    literals = string_literals(sources)
    quoted = re.findall(r'\*\*"([^"]+)"\*\*', text)
    if not quoted:
        return fail("docs/INTEROP.md quotes no screen line at all; it is supposed to name "
                    "what the overlay says when RomM is too old and when RetroArch syncs itself")
    for line in quoted:
        if not any(line in literal for literal in literals):
            return fail(f"docs/INTEROP.md quotes the console as saying {line!r}; no string "
                        "literal in core/src says it")

    print(f"RomM {minimum}, the limits, the conflict copy, {len(named)} retroarch.cfg keys, the "
          f"Cloud Sync path and {len(quoted)} quoted lines all match the code")
    return 0


def table_rows(text: str, header: str) -> list:
    """The body rows of the table whose header line starts with `header`."""
    # Stripped, because a table inside a numbered step is indented under it.
    lines = [line.strip() for line in text.splitlines()]
    for i, line in enumerate(lines):
        if line.startswith(header):
            rows = []
            for row in lines[i + 2:]:
                if not row.startswith("|"):
                    break
                rows.append([cell.strip() for cell in row.strip().strip("|").split("|")])
            return rows
    raise SystemExit(fail(f"docs/INTEROP.md has no table headed {header!r}"))


def phase_interop_table(args) -> int:
    repo = args.repo
    text = read(interop_path(repo))
    source = read(os.path.join(repo, "core", "src", "save_source.cpp"))

    tico = re.search(r"kTicoModules\[\] = \{(.*?)\};", source, re.DOTALL)
    cores = re.search(r"kCores = \{(.*?)\};", source, re.DOTALL)
    if not tico or not cores:
        return fail("cannot find kTicoModules or RetroArchCores' table in save_source.cpp")
    # A row may carry its module's save format after the id (#258).
    code_modules = set(re.findall(r'\{"[a-z0-9-]+", "([^"]+)", "([a-z0-9_-]+)"[},]', tico.group(1)))
    code_folders = set(re.findall(r'\{"([a-z0-9_-]+)", "([^"]+)"\}', cores.group(1)))
    if not code_modules or not code_folders:
        return fail("kTicoModules or RetroArchCores() parsed as empty")

    rows = table_rows(section(text, "Which systems sync with what"),
                      "| System | Tico module | Emulator id |")
    documented = set()
    for row in rows:
        if len(row) < 5:
            return fail(f"a row of the systems table has {len(row)} cells: {row}")
        ids = re.findall(r"`([a-z0-9_-]+)`", row[2])
        if len(ids) != 1:
            return fail(f"the systems table's emulator id cell should be one id: {row[2]!r}")
        documented.add((row[1], ids[0]))
        isolated = ids[0].startswith("tico-")
        says_isolated = "Switch-only" in row[4] or "not synced" in row[4]
        if isolated != says_isolated:
            return fail(f"{row[0]} is {ids[0]}: a tico-* id is Switch-only or not synced, and "
                        "nothing else is -- the table's status says otherwise")
    if documented != code_modules:
        return fail("the systems table is not save_source.cpp's Tico module table.\n"
                    f"  only in the page: {sorted(documented - code_modules)}\n"
                    f"  only in the code: {sorted(code_modules - documented)}")

    folders = table_rows(section(text, "Setting up RetroArch on your other devices"),
                         "| Emulator id | RetroArch core folder |")
    by_id = {}
    for row in folders:
        if len(row) < 2:
            return fail(f"a row of the core folder table has {len(row)} cells: {row}")
        ident = re.findall(r"`([^`]+)`", row[0])
        folder = re.findall(r"`([^`]+)`", row[1])
        if len(ident) != 1 or len(folder) != 1:
            return fail(f"a row of the core folder table is not `id` | `Folder`: {row}")
        if (ident[0], folder[0]) not in code_folders:
            return fail(f"RomM files {folder[0]}/ under another id than {ident[0]}; "
                        "RetroArchCores() in save_source.cpp is RomM's table")
        by_id[ident[0]] = folder[0]
    shared = {ident for _, ident in code_modules if not ident.startswith("tico-")}
    missing = sorted(shared - set(by_id))
    if missing:
        return fail(f"the core folder table leaves out {missing}, which the Switch syncs in "
                    "a RetroArch core's format")

    # Pairs rather than systems: kTicoModules has a row per Tico folder, and the
    # four Genesis Plus GX folders are one module and one id.
    print(f"{len(documented)} module/id pairs match the Tico module table both ways; "
          f"{len(by_id)} RetroArch core folders match RomM's table")
    return 0


PHASES = {
    "paths": phase_paths,
    "links": phase_links,
    "safety": phase_safety,
    "pairing": phase_pairing,
    "structure": phase_structure,
    "interop": phase_interop,
    "interop_facts": phase_interop_facts,
    "interop_table": phase_interop_table,
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("phase", choices=sorted(PHASES))
    ap.add_argument("--repo", required=True)
    args = ap.parse_args()
    return PHASES[args.phase](args)


if __name__ == "__main__":
    sys.exit(main())
