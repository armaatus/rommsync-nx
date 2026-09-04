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
make -C overlay                                  # ...and the overlay
make -C tlsprobe                                 # ...and the M0-1 TLS probe
```

The two devkitPro Makefiles are thin: each sets its target's variables and
includes [`../switch.mk`](../switch.mk), which holds devkitPro's own template
once instead of twice. The version both builds compile in comes from the
`VERSION` file at the repo root -- CMake reads it with `file(READ)`, `switch.mk`
with `cat`, and both substitute the same `version.hpp.in`.

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the **switch-dev**
  group: `devkitA64`, `libnx`, and portlibs.
- `pacman -S switch-dev switch-portlibs` after installing devkitPro's pacman.
- Relevant portlibs likely used: `switch-mbedtls` is **avoided** for the
  sysmodule -- M0-1 measured it at six times the code size of the `ssl` service
  path (see the TLS note); `switch-curl` only if it can be trimmed to fit.
- Overlay: [libultrahand](https://github.com/ppkantorski/libultrahand) as a
  submodule; build with its Makefile conventions and append the `ULTR` signature
  to the `.ovl`.

Recommended: build in the official devkitpro docker image
`devkitpro/devkita64` so contributors and CI match. See `.github/workflows/ci.yml`.

## Repo build layout

```
CMakeLists.txt       # host build entry point
switch.mk            # shared devkitPro rules, included by both Makefiles below
VERSION              # the one version string; read by CMake and by switch.mk
core/                # portable engine, host- and devkitPro-compatible
  include/rommsync/  # http.hpp -- the one HTTP surface the engine may use
  src/
host/                # host-only backends for core/'s interfaces; never built
  include/rommsync/host/   # for the Switch. Today: the libcurl HttpClient.
  src/
tests/               # CTest suites
sysmodule/
  Makefile
  sys-rommsync.json  # NPDM: title id, heap, service and syscall capabilities
  source/            # engine: auth, http(tls), sync, downloads, ipc, scheduler
overlay/
  Makefile
  source/            # gui screens, ipc client
  lib/libultrahand/  # submodule, from M4-1
tlsprobe/
  Makefile           # the M0-1 spike: a manually-launched .nro, never installed
  source/
```

## TLS in a sysmodule

The single biggest technical risk. Sysmodules run with a tight heap; a full
mbedTLS/OpenSSL stack is heavy.

- **Primary approach:** use the Horizon **`ssl` system service** through libnx
  (`sslInitialize`, `sslCreateContext`, `sslConnection*`) layered on `bsd`/socket
  services. This reuses the OS TLS + system cert store, keeping our footprint
  small. Prototyped by **M0-1**; the answer is below.
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
  `rommsync/` header, and `switch-build` compiles every `core/` translation unit
  with devkitA64 and links them into the sysmodule.

### M0-1: the measurement, and the decision

**Decision: go with the `ssl` service.** Provisionally — the qualifier is real
and is spelled out under *what is still unproven* below.

The experiment is [`tlsprobe/`](../tlsprobe/README.md): a manually-launched
`.nro`, not a sysmodule and not on the boot path, that does one HTTPS GET
through `ssl` and reports what each stage cost. It is built by CI on every push
and by `ctest -R switch.tlsprobe`, and it has **not been run** — see below.

**Code size.** Three aarch64 images, identical flags and libnx, differing only in
what they link (`./tlsprobe/measure-footprint.sh`, devkitA64 r29.2-1,
libnx 4.12.0-1, switch-mbedtls 2.28.10-1):

| image | `.text` | `.data` | `.bss` | Δ`.text` |
|---|---|---|---|---|
| baseline — libnx + newlib stdio | 113,806 | 14,264 | 19,392 | — |
| **`ssl` service** — sockets + the full handshake sequence | 176,513 | 15,216 | 19,712 | **+62,707** |
| mbedTLS — the same handshake in-process | 501,685 | 30,952 | 29,088 | +387,879 |

Borrowing Horizon's TLS costs **61 KiB of code**; carrying our own costs
**379 KiB**, six times more, before its per-connection heap. That is the whole
argument for the `ssl` service, and it is now a measurement rather than a claim.

**Heap.** libnx's `ssl` client allocates nothing: `nx/source/services/ssl.c`
contains no allocation at all, because the TLS record buffers, the certificate
parsing and the session cache all live in the `ssl` sysmodule's own process. What
a sysmodule pays for a TLS request is therefore:

1. **The bsd transfer memory**, allocated from *our* heap by `socketInitialize`,
   sized `sb_efficiency × page_round_up(tcp_tx_max + tcp_rx_max + udp_tx +
   udp_rx)` (libnx `_bsdGetTransferMemSizeForConfig`). This is the dominant term
   and the one place this design can go wrong by accident:

   | config | transfer memory |
   |---|---|
   | libnx default (`socketInitializeDefault()`) | **0x234000 — 2.25 MiB** |
   | the probe's trimmed config | **0x1D000 — 116 KiB** |

   `sys-rommsync`'s inner heap is `0x80000` (512 KiB). The default socket config
   does not fit in it and never will; the trimmed one leaves ~390 KiB. **Never
   call `socketInitializeDefault()` from the sysmodule** — that single line is
   the difference between a working engine and one that dies at
   `socketInitialize`.
2. **One in-flight buffer** — the download-streaming buffer, ours to size. Roms
   stream to file and never sit in RAM whole (hard rule 2's sibling: nothing is
   buffered that does not have to be).
3. **Transiently, the CA PEM** while `sslContextImportServerPki` copies it, for a
   self-signed server. Freed immediately after.

mbedTLS would add its own record buffers on top of its 379 KiB of code —
`MBEDTLS_SSL_IN/OUT_CONTENT_LEN` defaults to 16 KiB each, so ~32 KiB per
connection, plus X.509 chain parsing.

**Things the API forces on the design**, found while writing the probe and worth
knowing before the M8 backend is written:

- `ssl` does not create sockets, it *takes* one. The sequence is bsd first
  (`socket`, `connect`), then `sslContextCreateConnection`,
  `socketSslConnectionSetSocketDescriptor` (the libnx wrapper — the raw cmd
  wants the bsd-side descriptor), then `DoHandshake`. The descriptor the wrapper
  returns is ours to `close()`, *before* `sslConnectionClose`.
- **`SslIoMode_Blocking`'s timeout is five minutes** (libnx `ssl.h`). That is not
  a timeout a sync tick can wait out, and "never block boot" is a hard rule. The
  real backend needs `SslIoMode_NonBlocking` with `sslConnectionPoll`, or
  `sslConnectionSetIoTimeout` on [16.0.0+]. The probe sets `SO_RCVTIMEO` on the
  socket before handing it over and uses blocking mode; whether the service
  honours that socket option is one of the things a run would tell us.
- `SslVersion_Auto` is **TLS 1.0–1.2**; 1.3 needs [11.0.0+] and its own bit. A
  server that only offers 1.3 cannot be reached by a firmware-agnostic client, so
  the fixture terminator offers 1.2 as well (`ctest -R tls.serves` asserts it).
- A self-signed home RomM needs either `sslContextImportServerPki` (the probe's
  `ca_pem`) or `SslOptionType_SkipDefaultVerify` followed by
  `SetVerifyOption(0)` — on [5.0.0+] the service refuses to clear
  `PeerCa|HostName` without the former. Hostname verification can stay *on*
  against an IP-addressed fixture by setting SNI to a name the certificate
  carries, which is what `scripts/orca/tls-fixture.sh` mints.

**What is still unproven, and why.** No handshake has been executed by this code,
on hardware or in an emulator. The Ryujinx rung (docs/TESTING.md#rung-2--the-manually-launched-nro)
is not available in this environment: Ryujinx was discontinued in October 2024,
its GitHub mirrors answer HTTP 451, the maintained fork moved to a self-hosted
forge — and, decisively, running *any* title in it needs `prod.keys` and a
firmware dump from a console, which hard rule 1 forbids this project from having.
So the emulator rung is available in principle and not executable here.

Reading the emulator's source (a live mirror of the same code,
`src/Ryujinx.HLE/HOS/Services/Ssl/`) says what it would and would not have
proven, which is worth recording because it changes what the rung is *for*:

- Implemented for real: `SetSocketDescriptor` against its bsd socket table,
  `DoHandshake` via .NET `SslStream.AuthenticateAsClient`, `Read`/`Write`,
  `Poll`. So the transport half — sockets, handshake, I/O — is genuinely
  exercisable there.
- **Stubbed**: `ImportServerPki` (accepts the cert and discards it),
  `SetVerifyOption`, `SetSessionCacheMode`. Its handshake validates against the
  *host's* .NET trust store instead. So a green run in Ryujinx would say nothing
  about the certificate half of this design, which is precisely the half a
  self-signed home server depends on.

That asymmetry is the finding: the emulator can retire the transport risk and
cannot retire the PKI risk. The PKI half is settled on hardware at **M8**, behind
the **M8-1** gate, and the probe is the thing to run first when it is — an `.nro`
someone launches and closes, never an auto-boot sysmodule.

No auto-boot sysmodule was installed on any console for this spike, and no
console was involved in it at all.

Budget note: sysmodules declare their heap in `config.json`/`npdm`; size it for
one in-flight download buffer + the bsd transfer memory above, and stream to file
rather than buffering whole roms in RAM.

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

Two gates hold that shape in place, and neither is prose alone.
[The M0 exit gate](TESTING.md#the-m0-exit-gate) is what M1 waits for: nine claims
about the harness, each naming the test or command that demonstrates it, and the
one that would rot silently — *no test needs a console, an emulator, or a server
anyone would miss* — is re-checked on every `ctest` by the `policy.*` tests. The
[v1 gate](TESTING.md#rung-3--the-v1-gate-and-real-hardware-m8) is what hardware
waits for, and lives in issue **M8-1**.

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
schema before implementing. Its `--capture` output is committed under
`server/contract/captures/`, which is what [API_CONTRACT.md](API_CONTRACT.md),
[AUTH.md](AUTH.md) and [SYNC_PROTOCOL.md](SYNC_PROTOCOL.md) quote; `ctest -R
contract` re-captures and fails on drift, so those pages cannot quietly go
stale.

## Coding standards

- C++20, warnings-as-errors everywhere (`-Wall -Wextra -Wpedantic -Werror`),
  not just in CI.
- No secrets in the tree; `config.ini`, `token.dat` and `device.dat` are
  git-ignored, together with the `.tmp`/`.old` an interrupted commit leaves
  beside them.
- Every network call: timeout, offline-safe, ret/backoff. Never block boot.
