# Development environment

**Nothing here is built on the RomM server.** These components are compiled on a
dev machine with the devkitPro toolchain (or in CI). The server box only hosts
RomM and the contract snapshot/probe under `server/`.

For how work actually moves through this repo — issue to worktree to plan to PR,
and which parts run without a human — see [WORKFLOW.md](WORKFLOW.md). This file
is the toolchain; that one is the process.

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
- **The overlay is the exception to that line, and it is not a reversal.**
  `ovl-rommsync` links `-lcurl -lz -lminizip -lmbedtls -lmbedx509 -lmbedcrypto`
  because libultrahand's own objects reference them (its updater and its package
  handling), and `--gc-sections` cannot drop a symbol a linked object still
  names. The overlay is a launched process with the applet's memory, not a
  resident sysmodule in a `0x80000` heap, so the footprint M0-1 was measuring
  does not apply to it -- and `ovl-rommsync` itself opens no socket: everything
  it does over the network happens in `sys-rommsync` (overlay/AGENTS.md). The
  rule for the **sysmodule** is unchanged.
- Overlay: [libultrahand](https://github.com/ppkantorski/libultrahand) as a
  submodule at `overlay/lib/libultrahand`, compiled from source by
  `../switch.mk` as vendored code -- headers with `-isystem`, objects without
  `-Wextra -Wpedantic -Werror` -- and the `ULTR` signature appended to the
  `.ovl`. `git submodule update --init --recursive` before the first build; CI
  checks out with `submodules: recursive`.

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
packaging/           # what the release zip ships beside the two artifacts
  README.txt.in      # substituted with VERSION and the title id by package.sh
  config.ini.example
scripts/package.sh   # the two build outputs -> dist/rommsync-nx-<VERSION>.zip
scripts/release-notes.sh  # the body a `v*` tag's GitHub Release carries
```

## Packaging

`make -C sysmodule` and `make -C overlay` produce `sys-rommsync.nsp` and
`ovl-rommsync.ovl`, and neither of those names or locations is what Horizon
loads. `scripts/package.sh` turns them into the install tree:

```
atmosphere/contents/<TID>/exefs.nsp    the sysmodule, RENAMED
switch/.overlays/ovl-rommsync.ovl      the overlay
config/rommsync/config.ini.example     a starting configuration
README.txt
LICENSE
```

```bash
make -C sysmodule && make -C overlay
./scripts/package.sh              # -> dist/rommsync-nx-<VERSION>.zip
./scripts/package.sh --list       # the entry paths, without building anything
```

Four things about it are load-bearing, and every one of them fails silently on a
console rather than loudly here:

- **`exefs.nsp`, not `sys-rommsync.nsp`.** Atmosphère loads the former. The
  build's own name installs cleanly, boots, and does nothing at all.
- **`<TID>` comes out of `sysmodule/sys-rommsync.json`**, never typed. The id is
  still unconfirmed against the installed homebrew set (`sysmodule/README.md`),
  so it will move, and a second copy of it is how a move lands in one place only.
- **No `flags/boot2.flag`, and no `config.ini`.** The sysmodule ships disabled
  (#33), and an upgrade is this same zip unpacked over the top — replacing a
  user's settings is not something a release may do. `token.dat`, `device.dat`,
  `state.db` and `queue.json` are not in the archive either, so they survive.
- **The archive is byte-deterministic** — fixed entry order, timestamps and
  permissions — so the checksums a release publishes describe the build and not
  one upload.

`ctest -R package` is what holds all of it: four scenarios against stubbed
artifacts that need no toolchain, plus `package.builds`, which packages a real
devkitPro build inside the same container. `switch-build` runs the script on
every push as well, so a failure specific to packaging *inside* the container is
not something a tag discovers first.

## Releases

One version string, in `VERSION` at the repo root. Everything that reports a
version derives it from that file and nothing restates it:

| Reader | What it produces |
|---|---|
| `CMakeLists.txt` (`file(READ)`) | `rommsync/version.hpp` for the host build |
| `switch.mk` (`cat`) | the same header for the three devkitPro targets |
| `switch.mk` (`APP_VERSION`) | the overlay's NACP, which is what a user sees |
| `scripts/package.sh` | `dist/rommsync-nx-<VERSION>.zip` and the shipped `README.txt` |
| `scripts/release-notes.sh` | the release body |

A release is a `v*` tag on a commit whose `VERSION` says the same thing. The
bump goes through a pull request like every other change — nothing pushes to
`main` here, and `guard.py` will not let it — and only the tag is pushed
directly:

```bash
git switch -c release-0.2.0
echo 0.2.0 > VERSION            # or 0.2.0-rc1 for a prerelease
git commit -am "Release 0.2.0"
gh pr create --fill             # ...and a person merges it

git switch main && git pull     # now the bump is on main
git tag v0.2.0
git push origin v0.2.0          # this is what publishes
```

Push the tag and the `release` job in
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) does the rest. It waits
for `static`, `host-tests` and `switch-build`, checks the tag and `VERSION` agree
and that the tagged commit is an ancestor of `main` — the three jobs say the
commit is *good*, nothing else says it was *reviewed*, and a tag can be pushed at
any branch head — then builds the three Switch targets in `devkitpro/devkita64`,
runs `scripts/package.sh`, and writes a `SHA256SUMS` beside the archive.

It publishes in that order too: the release is created as a **draft**, the two
assets are uploaded, both are pulled back *out* of the release through the API
and checked with `sha256sum -c`, and only then is the draft flipped to public. A
release is live the instant it is created, so anything less would leave a public
page advertising a truncated download whenever an upload failed. The body is
`scripts/release-notes.sh`: the install lines, what the build targets, the
commits since the previous tag, and those checksums.

Six things are worth knowing before you do it:

- **The tag and `VERSION` must agree**, or the build is red rather than a release
  labelled with a version nothing inside it reports. `ctest -R version.tag` is
  that check; it is a no-op off a tag, and `host-tests` runs it on one because
  GitHub sets `GITHUB_REF` for every step. Run it yourself first:
  `GITHUB_REF=refs/tags/v0.2.0 ctest --test-dir build -R version.tag`.
- **A semver suffix publishes as a prerelease.** `0.2.0-rc1` is marked one;
  `0.2.0` is not. That is read off `VERSION`, so the tag cannot disagree with it.
- **The checksums are computed beside the build.** The archive is
  byte-deterministic for one `zip` build, which is what makes a published
  checksum a statement about these bytes — one recomputed on another host is a
  checksum of something else.
- **A stable release's notes start at the last stable release**, not at the last
  release candidate. `v0.2.0` cut after a `v0.2.0-rc1` lists everything since
  `v0.1.0`; `v0.2.0-rc2` lists everything since `rc1`. `ctest -R release.history`
  holds both halves against a throwaway repo, since this one has no tags.
- **Re-cutting a tag means deleting its release first.** Creating a release for a
  tag that already has one is a hard failure, on purpose: the alternative is
  attaching these bytes to a release built from other ones.
- **The compatibility line in the notes is a target, not a result.**
  `ATMOSPHERE_TARGET` in `scripts/release-notes.sh` is the only place any
  Atmosphère version is written down in this repo, and nothing here has run on a
  console — the notes say so in the release itself. Confirming or correcting it
  is part of the M8-1 gate (#43); the fix is that one constant.

The zip is the only thing to download. `switch-build`'s per-push artifact is the
three loose files, for debugging; a `.nsp` under its build name installs cleanly
and does nothing at all.

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
one in-flight download buffer + the bsd transfer memory above + the `state.db`
baseline, and stream to file rather than buffering whole roms in RAM. The
baseline is the one item that grows with the library rather than being a fixed
buffer — its text and then its parsed rows, bounded by `state::kMaxStateBytes`
and `state::kMaxRecords`, which are sized against the 390 KiB this section
leaves free and have to move with it (`core/include/rommsync/state_db.hpp`).

## IPC

The one surface `ovl-rommsync` talks to `sys-rommsync` through. Modelled on
ovl-sysmodules / sys-clk: a `cmif` service the overlay opens by name. The
contract is `core/include/rommsync/ipc.hpp`; this section is the published
summary of it, and `ipc.commands` reads the table below on every `ctest` so the
two cannot be renumbered apart.

### The command set

Ids are **stable and never renumbered**: the overlay and the sysmodule ship as
separate files and a user will pair an old one with a new one, so an id that
changed meaning is a call that does something other than what the caller asked.
A removed command leaves a hole.

| id | command | in -> out | errors |
|---|---|---|---|
| 0 | `GetInterfaceVersion` | - -> `u32` | never fails; **shape frozen forever** |
| 1 | `GetStatus` | - -> `Status` | never fails |
| 2 | `GetConfig` | - -> `Config` + `Diagnostic[]` | never fails |
| 3 | `SetConfig` | `ConfigEdit` -> `ConfigResult` (outcome + `Diagnostic[]`) | never fails; the outcome is `applied \| invalid \| write_failed` |
| 4 | `SetEnabled` | `bool` -> `EnabledResult` (outcome + the new **effective** state) | never fails; same outcome set |
| 5 | `SyncNow` | - -> `accepted \| already_running \| not_configured \| unauthenticated \| disabled` | never blocks |
| 6 | `StartPair` | - -> `PairingStatus` | `kNotConfigured` |
| 7 | `GetPairState` | - -> `PairingStatus` | never fails |
| 8 | `Unpair` | - -> - | `kWriteFailed` |
| 9 | `Enqueue` | `rom_id` -> position | `kUnknownRom`, `kQueueFull`, `kDuplicate`, `kMultiFile` |
| 10 | `Dequeue` | `rom_id` -> - | `kNotQueued` |
| 11 | `ListBegin` | `ListRequest` -> cursor | `kBadCursor`, `kOffline` |
| 12 | `ListNext` | cursor -> `ListPage` | `kBadCursor`, `kOffline` |
| 13 | `ListEnd` | cursor -> - | `kBadCursor` |

Rows 3 and 4 answer a `WriteOutcome` inside a successful reply rather than a
failing `Result`, and that is forced rather than chosen: a `cmif` reply's data
words are not delivered to a client whose `Result` says the call failed (libnx's
`cmifParseResponse` returns before it exposes them). `SetConfig`'s diagnostics
and `SetEnabled`'s effective state *are* those two refusals, so attaching them
to a failure would be attaching them to something nobody can read. Commands
whose failure carries nothing -- `Unpair`, `Enqueue`, `ListNext` -- keep
reporting it as an `ipc::Error`, which the sysmodule maps to a `Result`.

Three more errors belong to the transport rather than to any one command:
`kUnknownCommand` (an id this build does not implement -- the two halves are
different releases), `kMalformedRequest` (a request payload that did not decode)
and `kTooLarge` (a response that would not fit the cap).

A fourth, `kUnavailable`, belongs to *this stage of the project* rather than to
the wire: the sysmodule hosts the whole command set from M4-1 on, because the
overlay needs a service to talk to, while the machinery behind half of it is
still being built. A command whose engine does not exist yet answers
`kUnavailable` -- nothing attempted, nothing changed -- rather than a plausible
refusal that would send a user looking for a full queue or a failing SD card.
Each of M3-2, M5-3, M5-4 and M7-2 removes its own use of it, and the last one to
go is what says the engine is finished (`sysmodule/source/engine.hpp`). M3-2
took the queue commands off that list and M5-3 took `SetConfig` and `SetEnabled`;
what is left is `Unpair`, `StartPairing` and the three list commands.

`SyncNow` is the one command that cannot say it, and the reason is the seam
rather than a choice: `ipc::Engine::RequestSync()` is a `bool`, so an engine
that has not been built answers `false` and `ServiceCore` reports
`already_running` -- exactly the plausible refusal the rule above exists to
avoid. Widening `RequestSync` to an `Error` would be a contract change for one
caller that M7-2 is about to make true anyway, so it is recorded here and in
`engine.cpp` instead of papered over.

`Status` carries the interface version and the build, the enable switch, the auth
state (paired / unauthenticated / never paired), configured, online, the last
sync's time, result and counts, whether a tick is running right now
(`sync_in_progress`), the queue depth, and the **current download**
(`rom_id`, `fs_name`, `bytes_done`, `bytes_total`, state). That last field is how
M3-5 is served: one poll per frame, not a second round trip. Per-item queue
progress rides on the `queue` list kind (M5-4).

### Where the halves live

| Path | What |
|---|---|
| `core/include/rommsync/ipc.hpp`, `core/src/ipc.cpp` | ids, payloads, encoders and decoders |
| `core/src/ipc_service.cpp` | `ipc::ServiceCore` -- one method per command, and every decision |
| `sysmodule/source/ipc/service.*` | the `cmif` binding: buffers in, buffers out, `ipc::Error` to a `Result`. No logic. |
| `sysmodule/source/ipc/server.*` | hosting it: `smRegisterServiceCmif`, `svcAcceptSession`, the session table, `svcReplyAndReceive` |
| `sysmodule/source/engine.*` | the `ipc::Engine` `ServiceCore` reads the console out of, as far as it is built. Names no libnx type, so the host build compiles it too and `engine.*` drives it against a directory (`tests/test_engine.cpp`) |
| `overlay/source/ipc_client.*` | `smGetService("rommsync")` and the *same* codecs |
| `core/include/rommsync/overlay_status_view.hpp` | what the status screen *says*, decided off the framebuffer (`overlay.status`) |
| `core/include/rommsync/overlay_pairing_view.hpp` | the same for the pairing screen: the code, the address, the countdown, and which of four sentences a dead pairing gets (`overlay.pairing`) |

`core/` may not name a libnx type (hard rule 4), so the errors are a portable
`ipc::Error` and the sysmodule maps them to a Horizon `Result` at the boundary.
The pieces the commands drive -- the download queue (M3-2), the scheduler (M7-2),
live config writes (M5-3), list paging (M5-4) -- sit behind the `ipc::Engine`
interface, which is what lets the whole command set be driven end to end by the
host harness against a real RomM today (`ipc.engine`).

### The framing

Every command is the same shape: **a JSON object in, a JSON object out**,
serialised with `rommsync::json`. A command that takes nothing sends `{}`; one
that answers nothing answers `{}`. That is what lets the sysmodule side be a pure
dispatch -- one entry point taking a command id and two buffers -- rather than
fourteen hand-marshalled stubs, each a place to get a length wrong on a device
with no debugger attached.

`GetInterfaceVersion` is command 0 and its encoding is frozen forever:
`{"interface":<u32>}`, from every build there will ever be. An overlay from a
newer release meets an older sysmodule on somebody's SD card, and this is the one
call it makes before it knows whether it can decode anything else -- so it can
say "update the sysmodule" instead of decoding garbage.

### The bounds

`ipc::kMaxPayloadBytes` (8 KiB) caps every single request and response, in both
directions. The inner heap is `0x80000` with ~390 KiB left after the trimmed bsd
transfer memory ([M0-1](#m0-1-the-measurement-and-the-decision)), and that budget
already owes a download buffer and the `state.db` baseline.

Nothing on this wire may grow with the size of the library or of `config.ini`:

- Lists page. `kMaxPageSize` is a count cap and is *not* the binding one -- a
  rom's name is the user's data -- so a producer fills a page with
  `ipc::AppendIfItFits`, which stops on whichever bound comes first.
- `GetConfig` never fails, so a `[platform.*]` map too large to send is dropped
  whole and flagged (`platforms_truncated`) rather than refused or served
  half-empty, and diagnostics are trimmed to `kMaxDiagnosticsInPayload` with a
  notice saying how many did not fit. That trim bounds *characters* and the wire
  carries *bytes* -- `json::Quote` doubles a backslash and makes six of a control
  character, both of which `config.cpp` quotes back out of a rejected path -- so
  the command drops complaints until the payload fits rather than trusting the
  constants.
- The three values neither half chooses are bounded where they enter: a
  `server.url` at `kMaxServerUrlBytes` (the same bound `NormalizeServerUrl`
  applies), a rom's `fs_name` at `kMaxNameBytes`, and a verification URL off a
  server response at `kMaxVerificationUrlBytes`. Each belongs to a command
  documented never to fail.

### The two rules

**No command blocks on the network.** `SyncNow` and `StartPair` hand work to the
engine thread and return; the overlay polls. `PairingSession::status()` is
already documented as safe from any thread and never blocking, for exactly this
reason.

**Nothing secret crosses.** No bearer token and no `device_code` appears in any
payload -- asserted over every command by `ipc.secrets`, not reviewed. The server
URL is a different thing: `NormalizeServerUrl` refuses `user:password@` outright,
so a configured URL carries no credential, and the settings screen exists to show
and edit it. It is therefore served by `GetConfig` and by the two verification
URLs a human has to type, and by nothing else -- never in a diagnostic or an error
string, which go to a log ([SECURITY.md](SECURITY.md)).

The overlay may **read** `config.ini` directly but never writes it; the sysmodule
owns writes ([ARCHITECTURE.md](ARCHITECTURE.md#3-shared-state-on-sd)).

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
