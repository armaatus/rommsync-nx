#!/usr/bin/env python3
"""scripts/selftest-judge.py against canned reports, and the selftest NRO held to it.

M12-1 (#306). The NRO cannot run before the v1 gate (hard rule 1), so its
report is judged here only in the shapes a console could write it -- every one
canned under tests/fixtures/selftest/. What a console run alone can prove is
docs/HARDWARE_TEST_PLAN.md's PRE-02, not a claim made here.

  judge    a passing report passes; a failed, skipped or missing required row
           fails and is named; `na` passes only where the row allows it
           (card.fat32_ceiling, off a FAT32 card); an optional row's skip
           passes; a malformed report (not JSON, an unknown result, a row
           twice) is refused as MALFORMED with exit 2, never judged.
  pairs    `--pairs N` -- the launch gate's `selftest` row -- needs N distinct
           HOS/Atmosphere pairs among the passing reports; two reports off one
           pair are one pair.
  rows     every row the judge requires, the NRO writes: each id appears as a
           string literal in selftest/source, and the NRO writes no id the
           judge does not know.
  roots    the NRO writes only under sdmc:/config/rommsync-selftest/ (#306
           acceptance): every `sdmc:` path literal in selftest/source is under
           it, and every write it makes goes through the one root constant.
  gate     scripts/launch-gate.sh's selftest note, the plan's PRE-02 and the
           judge name each other: the note cites the judge with `--pairs 2`,
           the plan cites the judge, and the row stays a console row.
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JUDGE = os.path.join(REPO, "scripts", "selftest-judge.py")
FIXTURES = os.path.join(REPO, "tests", "fixtures", "selftest")
SOURCE = os.path.join(REPO, "selftest", "source")

ROOT = "sdmc:/config/rommsync-selftest/"
ROOT_SD = "/config/rommsync-selftest"


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def judge(*args: str) -> subprocess.CompletedProcess:
    # A bare `*.json` is a fixture; flags and their values pass through.
    fixed = [os.path.join(FIXTURES, a) if a.endswith(".json") and not os.path.isabs(a) else a
             for a in args]
    return subprocess.run([sys.executable, JUDGE, *fixed], capture_output=True, text=True)


def expect(result: subprocess.CompletedProcess, status: int, *needles: str, what: str) -> None:
    out = result.stdout + result.stderr
    if result.returncode != status:
        fail(f"{what}: exit {result.returncode}, expected {status}\n{out}")
    for needle in needles:
        if not re.search(needle, out, re.MULTILINE):
            fail(f"{what}: no /{needle}/ in the output\n{out}")


def scenario_judge() -> None:
    expect(judge("pass-21.0.0-ams-1.10.1.json"), 0,
           r"^PASS +proto\.tick", r"^VERDICT PASS .*pass-21\.0\.0", what="a passing report")
    expect(judge("pass-19.0.1-ams-1.8.0.json"), 0,
           r"^PASS +card\.fat32_ceiling .*na", r"^PASS +net\.tls13_only .*skip",
           what="an exFAT report with an optional row skipped")
    expect(judge("fail-tick.json"), 1,
           r"^FAIL +proto\.tick .*partial", r"^VERDICT FAIL", what="a failed row")
    expect(judge("missing-heap.json"), 1,
           r"^MISSING +footprint\.heap", r"^VERDICT FAIL", what="a missing row")
    expect(judge("skipped-required.json"), 1,
           r"^FAIL +proto\.download_resume .*skip", what="a required row skipped")
    expect(judge("na-on-fat32.json"), 1,
           r"^FAIL +card\.fat32_ceiling .*na", what="na on a FAT32 card")
    for name, why in (("malformed-truncated.json", "not JSON"),
                      ("malformed-result.json", "maybe"),
                      ("malformed-duplicate.json", "net\\.tls.*twice")):
        expect(judge(name), 2, r"^MALFORMED ", why, what=f"{name}")
    # One malformed report among good ones still refuses the set.
    expect(judge("pass-21.0.0-ams-1.10.1.json", "malformed-truncated.json"), 2,
           r"^MALFORMED ", what="a malformed report beside a good one")
    expect(judge(os.path.join(FIXTURES, "no-such-report.json")), 2,
           r"no-such-report", what="a report that is not there")
    expect(judge(), 2, r"usage", what="no report at all")
    print("ok: the judge passes, fails, names missing rows and refuses malformed reports")


def scenario_pairs() -> None:
    expect(judge("--pairs", "2", "pass-21.0.0-ams-1.10.1.json", "pass-19.0.1-ams-1.8.0.json"),
           0, r"2 HOS/Atmosphere pairs", what="two pairs")
    expect(judge("--pairs", "2", "pass-21.0.0-ams-1.10.1.json",
                 "pass-21.0.0-ams-1.10.1-again.json"),
           1, r"1 HOS/Atmosphere pair.*2", what="two reports off one pair")
    expect(judge("--pairs", "2", "pass-21.0.0-ams-1.10.1.json", "fail-tick.json"),
           1, r"^VERDICT FAIL", what="a failing report does not count as a pair")
    print("ok: --pairs counts distinct HOS/Atmosphere pairs among passing reports")


def judged_rows() -> set:
    out = subprocess.run([sys.executable, JUDGE, "--rows"], capture_output=True, text=True,
                         check=True).stdout
    rows = {line.split()[0] for line in out.splitlines() if line.strip()}
    if len(rows) < 20:
        fail(f"the judge lists {len(rows)} rows; the check below would pass vacuously")
    return rows


def sources() -> dict:
    found = {}
    for dirpath, _, names in os.walk(SOURCE):
        for name in names:
            if name.endswith((".cpp", ".hpp")):
                path = os.path.join(dirpath, name)
                with open(path, encoding="utf-8") as f:
                    found[os.path.relpath(path, REPO)] = f.read()
    if not found:
        fail("no sources under selftest/source")
    return found


def scenario_rows() -> None:
    rows = judged_rows()
    # Without the #include lines: "card.hpp" is a header, not a row.
    text = re.sub(r"^#include .*$", "", "\n".join(sources().values()), flags=re.MULTILINE)
    literals = set(re.findall(r'"((?:platform|card|net|proto|footprint)\.[a-z0-9_]+)"', text))
    missing = rows - literals
    if missing:
        fail(f"the judge requires rows the NRO never writes: {sorted(missing)}")
    unknown = literals - rows
    if unknown:
        fail(f"the NRO writes rows the judge does not know: {sorted(unknown)}")
    # The footprint row holds the NRO's peak to the sysmodule's heap: the two
    # numbers have to be the same number.
    main = open(os.path.join(REPO, "sysmodule", "source", "main.cpp"), encoding="utf-8").read()
    sysmodule = re.search(r"^constexpr size_t kInnerHeapSize = (0x[0-9A-Fa-f]+);", main, re.M)
    ours = re.search(r"kSysmoduleHeap = (0x[0-9A-Fa-f]+);", text)
    if not sysmodule or not ours:
        fail("could not read kInnerHeapSize or selftest's kSysmoduleHeap")
    if int(sysmodule.group(1), 16) != int(ours.group(1), 16):
        fail(f"selftest's kSysmoduleHeap {ours.group(1)} is not the sysmodule's "
             f"kInnerHeapSize {sysmodule.group(1)}")
    print(f"ok: the NRO writes exactly the {len(rows)} rows the judge reads, "
          f"and holds its heap to the sysmodule's {sysmodule.group(1)}")


def scenario_roots() -> None:
    files = sources()
    bad = []
    for path, text in files.items():
        for lit in re.findall(r'"(sdmc:[^"]*)"', text):
            if not lit.startswith(ROOT) and lit not in ("sdmc:", "sdmc:/"):
                bad.append(f"{path}: {lit}")
        # ...and SD-root absolute paths, the form core/ takes: anything inside
        # a top-level folder the card's tools keep. A bare "/saves" is a leaf
        # appended to the scratch folder, and "/api/..." a URL path.
        for lit in re.findall(r'"(/(?:config|switch|atmosphere|retroarch|emulators)/[^"]*)"',
                              text):
            if not (lit + "/").startswith(ROOT_SD + "/"):
                bad.append(f"{path}: {lit}")
    if bad:
        fail("the NRO names sdmc: paths outside " + ROOT + ":\n  " + "\n  ".join(bad))
    # Every write goes through the root: no file under selftest/source opens a
    # path for writing, renames, or makes a directory except from a path the
    # root helper built.
    writers = re.compile(r'\b(fopen|rename|mkdir|remove|unlink|rmdir|truncate)\s*\(\s*"')
    for path, text in files.items():
        hit = writers.search(text)
        if hit:
            fail(f"{path} calls {hit.group(1)} on a literal path; build it from the root")
    if not any(ROOT in text for text in files.values()):
        fail(f"no source names {ROOT}; the check above would pass vacuously")
    print(f"ok: the NRO writes only under {ROOT}")


def scenario_gate() -> None:
    out = subprocess.run(["bash", os.path.join(REPO, "scripts", "launch-gate.sh"), "--rows"],
                         capture_output=True, text=True, check=True).stdout
    kinds = {line.split("|")[0]: line.split("|")[1] for line in out.splitlines() if line}
    if kinds.get("selftest") != "console":
        fail(f"the selftest row is '{kinds.get('selftest')}', not a console row")
    with open(os.path.join(REPO, "scripts", "launch-gate.sh"), encoding="utf-8") as f:
        gate = f.read()
    note = re.search(r"selftest\) cat <<'NOTE'\n(.*?)\nNOTE", gate, re.DOTALL)
    if not note or not re.search(r"scripts/selftest-judge\.py\s+--pairs\s+2", note.group(1)):
        fail("the launch gate's selftest note does not cite scripts/selftest-judge.py --pairs 2")
    with open(os.path.join(REPO, "docs", "HARDWARE_TEST_PLAN.md"), encoding="utf-8") as f:
        plan = f.read()
    pre02 = re.search(r"^### PRE-02:.*?(?=^### )", plan, re.DOTALL | re.MULTILINE)
    if not pre02 or "scripts/selftest-judge.py" not in pre02.group(0):
        fail("the plan's PRE-02 does not cite scripts/selftest-judge.py")
    if "once it ships" in pre02.group(0) or "has not landed" in pre02.group(0):
        fail("PRE-02 still speaks of the selftest as not yet landed")
    print("ok: the gate's note, the plan's PRE-02 and the judge cite each other")


SCENARIOS = {"judge": scenario_judge, "pairs": scenario_pairs, "rows": scenario_rows,
             "roots": scenario_roots, "gate": scenario_gate}

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", choices=sorted(SCENARIOS))
    SCENARIOS[parser.parse_args().scenario]()
