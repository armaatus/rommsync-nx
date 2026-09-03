#!/usr/bin/env python3
"""
probe_contract.py - validate the RomM sync contract against a LIVE server.

Server-side developer tool. It does NOT touch a Switch. It walks the exact flow
the sysmodule will use and prints the real response shapes, so the on-console
code can be written against verified fields instead of guesses.

Usage:
    pip install requests
    python probe_contract.py --url http://localhost:1515

By default it runs the read-only checks. Add --auth to run the device-code
pairing flow interactively (you approve the code in RomM's web UI), then
--negotiate to do a no-op sync negotiation.

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://localhost:1515")
    ap.add_argument("--auth", action="store_true", help="run device-code pairing")
    ap.add_argument("--negotiate", action="store_true", help="run a no-op negotiate (implies --auth)")
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
    init = s.post(f"{base}/api/auth/device/init", json={
        "client_device_identifier": "probe-contract-script",
        "name": "probe_contract.py",
        "client": "rommsync-nx-probe",
        "platform": "switch",
        "client_version": "0.0.0",
        "requested_scopes": [
            "me.read", "roms.read", "roms.user.read", "roms.user.write",
            "assets.read", "assets.write", "devices.read", "devices.write",
            "collections.read",
        ],
    }, timeout=30)
    init.raise_for_status()
    init = init.json()
    show_shape(init, "init response")
    print(json.dumps(init, indent=2)[:800])

    device_code = init.get("device_code")
    user_code = init.get("user_code")
    verify = init.get("verification_uri") or init.get("verification_uri_complete")
    interval = init.get("interval", 5)
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
    device_id = dev.get("id")
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
