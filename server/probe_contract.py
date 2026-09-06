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

Nothing is written to your library in these modes: the negotiate call sends an
empty save list, which yields an all-noop plan, and we do NOT call /complete.

--capture DIR writes every response as pretty JSON, secrets redacted. That is
how server/contract/captures/ was produced (issue M0-4) -- the committed
captures are what docs/API_CONTRACT.md quotes and what tests/test_contract_captures.py
re-checks against a live server.

Every mode past the read-only one WRITES, and none of them may point at a server
someone cares about. --auth registers a device and burns a device code;
--sync-scenarios additionally uploads synthetic saves under a per-run slot name.
All of them delete what they created on the way out, including when they leave
by exception, and all of them refuse a non-loopback URL without
--i-know-this-is-disposable (CLAUDE.md hard rule 1). The bare read-only mode is
left open on purpose: it fetches /openapi.json and creates nothing.

--sync-scenarios exists because an empty-save negotiate pins the envelope and
nothing else, so the four SyncOperationSchema actions (upload, download, no_op,
conflict) can only be captured by putting saves on the server and negotiating
against them.
"""
import argparse
import hashlib
import json
import os
import secrets as secrets_mod
import sys
import time
import urllib.parse

import requests

# How long to poll for approval. Deliberately shorter than the code's own
# expiry, so an approval that never lands is reported by this script rather than
# by whatever timeout the caller wrapped it in.
POLL_LIMIT_SECONDS = 120

# Kept in a variable because /api/auth/device/init does NOT echo the scopes
# back, and /approve rejects an empty approved_scopes list.
REQUESTED_SCOPES = [
    "me.read", "roms.read", "roms.user.read", "roms.user.write",
    "assets.read", "assets.write", "devices.read", "devices.write",
    "collections.read",
]


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


class Captures:
    """Writes each real response to DIR/<name>.json with secrets redacted.

    Redaction is by value, not by key: `verification_path_complete` carries the
    user code inside a URL, so blanking the `user_code` field alone would commit
    the very code it was blanking. Every registered secret is replaced wherever
    its text appears.
    """

    def __init__(self, directory):
        self.dir = directory
        self.secrets = {}
        if directory:
            os.makedirs(directory, exist_ok=True)

    def secret(self, value, placeholder):
        """Register a value that must never reach a committed file."""
        if value:
            self.secrets[value] = placeholder
        return value

    def _redact(self, obj):
        if isinstance(obj, dict):
            return {k: self._redact(v) for k, v in obj.items()}
        if isinstance(obj, list):
            return [self._redact(v) for v in obj]
        if isinstance(obj, str):
            for value, placeholder in self.secrets.items():
                obj = obj.replace(value, placeholder)
        return obj

    def redacted(self, obj):
        """The safe form of a response, for printing as well as writing.

        --approve-as is documented for servers that are not the fixture, and the
        test harness echoes this script's stdout into a CI log on failure, so a
        live device_code must not reach the terminal either.
        """
        return self._redact(obj)

    def record(self, name, obj):
        if not self.dir:
            return obj
        path = os.path.join(self.dir, f"{name}.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(self._redact(obj), fh, indent=2, sort_keys=False)
            fh.write("\n")
        print(f"  captured {name}.json")
        return obj


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


def device_auth(s, base, cap, approver):
    """Run the device-code flow and return `(access token, paired device id)`.

    The device id comes back with the token because pairing is what registers
    the console: RomM creates the row from `client_device_identifier` and hands
    back its id. Nothing has to call `POST /api/devices` to get one, and calling
    it would produce a *second* device -- see `register_device` below.
    """
    hr("device auth: init")
    init = s.post(f"{base}/api/auth/device/init", json={
        "client_device_identifier": "probe-contract-script",
        "name": "probe_contract.py",
        "client": "rommsync-nx-probe",
        "platform": "switch",
        "client_version": "0.0.0",
        "requested_scopes": REQUESTED_SCOPES,
    }, timeout=30)
    # 201, not 200 -- the OpenAPI snapshot declares it and docs/AUTH.md now says
    # so, which is only worth writing down if something checks it.
    # `raise_for_status()` would wave any 2xx through.
    if init.status_code != 201:
        print(f"!! init answered {init.status_code}, not the documented 201: "
              f"{init.text[:200]}", file=sys.stderr)
        sys.exit(1)
    init = init.json()
    show_shape(init, "init response")
    device_code = cap.secret(init.get("device_code"), "<device_code>")
    user_code = cap.secret(init.get("user_code"), "<user_code>")
    cap.record("auth-device-init", init)
    print(json.dumps(cap.redacted(init), indent=2)[:800])

    # 5.2.0 returns `verification_path` / `verification_path_complete` -- a path,
    # not the absolute `verification_uri` the OAuth spec names and AUTH.md drew.
    verify = init.get("verification_path_complete") or init.get("verification_path") or ""
    if verify.startswith("/"):
        verify = base + verify
    interval = init.get("interval", 5)

    # One poll before anyone has approved anything. This is the shape the
    # sysmodule sees on every tick of the pairing screen, and the OpenAPI
    # snapshot does not declare it at all -- it lists 200 and 422 and no error
    # body. `authorization_pending` and `expired_token` are both 400, so the
    # `detail` string is the only thing separating "keep polling" from "this
    # pairing is dead": capture it, and refuse to write a file that says
    # something else.
    hr("device auth: poll before approval")
    pending = s.post(f"{base}/api/auth/device/token",
                     json={"device_code": device_code}, timeout=30)
    try:
        pending_body = pending.json()
    except ValueError:
        pending_body = {"raw": pending.text[:200]}
    if pending.status_code != 400 or pending_body.get("detail") != "authorization_pending":
        print(f"!! an unapproved poll answered {pending.status_code} "
              f"{json.dumps(cap.redacted(pending_body))[:200]}, not 400 "
              f"authorization_pending", file=sys.stderr)
        sys.exit(1)
    cap.record("auth-device-token-pending", pending_body)
    print(f"  {pending.status_code} {json.dumps(pending_body)}")

    if approver:
        hr("device auth: approve (no browser)")
        ap_r = s.post(f"{base}/api/auth/device/approve",
                      json={"user_code": user_code,
                            "approved_scopes": REQUESTED_SCOPES},
                      auth=approver, timeout=30)
        if ap_r.status_code not in (200, 201):
            print(f"!! approve failed: {ap_r.status_code} {ap_r.text[:200]}")
            sys.exit(1)
        print(f"  approved the pending code as {approver[0]}")
    else:
        print(f"\n>>> Approve this in RomM's web UI: code = {user_code}  at  {verify}")

    hr("device auth: poll token")
    token = None
    paired_device_id = None
    # Bounded well below the code's own lifetime (5.2.0 sends expires_in: 600).
    # tests/test_contract_captures.py runs under a CTest timeout, and a script
    # that outlives it is killed with a bare "Timeout" instead of printing the
    # diagnostic below -- the one line that says approval never arrived.
    deadline = time.time() + min(init.get("expires_in", 300), POLL_LIMIT_SECONDS)
    while time.time() < deadline:
        r = s.post(f"{base}/api/auth/device/token", json={"device_code": device_code}, timeout=30)
        if r.status_code == 200:
            tok = r.json()
            show_shape(tok, "token response")
            token = tok.get("access_token") or tok.get("token")
            paired_device_id = tok.get("device_id")
            cap.secret(token, "<access_token>")
            cap.record("auth-device-token", tok)
            print("token acquired")
            break
        print(f"  waiting... ({r.status_code})")
        time.sleep(interval)
    if not token:
        print(f"!! not approved within {POLL_LIMIT_SECONDS}s", file=sys.stderr)
        sys.exit(1)
    if not paired_device_id:
        print("!! the token response named no device_id", file=sys.stderr)
        sys.exit(1)
    return token, paired_device_id


def paired_device(s, base, cap, device_id):
    """`GET /api/devices/{id}` for the device pairing created.

    This is `DeviceSchema`, which is a different shape from the
    `DeviceCreateResponse` next door -- the id is `id` here and `device_id`
    there -- and it is the one the client reads: it is how the console confirms,
    on every boot, that the device it caches still exists, is still its own
    (`client_device_identifier`) and still has `sync_enabled` set.
    """
    r = s.get(f"{base}/api/devices/{device_id}", timeout=30)
    if r.status_code != 200:
        print(f"!! the paired device could not be read back: {r.status_code} "
              f"{r.text[:200]}", file=sys.stderr)
        sys.exit(1)
    device = r.json()
    cap.record("devices-get", device)
    return device


def register_device(s, base, cap, name="probe device", capture_as="devices-create"):
    # No mac_address or hostname on purpose: those are the fields RomM matches
    # on, and without one every call creates a new device -- which is how each
    # scenario below gets a device with no sync history. `allow_existing` alone
    # deduplicates nothing, and neither does calling this with the device-bound
    # token of the device above: `client_device_identifier` is not a match key
    # here (docs/API_CONTRACT.md#device-registration). That is why the client
    # never calls this, and why this script deletes what it creates.
    dev = s.post(f"{base}/api/devices", json={
        "name": name, "platform": "switch", "client": "rommsync-nx-probe",
        "allow_existing": True,
    }, timeout=30)
    dev.raise_for_status()
    dev = dev.json()
    if capture_as:
        cap.record(capture_as, dev)
    # `device_id`, not `id` -- verified against a live 5.2.0 (docs/API_CONTRACT.md).
    return dev.get("device_id") or dev.get("id"), dev


def negotiate(s, base, cap, device_id, saves, capture_as=None):
    r = s.post(f"{base}/api/sync/negotiate",
               json={"device_id": device_id, "saves": saves}, timeout=60)
    r.raise_for_status()
    body = r.json()
    if capture_as:
        cap.record(capture_as, body)
    return body


def expect_action(plan, action, slot):
    """Fail loudly when a scenario captured an action it does not claim.

    The capture files are named after the action they demonstrate and the docs
    quote them as such. If RomM's arbitration changes, a scenario that quietly
    produces `upload` where it says `conflict` is worse than a red run: it
    publishes a wrong shape under a right-looking name.
    """
    got = [op["action"] for op in plan["operations"] if op.get("slot") == slot]
    if action not in got:
        print(f"!! scenario for '{action}' produced {got or 'no operations'} instead. "
              f"The capture would be mislabelled, so it is not written.", file=sys.stderr)
        sys.exit(1)


def client_save(rom_id, file_name, slot, emulator, content, updated_at):
    """One ClientSaveState entry, as the sysmodule will build it.

    `content_hash` is MD5: RomM stores the MD5 of the save bytes and compares
    against it, even though roms carry sha1/md5/crc. Sending a SHA1 here makes
    every identical save look changed.
    """
    return {
        "rom_id": rom_id,
        "file_name": file_name,
        "slot": slot,
        "emulator": emulator,
        "content_hash": hashlib.md5(content).hexdigest(),
        "updated_at": updated_at,
        "file_size_bytes": len(content),
    }


def sync_scenarios(s, base, cap, rom_id):
    """Put real saves on the server so all four negotiate actions can be captured.

    A negotiate with an empty save list -- the read-only mode above -- returns an
    empty `operations` array, which pins the envelope and nothing about
    SyncOperationSchema. Every action below therefore needs state on the server,
    which is why this mode is gated on a disposable URL.

    Each scenario cleans up the saves it made, so the fixture is left as it was
    found and a second run captures the same shapes.
    """
    run = secrets_mod.token_hex(4)
    v1 = b"rommsync probe save v1\n"
    created = []
    devices = []
    states = []

    # Negotiate reports every server save this device has no history for, so a
    # fixture that already holds saves adds operations these scenarios did not
    # arrange. Harmless for the flow, but it makes a capture that is quoted as
    # "the shape of a download" carry unrelated entries.
    existing = s.get(f"{base}/api/saves", timeout=30)
    if existing.status_code == 200 and existing.json():
        print(f"  note: the library already holds {len(existing.json())} save(s); "
              f"the captures will include operations for them too.")

    def cleanup(devices_too=False):
        if created:
            s.post(f"{base}/api/saves/delete", json={"saves": created}, timeout=60)
            created.clear()
        # Every scenario device is a new row -- that is the point, they must have
        # no sync history -- so without this the fixture accretes four per run,
        # and this runs on every `ctest`.
        if devices_too:
            for device_id in devices:
                s.delete(f"{base}/api/devices/{device_id}", timeout=30)
            devices.clear()

    try:
        # --- upload / no_op / download -------------------------------------
        slot = f"probe-{run}-a"
        device, _ = register_device(s, base, cap, f"probe-{run}-client", capture_as=None)
        devices.append(device)

        hr("negotiate: a save the server does not have")
        entry = client_save(rom_id, "probe.srm", slot, "probe-emulator", v1,
                            "2026-01-01T00:00:00Z")
        plan = negotiate(s, base, cap, device, [entry])
        expect_action(plan, "upload", slot)
        cap.record("sync-negotiate-upload", plan)
        print(json.dumps(plan, indent=2)[:600])

        hr("upload it (POST /api/saves)")
        up = s.post(f"{base}/api/saves",
                    params={"rom_id": rom_id, "emulator": "probe-emulator",
                            "slot": slot, "device_id": device},
                    files={"saveFile": ("probe.srm", v1, "application/octet-stream")},
                    timeout=60)
        up.raise_for_status()
        save = cap.record("saves-post", up.json())
        created.append(save["id"])
        # RomM renames on ingest: the stored file_name carries a
        # `[YYYY-MM-DD_HH-MM-SS]` tag, so the name that comes back is never the
        # name that was sent. Every later operation echoes the *server's* name.
        print(f"  sent 'probe.srm', server stored {save['file_name']!r}")

        hr("negotiate: the same save, unchanged")
        same = negotiate(s, base, cap, device, [entry])
        expect_action(same, "no_op", slot)
        cap.record("sync-negotiate-no-op", same)
        print(json.dumps(same, indent=2)[:600])

        hr("complete the session (operations_completed reflects the no-op)")
        done = s.post(f"{base}/api/sync/sessions/{same['session_id']}/complete",
                      json={"operations_completed": same["total_no_op"],
                            "operations_failed": 0, "play_sessions": []}, timeout=60)
        done.raise_for_status()
        cap.record("sync-complete", done.json())

        hr("negotiate: a second device that has never seen that save")
        # An empty client list is enough: server saves the device has no sync
        # history for come back as `download` even for roms the client never
        # mentioned. This is the only way the client learns a save exists.
        fresh, _ = register_device(s, base, cap, f"probe-{run}-second", capture_as=None)
        devices.append(fresh)
        pull = negotiate(s, base, cap, fresh, [])
        expect_action(pull, "download", slot)
        cap.record("sync-negotiate-download", pull)
        print(json.dumps(pull, indent=2)[:600])
        cleanup()

        # --- conflict -------------------------------------------------------
        # Both sides must have changed *since this device last synced this save*,
        # so a sync record has to exist first. An upload carrying `device_id`
        # writes one, and so does POST /api/saves/{id}/downloaded -- which is
        # what device `a` needs here, because the copy it must end up in conflict
        # over is the one device `b` uploaded.
        hr("negotiate: both sides changed since the last sync")
        slot = f"probe-{run}-b"
        a, _ = register_device(s, base, cap, f"probe-{run}-a", capture_as=None)
        b, _ = register_device(s, base, cap, f"probe-{run}-b", capture_as=None)
        devices += [a, b]

        first = s.post(f"{base}/api/saves",
                       params={"rom_id": rom_id, "emulator": "probe-emulator",
                               "slot": slot, "device_id": a},
                       files={"saveFile": ("probe.srm", v1, "application/octet-stream")},
                       timeout=60)
        first.raise_for_status()
        created.append(first.json()["id"])

        shared = s.post(f"{base}/api/saves",
                        params={"rom_id": rom_id, "emulator": "probe-emulator",
                                "slot": slot, "device_id": b, "overwrite": "true"},
                        files={"saveFile": ("probe.srm", b"server v2\n",
                                            "application/octet-stream")},
                        timeout=60)
        shared.raise_for_status()
        save_id = shared.json()["id"]
        created.append(save_id)

        s.post(f"{base}/api/saves/{save_id}/downloaded",
               json={"device_id": a}, timeout=30).raise_for_status()
        # RomM stores these timestamps at second granularity, and the comparison
        # is strictly greater-than. Without this wait the server-side edit lands
        # in the same second as a's sync record, the server counts as unchanged,
        # and the negotiation comes back `upload` -- a plausible-looking capture
        # of the wrong action.
        time.sleep(1.1)
        # Now move the server's copy on without touching a's sync record.
        s.put(f"{base}/api/saves/{save_id}", params={"device_id": b},
              files={"saveFile": ("probe.srm", b"server v3\n",
                                  "application/octet-stream")},
              timeout=60).raise_for_status()

        clash = negotiate(s, base, cap, a, [
            client_save(rom_id, "probe.srm", slot, "probe-emulator", b"client v3\n",
                        "2099-01-01T00:00:00Z"),
        ])
        expect_action(clash, "conflict", slot)
        cap.record("sync-negotiate-conflict", clash)
        print(json.dumps(clash, indent=2)[:600])
        cleanup()

        # --- the second conflict reason, and the 409 that guards an upload ---
        # Both are cases a client meets on its first sync after pairing, and both
        # are invisible to the scenarios above: they need a device with no sync
        # history against a save that already exists.
        hr("execute a planned upload without overwrite, and collide on a timestamp")
        slot = f"probe-{run}-c"
        owner, newcomer = (register_device(s, base, cap, f"probe-{run}-{n}",
                                           capture_as=None)[0] for n in ("owner", "new"))
        devices += [owner, newcomer]

        server_copy = s.post(f"{base}/api/saves",
                             params={"rom_id": rom_id, "emulator": "probe-emulator",
                                     "slot": slot, "device_id": owner},
                             files={"saveFile": ("probe.srm", v1,
                                                 "application/octet-stream")},
                             timeout=60)
        server_copy.raise_for_status()
        server_copy = server_copy.json()
        created.append(server_copy["id"])

        planned = negotiate(s, base, cap, newcomer, [
            client_save(rom_id, "probe.srm", slot, "probe-emulator", b"client v2\n",
                        "2099-01-01T00:00:00Z"),
        ])
        expect_action(planned, "upload", slot)

        # RomM plans this upload and then refuses to accept it: with a device_id
        # and a slot, add_save wants a sync row at least as new as the save it is
        # replacing, and a device that has never synced has none. Capture the
        # refusal -- a client executing the server's own plan has to send
        # overwrite=true, and this is the error it gets when it forgets.
        refused = s.post(f"{base}/api/saves",
                         params={"rom_id": rom_id, "emulator": "probe-emulator",
                                 "slot": slot, "device_id": newcomer,
                                 "session_id": planned["session_id"]},
                         files={"saveFile": ("probe.srm", b"client v2\n",
                                             "application/octet-stream")},
                         timeout=60)
        if refused.status_code != 409:
            print(f"!! an upload without overwrite=true returned "
                  f"{refused.status_code}, not the documented 409. "
                  f"docs/SYNC_PROTOCOL.md is describing a server that changed.",
                  file=sys.stderr)
            sys.exit(1)
        cap.record("saves-post-409-no-overwrite", refused.json())
        print(f"  upload without overwrite: {refused.status_code} "
              f"{refused.json().get('detail')!r}")

        # Equal timestamps and different content, with no sync history: a
        # conflict that never mentions "changed since last sync".
        #
        # Read the timestamp back rather than reusing the upload response's:
        # POST answers with microseconds (`...:51.553771+00:00`) and the value
        # the comparison sees is whole seconds. Sending the microsecond one makes
        # the client look newer by a fraction of a second, and the tie this
        # scenario exists to produce comes back as a plain `upload`.
        stored = s.get(f"{base}/api/saves/{server_copy['id']}", timeout=30)
        stored.raise_for_status()
        tie = negotiate(s, base, cap, newcomer, [
            client_save(rom_id, "probe.srm", slot, "probe-emulator",
                        b"a third, different copy\n", stored.json()["updated_at"]),
        ])
        expect_action(tie, "conflict", slot)
        cap.record("sync-negotiate-conflict-same-timestamp", tie)
        print(json.dumps(tie, indent=2)[:600])
        # --- save states ---------------------------------------------------
        #
        # M2-8. `/api/states` is NOT "the same flow with a different field", and
        # these two calls are what says so: `StateSchema` carries no `slot`, no
        # `content_hash`, no `origin_device_id` and no `device_syncs`, and there
        # is no `/api/states/{id}/downloaded` to record that a device has one.
        # Whatever a client does about a state, it does on its own.
        hr("upload a save state (POST /api/states)")
        state_name = f"probe-{run}.state"
        posted = s.post(f"{base}/api/states",
                        params={"rom_id": rom_id, "emulator": "probe-emulator"},
                        files={"stateFile": (state_name, v1, "application/octet-stream")},
                        timeout=60)
        posted.raise_for_status()
        state = cap.record("states-post", posted.json())
        states.append(state["id"])
        # RomM renames a *save* on ingest and does not rename a state. The whole
        # of M2-8's pairing rests on that, so it is checked rather than assumed.
        if state["file_name"] != state_name:
            print(f"!! POST /api/states stored {state['file_name']!r} for a state sent as "
                  f"{state_name!r}. M2-8 pairs a local state to a server row on "
                  f"(rom_id, file_name) precisely because RomM does NOT rename one; "
                  f"docs/API_CONTRACT.md is describing a server that changed.",
                  file=sys.stderr)
            sys.exit(1)

        hr("...and post the same name again: it REPLACES the row, in place")
        again = s.post(f"{base}/api/states",
                       params={"rom_id": rom_id, "emulator": "probe-other-emulator"},
                       files={"stateFile": (state_name, v1 + b"and more\n",
                                            "application/octet-stream")},
                       timeout=60)
        again.raise_for_status()
        replaced = again.json()
        if replaced["id"] != state["id"]:
            print(f"!! a second POST /api/states under the same name created row "
                  f"{replaced['id']} beside {state['id']} instead of replacing it. That is the "
                  f"opposite of what M2-8 is built on -- the client refuses to POST over a name "
                  f"it does not own precisely because the POST destroys what is there.",
                  file=sys.stderr)
            sys.exit(1)
        print(f"  same id ({replaced['id']}), {state['file_size_bytes']} -> "
              f"{replaced['file_size_bytes']} bytes, emulator "
              f"{state['emulator']!r} -> {replaced['emulator']!r}")

        hr("list them (GET /api/states?rom_id=)")
        listed = s.get(f"{base}/api/states", params={"rom_id": rom_id}, timeout=30)
        listed.raise_for_status()
        cap.record("states-list", listed.json())
        show_shape(listed.json(), "GET /api/states")
    finally:
        if states:
            s.post(f"{base}/api/states/delete", json={"states": states}, timeout=60)
            states.clear()
        cleanup(devices_too=True)
        print("\n  scenario saves, states and devices deleted; the fixture is as it was found.")


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
    ap.add_argument("--capture", metavar="DIR",
                    help="write each response to DIR as redacted JSON")
    ap.add_argument("--sync-scenarios", action="store_true",
                    help="upload throwaway saves so all four negotiate actions "
                         "can be captured (WRITES to the library; implies --negotiate)")
    ap.add_argument("--i-know-this-is-disposable", action="store_true",
                    help="allow a writing mode against a non-loopback URL")
    args = ap.parse_args()
    base = args.url.rstrip("/")
    s = requests.Session()
    cap = Captures(args.capture)

    # Before the first request, not before the first write: a refusal that has
    # already registered a device on the server it was refusing to touch is not
    # a refusal. --auth is a writing mode too -- it registers a device and burns
    # a device code -- which is why the guard is not --sync-scenarios' alone.
    if args.auth or args.negotiate or args.sync_scenarios:
        host = urllib.parse.urlparse(base).hostname or ""
        if host not in ("127.0.0.1", "localhost", "::1") and not args.i_know_this_is_disposable:
            print(f"refusing to run against {base}: it is not loopback, and this mode "
                  f"writes to the server. See CLAUDE.md hard rule 1.", file=sys.stderr)
            return 2

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
        # Non-zero, not just a loud print: this script is what CI and
        # tests/test_contract_captures.py run to detect drift, and a checker that
        # reports a missing endpoint on stdout while exiting 0 is a green signal
        # over a broken contract.
        print("\n!! Contract drift: some endpoints are missing on this server.")
        print("   Update docs/API_CONTRACT.md before building against it.")
        return 1

    if not (args.auth or args.negotiate or args.sync_scenarios):
        print("\nRead-only checks done. Re-run with --auth (and --negotiate) to")
        print("exercise the live flow.")
        return 0

    approver = None
    if args.approve_as:
        user, sep, password = args.approve_as.partition(":")
        if not sep or not user or not password:
            print("--approve-as wants USER:PASS", file=sys.stderr)
            return 2
        approver = (user, password)
    else:
        approver = fixture_credentials(args.fixture_auth)

    token, paired_device_id = device_auth(s, base, cap, approver)
    s.headers["Authorization"] = f"Bearer {token}"

    hr("the device pairing registered (DeviceSchema)")
    paired = paired_device(s, base, cap, paired_device_id)
    show_shape(paired, "device")
    print("paired device_id:", paired_device_id,
          "| client_device_identifier:", paired.get("client_device_identifier"))

    hr("register device")
    device_id, dev = register_device(s, base, cap)
    show_shape(dev, "device")
    print("device_id:", device_id)
    # A name-only payload matches nothing (see the table in API_CONTRACT.md), so
    # this is a new row every run -- and contract.captures runs it on every
    # `ctest`. Registering it is part of what this script documents; leaving it
    # behind is not.
    probe_devices = [device_id]

    # The try opens here, not further down: everything between registering the
    # device and deleting it can raise -- an unchecked 401 on /api/roms, a
    # negotiate against a library that moved -- and each of those used to leave
    # the device row behind on the way out.
    try:
        hr("roms sample (shape the client needs)")
        roms_r = s.get(f"{base}/api/roms", params={"limit": 1}, timeout=30)
        # Checked before recording: the documented re-capture command writes
        # straight into server/contract/captures/, so an unchecked 401 would
        # replace the committed roms-list.json with a FastAPI error body -- a
        # capture that looks like a shape and is a failure.
        roms_r.raise_for_status()
        roms = roms_r.json()
        cap.record("roms-list", roms)
        items = roms.get("items", roms) if isinstance(roms, dict) else roms
        rom_id = None
        if items:
            show_shape(items[0], "rom")
            rom_id = items[0]["id"]

        if args.negotiate or args.sync_scenarios:
            hr("no-op negotiate (empty saves -> all noop, /complete NOT called)")
            neg = negotiate(s, base, cap, device_id, [], "sync-negotiate-empty")
            show_shape(neg, "negotiate response")
            print(json.dumps(neg, indent=2)[:800])
            print("\nSession left open (not completed) on purpose.")

        if args.sync_scenarios:
            if rom_id is None:
                print("\n!! the library is empty, so no save can be attached to a rom.",
                      file=sys.stderr)
                return 1
            sync_scenarios(s, base, cap, rom_id)
    finally:
        for probe_device in probe_devices:
            s.delete(f"{base}/api/devices/{probe_device}", timeout=30)

    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
