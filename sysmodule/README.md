# sys-rommsync (sysmodule)

The background engine. **Skeleton** — it builds, it is packaged, and it does
nothing yet. Build on a devkitPro dev machine, never on the RomM server.

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

npdmtool prints a dozen `Failed to get <field> (field not present)` lines while
packaging. That is its normal chatter about optional NPDM fields — devkitPro's
own template produces the same output — with one line worth reading literally:
it says that about `process_category`, which `sys-rommsync.json` *does* set.
This npdmtool ignores the key; the packaged NPDM is byte-identical with it set
to 0, set to 1, or deleted. The value we want is the default, so nothing is
wrong today, and the key stays for parity with devkitPro's template — but do not
read that one line as noise if the process category ever has to change.

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
sys-rommsync.json     # NPDM: title id, heap, service and syscall capabilities
source/
  main.cpp            # service loop, scheduler
```

Planned as the engine lands: `http/` (the `ssl`-service HttpClient backend),
`ipc/` (the service the overlay opens), and whatever Horizon glue the sync and
download workers need. The portable half of all of that belongs in `core/`,
where it is testable without a console.

## Three things in `sys-rommsync.json` to settle before hardware

None of them matters before the **M8-1** gate, and all of them are wrong to
guess at now — they are recorded here so they are not discovered on a console.

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
- **Heap and capabilities.** `main_thread_stack_size` and the inner heap in
  `source/main.cpp` are the template's numbers, sized for a skeleton. The real
  budget is one in-flight download buffer plus a TLS context
  ([../docs/DEVELOPMENT.md](../docs/DEVELOPMENT.md#tls-in-a-sysmodule)); roms
  stream to file and never sit in RAM whole. `service_access` lists what the
  design commits to (`fsp-srv`, `set:sys`, `ssl`, `bsd:u`, `sfdnsres`, `nifm:u`,
  `time:s`) rather than `*`, so adding a service is a deliberate edit — and a
  missing one fails at runtime, on hardware, which is the reason to keep the
  list honest as each is first used.

## Where the work is

The core engine is built and proven natively (host build + a real RomM in
docker, never a mock) before any of it runs on a Switch — see
[`../docs/TESTING.md`](../docs/TESTING.md).

- **M0-1** — the sysmodule `ssl`-service TLS question, a *de-risking spike*
  (Ryujinx-first, off the boot path).
- **M1 onward** — auth, sync, downloads and the IPC service, in `core/` first.

See milestone M0 in [`../ISSUES.md`](../ISSUES.md).
