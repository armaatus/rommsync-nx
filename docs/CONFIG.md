# Configuration

> This page is the console's `config.ini`. The agent fleet's own project
> configuration (`.autofleet/`) is [CONFIGURATION.md](CONFIGURATION.md).

`sdmc:/config/rommsync/config.ini`. The sysmodule reads it at boot and on an
IPC "reload"; the overlay edits it through the sysmodule (which owns writes).

```ini
version = 1                        ; the format of this file; see "Version" below

[server]
url = https://romm.example.com     ; RomM origin, HTTPS strongly recommended

[sync]
enabled       = true
interval_min  = 30                 ; 0 = only boot + manual
on_boot       = true
saves         = true
slot          = autosave           ; the slot RetroArch, the web player and Grout share
states        = false              ; fragile across cores; opt-in
map_state_slots = false            ; Tico's state slots under RetroArch's names; see below
conflict_show = true               ; surface conflicts in the overlay
on_server_delete = remove          ; remove | keep; a save deleted in RomM, see below

[downloads]
enabled       = true
verify_hash   = true               ; check sha1 (or md5) after download
resume        = true

[sources]
retroarch             = auto       ; auto | on | off; auto = off while RetroArch's Cloud Sync is on
retroarch_content_dir = false      ; read RetroArch saves kept beside the roms

[tico]
account = auto                     ; auto | legacy | a 32-digit account id; see "Tico" below

; Platform folder map: RomM platform_fs_slug -> SD folders.
; roms   = where downloads land (first entry is the write target)
; saves  = dirs to scan/sync for SRAM saves
; states = dirs to scan/sync for save states
; emulator = RomM emulator id for this platform's saves (optional; see below)
; play_with = tico | retroarch: where a save the card never held goes (default tico)
[platform.snes]
roms   = /tico/roms/snes
saves  = /retroarch/saves, /tico/saves/snes
states = /retroarch/states, /tico/states/snes

[platform.gba]
roms   = /tico/roms/gba
saves  = /retroarch/saves, /tico/saves/gba
states = /retroarch/states, /tico/states/gba

; ... and the rest of the built-in map, below
```

## Version

`version = 1`, above the first section, says which format the file is written
in. It describes the **file**, not the client: a release that adds a key with a
default (as `emulator` was added after v1.0.0-rc1) leaves it alone, because an
older file simply does not have that key. The number moves only when a key's
*meaning* changes, and the release that moves it reads every older version into
the new meaning.

- **No `version` line** is version 1. That is every `config.ini` v1.0.0-rc1 left
  on a card, and it is read exactly as before.
- **A version this build does not know yet** — a file written for a later
  release, met after a downgrade or after updating only the overlay — is
  **refused, not guessed at**. None of the file is in force: the built-in
  defaults are, so there is no server and the client stays idle. One `error`
  names the line and the remedy, the overlay's status screen counts it, and the
  file is left exactly as it is — an edit from the overlay is refused too, so an
  older sysmodule never writes over a newer file. Install the sysmodule and the
  overlay from the release that wrote it.
- A `version` line that is not a whole number of at least 1 is a `warning`, and
  the file is read as version 1.
- A file the sysmodule creates — the first edit from the overlay on a console
  with no `config.ini` — starts with `version = 1`. An edit to an existing file
  never adds or changes the line.

Every other record under `config/rommsync/` has its own version and its own
answer to an older and a newer one; they are listed in
[ARCHITECTURE.md](ARCHITECTURE.md#formats-across-releases).

## Defaults & the folder map

- A built-in default map ships for the Switch-capable systems. `config.ini`
  only needs to contain **overrides**.
- Keys are RomM `platform_fs_slug` values (what you see under RomM's
  `library/roms/`). The right-hand paths are absolute from the SD root.
- The folders are **Tico's names** for each system, which are not always
  RomM's. Every platform maps `roms = /tico/roms/<folder>`, `saves =
  /retroarch/saves, /tico/saves/<folder>` and `states = /retroarch/states,
  /tico/states/<folder>`:

| RomM `platform_fs_slug` | Tico folder |
|---|---|
| nes, snes, gb, gbc, gba, n64, genesis, psx, nds, psp, saturn | the same name |
| sms | `master-system` |
| gamegear | `game-gear` |
| segacd | `sega-cd` |
| dc, dreamcast | `dc` |
| arcade, fbneo | `fbneo` |
| ngc *(off by default)* | `gc` |
| wii, 3ds *(off by default)* | the same name |

- Where two spellings are common (`dc`/`dreamcast`, `arcade`/`fbneo`) both are
  mapped, to the one folder. The cost is that the folder no longer implies one
  platform, so a save name both platforms have a rom for is skipped as
  ambiguous rather than guessed.
- **gc, wii and 3ds are off by default.** Tico runs them, but they are heavy on
  the console and their roms are large. Turn one on with its built-in folders:

  ```ini
  [platform.ngc]
  enabled = true
  ```

  `enabled = false` switches any platform off, whatever else its section says.
  A section holding only `enabled` (or only `emulator`) keeps the built-in
  folders; add `roms`, `saves` or `states` and it becomes a folder map under
  the rule below. The overlay does not edit `enabled`; v1.0.0-rc1 warns that it
  is an unknown key and ignores it.
- ps2, ps3 and ps4 are intentionally unmapped: nothing on the console runs
  them, and the client skips anything without a mapping.
- **Tico's extra rom folders are not read.** Tico can scan up to 16 more rom
  folders (USB included); the client downloads into, and looks for roms in, only
  the folders this map lists. Add one to a platform's `roms` list if you keep
  roms there — after the first, which is where downloads land.
- **Up to v1.0.0-rc1 the map used `dreamcast` as Tico's folder name.** Tico's folder
  is `dc`, so Dreamcast roms downloaded into `/tico/roms/dreamcast` were never
  listed by Tico. Those downloads are now made again into `/tico/roms/dc`; the
  old folder can be deleted. A `[platform.dreamcast]` section an earlier
  overlay wrote out still names the old folders until you edit them.
- **RetroArch's folders come from RetroArch**, not from this map: see
  [RetroArch](#retroarch-read-from-retroarchcfg) below. The map's
  `/retroarch/saves` and `/retroarch/states` are still read as written, after
  RetroArch's own.
- **Tico's layout comes from the card**: see [Tico](#tico-per-account-saves)
  below.

### A platform section replaces that platform's defaults

`[platform.snes]` is the whole mapping for `snes`, not an addition to the
built-in one. Writing

```ini
[platform.snes]
roms = /roms/snes
```

leaves snes with **no** save folders, because you did not list any — which is
the "downloads work, saves are skipped" case below. The client says so (a
`notice` naming `[platform.snes] saves`) rather than leaving you to find out
when a save does not come back. List every key you want that platform to have.

The upside is that the map is exactly what you can read off the file, and that
emptying a section is how you switch a platform off:

```ini
[platform.psp]
roms   =
saves  =
states =
```

A platform that maps no folders is skipped entirely, the same as one nobody
ever mapped.

### `emulator`: which emulator RomM is told wrote a platform's saves

Every save and state goes to RomM with an `emulator` id, and it is not a label:
RomM files a save's versions by it, and RetroArch's Cloud Sync, the web player
and other clients look saves up by it. The client works it out per save:

1. A RetroArch **sort-by-core** folder names its core: a save in
   `/retroarch/saves/mGBA` is `mgba`, in `/retroarch/saves/Beetle PSX HW`
   `mednafen_psx_hw`. The folder names are RomM's own table.
2. Otherwise **`[platform.<slug>] emulator`**, when you set it.
3. Otherwise the built-in id for the folder's emulator and that platform:

| System | Tico (`/tico/...`) | RetroArch, flat `saves/` |
|---|---|---|
| nes | `fceumm` | `fceumm` |
| snes | `snes9x` | `snes9x` |
| gb, gbc | `gambatte` | `gambatte` |
| gba | `mgba` | `mgba` |
| genesis, sms, gamegear, segacd | `genesis_plus_gx` | `genesis_plus_gx` |
| arcade / fbneo | `fbneo` | `fbneo` |
| n64 | `tico-mupen64plus` | `mupen64plus_next` |
| psx | `tico-duckstation` | `pcsx_rearmed` |
| dc, dreamcast | `tico-flycast` | `flycast` |
| nds | `tico-drastic` | `melonds` |
| saturn | `tico-yabasanshiro` | — |
| psp | `tico-ppsspp` | `ppsspp` |

A `tico-` id is a save format no RetroArch core shares, so those saves sync
between Switches and nowhere else rather than being offered to a core that
would misread them. A system in neither column, in a folder that belongs to
neither emulator, is sent with no emulator at all.

The override is for a system whose emulator you changed. Tico can run GB on
mGBA instead of Gambatte, and the client cannot see that choice yet:

```ini
[platform.gb]
emulator = mgba
```

**A section that sets only `emulator` keeps that platform's built-in folders** —
it is not a folder map, so it does not replace one. Add `roms`, `saves` or
`states` to it and it becomes one, under the rule above. `emulator =` takes the
override back out.

The id has to be one RomM accepts — at most 50 characters, none of
`\ / : | * ? " < >`, no control character, no space at either end. One that is
not is dropped with a `warning` naming the line, and the built-in id stays in
force: RomM would otherwise refuse every upload of that platform's saves.

### `play_with`: where a save the console has never held goes

A save already on the card is synced where it is found. A save the server holds
and the card does not — a first-time restore — has no such place, so it goes to
the emulator the platform is **played with**:

```ini
[platform.snes]
play_with = retroarch      ; tico | retroarch
```

**Tico is the default** for every system Tico runs (the table above, and `ngc`,
`wii` and `3ds`). Any other platform has no default, and a save for it is not
placed until a `play_with` line says where. Like `emulator`, a section that sets
only `play_with` keeps the platform's built-in folders, and `play_with =` puts
the default back.

| `play_with` | Where it goes |
|---|---|
| `tico` | Tico's folder for the system, from the map, in the layout the card is in: `/tico/saves/users/<account>/snes/` on Tico 0.8, `/tico/saves/snes/` before it. **Nothing** while several accounts wait for `[tico] account` to name one. |
| `retroarch` | the folder `retroarch.cfg` names (by default `/retroarch/cores/savefiles`), in the core's own folder when it sorts by core (`Snes9x/`), or beside the rom when it keeps saves with the content — the last only with `[sources] retroarch_content_dir = true`. **Nothing** while RetroArch syncs itself, or with `[sources] retroarch = off`. |

Never the map's first `saves` folder: that is `/retroarch/saves` for every
system, which neither Tico nor a default RetroArch reads.

**Only for a game the card holds** (#241). A rom counts as on the card when a
file named after it is in one of its platform's `roms` folders, or a folder for a
multi-file rom. The server is asked only about those roms, so a save for a game
this console does not have is not placed. Keep your roms somewhere other than
`/tico/roms/<folder>`? List that folder in `roms`, after the download target.
The folder is not searched recursively, and only its first 512 entries are read.
Downloading a rom from the overlay brings its save straight away, without
waiting for the next scheduled sync.

The name is the **rom's**, with the emulator's extension —
`Super Mario World (USA).srm`, `Game.mcd` for DuckStation — never RomM's stored
name (`Super Mario World (USA) [2026-10-01_12-00-00].srm`). A state keeps the
name it has on the server, because that name is what pairs it.

A save is placed only when the result is the save it came from: bytes the target
emulator reads (an isolated `tico-*` format never goes to RetroArch, nor a
RetroArch core's save to a Tico module that is not that core), in a file the next
sync reads back as the same rom in the same slot, and where no file is already.
Anything else is a failed operation with the reason in the log — never a guess,
and never a file written over.

### RetroArch: read from `retroarch.cfg`

Where RetroArch keeps saves is RetroArch's setting, so every sync reads it from
`sdmc:/retroarch/retroarch.cfg` rather than trusting a guess here:

| `retroarch.cfg` | What is read |
|---|---|
| `savefile_directory` (`savestate_directory`) | that folder, whatever it is called. `default`, or a folder that does not exist, is the Switch build's default, `/retroarch/cores/savefiles` (`/retroarch/cores/savestates`) — not `/retroarch/saves` |
| `sort_savefiles_enable = true` (the default) | each folder one level under it, `<folder>/<CoreFolder>/`, named by RomM's core table: `mGBA` is `mgba`. A folder RomM does not know is RetroArch's with no emulator id. A file directly in the parent is skipped as *outside layout*: RetroArch does not read it |
| `sort_savefiles_enable = false` | the folder itself, every system in it, with the table above's RetroArch column |
| `savefiles_in_content_dir = true` | the `roms` folders of the map, and in them only `.srm`/`.rtc` (`.state`, `.stateN`) — **only** with `[sources] retroarch_content_dir = true`; otherwise nothing, and a `warning` says so |
| `sort_savefiles_by_content_enable = true` | nothing: that layout is not read, and a `warning` says so |
| `cloud_sync_enable = true` (with `cloud_sync_sync_saves`) | nothing, unless `[sources] retroarch = on`: **RetroArch syncs itself** |

A folder holds at most 512 entries as far as the client is concerned, the first
512 the card lists. A sorted RetroArch folder past that still has every core folder RomM's
table names read; a rom folder past it is read only as far as that, and the
skip says so. At most 64 RetroArch core folders are read in one sync.

A folder the map lists as `roms` is read as one however the scan got there —
`savefile_directory` naming it, or a sorted folder above the rom folders: only
`.srm`/`.rtc` (`.state`, `.stateN`) in it, a subfolder only when RomM's table
names the core, and its `.tmp`s are left to the rom download that staged them.

RetroArch's folders are read before the map's, so when the map's
`/retroarch/saves` still holds an older copy of a save, the copy RetroArch
actually reads is the one that syncs. The map's entry for the folder
`retroarch.cfg` names is not read a second time beside it.

A missing `retroarch.cfg` is RetroArch's defaults and a `notice`; an unreadable,
oversized or malformed one is its defaults for whatever it did not state
usably, with a `warning` naming the line. None of it is an `error`: it is not
this client's configuration. These appear on the overlay's settings screen
after `config.ini`'s own, and in the log when they change.

#### `[sources]`

```ini
[sources]
retroarch             = auto   ; auto (default) | on | off
retroarch_content_dir = false  ; true reads saves RetroArch keeps beside the roms
```

- `retroarch = auto` reads RetroArch's folders unless RetroArch's own Cloud Sync
  is syncing its saves. Two writers to one save look like another device's
  changes on every sync, so while Cloud Sync is on the client stays out of the
  way and the settings screen says **"RetroArch syncs itself"**.
- `retroarch = on` reads them anyway, and warns that two things are syncing the
  same saves. `off` reads no RetroArch folder at all — the map's included.
- `retroarch_content_dir` is off by default because the folders it reads are
  rom folders.

`[sources]` is edited by hand; the overlay does not offer it. v1.0.0-rc1 warns
that it is not a section it knows and ignores it.

### Tico: per-account saves

Tico 0.8.0 keeps each Switch account's saves apart:
`/tico/saves/users/<account>/<folder>/`, where `<account>` is the account's id as
32 hex digits. States moved the same way, under `/tico/states/users/`. Before
0.8.0 — and in 0.8.0 with no account signed in — the folder was
`/tico/saves/<folder>/`.

The client reads whichever layout the card is in, every sync. For each Tico
folder in the map, `/tico/saves/snes` say:

- **no `/tico/saves/users/`**, or an empty one: `/tico/saves/snes` is read, as
  before;
- **`users/` holds one account**: `/tico/saves/users/<account>/snes` is read,
  and the settings screen says whose. A file still in `/tico/saves/snes` is
  skipped as *outside layout* — Tico 0.8 does not read it. A module installed
  before 0.8.0 keeps writing there until it is reinstalled;
- **`users/` holds several accounts**: none of them is synced until you say
  which. The settings screen lists them as a `warning` on `[tico] account`.

```ini
[tico]
account = auto     ; the default: the one account, when there is exactly one
account = legacy   ; always the layout before 0.8.0, even beside users/
account = 0123456789abcdef0123456789abcdef   ; this account's folder
```

A custom root follows the same rule: a Tico folder `<root>/<folder>` is read at
`<root>/users/<account>/<folder>` once `<root>/users/` exists. The map's folders
themselves never change, so a file written by v1.0.0-rc1 means what it meant.
An `account` that is not `auto`, `legacy` or 32 hex digits is a `warning`, and
`auto` stays in force. `[tico]` is edited by hand; v1.0.0-rc1 ignores it with a
warning.

**What is never synced**, in any folder: Tico's `backups/` folders (the scan
reads no subfolder), PSP saves — PPSSPP keeps each game's as a folder in Tico's
psp folder, and each is skipped as a *save folder* until an adapter exists
([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#formats-only-one-emulator-reads)), `*.tmp` (Tico writes a save there and renames it; one a
crash left behind is never uploaded), `*.png` (a state's screenshot) and
`*.restart` (DuckStation's). Snes9x's `.rtc` clock is synced beside its `.srm`,
as a save of its own in its own slot (`autosave-rtc`; see below).

### `slot`: the shared slot

`[sync] slot` names the slot every save is synced in, and defaults to
`autosave` (#236). That is the slot RetroArch's own Cloud Sync, RomM's web
player, Argosy and Grout keep a game's save RAM in, so with the default a save
made on the Switch is the one those clients load, and theirs is the one the
Switch gets — for the systems whose Tico module is the same libretro core
([`emulator`](#emulator-which-emulator-romm-is-told-wrote-a-platforms-saves)).

A `.srm` is synced in the slot itself. Every other kind of save file gets a slot
of its own, `<slot>-<extension>` — `autosave-rtc` for Snes9x's clock,
`autosave-mcd` for a DuckStation card — because a slot holds one save per game:
RomM pairs a game's slot with its newest file, and a clock in the same slot
would be paired with the save RAM.

Another name keeps this console's saves to itself, in a slot no other client
reads: letters, digits, `-` and `_`, up to 24 of them — every `state.db` row
carries it, and that file is bounded. Anything else is a `warning` and
`autosave` stays in force. Changing it moves every save to the new
slot on the next tick, as a first meeting with whatever is there; the old slot
is left on the server. The overlay does not edit it.

### `states` and `map_state_slots`: save states

`[sync] states = true` syncs save states as well as saves. It is off by default
for a reason worth reading before turning it on: **a state is a snapshot of one
emulator core's memory, at one version of that core.** It loads in that core and
no other, and a core update can make every state it wrote before refuse to load
— or load into a crash. A save survives both; a state survives neither. What a
bad state costs is a resume, never progress: states never touch a save file, and
a local state is backed up under `.backup/` before anything is written over it
(docs/SYNC_PROTOCOL.md#save-states).

So a state moves only between devices running the same core (#239): one is
uploaded with the id of the core that wrote it, a server state written by
another core is never downloaded or placed here, and one this console has no
emulator for is passed over without a word — RomM has no negotiation for states,
so this client filters the server's list itself. DuckStation's (`tico-duckstation`)
meet only DuckStation's on another Switch; RetroArch's PSX cores meet only
themselves.

Tico and RetroArch number their slots differently. `map_state_slots = true`
syncs a Tico state of a libretro module (every Tico system whose
[`emulator`](#emulator-which-emulator-romm-is-told-wrote-a-platforms-saves) is a
RetroArch core) under RetroArch's name for the same slot, and places RetroArch's
back under Tico's:

| Slot | Tico file | RetroArch file |
|---|---|---|
| menu slot k (1–5) | `<game>.state{k-1}` (`.state0`–`.state4`) | `<game>.state{k}` |
| Auto (written on exit) | `<game>.state5` | `<game>.state.auto` |
| RetroArch slot 0 | — | `<game>.state` |

With it on, a game Tico suspended on exit resumes in RetroArch's Auto slot on a
PC, and back. RetroArch's slot 0 and slots past 5 have no Tico file and are not
placed in Tico's folder. Off — the default — each frontend's states keep their
own names, Tico's are placed only in Tico's folder and RetroArch's only in
RetroArch's, and Tico's `.state1`–`.state5` share their names with RetroArch's
slots 1–5 a slot apart — so with it off, a Tico state of a libretro module
that reaches RetroArch lands one slot along, and Tico's Auto in RetroArch's
slot 5. Turn it on on every Switch or on none: the server's names do not say
which numbering wrote them. Changing it renames every Tico state this console
sends. A renamed state that meets a row synced from another of its files —
`Game.state0`, now `Game.state1`, meeting the row the card's own `Game.state1`
went up as — is kept apart rather than uploaded over it, and said so every tick
until that row is deleted in RomM; the Auto slot, a new name either way, simply
uploads. The `.png` Tico (and RetroArch) keeps beside a state is uploaded as the
state's screenshot in RomM.

### `conflict_show` hides the screen, never the recording

`[sync] conflict_show = false` takes the **Conflicts** row off the overlay's
menu, and that is all it does. The sysmodule still records every overwrite in
`conflicts.db` and still writes every backup under `.backup/` — a backup that
nothing recorded is a file whose name says neither the game nor the day, which
is the state the screen exists to fix (docs/SYNC_PROTOCOL.md#conflicts).

Turning it back on shows every conflict that happened while it was off. The
`[sync]` section of the settings screen reports the value, so a user who cannot
find the screen can see why in the same place.

### `on_server_delete`: a save deleted in RomM

RomM 5.4 tells a console when a save it holds was deleted on the server — in
the web UI, or by another device such as RetroArch deleting its `.srm` — and
asks it not to upload that copy again ([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#a-save-deleted-on-the-server)).

| value | the file on the card |
|---|---|
| `remove` (default) | copied under `/config/rommsync/.backup/` **first**, then removed. Listed on the **Conflicts** screen as *deleted on RomM*, from where it can be put back. |
| `keep` | left exactly as it is. `state.db` remembers it as deleted on the server, and while the file holds those bytes it is not offered to RomM at all — not even when an emulator writes the same bytes back on closing. Once you play and it changes, it is new progress and syncs as usual. |

Anything else is a `warning` and the current value stays. The overlay does not
show or edit this key yet; set it in this file.

### Where a rom lands

A download's destination is that platform's **first** `roms` folder with the
rom's `fs_name` — the name the file has on RomM's own filesystem — under it:

```
platform_fs_slug = gba, fs_name = synthetic-large.gba
  →  /tico/roms/gba/synthetic-large.gba
```

- The lookup key is `platform_fs_slug`, the directory name under RomM's
  `library/roms/` — never `platform_slug`. The two agree on a conventional
  library and part company on one whose PlayStation folder is called
  `playstation`, which is the library that needs a `[platform.playstation]`
  section rather than a `[platform.psx]` one.
- A platform with no mapping, or one mapped with no `roms` folder, has **no
  destination**. That rom is skipped with a reason; it is never guessed into a
  folder.
- The remaining `roms` entries are where the client looks to see whether the
  rom is *already* on the card. Only the first is ever written to.
- A `fs_name` is refused rather than repaired when it is not a plain file name:
  an empty one, a `/` or `\`, a bare `.` or `..`, a control character or a NUL,
  one of the characters FAT32 and exFAT reserve (`? * : " < > |`), a name over
  768 characters, or a resolved path over 768 characters — which is refused
  rather than truncated. That name comes off someone else's disk and the folder
  it would land in is one you mapped, so `../../atmosphere` is a refusal by name
  and not a path this client resolves.
- The reserved characters are the one refusal an ordinary library meets. RomM
  scans a Linux filesystem where all of them are legal, and `?` is conventional
  in a No-Intro name — `Where in Time Is Carmen Sandiego? (USA).nes`. Your SD
  card cannot store that name, so the rom is skipped with a reason instead of
  failing partway through the download. Renaming it in RomM is the fix; the
  client will not rename it for you, because it also looks for the rom on the
  card *by that name* and would download it again on every sync.

## Validation rules

Nothing in this file can stop the client from starting — not even a file from
a later release, which leaves it running on the defaults ([Version](#version)).
A line the parser
cannot use is reported and dropped, the built-in default stays in force, and the
rest of the file still applies — a console with no keyboard cannot be talked
through a parse error, and a sysmodule that refuses to boot over a typo is worse
than one running on defaults.

Each report carries a severity, the line number, and the section and key:

| severity | means |
|---|---|
| `notice` | taken as written, worth reading once |
| `warning` | that line did not take effect, or it did and carries a risk |
| `error` | the client cannot sync as configured |

**The file**

- `;` and `#` start a comment, at the start of a line or after whitespace. A `#`
  inside a word is part of the value, so a folder called `disc#2` survives.
- Section names and keys are case-insensitive; a `platform_fs_slug` is **not**,
  because it is a directory name on the server.
- A UTF-8 BOM, CRLF line endings and a missing final newline are all fine.
- A key set twice: the last **usable** line wins, `warning` — a later value the
  parser rejects leaves the earlier one in force, the same as anywhere else.
  A section repeated: the two are merged, `warning`.
- A section this client does not know → `warning`, and everything under it is
  ignored without a second complaint per line.
- Larger than 256 KiB → `error`, and the built-in defaults are used. It is read
  at boot into a sysmodule heap that a corrupt card could otherwise exhaust. For
  the same reason at most 256 platform sections, 64 directories per key and 64
  reports are honoured; past any of those the rest are counted, not kept.
- If `config.ini` is **missing** but `config.ini.old` is there, the `.old` is
  read with a `warning`. That is the window an interrupted write leaves: the
  sysmodule moves the live file aside before renaming the new one on, so the one
  moment the file legitimately does not exist is the moment your settings are
  sitting intact under the other name.

**`[server]`**

- `url` must be `http://` or `https://` with a real host. A trailing slash is
  dropped; a path prefix is kept, so RomM behind a reverse proxy at `/romm`
  works. A query or a fragment is refused — this is a server address, not a link.
  `https://:8080` and `https://host:` are refused too: both parse, and both would
  report a configured client that reaches nothing. `http://[::1]:8080` is fine.
- `https://user:password@host` is **refused**, not stripped: RomM authenticates
  with a bearer token, so credentials there are never sent anywhere and would
  only follow the server's name into every log line. No message ever quotes the
  URL, for the same reason.
- Plain `http://` is accepted with a `warning`: the token and every save cross
  the network in the clear.
- A `url` line the parser cannot use is a `warning`, like any other rejected
  line. Ending the file with no usable `url` at all is the `error` — one of them,
  whether the setting was absent or present and rejected. It is the one setting
  with no sensible default, and the client stays idle without it.

**`[sync]` / `[downloads]`**

- Booleans are `true`/`false`, `yes`/`no`, `on`/`off`, `1`/`0`, in any case.
  Anything else → `warning`, and the previous value stands.
- `verify_hash` decides two things, not one: whether a finished download is
  checked against the rom's `sha1_hash` (falling back to `md5_hash`), **and**
  whether "this rom is already on the card" can be answered at all. With it off
  there is no way to tell a rom that is already there and correct from one that
  is already there and truncated, so a rom queued again is downloaded again
  rather than assumed good. A download nothing could check still finishes; the
  queue entry says it was not verified and never claims it was.
- `slot` is 1–24 ASCII letters, digits, `-` and `_`. Anything else → `warning`,
  and the slot in force (`autosave` unless an earlier line set one) is kept.
- `interval_min` is a whole number of minutes. Negative or unparseable →
  `warning`, default kept. Above the 10080-minute (one week) maximum → clamped
  to it with a `warning`, because falling back to the 30-minute default would
  sync far more often than you asked for.

  **What the interval means across a suspend.** It is measured against the wall
  clock, not against a timer: a console that slept for eleven hours with a
  30-minute interval syncs **once** when it wakes and then goes back to the
  ordinary schedule, rather than firing the twenty-two ticks it "missed". A
  clock corrected backwards costs nothing at all — the monotonic clock is a
  floor under the interval, so a correction can lengthen a wait and never
  shorten one (`core/include/rommsync/scheduler.hpp`).

  `0` is not a very short interval, it is **no timer at all**: the sysmodule
  syncs at boot and when you press *Sync now*, and sleeps with no deadline in
  between. `enabled = false` is the same, minus the boot tick. Neither costs a
  wakeup.

  After a tick that could not reach the server the next one is not at the
  interval but on a **backoff** — 30 seconds, doubling, capped at 30 minutes,
  with a little jitter so several consoles on one network do not retry in
  lockstep. The first tick that works puts it back to the interval. A TLS
  failure is treated differently from an unreachable server: a certificate the
  console does not trust does not come right on its own, so after three the
  schedule parks until you press *Sync now* or change a setting.

**`[platform.<slug>]`**

- A slug this build ships no default for is honoured, with a `notice`. It cannot
  be checked against your library from here, and dropping it would silently
  discard a mapping for a platform RomM knows about and this client does not.
  Check it against RomM's `library/roms/`: a slug matching nothing maps nothing.
- Paths are absolute from the SD root, `/`-separated. Repeated slashes and a
  trailing slash are normalised away; `.` segments are dropped. A relative path,
  a `..` segment, a backslash, a control character, or anything over 768
  characters is refused with a `warning` and dropped from the list — `..` is
  refused rather than resolved, since these name folders you picked and
  resolving a typo would turn it into a folder that exists and gets written to.
- A path listed twice in one list is deduped (`notice`). The same directory
  under many platforms is normal and expected — RetroArch keeps one flat
  `saves/` for every system — and the scanner reads it once, matching those
  files by name across the library.
- A platform with `roms` but no `saves` → downloads work, saves are skipped
  for it.
- `emulator` is checked against RomM's rule for an emulator id (see
  [above](#emulator-which-emulator-romm-is-told-wrote-a-platforms-saves));
  a refused one is a `warning` and the built-in id stays in force.
- A section left with no folders at all is skipped entirely. Doing that on
  purpose (every key set to nothing) is a `notice`; arriving there because every
  line in the section was dropped — `rom` for `roms`, say — is a `warning`, since
  one typo silently unmapping a platform looks exactly like removing it.

## Precedence

built-in defaults  ◀  `config.ini` overrides  ◀  overlay live changes (persisted
back to `config.ini` by the sysmodule).

"Override" is per platform for the folder map — a `[platform.x]` section
replaces the built-in entry for `x` and leaves every other platform alone — and
per key everywhere else.

## Editing from the overlay

The overlay never writes this file. It sends the sysmodule one `SetConfig` — a
short list of `section`, `key`, `value` — and the sysmodule validates it,
rewrites the file and re-reads it, so the change is in force before the call
comes back. No restart, and no second copy of the settings anywhere.

**Your file survives the edit.** One line changes; every other byte is the byte
you typed. Comments, blank lines, the order of your `[platform.x]` sections, a
section this build has never heard of, a UTF-8 BOM and CRLF endings all come
through untouched, because the file is edited as text rather than regenerated
from what the parser understood of it. A key that is set twice has its **last**
line rewritten, which is the one the parser uses; a key that is not there yet is
added to the end of its section; a section that is not there is added to the end
of the file.

Three things do change beyond the one line:

- the value is stored **normalised** — the form the client actually loads. A URL
  keeps no trailing slash, `YES` is stored as `true`, and a folder list is
  stored deduplicated with its paths tidied.
- "reset to default" removes the key, and removes **every** occurrence of it in
  that section. Leaving an earlier one would hand the old value straight back on
  the next boot.
- setting a folder for a platform that has **no section in your file yet** — or
  whose section sets only `emulator` — writes that platform's built-in mapping
  out with your change applied on top, and says so. A `[platform.x]` section replaces the built-in entry rather than adding to
  it, so writing only the key you changed would unmap that platform's other
  folders as a side effect. Emptying one on purpose is still `roms =`.

**An edit is refused where the boot path would carry on.** Nothing in this file
can stop the client from starting, so a bad line already on the card is dropped
or clamped and reported. An edit arriving from the overlay is different: there
is somebody looking at the screen who can be told, and writing a value the next
boot would drop is a setting that looks saved and is not. So an edit is refused
outright — with the reason, and **nothing at all written** — when it carries a
`url` that is not a usable server address, a folder path that is not absolute or
that contains `..`, a boolean that is not one, an `interval_min` that is
negative or **above the 10080-minute maximum** (the read path clamps that one;
this one tells you the ceiling), a section or key this client does not have, or
a folder name that could not be read back — `/roms/great #2` is a legal SD path
and an illegal value here, because a `#` after a space starts a comment.

An edit is applied as a unit. A settings screen that sends four values and gets
one of them wrong leaves the card exactly as it was, rather than three-quarters
changed.

**Changing `url` un-pairs the console**, and does so before the file is written
(see below). The token in `token.dat` was issued by the RomM you were pointed at
and is meaningless to any other — so pointing the
client at a different server discards it rather than sending your bearer token
to a host that never issued it. Pair again from the overlay. Editing anything
else leaves the pairing alone.

If the write itself cannot happen — no card, a full one — the answer says so and
your settings are unchanged: the file is committed with the same two-rename
write the token and device records use, so an interrupted one leaves your
previous settings under `config.ini.old` and the next boot reads them from
there.

The one thing that is *not* put back is the pairing, and only on an edit that
changed `url`. The token is discarded **before** the file is written, because
the other order would leave a moment where the card names a new server and still
holds the old server's credential. So a `url` edit whose write then failed
leaves you pointed at the server you were already using and needing to pair
again; the answer says that too.
