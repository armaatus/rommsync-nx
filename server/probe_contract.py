#!/usr/bin/env python3
"""
probe_contract.py - validate the RomM sync contract against a LIVE server.

Server-side developer tool. It does NOT touch a Switch. It walks the exact flow
the sysmodule will use and prints the real response shapes, so the on-console
code can be written against verified fields instead of guesses.

Usage:
    ./.venv/bin/python server/probe_contract.py --url "$ROMM_BASE_URL"

(The venv is created by scripts/orca/setup.sh from server/requirements.txt.)

By default it runs the read-only checks. Add --auth to run the device-code
pairing flow, then --negotiate to do a no-op sync negotiation.

--auth approves its own code using the fixture credentials written by
server/testing/provision.py, so it needs no human at a browser. Against a server
that is not the fixture, pass --approve-as USER:PASS, or nothing at all to fall
back to approving by hand in the web UI.

Nothing is written to your library: the negotiate call sends an empty save list,
which yields an all-noop plan, and we do NOT call /complete.
"""
import argparse
import json
import sys
import time

import requests


def hr(t):
    print("\n" + "=" * 4, t)


def show_shape(obj, name):
    if isinstance(obj, dict):
        print(f"{name} keys: {list(obj.keys())}")
    elif isinstance(obj, list):
        print(f"{name}: list[{len(obj)}]", "first keys:",
              list(obj[0].keys()) if obj and isinstance(obj[0], dict) else "-")
    else:
        print(f"{name}: {type(obj).__name__}")


def fixture_credentials(path):
    """Read the fixture's user/password, or None if this is not the fixture.

    The device-code grant is designed around a human at a browser, which is
    exactly the wrong shape for CI and for an agent: the flow simply never
    completes. /api/auth/device/approve is an ordinary authenticated endpoint,
    so anyone who can log in can approve their own code and the flow closes
    itself.
    """
    try:
        with open(path, encoding="utf-8") as fh:
            env = dict(
                line.rstrip("\n").split("=", 1)
                for line in fh
                if "=" in line and not line.startswith("#")
            )
    except OSError:
        return None
    user, password = env.get("ROMM_FIXTURE_USER"), env.get("ROMM_FIXTURE_PASSWORD")
    return (user, password) if user and password else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://localhost:1515")
    ap.add_argument("--auth", action="store_true", help="run device-code pairing")
    ap.add_argument("--negotiate", action="store_true", help="run a no-op negotiate (implies --auth)")
    ap.add_argument("--approve-as", metavar="USER:PASS",
                    help="approve the device code over the API as this user "
                         "(default: the fixture credentials, if present)")
    ap.add_argument("--fixture-auth", default="server/testing/fixture-auth.env",
                    help="where provision.py wrote the fixture credentials")
    args = ap.parse_args()
    base = args.url.rstrip("/")
    s = requests.Session()

    hr("server version")
    spec = s.get(f"{base}/openapi.json", timeout=30).json()
    print("RomM", spec["info"]["version"], "| paths:", len(spec["paths"]))

    # sanity: the endpoints we depend on exist
    hr("required endpoints present")
    need = [
        ("POST", "/api/auth/device/init"),
        ("POST", "/api/auth/device/token"),
        ("POST", "/api/devices"),
        ("POST", "/api/sync/negotiate"),
        ("POST", "/api/sync/sessions/{session_id}/complete"),
        ("POST", "/api/saves"),
        ("GET", "/api/saves/{id}/content"),
        ("GET", "/api/roms"),
        ("GET", "/api/roms/{id}/content/{file_name}"),
        ("GET", "/api/collections"),
    ]
    ok = True
    for m, p in need:
        present = p in spec["paths"] and m.lower() in spec["paths"][p]
        print(f"  [{'ok' if present else 'MISSING'}] {m} {p}")
        ok = ok and present
    if not ok:
        print("\n!! Contract drift: some endpoints are missing on this server.")
        print("   Update docs/API_CONTRACT.md before building against it.")

    if not (args.auth or args.negotiate):
        print("\nRead-only checks done. Re-run with --auth (and --negotiate) to")
        print("exercise the live flow.")
        return

    # ---- device-code pairing ----
    hr("device auth: init")
    # Kept in a variable because /api/auth/device/init does NOT echo the scopes
    # back, and /approve rejects an empty approved_scopes list.
    requested_scopes = [
        "me.read", "roms.read", "roms.user.read", "roms.user.write",
        "assets.read", "assets.write", "devices.read", "devices.write",
        "collections.read",
    ]
    init = s.post(f"{base}/api/auth/device/init", json={
        "client_device_identifier": "probe-contract-script",
        "name": "probe_contract.py",
        "client": "rommsync-nx-probe",
        "platform": "switch",
        "client_version": "0.0.0",
        "requested_scopes": requested_scopes,
    }, timeout=30)
    init.raise_for_status()
    init = init.json()
    show_shape(init, "init response")
    print(json.dumps(init, indent=2)[:800])

    device_code = init.get("device_code")
    user_code = init.get("user_code")
    # 5.2.0 returns `verification_path` / `verification_path_complete` -- a path,
    # not the absolute `verification_uri` the OAuth spec names and AUTH.md drew.
    verify = init.get("verification_path_complete") or init.get("verification_path") or ""
    if verify.startswith("/"):
        verify = base + verify
    interval = init.get("interval", 5)
    approver = None
    if args.approve_as:
        user, _, password = args.approve_as.partition(":")
        approver = (user, password)
    else:
        approver = fixture_credentials(args.fixture_auth)

    if approver:
        hr("device auth: approve (no browser)")
        ap_r = s.post(f"{base}/api/auth/device/approve",
                      json={"user_code": user_code,
                            "approved_scopes": requested_scopes},
                      auth=approver, timeout=30)
        if ap_r.status_code not in (200, 201):
            print(f"!! approve failed: {ap_r.status_code} {ap_r.text[:200]}")
            sys.exit(1)
        print(f"  approved {user_code} as {approver[0]}")
    else:
        print(f"\n>>> Approve this in RomM's web UI: code = {user_code}  at  {verify}")

    hr("device auth: poll token")
    token = None
    deadline = time.time() + init.get("expires_in", 300)
    while time.time() < deadline:
        r = s.post(f"{base}/api/auth/device/token", json={"device_code": device_code}, timeout=30)
        if r.status_code == 200:
            tok = r.json()
            show_shape(tok, "token response")
            token = tok.get("access_token") or tok.get("token")
            print("token acquired (", (token or "")[:12], "...)")
            break
        print(f"  waiting... ({r.status_code})")
        time.sleep(interval)
    if not token:
        print("!! not approved in time"); sys.exit(1)
    s.headers["Authorization"] = f"Bearer {token}"

    hr("register device")
    dev = s.post(f"{base}/api/devices", json={
        "name": "probe device", "platform": "switch", "client": "rommsync-nx-probe",
        "allow_existing": True,
    }, timeout=30)
    dev.raise_for_status(); dev = dev.json()
    show_shape(dev, "device")
    # `device_id`, not `id` -- verified against a live 5.2.0 (docs/API_CONTRACT.md).
    device_id = dev.get("device_id") or dev.get("id")
    print("device_id:", device_id)

    hr("roms sample (shape the client needs)")
    roms = s.get(f"{base}/api/roms", params={"limit": 1}, timeout=30).json()
    items = roms.get("items", roms) if isinstance(roms, dict) else roms
    if items:
        show_shape(items[0], "rom")

    if args.negotiate:
        hr("no-op negotiate (empty saves -> all noop, /complete NOT called)")
        neg = s.post(f"{base}/api/sync/negotiate",
                     json={"device_id": device_id, "saves": []}, timeout=60)
        neg.raise_for_status(); neg = neg.json()
        show_shape(neg, "negotiate response")
        print(json.dumps(neg, indent=2)[:800])
        print("\nDone. Session left open (not completed) on purpose.")


if __name__ == "__main__":
    main()
