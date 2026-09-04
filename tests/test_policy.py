#!/usr/bin/env python3
"""The M0 exit gate's two mechanical clauses (docs/TESTING.md#the-m0-exit-gate).

The gate says no test in this suite needs a console or a production server. That
is a claim about every test, so it rots the moment someone adds one -- and the
way it rots is silent: a test pointed at a real RomM passes, and passing is
exactly what makes it look fine.

Three phases, run as separate CTest entries so a red run names which clause broke:

  loopback   every base URL the suite is CONFIGURED with is loopback -- the one
             compiled into the test binaries, the ones env.sh derives (RomM, the
             fault proxy, and the TLS terminator the M0-1 probe aims at), the
             fallback in tests/CMakeLists.txt, and this worktree's .env. Not a
             scan for URL literals: test_auth_shapes and test_token_store parse
             `http://romm.lan:8080` as data and never dial it, so the literal is
             not the thing that matters. What is configured is.

  guards     the two scripts that take a --base-url and write to it REFUSE a
             non-loopback one. Asserted on the refusal, not on a non-zero exit:
             a probe that tries to reach a production RomM and fails to connect
             also exits non-zero, and that is the opposite of the guarantee.

  host_only  every registered CTest command is a host executable in this tree,
             and no test command names a non-loopback URL, an emulator or a
             remote transport. It cannot prove a test does not open a socket to
             the internet from inside its own code -- `loopback` covers the
             configured URLs and code review covers the rest -- but it does
             catch the shape the policy is actually at risk from: a test that
             shells out to a console, an emulator, or a real server. Building or
             inspecting a .nsp/.ovl on the host is fine; running one is not.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.parse

SKIP = 77
# urlparse().hostname strips the brackets from an IPv6 authority, so "::1"
# is the form that reaches here, never "[::1]".
LOOPBACK = ("127.0.0.1", "localhost", "::1")

# TEST-NET-1 (RFC 5737): reserved for documentation, routed nowhere. If a guard
# ever fails to fire, this is the address it fails to reach.
REMOTE = "http://192.0.2.1"


def fail(msg: str) -> int:
    print(f"FAIL: {msg}", file=sys.stderr)
    return 1


def is_loopback(url: str) -> bool:
    host = urllib.parse.urlparse(url).hostname or ""
    return host in LOOPBACK


# --- loopback ---------------------------------------------------------------

def phase_loopback(args) -> int:
    checks: list[tuple[str, str]] = [
        ("compiled into the test binaries", args.proxy_url),
    ]

    # The environment beats the compiled-in value: rig::BaseUrl() (tests/rig.hpp)
    # and test_rig_smoke.cpp both prefer $PROXY_BASE_URL, so a shell that
    # exported a remote one would point every http.*, pair.* and rig.* test at
    # it while every file this function reads still said 127.0.0.1.
    for var in ("PROXY_BASE_URL", "ROMM_BASE_URL", "TLS_BASE_URL"):
        value = os.environ.get(var)
        if value:
            checks.append((f"${var} in this environment", value))

    # This worktree's .env, if it has one. A plain clone has not run env.sh yet.
    env_file = os.path.join(args.repo, ".env")
    if os.path.exists(env_file):
        with open(env_file, encoding="utf-8") as fh:
            for line in fh:
                key, sep, value = line.strip().partition("=")
                if sep and key in ("ROMM_BASE_URL", "PROXY_BASE_URL", "TLS_BASE_URL"):
                    checks.append((f".env {key}", value))

    # ...and the two places a .env comes from, so a fresh worktree is covered
    # before it has one.
    # Matched on the variable, not on "any URL on the line": these files carry
    # comments with links in them, and a doc link failing the policy check would
    # teach people to delete the check.
    assignment = re.compile(r'(?:ROMM|PROXY|TLS)_BASE_URL[=\s"]+(https?://[^\s"\')]*)')
    for path in ("scripts/orca/env.sh", "tests/CMakeLists.txt"):
        full = os.path.join(args.repo, path)
        found = 0
        with open(full, encoding="utf-8") as fh:
            for n, line in enumerate(fh, 1):
                for url in assignment.findall(line):
                    checks.append((f"{path}:{n}", url))
                    found += 1
        if not found:
            return fail(f"{path} no longer assigns a base URL this check can see; "
                        "the check has stopped checking, which is worse than a red one")

    # And the far end of every UPSTREAM in the compose file -- the fault proxy's,
    # and the TLS terminator's -- which is where the traffic actually lands. The
    # host is a compose service name rather than an address, so is_loopback()
    # cannot judge it: what makes it safe is that it names the `romm` service in
    # this project's own compose file, and nothing else.
    compose = os.path.join(args.repo, "server/testing/docker-compose.yml")
    upstreams = re.findall(r"^\s*UPSTREAM:\s*(\S+)", open(compose, encoding="utf-8").read(),
                           re.MULTILINE)
    if not upstreams:
        return fail("server/testing/docker-compose.yml no longer sets UPSTREAM; the fault "
                    "proxy's far end is now unchecked")
    for upstream in upstreams:
        host = urllib.parse.urlparse(upstream).hostname or ""
        if host not in ("romm", *LOOPBACK):
            return fail(f"the fault proxy forwards to {upstream}, which is neither this "
                        "project's `romm` service nor loopback. Every http.*, pair.* and "
                        "rig.* test writes to whatever is on the far end of that proxy.")
        print(f"  ok  {upstream}  (fault proxy upstream)")

    bad = [(where, url) for where, url in checks if not is_loopback(url)]
    for where, url in bad:
        print(f"  not loopback: {url}  ({where})", file=sys.stderr)
    if bad:
        return fail(f"{len(bad)} base URL(s) the suite is configured with are not loopback. "
                    "The suite may only talk to a throwaway fixture -- CLAUDE.md hard rule 1.")

    for where, url in checks:
        print(f"  ok  {url}  ({where})")
    print(f"{len(checks)} configured URL(s), all loopback, plus the proxy upstream")
    return 0


# --- guards -----------------------------------------------------------------

def find_python(repo: str) -> str | None:
    """An interpreter that can import the scripts under test."""
    for candidate in (os.path.join(repo, ".venv/bin/python"), sys.executable):
        if not candidate or not os.path.exists(candidate):
            continue
        # Both imports, not just requests: provision.py imports socketio at
        # module scope, and an interpreter with only half of them dies with a
        # ModuleNotFoundError that this test would have reported as "the guard
        # does not refuse" -- a red run pointing at the wrong file.
        probe = subprocess.run([candidate, "-c", "import requests, socketio"],
                               capture_output=True, timeout=60)
        if probe.returncode == 0:
            return candidate
    return None


def phase_guards(args) -> int:
    python = find_python(args.repo)
    if python is None:
        print("no interpreter with `requests`; run scripts/orca/setup.sh")
        return SKIP

    invocations = [
        # One per writing mode, because the guard is a condition over three flags
        # and dropping any one of them from it leaves this test green.
        ("probe_contract.py --auth", ["server/probe_contract.py", "--url", REMOTE, "--auth"]),
        ("probe_contract.py --negotiate",
         ["server/probe_contract.py", "--url", REMOTE, "--negotiate"]),
        ("probe_contract.py --sync-scenarios",
         ["server/probe_contract.py", "--url", REMOTE, "--sync-scenarios"]),
        ("provision.py", ["server/testing/provision.py", "--base-url", REMOTE]),
    ]

    failures = []
    for name, argv in invocations:
        try:
            # Short, because refusing is not supposed to involve the network:
            # anything that gets as far as a connect attempt to TEST-NET-1 hangs
            # here rather than passing slowly.
            done = subprocess.run([python, *argv], cwd=args.repo,
                                  capture_output=True, text=True, timeout=15)
        except subprocess.TimeoutExpired:
            failures.append(f"{name}: no refusal within 15s -- it tried to reach {REMOTE}")
            continue
        stderr = done.stderr.strip()
        if "refusing" not in stderr.lower():
            failures.append(f"{name}: exit {done.returncode}, and stderr does not refuse: {stderr[:200]!r}")
        elif done.returncode == 0:
            failures.append(f"{name}: refused on stderr but exited 0")
        else:
            print(f"  ok  {name} -> exit {done.returncode}: {stderr.splitlines()[0]}")

    for line in failures:
        print(f"  {line}", file=sys.stderr)
    if failures:
        return fail("a script that writes to a RomM does not refuse a non-loopback URL. "
                    "That guard is what stops one mistyped --base-url from reaching a "
                    "real library -- CLAUDE.md hard rule 1.")
    return 0


# --- host_only --------------------------------------------------------------

# Things a test command may not name. Not a blocklist of everything dangerous --
# it is the set of ways a test would reach off this machine, which is what the
# gate forbids.
#
# Deliberately NOT here: .nsp, .ovl and .nro. Building a Switch artifact on the
# host and asserting something about the file is host-local work, and M0-3 is
# going to want exactly that when it finishes the devkitPro job. What the gate
# forbids is *dispatching* to a device or an emulator, which is what the names
# below catch -- and a test whose argv[0] is itself a Horizon binary is already
# caught by the "not an executable on this machine" check.
FORBIDDEN = re.compile(
    r"(?:^|[/\s])(?:ssh|scp|adb|nxlink|nxdumptool|ryujinx|yuzu|sudachi|qemu[\w-]*)(?:$|[\s])"
    r"|/dev/tty|/dev/cu\.",
    re.IGNORECASE,
)


def phase_host_only(args) -> int:
    listing = subprocess.run([args.ctest, "--show-only=json-v1"],
                             cwd=args.build, capture_output=True, text=True, timeout=120)
    if listing.returncode != 0:
        return fail(f"ctest --show-only failed: {listing.stderr.strip()[:300]}")
    tests = json.loads(listing.stdout)["tests"]
    if not tests:
        return fail("ctest reports no tests at all; this phase would pass vacuously")

    failures = []
    for test in tests:
        name = test["name"]
        argv = test.get("command", [])
        if not argv:
            failures.append(f"{name}: no command")
            continue
        exe = argv[0]
        if not (os.path.isfile(exe) or shutil.which(exe)):
            failures.append(f"{name}: {exe} is not an executable on this machine")
        for arg in argv:
            if FORBIDDEN.search(arg):
                failures.append(f"{name}: command names a console/emulator/remote transport: {arg}")
            for url in re.findall(r'https?://[^\s"\']+', arg):
                if not is_loopback(url):
                    failures.append(f"{name}: command carries a non-loopback URL: {url}")

    for line in failures:
        print(f"  {line}", file=sys.stderr)
    if failures:
        return fail("a registered test does not run on this machine against the local fixture. "
                    "Hardware and emulator rungs are climbed by hand (docs/TESTING.md), never "
                    "by ctest.")
    print(f"{len(tests)} registered tests, all host-local")
    return 0


PHASES = {"loopback": phase_loopback, "guards": phase_guards, "host_only": phase_host_only}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("phase", choices=sorted(PHASES))
    ap.add_argument("--repo", required=True)
    ap.add_argument("--build", help="build tree, for the host_only phase")
    ap.add_argument("--ctest", default="ctest")
    ap.add_argument("--proxy-url", default="", help="the URL compiled into the test binaries")
    args = ap.parse_args()
    if args.phase == "host_only" and not args.build:
        return fail("--build is required for host_only")
    return PHASES[args.phase](args)


if __name__ == "__main__":
    sys.exit(main())
