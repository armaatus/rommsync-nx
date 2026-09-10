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

Four phases, one CTest entry each, so a red run names what broke:

  rows      every row's hex and KiB agree, and the terms sum to the peak row.
  pinned    every row's constant exists, is summed into `kHeapPeak`, and is
            pinned to the row's byte count by a `static_assert`.
  margin    the heap minus the peak is the one margin the prose states, and it
            states it once. Two margins is how #207's prose was wrong twice.
  dirs      `__nx_fsdev_direntry_cache_size` is set deliberately rather than
            left at libnx's 32, which costs 784 bytes per cached entry per
            open `DIR` out of this heap (fs_dev.c).
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

# `constexpr size_t kInnerHeapSize = 0x100000;`
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


PHASES = {
    "rows": phase_rows,
    "pinned": phase_pinned,
    "margin": phase_margin,
    "dirs": phase_dirs,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=sorted(PHASES))
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()
    return PHASES[args.phase](args.repo.rstrip("/"))


if __name__ == "__main__":
    sys.exit(main())
