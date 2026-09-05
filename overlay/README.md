# ovl-rommsync (Ultrahand / Tesla overlay)

The control UI. It opens the sysmodule's IPC service and draws the **status
screen** — connection, pairing, last sync, counts, current download and queue
depth. It holds no sync logic of its own; the engine lives behind
`sys-rommsync` and the overlay talks to it over IPC.

```bash
git submodule update --init --recursive   # lib/libultrahand, once
make -C overlay                           # -> overlay/ovl-rommsync.ovl
make -C overlay clean
```

An `.ovl` is an `.nro` with the four-byte `ULTR` signature appended, which is
what makes Ultrahand list it rather than ignore it; [`../switch.mk`](../switch.mk)
appends it and CI checks for it, because a missing signature builds and uploads
cleanly and only shows up as an overlay that is not in the menu.

[libultrahand] is pinned as a submodule at `lib/libultrahand` and compiled from
source — upstream ships sources and a `.mk`, not a library. `../switch.mk`
builds it as *vendored* code: its headers are reached with `-isystem` and its
objects skip `-Wextra -Wpedantic -Werror`, which it was never written against,
so our own translation units stay warnings-as-errors either way. Bumping it is
moving the submodule pointer, and `git submodule status` is where a reviewer
reads which revision an `.ovl` came from.

The overlay *compiles* `core/` (`ROMMSYNC_USE_CORE`) and links almost none of
it: the IPC codecs, so the two halves cannot disagree about a field name, and
the status screen's view model. See [AGENTS.md](AGENTS.md) — it is a thin
client, and that rule is about ownership rather than about the link map.

## Screens

- **Status** — connection, last sync, counts, current download + queue depth
  ✅ M4-1
- **Library / queue** — browse platforms → roms, enqueue downloads, progress
- **Sync** — Sync now, enable/disable, per-emulator toggles
- **Settings** — server URL, re-pair, folder-map overrides, interval, states on/off

## Structure

```
Makefile              # target-specific vars; the rules are in ../switch.mk
lib/libultrahand/     # submodule: libtesla + libultra, compiled from source
source/
  main.cpp            # the tsl::Overlay, and the one IPC session every screen shares
  ipc_client.*        # the only place this directory talks to sys-rommsync
  status_screen.*     # the status screen's drawing; its decisions are in core/
  pairing_screen.*    # the pairing screen's drawing; likewise
```

Screen files are flat here, and their basenames may not collide with anything in
`core/src/` — `switch.mk` names objects after the source basename and errors out
on a duplicate, so a `source/screens/pairing.cpp` would clash with the
`core/src/pairing.cpp` this target already compiles. That is why the screen is
`pairing_screen.*` rather than `screens/pairing.*`.

The half of a screen that is a *decision* rather than a draw call lives in
`core/include/rommsync/overlay_status_view.hpp` and
`core/include/rommsync/overlay_pairing_view.hpp` — which sentence an unpaired
console gets, what a download with no declared length shows instead of a
percentage, what goes where a timestamp would go on a console that has never
synced, and which of four sentences a dead pairing gets. That is deliberate: it
is the half a host test can reach, and `ctest -R overlay.status` and
`ctest -R overlay.pairing` are what hold it. Model toggling + IPC on
[ovl-sysmodules](https://github.com/ppkantorski/ovl-sysmodules).

## Manual verification (M8-2)

Overlay UI is one of the few things an emulator cannot exercise, so it is
verified last, on hardware, after the **M8-1** gate. This is the script #44
runs; nothing here may be run before that gate passes, and never against a
production RomM or a real library.

1. Copy `overlay/ovl-rommsync.ovl` to `sdmc:/switch/.overlays/` and
   `sysmodule/sys-rommsync.nsp` to
   `sdmc:/atmosphere/contents/4200000000524D53/exefs.nsp`. Reboot.
2. **Sysmodule off.** With `sys-rommsync` not running (delete or rename its
   `exefs.nsp`, or leave a `boot2.flag` off), open the overlay. Expect
   *"sys-rommsync is not running"* and a line telling the user to enable it —
   **not** an empty panel and not a crash.
3. **Sysmodule on, nothing configured.** Boot with the sysmodule installed and
   no `sdmc:/config/rommsync/config.ini`. Expect *"No server set"*, `Server`
   *Not set*, `Pairing` *Not paired*, `Last sync` **Never** — every row carrying
   a value. A blank beside any label is the failure this screen exists to
   prevent.
4. **Configured, not paired.** Write a `config.ini` with a `server.url` on the
   test RomM. Expect *"Not paired"* and `Server` reading *Not reached*.
5. **Version mismatch.** Install an `.ovl` and an `.nsp` built from different
   `ipc::kVersion`s. Expect *"sysmodule unreachable"* with *"Update the
   sysmodule"* naming both numbers — not a decode failure and not garbage rows.
6. **Legibility.** Read the panel at arm's length in handheld and docked. Every
   row's value is on screen and none is clipped by the value column.

7. **Pairing.** From the settings screen, choose *Re-pair*. Expect
   *"Contacting the server"* first — never *"Not paired"* — then the eight
   characters and the address, with a countdown that moves. Read the code back
   at arm's length: it is the one string on any of these screens a person has to
   copy by hand. Approve it in the browser and expect *"Paired"*; let one
   expire and expect *"The code expired"*, not *"Pairing failed"*.

Record what each step actually drew in #44, including the layout constants that
had to move — `status_screen.cpp` and `pairing_screen.cpp` each keep them in one
block for exactly that.

## Where the work is

**M4-2..M4-4** — the library/queue, sync and settings screens, built on the
frame M4-1 landed. The pairing screen (M4-5) is built and **nothing pushes it
yet**: *Re-pair* on the settings screen (M4-4, #26) is the one place it is
reached from, so until that lands it compiles and `--gc-sections` drops it from
the image. See milestone M4 in [`../ISSUES.md`](../ISSUES.md).

The palette (`ColorFor`), the `GetInterfaceVersion` handshake and the
drop-and-reopen that decides *not running* from *unreachable* are now written
out in both `status_screen.cpp` and `pairing_screen.cpp`. The third screen is
the one that should lift them into a shared `overlay/source/screen_frame.*`
rather than typing them a third time; doing it now, while three M4 screens are
in flight in parallel worktrees, would be a merge conflict bought for nothing.

[libultrahand]: https://github.com/ppkantorski/libultrahand
