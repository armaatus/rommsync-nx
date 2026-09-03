# Development environment

**Nothing here is built on the RomM server.** These components are compiled on a
dev machine with the devkitPro toolchain (or in CI). The server box only hosts
RomM and the contract snapshot/probe under `server/`.

## Toolchain

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

## Repo build layout (to be created under each component)

```
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
  swappable without touching sync/download logic.

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

1. **Host build + mock RomM** — the core engine (auth, sync, downloads, config,
   `state.db`, IPC protocol) compiles native and runs against a scriptable mock
   RomM that forces every edge case (401, conflict, partial failure, `Range`
   resume, multi-file skip). Fast, offline, runs in CI. This is the primary loop.
2. **Host build + docker RomM** — the same harness against a real RomM 5.2.0 in
   docker-compose (throwaway volume) to confirm the mock matches reality.
3. **Ryujinx NRO** — a manually-launched **NRO** (not a sysmodule, not on the boot
   path) built from the same core lib, run in Ryujinx against docker RomM. This is
   where the Horizon `ssl`/`fs`/socket path is exercised without hardware.
4. **Real hardware** — last, gated behind the v1 gate (milestone **M8**).

The sysmodule heap behavior under Atmosphère, boot scheduling, and the
Tesla/Ultrahand overlay UI are the only things that truly need a console — they're
deliberately the last things touched. Everything else is proven before then.

The **server contract** is testable off-console: `server/probe_contract.py`
exercises auth + negotiate + saves against a RomM and prints the real response
shapes — run it against the docker fixture (never production) to confirm any
schema before implementing.

## Coding standards

- C++20, warnings-as-errors in CI.
- No secrets in the tree; `config.ini`/`token.dat` are git-ignored.
- Every network call: timeout, offline-safe, ret/backoff. Never block boot.
