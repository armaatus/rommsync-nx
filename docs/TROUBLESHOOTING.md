# Troubleshooting

The page for when a sync did not happen. Every failure this client knows how to
have, the line it writes when it has one, and what to do about it.

> **Not yet validated on hardware.** Nothing in this repository has run on a
> Switch: hard rule 1 in [CLAUDE.md](../CLAUDE.md) keeps a console on the far
> side of the **M8-1** gate. Every log line quoted below is one a test makes the
> code produce on a laptop, against a real RomM — a build pinned to those lines
> is the closest thing to a verified guide that can exist before that gate, and
> **M8-2** is where a person follows this page on a console and fixes what is
> wrong with it. The three places where the console differs from the laptop are
> marked *not verified until M8* where they come up.

[INSTALL.md](INSTALL.md) is the page for putting this on a card in the first
place, and it already answers three things this one does not repeat: the
sysmodule not running, the overlay saying **"No server set"**, and where the
folder map lives.

---

## Reading the log

`sys-rommsync` writes one file:

```
sdmc:/config/rommsync/rommsync.log
sdmc:/config/rommsync/rommsync.log.old
```

Take the card out and open them in a text editor. `.old` is the previous file;
each is capped at **32 KiB**, so the pair never costs more than 64 KiB of the
card and never grows without bound. There is no setting to make it bigger and
none to turn it off.

Every line is the same four fields:

```
<ordinal> <level> <event> <detail>
```

```
1 info boot rommsync-nx/0.1.0
2 info auth.token server=https://romm.example.com device=console-1 scopes=[roms.read] credential=present(68 chars) expires=never
3 warn net.offline GET /api/roms: connection failed; the rom index could not be fetched: connection failed (Connection refused)
4 info sync.tick outcome=offline completed=0 failed=0 states_completed=0 states_failed=0 baseline=not stored
```

That is a whole failed sync: the build, the pairing it is using, what went
wrong, and how the tick ended.

- **ordinal** counts up from 1 for as long as the sysmodule has been running. It
  is what orders the file, and a jump back to `1` is a reboot.
- **level** is `error`, `warn` or `info` — the same three the settings screen
  draws for `config.ini`.
- **event** is the tag this page is organised by. Find it in the table below and
  read that section.
- **detail** is whatever the part that failed had to say.

**Only the first three fields are promised.** The ordinal, the level and the
event tag are a contract — a build is tested against the tag list in this page,
both ways. The detail after the tag is the sentence the failing part already had
for itself, and its exact wording moves when that part's does. So match what you
see against the **tag**, and read the detail; do not expect it word for word.

**Every tick ends with a `sync.tick` line**, whatever ended it — including the
ones that give up before they send anything. So the quickest way to read a log
is backwards: find the last `sync.tick`, read its `outcome=`, and then read
upwards to the lines that explain it.

**There is no timestamp, deliberately.** A Switch answers "what time is it" with
nothing usable until the clock service has come up, and a console whose clock
never initialises is a state this client supports rather than refuses — so a
stamped log would date every line to 1970 on exactly the consoles that are
hardest to debug. The ordinal is what puts the lines in order.

**No secret is ever written.** A bearer token, a `device_code` and the
`user:password@` half of a URL are replaced with `<redacted>` on the way to the
file, by the writer rather than by each caller, so the rule holds for a line
nobody reviewed. You can paste the log into a bug report without reading it
first. What it *does* name is the server's address, because "which RomM is this
console paired to" is the first question of every support thread.

**The last few lines can be missing after a power cut.** The log is not flushed
to the card the way a save is — that guarantee costs a full card commit per line
and a log is not worth it. A save is never in that position; see
[SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#backups).

### The events, and where each one is answered

| event | means | section |
|---|---|---|
| `boot` | which build started, and whether it has a network transport | [What to attach to a bug report](#what-to-attach-to-a-bug-report) |
| `auth.token` | which server, device and scopes the stored pairing names | [What to attach to a bug report](#what-to-attach-to-a-bug-report) |
| `config.diagnostic` | one complaint about `config.ini` | [config.ini complaints](#configini-complaints) |
| `config.no_server` | there is no usable `[server] url` | [The server URL is wrong or missing](#the-server-url-is-wrong-or-missing) |
| `config.no_save_dirs` | the folder map names no save folders at all | [The folder map switched saves off](#the-folder-map-switched-saves-off) |
| `net.offline` | the request never completed | [Nothing syncs, and nothing is reachable](#nothing-syncs-and-nothing-is-reachable) |
| `net.tls` | the TLS handshake or the certificate was refused | [TLS fails](#tls-fails) |
| `auth.rejected` | the server has stopped accepting this console | [The token expired or was revoked](#the-token-expired-or-was-revoked) |
| `scan.skipped` | a save file was not matched to a rom, or a server's save will not be taken here | [Saves are skipped: nothing matches](#saves-are-skipped-nothing-matches) |
| `sync.refused` | the server answered, and the answer will not change | [The server refused the sync](#the-server-refused-the-sync) |
| `save.failed` | a save could not be written — usually a full card | [The SD card is full](#the-sd-card-is-full) |
| `sync.tick` | how one sync ended — every tick writes one | [What to attach to a bug report](#what-to-attach-to-a-bug-report) |
| `download.drain` | how one pass at the download queue ended — how many roms came down, and what stopped it | [A queued rom never arrives](#a-queued-rom-never-arrives) |
| `play.failed` | play time could not be recorded — no save is at risk | [The SD card is full](#the-sd-card-is-full) |
| `power.sleep` | the console went to sleep while this client was still busy | [The console slept mid-sync](#the-console-slept-mid-sync) |
| `server.version` | which RomM release the server says it runs — once a boot and once a pairing | [What to attach to a bug report](#what-to-attach-to-a-bug-report) |
| `server.unsupported` | the server is older than RomM 5.4.0, so nothing is synced with it | [RomM is too old](#romm-is-too-old) |
| `sync.held` | a sync fell due while a game was open, and waits for it to close | [Nothing syncs while a game is open](#nothing-syncs-while-a-game-is-open) |
| `save.deleted` | RomM said a save on this console was deleted on the server, and what became of the file | [A save disappeared from the card](#a-save-disappeared-from-the-card) |
| `download.remote` | a rom queued for this console in RomM's web UI was taken into the queue or reported back, or the server would not answer about one | [A queued rom never arrives](#a-queued-rom-never-arrives) |
| `play.recorded` | a play session this console recorded, and the title seen writing its save — or `unknown` | [Nothing syncs while a game is open](#nothing-syncs-while-a-game-is-open) |
| `process.terminated` | the sysmodule stopped itself — usually out of memory — and this was the last thing it wrote | [The sysmodule stopped](#the-sysmodule-stopped) |

The overlay shows the end of the log under **Settings → Log**, read off the
sysmodule over IPC — `GetLog`, command 16 in
[DEVELOPMENT.md](DEVELOPMENT.md#the-command-set) — so the lines above can be
read without an SD reader. It is the last lines the sysmodule kept in memory;
the whole log is the file on the card.

---

## Nothing syncs, and nothing is reachable

**Symptom.** The status screen says the console is offline. Saves never appear
in RomM and never come back from it. Nothing changes on the card.

**Log line.**

```
3 warn net.offline GET /api/roms: body ended early; the rom index could not be fetched: body ended early (transfer closed with 14822 bytes remaining to read)
3 warn net.offline negotiate: connection failed; the negotiation never completed
2 warn net.offline tick skipped: this build has no transport
```

The part before the `;` is where it failed and how — `GET /api/roms` is the
library fetch a tick starts with, `negotiate` is the call that decides what
syncs. The part after it is the same failure as that step already described it.

**What is happening.** The request never completed — the name did not resolve,
the connection was refused, the link went away mid-body, or the server stopped
answering. The tick is **abandoned before anything is written**: no save is
touched, no backup is taken, `state.db` is not rewritten
([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#failure--safety-rules)). A missed tick is
harmless; the next one picks up where this one would have.

**The fix.**

1. Check the console's Wi-Fi. This client never touches the network at boot, so
   a console that has just woken up may genuinely have no link yet.
2. Check that RomM is up and reachable *from the console's network* — a server
   on a VPN or a tailnet the console is not on is unreachable in exactly this
   way.
3. Check `[server] url` for a port. RomM on `:8080` behind no reverse proxy
   needs the port written out.
4. If the detail says **host could not be resolved**, it is DNS: try the
   server's IP address in `[server] url` to confirm, then fix the name.

`tick skipped: this build has no transport` is a different sentence and is not
a network problem — it is a build with no HTTP client wired in, which is not
something a release does. If you see it on a console, that is worth reporting.

---

## The token expired or was revoked

**Symptom.** The status screen says the console needs pairing again. Everything
was working and then stopped, all at once, without the network changing.

**Log line.**

```
3 error auth.rejected GET /api/roms answered 401; the rom index was refused with status 401
7 error auth.rejected the server stopped accepting this console's token: refused with 401
2 error auth.rejected tick skipped: this console has no usable token; pair it again
```

**What is happening.** RomM answered **401**. The client stops there rather than
spending a whole plan's worth of requests on credentials that are gone — and it
never reads a 401 as "the server has no saves", which would be a plan to upload
everything over the server's copies. RomM issues tokens with no expiry to
refresh, so nothing here comes back on its own
([AUTH.md](AUTH.md#re-pairing--revocation)).

After a few rejections in a row the console stops asking altogether, and only a
new pairing lifts that. A console in that state writes two lines per sync
interval and makes no request at all:

```
41 error auth.rejected this console is blocked until it is paired again; no request will be made
42 info sync.tick outcome=unauthorized completed=0 failed=0 states_completed=0 states_failed=0 baseline=not stored
```

**The fix.** Pair the console again — **Re-pair** on the overlay's settings
screen, or step 4 of [INSTALL.md](INSTALL.md#4-pair-the-console). Re-pairing
leaves `device.dat` alone, so RomM recognises the same console instead of
collecting a second one.

**A 403 is not this, even though it is logged under the same tag.** 403 means
the pairing is real and was not granted a scope this call needs — RomM approves
what the *user* ticked at the browser, which need not be everything that was
asked for. Re-pairing and ticking the boxes is the fix, and the scopes this
client asks for are in [API_CONTRACT.md](API_CONTRACT.md#scopes-to-request).

---

## The server URL is wrong or missing

**Symptom.** The overlay says **"No server set"**, or it says nothing is wrong
and no request ever reaches RomM.

**Log line.**

```
3 error config.no_server no usable [server] url; nothing will sync until one is set
2 error config.diagnostic [server] url: has no usable value, so there is no RomM to sync with
```

**What is happening.** There is no origin to send anything to, so the tick
returns before it sends. **The log never quotes the URL back at you**, and
neither does the diagnostic on the settings screen: a URL is the one field here
that can carry a password, and both of these end up somewhere they should not.

**The fix.** Set `[server] url` in
`sdmc:/config/rommsync/config.ini` — scheme, host, and the port if RomM is not
behind a proxy on 80 or 443:

```ini
[server]
url = https://romm.example.com
```

`user:password@` in the URL is refused outright rather than used; this client
authenticates with a paired token and nothing else.

**A URL that reaches something that is not RomM** is a different failure with
the same cause. It shows up as [The server refused the
sync](#the-server-refused-the-sync) — a reverse proxy answering its own error
page, or a login portal answering HTML where a rom index was expected.

---

## TLS fails

**Symptom.** An `https://` server, a console that reaches the network fine, and
every request failing instantly rather than slowly.

**Log line.**

```
2 error net.tls GET /api/roms: TLS failure; the rom index could not be fetched: TLS failure (error:1404B42E:SSL routines:ST_CONNECT:tlsv1 alert protocol version)
```

The text in brackets is the TLS library's own, and it is the most useful part:
`alert protocol version` is cause 3 below, and a message naming a certificate or
a hostname is cause 1 or 2.

**What is happening.** The handshake did not complete, or the certificate was
not one the console trusts. It is kept apart from `net.offline` because it does
not come right on its own — the schedule stops retrying after a few attempts
rather than re-handshaking every half hour forever.

The three causes, in the order they are likely:

1. **`https://` in front of a server that speaks plain HTTP.** RomM on `:8080`
   with no reverse proxy is `http://`, not `https://`.
2. **A certificate no public CA signed.** A home server with a self-signed
   certificate has to have that certificate *imported* — verification is not
   turned off, and there is no setting in this client that turns it off. See
   [SECURITY.md](SECURITY.md#a-self-signed-certificate-on-a-home-server).
3. **A server offering only TLS 1.3.** The console's TLS ceiling is **1.2**
   ([DEVELOPMENT.md](DEVELOPMENT.md#tls-in-a-sysmodule)), so a reverse proxy
   configured 1.3-only cannot be talked to at all. Allow 1.2 as well.

**The fix.** Take them in that order: correct the scheme, then import the
certificate, then check the proxy's minimum TLS version.

*Not verified until M8:* causes 2 and 3 are properties of the console's own TLS
stack. What is checked today is that the client reports them as `net.tls` rather
than as a dropped connection, and the M0-1 probe under `tlsprobe/` is what
measured the 1.2 ceiling.

---

## Saves are skipped: nothing matches

**Symptom.** Some games sync and others never do. The ones that do not have
nothing obviously different about them.

**Log line.**

```
5 warn scan.skipped unmatched /retroarch/saves/Game (USA).srm: no rom named "Game (USA)"
6 warn scan.skipped ambiguous /retroarch/saves/Game.srm: "Game" matches 2 roms (gb, gba)
```

**What is happening.** A save is matched to a rom by its **base name** — the
file name with the save extension taken off — against the file names in your
RomM library. A save whose base name matches nothing, or matches on two
platforms with no folder to say which, is **skipped and logged, never guessed
at** ([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#step-0--matching-local-files-to-roms)).
Guessing would mean writing one game's save over another's through the server.

Two folder layouts behave differently here, and it matters:

- **Tico's per-system folders imply a platform**, so a name that exists on two
  systems is still unambiguous.
- **RetroArch's flat `saves/`** implies nothing, so the same name is matched
  across your whole library — and that is where `ambiguous` comes from.

**The fix.**

- `unmatched` — make the save's base name match the rom's file name in RomM.
  Renaming either works; the rom's name in RomM is what the client reads.
- `ambiguous` — move the save into a per-platform folder and map that folder for
  that platform in `config.ini` ([CONFIG.md](CONFIG.md#defaults--the-folder-map)).
  A folder that belongs to one platform is what resolves it.
- `save folder` — a PSP save. PPSSPP keeps each game's save as a folder, and
  PSP saves are not synced yet; nothing to fix
  ([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#formats-only-one-emulator-reads)).

A PS1, N64, Dreamcast, NDS or Saturn save that **is** matched can still be
declined at the download, in a `scan.skipped declined` line saying the two are
not the same save format: the server's copy came from RetroArch, whose format for that system
Tico's module does not read, so it is never written over the card. It syncs with
the same module on another Switch only.

The same kind of line names a save the console has never had and will not
place — the platform has no folder for the emulator it is played in, Tico has
several accounts and none chosen, or a file the scan did not pair is already
where it would go:

```
8 warn scan.skipped declined download rom 6, slot autosave: there is no local save for this (rom_id, slot), and /tico/saves/psx/Game.mcd is already a file this sync did not pair with it; it was not touched
```

None of these is a failed sync, and the sync is not reported partial on their
account. Each is said **once**, and again only when something changes — a new
copy on the server, the file moving, or the reason the placement gave — or after
a reboot. Moving the file in the way, or fixing what the reason names, lets the
next sync bring the save down.

---

## The folder map switched saves off

**Symptom.** Roms download. Saves never sync, in either direction, and nothing
looks broken.

**Log line.**

```
11 info config.diagnostic [platform.snes] saves: is not set, and a platform section replaces the built-in map for that platform rather than adding to it, so snes saves will not sync
12 warn config.no_save_dirs the folder map yields no save directories; no save will be synced
```

**What is happening.** This is the common one. **A `[platform.*]` section
replaces that platform's built-in mapping rather than adding to it.** Writing

```ini
[platform.snes]
roms = /roms/snes
```

leaves snes with **no** save folders, because you did not list any — so roms
land and saves are silently not looked for. Emptying a section is the supported
way to switch a platform off entirely, which is why this cannot be treated as a
mistake and fixed for you.

`config.no_save_dirs` is the worse version: *no* platform maps a save folder, so
the console will never sync a save at all.

**The fix.** List every key you want that platform to have:

```ini
[platform.snes]
roms   = /roms/snes
saves  = /retroarch/saves
states = /retroarch/states
```

Two things worth knowing while you are in there
([CONFIG.md](CONFIG.md#defaults--the-folder-map)):

- **The shipped save paths are best guesses.** Tico's in particular are a guess
  at where that emulator keeps saves; if yours are somewhere else, map them.
- **The keys are folders your emulator actually writes to.** A mapped folder
  that does not exist is not an error — a platform you do not play yet has no
  folder — so a typo looks exactly like a platform with nothing in it.

---

## The SD card is full

**Symptom.** Saves stop coming down. Files named `<save>.part` or `<save>.tmp`
appear beside your saves and never turn into anything.

**Log line.**

```
9 error save.failed download Game (USA).srm: the previous bytes could not be backed up
9 error save.failed the baseline could not be written
```

**What is happening.** Something could not be written to the card. The important
part is what *did not* happen as a result:

- **A backup that fails aborts the overwrite it was protecting.** No backup, no
  overwrite — that is hard rule 2, and it means a full card costs you the
  incoming save and never the one you had.
- **A save is never half-written.** A download lands in `<save>.part`, is
  renamed to `<save>.tmp` only when the whole body has arrived, is checked
  against the digest RomM gave, and only then replaces the file. A card that
  fills up mid-transfer leaves a `.part` and nothing else.
- **A leftover `.tmp` is deleted on the next tick, not committed.** It is a
  complete body that may not be the right one, with no backup beside it, and
  nothing after the fact can tell — so it costs one re-fetch to say no.

- **A rom that will not fit is refused before it starts.** Before the first
  byte of a download moves, the sysmodule asks the card how much space is left;
  a rom bigger than that fails at once, and the queue screen says how much is
  free and how much the rom needs. A card that fills up *during* a download
  anyway fails the rom with "the card is full" and deletes the partial file, so
  the space it took comes back. The rom already on the card under that name, if
  there is one, is not touched.

A full card also stops the client's own records being written, and one of those
has a line of its own:

```
9 error play.failed /config/rommsync/play.db: the bytes did not all reach the card
```

`play.failed` is play time, not save data — **no save is ever at risk from it**
(that is always `save.failed`). What it costs is the play time this console had
recorded and not yet sent, and the record of when it last looked at your saves,
so play time stops being reported until the card can be written again. Your saves
keep syncing throughout.

**The fix.** Free space on the card. Then look for leftovers:

- `<save>.part` and `<save>.tmp` next to your saves are swept on the next sync.
- `sdmc:/config/rommsync/.backup/` holds every save this client has ever
  overwritten. It is not swept, deliberately — it is what the overlay's
  **Conflicts** screen restores from. Deleting old backups by hand is safe and
  is the fastest space you will find.

---

## `config.ini` complaints

**Symptom.** A setting you wrote is not in force, and the settings screen shows
a complaint.

**Log line.**

```
1 warn config.diagnostic line 5 [sync] states: expected true or false, got 'yes please'; keeping false
1 info config.diagnostic sdmc:/config/rommsync/config.ini does not exist; the built-in defaults are in use
```

**How to read one.** `line 5 [sync] states:` is the line number, the section and
the key — which is the whole point of the format. "Invalid boolean" would send
you hunting through a file you cannot see on a console; this tells you what to
edit.

The level tells you how much it cost you:

| level | means |
|---|---|
| `info` | worth reading once. A console with no `config.ini` yet, an unrecognised platform name. Nothing is wrong. |
| `warn` | that line did not take effect as written — it was ignored or replaced by its default. Everything else in the file is in force. |
| `error` | the client cannot do its job as configured. In practice this is always the server URL. |

**A bad `config.ini` never stops the client.** It cannot: a console with no
keyboard cannot be talked through a parse error, and nothing may block boot. The
file is parsed as far as it goes, every bad line is reported and replaced by its
default, and the client runs on what is left. That is why a typo shows up as a
setting quietly not applying rather than as a failure.

**The fix.** Edit the line the diagnostic names.
[CONFIG.md](CONFIG.md#validation-rules) is every rule the parser applies.

---

## The server refused the sync

**Symptom.** The console reaches RomM, and the sync fails the same way every
time no matter how long you wait — or the status screen says **Sync disabled on
server**.

**Log line.**

```
3 warn sync.refused GET /api/roms: the rom index is not the shape this client reads: at offset 8: expected ':' after an object key
7 warn sync.refused no such device: the device was deleted in RomM's web UI
7 warn sync.refused sync_disabled: sync is turned off for this device in RomM; turn it back on there
7 info sync.tick outcome=server_disabled completed=0 failed=0 states_completed=0 states_failed=0 baseline=not stored
```

The first of those is a truncated or non-RomM body, and it is the one that reads
like a bug in this client and is not: the offset is where the parse gave up on
whatever answered.

**What is happening.** The server answered, and the answer will not be different
next time — which is why this is not `net.offline`. Three real causes:

1. **The device was deleted in RomM's web UI.** The token still works; the
   device it was bound to is gone.
2. **Sync is switched off for this device**, in RomM, by you. This one is not
   a failure, and the client does not treat it as one: the status screen says
   **Sync disabled on server**, the tick ends `outcome=server_disabled`, save
   states are left alone too, and the console keeps asking once per interval
   rather than backing off — so it starts syncing again within one interval of
   the switch going back on.
3. **The answer was not the shape this client reads.** A truncated body, a
   reverse proxy's error page, or a login portal's HTML where a rom index was
   expected — the URL reaches *something*, and that something is not RomM.

**The fix.** For 1 and 2, open RomM's device list and either re-enable sync for
this console or pair it again. For 3, open `[server] url` in a browser and check
that RomM itself answers there rather than a proxy, a portal or a redirect.

---

## RomM is too old

**Symptom.** The overlay's status screen says **RomM is too old to sync with**,
its `RomM` row names the version with "(too old)" after it, and nothing syncs.

**Log line.** One on every sync attempt, until the server is upgraded:

```
4 error server.unsupported RomM 5.2.0 is older than 5.4.0, the oldest this build syncs with; nothing is synced until it is upgraded
5 info sync.tick outcome=unsupported completed=0 failed=0 states_completed=0 states_failed=0 baseline=not stored
```

**What is happening.** Before a sync asks the server anything, `sys-rommsync`
reads which RomM release it is — RomM says so, unauthenticated, at
`/api/heartbeat`. Below **5.4.0** it stops there: no save is offered to the
server and none is downloaded from it. That is deliberate, not a missing
feature. A RomM before 5.4 cannot tell this console that a save was deleted on
the server, cannot keep two devices' saves of one game apart, and cannot record
which version each device last synced — so syncing with one is how a save from
another device gets silently replaced. Downloading roms is not affected.

**The fix.** Upgrade RomM to 5.4.0 or later. Nothing needs doing on the console:
it asks again on every sync attempt while the server is too old, so the next
sync after the upgrade runs normally — no reboot and no re-pairing — and says
which release it found:

```
9 info server.version RomM 5.4.0
```

The first
start of RomM 5.4 after an upgrade runs its database migrations, which RomM's
release notes put at 15–20 minutes on a large library; the console reads that
time as the server being unreachable and retries.

---

## A queued rom never arrives

**Symptom.** You queued a rom from the overlay's library screen, the queue
screen shows it waiting, and it never turns into a file.

**Log line.**

```
11 info download.drain drain: 1 downloaded, 0 skipped, 0 failed
11 warn download.drain drain ended retryable: GET /api/roms/42 answered 500
11 info download.drain the drain was stopped; the queue is untouched
```

**What is happening.** The sysmodule drains the queue on the same thread that
runs the sync tick, and it does it **last** — a sync that is due and a list page
the overlay is waiting for both go first. So a rom that is not moving is usually
one of seven things, and the queue screen names which:

1. **`[downloads] enabled = false`.** Nothing is opened and nothing is lost;
   switching it back on resumes exactly what was there.
2. **This console is not paired, has no `[server] url`, or its token was
   revoked.** Downloads need the same credentials a sync does, and a revoked one
   is `auth.rejected` rather than a line here.
3. **The entry was set aside.** One rom whose own endpoint keeps answering 500
   does not hold up the rest of the queue: it stays queued, its attempt count
   goes up, and the drain tries again on a backoff that starts at 30 seconds and
   doubles to 15 minutes. That is the `retryable` line above.
4. **It is a disc set.** A multi-file rom is skipped deliberately and never
   downloaded as a zip — the queue screen says so on the row.
5. **Something asked for the thread.** Pressing **Sync now**, taking the rom out
   of the queue, or terminating the sysmodule all stop the transfer where it
   stands. The bytes already on the card are kept as a `.part`, so the next
   attempt resumes rather than starting over — that is the `stopped` line.

6. **The card cannot hold it.** A rom bigger than the space left on the card,
   or over **4 GiB** -- the largest file FAT32 holds, and the card is held to
   that limit unless the console can tell it is not FAT32, which it usually
   cannot -- fails before it starts, and the row says which. A card that refused
   a write part way through says "the card refused the write"; that one is worth
   checking the card for. All three are failed rather than retried, because
   trying again changes nothing until you do: free space (or check the card) and
   queue the rom again. This version does not split a file over 4 GiB into the
   pieces Horizon can store; a rom that size has to reach the card some other way.
7. **RomM describes the rom at more length than the console reads.** Before a
   download the sysmodule asks RomM about the rom, and reads at most **200 KiB**
   of answer. RomM's answer carries every user's saves, states, screenshots and
   notes for that rom, and its RetroAchievements, so it grows as the rom is
   used. Past that the row says "the server's description of this rom is larger
   than this console reads" and the rom is failed rather than retried, because
   the same answer comes back. This version has no way past it; deleting old
   saves, states and screenshots of that rom in RomM's web UI shrinks the answer.

**The fix.** Read the row on the overlay's queue screen first; it carries the
reason for that rom. For 3, the drain is already retrying and nothing needs
doing. For 1 and 2, the settings and pairing screens are where both are fixed.

**Queued from RomM's web UI instead.** "Install on device" in RomM 5.4 puts the
rom in this same queue, so everything above applies once it is there. Getting
it there takes a sync tick: the console asks RomM for new requests once a tick
rather than holding a connection open, so a rom queued in the browser starts at
the next scheduled sync, or at once on **Sync now**. With sync switched off, no
tick runs and nothing is fetched. When the rom is done, failed or taken out of
the queue on the console, RomM is told and shows it.

```
12 info download.remote RomM's web UI can install roms on this console
13 info download.remote rom 42 queued from RomM's web UI
13 info download.remote rom 42 from RomM's web UI: reported done
```

The console is not offered in the browser at all while `[downloads] enabled =
false` (it says `RomM's web UI will not offer this console`, and any request it
had taken is reported failed so the browser is not left waiting), or when the
server has `DEVICE_INSTALL_ENABLED` off. A `warn download.remote` line is an
exchange that did not work; the next tick tries again, and sync carries on
either way.

---

## The console slept mid-sync

**Symptom.** One `power.sleep` line, usually beside a sync or a download that
stopped part way, and a queue entry that is still waiting afterwards.

```
14 warn power.sleep the console is sleeping and this client is still busy after 3000ms; the worker has not come back
```

**What it means, and why it is not a bug.** The console tells `sys-rommsync`
when it is about to sleep, and the client answers by stopping everything it is
doing to the SD card and to the network *before* it says "done" — a save write
in flight is finished or rolled back first, and a rom in flight is stopped with
its partial file kept. That answer has a deadline: a sysmodule that does not
answer in time is a console that will not go to sleep at all, and there is no
error message anywhere when that happens. So the client answers after three
seconds whatever state it is in, and writes this line to say that it did.

**What it costs.** Nothing that is not picked up again. A stopped sync runs at
the next interval; a stopped download resumes from the bytes already on the card
when the console wakes; a save is never left half written, because every write
is atomic and the backup is taken first
([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#backups)).

**When to report it.** If you see this line *often* — more than the odd one — or
if a queued rom never finishes across several sleeps, attach the log
([What to attach to a bug report](#what-to-attach-to-a-bug-report)). A single
line after putting the console to sleep during a sync is the client working.

**Nothing here needs fixing on the console.** There is no setting for it, and
there is nothing to turn off.

---

## Nothing syncs while a game is open

**Symptom.** You pressed "Sync now" from the overlay during a game and it said
*Syncs when the game closes*, or the log shows a sync waiting and nothing after it:

```
21 info sync.held a game is running; the interval tick waits until it closes
```

**What it means, and why it is not a bug.** While a game — a retail game, Tico,
or RetroArch launched over a title — is open, `sys-rommsync` downloads, places
and uploads no save. A game keeps its save in memory and writes it when it
closes, so a newer save downloaded under it would be overwritten with the old
one the moment you quit, and a save uploaded under it would be the one from
before you started playing. Tico's library screen counts as a game too: it is
the same application its emulators run in.

**What happens next.** One sync runs within a few seconds of the game closing,
however many fell due while it was open, and ends with the usual `sync.tick`
line. A "Sync now" pressed during the game is that same sync. A game started
while a sync was already moving saves leaves that sync to finish its saves,
and its save states wait instead —
`sync.held a game started during this sync; its save states wait until it closes`,
and the status screen says *partial* — for the sync that runs once the game
closes. Downloads from
the queue are not held — a rom is not a save — so a rom queued from the overlay
mid-game still comes down.

**The play time it records.** The sync after the game names what you played,
one line per save that changed, with the application the console saw in the
foreground when the save was written — its title id:

```
22 info play.recorded rom 12 slot autosave: played up to 31 min, title 0100000000ABC000
```

"Up to", because the console knows when it last looked and when the save was
written, not when you started. `title unknown` is not a fault: the console only
looks while a sync is waiting for a game, so a game played entirely between two
syncs is never seen, and neither is a save written just as a second application
came up. The title is the *application's* — a Tico module, or for RetroArch
launched over a game, that game's — and the rom is the one the save belongs to.
RomM has nowhere to keep the title, so the log is the only place it appears.

**When to report it.** If a `sync.held` line is never followed by a `sync.tick`
after you have closed the game and gone back to the HOME Menu, attach the log
([What to attach to a bug report](#what-to-attach-to-a-bug-report)). RetroArch
started from the album rather than over a title is not seen as a game, which is
a known gap rather than this failing.

---

## A save disappeared from the card

**Symptom.** A save that was on the console is gone after a sync, or a save you
deleted in RomM is still on the console.

**Log line.**

```
22 info save.deleted delete rom 4, slot autosave: deleted on the server; removed /tico/saves/nes/nova.srm after backing it up to /config/rommsync/.backup/4-autosave-1757000901.srm
22 info save.deleted delete rom 4, slot autosave: deleted on the server; /tico/saves/nes/nova.srm stays on the card ([sync] on_server_delete = keep), and these bytes are not offered to RomM again
```

**What is happening.** The save was deleted in RomM — in its web UI, or by
another device; RetroArch deleting a `.srm` removes every version RomM holds for
it — and RomM 5.4 told this console not to upload its copy again. With
`[sync] on_server_delete = remove`, the default, the file is copied under
`/config/rommsync/.backup/` **first** and only then removed: the server is the
source of truth, and the backup is what makes obeying it safe. With `keep`, the
file stays exactly as it was and is not offered to RomM again while it holds the
bytes RomM deleted — one `save.deleted` line, then silence; play it and the new
progress syncs as usual ([CONFIG.md](CONFIG.md#on_server_delete-a-save-deleted-in-romm)).

**Fix.** To have the save back, open the overlay's **Conflicts** screen: a
removed save is listed as *deleted on RomM*, and restoring it puts the file back
where it was. The next sync then offers it to RomM as new progress. If a save
you did not delete disappeared, check RomM's own history for who deleted it; the
backup is on the card either way.

**When to report it.** A save gone from the card with no `save.deleted` line
for it, or a `save.deleted ... removed` line whose backup is not under
`/config/rommsync/.backup/`.

---

## Another device's save got there first (409)

**Symptom.** A save comes back to the console as a different version than the
one you played, or a conflict appears on the **Conflicts** screen with *Slot has
a newer save since your last sync* as what the server said — or one sync ends
partly finished with a line like:

```
14 error save.failed upload rom 4, slot autosave: the upload was refused: HTTP 409; the slot holds a newer save than this device last synced, so nothing was uploaded over it
15 info sync.tick outcome=partial completed=2 failed=1 states_completed=0 states_failed=0 baseline=stored
```

**What is happening.** Another device — RetroArch's Cloud Sync, RomM's web
player, another Switch — saved this game to RomM since this console last synced
it. RomM refuses to let the console's upload replace that save (HTTP **409**),
and that refusal is what stops a save made elsewhere from being silently
overwritten. The same sync then asks RomM about that save once more
([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#the-slot-guard-and-the-409-232)):

- if the card's copy has not changed since this console last synced it, the
  newer save is downloaded over it, after a backup;
- if it has, both changed, and it is a **conflict**: the server's copy goes on
  the card, and the card's bytes are kept under `/config/rommsync/.backup/` and
  on RomM as an extra save named `<game> [switch-conflict <time>]` plus the
  save's extension
  ([INTEROP.md](INTEROP.md#when-two-devices-disagree)).

Either way the line above does not stay. It is left — and the sync reads partly
finished — only when that second question could not settle it:

- a game was open, and this client touches no save while one is
  ([Nothing syncs while a game is open](#nothing-syncs-while-a-game-is-open));
- the server did not answer it;
- more than four uploads were refused at once: four are settled at a time
  (`sync::kMaxResolvedUploads`, per round of a sync), and the rest keep the line
  until a later sync reaches them;
- or the save went from the server between the two questions, so the answer
  named nothing to compare.

The next sync settles it.

**The fix.** Usually nothing. To keep the card's version after a conflict,
restore it from the **Conflicts** screen; the next sync decides again, and will
most likely offer it to RomM as new progress. If the same 409 line comes back on every sync with no game open,
attach the log ([What to attach to a bug report](#what-to-attach-to-a-bug-report)).

---

## Tico saves from the wrong account, or none

**Symptom.** RetroArch's saves sync and Tico's never do, or the Tico saves that
sync are another Switch user's.

**Log line.**

```
3 warn config.diagnostic [tico] account: Tico keeps saves for 2 accounts in /tico/saves/users and [tico] account does not say whose to sync, so none of them is synced -- set it to one of 0123456789abcdef0123456789abcdef, fedcba9876543210fedcba9876543210
6 warn scan.skipped tico account /tico/saves/users: Tico keeps saves for 2 accounts in /tico/saves/users and [tico] account does not say whose to sync, so none of them is synced
3 warn config.diagnostic [tico] account: names an account with no folder in /tico/saves/users, so no Tico save there is synced -- the accounts there are 0123456789abcdef0123456789abcdef
3 info config.diagnostic [tico] account: Tico keeps saves per account in /tico/saves/users; the only one there, 0123456789abcdef0123456789abcdef, is synced
6 warn scan.skipped outside layout /tico/saves/snes/Game.srm: Tico 0.8 keeps saves in /tico/saves/users/0123456789abcdef0123456789abcdef/snes now and does not read this one; a module installed before 0.8.0 writes here until it is reinstalled
```

The `config.diagnostic` lines are also on the overlay's settings screen, and are
written to the log when they change rather than on every sync.

**What is happening.** Tico 0.8 keeps each Switch user's saves apart, in
`/tico/saves/users/<account>/` ([CONFIG.md](CONFIG.md#tico-per-account-saves)).
With one account there the client syncs it, and says whose. With **several**
it syncs none of them: it cannot tell which are yours, and guessing would sync
one user's save over another's through the server. An `account` naming a folder
that is not there syncs nothing from that root. And a file still in the folder
from before 0.8 is *outside layout*: Tico 0.8 does not read it, though a module
installed before 0.8.0 keeps writing there until it is reinstalled.

"The wrong account" is usually the first case's cousin: the only account with a
folder is not the one you play as — you have never saved under yours yet — so
that one is synced.

**The fix.** Name the account in `config.ini`, using an id from the settings
screen's list:

```ini
[tico]
account = 0123456789abcdef0123456789abcdef
```

`account = legacy` reads the layout from before 0.8 even when `users/` exists.
For *outside layout* files, reinstall the Tico module from Tico 0.8 so it writes
the new layout, and move a save you still need into your account's folder.

---

## RomM calls a platform something else

**Symptom.** One system's roms never download — the queue screen says no folder
is mapped for its platform — and its saves never sync, while every other system
works.

**Log line.**

```
6 warn scan.skipped unmatched /tico/saves/genesis/Sonic the Hedgehog (USA, Europe).srm: no rom named "Sonic the Hedgehog (USA, Europe)" on platform genesis
1 info config.diagnostic line 14 [platform.megadrive] is not a platform this build maps by default; it is used as written -- check it matches the folder name under RomM's library/roms/, because a slug that matches nothing maps nothing
```

**What is happening.** The folder map is keyed by RomM's `platform_fs_slug`:
the name of the platform's folder under RomM's `library/roms/`
([CONFIG.md](CONFIG.md#defaults--the-folder-map)). The built-in map knows the
usual names — both spellings where two are common (`dc` and `dreamcast`,
`arcade` and `fbneo`), and Tico's own folder name where it differs (`sms` is
Tico's `master-system`). A library whose folder has another name — `megadrive`
for Genesis, say — gets none of that: its roms have nowhere to land, and a save
in Tico's `genesis` folder is looked for among `genesis` roms, of which RomM has
none. That is the first line. The second is what a `[platform.…]` section for a
name this build does not know says: a reminder, not a fault — check the name
against RomM's folder, because a slug that matches nothing maps nothing.

**The fix.** A section under RomM's name, pointing at Tico's folders and saying
which emulator writes them and plays them:

```ini
[platform.megadrive]
roms      = /tico/roms/genesis
saves     = /tico/saves/genesis
states    = /tico/states/genesis
emulator  = genesis_plus_gx
play_with = tico
```

`emulator` and `play_with` are needed because both defaults are keyed by the
spelling this build knows: without the first, the saves go to RomM with no
emulator and meet nobody else's
([INTEROP.md](INTEROP.md#which-systems-sync-with-what) has the ids); without the
second, a save the console has never had is not placed. The name is
case-sensitive, because it is a folder name on the server.

---

## Some saves are left out: the limits

**Symptom.** Most saves sync and a few never do, or a game you have saves for
on the server never gets one on the console. The status screen may say the last
sync **partly finished** every time.

**Log line.**

```
7 warn scan.skipped too many saves /tico/saves/snes/Zelda (USA).srm: the scan stopped at 128 saves; the rest of this card was not read
7 warn scan.skipped unmatched /tico/saves/gba/Golden Sun (USA).srm: no rom named "Golden Sun (USA)" on platform gba; the rom index is incomplete, so this may be a rom that was never read rather than one you do not have
9 warn sync.refused Tetris (World) (rom 812): the server holds more saves for it than the 128 one plan can carry, so it was left out of this sync
```

**What is happening.** The sysmodule runs in a fixed, small amount of memory,
so a sync holds a bounded amount of everything
([INTEROP.md](INTEROP.md#known-limits)):

- **128 saves per console.** The scan reads RetroArch's folders first, then the
  folder map, each folder's files in name order, and stops at the 128th save.
  The rest of the card is not read that sync — nothing of it is uploaded, and
  nothing is written over it — and the status screen reads partly finished.
- **32 states**, when states are on, sharing those records with the saves.
- **1,024 roms** in the library index a sync holds, the ones with the lowest
  RomM ids. A save of any other game is *unmatched*, with the index named as
  the likely reason.
- **A game with more than 128 saves on the server** — one plan's worth — is
  left out of the sync whole: none of its saves is offered and nothing of it is
  touched. The `sync.refused` line names it on every sync, and the sync reads
  partly finished.

**The fix.** Fewer saves in the folders the client reads: move old saves of
games you no longer play out of them, or switch a platform's saves off in the
folder map ([CONFIG.md](CONFIG.md#a-platform-section-replaces-that-platforms-defaults)).
For a game with too many saves on the server, delete old versions in RomM's web
UI. For a library past the index, nothing on the console helps yet; the roms
with the lowest ids are the ones matched.

---

## The sysmodule stopped

**Symptom.** The overlay says `sys-rommsync` is not running, and the last line
of the log is this one:

```
812 error process.terminated an uncaught St9bad_alloc: the heap is spent, and the sysmodule stopped
```

**What it means.** The sysmodule ran out of memory — it lives in a fixed heap
of about 1.5 MiB — and stopped. Something the standard library refused ends
the process the same way, named by its type instead (`St12length_error`). Either
way it is a bug in the client, not in your setup: the heap is sized for the
largest library and server answers the code accepts, and a stop means one of
those bounds is wrong.

**What it costs.** Nothing that is not picked up again. Every save is backed
up before it is overwritten and every write is atomic, so a stop part way
through a sync leaves each save as it was or as the server had it, with a
backup of the old copy under `config/rommsync/.backup/`
([SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#backups)). A rom part way down is resumed
or started again. A test runs a sync and a download with the memory running
out at every single point they allocate, and holds each one to exactly this.

**What to do.** Reboot the console to start the sysmodule again, and report it
with the log attached ([What to attach to a bug report](#what-to-attach-to-a-bug-report)):
the lines before this one say what it was doing. If it happens on every boot,
move `config/rommsync/queue.json` out of the way first — a queue is the one
thing a boot reads that grows with what you asked for.

---

## What to attach to a bug report

Five things, and the log has four of them:

1. **The version.** The first line of the log, and also on the overlay's status
   screen:

   ```
   1 info boot rommsync-nx/0.1.0
   ```

2. **The pairing line.** Which server, which device, which scopes — and *not*
   the token, which is reduced to its length before it is ever written:

   ```
   2 info auth.token server=https://romm.example.com device=console-1 scopes=[roms.read] credential=present(68 chars) expires=never
   ```

   The field is `credential=`, not `token=`, and only ever says *absent* or how
   many characters long the token is. It is named that way on purpose: a field
   called `token` is one the redactor blanks, and blanking it would lose the one
   thing this line is for — whether this console has credentials at all.

3. **The server's version**, written once a boot and again after a pairing —
   the first question about any sync problem is which RomM answered it. It is
   also the `RomM` row on the overlay's status screen:

   ```
   5 info server.version RomM 5.4.0
   ```

4. **The last tick.** What the sync did, or did not:

   ```
   14 info sync.tick outcome=partial completed=3 failed=1 states_completed=0 states_failed=0 baseline=stored
   ```

5. **The log tail.** The last twenty or thirty lines of
   `sdmc:/config/rommsync/rommsync.log`, and `rommsync.log.old` beside it if the
   failure is older than the current file.

**If the log's first lines are warnings**, paste those too, and paste them
first. `sys-rommsync` writes down anything that did not come up while it was
starting and repeats it at the top of the file as soon as there is a file:

```
2 warn boot rommsync: ssl never registered after 10000ms
```

That is a system service the sysmodule waited for and did not get. It keeps
running without it — the overlay opens, settings read, the queue draws — and the
line says which one, which is the whole reason it is there.

**If the overlay says the sysmodule is not running at all**, there is a fifth
thing: `sdmc:/atmosphere/crash_reports/`. The newest file in it is this
process's, and it is written by Atmosphere rather than by us, so it exists even
when nothing of ours got far enough to log. An empty directory is itself an
answer: the module was never launched, which is an install problem
([INSTALL.md](INSTALL.md)) rather than a runtime one.

Paste them as they are. Nothing in the log is a credential — see [Reading the
log](#reading-the-log) — so it does not need editing first, and editing it is
how the line that explains the failure gets lost.

Do **not** attach `token.dat`, `device.dat`, or a screenshot of the pairing
screen while a code is on it.

---

## What this page cannot tell you yet

Hard rule 1 means no part of this has been followed on a console. What is
checked on every build is narrower and worth knowing:

- Every event tag in the table above is one the code can actually write, and
  every tag the code can write is in that table — a test reads both.
- The offline, 401, TLS and truncated-body sections match what a real RomM
  produces when the test rig's fault proxy damages a real response, rather than
  what a mock would.
- The log is bounded at two files and holds no bearer token, no `device_code`
  and no URL credential.

What is **not** checked, and is marked in place above: anything that is a
property of the console's own TLS stack, and the shipped folder map's guess at
where each emulator keeps its saves. **M8-2** is the issue that walks this page
on hardware and corrects it.
