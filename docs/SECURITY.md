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

## Tokens & scopes

- Create a **dedicated RomM user** for the Switch (or at least a dedicated client
  token), so it can be revoked without touching your main account.
- The device-code flow issues a scoped bearer token. Request only the scopes in
  [API_CONTRACT.md](API_CONTRACT.md#scopes-to-request).
- Revoke via RomM's client-tokens UI / `DELETE /api/client-tokens/{id}`; the
  sysmodule handles the resulting `401` by prompting re-pair.

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
  the `.tmp`/`.old` an interrupted commit can leave beside it, overwriting each
  first. The overwrite is worth what it is worth: it removes the bytes any
  reader of the file system can see, and on a wear-levelling SD card it does not
  promise the old sectors are gone. Unlinking only `token.dat` while
  `token.dat.old` still held the same token would have discarded nothing, which
  is the failure the overwrite is incidental to and the sweep is the point of.
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

## What we never do

- No inbound listeners on the Switch.
- No third-party services — the client talks only to your RomM.
- No telemetry.
