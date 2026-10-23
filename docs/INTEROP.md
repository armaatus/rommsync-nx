# Syncing with your other devices

How a save made on the Switch becomes the save RetroArch loads on your PC, the
one RomM's web player opens in a browser, and the one Argosy or Grout picks up
on a handheld — and theirs the one the Switch gets. [INSTALL.md](INSTALL.md)
puts the client on the console; this page is about everything else that talks
to the same RomM.

> **Not yet validated on hardware.** Nothing here has been followed on a real
> console: every exchange described below is one a test makes against a real
> RomM 5.4.0 in Docker, with the other device played by a test client speaking
> its protocol. The first real-console run is **M8-2**
> ([#44](https://github.com/armaatus/rommsync-nx/issues/44)), which also checks
> the file names marked *[verify]* below, and that is what removes this note.

---

## Which RomM

**RomM 5.4.0 or later.** The client asks the server which release it runs
once a boot and again after pairing — and on every sync while the answer is too
old — and below 5.4.0 it syncs nothing — no save offered, none
downloaded — and the overlay says **"RomM is too old to sync with"**
([TROUBLESHOOTING.md](TROUBLESHOOTING.md#romm-is-too-old)). That is deliberate:
5.4.0 is the first release that can tell a device a save was deleted, keep two
emulators' saves of one game apart, and remember which version each device last
synced. Without those, a save from your PC can be silently replaced by the
Switch's. Downloading roms works with any version.

Every test in this repository runs against RomM 5.4.0. A later release is read
as at least as capable; nothing older than 5.4.0 is supported.

---

## What has to agree

RomM keeps a game's saves in **slots**, and inside a slot apart by the emulator
that wrote them. Two devices share a save only when three things agree:

1. **The slot.** The client syncs in **`autosave`**, the slot RetroArch's Cloud
   Sync, RomM's web player, Argosy and Grout use for a game's save RAM. It is
   `[sync] slot` in `config.ini`; any other name keeps this console's saves to
   itself ([CONFIG.md](CONFIG.md#slot-the-shared-slot)).
2. **The emulator.** RomM records which core wrote each save, and the Switch
   asks only for saves written by the cores it runs. A Super Nintendo save
   written by Snes9x on your PC meets Tico's Snes9x on the Switch; one written by
   bsnes does not. The id the Switch sends for each system is the table below.
3. **The game's name.** A save is matched to a rom by its file name: the rom's
   file name in RomM without its extension, `Super Metroid (USA).srm` for
   `Super Metroid (USA).sfc`. The client uploads under that name whatever the
   file on the card is called, and writes a save it has never had under it too.

---

## Which systems sync with what

Tico runs most systems on the same libretro core RetroArch uses, and those
saves are the same bytes on both: they sync everywhere. Six of Tico's modules
write saves in a format no RetroArch core reads; those sync between Switches and
never with anything else, under an emulator id starting `tico-`, so that a
format nobody has proven identical is never written over another
([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#formats-only-one-emulator-reads) has the
mechanism).

| System | Tico module | Emulator id | Syncs with | Status |
|---|---|---|---|---|
| NES | FCEUmm | `fceumm` | RetroArch `fceumm`, the web player | full [verify bytes] |
| SNES | Snes9x | `snes9x` | RetroArch `snes9x`, the web player | full (`.rtc` in a slot of its own) |
| GB / GBC | Gambatte | `gambatte` | RetroArch `gambatte` | full [verify RTC] |
| GBA | mGBA | `mgba` | RetroArch `mgba`, the web player | full [verify] |
| Genesis / SMS / GG / Sega CD | Genesis Plus GX | `genesis_plus_gx` | RetroArch `genesis_plus_gx` | full [verify `.brm`] |
| Arcade | FinalBurn Neo | `fbneo` | RetroArch `fbneo` | full [verify] |
| PS1 | DuckStation | `tico-duckstation` | DuckStation on another Switch | Switch-only: `<game>.mcd`, card 2 `<game>_2.mcd` (own slot), one card for every disc, kept under disc 1's rom; a first-time restore is placed under those names, and a card kept under a later disc's rom is not placed |
| N64 | Mupen64Plus | `tico-mupen64plus` | Mupen64Plus on another Switch | Switch-only: `.eep`, `.sra`, `.fla`, `.mpk` each a save in its own slot (`autosave-eep` …) |
| Dreamcast | Flycast | `tico-flycast` | Flycast on another Switch | Switch-only, for a file named after its rom [verify on #44: Flycast may name VMUs by game id] |
| NDS | DraStic | `tico-drastic` | DraStic on another Switch | Switch-only (`.dsv` likely) [verify on #44] |
| Saturn | YabaSanshiro | `tico-yabasanshiro` | YabaSanshiro on another Switch | Switch-only [verify the file name on #44] |
| PSP | PPSSPP | `tico-ppsspp` | — | not synced: its saves are folders, a *save folder* skip; the `PSP-<folder>.zip` adapter needs #44 and RomM's per-entry ZIP hash (#234) |

"Full" needs the other device on the same core for that system. A RetroArch
save for one of the Switch-only systems — Beetle PSX, `mupen64plus_next`,
RetroArch's `ppsspp` — still syncs between RetroArch installs, the Switch's own
included; it is only never written into Tico's folder, nor Tico's into
RetroArch's. *[verify]* marks a file name or format nobody has yet compared on a
console.

A system Tico runs is played in Tico unless you say otherwise: a save the Switch
has never held lands in Tico's folder. `[platform.<slug>] play_with = retroarch`
sends it to RetroArch's instead
([CONFIG.md](CONFIG.md#play_with-where-a-save-the-console-has-never-held-goes)).

---

## Setting up RetroArch on your other devices

RetroArch 1.22 or later on a PC, a phone or another handheld syncs with RomM
through its own **Cloud Sync**, and RomM files what it uploads in `autosave` —
so the Switch meets it without anything extra. Four settings decide whether it
does:

1. **Cloud Sync on, pointed at RomM.** In RetroArch's Cloud Sync settings: turn
   it on, turn on syncing saves, choose WebDAV, and set the URL to your RomM
   with `/api/sync/retroarch/` after it —
   `https://romm.example.com/api/sync/retroarch/` — with your RomM username and
   password. (In `retroarch.cfg` the first two are `cloud_sync_enable` and
   `cloud_sync_sync_saves`.)
2. **Sort saves by core: on** (`sort_savefiles_enable = "true"`, and
   `sort_savestates_enable` for states; check it, since an older config may
   have it off). RomM reads which core wrote a save from
   the folder RetroArch keeps it in, `saves/Snes9x/`. A save written straight
   into `saves/` reaches RomM with no emulator at all, and the Switch, which asks
   RomM for its own cores' saves, is not offered it.
3. **The same core for each system as the Switch.** The table above names the
   id; this is the folder RetroArch writes that id's saves into, which is what
   RomM goes by. A save in any other core's folder is a different save to RomM.

   | Emulator id | RetroArch core folder |
   |---|---|
   | `fceumm` | `FCEUmm` |
   | `snes9x` | `Snes9x` |
   | `gambatte` | `Gambatte` |
   | `mgba` | `mGBA` |
   | `genesis_plus_gx` | `Genesis Plus GX` |
   | `fbneo` | `FBNeo` |

4. **Saves not kept beside the content** (`savefiles_in_content_dir = "false"`,
   the default). Cloud Sync uploads RetroArch's saves folder; a save written
   next to the rom is not in it, and never reaches RomM.

The rom has to have the same name on both devices too — Cloud Sync matches a
save to a rom by the file name RomM gave the rom — so download it from RomM
rather than from somewhere else.

---

## RetroArch on the Switch: Cloud Sync or rommsync-nx, not both

RetroArch on the Switch has the same Cloud Sync, and either it or this client
can sync RetroArch's saves there — **not both**. Two things writing the same save
to one server read, to each other, as another device changing it on every sync.

- **Let rommsync-nx do it** (recommended). Leave Cloud Sync off in the Switch's
  RetroArch. The client reads RetroArch's own folders from `retroarch.cfg`, sorted
  by core or not ([CONFIG.md](CONFIG.md#retroarch-read-from-retroarchcfg)), and
  syncs them beside Tico's — paired once from the overlay, with no RomM password
  on the card, a backup before every overwrite, and nothing synced while a game
  is open.
- **Let RetroArch do it.** Turn Cloud Sync on in the Switch's RetroArch. The
  client sees that in `retroarch.cfg` and leaves RetroArch's folders alone — the
  default `[sources] retroarch = auto` — and the settings screen says
  **"RetroArch syncs itself"**. Tico's saves are still this client's. Cloud
  Sync keeps your RomM password in `retroarch.cfg` on the card.

`[sources] retroarch = on` syncs RetroArch's folders even with Cloud Sync on, and
the settings screen warns that two things are syncing the same saves; `off`
reads no RetroArch folder at all
([CONFIG.md](CONFIG.md#sources)).

One game played in both Tico and RetroArch on the same Switch is one save to
RomM (one slot), and the client syncs the copy in the emulator
`[platform.<slug>] play_with` names — Tico unless you say otherwise — and skips
the other as a *duplicate slot*. Play each game in one of them.

---

## The web player, Argosy and Grout

All three keep a game's save RAM in `autosave`, so the rules above are the whole
setup: the same core, the same rom.

- **RomM's web player** runs in the browser on RomM's own cores — the table's
  "the web player" rows. Nothing to configure; a save made there is on the
  Switch after its next sync, and the Switch's is in the browser after it. In
  RomM 5.4 the web player is a sync device of its own: it registers itself, so
  it is listed in RomM's devices beside the Switch, asks only about the game it
  is playing, and on a conflict keeps its own copy aside as the Switch does. Its
  save states are named `<game> [<local date and time>].state`, a name no Tico
  or RetroArch slot loads, so they never reach the Switch.
- **Grout** names the emulator after the folder the save is in on that device.
  The Switch accepts both the core's id and RetroArch's folder name for it
  (`snes9x` or `Snes9x`), so a save Grout files under either meets the Switch's;
  one under any other name is a different save to RomM.
- **Argosy** follows the same rule: a save filed in `autosave` under the core id
  the Switch uses meets it.

**Tested with**, against a real RomM 5.4.0
([TESTING.md](TESTING.md#the-other-clients-are-their-requests-244)):

| Client | Plays, in the test | Each meets the Switch in |
|---|---|---|
| RetroArch's Cloud Sync | GBA, `mgba` | the Switch's save reaching it, its save reaching the Switch (and not while the rom is missing from the card), a conflict, a delete each way, and save states |
| a Grout-like device | GB, `gambatte` | the same five |
| RomM's web player | NES, `fceumm` | the same five |

Each client there is the requests it sends RomM, not the app itself, and
nothing has been run on a console. Argosy is not among them: it is expected to
behave as the Grout row does, by the rule above, and that is not yet tested.

---

## Tico 0.8 accounts

Tico 0.8 keeps each Switch user's saves apart, under
`/tico/saves/users/<account>/<system>/`. The client reads whichever layout the
card is in, on every sync. With **one** account it syncs that account's saves,
and the settings screen names it. With **several**, it syncs none of them until
`config.ini` says whose:

```ini
[tico]
account = 0123456789abcdef0123456789abcdef   ; the account's 32-digit id
```

The settings screen lists the ids it found. `account = legacy` reads the layout
from before 0.8 even when `users/` exists. The whole rule is in
[CONFIG.md](CONFIG.md#tico-per-account-saves), and what the log says about it in
[TROUBLESHOOTING.md](TROUBLESHOOTING.md#tico-saves-from-the-wrong-account-or-none).

A save one account made on the Switch is just a save to RomM: other devices see
it as the RomM user's, whichever Switch user made it.

---

## When two devices disagree

The server is the source of truth: when both sides changed a save since the
Switch last synced it, the **server's copy goes on the card**. Nothing is lost:

- the card's bytes are copied under `/config/rommsync/.backup/` first;
- they are also uploaded to RomM as a save **with no slot**, named
  `<game> [switch-conflict <YYYY-MM-DD_HH-MM-SS>].srm` (or `.mcd`, `.eep`: the
  save's own extension) — the time, in UTC, of the backup — under the emulator
  the Switch synced it as. RomM never syncs a save without a slot to
  any device, so this copy stays on the server where you can download it from
  the web UI, and never comes back by itself;
- the overlay's **Conflicts** screen lists it, says whether the server holds
  that copy, and can put the card's bytes back. The next sync then decides
  again, and will most likely offer them to RomM as new progress.

The same happens when another device saves between the Switch asking what to
sync and uploading: RomM refuses the Switch's upload (a 409), the Switch asks
again, and either takes the newer save — when its own copy had not changed — or
records a conflict exactly as above
([TROUBLESHOOTING.md](TROUBLESHOOTING.md#another-devices-save-got-there-first-409)).

A save **deleted** in RomM's web UI, or by RetroArch deleting its `.srm`, is
removed from the Switch on its next sync, after a backup, and is listed on the
Conflicts screen as *deleted on RomM*. `[sync] on_server_delete = keep` leaves
the file instead and stops offering it
([CONFIG.md](CONFIG.md#on_server_delete-a-save-deleted-in-romm)).

---

## Known limits

- **Multi-disc games.** The client never downloads a disc set; put one on the
  card yourself, as a folder named like the rom in one of the platform's `roms`
  folders. A save is matched to a rom by name, so on every device it has to be
  named after the same rom — a save named after a playlist (`.m3u`) or after one
  disc syncs only with a rom of that name. Tico's DuckStation keeps one memory
  card for every disc of a game, and it is synced under the first disc's rom.
- **States across cores.** A save state is one core's memory at one version of
  that core. States are off by default (`[sync] states`); on, a state moves only
  between devices running the core that wrote it, and a core update can still
  make an old one refuse to load. Tico and RetroArch number their slots one
  apart: `[sync] map_state_slots` lines them up, and it belongs on every Switch
  or on none ([CONFIG.md](CONFIG.md#states-and-map_state_slots-save-states)).
  RomM keeps one state per game per file name, whoever wrote it: another
  device's `<game>.state1` uploaded after the Switch's replaces it on the
  server. The Switch takes it only when the same core wrote it and the card's
  copy is unchanged; otherwise it keeps its own, which is then on the card
  alone ([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#the-policy)).
- **Deleting a save on the console is not a delete.** RomM and your other
  devices keep it, and the Switch does not bring it back either. To delete a
  save everywhere, delete it in RomM's web UI or in RetroArch; the Switch then
  removes its copy, after a backup.
- **128 saves per console.** The scan reads RetroArch's folders first, then the
  folder map, each folder's files in name order, and stops at 128 saves. The
  rest of the card is not read: not uploaded, and nothing is written over them.
  The log says where it stopped and the status screen reads partly finished.
- **32 states**, sharing those 128 records with the saves; saves come first.
- **1,024 roms** in the index one sync holds, the ones with the lowest RomM ids.
  A save for a game past that is skipped as unmatched, and the log says the
  index is incomplete.
- **A game with more than 128 saves on the server** is left out of the sync
  altogether — nothing of it is offered or touched — and named in the log on
  every sync.
- **512 entries** of any folder are read, the first the card lists: a `roms`
  folder with more gives the rest no first-time restore, and a save folder with
  more is read only that far. The log names the folder either way.
- **A save from another device reaches the Switch only for a game the card
  has** — a rom in one of the platform's `roms` folders, or one just downloaded
  from the overlay. The Switch is not offered saves for games it does not hold.
- **A conflict puts the server's copy on the card first.** Your bytes are in
  the backup and on RomM, and the Conflicts screen offers **Keep local**: your
  bytes back on the card, and the next sync uploads them over the server's copy
  (which RomM does not delete). **Keep server** and **Skip** change
  nothing.
- **PSP saves are not synced**, and the Switch-only systems above sync with no
  other emulator, until #44 has captured real files to build adapters from.
