# Authentication — device-code flow

The Switch has no keyboard-friendly login. RomM 5.2.0 ships the OAuth 2.0 Device
Authorization Grant, which is designed for exactly this: the device shows a short
code, the human approves it once in a browser, the device gets a token.

Every response below is real, captured from a live 5.2.0 by
`server/probe_contract.py --auth --capture` and committed under
`server/contract/captures/` (issue M0-4). `ctest -R contract` re-captures and
compares, so a RomM upgrade that changes these shapes turns the suite red.

## Flow

```
sys-rommsync                         RomM server                 you (browser)
     │  POST /api/auth/device/init        │                           │
     │  {client_device_identifier, name,  │                           │
     │   client:"rommsync-nx",            │                           │
     │   platform:"switch",               │                           │
     │   requested_scopes:[...]}          │                           │
     │ ─────────────────────────────────▶│                           │
     │  ◀── {device_code, user_code,      │                           │
     │        verification_path,          │                           │
     │        verification_path_complete, │                           │
     │        expires_in, interval}       │                           │
     │                                    │                           │
  overlay shows: "Go to <origin+path>,   │                           │
  enter code ABCD1234"                   │       approve ABCD1234     │
     │                                    │◀──────────────────────────│
     │  POST /api/auth/device/token       │                           │
     │  {device_code}  (poll every        │                           │
     │   `interval`s until approved)      │                           │
     │ ─────────────────────────────────▶│                           │
     │  ◀── {access_token (rmm_...),      │                           │
     │        device_id, scopes,          │                           │
     │        expires_at}                 │                           │
     └── persist {token, device_id} ──────┘
```

## The two responses, verbatim

`POST /api/auth/device/init` → `200`
([`captures/auth-device-init.json`](../server/contract/captures/auth-device-init.json)):

```json
{
  "device_code": "<64 lowercase hex characters>",
  "user_code": "<8 uppercase alphanumerics, no dash>",
  "verification_path": "/pair/device",
  "verification_path_complete": "/pair/device?user_code=<user_code>",
  "expires_in": 600,
  "interval": 5
}
```

`POST /api/auth/device/token` → `200` once approved
([`captures/auth-device-token.json`](../server/contract/captures/auth-device-token.json)):

```json
{
  "access_token": "rmm_<64 lowercase hex characters>",
  "device_id": "9fdce844-779b-4216-a6e2-597a2f3e7027",
  "scopes": ["assets.read", "assets.write", "collections.read", "devices.read",
             "devices.write", "me.read", "roms.read", "roms.user.read",
             "roms.user.write"],
  "expires_at": null
}
```

Four things differ from what a reading of the OAuth device-grant spec — and the
first draft of this file — would have you write:

1. **The verification target is a path**, `verification_path`, not the absolute
   `verification_uri` the spec names. RomM is origin-agnostic; the overlay must
   join it with the server URL the user configured. `verification_path_complete`
   is the same path with `?user_code=` appended, for a QR code.
2. **`user_code` has no dash** and is 8 characters, so don't format it as
   `ABCD-1234` — a human retyping what the overlay shows must be able to match it.
3. **The token response carries no `token_type`, no `refresh_token` and no
   `expires_in`.** There is nothing to refresh; M1-4 is `401` handling and
   re-pairing, not a refresh flow.
4. **`expires_at` is `null`** — this token does not expire on its own. Persist
   the field anyway (RomM may start setting it), but never treat "no expiry" as
   an error, and never pre-emptively re-pair on it.

The token response already includes a **`device_id`**. That is the paired
device, and it is enough to negotiate with: `POST /api/devices` creates *another*
device unless it is given a stable `mac_address` or `hostname`
([API_CONTRACT.md](API_CONTRACT.md#device-registration)). Registering a fresh
device on each boot gives every save an empty sync history and makes the first
negotiation of every tick look like a first encounter.

`scopes` is what the user **approved**, which need not be everything requested —
read it back and disable the features whose scope is missing rather than
discovering it as a `403` mid-sync.

## Approving without a browser

The grant assumes a human at a browser, which is the wrong shape for CI and for
an agent: the flow simply never completes. `/api/auth/device/approve` is an
ordinary authenticated endpoint, so anything that can log in can approve its own
code — that is how `server/testing/provision.py` and `probe_contract.py` close
the loop unattended. Details in [TESTING.md](TESTING.md).

## Client identifier

`client_device_identifier` must be stable per console (so re-pairing recognizes
the same device). Derive it from a stable value (e.g. SHA1 of the console serial
or a random id generated once and stored next to the token). Never send anything
that identifies the *user* beyond what RomM needs.

## Token storage

Persist `{access_token, device_id, scopes, expires_at, server_url}` to
`sdmc:/config/rommsync/token.dat`. There is no refresh token to store. See
[SECURITY.md](SECURITY.md) for the threat model — the SD is readable by anything
on the console, so treat the token as sensitive and scope it minimally.

## Re-pairing / revocation

- The overlay offers "Re-pair" → discards the token and restarts the flow.
- Revoking on the server (`DELETE /api/client-tokens/{id}`) invalidates it; the
  sysmodule detects `401`, marks itself unauthenticated, and prompts re-pair via
  the overlay status screen.
- Because `expires_at` is null, a `401` means **revoked**, not expired. Do not
  retry it as a transient failure — go straight to unauthenticated and ask for
  a re-pair.

## Scopes

Request the minimum (see [API_CONTRACT.md](API_CONTRACT.md#scopes-to-request)).
Don't request `*.write` you don't use; add `me.write` only if/when play-session
recording lands.
