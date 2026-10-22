#!/usr/bin/env python3
"""The inner heap's table, its constants and its prose, checked against each other (M9-2, #207).

`sysmodule/source/main.cpp` sizes `g_inner_heap` from a table of terms written
in a comment. Nothing executed that table, so it drifted from the code three
ways at once (#207): two `std::thread`s were budgeted at 32 KiB each and cost
128 KiB each, an open `DIR` cost ~24 KiB that had no term at all, and the two
margins the prose quoted -- 94 KiB and 78 KiB -- were neither of them the
number the arithmetic gives. The `static_assert` was right and passed; the
prose is what everyone reads.

So the table is now written in a form a script can add up, every row names the
`constexpr` it stands for, and each of those constants is pinned to the row's
byte count by a `static_assert`. That closes the loop in both directions: the
compiler catches a constant that moved away from its row, and this catches a
row -- or a margin -- that moved away from the arithmetic.

Five phases, one CTest entry each, so a red run names what broke:

  rows      every row's hex and KiB agree, and the terms sum to the peak row.
  pinned    every row's constant exists, is summed into `kHeapPeak`, and is
            pinned to the row's byte count by a `static_assert`.
  margin    the heap minus the peak is the one margin the prose states, and it
            states it once. Two margins is how #207's prose was wrong twice.
  dirs      `__nx_fsdev_direntry_cache_size` is set deliberately rather than
            left at libnx's 32, which costs 784 bytes per cached entry per
            open `DIR` out of this heap (fs_dev.c).
  prose     the rest of the tree quotes the heap's size as `kInnerHeapSize`
            says, in hex or in MiB/KiB.
"""
import argparse
import re
import sys

MAIN = "sysmodule/source/main.cpp"

# A row of the table in the comment above `kInnerHeapSize`:
#   | term                 | `kHeapLogTail` | 0x1800 | 6 KiB |
ROW = re.compile(
    r"^//\s*\|\s*(?P<term>[^|]*?)\s*\|\s*`(?P<constant>k\w+)`\s*\|"
    r"\s*(?P<bytes>0x[0-9A-Fa-f]+)\s*\|\s*(?P<kib>\d+) KiB\s*\|\s*$")

# The one sentence that states what is left over. Written once, on purpose.
MARGIN = re.compile(r"leaves (?P<bytes>0x[0-9A-Fa-f]+) -- (?P<kib>\d+) KiB")

# `constexpr size_t kInnerHeapSize = 0x150000;`
HEAP = re.compile(r"constexpr\s+size_t\s+kInnerHeapSize\s*=\s*(0x[0-9A-Fa-f]+)\s*;")

# `static_assert(kHeapLogTail == 0x1800,`
PINNED = r"static_assert\s*\(\s*%s\s*==\s*(0x[0-9A-Fa-f]+)\s*,"

# `constexpr size_t kHeapPeak = kHeapA + kHeapB + ...;`
PEAK_SUM = re.compile(r"constexpr\s+size_t\s+kHeapPeak\s*=(.*?);", re.S)


def read(repo, relative):
    with open(repo + "/" + relative, "r", encoding="utf-8") as handle:
        return handle.read()


def fail(message):
    print("FAIL: " + message)
    return 1


def rows(source):
    """The table's term rows and its `**peak**` row, in source order."""
    found = [m.groupdict() for m in (ROW.match(line) for line in source.splitlines()) if m]
    for row in found:
        row["bytes"] = int(row["bytes"], 16)
        row["kib"] = int(row["kib"])
    peak = [r for r in found if r["constant"] == "kHeapPeak"]
    terms = [r for r in found if r["constant"] != "kHeapPeak"]
    return terms, (peak[0] if peak else None)


def phase_rows(repo):
    source = read(repo, MAIN)
    terms, peak = rows(source)
    if not terms:
        return fail("no heap table rows in %s; the table above kInnerHeapSize is what "
                    "sizes this process's whole heap and it has to stay machine-readable "
                    "(#207)" % MAIN)
    if peak is None:
        return fail("the heap table has no `kHeapPeak` row (#207)")
    failures = 0
    for row in terms + [peak]:
        if row["bytes"] % 1024 != 0 or row["bytes"] // 1024 != row["kib"]:
            failures += fail("heap term `%s` says %s and %d KiB, which are not the same "
                             "number (#207)" % (row["constant"], hex(row["bytes"]), row["kib"]))
    total = sum(row["bytes"] for row in terms)
    if total != peak["bytes"]:
        failures += fail("the heap table's %d terms sum to %s (%d KiB) and the peak row "
                         "says %s (%d KiB) (#207)"
                         % (len(terms), hex(total), total // 1024,
                            hex(peak["bytes"]), peak["kib"]))
    if failures:
        return 1
    print("ok: %d heap terms sum to the stated peak of %s (%d KiB)"
          % (len(terms), hex(total), total // 1024))
    return 0


def phase_pinned(repo):
    source = read(repo, MAIN)
    terms, peak = rows(source)
    if not terms or peak is None:
        return fail("the heap table is missing or has no peak row (#207)")
    summed = PEAK_SUM.search(source)
    if summed is None:
        return fail("no `constexpr size_t kHeapPeak = ...` in %s; the table has to be "
                    "added up by the compiler, not only by a reader (#207)" % MAIN)
    addends = set(re.findall(r"\bk\w+\b", summed.group(1)))
    failures = 0
    for row in terms + [peak]:
        pin = re.search(PINNED % row["constant"], source)
        if pin is None:
            failures += fail("heap term `%s` has no `static_assert(%s == %s, ...)` pinning "
                             "it to its row; without one the table can drift from the code "
                             "again (#207)" % (row["constant"], row["constant"],
                                               hex(row["bytes"])))
        elif int(pin.group(1), 16) != row["bytes"]:
            failures += fail("heap term `%s` is pinned to %s and its row says %s (#207)"
                             % (row["constant"], pin.group(1), hex(row["bytes"])))
        if row is not peak and row["constant"] not in addends:
            failures += fail("heap term `%s` has a row in the table and is not summed into "
                             "kHeapPeak (#207)" % row["constant"])
    for addend in sorted(addends):
        if addend.startswith("kHeap") and addend not in {r["constant"] for r in terms}:
            failures += fail("kHeapPeak sums `%s`, which has no row in the table (#207)"
                             % addend)
    if failures:
        return 1
    print("ok: %d heap terms are summed into kHeapPeak and pinned to their rows"
          % len(terms))
    return 0


def phase_margin(repo):
    source = read(repo, MAIN)
    _, peak = rows(source)
    heap = HEAP.search(source)
    if heap is None:
        return fail("no `constexpr size_t kInnerHeapSize = 0x...;` in %s (#207)" % MAIN)
    # Counted before the table is looked at, because a second margin sentence is
    # a defect on its own: the two #207 found disagreed with the arithmetic and
    # with each other, and a reader believes whichever one they reach first.
    stated = MARGIN.findall(source)
    if len(stated) != 1:
        return fail("%s states %d margins over the heap's peak. There is one margin, and "
                    "a second sentence quoting a different one is how #207's prose was "
                    "wrong twice -- 94 KiB and 78 KiB, against an arithmetic answer of "
                    "62 KiB and 46 KiB" % (MAIN, len(stated)))
    if peak is None:
        return fail("the heap table has no `kHeapPeak` row (#207)")
    size = int(heap.group(1), 16)
    margin = size - peak["bytes"]
    said_bytes, said_kib = int(stated[0][0], 16), int(stated[0][1])
    if said_bytes != margin or said_kib != margin // 1024:
        return fail("the prose says the heap leaves %s (%d KiB) over the peak; "
                    "kInnerHeapSize %s minus the peak %s is %s (%d KiB) (#207)"
                    % (hex(said_bytes), said_kib, hex(size), hex(peak["bytes"]),
                       hex(margin), margin // 1024))
    if margin <= 0:
        return fail("the heap's peak does not fit in kInnerHeapSize (#207)")
    print("ok: kInnerHeapSize %s leaves %s (%d KiB) over the peak, said once"
          % (hex(size), hex(margin), margin // 1024))
    return 0


def phase_dirs(repo):
    source = read(repo, MAIN)
    found = re.search(r"__nx_fsdev_direntry_cache_size\s*=\s*(\w+)\s*;", source)
    if found is None:
        return fail("%s does not set __nx_fsdev_direntry_cache_size; libnx defaults it to "
                    "32 and fs_dev.c allocates sizeof(FsDirectoryEntry) -- 784 bytes -- "
                    "times that per open DIR, so every ::opendir in card.cpp takes ~24 KiB "
                    "off this heap for its lifetime (#207)" % MAIN)
    # Written as a named constant rather than a bare number, so the table's
    # open-directory term and the override cannot say different things.
    value = found.group(1)
    if not value.isdigit():
        named = re.search(r"constexpr\s+u32\s+" + value + r"\s*=\s*(\d+)\s*;", source)
        if named is None:
            return fail("__nx_fsdev_direntry_cache_size is set to `%s`, which is not a "
                        "`constexpr u32` in %s (#207)" % (value, MAIN))
        value = named.group(1)
    if int(value) >= 32:
        return fail("__nx_fsdev_direntry_cache_size is %s, which is libnx's default or worse; "
                    "that is 784 * %s bytes per open DIR out of this heap (#207)"
                    % (value, value))
    # Zero is the cheapest value and it breaks every directory scan silently:
    # fsdev asks `fsDirRead` for zero entries, gets zero back, and the first
    # `readdir` reports end-of-directory. Every save folder then scans as empty
    # and nothing ever syncs, with no error anywhere (#207).
    if int(value) < 1:
        return fail("__nx_fsdev_direntry_cache_size is %s; fsdev then reads zero entries per "
                    "fsDirRead and every directory scans as empty, which is a console that "
                    "syncs nothing and says nothing (#207)" % value)
    print("ok: __nx_fsdev_direntry_cache_size is %s rather than libnx's 32, or %d bytes "
          "per open DIR" % (value, 784 * int(value)))
    return 0


# A line that talks about the heap, and the hex literals on it. The window is
# deliberately narrow: `kHeapStateBaseline` (0x40000) and `kHeapListResponse`
# (0x7D000) are legitimately quoted beside the heap and are not it, while every
# value `kInnerHeapSize` has ever held -- 0x80000, 0xC0000, 0x100000, 0x150000,
# 0x160000, 0x180000 -- sits inside it.
HEAP_LINE = re.compile(r"inner heap|kInnerHeapSize|[Tt]he heap is")
HEX = re.compile(r"0x[0-9A-Fa-f]+")
HEAP_SIZE_MIN = 0x80000
HEAP_SIZE_MAX = 0x400000

# Where prose about the heap lives. `main.cpp` is excluded because the first
# three phases are already the authority on it, and vendored trees because their
# comments are upstream's.
PROSE_ROOTS = ("core", "sysmodule", "docs", "tests", "host", "scripts")
PROSE_SUFFIXES = (".hpp", ".cpp", ".md", ".py", ".sh")
# This file names every value the constant has ever held, one sentence above.
# `scripts/fleet/` is vendored autofleet, which knows nothing of this heap.
PROSE_EXCLUDED = ("sysmodule/source/main.cpp", "tests/test_heap_budget.py",
                  "overlay/lib", "/lib/", "/build/", "scripts/fleet/")

# The same size in words: "a 1.4 MiB heap", "the inner heap is 1.4 MiB", "the
# 768 KiB inner heap". The hex check above never saw these, so when #207 and
# #246 moved the heap -- 0xC0000, 0x150000, 0x160000, 0x180000 -- the sentences
# that said "512 KiB", "768 KiB" and "1.4 MiB" each outlived the size they
# named, and #206 found a dozen of them. Matched across a wrapped comment line,
# because that is where "heap is" and its number most often part. "~1.4 MiB left
# after the socket transfer memory" is a different quantity and is not matched.
SIZE_WORDS = r"~?(?P<n%d>\d+(?:\.\d+)?) (?P<unit%d>MiB|KiB)"
HEAP_WORDS = re.compile(
    SIZE_WORDS % (1, 1) + r"(?: inner)? heap\b"
    r"|\bheap (?:is|of) " + SIZE_WORDS % (2, 2))
COMMENT_LEAD = re.compile(r"^\s*(?:///?|#+|\*)?\s*")


def heap_words(lines):
    """Each size quoted for the heap in words, as (line, text, bytes)."""
    import bisect
    starts = []
    joined = []
    offset = 0
    for line in lines:
        text = COMMENT_LEAD.sub("", line, count=1)
        starts.append(offset)
        joined.append(text)
        offset += len(text) + 1
    found = []
    for match in HEAP_WORDS.finditer(" ".join(joined)):
        number = match.group("n1") or match.group("n2")
        unit = match.group("unit1") or match.group("unit2")
        scale = 1024 * 1024 if unit == "MiB" else 1024
        line = bisect.bisect_right(starts, match.start())
        found.append((line, match.group(0), float(number) * scale))
    return found


def phase_prose(repo):
    import os
    source = read(repo, MAIN)
    heap = HEAP.search(source)
    if heap is None:
        return fail("no `constexpr size_t kInnerHeapSize = 0x...;` in %s (#207)" % MAIN)
    size = int(heap.group(1), 16)
    failures = 0
    scanned = 0
    for root in PROSE_ROOTS:
        for base, _, names in os.walk(os.path.join(repo, root)):
            for name in names:
                path = os.path.join(base, name)
                relative = os.path.relpath(path, repo)
                if not relative.endswith(PROSE_SUFFIXES):
                    continue
                if any(skip in "/" + relative for skip in PROSE_EXCLUDED):
                    continue
                scanned += 1
                try:
                    lines = open(path, "r", encoding="utf-8").read().splitlines()
                except (UnicodeDecodeError, OSError):
                    continue
                for number, line in enumerate(lines, 1):
                    if not HEAP_LINE.search(line):
                        continue
                    for token in HEX.findall(line):
                        value = int(token, 16)
                        if not HEAP_SIZE_MIN <= value <= HEAP_SIZE_MAX:
                            continue
                        if value != size:
                            failures += fail(
                                "%s:%d says the heap is %s and %s says it is %s. "
                                "Nothing but this phase reads that sentence, and a bound "
                                "sized against it would be off by %d KiB (#207)"
                                % (relative, number, token, MAIN, hex(size),
                                   abs(value - size) // 1024))
                # To a tenth of a MiB, or to the KiB: "1.5 MiB" is right for
                # 0x180000, "1.4 MiB" is 0x160000's and is not. Outside the hex
                # check's window it is some other heap -- a test's fake 4 KiB one.
                for number, text, stated in heap_words(lines):
                    if not HEAP_SIZE_MIN <= stated <= HEAP_SIZE_MAX:
                        continue
                    precision = 0.05 * 1024 * 1024 if "MiB" in text else 512
                    if abs(stated - size) > precision:
                        failures += fail(
                            "%s:%d says \"%s\", and %s says the heap is %s (%d KiB). "
                            "A size in words outlives the hex it was read from (#206)"
                            % (relative, number, text, MAIN, hex(size), size // 1024))
    if failures:
        return 1
    print("ok: %d files scanned, none quoting a heap size other than %s"
          % (scanned, hex(size)))
    return 0


PHASES = {
    "rows": phase_rows,
    "pinned": phase_pinned,
    "margin": phase_margin,
    "dirs": phase_dirs,
    "prose": phase_prose,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=sorted(PHASES))
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()
    return PHASES[args.phase](args.repo.rstrip("/"))


if __name__ == "__main__":
    sys.exit(main())
