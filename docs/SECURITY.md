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
the card. Therefore:

- Store `token.dat` under `sdmc:/config/rommsync/` and keep it minimal.
- Treat the token as a **bearer secret**: its blast radius is exactly the scopes
  granted to one dedicated user — that's the mitigation, not filesystem secrecy.
- Never log the token. Never commit a real `config.ini`/`token.dat` (see
  `.gitignore`).
- On "Re-pair" or factory reset, delete `token.dat`.

## What we never do

- No inbound listeners on the Switch.
- No third-party services — the client talks only to your RomM.
- No telemetry.
