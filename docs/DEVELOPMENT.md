# Development environment

**Nothing here is built on the RomM server.** These components are compiled on a
dev machine with the devkitPro toolchain (or in CI). The server box only hosts
RomM and the contract snapshot/probe under `server/`.

## Toolchain

Two build systems, on purpose:

- **Host / `core/` — CMake + CTest.** This is where ~90% of development happens,
  so it gets the toolchain with per-test granularity and a globbed source list
  (no shared object list for parallel worktrees to conflict on).
- **Switch targets — devkitPro Makefiles.** devkitA64's build rules and the
  `.ovl`/`.nsp` packaging steps are Makefile-native; fighting that would mean
  debugging build tooling instead of building the app.

Both consume the same `core/` sources.

```bash
cmake -S . -B build && cmake --build build      # host
ctest --test-dir build --output-on-failure       # host tests
make -C sysmodule                                # devkitPro (in the container)
```

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the **switch-dev**
  group: `devkitA64`, `libnx`, and portlibs.
- `pacman -S switch-dev switch-portlibs` after installing devkitPro's pacman.
- Relevant portlibs likely used: `switch-mbedtls` is **avoided** for the
  sysmodule (see TLS note); `switch-curl` only if it can be trimmed to fit.
- Overlay: [libultrahand](https://github.com/ppkantorski/libultrahand) as a
  submodule; build with its Makefile conventions and append the `ULTR` signature
  to the `.ovl`.

Recommended: build in the official devkitpro docker image
`devkitpro/devkita64` so contributors and CI match. See `.github/workflows/ci.yml`.

## Repo build layout

```
CMakeLists.txt       # host build entry point
core/                # portable engine, host- and devkitPro-compatible
  include/rommsync/  # http.hpp -- the one HTTP surface the engine may use
  src/
host/                # host-only backends for core/'s interfaces; never built
  include/rommsync/host/   # for the Switch. Today: the libcurl HttpClient.
  src/
tests/               # CTest suites
sysmodule/
  Makefile
  source/            # engine: auth, http(tls), sync, downloads, ipc, scheduler
  include/
overlay/
  Makefile
  source/            # gui screens, ipc client
  lib/libultrahand/  # submodule
```

## TLS in a sysmodule

The single biggest technical risk. Sysmodules run with a tight heap; a full
mbedTLS/OpenSSL stack is heavy.

- **Primary approach:** use the Horizon **`ssl` system service** through libnx
  (`sslInitialize`, `sslCreateContext`, `sslConnection*`) layered on `bsd`/socket
  services. This reuses the OS TLS + system cert store, keeping our footprint
  small. Prototype this **first** (issue **M0-1**) — it de-risks everything else.
- **Fallbacks if the `ssl` service proves impractical in-sysmodule:**
  1. mbedTLS with a carefully sized heap and trimmed cipher suites.
  2. Split model: a tiny privileged sysmodule for scheduling + a companion
     applet/NRO that does the networking when foregrounded (less "auto", keep as
     last resort).
- Keep all networking behind an `HttpClient` interface so the TLS backend is
  swappable without touching sync/download logic. That interface is
  [`core/include/rommsync/http.hpp`](../core/include/rommsync/http.hpp) (M0-2):
  GET/POST, multipart with file parts streamed from disk, streamed
  download-to-file with `Range` resume, connect/total/stall timeouts and
  cancellation. The native backend is `host/src/curl_http_client.cpp`, the only
  file in the tree that names a transport library. Two CI checks keep it that
  way: `static` rejects any include in `core/` that is not a standard or
  `rommsync/` header, and `switch-build` syntax-checks `core/` with devkitA64.

Budget note: sysmodules declare their heap in `config.json`/`npdm`; size it for
one in-flight download buffer + TLS context, stream to file rather than buffering
whole roms in RAM.

## IPC

Model on ovl-sysmodules / sys-clk: a `tipc` or `cmif` service the overlay opens.
Commands: `GetStatus`, `SetEnabled`, `SyncNow`, `GetConfig`, `SetConfig`,
`ListPlatforms`, `ListRoms(page)`, `Enqueue(rom_id)`, `GetQueue`, `StartPair`,
`GetPairState`. Keep payloads small; stream large lists page-by-page.

## Testing without hardware

**Hard rule: no real Switch and no production RomM until v1 is proven
off-console.** Full strategy in [TESTING.md](TESTING.md); the ladder:

1. **Host build + real docker RomM** — the core engine (auth, sync, downloads,
   config, `state.db`, IPC protocol) compiles native and runs against a genuine
   RomM 5.2.0 in Docker. There is **no mock**: a passing test means the behaviour
   is real. Failure modes a healthy RomM will not produce on demand (401
   mid-sync, truncated body, dropped connection, stall) are forced by a fault
   proxy sitting in front of it. Runs in CI. This is the primary loop.
2. **Ryujinx NRO** — a manually-launched **NRO** (not a sysmodule, not on the boot
   path) built from the same core lib, run in Ryujinx against docker RomM. This is
   where the Horizon `ssl`/`fs`/socket path is exercised without hardware.
3. **Real hardware** — last, gated behind the v1 gate (milestone **M8**).

The sysmodule heap behavior under Atmosphère, boot scheduling, and the
Tesla/Ultrahand overlay UI are the only things that truly need a console — they're
deliberately the last things touched. Everything else is proven before then.

### Worktree isolation

Agents work in parallel worktrees and sync tests mutate saves by design, so each
worktree runs **its own** RomM. `scripts/orca/env.sh` derives a compose project
name and two ports from the worktree path into `.env`; `orca.yaml` runs it on
worktree creation and tears the stack down on removal. Only immutable, expensive
things are shared across worktrees — the checksum-pinned ROM cache and the
content-addressed ccache. Never hardcode a port; read `.env`.

Setup finishes by putting the environment where you can see it: a log tab
following the stack, a browser tab on this worktree's RomM already signed in as
the fixture admin (`scripts/orca/romm-browser.sh`), and — for a worktree created
from an issue — the spec submitted to the agent rather than left drafted in its
composer (`scripts/orca/agent-autostart.sh`). Both are conveniences and neither
can fail setup. See [TESTING.md](TESTING.md#the-romm-browser-tab).

Removal is the reverse: `scripts/orca/archive.sh` drops that worktree's stack and
volumes, leaving the shared caches alone. Note that only the Orca UI runs that
hook by itself — `orca worktree rm` skips `orca.yaml` hooks unless `--run-hooks`
is passed. For stacks orphaned that way, or by a worktree deleted with `rm -rf`,
`scripts/orca/reap.sh` lists them and `--yes` removes them; it errs towards
keeping anything it cannot prove stale. See [TESTING.md](TESTING.md#worktree-isolation).

The **server contract** is testable off-console: `server/probe_contract.py`
exercises auth + negotiate + saves against a RomM and prints the real response
shapes — run it against the docker fixture (never production) to confirm any
schema before implementing.

## Coding standards

- C++20, warnings-as-errors everywhere (`-Wall -Wextra -Wpedantic -Werror`),
  not just in CI.
- No secrets in the tree; `config.ini`/`token.dat` are git-ignored.
- Every network call: timeout, offline-safe, ret/backoff. Never block boot.
