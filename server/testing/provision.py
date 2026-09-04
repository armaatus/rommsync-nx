#!/usr/bin/env python3
"""Provision this worktree's RomM fixture so tests can authenticate.

seed.sh puts ROM files on disk; this puts RomM into the state the test suite
needs. It runs after `compose up -d` (scripts/orca/setup.sh) because every step
here talks to a live server.

What it guarantees, idempotently:

  1. an admin user exists, with fixture credentials the suite knows
  2. every platform folder on disk is registered
  3. the library has been SCANNED -- without this RomM reports zero roms even
     though the files are mounted, and every rom-dependent test fails on an
     empty library that looks like a code bug
  4. a curated `Handheld` collection exists (M0-6)
  5. a client token exists, obtained through the real device-code flow with no
     human in the loop -- the thing that unblocks M0-4

The credentials land in server/testing/fixture-auth.env (gitignored).

Never point this at a RomM you care about: step 1 creates an admin and step 3
scans the library. It refuses to run against anything but a loopback URL unless
--i-know-this-is-disposable is passed.
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import sys
import time
import urllib.parse

import requests
import socketio

# Fixture-only credentials. Committed on purpose: this stack is disposable and
# never holds real data, and a known password keeps runs reproducible -- the
# same reasoning as ROMM_AUTH_SECRET_KEY in docker-compose.yml.
#
# These MUST match rig::kUser/kPassword/kEmail in tests/rig.hpp. The C++ rig
# tests create the same account themselves (rig::EnsureUser) so that a worktree
# older than this script still works; whichever runs first wins, and they only
# agree if the constants do.
FIXTURE_USER = "rommsync"
FIXTURE_PASSWORD = "rommsync-test-only"
FIXTURE_EMAIL = "rommsync@example.invalid"

# The collection M0-6 asks for. Handheld platforms only, so the set is
# meaningful rather than "everything we happened to seed".
COLLECTION_NAME = "Handheld"
HANDHELD_SLUGS = {"gb", "gbc", "gba", "nds", "3ds", "psp", "ngp", "gg", "ws"}

# Scopes the sysmodule asks for -- docs/API_CONTRACT.md#scopes-to-request.
CLIENT_SCOPES = [
    "me.read",
    "roms.read",
    "roms.user.read",
    "roms.user.write",
    "assets.read",
    "assets.write",
    "devices.read",
    "devices.write",
    "collections.read",
]

SOCKETIO_PATH = "/ws/socket.io"


class ProvisionError(RuntimeError):
    pass


class Romm:
    """A logged-in RomM session that keeps CSRF and cookies straight.

    RomM rejects unsafe methods without the `romm_csrftoken` cookie echoed in an
    `X-CSRFToken` header, so every POST goes through here rather than through a
    bare requests call.
    """

    def __init__(self, base_url: str):
        self.base = base_url.rstrip("/")
        self.s = requests.Session()

    def _csrf(self) -> str:
        token = self.s.cookies.get("romm_csrftoken")
        if not token:
            # Any GET mints one.
            self.s.get(f"{self.base}/api/heartbeat", timeout=10)
            token = self.s.cookies.get("romm_csrftoken")
        if not token:
            raise ProvisionError("server never issued a romm_csrftoken cookie")
        return token

    def get(self, path: str, **kw) -> requests.Response:
        return self.s.get(f"{self.base}{path}", timeout=30, **kw)

    def post(self, path: str, **kw) -> requests.Response:
        headers = {"X-CSRFToken": self._csrf(), **kw.pop("headers", {})}
        return self.s.post(f"{self.base}{path}", headers=headers, timeout=120, **kw)

    def login(self) -> None:
        r = self.post("/api/login", auth=(FIXTURE_USER, FIXTURE_PASSWORD))
        if r.status_code != 200:
            raise ProvisionError(
                f"login as {FIXTURE_USER} failed ({r.status_code}). If this fixture "
                f"predates the provisioner its admin has a different password; "
                f"reset it with `./scripts/orca/compose.sh down -v && "
                f"./scripts/orca/compose.sh up -d`."
            )
        if not self.s.cookies.get("romm_session"):
            raise ProvisionError("login succeeded but set no romm_session cookie")


def wait_for_heartbeat(romm: Romm, timeout: int) -> None:
    """Block until RomM answers, or fail with a diagnosis rather than a stack trace."""
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        try:
            r = romm.get("/api/heartbeat")
            if r.status_code == 200:
                return
            last = f"http {r.status_code}"
        except requests.RequestException as exc:
            last = type(exc).__name__
        time.sleep(2)
    raise ProvisionError(
        f"RomM did not answer within {timeout}s (last: {last}). "
        f"Is the stack up? ./scripts/orca/compose.sh ps"
    )


def ensure_admin(romm: Romm) -> None:
    """Create the first admin, which RomM allows unauthenticated while none exists.

    Once a user exists the same call is 403, so a 403 here means "already done"
    rather than a failure -- that is what makes re-running setup.sh safe.
    """
    r = romm.post(
        "/api/users",
        json={
            "username": FIXTURE_USER,
            "password": FIXTURE_PASSWORD,
            "email": FIXTURE_EMAIL,
            "role": "admin",
        },
    )
    if r.status_code in (200, 201):
        print(f"  created admin {FIXTURE_USER}")
    elif r.status_code == 403:
        print(f"  admin already exists")
    else:
        raise ProvisionError(f"could not create the admin user: {r.status_code} {r.text[:200]}")


def expected_roms_on_disk(library: str) -> int:
    """Count the roms seed.sh staged, the way RomM counts them.

    Readiness cannot be keyed on /api/setup/library: it lists only platforms not
    yet imported, so on the second run it reports nothing on disk and the check
    passes vacuously against whatever the library happens to contain. The
    filesystem is the one source that says the same thing on every run.

    One entry per rom, directories included -- a multi-file rom (M3-4) is a
    directory and still one rom.
    """
    roms = os.path.join(library, "roms")
    if not os.path.isdir(roms):
        return 0
    total = 0
    for platform in os.listdir(roms):
        pdir = os.path.join(roms, platform)
        if not os.path.isdir(pdir) or platform.startswith("."):
            continue
        total += sum(1 for e in os.listdir(pdir) if not e.startswith("."))
    return total


def ensure_platforms(romm: Romm) -> list[str]:
    """Register every platform folder seed.sh created.

    A platform that is not registered is not scanned, so its roms never appear.
    """
    setup = romm.get("/api/setup/library")
    if setup.status_code != 200:
        raise ProvisionError(f"GET /api/setup/library: {setup.status_code}")
    on_disk = [p["fs_slug"] for p in setup.json().get("existing_platforms", [])]

    known = romm.get("/api/platforms")
    registered = {p["fs_slug"] for p in known.json()} if known.status_code == 200 else set()

    added = []
    for slug in on_disk:
        if slug in registered:
            continue
        r = romm.post("/api/platforms", json={"fs_slug": slug})
        if r.status_code not in (200, 201):
            raise ProvisionError(f"could not register platform {slug}: {r.status_code} {r.text[:200]}")
        added.append(slug)

    # /api/setup/library lists only what is NOT yet imported, so on a re-run it
    # is empty and says nothing about what the fixture holds. Report the union.
    all_slugs = sorted(registered | set(on_disk))
    print(f"  platforms: {', '.join(all_slugs) or '(none)'}"
          + (f" (registered {', '.join(added)})" if added else ""))
    return all_slugs


def scan_library(romm: Romm, timeout: int) -> dict:
    """Run a library scan and wait for it to finish.

    The scan is not reachable over REST: `POST /api/tasks/run/scan_library`
    answers "cannot be run" because the task is declared manual_run=False. It is
    driven over socket.io, and authorization is resolved from the session cookie
    stored at connect -- not from the payload -- so the client has to carry the
    login cookie into the handshake.

    Metadata providers are left off, matching docker-compose.yml: enrichment
    would reach the network and make the fixture non-deterministic.
    """
    session = romm.s.cookies.get("romm_session")
    if not session:
        raise ProvisionError("no romm_session cookie; call login() first")

    sio = socketio.Client(reconnection=False, request_timeout=30)
    done: dict = {}

    @sio.on("scan:done")
    def _done(stats=None):  # noqa: ANN001 - socketio hands us whatever it has
        done["stats"] = stats or {}
        done["ok"] = True

    @sio.on("scan:done_ko")
    def _failed(message=None):  # noqa: ANN001
        done["error"] = message or "unknown error"
        done["ok"] = False

    try:
        sio.connect(
            romm.base,
            socketio_path=SOCKETIO_PATH,
            headers={"Cookie": f"romm_session={session}"},
            transports=["websocket", "polling"],
            wait_timeout=30,
        )
    except Exception as exc:  # socketio raises its own hierarchy
        raise ProvisionError(f"could not open the scan socket: {exc}") from exc

    try:
        sio.emit(
            "scan",
            {
                "platforms": [],
                "platform_fs_slugs": [],
                "type": "quick",
                "roms_ids": [],
                "apis": [],
                "launchbox_remote_enabled": False,
                "playmatch_enabled": False,
            },
        )
        deadline = time.monotonic() + timeout
        while "ok" not in done and time.monotonic() < deadline:
            sio.sleep(0.5)
    finally:
        sio.disconnect()

    if "ok" not in done:
        raise ProvisionError(f"scan did not finish within {timeout}s")
    if not done["ok"]:
        raise ProvisionError(f"scan failed: {done['error']}")
    return done.get("stats") or {}


def wait_for_roms(romm: Romm, expected: int, timeout: int) -> int:
    """Wait until the scanned roms are actually queryable.

    `scan:done` fires when the scan job ends; the rows it wrote are what tests
    read. Keying readiness on the rom count -- not on the event alone, and
    certainly not on /api/heartbeat -- is the difference between a fixture that
    is up and one that is ready.
    """
    deadline = time.monotonic() + timeout
    seen = 0
    while time.monotonic() < deadline:
        r = romm.get("/api/roms", params={"limit": 1})
        if r.status_code == 200:
            seen = r.json().get("total", 0)
            if seen >= expected > 0:
                return seen
            if expected == 0:
                return seen
        time.sleep(1)
    raise ProvisionError(
        f"only {seen} of {expected} roms were queryable after {timeout}s; "
        f"the scan reported done but the library looks empty"
    )


def ensure_collection(romm: Romm) -> int | None:
    """Create the curated Handheld collection and put the handheld roms in it."""
    existing = romm.get("/api/collections")
    if existing.status_code != 200:
        raise ProvisionError(f"GET /api/collections: {existing.status_code}")
    for c in existing.json():
        if c.get("name") == COLLECTION_NAME:
            print(f"  collection '{COLLECTION_NAME}' already exists (id {c['id']})")
            return c["id"]

    # multipart, not json: Body_add_collection_api_collections_post is a form.
    r = romm.post(
        "/api/collections",
        files={
            "name": (None, COLLECTION_NAME),
            "description": (None, "Handheld platforms — the fixture's curated collection (M0-6)."),
        },
    )
    if r.status_code not in (200, 201):
        raise ProvisionError(f"could not create the collection: {r.status_code} {r.text[:200]}")
    collection_id = r.json()["id"]

    roms = romm.get("/api/roms", params={"limit": 500})
    ids = [
        rom["id"]
        for rom in roms.json().get("items", [])
        if (rom.get("platform_fs_slug") or rom.get("platform_slug")) in HANDHELD_SLUGS
    ]
    if ids:
        add = romm.post(f"/api/collections/{collection_id}/roms", json={"rom_ids": ids})
        if add.status_code not in (200, 201):
            raise ProvisionError(f"could not add roms to the collection: {add.status_code} {add.text[:200]}")
    print(f"  collection '{COLLECTION_NAME}' (id {collection_id}) with {len(ids)} rom(s)")
    return collection_id


def issue_client_token(romm: Romm) -> dict:
    """Drive the real device-code flow to completion with no human in the loop.

    This is the flow docs/AUTH.md describes and M1 implements: init, approve,
    poll for the token. The web UI is the only approver a human has, but
    /api/auth/device/approve is an ordinary authenticated endpoint -- so the
    provisioner approves its own code and the suite gets a token without anyone
    clicking anything. That is what unblocks M0-4.
    """
    device_identifier = f"fixture-{secrets.token_hex(8)}"
    init = romm.post(
        "/api/auth/device/init",
        json={
            "client_device_identifier": device_identifier,
            "name": "rommsync fixture",
            "client": "rommsync-nx",
            "platform": "switch",
            "requested_scopes": CLIENT_SCOPES,
        },
    )
    if init.status_code not in (200, 201):
        raise ProvisionError(f"device init failed: {init.status_code} {init.text[:200]}")
    payload = init.json()
    user_code = payload.get("user_code")
    device_code = payload.get("device_code")
    if not user_code or not device_code:
        raise ProvisionError(f"device init response missing codes: {sorted(payload)}")

    approve = romm.post(
        "/api/auth/device/approve",
        json={"user_code": user_code, "approved_scopes": CLIENT_SCOPES},
        auth=(FIXTURE_USER, FIXTURE_PASSWORD),
    )
    if approve.status_code not in (200, 201):
        raise ProvisionError(f"device approve failed: {approve.status_code} {approve.text[:200]}")

    # Poll exactly as the sysmodule will, rather than assuming approval is
    # instantly visible to the token endpoint.
    interval = max(1, int(payload.get("interval") or 1))
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        tok = romm.post("/api/auth/device/token", json={"device_code": device_code})
        if tok.status_code == 200:
            body = tok.json()
            token = body.get("access_token")
            if not token:
                raise ProvisionError(f"token response has no access_token: {sorted(body)}")
            return {"token": token, "device_identifier": device_identifier, "raw": body}
        if tok.status_code not in (400, 401, 428):
            raise ProvisionError(f"device token failed: {tok.status_code} {tok.text[:200]}")
        time.sleep(interval)
    raise ProvisionError("device code was approved but no token was issued within 60s")


def register_device(romm: Romm, token: str) -> str | None:
    """Register the fixture as a device and return its device_id.

    Sync calls are scoped by device_id (docs/API_CONTRACT.md#device-registration),
    so a fixture without one cannot exercise the sync loop at all.
    """
    r = romm.post(
        "/api/devices",
        headers={"Authorization": f"Bearer {token}"},
        json={
            "name": "rommsync fixture",
            "platform": "switch",
            "client": "rommsync-nx",
            "allow_existing": True,
        },
    )
    if r.status_code in (200, 201):
        body = r.json()
        # The response names it `device_id`, not `id` -- API_CONTRACT.md said
        # `id` until this fixture checked it against a live 5.2.0.
        device_id = body.get("device_id") or body.get("id")
        if device_id is None:
            raise ProvisionError(f"device response has no device_id: {sorted(body)}")
        return device_id
    # Not fatal: M1-3 owns device registration, and a fixture without a device
    # is still enough for auth and library work.
    print(f"  note: device registration returned {r.status_code}; continuing without a device_id")
    return None


def write_env(path: str, values: dict) -> None:
    tmp = f"{path}.tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write("# Generated by server/testing/provision.py — do not commit, do not hand-edit.\n")
        fh.write("# Credentials for THIS worktree's disposable RomM fixture.\n")
        for k, v in values.items():
            fh.write(f"{k}={v}\n")
    os.replace(tmp, path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-url", default=os.environ.get("ROMM_BASE_URL"))
    ap.add_argument("--out", default="server/testing/fixture-auth.env")
    ap.add_argument("--wait", type=int, default=180, help="seconds to wait for RomM to answer")
    ap.add_argument("--scan-timeout", type=int, default=600)
    ap.add_argument("--library", default="server/testing/library",
                    help="where seed.sh staged the roms; readiness is keyed on it")
    ap.add_argument("--i-know-this-is-disposable", action="store_true")
    args = ap.parse_args()

    if not args.base_url:
        print("no --base-url and no ROMM_BASE_URL in the environment", file=sys.stderr)
        return 2

    host = urllib.parse.urlparse(args.base_url).hostname or ""
    if host not in ("127.0.0.1", "localhost", "::1") and not args.i_know_this_is_disposable:
        print(
            f"refusing to provision {args.base_url}: it is not loopback, and this script "
            f"creates an admin and rescans the library. See CLAUDE.md hard rule 1.",
            file=sys.stderr,
        )
        return 2

    romm = Romm(args.base_url)
    try:
        print(f"==> provisioning {args.base_url}")
        wait_for_heartbeat(romm, args.wait)
        ensure_admin(romm)
        romm.login()
        on_disk = ensure_platforms(romm)

        expected = expected_roms_on_disk(args.library)
        print(f"  scanning ({len(on_disk)} platform(s), expecting {expected} rom(s))")
        scan_library(romm, args.scan_timeout)
        found = wait_for_roms(romm, expected, timeout=120)
        print(f"  library ready: {found} rom(s)")

        collection_id = ensure_collection(romm)
        creds = issue_client_token(romm)
        device_id = register_device(romm, creds["token"])

        write_env(
            args.out,
            {
                "ROMM_FIXTURE_USER": FIXTURE_USER,
                "ROMM_FIXTURE_PASSWORD": FIXTURE_PASSWORD,
                "ROMM_FIXTURE_TOKEN": creds["token"],
                "ROMM_FIXTURE_DEVICE_IDENTIFIER": creds["device_identifier"],
                "ROMM_FIXTURE_DEVICE_ID": device_id if device_id is not None else "",
                "ROMM_FIXTURE_COLLECTION_ID": collection_id if collection_id is not None else "",
                "ROMM_FIXTURE_ROM_COUNT": found,
            },
        )
        print(f"  credentials written to {args.out}")
        return 0
    except ProvisionError as exc:
        print(f"provisioning failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
