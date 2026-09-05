# Configuration

`sdmc:/config/rommsync/config.ini`. The sysmodule reads it at boot and on an
IPC "reload"; the overlay edits it through the sysmodule (which owns writes).

```ini
[server]
url = https://romm.example.com     ; RomM origin, HTTPS strongly recommended

[sync]
enabled       = true
interval_min  = 30                 ; 0 = only boot + manual
on_boot       = true
saves         = true
states        = false              ; fragile across cores; opt-in
conflict_show = true               ; surface conflicts in the overlay

[downloads]
enabled       = true
verify_hash   = true               ; check sha1 after download
resume        = true

; Platform folder map: RomM platform_fs_slug -> SD folders.
; roms   = where downloads land (first entry is the write target)
; saves  = dirs to scan/sync for SRAM saves
; states = dirs to scan/sync for save states
[platform.snes]
roms   = /tico/roms/snes
saves  = /retroarch/saves, /tico/saves/snes
states = /retroarch/states, /tico/states/snes

[platform.gba]
roms   = /tico/roms/gba
saves  = /retroarch/saves, /tico/saves/gba
states = /retroarch/states, /tico/states/gba

; ... nes, gbc, gb, n64, genesis, psx, nds, dreamcast, psp
```

## Defaults & the folder map

- A built-in default map ships for the Switch-capable systems (nes, snes, gb,
  gbc, gba, n64, genesis, psx, nds, dreamcast, psp). `config.ini` only needs to
  contain **overrides**.
- Keys are RomM `platform_fs_slug` values (what you see under RomM's
  `library/roms/`). The right-hand paths are absolute from the SD root.
- Heavy platforms with no Switch emulator (ps2/ps3/ps4/wii/ngc/3ds) are
  intentionally unmapped; the client skips anything without a mapping.
- **Verify Tico's real save paths** on your SD and correct `saves`/`states` —
  the Tico defaults are best-guesses; RetroArch's defaults are correct.

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
  a `/` or `\`, a bare `.` or `..`, a control character, a NUL, or a resolved
  path over 768 characters, which is refused rather than truncated. That name
  comes off someone else's disk and the folder it would land in is one you
  mapped, so `../../atmosphere` is a refusal by name and not a path this client
  resolves.

## Validation rules

Nothing in this file can stop the client from starting. A line the parser
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
- `interval_min` is a whole number of minutes. Negative or unparseable →
  `warning`, default kept. Above the 10080-minute (one week) maximum → clamped
  to it with a `warning`, because falling back to the 30-minute default would
  sync far more often than you asked for.

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
