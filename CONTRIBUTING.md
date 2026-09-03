# Contributing

## Where things get built

- `sysmodule/` and `overlay/` are **Switch homebrew** — built on a devkitPro dev
  machine or in CI (`devkitpro/devkita64` docker image). See
  [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). **Do not build these on the RomM
  server.**
- `server/` is the only part that runs on the RomM host (a snapshot + a Python
  probe script).

## Workflow

1. Pick an issue (start at milestone **M0**; `good-first-issue` where labelled).
2. Branch from `main`: `feature/M2-2-rom-matching`.
3. Keep changes scoped to the issue. Reference it in commits (`M2-2: ...`).
4. CI must pass (warnings-as-errors). Open a PR; link the issue and fill in the
   PR template.

## Order of work

M0 (especially **M0-1**, the TLS spike) gates the networked milestones. Auth
(M1) → sync engine (M2) and downloads (M3) can proceed in parallel once the
`HttpClient` interface (M0-2) exists. The overlay (M4) can start against a stub
IPC and integrate later.

## Ground rules

- No secrets in the tree. `config.ini` and `token.dat` are git-ignored.
- Never overwrite a save without a backup (see
  [docs/SYNC_PROTOCOL.md](docs/SYNC_PROTOCOL.md)).
- Every network path: timeout, offline-safe, backoff. Never block boot.
- Verify any RomM schema against `server/contract/` before coding to it; if the
  server drifts, re-snapshot and update `docs/API_CONTRACT.md`.

## Commit attribution

See the repo's commit history for the established trailer format.

## Conduct & security

- By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).
- Found a vulnerability? Don't open a public issue — follow
  [SECURITY.md](SECURITY.md).
