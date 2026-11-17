#!/usr/bin/env python3
"""docs/HARDWARE_TEST_PLAN.md and docs/HARDWARE_RESULTS.md, held together and to the code.

M12-2 (#307). The plan is the one script whoever holds a console runs, and the
results page is where each run is written down in the same shape. Nobody can
check either by following it before the v1 gate (hard rule 1), so what this
test holds them to is what a reader on the far side of that gate relies on:

  ids      every scenario has a stable id, unique, of the form `AREA-NN`, under
           an area the plan declares, with its steps and its pass criteria.
  results  the results template lists every scenario, in the plan's order and
           nothing else, and asks for every field #307 names about a console.
  cited    every scenario is cited by a launch-gate row (M12-0, #305), every id
           the plan cites anywhere exists, and #44's checklists each point at
           scenarios that exist -- an id nothing cites is a scenario the gate
           would never ask about.
  events   every log line the plan quotes is one this build can write: its tag
           is in `log::kAllEvents` (read the way docs.troubleshooting reads it).
  facts    the names the plan quotes are the code's: the title id, the log and
           backup paths, the crash-report folder, `ssl`'s would-block result
           code, the Clock row's wording, and every repo path it names exists.
  links    every relative link resolves, anchors included, on both pages.
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_docs_troubleshooting as tsg  # noqa: E402  (events(), headings(), FENCE)

PLAN = os.path.join("docs", "HARDWARE_TEST_PLAN.md")
RESULTS = os.path.join("docs", "HARDWARE_RESULTS.md")

SCENARIO = re.compile(r"^### ([A-Z]+-\d{2}): \S", re.MULTILINE)
ID = re.compile(r"\b([A-Z]{2,5}-\d{2})\b")

# What a run is described by, so runs on different consoles compare (#307 Scope).
RUN_FIELDS = (
    "Console model",
    "HOS",
    "Atmosphère",
    "emuMMC or sysMMC",
    "SD card filesystem",
    "Ultrahand",
    "nx-ovlloader",
    "Co-resident sysmodules",
    "Build sha",
)


def gate_rows(repo: str) -> dict:
    """M12-0's rows (#305), id -> kind, read from `scripts/launch-gate.sh --rows`:
    the script owns them, so the plan's table cannot drift from a copy here."""
    out = subprocess.run(["bash", os.path.join(repo, "scripts", "launch-gate.sh"), "--rows"],
                         capture_output=True, text=True, check=True).stdout
    return {line.split("|")[0]: line.split("|")[1] for line in out.splitlines() if line}


def fail(msg: str) -> int:
    print(f"FAIL: {msg}", file=sys.stderr)
    return 1


def read(repo: str, rel: str) -> str:
    path = os.path.join(repo, rel)
    if not os.path.exists(path):
        raise SystemExit(fail(f"{rel} does not exist"))
    with open(path, encoding="utf-8") as f:
        return f.read()


def section(text: str, heading: str) -> str:
    """The body under `heading` up to the next heading of the same or higher level."""
    m = re.search(rf"^(#+) {re.escape(heading)}\s*$", text, re.MULTILINE)
    if not m:
        raise SystemExit(fail(f"no {heading!r} heading"))
    level = len(m.group(1))
    rest = text[m.end():]
    end = re.search(rf"^#{{1,{level}}} ", rest, re.MULTILINE)
    return rest[: end.start()] if end else rest


def scenarios(text: str) -> list:
    return SCENARIO.findall(tsg.FENCE.sub("", text))


def table_rows(body: str) -> list:
    """`[cells]` per table row, the header and separator dropped."""
    rows = []
    for line in body.splitlines():
        line = line.strip()
        if not line.startswith("|") or re.match(r"^\|[\s:|-]+\|$", line):
            continue
        rows.append([c.strip() for c in line.strip("|").split("|")])
    return rows[1:] if rows else rows


# --- ids ----------------------------------------------------------------------

def phase_ids(args) -> int:
    text = read(args.repo, PLAN)
    ids = scenarios(text)
    if len(ids) < 20:
        return fail(f"{PLAN} has {len(ids)} scenarios; #307's scope lists more than that")
    dupes = sorted({i for i in ids if ids.count(i) > 1})
    if dupes:
        return fail(f"scenario ids are not unique: {dupes}")

    areas = {row[0].strip("`") for row in table_rows(section(text, "Scenario ids"))}
    if not areas:
        return fail("the 'Scenario ids' section declares no areas")
    for sid in ids:
        if sid.split("-")[0] not in areas:
            return fail(f"{sid}: its area is not one the 'Scenario ids' table declares {sorted(areas)}")

    bodies = re.split(r"^### ", text, flags=re.MULTILINE)[1:]
    for body in bodies:
        sid = body.split(":", 1)[0]
        if sid not in ids:
            continue
        body = re.split(r"^#{1,2} ", body, flags=re.MULTILINE)[0]
        for part in ("**Steps.**", "**Pass.**"):
            if part not in body:
                return fail(f"{sid} has no {part} paragraph")
    print(f"{len(ids)} scenarios, unique, in {len(areas)} areas, each with steps and pass criteria")
    return 0


# --- results ------------------------------------------------------------------

def phase_results(args) -> int:
    plan_ids = scenarios(read(args.repo, PLAN))
    text = read(args.repo, RESULTS)
    template = section(text, "The template")

    fields = [row[0].strip("*") for row in table_rows(section(template, "The console"))]
    missing = [f for f in RUN_FIELDS if f not in fields]
    if missing:
        return fail(f"{RESULTS}'s console table does not ask for {missing}")

    listed = [ID.search(row[0]).group(1) for row in table_rows(section(template, "The scenarios"))
              if ID.search(row[0])]
    if listed != plan_ids:
        only_plan = [i for i in plan_ids if i not in listed]
        only_results = [i for i in listed if i not in plan_ids]
        return fail(f"{RESULTS}'s scenario table is not the plan's scenarios in the plan's order: "
                    f"missing {only_plan}, not in the plan {only_results}"
                    + ("" if only_plan or only_results else ", order differs"))
    print(f"the template asks for {len(RUN_FIELDS)} console fields and lists all {len(listed)} scenarios")
    return 0


# --- cited --------------------------------------------------------------------

def phase_cited(args) -> int:
    text = read(args.repo, PLAN)
    ids = set(scenarios(text))

    # Every id-shaped token, not only those under a known area: `NETW-01` is a typo too.
    for rel, body in ((PLAN, text), (RESULTS, read(args.repo, RESULTS))):
        unknown = sorted(set(ID.findall(tsg.FENCE.sub("", body))) - ids)
        if unknown:
            return fail(f"{rel} cites {unknown}, which no scenario heading defines")

    rows = table_rows(section(text, "What the launch gate cites"))
    gate = gate_rows(args.repo)
    cited, named = set(), set()
    for row in rows:
        name = row[0].strip("`")
        if name not in gate:
            return fail(f"{name!r} is not a row of scripts/launch-gate.sh {sorted(gate)}")
        named.add(name)
        cited |= set(ID.findall(row[-1]))
    # Every console row cites its evidence. A row a person has since flipped to
    # `repo` keeps its line here, so the check runs one way only.
    unnamed = sorted(r for r, kind in gate.items() if kind == "console" and r not in named)
    if unnamed:
        return fail(f"the launch gate's console rows {unnamed} cite no scenarios in {PLAN}")
    uncited = sorted(ids - cited)
    if uncited:
        return fail(f"no launch-gate row cites {uncited}")

    folded = table_rows(section(text, "Where #44's checklists went"))
    if len(folded) < 10:
        return fail("the #44 table folds fewer than ten of its checklists; #307 asks for all of them")
    for row in folded:
        if not ID.findall(row[-1]):
            return fail(f"#44's {row[0]!r} points at no scenario")
    print(f"{len(rows)} launch-gate rows cite all {len(ids)} scenarios; {len(folded)} #44 checklists folded")
    return 0


# --- events -------------------------------------------------------------------

def phase_events(args) -> int:
    known = tsg.events(args.repo)
    text = read(args.repo, PLAN)
    lines = tsg.quoted_lines(text)
    if len(lines) < 10:
        return fail(f"{PLAN} quotes {len(lines)} log lines; a scenario's expected line is its evidence")
    for m in lines:
        if m.group(3) not in known:
            return fail(f"{PLAN} quotes a `{m.group(3)}` line; the log has no such event "
                        f"(log::kAllEvents: {sorted(known)})")

    # An inline `tag` too: a pass criterion naming a line the code cannot write is never met.
    namespaces = {t.split(".")[0] for t in known if "." in t}
    files = re.compile(r"\.(ini|dat|db|log|old|md|json|txt|flag|nsp|ovl|nro|cpp|hpp|py|sh|tmp|part|srm)$")
    for span in re.findall(r"`([a-z]+\.[a-z_]+)`", tsg.FENCE.sub("", text)):
        if span.split(".")[0] in namespaces and not files.search(span) and span not in known:
            return fail(f"{PLAN} names `{span}` as if it were a log event; it is not one")
    print(f"{len(lines)} quoted log lines, every tag one the code writes")
    return 0


# --- facts --------------------------------------------------------------------

def cpp_string(repo: str, rel: str, name: str) -> str:
    src = read(repo, rel)
    m = re.search(rf"\b{name} = \"([^\"]+)\"", src)
    if not m:
        raise SystemExit(fail(f"cannot find `{name}` in {rel}"))
    return m.group(1)


def phase_facts(args) -> int:
    repo = args.repo
    text = read(repo, PLAN)

    npdm = read(repo, "sysmodule/sys-rommsync.json")
    m = re.search(r"\"program_id\": \"0x([0-9A-Fa-f]{16})\"", npdm)
    if not m:
        return fail("cannot find program_id in sysmodule/sys-rommsync.json")
    tid = m.group(1).upper()
    named = set(t.upper() for t in re.findall(r"\b(42[0-9A-Fa-f]{14})\b", text))
    if named != {tid}:
        return fail(f"{PLAN} names title id(s) {sorted(named)}; the NPDM's is {tid}")

    for rel, name in (("core/include/rommsync/log.hpp", "kLogSdPath"),
                      ("core/include/rommsync/sync_execute.hpp", "kBackupDir"),
                      ("core/include/rommsync/diagnostics.hpp", "kCrashReportsSdDir")):
        value = cpp_string(repo, rel, name)
        if value not in text:
            return fail(f"{PLAN} never names {value} ({name}, {rel})")

    ssl = read(repo, "sysmodule/source/http/ssl_http_client.cpp")
    m = re.search(r"kSslResultWouldBlock = MAKERESULT\((\d+), (\d+)\)", ssl)
    if not m:
        return fail("cannot find kSslResultWouldBlock in ssl_http_client.cpp")
    module, description = int(m.group(1)), int(m.group(2))
    would_block = module | (description << 9)
    # Both spellings: the raw value a debugger shows and the 2xxx-yyyy a report shows.
    for spelling in (f"0x{would_block:X}", f"{2000 + module}-{description:04d}"):
        if spelling not in text:
            return fail(f"{PLAN} does not name ssl's would-block result as {spelling} "
                        "(kSslResultWouldBlock, ssl_http_client.cpp)")

    status = read(repo, "core/src/overlay_status_view.cpp")
    if "uploads held" not in text or "; uploads held\"" not in status:
        return fail("the Clock row's 'uploads held' is not in both the plan and overlay_status_view.cpp")

    roots = r"(?:core|host|sysmodule|overlay|tlsprobe|scripts|docs|server|tests|packaging)"
    paths = set(re.findall(rf"`({roots}/[\w./-]+)`", text))
    paths |= set(re.findall(rf"\]\(\.\./({roots}/[\w./-]+)", text))
    for p in sorted(paths):
        if not os.path.exists(os.path.join(repo, p.rstrip("/"))):
            return fail(f"{PLAN} names {p}; there is no such path in the tree")

    readme = read(repo, "overlay/README.md")
    steps = max(int(n) for n in re.findall(r"^(\d+)\. ", section(readme, "Manual verification (M8-2)"),
                                           re.MULTILINE))
    # `overlay/README.md step 5`, or the same with the name a link: `[...](...)\nsteps 1-17`.
    cites = re.findall(r"overlay/README\.md(?:\]\([^)]*\))?\s+steps? (\d+)(?:[-–](\d+))?", text)
    if not cites:
        return fail(f"{PLAN} cites no step of overlay/README.md's script")
    for a, b in cites:
        if max(int(a), int(b or a)) > steps:
            return fail(f"{PLAN} cites overlay/README.md step {b or a}; the script has {steps}")
    print(f"title id {tid}, log/backup/crash paths, 0x{would_block:X}, {len(paths)} repo paths, "
          f"and the overlay script's {steps} steps all agree with the tree")
    return 0


# --- links --------------------------------------------------------------------

def phase_links(args) -> int:
    checked = 0
    for rel in (PLAN, RESULTS):
        page = os.path.join(args.repo, rel)
        text = read(args.repo, rel)
        for target in re.findall(r"\]\(([^)\s]+)\)", tsg.FENCE.sub("", text)):
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            path, _, anchor = target.partition("#")
            resolved = os.path.normpath(os.path.join(os.path.dirname(page), path)) if path else page
            if not os.path.exists(resolved):
                return fail(f"{rel} links {target!r}; there is no such file")
            if anchor and os.path.isfile(resolved):
                with open(resolved, encoding="utf-8") as f:
                    if anchor not in tsg.headings(f.read()):
                        return fail(f"{rel} links {target!r}; "
                                    f"{os.path.relpath(resolved, args.repo)} has no such heading")
            checked += 1
    print(f"{checked} relative links resolve")
    return 0


PHASES = {
    "ids": phase_ids,
    "results": phase_results,
    "cited": phase_cited,
    "events": phase_events,
    "facts": phase_facts,
    "links": phase_links,
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("phase", choices=sorted(PHASES))
    ap.add_argument("--repo", required=True)
    args = ap.parse_args()
    return PHASES[args.phase](args)


if __name__ == "__main__":
    sys.exit(main())
