#!/usr/bin/env python3
"""Hold docs/API_CONTRACT.md to the RomM this client actually requires.

The page describes one RomM: the floor, `server::kMinimumVersion`. It spent a
release describing 5.2.0 while the client refused 5.2.0 (#260), because nothing
connected the two, and contract.captures could not notice -- it compares the
captures against a server, not the page against the captures.

So two things are checked, with no network:

  - the page's H1 names exactly the floor's version, read out of
    core/include/rommsync/server_capabilities.hpp, so raising the floor turns
    this red until the page is re-based;
  - every `captures/<name>.json` the page cites exists under
    server/contract/captures/<floor>/, which is what the page's own header says
    that path means. A citation of a capture the floor's set does not have is a
    claim resting on another version's server.

Exit codes: 0 pass, 1 the page and the floor disagree.
"""

from __future__ import annotations

import argparse
import os
import re
import sys


def floor_version(repo: str) -> str | None:
    header = os.path.join(repo, "core", "include", "rommsync", "server_capabilities.hpp")
    with open(header, encoding="utf-8") as fh:
        match = re.search(r"\bkMinimumVersion\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", fh.read())
    return ".".join(match.groups()) if match else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    args = ap.parse_args()

    version = floor_version(args.repo)
    if version is None:
        print("kMinimumVersion not found in server_capabilities.hpp")
        return 1

    with open(os.path.join(args.repo, "docs", "API_CONTRACT.md"), encoding="utf-8") as fh:
        page = fh.read()
    problems = []

    title = next((line for line in page.splitlines() if line.startswith("# ")), "")
    versions = re.findall(r"\d+\.\d+\.\d+", title)
    if versions != [version]:
        problems.append(f"the title names {versions or 'no version'}, and the floor is "
                        f"{version}: {title!r}")

    captures = os.path.join(args.repo, "server", "contract", "captures", version)
    cited = sorted(set(re.findall(r"(?<![\w/.])captures/([a-z0-9][a-z0-9\-]*\.json)", page)))
    if not cited:
        problems.append("the page cites no capture at all")
    for name in cited:
        if not os.path.isfile(os.path.join(captures, name)):
            problems.append(f"captures/{name} is cited, and {version}'s captures have no such file")

    if problems:
        print(f"docs/API_CONTRACT.md does not describe RomM {version} ({len(problems)}):\n")
        for p in problems:
            print(f"  {p}")
        return 1
    print(f"docs/API_CONTRACT.md is RomM {version}'s, and its {len(cited)} cited capture(s) exist")
    return 0


if __name__ == "__main__":
    sys.exit(main())
