#!/usr/bin/env python3
"""Generate the deterministic ROM fixtures that no real homebrew ROM provides.

Two cases in the backlog cannot be covered by a downloaded ROM:

* **Range resume (M3-3)** needs a file large enough that a download can be
  interrupted and resumed part-way. Real homebrew ROMs are tens of kilobytes.
* **Multi-file roms (M3-4)** needs a rom directory holding several files, so the
  client's ``has_multiple_files`` skip path can be exercised -- and, beside it, a
  rom directory holding exactly *one* file, which RomM reports as
  ``has_nested_single_file`` with ``has_multiple_files: false``. That one is a
  normal download, and it is what proves the skip does not over-trigger on every
  rom that happens to be a directory.

Both are generated from a fixed seed, so every machine and every run produces
byte-identical files and therefore stable SHA1/SHA256 hashes.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

SEED = 0x524F4D4D  # "ROMM"
MIB = 1024 * 1024


def make_large(path: Path, size_mb: int) -> None:
    """Write a deterministic file of ``size_mb`` MiB.

    A single random 1 MiB block is reused, but each block is prefixed with its
    index so no two blocks are identical -- a resumed download that stitches the
    wrong offset together produces a hash mismatch rather than passing by luck.
    """
    block = random.Random(SEED).randbytes(MIB)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as fh:
        for index in range(size_mb):
            fh.write(index.to_bytes(8, "big") + block[8:])


def make_multifile(directory: Path) -> None:
    """Write a rom directory holding two discs."""
    directory.mkdir(parents=True, exist_ok=True)
    for disc in (1, 2):
        name = f"{directory.name} (Disc {disc}).bin"
        (directory / name).write_bytes(f"synthetic disc {disc} fixture\n".encode())


def make_nested(directory: Path) -> None:
    """Write a rom directory holding exactly one file.

    RomM reports this as ``has_nested_single_file: true`` with
    ``has_multiple_files: false``, and serves the file itself -- not a zip --
    from the whole-rom ``content`` endpoint. The client downloads it like any
    other rom; the fixture exists so that "like any other rom" is asserted rather
    than assumed.
    """
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f"{directory.name}.bin").write_bytes(
        b"synthetic nested single-file fixture\n")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--multi", required=True, type=Path,
                        help="rom directory to fill with a two-disc set")
    parser.add_argument("--nested", required=True, type=Path,
                        help="rom directory to fill with a single nested file")
    parser.add_argument("--large", type=Path,
                        help="large fixture to write; omit to generate only the rom directories")
    parser.add_argument("--size-mb", type=int, default=120)
    args = parser.parse_args(argv[1:])

    if args.large is not None:
        make_large(args.large, args.size_mb)
    make_multifile(args.multi)
    make_nested(args.nested)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
