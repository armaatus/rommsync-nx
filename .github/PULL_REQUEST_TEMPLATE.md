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

## Plan

<!-- plans/<issue>-<slug>.md, and anything the implementation did differently.
     Departing from the plan is normal; departing silently is not. -->

## Review findings

<!-- Paste the /code-review output. REVIEW.md is the policy it follows: three
     passes, Important before Nit, at most five nits. Required, not optional --
     it is what makes a human review tractable. -->

## Issues edited

<!-- Any issue this work invalidated -- yours or another -- and why. Three
     worktrees cannot see each other; these bodies are the only channel.
     "None" is a fine answer when it is true. -->

## Checklist

- [ ] Scoped to one issue; the issue is referenced in the commits (`M?-?: ...`).
- [ ] `ctest --test-dir build --output-on-failure` is green, and there is a test
      that would have failed before this change.
- [ ] `/code-review` ran on this branch and its findings are above.
- [ ] `plans/<issue>-<slug>.md` matches what was built.
- [ ] No secrets in the tree (`config.ini`, `token.dat`, `*.token`, `.env` stay git-ignored).
- [ ] For network paths: timeout, offline-safe, backoff; never blocks boot.
- [ ] Never overwrites a save without a backup (see `docs/SYNC_PROTOCOL.md`).
- [ ] Any RomM schema used was verified against `server/contract/`; `docs/API_CONTRACT.md` updated if it drifted.
- [ ] Docs updated where behavior changed.

## Notes / risks

<!-- TLS/heap/IPC gotchas, follow-ups, anything a reviewer should know. -->
