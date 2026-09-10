# sys-rommsync (sysmodule)

The background engine. It registers the `rommsync` IPC service and answers it;
everything behind that service is still arriving milestone by milestone, so the
commands whose machinery does not exist yet answer `ipc::Error::kUnavailable`
rather than a plausible refusal (see `source/engine.hpp`). Build on a devkitPro
dev machine, never on the RomM server.

```bash
make -C sysmodule            # -> sysmodule/sys-rommsync.nsp
make -C sysmodule clean
```

Anything not specific to this target lives in [`../switch.mk`](../switch.mk),
which both Switch targets include. Everything under `core/src` is compiled into
this one, so a `make` here is also what proves the portable engine still builds
for aarch64 — CI runs it on every push (`switch-build` in
[`../.github/workflows/ci.yml`](../.github/workflows/ci.yml)), and `ctest -R
switch` runs the same build locally when the `devkitpro/devkita64` image is
pulled.

npdmtool prints a run of `Failed to get <field> (field not present)` lines while
packaging. That is its normal chatter about optional NPDM fields — devkitPro's
own template produces the same output — with one line worth reading literally:
it says that about `process_category`, which `sys-rommsync.json` *does* set.
This npdmtool ignores the key; the packaged NPDM is byte-identical with it set
to 0, set to 1, or deleted. The value we want is the default, so nothing is
wrong today, and the key stays for parity with devkitPro's template — but do not
read that one line as noise if the process category ever has to change.

Three of those lines used to name `program_id`, `program_id_range_min` and
`program_id_range_max`. The config spelled them `title_id*`, npdmtool's
deprecated aliases, and it looks for the new names first and falls back — so the
build reported a missing field it then found under another name. M9-3 (#196)
renamed the keys; the packaged NPDM is unchanged.

## What the NPDM may be asked for

`sys-rommsync.json` is npdmtool's *input*. What the kernel enforces is the NPDM
inside `sys-rommsync.nsp`, and what the process actually does is whatever
survived `--gc-sections` in `sys-rommsync.elf` — so the interesting question is
about two built files and not about this tree. An SVC the NPDM does not grant is
not a soft failure: the kernel refuses it and the process dies.

[`../scripts/npdm-check.py`](../scripts/npdm-check.py) is what asks. It parses
the NPDM out of the `.nsp`, scans the linked ELF's executable sections for
`svc` instructions, and fails on any the NPDM does not declare;
`ctest -R npdm` runs it, and `--print` dumps the declared SVCs and the SAC. It
found two (#196), both now declared:

| SVC | Reached from | Why it is declared rather than removed |
|---|---|---|
| `svcReturnFromException` (0x28) | libnx's exception entry | It is the last thing the handler does. Undeclared, the process dies at the exact moment something else has already gone wrong, and the crash that would have been reported is replaced by one that cannot be. |
| `svcUnmapTransferMemory` (0x52) | `socketExit` → `bsdExit` → `_bsdCleanup` → `tmemClose` | `NetworkExit` calls `socketExit`, so this path runs. Whether the SVC is *issued* rests on libnx leaving `TransferMemory::map_addr` NULL for the bsd transfer memory — an internal of libnx's bsd client, not ours to guarantee. There is no structural way to drop it either: `_bsdCleanup` also runs on `bsdInitialize`'s own failure path. `svcCreateTransferMemory` (0x15) is already granted for the same object. |

The other direction — 18 SVCs are declared that nothing in the image calls — is
reported and not failed. Narrowing that set is M9-11's, and the script prints
the list it starts from.

## The two switches

Whether this process runs, and whether it syncs, are two different questions
with two different answers.

| Switch | Mechanism | What it decides |
|---|---|---|
| **ovl-sysmodules' boot toggle** | `atmosphere/contents/<TID>/flags/boot2.flag`, plus `pmshellLaunchProgram` / `pmshellTerminateProgram` for *right now* | whether this **process exists at all** |
| **ovl-rommsync's enable switch** | `[sync] enabled` in `config.ini`, written over `SetEnabled` (#29, #30) | whether a **resident process syncs** |

`toolbox.json` — shipped beside `exefs.nsp` by `scripts/package.sh`, and the
only reason ovl-sysmodules lists this module at all — declares
`"requires_reboot": false`. That is a promise about this process: it is
terminated where it stands, at any instant, with no shutdown hook, and relaunched
with no gap. Nothing here may hold state only in RAM, and nothing may write a
lock or a pidfile a fresh process would trip over. `ctest -R toggle` is that
promise checked; the long form, with the four states the overlay draws off these
two switches, is
[../docs/DEVELOPMENT.md](../docs/DEVELOPMENT.md#the-two-switches) and — for the
person holding the console — [../docs/INSTALL.md](../docs/INSTALL.md) step 2.

With `[sync] enabled = false` the process is resident, IPC answers, and no sync
runs: `sync::RunTick` returns `TickOutcome::kDisabled` before it opens a file or
sends a request, and `ServiceCore::SyncNow` answers `SyncOutcome::kDisabled`
rather than starting one — a user who pressed "Sync now" with the switch off is
told which switch to flip, not shown a spinner. `SetEnabled(true)` lifts it in
the running process; nothing here needs a reboot.

`[downloads] enabled` is a separate key and stays where the user left it: a
console with save sync switched off still drains its download queue
([../docs/CONFIG.md](../docs/CONFIG.md), `download.disabled`). "Nothing runs"
is a console with both off.

## Responsibilities

- Device-code auth + token lifecycle ([../docs/AUTH.md](../docs/AUTH.md))
- HTTPS via the Horizon `ssl` service ([../docs/DEVELOPMENT.md](../docs/DEVELOPMENT.md#tls-in-a-sysmodule))
- Save sync: negotiate → execute → complete ([../docs/SYNC_PROTOCOL.md](../docs/SYNC_PROTOCOL.md))
- Download queue worker (Range resume, hash verify) → emulator folders
- Scheduler (boot / interval / on-demand)
- IPC service for the overlay

## Structure

```
Makefile              # target-specific vars; the rules are in ../switch.mk
sys-rommsync.json     # NPDM: title id, heap, service and syscall capabilities.
                      #   Also where scripts/package.sh reads the title id and
                      #   the name it writes into the shipped toolbox.json.
source/
  main.cpp            # init, service registration, and the loop
  engine.hpp/.cpp     # the ipc::Engine ServiceCore reads the console out of
  http/
    http_wire.hpp/.cpp       # HTTP/1.1 over an abstract Connection. No libnx.
    ssl_http_client.hpp/.cpp # bsd + the `ssl` service under it. libnx only.
  ipc/
    server.hpp/.cpp   # the port, the session table, svcReplyAndReceive
    service.hpp/.cpp  # one cmif request in, one reply out. No logic.
```

`http/` is split in two on purpose (M1-7, #126). `http_wire.cpp` names no libnx
type, so the host CMake build compiles it and `ctest -R wire` drives it against
the real docker RomM through the fault proxy — the same eighteen scenarios
`http.*` holds libcurl's backend to, out of the same source file compiled twice
(`tests/test_http_native.cpp`). What is left in `ssl_http_client.cpp` is opening
a socket, handing it to `ssl`, and reading and writing through the connection
that comes back: the only part of the transport nothing can execute before the
M8-1 gate, and deliberately as small as it could be made.

Object files are named by basename, so nothing under `source/` may share one
with `core/src/` — `switch.mk` makes that an error rather than a silently
dropped translation unit.

Planned as the engine lands: whatever Horizon glue the sync and download workers
need. The portable half of all of that belongs in `core/`, where it is testable
without a console.

## `sys-rommsync.json`, and what is settled in it

The **M8-1** gate is still what confirms any of this on a console. Two of the
three items that used to be listed here as "wrong to guess at now" stopped being
guesses with M1-7 (#126), which gave this process a transport and so made its
heap and its capabilities things somebody had to add up. The third is still a
guess, and is still wrong to make now.

- **Title id `0x4200000000524D53`.** The `0x42…` range is where homebrew
  sysmodules live by convention (ldn_mitm, sys-ftpd, sys-con); the low bytes
  spell `RMS`. Nothing verifies it is unused. Confirm it against the installed
  set before anything is loaded on real hardware.
- **Filesystem access.** `filesystem_access.permissions` is
  `0xffffffffffffffff` — everything — which is devkitPro's template default and
  not a decision. It deserves the same treatment `service_access` gets below:
  narrowed to what the engine actually opens. It is also the capability with the
  most reach in a project whose second hard rule is about not destroying a
  player's save.
- **Heap and capabilities, settled.** `kInnerHeapSize` is `0xC0000` (768 KiB)
  and is derived term by term in the table above the constant in
  `source/main.cpp`, where a `static_assert` fails the build rather than the
  console if a term outgrows it. The dominant term is the bsd transfer memory,
  which `socketInitialize` takes out of that heap: the config in
  `source/http/ssl_http_client.hpp` needs **116 KiB**, and libnx's default needs
  **2.25 MiB** and cannot fit at all — so `socketInitializeDefault()` is the one
  call this process must never make. Add one in-flight transfer buffer to that;
  roms stream to file and never sit in RAM whole.

  `main_thread_stack_size` went from the template's `0x4000` to `0x8000` for a
  reason that is not the transport: the main thread is where `SdEngine::Load`
  parses `config.ini`, `queue.json` and `auth.json`, and `json::kMaxDepth` is 64
  — so a document a card left behind can recurse further than 16 KiB of stack
  allows, and the failure that causes has no console to report it on.

  `service_access` is what is used and nothing more — `fsp-srv`, `set:sys`,
  `ssl`, `bsd:u`, `sfdnsres`, `nifm:u`, `time:s`. M1-7 (#126) dropped `time:s`
  because nothing in that build called `timeInitialize`, and the `ssl` service
  does its own certificate date checks in its own process; M7-2 (#37) put it
  back with the `timeInitialize` beside `fsInitialize`, because the scheduler is
  what makes `std::chrono::system_clock` matter — `core/`'s sync and download
  paths call it, and a save stamped from an uninitialised clock is one
  docs/SYNC_PROTOCOL.md refuses as an epoch `updated_at`.

## Where the work is

The core engine is built and proven natively (host build + a real RomM in
docker, never a mock) before any of it runs on a Switch — see
[`../docs/TESTING.md`](../docs/TESTING.md).

- **M0-1** — the sysmodule `ssl`-service TLS question. Answered as far as it can
  be answered off-console: the spike is [`../tlsprobe/`](../tlsprobe/README.md),
  the decision is `ssl`, and the part that still needs a console is recorded with
  it.
- **M4-1** — the IPC service is hosted here now (`source/ipc/server.*`), so the
  overlay has something to open. None of it has run: it is exercised in Ryujinx
  as a manually-launched build first, never as an auto-boot sysmodule and never
  on hardware before the M8-1 gate.
- **M1 onward** — auth, sync and downloads, in `core/` first. Each one replaces
  its `kUnavailable` in `source/engine.cpp`; the last one to go is what says the
  engine is finished.

See milestone M0 in [`../ISSUES.md`](../ISSUES.md).
