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
verify_hash   = true               ; check sha1 (or md5) after download
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
- `verify_hash` decides two things, not one: whether a finished download is
  checked against the rom's `sha1_hash` (falling back to `md5_hash`), **and**
  whether "this rom is already on the card" can be answered at all. With it off
  there is no way to tell a rom that is already there and correct from one that
  is already there and truncated, so a rom queued again is downloaded again
  rather than assumed good. A download nothing could check still finishes; the
  queue entry says it was not verified and never claims it was.
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
- setting a folder for a platform that has **no section in your file yet** writes
  that platform's built-in mapping out with your change applied on top, and says
  so. A `[platform.x]` section replaces the built-in entry rather than adding to
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

**Changing `url` un-pairs the console.** The token in `token.dat` was issued by
the RomM you were pointed at and is meaningless to any other — so pointing the
client at a different server discards it rather than sending your bearer token
to a host that never issued it. Pair again from the overlay. Editing anything
else leaves the pairing alone.

If the write itself cannot happen — no card, a full one — the answer says so and
nothing changed: the file is committed with the same two-rename write the token
and device records use, so an interrupted one leaves your previous settings
under `config.ini.old` and the next boot reads them from there.
