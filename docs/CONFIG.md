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

## Validation rules

- Unknown platform section → ignored with a warning.
- A platform with `roms` but no `saves` → downloads work, saves are skipped for it.
- RetroArch's flat `saves/`/`states/` may be listed under many platforms; the
  scanner dedupes shared dirs and matches those files by name across the library.

## Precedence

built-in defaults  ◀  `config.ini` overrides  ◀  overlay live changes (persisted
back to `config.ini` by the sysmodule).
