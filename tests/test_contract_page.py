#!/usr/bin/env python3
"""Hold the pages that quote RomM to the RomM this client actually requires.

docs/API_CONTRACT.md describes one RomM: the floor, `server::kMinimumVersion`.
It spent a release describing 5.2.0 while the client refused 5.2.0 (#260), and
SYNC_PROTOCOL.md and AUTH.md went on citing 5.2.0's captures after it was
re-based (#266), because nothing connected a page to the floor, and
contract.captures could not notice -- it compares the captures against a server,
not a page against the captures.

So, with no network:

  - API_CONTRACT.md's H1 names exactly the floor's version, read out of
    core/include/rommsync/server_capabilities.hpp, so raising the floor turns
    this red until the page is re-based;
  - every capture a page cites exists in the floor's set. On API_CONTRACT.md a
    bare `captures/<name>.json` means server/contract/captures/<floor>/, which
    is what its header says. SYNC_PROTOCOL.md and AUTH.md make no such promise,
    so there -- and anywhere as `server/contract/captures/<name>.json` -- a bare
    one is the root's, 5.2.0's set kept as history, and a citation must name
    `captures/<floor>/`;
  - no page claims to have checked a claim against another version -- a live
    one, a fixture of one, its OpenAPI snapshot or its source -- outside a
    passage marked as history: a section (and its subsections) whose heading
    starts "History" or says "since <version>", or a paragraph or list item that
    says it is history. Fenced code is not prose and is not read.

Exit codes: 0 pass, 1 a page and the floor disagree.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

VERSION = r"\d+\.\d+\.\d+"

# (page, whether it is the contract page: titled with the floor's version, and
# promising that a bare `captures/<name>.json` means the floor's set)
PAGES = [
    ("API_CONTRACT.md", True),
    ("SYNC_PROTOCOL.md", False),
    ("AUTH.md", False),
]

CAPTURE = re.compile(r"(?:(?<![\w.])(server/contract/)|(?<![\w./]))captures/(?:(" + VERSION + r")/)?"
                     r"([a-z0-9][a-z0-9\-]*\.json)")

# The phrasings a page uses to say what a claim was checked against. Matched on
# whitespace-collapsed text, so a version wrapped onto the next line still counts.
VERIFIED_AGAINST = [
    re.compile(r"\blive (" + VERSION + r")\b"),
    re.compile(r"\b(" + VERSION + r") fixture\b"),
    re.compile(r"\bromm-openapi-(" + VERSION + r")\.json"),
    re.compile(r"\b(" + VERSION + r")'s (?:snapshot|source)\b"),
    re.compile(r"\.py`?,? in (" + VERSION + r")\b"),
]

# "sync history" is this domain's own term, so a paragraph marks itself only by
# saying what it is: "kept as history", "History only", "historical".
HISTORY = re.compile(r"\b(?:as|is) history\b|\bhistory only\b|\bhistorical(?:ly)?\b",
                     re.IGNORECASE)
HISTORY_HEADING = re.compile(r"^#+ history\b|\bhistorical\b|\bsince " + VERSION, re.IGNORECASE)
HEADING = re.compile(r"(#{1,6}) ")


def floor_version(repo: str) -> str | None:
    header = os.path.join(repo, "core", "include", "rommsync", "server_capabilities.hpp")
    with open(header, encoding="utf-8") as fh:
        match = re.search(r"\bkMinimumVersion\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", fh.read())
    return ".".join(match.groups()) if match else None


def current_paragraphs(page: str):
    """Yield the paragraphs and list items a page presents as current: not under a
    history heading, and not marking themselves as history."""
    history_level = 0  # the level of the history heading we are under, 0 if none
    in_fence = False
    paragraph: list[str] = []

    def flush():
        text = " ".join(" ".join(paragraph).split())
        paragraph.clear()
        return text if text and not history_level and not HISTORY.search(text) else None

    for line in page.splitlines():
        heading = None if in_fence else HEADING.match(line)
        if line.lstrip().startswith("```") or heading or not line.strip() or (
                not in_fence and re.match(r"\s*(?:[-*]|\d+\.) ", line)):
            if (text := flush()) is not None:
                yield text
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence or not line.strip():
            continue
        if heading:
            level = len(heading.group(1))
            if history_level and level > history_level:
                continue
            history_level = level if HISTORY_HEADING.search(line) else 0
            continue
        paragraph.append(line)
    if (text := flush()) is not None:
        yield text


def check_page(repo: str, version: str, name: str, contract: bool) -> tuple[list[str], int]:
    with open(os.path.join(repo, "docs", name), encoding="utf-8") as fh:
        page = fh.read()
    problems = []

    if contract:
        title = next((line for line in page.splitlines() if line.startswith("# ")), "")
        versions = re.findall(VERSION, title)
        if versions != [version]:
            problems.append(f"the title names {versions or 'no version'}, and the floor is "
                            f"{version}: {title!r}")

    captures = os.path.join(repo, "server", "contract", "captures", version)
    cited = sorted(set(CAPTURE.findall(page)))
    if contract and not cited:
        problems.append("the page cites no capture at all")
    for full_path, cited_version, capture in cited:
        if not cited_version and (full_path or not contract):
            problems.append(f"{full_path}captures/{capture} is the root set's, not {version}'s; "
                            f"cite captures/{version}/{capture}")
        elif cited_version and cited_version != version:
            problems.append(f"captures/{cited_version}/{capture} is {cited_version}'s, "
                            f"and the floor is {version}")
        elif not os.path.isfile(os.path.join(captures, capture)):
            problems.append(f"captures/{capture} is cited, and {version}'s captures have no such file")

    for paragraph in current_paragraphs(page):
        for pattern in VERIFIED_AGAINST:
            for match in pattern.finditer(paragraph):
                if match.group(1) != version:
                    problems.append(f"{match.group(0)!r} rests a current claim on "
                                    f"{match.group(1)}, outside a passage marked as history: "
                                    f"...{paragraph[max(0, match.start() - 60):match.end() + 20]}...")
    return problems, len({(v, c) for _, v, c in cited})


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    args = ap.parse_args()

    version = floor_version(args.repo)
    if version is None:
        print("kMinimumVersion not found in server_capabilities.hpp")
        return 1

    failed = False
    for name, contract in PAGES:
        problems, cited = check_page(args.repo, version, name, contract)
        if problems:
            failed = True
            print(f"docs/{name} does not describe RomM {version} ({len(problems)}):\n")
            for p in problems:
                print(f"  {p}")
            print()
        else:
            print(f"docs/{name} is RomM {version}'s, and its {cited} cited capture(s) exist")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
