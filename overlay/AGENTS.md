# overlay/ — ovl-rommsync

The Ultrahand/Tesla overlay that drives the sysmodule. Built with devkitPro
against [libultrahand] as a submodule; ships an `.ovl` with the `ULTR` signature
so it appears in the Ultrahand overlay list.

- The overlay is a **thin client**. It renders state and sends IPC commands; it
  owns no sync, download, or auth logic — that lives in `core/` behind the
  sysmodule.
- Keep IPC payloads small and page large lists (platforms, roms, queue) rather
  than sending them whole. See `docs/DEVELOPMENT.md#ipc`.
- The sysmodule owns writes to `config.ini`; the overlay asks it to change
  settings and never writes the file itself.

Overlay UI is one of the few things an emulator cannot exercise, so it is
verified last, on hardware, after the M8-1 gate.

[libultrahand]: https://github.com/ppkantorski/libultrahand
