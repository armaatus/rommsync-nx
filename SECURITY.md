# Security Policy

Thanks for helping keep **rommsync-nx** and its users safe.

This document is about **reporting vulnerabilities in this project**. For how to
*deploy and expose RomM safely* and how the client handles tokens on-device, see
[`docs/SECURITY.md`](docs/SECURITY.md).

## Supported versions

rommsync-nx ships as a release zip holding the sysmodule (`sys-rommsync`) and
the overlay (`ovl-rommsync`); its version is the first line of
`rommsync.log` and the **Build** row of the overlay's status screen. Fixes land
on `main` and go out in the next release of the current series.

| Version | Supported |
|---------|-----------|
| `1.0.x` (release candidates included) | ✅ |
| `main` (unreleased) | ✅ |

## Reporting a vulnerability

**Please do not open a public issue for security problems.**

Report privately through GitHub's **private vulnerability reporting**:

1. Go to the repository's **Security** tab.
2. Click **Report a vulnerability**.
3. Fill in what you found, how to reproduce it, and the impact.

This opens a private advisory visible only to you and the maintainers. If you
cannot use that flow, open a normal issue that contains **no exploit details**
and simply asks a maintainer to reach out for a private channel.

### What to include

- A description of the issue and the component affected (sysmodule, overlay,
  server probe, or a doc/protocol assumption).
- Steps to reproduce or a proof of concept.
- The impact you believe it has (e.g. token disclosure, save corruption,
  request forgery against a user's RomM).

### What to expect

- **Acknowledgement:** within a few days.
- **Assessment & fix:** we'll work with you on a fix and a coordinated
  disclosure timeline. Because this is a volunteer project, please allow
  reasonable time before any public disclosure.
- **Credit:** we're happy to credit you in the advisory unless you prefer to
  stay anonymous.

## Scope

In scope:

- The Switch client's handling of credentials/tokens, TLS, and save data.
- The sync protocol as implemented (data loss, save overwrite without backup,
  request forgery).
- The `server/` probe script and anything in this repository.

Out of scope:

- Vulnerabilities in **RomM itself** — report those to
  [rommapp/romm](https://github.com/rommapp/romm).
- Misconfiguration of a user's own reverse proxy / network exposure (see
  [`docs/SECURITY.md`](docs/SECURITY.md) for hardening guidance).
- Anything requiring a modified/malicious Atmosphère or physical control of an
  unlocked console.

## Our security commitments

- No inbound listeners on the Switch — the client only ever calls *out* to
  your RomM. `https://` is the recommended scheme; plain `http://` works, sends
  the token in the clear, and is meant for a trusted LAN only (see
  [`docs/INSTALL.md`](docs/INSTALL.md)).
- No third-party services and no telemetry.
- Secrets (`config.ini`, `token.dat`, `device.dat`) are never committed; see
  [`.gitignore`](.gitignore).
