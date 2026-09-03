#!/usr/bin/env python3
"""Generate the deterministic ROM fixtures that no real homebrew ROM provides.

Two cases in the backlog cannot be covered by a downloaded ROM:

* **Range resume (M3-3)** needs a file large enough that a download can be
  interrupted and resumed part-way. Real homebrew ROMs are tens of kilobytes.
* **Multi-file roms (M3-4)** needs a rom directory holding several files, so the
  client's ``has_multiple_files`` skip path can be exercised.

Both are generated from a fixed seed, so every machine and every run produces
byte-identical files and therefore stable SHA1/SHA256 hashes.
"""

from __future__ import annotations

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


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(f"usage: {argv[0]} <large-file> <size-mb> <multifile-dir>", file=sys.stderr)
        return 2
    make_large(Path(argv[1]), int(argv[2]))
    make_multifile(Path(argv[3]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
