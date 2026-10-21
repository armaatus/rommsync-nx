#!/usr/bin/env python3
"""Keep server/contract/captures/ honest against a live RomM.

The captures are the shapes docs/API_CONTRACT.md, AUTH.md and SYNC_PROTOCOL.md
quote and the ones M1/M2 will be coded against. A committed capture is only
worth anything if it still matches the server, so this re-runs the probe against
the docker fixture and compares.

It compares *shape*, not values: field names, nesting, and JSON types, plus the
`action`/`reason` pairs each negotiate scenario is named for. Ids, timestamps and
the per-run slot names differ every run and say nothing about the contract.

Which captures depends on which RomM the rig runs (#227): ROMM_VERSION, from the
environment or else .env. 5.2.0's are server/contract/captures/ itself, kept as
history; every later version's are server/contract/captures/<version>/. The
server is asked its version too, and a rig running some other RomM is a failure,
not drift: comparing against the wrong snapshot proves nothing either way.

Exit codes: 0 pass, 1 drift, 77 skipped (CTest's SKIP_RETURN_CODE) when the rig
is not up or not provisioned.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

SKIP = 77


def venv_python(repo: str) -> str:
    """Prefer the worktree venv, which is where requests is installed.

    Resolved here rather than in CMake because setup.sh creates the venv and
    configures the build in whichever order it happens to run; a path baked in
    at configure time can be the one that does not exist yet.
    """
    candidate = os.path.join(repo, ".venv", "bin", "python")
    return candidate if os.path.exists(candidate) else sys.executable


def read_env(path: str) -> dict:
    try:
        with open(path, encoding="utf-8") as fh:
            return dict(
                line.rstrip("\n").split("=", 1)
                for line in fh
                if "=" in line and not line.startswith("#")
            )
    except OSError:
        return {}


# The version server/contract/captures/ itself was captured from. Every other
# version's captures live in a subdirectory named after it.
ROOT_CAPTURES_VERSION = "5.2.0"


def captures_dir(repo: str, version: str) -> str:
    root = os.path.join(repo, "server", "contract", "captures")
    return root if version == ROOT_CAPTURES_VERSION else os.path.join(root, version)


def shape(obj):
    """Reduce a value to the part of it that is a contract.

    `None` becomes a wildcard: a nullable field that happens to be null in one
    run and set in another is not drift, and every RomM schema here is full of
    them. An empty list is a wildcard for the same reason.
    """
    if obj is None:
        return None
    if isinstance(obj, dict):
        return {k: shape(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return ["list", shape(obj[0])] if obj else None
    if isinstance(obj, bool):
        return "bool"
    if isinstance(obj, int):
        return "int"
    if isinstance(obj, float):
        return "float"
    return "str"


def is_list(s) -> bool:
    return isinstance(s, list) and len(s) == 2 and s[0] == "list"


def diff(expected, actual, path: str, out: list) -> None:
    if expected is None or actual is None:
        return
    if is_list(expected) and is_list(actual):
        # Recurse into the element shape rather than comparing the two lists
        # whole, so a drifted field is named instead of dumping both schemas.
        diff(expected[1], actual[1], f"{path}[]", out)
        return
    if isinstance(expected, dict) and isinstance(actual, dict):
        for key in sorted(set(expected) - set(actual)):
            out.append(f"{path}.{key}: in the capture, gone from the server")
        for key in sorted(set(actual) - set(expected)):
            out.append(f"{path}.{key}: new on the server, missing from the capture")
        for key in sorted(set(expected) & set(actual)):
            diff(expected[key], actual[key], f"{path}.{key}", out)
        return
    if expected != actual:
        out.append(f"{path}: capture has {expected!r}, server sent {actual!r}")


def operation_reasons(body) -> set:
    """The (action, reason) pairs a negotiate response carries."""
    if not isinstance(body, dict):
        return set()
    return {(op.get("action"), op.get("reason")) for op in body.get("operations", [])}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--scratch", required=True)
    args = ap.parse_args()

    env = read_env(os.path.join(args.repo, ".env"))
    version = os.environ.get("ROMM_VERSION") or env.get("ROMM_VERSION") or ROOT_CAPTURES_VERSION
    committed = captures_dir(args.repo, version)
    base = env.get("ROMM_BASE_URL")
    fixture_auth = os.path.join(args.repo, "server", "testing", "fixture-auth.env")

    if not base:
        print("no ROMM_BASE_URL in .env; run ./scripts/orca/env.sh -- skipping")
        return SKIP
    if not os.path.exists(fixture_auth):
        print("no fixture-auth.env; the fixture is not provisioned -- skipping")
        return SKIP
    try:
        with urllib.request.urlopen(f"{base}/api/heartbeat", timeout=5) as r:
            heartbeat = json.load(r)
    except (urllib.error.URLError, OSError, ValueError) as exc:
        print(f"RomM at {base} is not answering ({exc}) -- skipping")
        return SKIP
    running = (heartbeat.get("SYSTEM") or {}).get("VERSION")
    if running != version:
        print(f"RomM at {base} is {running}, and this run expects {version} (ROMM_VERSION).\n"
              f"Recreate the stack for {version} (docs/TESTING.md#choosing-the-romm-version), "
              f"or set ROMM_VERSION to what is running.")
        return 1
    if not os.path.isdir(committed):
        print(f"no captures for RomM {version}: {committed} does not exist")
        return 1

    python = venv_python(args.repo)
    # Empty first: a leftover file from a previous run would stand in for one the
    # probe has stopped producing, which is exactly the drift this exists to see.
    if os.path.isdir(args.scratch):
        for stale in os.listdir(args.scratch):
            os.remove(os.path.join(args.scratch, stale))
    os.makedirs(args.scratch, exist_ok=True)
    probe = subprocess.run(
        [python, os.path.join(args.repo, "server", "probe_contract.py"),
         "--url", base, "--auth", "--negotiate", "--sync-scenarios",
         "--fixture-auth", fixture_auth, "--capture", args.scratch],
        cwd=args.repo, capture_output=True, text=True)
    if probe.returncode != 0:
        # A probe that cannot complete the flow is drift too -- that is what it
        # looks like when an endpoint or a payload field goes away.
        print(probe.stdout[-3000:])
        print(probe.stderr[-2000:], file=sys.stderr)
        print(f"\nprobe_contract.py exited {probe.returncode} against {base}")
        return 1

    problems = []
    names = sorted(f[:-5] for f in os.listdir(committed) if f.endswith(".json"))
    if not names:
        print(f"no captures in {committed}")
        return 1

    for name in names:
        fresh_path = os.path.join(args.scratch, f"{name}.json")
        if not os.path.exists(fresh_path):
            problems.append(f"{name}: committed, but the probe no longer produces it")
            continue
        with open(os.path.join(committed, f"{name}.json"), encoding="utf-8") as fh:
            expected = json.load(fh)
        with open(fresh_path, encoding="utf-8") as fh:
            actual = json.load(fh)
        diff(shape(expected), shape(actual), name, problems)

        # Wording is contract here: the docs quote these reasons to explain when
        # each action is chosen, and the capture files are named after them.
        # A subset check, because a fixture holding unrelated saves adds
        # operations these scenarios did not arrange.
        missing = operation_reasons(expected) - operation_reasons(actual)
        for action, reason in sorted(missing, key=str):
            problems.append(f"{name}: no operation {action!r} with reason {reason!r} any more")

    if problems:
        print(f"contract drift against {base} ({len(problems)}):\n")
        for p in problems:
            print(f"  {p}")
        print(f"\nRe-capture with:\n  . ./.env && {python} server/probe_contract.py "
              f'--url "$ROMM_BASE_URL" --auth --negotiate --sync-scenarios '
              f"--capture {os.path.relpath(committed, args.repo)}\n"
              f"and update the docs that quote them before trusting the new shapes.")
        return 1

    print(f"{len(names)} capture(s) for RomM {version} still match {base}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
