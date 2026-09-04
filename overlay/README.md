# ovl-rommsync (Ultrahand / Tesla overlay)

The control UI. **Skeleton** — it builds and packages as an `.ovl`, and it draws
nothing. It holds no sync logic of its own; the engine lives behind
`sys-rommsync` and the overlay talks to it over IPC.

```bash
make -C overlay              # -> overlay/ovl-rommsync.ovl
make -C overlay clean
```

An `.ovl` is an `.nro` with the four-byte `ULTR` signature appended, which is
what makes Ultrahand list it rather than ignore it; [`../switch.mk`](../switch.mk)
appends it and CI checks for it, because a missing signature builds and uploads
cleanly and only shows up as an overlay that is not in the menu.

libultrahand is **not** a dependency yet. It arrives with the first real screen
in **M4-1**; carrying a submodule nobody exercises for four milestones buys
nothing, and the packaging is what M0-3 needed to prove. The overlay does not
link `core/` either, and should not: it is a thin client (see
[AGENTS.md](AGENTS.md)).

## Screens

- **Status** — connection, last sync, counts, current download + queue depth
- **Library / queue** — browse platforms → roms, enqueue downloads, progress
- **Sync** — Sync now, enable/disable, per-emulator toggles
- **Settings** — server URL, re-pair, folder-map overrides, interval, states on/off

## Structure

```
Makefile              # target-specific vars; the rules are in ../switch.mk
source/
  main.cpp            # Tesla/Ultrahand gui roots
```

Planned as the UI lands: `lib/libultrahand/` (submodule), `screens/`, and
`ipc_client.*`. Model toggling + IPC on
[ovl-sysmodules](https://github.com/ppkantorski/ovl-sysmodules).

## Where the work is

**M4-1: minimal overlay that connects to the sysmodule IPC and shows status.**
See milestone M4 in [`../ISSUES.md`](../ISSUES.md). Overlay UI is one of the few
things an emulator cannot exercise, so it is verified last, on hardware, after
the M8-1 gate.
