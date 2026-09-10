# sysmodule/ — sys-rommsync

The background engine. Built for the console by devkitPro (`devkitA64` + libnx).
Runs under Atmosphère with a tight heap.

- This directory holds only the **Horizon glue**: the `ssl`-service HTTP backend,
  `fs` access, the IPC service, and the scheduler. All portable logic belongs in
  `core/`, where it can be tested without a console.
- **A file here that names no libnx type may also be compiled by the host CMake
  build, for a test.** `engine.*` is the one that is: it is the `ipc::Engine`
  behind the service, and since M3-2 its `Enqueue`/`Dequeue` change a file on the
  card, so "it compiles for aarch64" stopped being a description of what it does.
  `SdEngine::Load` takes its config directory for exactly that reason --
  `sdmc:` was the only thing tying the class to a console. This is not a licence
  to move logic here: it is what makes the glue that has to live here provable.
  Anything that includes `<switch.h>` is devkitPro's alone (`ipc/`, `main.cpp`,
  `http/ssl_http_client.*`). `http/` is the worked example of the rule since
  M1-7 (#126): `http_wire.*` and `posix_connection.*` name no libnx type and are
  driven against the real docker RomM by `wire.*`, and `ssl_http_client.*` is the
  `ssl` service and is the only part of the transport no test can reach.
- TLS is the project's biggest technical risk. Use the Horizon `ssl` system
  service through libnx; see `docs/DEVELOPMENT.md#tls-in-a-sysmodule` and issue
  M0-1 before choosing anything else.
- **Heap discipline:** size for one in-flight download buffer plus a TLS context.
  Stream to file; never hold a whole rom in RAM.
- **Never block boot.** Every network call has a timeout and is offline-safe.
- **`__appInit` runs before global constructors, and before there is anywhere to
  log.** `__libnx_init`'s order is `__libnx_initheap → __libnx_init_thread →
  argvSetup → __appInit → __libc_init_array`, so anything at file scope that
  `__appInit` touches has to be **constant-initialised** or it is used before its
  constructor runs. Two rules follow, and `boot.*` is what keeps them (M9-1,
  #195):
  - nothing `__appInit` reads or writes may have a runtime constructor —
    `boot::Journal` is fixed character storage for that reason and static_asserts
    it;
  - **no unbounded wait.** A service in the SAC but not yet registered makes `sm`
    *defer* the request rather than fail it, and a deferred request is never
    answered — which left this process inert with no crash report and no log.
    Every acquisition waits with a bound first (`boot_wait.hpp`), and what times
    out is written to a journal that `main` flushes once the log exists.

Nothing here is run on real hardware until the M8-1 gate passes. It is exercised
in Ryujinx as a manually-launched NRO first — never as an auto-boot sysmodule.
