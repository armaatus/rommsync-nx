# sysmodule/ — sys-rommsync

The background engine. Built with devkitPro (`devkitA64` + libnx), **not** by the
host CMake build. Runs under Atmosphère with a tight heap.

- This directory holds only the **Horizon glue**: the `ssl`-service HTTP backend,
  `fs` access, the IPC service, and the scheduler. All portable logic belongs in
  `core/`, where it can be tested without a console.
- TLS is the project's biggest technical risk. Use the Horizon `ssl` system
  service through libnx; see `docs/DEVELOPMENT.md#tls-in-a-sysmodule` and issue
  M0-1 before choosing anything else.
- **Heap discipline:** size for one in-flight download buffer plus a TLS context.
  Stream to file; never hold a whole rom in RAM.
- **Never block boot.** Every network call has a timeout and is offline-safe.

Nothing here is run on real hardware until the M8-1 gate passes. It is exercised
in Ryujinx as a manually-launched NRO first — never as an auto-boot sysmodule.
