# Authentication — device-code flow

The Switch has no keyboard-friendly login. RomM 5.2.0 ships the OAuth 2.0 Device
Authorization Grant, which is designed for exactly this: the device shows a short
code, the human approves it once in a browser, the device gets a token.

Every response below is real, captured from a live 5.2.0 by
`server/probe_contract.py --auth --capture` and committed under
`server/contract/captures/` (issue M0-4). `ctest -R contract` re-captures and
compares, so a RomM upgrade that changes these shapes turns the suite red.

The engine codes against [`core/include/rommsync/auth.hpp`](../core/include/rommsync/auth.hpp),
whose structs carry exactly these fields with these types — no field is inferred
from the OAuth spec, and none is optional that the server does not send as
`null`. `ctest -R auth.shapes` parses the committed captures through those
structs, so the code and this page cannot drift apart from the captures without
something going red.

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
     │  ◀── 400 {detail:                  │                           │
     │        "authorization_pending"}    │   ...until approval lands │
     │  ◀── 200 {access_token (rmm_...),  │                           │
     │        device_id, scopes,          │                           │
     │        expires_at}                 │                           │
     └── persist {token, device_id} ──────┘
```

## The responses, verbatim

`POST /api/auth/device/init` → **`201 Created`**, not `200`
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

A poll that has not been approved yet
([`captures/auth-device-token-pending.json`](../server/contract/captures/auth-device-token-pending.json)):

```json
{
  "detail": "authorization_pending"
}
```

Six things differ from what a reading of the OAuth device-grant spec — and the
first draft of this file — would have you write:

1. **The verification target is a path**, `verification_path`, not the absolute
   `verification_uri` the spec names. RomM is origin-agnostic; the overlay must
   join it with the server URL the user configured. `verification_path_complete`
   is the same path with `?user_code=` appended, for a QR code.
   `DeviceInitResponse::VerificationUrl()` does the join, dropping trailing
   slashes on the configured URL so it cannot produce `//pair/device`.
2. **`user_code` has no dash** and is 8 characters, so don't format it as
   `ABCD-1234` — a human retyping what the overlay shows must be able to match it.
   Its alphabet is `ABCDEFGHJKMNPQRSTUVWXYZ23456789`: `I`, `L`, `O`, `0` and `1`
   are already excluded, so the overlay does not need a font that disambiguates
   them and must never "helpfully" correct a character the user typed.
3. **The token response carries no `token_type`, no `refresh_token` and no
   `expires_in`.** There is nothing to refresh; M1-4 is `401` handling and
   re-pairing, not a refresh flow.
4. **`expires_at` is `null`** — this token does not expire on its own. Persist
   the field anyway (RomM may start setting it), but never treat "no expiry" as
   an error, and never pre-emptively re-pair on it.
5. **The error is not an OAuth error object.** RFC 8628 says the pending poll
   answers `{"error": "authorization_pending"}`; RomM answers FastAPI's
   `{"detail": "..."}`. Reading `error` finds nothing, and a client that treats
   "no `error` key" as success walks into the token branch with no token.
6. **The grant reasons are not distinguished by status.** `authorization_pending`,
   `slow_down`, `access_denied` and `expired_token` are all `400`. The status
   code alone cannot tell "keep waiting" from "this pairing is dead"; only the
   `detail` string can — see the table below.

The token response already includes a **`device_id`**. That is the paired
device, and it is enough to negotiate with: `POST /api/devices` creates *another*
device unless it is given a stable `mac_address` or `hostname`
([API_CONTRACT.md](API_CONTRACT.md#device-registration)). Registering a fresh
device on each boot gives every save an empty sync history and makes the first
negotiation of every tick look like a first encounter.

`scopes` is what the user **approved**, which need not be everything requested —
read it back and disable the features whose scope is missing rather than
discovering it as a `403` mid-sync.

## Polling the token endpoint

Every `400` state below was observed against a live 5.2.0, and four different
meanings share that one status. `auth::ClassifyTokenPoll(status, body)` maps a
response to one of them and `auth::ShouldKeepPolling()` says whether the loop
continues; nothing else in the engine branches on the raw string.

| Status | `detail` | Means | Poll again? |
|---|---|---|---|
| `200` | — | approved; the body is the token response | no, you're done |
| `400` | `authorization_pending` | nobody has approved the code yet | yes, after `interval` |
| `400` | `slow_down` | you polled again inside `interval` for this `device_code` | yes, after `interval` |
| `400` | `access_denied` | a human denied it in the web UI | **no** — restart the flow |
| `400` | `expired_token` | the code expired, was never valid, or has already been redeemed | **no** — restart the flow |
| `429` | prose, not a code | more than 60 polls in a minute from this IP | yes, after backing off |
| `5xx` | anything | RomM or something in front of it is unwell | yes — the code is still good |
| anything else | | not a shape this client knows | **no** — surface the failure |

Four of those matter more than they look:

- **Never poll faster than `interval`, whatever it says.** RomM restarts its
  pacing window on *every* poll it answers, `slow_down` included — so a client
  that undercuts the interval gets `slow_down` for every poll after the first
  and never recovers, until the code expires under it. That is why
  `DeviceInitResponse::poll_interval()` has a floor and no ceiling below the
  code's own lifetime: clamping the server's value *down* is the one adjustment
  that can stop a pairing from ever completing.
- **`slow_down` is per `device_code`, `429` is per IP.** Honour `interval` and
  neither happens. The `429` body is an English sentence
  (`"Too many polling attempts. Try again later."`), not a machine-readable
  code, so the *status* is what classifies it.
- **`expired_token` also means "already used".** The approved code is consumed
  by the poll that redeems it, so polling once more after a successful pairing
  is indistinguishable from polling a code that never existed. Stop polling the
  moment a token arrives; do not re-poll to confirm.
- **A `5xx` is retried; an unrecognised `4xx` is not.** A restarting container
  or a proxy having a bad minute says nothing about the `device_code`, which
  still has most of its 600 seconds — abandoning the pairing screen over a `502`
  throws that away. A status or a `detail` this client has never seen is the
  opposite case: polling a code that may already be dead until the rate limiter
  answers is worse than telling the user pairing failed.

## Where the OpenAPI snapshot and the running server disagree

`server/contract/romm-openapi-5.2.0.json` is what RomM *declares*; the captures
are what it *does*. Both were checked field by field against this worktree's
fixture, and on the **response bodies they agree exactly**: no field the
snapshot declares is missing from a live response, and no live response carries
a field the snapshot does not declare. Where they part company is everything the
snapshot does not model.

| The snapshot | The server | What we do |
|---|---|---|
| declares only `200` and `422` on `POST /api/auth/device/token` | also answers `400` with a `detail` string, and `429` when rate-limited | Table above; captured as `auth-device-token-pending.json`; typed as `auth::TokenPoll`. |
| declares `expires_at` as `string \| null` and says no more | sends `null` on every response 5.2.0 produces | `std::optional<std::string>`, persisted either way. Empty is "no expiry", never an error, and never a reason to re-pair. |
| says nothing about the order of `scopes` | returns them sorted alphabetically, not in the order requested | Read as a set (`DeviceTokenResponse::HasScope`), never by index. |
| says nothing about `scopes` being a subset | really does return only the subset that was approved | Read back and used to disable features, per the note above. |
| says nothing about the `user_code` alphabet | draws from `ABCDEFGHJKMNPQRSTUVWXYZ23456789` | Displayed verbatim. The confusable characters are already excluded upstream, so nothing "corrects" them. |

One thing the snapshot got right and an earlier draft of this page got wrong:
`POST /api/auth/device/init` answers **`201`**, which the snapshot declares and
the server does. Fixed above.

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
`sdmc:/config/rommsync/token.dat`. There is no refresh token to store.

The parser refuses a token or a `device_id` that is blank, or that carries an
embedded NUL — `"rmm_a\u0000EVIL"` is legal JSON that `std::string` holds
faithfully and every C API downstream truncates, so the value that would be sent
is not the value that was checked. `expires_at` is the same: `null` means "no
expiry", `""` is refused rather than handed to a timestamp parser. See
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
