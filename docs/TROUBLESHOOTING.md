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
| `scan.skipped` | a save file was not matched to a rom | [Saves are skipped: nothing matches](#saves-are-skipped-nothing-matches) |
| `sync.refused` | the server answered, and the answer will not change | [The server refused the sync](#the-server-refused-the-sync) |
| `save.failed` | a save could not be written — usually a full card | [The SD card is full](#the-sd-card-is-full) |
| `sync.tick` | how one sync ended — every tick writes one | [What to attach to a bug report](#what-to-attach-to-a-bug-report) |

The overlay does not show the log yet. `sys-rommsync` serves it over IPC —
`GetLog`, command 16 in [DEVELOPMENT.md](DEVELOPMENT.md#the-command-set) — so a
screen can be added without an SD reader, and until one is, the card is how you
read it.

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
everything over the server's copies. RomM 5.2.0 issues tokens with no expiry to
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
5 warn scan.skipped unmatched /retroarch/saves/Game (USA).srm: no rom is named "Game (USA)"
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
time no matter how long you wait.

**Log line.**

```
3 warn sync.refused GET /api/roms: the rom index is not the shape this client reads: at offset 8: expected ':' after an object key
7 warn sync.refused no such device: the device was deleted in RomM's web UI
7 warn sync.refused sync disabled: sync is disabled for this device
```

The first of those is a truncated or non-RomM body, and it is the one that reads
like a bug in this client and is not: the offset is where the parse gave up on
whatever answered.

**What is happening.** The server answered, and the answer will not be different
next time — which is why this is not `net.offline`. Three real causes:

1. **The device was deleted in RomM's web UI.** The token still works; the
   device it was bound to is gone.
2. **Sync is switched off for this device**, in RomM, by you.
3. **The answer was not the shape this client reads.** A truncated body, a
   reverse proxy's error page, or a login portal's HTML where a rom index was
   expected — the URL reaches *something*, and that something is not RomM.

**The fix.** For 1 and 2, open RomM's device list and either re-enable sync for
this console or pair it again. For 3, open `[server] url` in a browser and check
that RomM itself answers there rather than a proxy, a portal or a redirect.

---

## What to attach to a bug report

Four things, and the log has three of them:

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

3. **The last tick.** What the sync did, or did not:

   ```
   14 info sync.tick outcome=partial completed=3 failed=1 states_completed=0 states_failed=0 baseline=stored
   ```

4. **The log tail.** The last twenty or thirty lines of
   `sdmc:/config/rommsync/rommsync.log`, and `rommsync.log.old` beside it if the
   failure is older than the current file.

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
