# overlay/ — ovl-rommsync

The Ultrahand/Tesla overlay that drives the sysmodule. Built with devkitPro
against [libultrahand], pinned as a submodule at `lib/libultrahand` and compiled
from source; ships an `.ovl` with the `ULTR` signature so it appears in the
Ultrahand overlay list.

- The overlay is a **thin client**. It renders state and sends IPC commands; it
  owns no sync, download, or auth logic — that lives in `core/` behind the
  sysmodule.
- It *compiles* `core/` (`ROMMSYNC_USE_CORE` in the Makefile) and links almost
  none of it. Two things it does link: the IPC codecs — `rommsync/ipc.hpp`,
  shared with the sysmodule so one copy of the field names serves both halves —
  and the screens' view models — `rommsync/overlay_status_view.hpp`,
  `rommsync/overlay_pairing_view.hpp`, `rommsync/overlay_sync_actions.hpp`,
  `rommsync/overlay_library_model.hpp` and whatever M4-4 adds beside them. `--gc-sections` drops the engine no screen
  references. The rule above is about ownership, not about the link map.
- **A screen is two halves, and only one of them is in this directory.** What
  the screen *says* — which sentence a never-paired console gets, what a
  download with no declared length shows instead of a percentage, what goes
  where a timestamp would go on a console that has never synced — is a
  rendering-independent view model in `core/`, held by a host test
  (`ctest -R overlay.status`). What is left here is the drawing. Put a decision
  in a `tsl::Gui` and it becomes untestable until someone has a console.
- Flat files in `core/src/` only. CMake globs recursively; `switch.mk` uses a
  non-recursive wildcard, so a `core/src/overlay/` would build on the host and
  silently vanish from the Switch build.
- Flat files in `source/` too, and no basename this target already compiles.
  Objects are named after the source basename, so a `source/screens/pairing.cpp`
  would collide with the `core/src/pairing.cpp` linked in beside it —
  `switch.mk` stops the build rather than letting VPATH pick a winner. Screens
  are `<thing>_screen.*`, which keeps them clear of `core/src/`.
- **A screen's palette, handshake and reopen are `source/screen_frame.*`'s, not
  its own.** Take a `ScreenFrame` member, call `Ready()` before any command and
  `Diagnose(rc)` after a failed one, and name no colour: `ColorFor(Tone)` is the
  one place `core/`'s vocabulary and libultrahand's meet. Three copies of the
  handshake is how a version check gets fixed in two screens out of four. What
  stays per-screen is the layout block, because M8-2 (#44) adjusts it against a
  real panel one screen at a time.
- **A refusal the sysmodule named is not a transport failure.** Both arrive as a
  failing `Result`; `overlay::DecodeError(rc, &error)` is what tells them apart,
  and only what it refuses goes to `ScreenFrame::Diagnose`. A screen that sent
  every failure to `Diagnose` would draw "sys-rommsync is not running" over a rom
  that was simply already queued (M4-3, #25). The three screens before it have no
  command whose refusal carries meaning, which is why it did not exist until now.
- All IPC goes through `source/ipc_client.*`. A screen never builds a payload
  itself; if a screen needs something the client cannot answer, the command
  belongs in `docs/DEVELOPMENT.md#ipc` and in `rommsync/ipc.hpp` first.
- **Check `GetInterfaceVersion` against `ipc::kVersion` before anything else.**
  It is command 0 and its encoding is frozen, so it is the only call that is
  safe to make before knowing whether the two halves agree. A mismatch is
  "update the sysmodule", not a decode failure.
- **IPC-unreachable is a first-class state, not an error toast**, and so are
  never-paired, never-synced and offline. A value drawn as an empty string is
  indistinguishable from an overlay that failed to read something; every line a
  view model produces carries text.
- Keep IPC payloads small and page large lists (platforms, roms, queue) rather
  than sending them whole. See `docs/DEVELOPMENT.md#ipc`.
- The sysmodule owns writes to `config.ini`; the overlay asks it to change
  settings and never writes the file itself. `ctest -R overlay.sync_actions`
  greps this directory for the write path and for the boot flag rather than
  leaving it reviewed. Reading `config.ini` is allowed — both components read it
  (docs/ARCHITECTURE.md); writing it is the sysmodule's alone.
- **The enable switch is a runtime pause, not the ovl-sysmodules boot flag.**
  That flag lives under `/atmosphere/contents/<TID>/flags/` and means the
  process does not exist; `[sync] enabled` means it is resident, idle and still
  answering IPC. Rendering both as "disabled" hides a sysmodule that failed to
  start. Nothing here reads or writes the flag — that is M6-2 (#33).
- libultrahand is upstream's code, not ours: `switch.mk` compiles it with
  `-isystem` headers and without `-Wextra -Wpedantic -Werror`. Do not relax
  those for anything in `source/`, and do not patch the submodule in place —
  bumping it is moving the pointer.

Overlay UI is one of the few things an emulator cannot exercise, so it is
verified last, on hardware, after the M8-1 gate. `README.md` carries the script
M8-2 (#44) runs.

[libultrahand]: https://github.com/ppkantorski/libultrahand
