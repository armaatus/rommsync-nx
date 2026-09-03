# ovl-rommsync (Ultrahand / Tesla overlay)

The control UI. **Not yet implemented.** A libultrahand `.ovl` that drives
`sys-rommsync` over IPC — it holds no sync logic of its own.

## Screens

- **Status** — connection, last sync, counts, current download + queue depth
- **Library / queue** — browse platforms → roms, enqueue downloads, progress
- **Sync** — Sync now, enable/disable, per-emulator toggles
- **Settings** — server URL, re-pair, folder-map overrides, interval, states on/off

## Planned structure

```
Makefile
lib/libultrahand/     # git submodule
source/
  main.cpp            # Tesla/Ultrahand gui roots
  screens/
  ipc_client.*        # talks to sys-rommsync
```

Append the `ULTR` four-byte signature to the built `.ovl` so Ultrahand lists it.
Model toggling + IPC on
[ovl-sysmodules](https://github.com/ppkantorski/ovl-sysmodules).

## First task

**M4-1: minimal overlay that connects to the sysmodule IPC and shows status.**
See milestone M4 in [`../ISSUES.md`](../ISSUES.md).
