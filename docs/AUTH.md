# Authentication — device-code flow

The Switch has no keyboard-friendly login. RomM 5.2.0 ships the OAuth 2.0 Device
Authorization Grant, which is designed for exactly this: the device shows a short
code, the human approves it once in a browser, the device gets a token.

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
     │        interval, expires_in}       │                           │
     │                                    │                           │
  overlay shows: "Go to <uri>,           │                           │
  enter code ABCD-1234"                  │       approve ABCD-1234    │
     │                                    │◀──────────────────────────│
     │  POST /api/auth/device/token       │                           │
     │  {device_code}  (poll every        │                           │
     │   `interval`s until approved)      │                           │
     │ ─────────────────────────────────▶│                           │
     │  ◀── {access_token (rmm_...),      │                           │
     │        device_id, scopes,          │                           │
     │        expires_at}                 │                           │
     │                                    │                           │
     │  POST /api/devices  {name,         │                           │
     │   platform, client}                │                           │
     │ ─────────────────────────────────▶│                           │
     │  ◀── DeviceSchema {id=device_id}   │                           │
     └── persist {token, device_id} ──────┘
```

> The response fields above were read off a live 5.2.0 by
> `server/probe_contract.py --auth`, which now approves its own code and so runs
> unattended. Three differ from what this file drew before it could be checked:
> the verification target is a **path** (`verification_path`), not an absolute
> `verification_uri`; the token response carries **`expires_at`**, not
> `expires_in`, and no `token_type` or refresh token; and it already includes a
> **`device_id`**. Pinning the full set — every field, every type — is issue
> **M1-2**; this is only what the fixture had to get right to authenticate.

## Client identifier

`client_device_identifier` must be stable per console (so re-pairing recognizes
the same device). Derive it from a stable value (e.g. SHA1 of the console serial
or a random id generated once and stored next to the token). Never send anything
that identifies the *user* beyond what RomM needs.

## Token storage

Persist `{access_token, refresh_token?, device_id, expires_at, server_url}` to
`sdmc:/config/rommsync/token.dat`. See [SECURITY.md](SECURITY.md) for the
threat model — the SD is readable by anything on the console, so treat the token
as sensitive and scope it minimally.

## Re-pairing / revocation

- The overlay offers "Re-pair" → discards the token and restarts the flow.
- Revoking on the server (`DELETE /api/client-tokens/{id}`) invalidates it; the
  sysmodule detects `401`, marks itself unauthenticated, and prompts re-pair via
  the overlay status screen.

## Scopes

Request the minimum (see [API_CONTRACT.md](API_CONTRACT.md#scopes-to-request)).
Don't request `*.write` you don't use; add `me.write` only if/when play-session
recording lands.
