# Security & exposing RomM for internet sync

## Posture

The Switch client **calls out** to RomM over HTTPS. Nothing listens inbound on
the console. This is the whole reason we don't reuse the old server-polls-FTP
bridge for internet sync — exposing the Switch's FTP to the internet would be
both insecure and unreliable (the console is usually off).

## Exposing RomM safely

RomM listens on `1515` (→ container `8080`) with no TLS of its own. To reach it
from any network, put it behind an HTTPS reverse proxy — **do not** port-forward
`1515` directly.

Recommended: a reverse proxy (Caddy / Traefik / nginx) terminating TLS with a
real certificate on a hostname you control, e.g. `https://romm.example.com`.

- Prefer restricting exposure further if you can: a VPN/WireGuard or Tailscale
  tailnet means the Switch reaches RomM without RomM being on the public
  internet at all. If your Switch/router supports it, this is the safest option.
- If publicly exposed, keep RomM updated, use strong credentials, and consider
  the proxy enforcing rate limits.

A sample Caddyfile lives in `server/README.md`.

## A self-signed certificate on a home server

A home RomM behind a proxy with a certificate no public CA signed is a case this
client has to answer for, because the wrong answer is one line and it is the line
everyone reaches for: turning certificate verification off.

M0-1 established what the console actually offers here
([DEVELOPMENT.md](DEVELOPMENT.md#m0-1-the-measurement-and-the-decision)). The
Horizon `ssl` service verifies against the console's own CertStore, with
`PeerCa | HostName` set by default, and there are two ways past it:

- **Import the certificate** (`sslContextImportServerPki`). Verification stays
  **on** — the server is checked, against a CA the user chose. Hostname checking
  stays on too: set SNI to a name the certificate carries and the server may
  still be addressed by IP. This is the supported path, and it is what
  `ClientOptions::ca_bundle_path` in
  [`core/include/rommsync/http.hpp`](../core/include/rommsync/http.hpp) is for.
- **Turn verification off** (`SslOptionType_SkipDefaultVerify`, then
  `SetVerifyOption(0)`). This is not "trust this one server": it is trust
  *anything* on that connection, so a machine on the same network can be RomM as
  far as the console is concerned — and the bearer token goes to whoever
  answers. `ClientOptions::verify_peer = false` is this. There is no
  `config.ini` key for it today ([CONFIG.md](CONFIG.md)); if one is ever added it
  has to be an opt-in a user typed — never a default, and never inferred from a
  handshake that failed.

The test fixture takes the first path deliberately, even though it is the more
awkward one for a throwaway certificate: `scripts/orca/tls-fixture.sh` mints a
certificate with a `subjectAltName`, and `tls.cert` fails if it ever stops doing
so. A harness that got used to running with verification off is how a client
ships with it off.

## Tokens & scopes

- Create a **dedicated RomM user** for the Switch (or at least a dedicated client
  token), so it can be revoked without touching your main account.
- The device-code flow issues a scoped bearer token. Request only the scopes in
  [API_CONTRACT.md](API_CONTRACT.md#scopes-to-request).
- Revoke via RomM's client-tokens UI / `DELETE /api/client-tokens/{id}`; the
  sysmodule handles the resulting `401` by prompting re-pair. This is the only
  way to make a token that has been on an SD card stop working — deleting the
  file is not.

## Token at rest on the SD

The SD card is readable by any homebrew on the console and by anyone who pulls
the card, and Horizon's FAT32 has **no permission bits** — there is nothing to
restrict, and a client that claimed to restrict them would be claiming a
mitigation it does not have. So the token is at rest in the clear, and that is
a fact to design around rather than one to hide:

- Its blast radius is exactly the scopes granted to one dedicated user. That is
  the mitigation, not filesystem secrecy — which is why the requested scopes are
  the documented minimum, checked by a test
  ([API_CONTRACT.md](API_CONTRACT.md#scopes-to-request)) rather than asserted in
  a comment, and why `me.write` is requested by nobody.
- It is revocable. `DELETE /api/client-tokens/{id}` ends it, and the sysmodule
  treats the resulting `401` as revoked rather than retrying it.
- Store `token.dat` under `sdmc:/config/rommsync/` and keep it minimal: no
  refresh token to store, and nothing in the record that RomM did not issue.

What the client owes the user is that the secret never leaves that file by
accident:

- **Never logged.** Not by the store, not by the parser, not by the pairing
  state machine, and not in the IPC payload the overlay renders. `json::Error`
  never quotes a value; `DescribeStoredToken` is the one supported way to
  summarise a pairing for a log or a diagnostics screen, and it reports the
  token by length only. `core.token_store` asserts this with a needle rather
  than leaving it to inspection.
- **Genuinely discarded.** "Re-pair" and factory reset remove `token.dat` *and*
  the `.tmp`/`.old` an interrupted commit can leave beside it. The **sweep** is
  the part that does the work: unlinking only `token.dat` while `token.dat.old`
  still held the same bearer token would have discarded nothing, and that is
  what the test checks. Each file is zeroed before it is unlinked, but that
  overwrite claims nothing — there is no `fsync` reachable from the standard
  library, so a filesystem may drop the zeroed pages of an inode it is about to
  unlink and never write them, and wear levelling can preserve the original
  blocks regardless. Treat a token that has ever been on a card as recoverable
  from that card, and **revoke it on the server** if that matters.
- Never commit a real `config.ini`/`token.dat` (see `.gitignore`).

## The console identifier

RomM needs a value that is the same on every pairing of this console and
different on another one. It does **not** need to know which console that is, so
what it gets is `nx-` + the first 128 bits of a salted SHA-256 of the serial —
never the serial itself, which identifies the hardware and, through a warranty
record, a person. Details and the fallback for a platform with no stable value
are in [AUTH.md](AUTH.md#client-identifier).

The salt is domain separation, not secrecy: this repository is public, so the
derivation is reproducible by anyone holding a serial to try. The property that
protects the user is that the serial never leaves the console — not in a request
body, not in `device.dat`, not in a log.

The identifier survives a re-pair on purpose. Discarding it with the token would
register the console in RomM a second time and give every save an empty sync
history, which is a data problem dressed as a privacy improvement.

The opposite mistake is worse, and the platform layer is where it would happen:
a seed provider that substitutes a placeholder when it cannot read the serial
gives *every affected console the same identifier*, so RomM treats them as one
device and one console's saves overwrite another's. It must fail instead. The
engine refuses a stable value too short to be a serial, which catches the shape
that mistake usually takes but cannot catch a plausible-looking constant.

## What we never do

- No inbound listeners on the Switch.
- No third-party services — the client talks only to your RomM.
- No telemetry.
