# Privacy: what leaves the console

Everything sys-rommsync sends, where it goes, what it keeps on the SD card, and
how to turn off the one thing save sync does not need. The overlay sends
nothing of its own: it talks only to the sysmodule, on the console.

`tests/test_docs_install.py privacy` checks the field names below against the
code that builds each request, so a field added to a request and left off this
page fails the build.

## Where it goes

Only to the RomM server named by `[server] url` in `config.ini`: the one you
paired this console with, and usually one you run yourself. There is no other
destination. No analytics, no crash reporter, no update check, no third-party
service, and nothing listens for connections on the console
([SECURITY.md](SECURITY.md#what-we-never-do)). Like any network client, the
console's IP address is visible to that server.

Over plain `http://` all of it, the token included, crosses your network
unencrypted ([SECURITY.md](SECURITY.md#exposing-romm-safely)).

## What is sent

**On every request**

- `User-Agent: rommsync-nx/<version>`, the client's name and release.
- The bearer token RomM issued at pairing. The unauthenticated heartbeat, which
  reads RomM's version, carries no token.

**Once, when you pair** (`POST /api/auth/device/init`)

- `client_device_identifier`: `nx-` and 32 hex digits, a salted hash of the
  console's serial. The serial itself never leaves the console
  ([SECURITY.md](SECURITY.md#the-console-identifier)).
- `name`: always `rommsync-nx on Switch`. Not the console's nickname, and not
  your Nintendo account.
- `client` (`rommsync-nx`), `platform` (`switch`) and `client_version`.
- `requested_scopes`: the documented minimum
  ([API_CONTRACT.md](API_CONTRACT.md#scopes-to-request)).

**Each sync, per save on the card that RomM has a game for**
(`POST /api/sync/negotiate`)

- `device_id`, the id RomM gave this console at pairing.
- `saves`, and for each save: `rom_id`, `file_name`, `slot`, `emulator`,
  `content_hash` (an MD5 of the save's bytes), `updated_at` (its modified time)
  and `file_size_bytes`.
- `rom_ids` and `emulators`: which games and emulators this sync covers, so
  RomM can offer saves the card does not have yet; and `restore_unlisted`, sent
  only just after a rom is downloaded, asking for that game's save back.
- The **save files themselves**, when RomM's plan says to upload one. Save
  states too, but only with `[sync] states = true` (off by default).

**At the end of each sync** (`POST /api/sync/sessions/{id}/complete`)

- `operations_completed` and `operations_failed`: how many uploads and
  downloads worked.
- `play_sessions`: see [Play sessions](#play-sessions). An empty list when
  there are none or they are turned off.

**For the library and downloads**

- Reads of your library, platforms and collections, to match saves to games and
  to fill the overlay's library screen.
- Downloads of the roms you queue.
- With RomM's remote install: `capabilities` (whether this console accepts
  installs), a claim for the installs queued for it, and a `status` with a short
  `reason` for each one it finishes or refuses.
- Before pruning `.backup/`, a read of each game's save hashes on the server,
  so a backup whose bytes exist nowhere else is never deleted.

## Play sessions

RomM shows play time per game. sys-rommsync works it out from the saves: when a
save changed between two syncs, it records a session for that game from the
last time it looked until the save's modified time, at most 12 hours. That is
an upper bound, not a stopwatch; the console has no record of when a game
actually started.

What is sent per session: `rom_id`, `save_slot`, `start_time`, `end_time` and
`duration_ms`, inside the completion above. Nothing else.

The sysmodule also notes which application was in the foreground (its title
id, read from the system's process manager), so a session can say which
program wrote the save. That **stays on the console**: it is written to
`play.db` and to the log, and no request carries it. The same question to the
process manager is how a sync waits while a game is running, so it is asked
with play sessions off too; then the answer is kept in memory only, and
dropped at the next sync.

Sessions wait in `/config/rommsync/play.db` until RomM confirms them, so an
offline console loses none. The file holds at most 48; the oldest are dropped
first.

**On by default.** The only place sessions go is your own RomM, and its play
time is a reason people pair a console with it. To turn it off, set the
**`play_sessions`** row under `[sync]` on the overlay's settings screen to Off,
or write this in `config.ini`:

```ini
[sync]
play_sessions = false
```

From the next sync, nothing is recorded, `play.db` is not written, and every
completion carries an empty `play_sessions`. Sessions already in `play.db` stay
there unsent; delete the file to discard them. When you turn it back on, the
first sync reports nothing, so play from while it was off is never sent. Saves
sync the same either way ([CONFIG.md](CONFIG.md#play_sessions-play-time-sent-to-romm)).

## What stays on the console

Under `/config/rommsync/` on the SD card, readable by any homebrew and by anyone
who takes the card out:

| file | what it holds |
|---|---|
| `config.ini` | your settings, including the server URL |
| `token.dat` | the bearer token, in the clear ([SECURITY.md](SECURITY.md#token-at-rest-on-the-sd)) |
| `device.dat` | the hashed console identifier |
| `state.db` | each synced save's game, file name, hash, size and modified time |
| `conflicts.db` | every save overwritten, and where its backup is |
| `play.db` | play sessions waiting to be sent, with the title id seen for each |
| `queue.json` | the roms queued for download |
| `rommsync.log` | what each sync did |
| `.backup/` | a copy of every save before it was overwritten |
| `diagnostics-<time>.txt` | only if you pressed *Write diagnostics*: see below |

The log names the server URL, this console's RomM device id, save paths, rom
ids and the play sessions recorded. It never contains the token: it says only
whether one is present and how long it is. It is never uploaded. If you attach
it to a bug report, that is the list of what you are sharing.

A diagnostics file is written only when you press **Y** (*Write diagnostics*)
on the overlay's log screen, one new file per press. It holds the versions of
the overlay, the sysmodule, libultrahand, libnx and the firmware; the status
screen's numbers; how many rows `state.db` has and how much `.backup/` holds;
`config.ini`, including the server URL; the last 200 log lines; and the names
and dates of Atmosphère's crash reports for sys-rommsync. Any value whose key
names a token, code, password or secret is replaced by `<redacted>`, every line
is scrubbed the way the log is, and `token.dat` and `device.dat` are never read.
It is never uploaded; attaching it to a bug report is your choice, and it is
the list of what you are sharing
([TROUBLESHOOTING.md](TROUBLESHOOTING.md#what-to-attach-to-a-bug-report)).

## What is never sent

- The console's serial number. Only the salted hash above.
- Your Nintendo account, its nickname, or the console's name.
- The title id of any running game, or a list of installed software.
- Any file outside the save and state folders `config.ini` maps, and states only
  when `[sync] states = true`.
- The log, a diagnostics file, `state.db`, `conflicts.db` or anything else
  under `/config/rommsync/`, except the play sessions in `play.db` as described
  above.

## Turning things off

| setting | what stops |
|---|---|
| `[sync] play_sessions = false` | play sessions; saves still sync |
| `[sync] states = false` (default) | save states |
| `[downloads] enabled = false` | rom downloads and remote installs |
| `[sync] enabled = false` | every sync, and with it every save, state and session |
| Un-pair, then revoke the client token in RomM | everything ([INSTALL.md](INSTALL.md#7-uninstalling-and-un-pairing)) |

Turning a sync off does not delete what RomM already has. Saves, states and
play sessions already sent stay on the server until you delete them there.
