# ovl-rommsync (Ultrahand / Tesla overlay)

The control UI. It opens the sysmodule's IPC service and draws the **status
screen** — connection, pairing, last sync, counts, current download and queue
depth — with **Y** opening the settings screen, which is the root menu the other
three are reached from. It holds no sync logic of its own; the engine lives
behind `sys-rommsync` and the overlay talks to it over IPC.

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
  ✅ M4-3
- **Sync** — Sync now, enable/disable auto-sync ✅ M4-2. The per-emulator toggles
  are config values and belong to **Settings** and the write path (#30); there is
  no second config writer here.
- **Pairing** — the code, the address and the countdown ✅ M4-5
- **Settings** — the whole effective configuration, every complaint the parser
  had about it, "Re-pair", and the **root menu** the other four are reached from
  ✅ M4-4. Read-only: rows are marked where M5-3 (#30) will make them editable,
  and nothing here writes `config.ini`.

**Y on the status screen opens Settings, and Settings opens the rest.** Before
M4-4 the overlay had no navigation at all — `main.cpp` opened on the status
screen and nothing pushed any other gui, so the sync, library and pairing
screens compiled and `--gc-sections` dropped them from the image.

## Structure

```
Makefile              # target-specific vars; the rules are in ../switch.mk
lib/libultrahand/     # submodule: libtesla + libultra, compiled from source
source/
  main.cpp            # the tsl::Overlay, and the one IPC session every screen shares
  ipc_client.*        # the only place this directory talks to sys-rommsync
  screen_frame.*      # the palette, the version handshake, and not-running vs unreachable
  status_screen.*     # the status screen's drawing; its decisions are in core/
  pairing_screen.*    # the pairing screen's drawing; likewise
  sync_screen.*       # the switch and "Sync now"; likewise
  library_screen.*    # platforms -> roms -> the queue; likewise
  settings_screen.*   # the configuration, the root menu and "Re-pair"; likewise
```

Screen files are flat here, and their basenames may not collide with anything in
`core/src/` — `switch.mk` names objects after the source basename and errors out
on a duplicate, so a `source/screens/pairing.cpp` would clash with the
`core/src/pairing.cpp` this target already compiles. That is why the screen is
`pairing_screen.*` rather than `screens/pairing.*`.

The half of a screen that is a *decision* rather than a draw call lives in
`core/include/rommsync/overlay_status_view.hpp`,
`core/include/rommsync/overlay_pairing_view.hpp`,
`core/include/rommsync/overlay_sync_actions.hpp`,
`core/include/rommsync/overlay_library_model.hpp` and
`core/include/rommsync/overlay_settings_view.hpp` — which sentence an unpaired
console gets, what a download with no declared length shows instead of a
percentage, what goes where a timestamp would go on a console that has never
synced, which of four sentences a dead pairing gets, and which of the two
controls on the sync screen may be pressed, what `interval_min = 0` reads as,
and which `roms` folder is the one that gets written to. That is deliberate: it
is the half a host test can reach, and `ctest -R overlay.status`,
`ctest -R overlay.pairing`, `ctest -R overlay.sync_actions`,
`ctest -R overlay.library` and `ctest -R overlay.settings` are what hold it. Model toggling + IPC on
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

7. **The settings screen and the menu.** Press **Y** on the status screen and
   expect the settings screen — `Screens` first, then `[server]`, `[sync]`,
   `[downloads]` and one section per mapped platform. Check three things a host
   test cannot: that the folder paths are legible at arm's length, that the
   first `roms` row reads *the write target* and the rest do not, and that
   **Ⓧ Re-pair** is still on screen with the built-in folder map's eleven
   sections above it — the prompts are reserved before the list precisely so a
   long map cannot push them off the bottom. Press **A** on each of the three
   menu rows and expect the sync, library and pairing screens; **B** comes back
   to settings, and **B** again to status.

8. **Re-pair, as far as it goes.** Press **Ⓧ**, expect *"This starts a new
   pairing, then discards the one on this console"* — that order is the whole
   design, not a wording preference — and the button to become
   *press again to confirm*,
   and expect **nothing to have happened yet**. Press **Ⓧ** again and — until
   `SdEngine::StartPairing` is built — expect *"This sysmodule cannot start a
   pairing yet; this console is still paired"*. Then confirm the console really
   is still paired: go back to the status screen and expect *Paired*, not
   *Not paired*. That is the whole point of the gate, and it is the one step
   here that would be a destroyed pairing if it were wrong.

9. **Pairing.** From the pairing screen (or, once `StartPairing` exists, from
   *Re-pair*), expect
   *"Contacting the server"* first — never *"Not paired"* — then the eight
   characters and the address, with a countdown that moves. Read the code back
   at arm's length: it is the one string on any of these screens a person has to
   copy by hand. Approve it in the browser and expect *"Paired"*; let one
   expire and expect *"The code expired"*, not *"Pairing failed"*.

10. **The sync screen's two controls.** On the sync screen with the sysmodule
    running, configured and paired:
    - **X** with auto-sync on. Expect the `Auto-sync` row to read *Off* and the
      prompt to become *Turn auto-sync on* — the switch is drawn from what the
      sysmodule read back, so a row that does not move is a write that did not
      take and must have said so. Press **X** again and expect the screen to be
      exactly as it was before the first press.
    - **A** with auto-sync off. Expect *Auto-sync is off* under a greyed
      *Sync now* — and expect it to have been there **before** the press, not to
      appear because of it. That is the point: the refusal is permanent while it
      is true, so the button never looks pressable and then does nothing. What
      the press must not produce is a spinner, a second copy of the sentence, or
      silence.
    - **A** with auto-sync on. **Check which sysmodule you are running first.**
      Until M7-2 (#37) lands, `SdEngine::RequestSync()` returns `false`
      unconditionally — it is a `bool` with nowhere to put "not built yet" — so
      `SyncNow` answers `already_running` on an idle console and this step draws
      *A sync is already running*. That is the sysmodule, not the screen. With
      M7-2 in, expect *Sync started* and the headline moving to *Syncing* within
      a poll or two; press **A** again while it runs and expect *A sync is
      already running* with **no second `SyncNow`** reaching the sysmodule — the
      screen refuses that press itself, so the two cases are told apart by what
      the sysmodule saw, not by what the panel says.
    - **Runtime pause is not the boot flag.** Turn auto-sync off here, then leave
      the overlay and confirm `sys-rommsync` is still resident and the status
      screen still answers. A screen that reads *sys-rommsync is not running*
      after this switch is the failure #24 was written around.

Record what each step actually drew in #44, including the layout constants that
had to move — every screen file keeps them in one block at the top for exactly
that.

## Where the work is

**M4 is complete.** All five screens are built on the frame M4-1 landed and the
`screen_frame.*` M4-2 lifted out of it. What is left for this directory is
M5-3's editing UI on the settings rows (#30 landed the write path; nothing here
sends an edit yet), M7-1's conflict UX (#36), and the layout pass against a real
panel in M8-2 (#44). See milestone M4 in [`../ISSUES.md`](../ISSUES.md).

The palette (`ColorFor`), the `GetInterfaceVersion` handshake and the
drop-and-reopen that decides *not running* from *unreachable* now live in
`source/screen_frame.*`, lifted there by M4-2 (#24) — the third screen, as #27
asked. A new screen takes a `ScreenFrame` member and calls `Ready()` and
`Diagnose()`; it does not open the port itself and does not name a colour.

**The root menu is `settings_screen.cpp`'s first section**, and every screen is
reached through it (M4-4, #26). A new screen adds an `overlay::Destination` and
a row there; it does not add a second way in from `main.cpp`.

**"Re-pair" asks before it discards.** The button sends `StartPair` first —
which writes nothing when it refuses — and only discards the token once a
pairing is genuinely starting. M1-6 (#123) built `SdEngine::StartPairing`, so
that is no longer the *only* thing standing between a user and a destroyed
pairing; it is still the right order, because an attempt can be refused for want
of a `server.url` or for want of an HTTP transport (the console has no
`HttpClient` yet, #126, which #43's gate wants), and each of those answers *"This
sysmodule cannot
start a pairing yet; this console is still paired"* with nothing lost.

[libultrahand]: https://github.com/ppkantorski/libultrahand
