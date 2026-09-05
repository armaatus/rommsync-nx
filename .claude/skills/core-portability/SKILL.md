---
name: core-portability
description: >-
  Apply when adding, moving or including a header in core/, when adding a
  dependency to the portable engine, or when a change needs a platform facility
  (sockets, TLS, filesystem, threads, time, logging) from inside core/. Triggers
  on core/, HttpClient, libnx, devkitPro, switch.h, curl, platform header,
  interface, backend.
---

# core/ stays platform-free

Hard rule 4 in CLAUDE.md. `core/` is the engine -- auth, sync, downloads,
config, state -- and it compiles for a laptop and for Horizon from the same
source. That is what makes the whole test suite possible: every behaviour worth
proving is proven natively, against a real RomM, before anything runs on a
console.

## The rule

**Nothing in `core/` may include a host-only or libnx header.** CI enforces the
mechanical half of this (`core/ includes nothing platform-specific` in
`.github/workflows/ci.yml`): a `#include` in `core/include` or `core/src` must be
either `<a_standard_header>` or `"rommsync/…"`. Nothing else.

## What to do instead

Platform detail lives behind an interface that `core/` owns and someone else
implements:

- `core/` declares the interface (`rommsync/http_client.hpp` is the standing
  example -- see M0-2).
- `host/` implements it for the desktop and for CI (libcurl).
- `sysmodule/` implements it for Horizon (libnx).
- Tests inject a native implementation, so no test needs a Switch.

When a change seems to need a platform facility inside `core/`, the answer is a
new method on an existing interface, or a new interface -- not a conditional
include and not a `#ifdef __SWITCH__`.

## Adding an interface

A header that later issues include is a **foundation**: CLAUDE.md's "Working in
parallel" says it lands alone, before anything that depends on it starts. Three
worktrees each inventing their own version of a shared header is the one merge
conflict worth serialising to avoid. Say so in the PR body.

See [docs/ARCHITECTURE.md](../../../docs/ARCHITECTURE.md) for the layering.
