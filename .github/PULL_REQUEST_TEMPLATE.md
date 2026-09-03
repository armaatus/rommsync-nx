<!-- Thanks for contributing to rommsync-nx! Keep PRs scoped to a single issue. -->

## Summary

<!-- What does this change and why? -->

Closes #<!-- issue number -->

## Type of change

- [ ] Docs / contract / protocol
- [ ] Sysmodule (`sys-rommsync`)
- [ ] Overlay (`ovl-rommsync`)
- [ ] Server-side (`server/`)
- [ ] Repo / tooling

## Checklist

- [ ] Scoped to one issue; the issue is referenced in the commits (`M?-?: ...`).
- [ ] No secrets in the tree (`config.ini`, `token.dat`, `*.token`, `.env` stay git-ignored).
- [ ] For network paths: timeout, offline-safe, backoff; never blocks boot.
- [ ] Never overwrites a save without a backup (see `docs/SYNC_PROTOCOL.md`).
- [ ] Any RomM schema used was verified against `server/contract/`; `docs/API_CONTRACT.md` updated if it drifted.
- [ ] Docs updated where behavior changed.

## Notes / risks

<!-- TLS/heap/IPC gotchas, follow-ups, anything a reviewer should know. -->
