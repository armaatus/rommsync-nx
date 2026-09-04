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
device, and it is the whole registration: `POST /api/devices` never matches on
`client_device_identifier`, so it creates *another* device however it is called
([API_CONTRACT.md](API_CONTRACT.md#why-post-apidevices-is-the-wrong-call)).
Registering a fresh device on each boot gives every save an empty sync history
and makes the first negotiation of every tick look like a first encounter. See
[Device registration](#device-registration) below.

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

## Running the flow: the pairing state machine

`auth.hpp` says what an answer *means*; [`pairing.hpp`](../core/include/rommsync/pairing.hpp)
runs the conversation. `PairingSession` is a state machine, not a loop, and both
halves of that matter:

- the overlay asks for the pairing state every time it redraws (`GetPairState`,
  [DEVELOPMENT.md](DEVELOPMENT.md#ipc)) and must never wait on a socket to get
  an answer — `status()` never touches the network and never blocks behind a
  poll that is in flight;
- a sysmodule may not park a thread in a ten-minute poll loop when *never block
  boot* is a hard rule. `Poll()` performs at most one request, returns
  immediately when the next one is not due, and leaves the scheduling to the
  caller.

```
      Begin()               init answers        approve in browser
idle ─────────▶ starting ───────────────▶ pending ─────────────────▶ approved
                                            │  ▲                     token(), then
                                            │  └── poll every         persist to
                                            │      `interval`,        token.dat
                                            │      backing off on
                                            │      a transient failure
                                            ├──────▶ denied   a human refused it
                                            ├──────▶ expired  ran out, or was spent
                                            └──────▶ failed   could not be completed
```

Four terminal states rather than one, because "you refused this on the website",
"the code ran out, here is a new one" and "your server rejected the pairing" are
three different sentences on the pairing screen, and only the last is worth a bug
report.

`starting` is there for the same reason from the other end: the init request can
take as long as `request_timeout`, and reporting `idle` — "nothing has been
started" — for thirty seconds after the user pressed Pair is indistinguishable
from not having registered the press. The owning thread never sees it; the
overlay, asking while the request is in flight, is exactly who does.

`PairingStatus` is the whole IPC payload: state, `user_code`, both verification
URLs, the countdown, the poll count and a log-safe message. It never carries the
`device_code` and never carries the token, so whatever renders it can be as
careless as a UI usually is.

### Retry policy

| What came back | What the session does |
|---|---|
| `authorization_pending` | poll again after `interval` |
| `slow_down` | back off — we already waited `interval`, so the server's window is wider than the number it sent |
| `429`, `5xx` | back off; the `device_code` is still good |
| no response at all — offline, a stall the timeout caught, a connection dropped | back off; the exchange never happened |
| `401`/`403` | back off, up to `max_rejected_polls` **consecutive** times, then fail naming the status |
| `access_denied`, `expired_token` | stop, on the matching terminal state |
| anything else | stop, `failed`, naming the status |

Backoff is the server's `interval` doubled per consecutive failure and capped by
`max_poll_backoff`; the interval is always the floor, never the ceiling — see
`DeviceInitResponse::poll_interval()` for why clamping it *down* is the one
adjustment that can stop a pairing from completing.

The `401` row is the one that argues with itself. The token endpoint takes no
credentials, so RomM has nothing to reject there: a `401` comes from something
in *front* of it — an authenticating reverse proxy, a gateway having a bad
minute. A blip deserves a retry. A proxy that will answer `401` every time must
not be allowed to burn the code's whole 600 seconds and then report "expired",
which is the one diagnosis guaranteed to send the user looking in the wrong
place. Hence a small budget and then a named failure, rather than either
extreme. `ClassifyTokenPoll` still calls it `kUnrecognized` — this is a policy
the session applies, not a grant state RomM defines.

The budget counts *consecutive* rejections, which is why every other outcome
clears it: a gateway that answers `401` once every few minutes is having a bad
minute, not demanding credentials, and abandoning a code with eight minutes left
over two of them an hour apart would report something that did not happen.

### Two things a poll loop gets wrong

**`POST /api/auth/device/init` is rate limited too: ten a minute, per IP.** Over
that it answers `429`, and it is the *init* that is limited, not just the poll.
A console pairs once, so nobody reaches it by hand — but a test suite that opens
a dozen codes does, and so does anything that retries pairing in a loop. The
session reports it as a distinct, transient message rather than as a generic
refusal, because "wait a minute and press Pair again" is the whole fix.

**A poll whose answer is lost has still redeemed the code.** RomM consumes an
approved code on the poll it *receives*; if the response never gets home — a
dropped connection, a stall the client timed out on — the token was issued into
a response nobody read, and every later poll answers `expired_token`,
indistinguishable from a code that never existed. There is nothing to recover
and nothing to retry. The session ends on `expired` so the overlay offers a
fresh code, which is why `pair.lost_grant` asserts on that ending rather than on
a completed pairing. Retrying instead would poll a dead code until the rate
limiter answered.

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
| gives `DeviceCreatePayload.allow_existing` a default of `true` and no other description | matches an existing device on `hostname` or `mac_address` only, so the flag deduplicates nothing on its own — and never matches on `client_device_identifier` | The client does not call `POST /api/devices` at all. [Device registration](#device-registration). |

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

`client_device_identifier` must be stable per console, because RomM keys a
device on it: send a different value on a re-pair and RomM does not recognise
the console — it registers a *second* device, and every save on it starts with
an empty sync history. [`device_identity.hpp`](../core/include/rommsync/device_identity.hpp)
is the derivation and both directions of the record.

It is **not** the console serial. The serial identifies the hardware and,
through a warranty record, a person; RomM needs neither. What it is:

```
nx-<32 lowercase hex>   = "nx-" + first 128 bits of
                          SHA-256("rommsync-nx/client-device-identifier/v1" \0 <serial>)
```

The salt is domain separation, not a secret — this repository is public, so
anyone holding a list of serials can hash them. What it buys is that the same
serial hashed for another purpose is a different string, so this is not a join
key across systems. The property that protects the user is that the serial never
leaves the console at all: only the digest is written, and only the digest is
sent.

A platform that offers nothing stable — or that offers something too short to be
a serial, which is what a placeholder looks like — gets a random identifier
instead, minted from at least 16 bytes of platform entropy and hashed the same
way. A seed provider must **fail rather than substitute**: a placeholder shared
across consoles derives one identifier for all of them, and RomM would let one
console's saves overwrite another's. A stable
value is preferred when there is one, because a *derived* identifier survives
losing `device.dat` — a wiped `config/` folder re-derives the same value and RomM
still recognises the console, where a random one would not.

**It lives in its own file, `sdmc:/config/rommsync/device.dat`, not in
`token.dat`.** "Re-pair" discards the token, and an identifier discarded with it
would be re-derived as a new one on the next pairing — exactly the duplicate this
exists to prevent. Written once, atomically, and after that **the file wins over
the seed, always**: a console that gains a stable serial after having been given
a random identifier keeps the random one. A file that cannot be *read* is an
error rather than a reason to mint, because a card having a bad moment is not
evidence that no identifier exists and minting over one cannot be undone. A file
that reads back and is not a record is replaced, since there is nothing left in
it to preserve.

## Device registration

Every sync call is scoped by a `device_id`, and pairing is what produces one.
There is no separate registration step: RomM creates the device row from the
`client_device_identifier` in `POST /api/auth/device/init`, and the token
response hands back its id. `POST /api/devices` would create a *second* device —
it matches on `hostname` or `mac_address` and on nothing else, not even for the
device-bound token of the device it is describing
([API_CONTRACT.md](API_CONTRACT.md#why-post-apidevices-is-the-wrong-call)).
[`device_registration.hpp`](../core/include/rommsync/device_registration.hpp) is
the module, and it only ever reads.

So the console's three states are:

| State | The record holds | What to do |
|---|---|---|
| `unpaired` | no token | pair |
| `unregistered` | a token and no `device_id` | pair — but **not** an error to raise at sync time |
| `registered` | both | confirm it, then sync |

The middle one is why this is a state and not a boolean. `SaveToken` already
refuses to write a record with a blank `device_id`, so it is not a file the
console can end up holding — but a token that names no device must never be
*used* either, because an empty id scopes nothing, and "not fully paired" and
"unpaired" have the same remedy.

**Confirm at boot, with one `GET /api/devices/{id}`.** It costs one request and
turns three things that would otherwise surface as a puzzling mid-sync failure
into a sentence before the first save is touched: a `401` (the token was
revoked), a `404` (the device was deleted in the web UI while the token stayed
valid), and `sync_enabled: false` (the user's own switch, which makes negotiate
answer `400 "Sync is disabled for this device"`).

The four outcomes are deliberately not collapsed, because the remedies differ:

| Outcome | Retry? | Re-pair? |
|---|---|---|
| `unreachable`, `server_error` | yes | no — a dropped connection is not a verdict on the pairing |
| `unauthorized`, `no_such_device`, `not_registered` | no | yes |
| `sync_disabled`, `ambiguous` | no | no — neither waiting nor re-pairing turns the switch back on |
| `malformed` | no | no — an answer this client cannot read is not one to hammer |

`server_error` covers the `429` and `408` a rate limiter or a reverse proxy
answers with, not only a `5xx`. They belong there because their remedy is
"wait", and the alternative is `malformed`, which has no remedy at all — a
rate-limited boot would wedge registration until the console was rebooted.

**Recovery is a search, not a registration.** A token with no `device_id` is
resolved by listing `GET /api/devices` and matching on
`client_device_identifier` — the row is already there, and that field is the only
thing pointing back at this console. Two rows carrying one identifier is a state
RomM permits and this client refuses to guess its way out of (`ambiguous`):
picking one would send this console's saves to whichever sorted first.

That listing returns **every** device the user owns, which sets how strictly a
neighbour's fields can be read. RomM stores `""` for `name`, `platform` and
`client` — `POST /api/devices {"name":""}` answers `201` with `"name":""` — so
holding those to the bar `expires_at` is held to would let one row written by a
browser session stop this console finding its own. They are read as "blank and
absent mean the same thing"; `id` and `sync_enabled`, which are acted on rather
than displayed, stay strict, and a value that is not a string or that carries an
embedded NUL is still refused.

Confirming does not prove the device is this *console's*: the returned
`client_device_identifier` says that, and it is handed back rather than checked,
because a device RomM created some other way legitimately carries none. It would
not catch a cloned SD card either — `device.dat` travels with `token.dat`, so
both consoles present the same identifier.

`ResolveRegistration` falls back to that search only when the cached id names no
device. Widening it inverts a diagnosis — a console that could not reach its
server would be told by a second, luckier request that its device is fine.

Caching the resolved id writes `token.dat` only when it actually changed, which
is never after the first boot — and leaves the in-memory record untouched when
the write fails, so a retry writes instead of short-circuiting on an id that
never reached the disk.

## Scopes

`MinimumScopes()` in [`pairing.hpp`](../core/include/rommsync/pairing.hpp) is
what the client requests, and it is exactly the unconditional list in
[API_CONTRACT.md](API_CONTRACT.md#scopes-to-request) — pinned to that document by
the `auth.scopes` test, so the two cannot drift. Every `.write` in it is one the
client performs; `me.write` is documented for recording play sessions, which
this client does not do, so it is not requested. RomM may approve a subset, which
is why the granted set is read back off the token response rather than assumed.

## Token storage

Persist `{access_token, device_id, scopes, expires_at, server_url}` to
`sdmc:/config/rommsync/token.dat`. There is no refresh token to store.
[`token_store.hpp`](../core/include/rommsync/token_store.hpp) is the record and
both directions of it.

`server_url` is in there because a token only means anything against the RomM
that issued it: a user who repoints the sysmodule at a different server has to
re-pair rather than send a stranger a bearer token.

**The write is atomic.** The record goes to `token.dat.tmp` and is committed onto
`token.dat` only once it is complete, so a reader sees either the previous token
or the new one and never a splice — the same reasoning as backup-before-overwrite
for saves ([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md)) and as `DownloadTarget`'s
`.part` file, applied to the one file that cannot be re-fetched without a human
at a browser. Every failure before that rename leaves whatever was already there
completely intact: a failed write costs the *new* token, never the working one.
What `rename` does not give is durability across a power cut, which needs an
`fsync` the C++ standard library does not expose; the promise is that no reader
ever sees a partial token.

**The commit is two renames, because Horizon's is not a replace.** POSIX
`rename` replaces the destination atomically; `fsFsRenameFile`, which libnx's
`fsdev` maps it to, refuses a destination that already exists — and on a re-pair
the destination always exists. So the record already in place is moved to
`token.dat.old` first, on *both* platforms deliberately: a fallback taken only
on the console is a path no host test ever runs, and the v1 gate would be the
first thing to see it. The cost is one moment where `token.dat` does not exist
and `token.dat.old` holds the previous record, so `LoadToken` reads that one
rather than reporting that nothing was ever paired. `core.token_store` covers
the recovery; the rename behaviour itself is a property of the platform, not
something a host test can force.

A record that could not be read back is refused on the way *out*, not just on
the way in — every field, `expires_at` included: `null` means "no expiry" and is
fine, but a *present* empty string is refused on the way back in, so writing one
would produce a file that exists, looks paired, and cannot be read — a file that exists and holds an unusable token is worse than no
file, because the sysmodule would find one, believe it is paired, and `401` on
every tick. Creating `sdmc:/config/rommsync/` is the platform layer's job, not
the portable engine's; a missing directory is a named error.

The parser refuses a token or a `device_id` that is blank, or that carries an
embedded NUL — `"rmm_a\u0000EVIL"` is legal JSON that `std::string` holds
faithfully and every C API downstream truncates, so the value that would be sent
is not the value that was checked. `expires_at` is the same: `null` means "no
expiry", `""` is refused rather than handed to a timestamp parser. See
[SECURITY.md](SECURITY.md) for the threat model — the SD is readable by anything
on the console, so treat the token as sensitive and scope it minimally.

**Nothing here reaches a log.** `json::Error` never quotes a value, the store's
messages name the path and the failure and never the record, and
`DescribeStoredToken` is the one right way to summarise a pairing for a log or a
diagnostics screen: which server, which device, which scopes, and that a token
exists, by length only. `core.token_store` asserts it rather than trusting it —
every failure path is run with a distinctive needle for the token and the device
code, and the output is searched for both.

## Re-pairing / revocation

- The overlay offers "Re-pair" → discards the token and restarts the flow.
  `DiscardToken` removes `token.dat` **and** the `.tmp`/`.old` an interrupted
  commit leaves beside it: unlinking only the obvious file would leave the same
  bearer token under a name nobody looks at. Each is zeroed first, which claims
  nothing on flash — see [SECURITY.md](SECURITY.md#token-at-rest-on-the-sd). It
  does not touch `device.dat` — see [Client identifier](#client-identifier).
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
