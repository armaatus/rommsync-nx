#!/usr/bin/env python3
"""Judge the on-console selftest's reports: PASS or FAIL per row (M12-1, #306).

    scripts/selftest-judge.py [--pairs N] REPORT.json...
    scripts/selftest-judge.py --rows

The selftest NRO (selftest/README.md) writes
`sdmc:/config/rommsync-selftest/report-<ts>.json` on a console. This is the
host half: it reads those files and says, row by row, whether the run proved
what the row asks. It is what the launch gate's `selftest` row
(scripts/launch-gate.sh) and the hardware plan's PRE-02
(docs/HARDWARE_TEST_PLAN.md) cite.

Each report gets one line per row the NRO is expected to write,

    PASS     net.tls                 pass  TLSv1.3, verified against the CertStore
    FAIL     proto.tick              fail  tick ended partial: 1 operation failed
    MISSING  footprint.heap          the report has no such row

and then `VERDICT PASS|FAIL <file> (<HOS>/<AMS>)`. A report that is not one the
NRO could have written -- not JSON, a schema this script does not know, an
unknown result, a row twice -- is `MALFORMED <file>: <why>` and is not judged at
all: a half-written file off a console that crashed is evidence of a crash, not
of a pass.

`--pairs N` is the launch gate's rule: the passing reports have to come from at
least N distinct HOS/Atmosphere pairs. Two runs on one console are one pair.

Exit status: 0 every report passed (and the pairs held), 1 a row failed or
went missing or the pairs did not hold, 2 a report was malformed or unreadable,
or the arguments were wrong. Standard library only: it runs on whatever Python
the person holding the SD card has.
"""
import argparse
import json
import sys

SCHEMA = 1

# The rows, in the order the NRO runs them, and what each may answer besides
# `pass`. This table is the contract between the NRO and this script:
# `ctest -R selftest.rows` holds selftest/source to exactly these ids.
#
#   required  must be `pass`. `skip` is a FAIL: a step nobody ran proved nothing.
#   na_unless a required row that may answer `na` when the console's
#             `sd_format` is anything but the value named -- the FAT32 ceiling
#             has nothing to refuse on exFAT. `na` on that format is a FAIL.
#   optional  `pass`, `skip` and `na` all pass; only `fail` fails. For the steps
#             that need something the minimum setup does not have (a TLS
#             1.3-only listener).
REQUIRED, OPTIONAL = "required", "optional"
ROWS = (
    # 1. Platform facts.
    ("platform.hos", REQUIRED, None, "HOS version, from setsysGetFirmwareVersion"),
    ("platform.ams", REQUIRED, None, "Atmosphere version, from spl config item 65000"),
    ("platform.emummc", REQUIRED, None, "emuMMC or sysMMC, from Atmosphere's emummc config"),
    ("platform.sd_format", REQUIRED, None, "FAT32 or exFAT, as the sysmodule reads it"),
    ("platform.free_space", REQUIRED, None, "free bytes on the card, above the floor"),
    ("platform.clock", REQUIRED, None, "the console clock against the server's Date header"),
    ("platform.mtime_utc", REQUIRED, None, "st_mtime of a file written now reads back as UTC"),
    # 2. Card I/O, under the selftest's own folder only.
    ("card.atomic_write", REQUIRED, None, "write .tmp, rename over, read back"),
    ("card.commit_device", REQUIRED, None, "fsdevCommitDevice(\"sdmc\") succeeds"),
    ("card.backup_then_commit", REQUIRED, None, "the save-overwrite sequence: backup first"),
    ("card.fat32_ceiling", REQUIRED, "fat32", "a file past 4 GiB is refused before writing"),
    ("card.long_unicode_path", REQUIRED, None, "a long, non-ASCII path round-trips"),
    # 3. Network and TLS.
    ("net.nifm", REQUIRED, None, "nifm says the console is online"),
    ("net.dns", REQUIRED, None, "the server's host resolves"),
    ("net.tls", REQUIRED, None, "a verified TLS request to the server answers"),
    ("net.tls13_only", OPTIONAL, None, "a TLS 1.3-only listener answers, when one is named"),
    ("net.handshake_leak", REQUIRED, None, "failed handshakes leave no socket behind (ENOENT)"),
    ("net.timeout", REQUIRED, None, "a connect to a black hole gives up inside its timeout"),
    ("net.connect_50", REQUIRED, None, "fifty requests in a row, no socket or heap growth"),
    # 4. Protocol, against a scratch folder.
    ("proto.pair", REQUIRED, None, "pairing by device code ends with a token"),
    ("proto.negotiate", REQUIRED, None, "a negotiate answers with a plan"),
    ("proto.tick", REQUIRED, None, "a full tick on the fixture save completes"),
    ("proto.download_resume", REQUIRED, None, "a download cut mid-way resumes with Range"),
    # 5. Footprint.
    ("footprint.heap", REQUIRED, None, "heap high-water mark, under the sysmodule's heap"),
)

RESULTS = ("pass", "fail", "skip", "na")
CONSOLE_FIELDS = ("hos", "ams", "emummc", "sd_format")


class Malformed(Exception):
    pass


def load(path: str) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        raise Malformed(f"cannot read it: {e.strerror}") from e
    try:
        report = json.loads(text)
    except ValueError as e:
        raise Malformed(f"not JSON ({e})") from e
    if not isinstance(report, dict):
        raise Malformed("not a JSON object")
    if report.get("schema") != SCHEMA:
        raise Malformed(f"schema {report.get('schema')!r}, this judge reads {SCHEMA}")
    console = report.get("console")
    if not isinstance(console, dict):
        raise Malformed("no console object")
    for field in CONSOLE_FIELDS:
        if field not in console:
            raise Malformed(f"console has no {field}")
    rows = report.get("rows")
    if not isinstance(rows, list):
        raise Malformed("no rows list")
    seen = {}
    for i, row in enumerate(rows):
        if not isinstance(row, dict) or not isinstance(row.get("id"), str):
            raise Malformed(f"row {i} has no id")
        if row.get("result") not in RESULTS:
            raise Malformed(f"row {row['id']} answers {row.get('result')!r}, not one of "
                            + "/".join(RESULTS))
        if row["id"] in seen:
            raise Malformed(f"row {row['id']} appears twice")
        seen[row["id"]] = row
    report["_by_id"] = seen
    return report


def judge_row(spec: tuple, row: dict, console: dict) -> bool:
    row_id, kind, na_unless, _ = spec
    result = row["result"]
    if result == "pass":
        return True
    if kind == OPTIONAL:
        return result in ("skip", "na")
    if result == "na" and na_unless is not None:
        return str(console.get("sd_format", "")).lower() != na_unless
    return False


def judge(path: str, report: dict) -> bool:
    console = report["console"]
    by_id = report["_by_id"]
    ok = True
    for spec in ROWS:
        row_id = spec[0]
        row = by_id.get(row_id)
        if row is None:
            print(f"MISSING  {row_id:<24}the report has no such row")
            ok = False
            continue
        passed = judge_row(spec, row, console)
        ok = ok and passed
        detail = str(row.get("detail", ""))
        print(f"{'PASS' if passed else 'FAIL':<8} {row_id:<24}{row['result']:<5} {detail}")
    known = {spec[0] for spec in ROWS}
    for row_id in by_id:
        if row_id not in known:
            # A newer NRO's row: shown, not judged -- the table above is what a
            # pass means, and widening it is a reviewed edit here.
            print(f"UNKNOWN  {row_id:<24}not a row this judge reads")
    print(f"VERDICT {'PASS' if ok else 'FAIL'} {path} ({pair_name(report)})")
    return ok


def pair(report: dict) -> tuple:
    return (str(report["console"]["hos"]), str(report["console"]["ams"]))


def pair_name(report: dict) -> str:
    hos, ams = pair(report)
    return f"HOS {hos} / Atmosphere {ams}"


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(
        description="Judge selftest reports (M12-1, #306).",
        usage="%(prog)s [--pairs N] REPORT.json... | --rows")
    parser.add_argument("--pairs", type=int, default=0,
                        help="require passing reports from at least N HOS/Atmosphere pairs")
    parser.add_argument("--rows", action="store_true", help="list the rows judged, and exit")
    parser.add_argument("reports", nargs="*")
    args = parser.parse_args(argv)

    if args.rows:
        for row_id, kind, na_unless, what in ROWS:
            extra = f", na unless {na_unless}" if na_unless else ""
            print(f"{row_id} {kind}{extra}: {what}")
        return 0
    if not args.reports:
        parser.print_usage(sys.stderr)
        return 2

    # Every report is parsed before any is judged: one malformed file refuses
    # the set, rather than a PASS printed above an error someone scrolls past.
    loaded = []
    malformed = False
    for path in args.reports:
        try:
            loaded.append((path, load(path)))
        except Malformed as e:
            print(f"MALFORMED {path}: {e}")
            malformed = True
    if malformed:
        return 2

    passing = []
    for path, report in loaded:
        if judge(path, report):
            passing.append(report)
        print()

    ok = len(passing) == len(loaded)
    if args.pairs:
        pairs = {pair(r) for r in passing}
        noun = "pair" if len(pairs) == 1 else "pairs"
        held = len(pairs) >= args.pairs
        print(f"{'PASS' if held else 'FAIL'}: {len(pairs)} HOS/Atmosphere {noun} passed, "
              f"{args.pairs} required")
        ok = ok and held
    print(f"VERDICT {'PASS' if ok else 'FAIL'} ({len(passing)} of {len(loaded)} reports passed)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
