# The hardware test plan

The one script for every check that only a console can answer. Whoever holds a
console runs these scenarios **in the order written**, and records each run in
[HARDWARE_RESULTS.md](HARDWARE_RESULTS.md) in the same shape, so that runs on
different consoles compare and the launch gate (M12-0, #305) can cite them by id.

> **Nothing on this page may be run before `./scripts/v1-gate.sh` exits 0.**
> That is CLAUDE.md's hard rule 1. Until then the page is an agenda, written and
> tested off-console ([TESTING.md](TESTING.md#rung-3--the-v1-gate-and-real-hardware-m8)).
> Never against a production RomM or a real library: a **disposable** collection
> on a RomM that is not the one you use, a **spare SD card or emuMMC**, and a
> NAND backup that is not on the console.

`ctest -R docs.hardware` holds this page to the code: every scenario id is
unique and cited, every quoted log line has a tag the sysmodule writes, and
every path, title id and result code below is the tree's.

## How to read a scenario

Each scenario is a `###` heading with a **stable id** (`AREA-NN`). Ids are
never renumbered or reused: a scenario that stops applying is marked
*retired* in its heading and keeps its id, because results already recorded
cite it. A new scenario takes the next free number in its area.

Each has:

- **Steps.** What to do, exactly.
- **Expect.** What the console should show and log. Log lines are quoted in the
  shape the log writes them (`<seq> <level> <tag> <detail>`). The tag is fixed
  by the code; the detail after it is illustrative. **Record the line the
  console actually wrote**, verbatim -- nothing pins the detail but this run.
- **Pass.** The criterion. Anything else is a FAIL, recorded with its log line.
- **Answers.** Where present: the open question this run settles, and the doc
  or issue its answer is written into.

The log is `sdmc:/config/rommsync/rommsync.log` (`/config/rommsync/rommsync.log`
on the card, 32 KiB plus one `.old`), readable on the console under Settings ->
**Log**. Backups are under `/config/rommsync/.backup/`. The sysmodule's title id
is `4200000000524D53`.

## Scenario ids

| Area | What it covers |
|---|---|
| `PRE` | Before anything is installed: media, the selftest NRO, identifying the files |
| `INST` | Install, enable, upgrade, uninstall, the ovl-sysmodules toggle |
| `BOOT` | Cold boot with Wi-Fi, without it, and with the server down |
| `NET` | Network loss, DNS, captive portals, certificates, throughput |
| `LIFE` | Sleep, dock, overlay cycling, games launched over a sync |
| `SAVE` | Save round trips, conflicts, power cuts, a full card, emulator layouts |
| `MEM` | The co-resident sysmodule set, applets, the overlay's heap |
| `CLK` | A wrong, reset, or non-UTC console clock |
| `OVL` | The overlay's own script, and the diagnostics bundle |
| `DBG` | Collecting what a failure leaves behind |

---

## PRE -- before anything is installed

### PRE-01: Media, backups and a disposable server

**Steps.** Confirm `./scripts/v1-gate.sh` exits 0 on the commit under test.
Take a NAND backup and copy it off the console. Use a spare SD card or an
emuMMC, never the daily one. Stand up a RomM that is not your own, with a
disposable collection holding only homebrew roms and throwaway saves.

**Pass.** All four are true and written in the run's console table. A run
without them is not a run of this plan.

### PRE-02: The selftest NRO first, no sysmodule yet

**Steps.** Before any sysmodule is installed, copy `rommsync-selftest.nro`
(switch-build's artifact) to `/switch/` and write
`/config/rommsync-selftest/selftest.ini` as [selftest/README.md](../selftest/README.md)
says, naming the disposable RomM. Launch it from hbmenu, approve its device code
in RomM, and toggle airplane mode when it asks. Copy
`/config/rommsync-selftest/report-<ts>.json` off the card into
`docs/selftest/` and run `scripts/selftest-judge.py` on it.

**Pass.** The judge prints `VERDICT PASS` for the report. The launch gate's
`selftest` row wants two such reports from two HOS/Atmosphere pairs:
`scripts/selftest-judge.py --pairs 2 docs/selftest/report-*.json`.

**Answers.** The three `ssl` questions in
[DEVELOPMENT.md](DEVELOPMENT.md#m0-1-the-measurement-and-the-decision)
(`sslConnectionPoll` and `sslConnectionSetIoTimeout` are the report's
*net.timeout* row when `selftest.ini` names a stall listener, who owns the
descriptor on `ENOENT` its *net.handshake_leak*), and the `st_mtime` question
CLK-04 also asks (its *platform.mtime_utc* row).

### PRE-03: The files on the card are the build under test

**Steps.** After unzipping the release, copy `exefs.nsp` and `ovl-rommsync.ovl`
back off the card and run `scripts/artifact-check.py version <file>` on each.

**Pass.** Both print the `VERSION` of the release under test; record it, with
the commit sha, in the console table.

---

## INST -- installing, enabling, upgrading, removing

### INST-01: Fresh install leaves the sysmodule off

**Steps.** Unzip the release onto the SD root ([INSTALL.md](INSTALL.md#1-unpack-the-zip)).
Reboot. Open Ultrahand.

**Expect.** Ultrahand lists `ovl-rommsync` with its name and `VERSION`, not
greyed out (the `LNY2` stamp, #211). `sys-rommsync` is **not** running after the
reboot: `atmosphere/contents/4200000000524D53/flags/` exists and is empty. The
overlay says *sys-rommsync is not running* (overlay/README.md step 2).

**Pass.** All three. A sysmodule that starts on the first boot is a FAIL: the
first enable has to be deliberate.

**Answers.** The Atmosphère and HOS versions this boots on are the first real
compatibility line (#44, #310).

### INST-02: The first enable is deliberate

**Steps.** In ovl-sysmodules, press **A** on `sys-rommsync` (start now), then
**Y** (boot toggle). Reboot.

**Expect.** **A** starts it without a reboot (the *Dynamic* section); **Y**
writes `boot2.flag` inside the shipped `flags/`; after the reboot it is running
and the log opens with a `boot` line:

```
1 info boot rommsync-nx/1.0.0-rc2
```

**Pass.** Started on **A**, `boot2.flag` written by **Y** and by nothing else,
running after reboot, and the boot log has no `rommsync: no pm:dmnt` and no
`rommsync: NetworkInitialize` line.

### INST-03: Upgrade from the previous release

**Steps.** With the previous release (v1.0.0-rc2, for the first stable) installed, paired, and holding at least one
synced save, a queued download and a Conflicts entry, unzip the build under test
over it ([INSTALL.md](INSTALL.md#6-upgrading)). Reboot. Repeat once from
v1.0.0-rc1 if a card with it is to hand.

**Expect.** `config.ini`, `token.dat`, `device.dat`, `state.db`, `queue.json`,
`conflicts.db`, `.backup/` and the log all survive; `boot2.flag` survives. The
first sync after it uploads nothing already in sync. From rc1: each save is
offered to the shared `autosave` slot, a newer server copy coming down with the
card's backed up first.

**Pass.** Still paired, no new device row in RomM, nothing re-uploaded, the
queued rom finishes, and every save that changed has a backup.

### INST-04: Uninstall

**Steps.** Turn `sys-rommsync` off in ovl-sysmodules, delete
`atmosphere/contents/4200000000524D53/` and `switch/.overlays/ovl-rommsync.ovl`
([INSTALL.md](INSTALL.md#7-uninstalling-and-un-pairing)). Reboot. Revoke the
token in RomM.

**Pass.** No `sys-rommsync` entry left in ovl-sysmodules, no overlay in
Ultrahand, `config/rommsync/.backup/` untouched, and after the revoke a
re-install fails to sync with `auth.rejected` rather than syncing.

### INST-05: ovl-sysmodules stops it mid-sync

**Steps.** Start a sync with a download running, then press **A** on
`sys-rommsync` in ovl-sysmodules to stop it. Start it again.

**Expect.** The overlay says *sys-rommsync is not running* while it is stopped.
After the restart the download resumes from its `.part`; no save is half
written.

**Pass.** The console does not fatal, the restart succeeds without a reboot,
and SAVE-05's judgement holds for any save in flight.

### INST-06: Overlay and sysmodule from different releases

**Steps.** Install an `.ovl` and an `.nsp` built from different `ipc::kVersion`s
(overlay/README.md step 5).

**Pass.** *sysmodule unreachable* with *Update the sysmodule* naming both
numbers; no garbage rows, no crash.

---

## BOOT -- cold boots

### BOOT-01: Cold boot with Wi-Fi

**Steps.** Power off fully, power on with Wi-Fi, a paired console and auto-sync
on. Time from the Atmosphère splash to the home menu against a boot with
`sys-rommsync` off.

**Expect.**

```
1 info boot rommsync-nx/1.0.0-rc2
2 info boot rommsync: card format unknown (this console reads exFAT, so it could be either); a file on it stops at 4 GiB
9 info server.version RomM 5.4.0
12 info sync.tick outcome=completed completed=0 failed=0 states_completed=0 states_failed=0 baseline=stored
```

**Pass.** No measurable boot delay, a `sync.tick` within one interval, the
server version shown on the overlay (#228), and no `boot` warning.

### BOOT-02: Cold boot without Wi-Fi

**Steps.** Airplane mode on, power on.

**Expect.**

```
2 warn net.offline the console has no internet connection; the interval tick waits for the network
```

**Pass.** No boot delay, no fatal, the overlay answers. Turn Wi-Fi on: a tick
runs within one interval, with no reboot.

### BOOT-03: Cold boot with the server down

**Steps.** Stop the disposable RomM. Power on with Wi-Fi.

**Expect.**

```
3 warn net.offline GET /api/roms: connection failed; the rom index could not be fetched: connection failed (Connection refused)
4 info sync.tick outcome=offline completed=0 failed=0 states_completed=0 states_failed=0 baseline=not stored
```

**Pass.** No boot delay, no fatal, ticks retry on the interval; start RomM and
the next tick completes.

---

## NET -- the network going wrong

### NET-01: Airplane mode mid-download

**Steps.** Queue a rom of at least 100 MB. At about half, turn airplane mode on;
after a minute, off.

**Expect.**

```
11 warn download.drain drain ended retryable: GET /api/roms/42: connection failed
```

**Pass.** The download resumes from its `.part` (RomM's access log shows a
`Range` request), finishes, and its hash verifies. No fatal.

### NET-02: Wi-Fi drops and reconnects mid-sync

**Steps.** During a tick with several saves, walk out of range (or switch the
access point off) for 30 s, then back.

**Pass.** The tick ends `offline` or `partial`, never with a half-written save;
the next tick after reconnecting completes everything.

### NET-03: DNS fails

**Steps.** Redirect the server's hostname to an unroutable address with
Atmosphère's `dns.mitm` (`/atmosphere/hosts/default.txt`, e.g.
`romm.example.test 0.0.0.0`), reboot, and let a tick run. Then point the name
at a resolver that never answers, if one is to hand.

**Expect.** A `net.offline` line naming the request; record its detail.

**Pass.** Each tick ends `offline` within the request timeout, no fatal, and
the overlay keeps answering.

### NET-04: A captive portal

**Steps.** Join a network with a captive portal (a hotel or a guest network)
without signing in. Let a tick fall due, then sign in.

**Expect.** nifm reports no internet, and the tick waits; after two minutes
RomM's heartbeat is asked directly, and the portal's answer is not one (#328):

```
2 warn net.offline nifm says the console has no internet connection; the interval tick waits for the network, and asks RomM directly after 120 s
3 warn net.offline nifm says the console has no internet connection, and RomM did not answer either: the heartbeat was refused with status 302
```

**Pass.** No request but `GET /api/heartbeat` sent to the portal, no
`auth.rejected` from the portal's page; after sign-in a tick completes.

### NET-05: Certificate variants

**Steps.** The cells of the server matrix (M12-5, #311): Let's Encrypt RSA and
ECDSA, a private CA and a self-signed certificate via `[server] ca_file`
([INSTALL.md](INSTALL.md#self-signed-or-private-ca)), an IP literal over
`https://`, a `.local` name, plain `http://` on the LAN, tunnels. One tick per
cell.

**Expect.** A working cell syncs; a refused one logs `net.tls`:

```
2 error net.tls GET /api/roms: TLS failure; the rom index could not be fetched: TLS failure (error:1404B42E:SSL routines:ST_CONNECT:tlsv1 alert protocol version)
```

**Pass.** Every cell is recorded with its exact line in #311's table on
[HARDWARE_RESULTS.md](HARDWARE_RESULTS.md). Verification is never turned off
to make a cell pass.

### NET-06: Every TLS handshake completes

**Steps.** Against an `https://` server, run 50 ticks (a short `[sync]
interval`, or **Sync now** repeatedly). Then put a TLS 1.3-only proxy in front
of it and run one more.

**Expect.** The handshake is non-blocking (M11-6, #290): `ssl`'s
*ResultIoWouldBlock*, `0x1987B` (shown as `2123-0204`), means "try again".
If that value were wrong, **every** `https://` request would fail with a
`net.tls` line from the first tick on.

**Pass.** 50 of 50 handshakes succeed. The 1.3-only cell's line is recorded
as the answer to the TLS 1.2 ceiling (#44, `SslVersion_Auto`).

### NET-07: Download throughput

**Steps.** On the LAN, queue a rom of at least 100 MB and time enqueue to
*downloaded*. Then run `tlsprobe/` GETting the same file at `read_buf` 0x4000,
0x10000 and 0x20000, each at `tcp_rx_max` 0x10000 and 0x40000. Repeat the
sysmodule download over a real WAN round trip if one is to hand.

**Pass.** At least 1 MB/s on the LAN (#201's 64 KiB chunk). Record the MB/s
of every cell and the probe's *reads N, the largest M bytes*.

**Answers.** Whether the 64/64 KiB choice is the limit
([DEVELOPMENT.md](DEVELOPMENT.md#what-the-process-costs-the-pool)).

### NET-08: Sessions held against the system's limits

**Steps.** With the co-resident set of MEM-01 running, including any that hold
`bsd:u` (ldn_mitm, sys-ftpd), cold boot and let a tick and a download run.

**Pass.** No `rommsync: NetworkInitialize` line in the boot log, and `hid`
keeps working the whole time (#44's `sslInitialize` session-count question:
2 is safe).

**If it fails.** A `rommsync: NetworkInitialize` line is followed by `the
socket transport did not come up (0x…)`: the worker retries it with backoff
(#333). Record the code, then stop the sysmodule holding `bsd:u` (sys-ftpd is
the easiest) without rebooting: within five minutes the log says `the socket
transport came up on retry N` and the held tick runs.

### NET-09: A console that blocks Nintendo

**Steps.** Three cells, each a cold boot with RomM on the LAN by IP address and
a short `[sync] interval`: emuMMC with Atmosphère's default DNS block (no
hosts file of your own, `add_defaults_to_dns_hosts` left on), the same console
with 90DNS set as its DNS servers, and a network with no internet at all. In
each, note what System Settings -> Internet -> Connection Status says, and run
the selftest (PRE-02) for its nifm row.

**Expect.** Either nifm says Connected and the boot tick runs at once, or it
does not, and the tick runs once the boot wait's two minutes are out (#328):

```
2 info net.offline nifm says the console has no internet connection, but RomM answered; syncing without nifm
```

**Pass.** Saves sync in every cell; the second tick after a boot starts on time
rather than two minutes late.

**Answers.** What nifm reports with Nintendo blocked -- the selftest's nifm
row, its verdict (`online`, `no internet`, `unexpected 0x…`), `rc` and
`status`, in each cell -- which #328 could not know. The judge passes an unsure
verdict as `WARN` when the heartbeat row passes (#342). Written
into #328 and into the cell's row of #310's matrix.

---

## LIFE -- the console's lifecycle

### LIFE-01: Sleep mid-download, ×20

**Steps.** Queue a large rom. While it downloads, press the power button to
sleep; wake after ~10 s. Twenty times.

**Expect.** Mostly nothing; an occasional

```
14 warn power.sleep the console is sleeping and this client is still busy after 3000ms; the worker has not come back
```

**Pass.** The console sleeps every time (none stays awake), never fatals, and
the rom finishes after the twentieth wake with its hash verified. More than one
`power.sleep` line in twenty is a FAIL to report against #290.

### LIFE-02: Sleep mid-handshake, ×20

**Steps.** Point `[server] url` at an `https://` server that accepts the TCP
connection and never answers (e.g. `nc -lk 8443`). Press **Sync now**, and sleep
within a second. Twenty times.

**Pass.** Sleep is acknowledged within 3 s every time, with **no**
`power.sleep` line (M11-6, #290: the handshake is cancellable from the
quiescing thread).

### LIFE-03: Sleep mid-DNS, ×20

**Steps.** Make the server's name resolve through a slow or unreachable
resolver (NET-03's setup, with a resolver that never answers). **Sync now**,
then sleep at once. Twenty times.

**Pass.** Sleep acknowledged within 3 s every time, no `power.sleep` line
(`resolverGetCancelHandle` is taken before `getaddrinfo`, and `resolverCancel`
comes from the quiescing thread).

### LIFE-04: Dock and undock during a sync

**Steps.** Start a tick with several saves and a download, then dock, undock,
dock again within the tick.

**Pass.** The tick completes, the overlay is legible in both modes
(overlay/README.md step 6), no fatal.

### LIFE-05: Overlay open and close ×50 in a game

**Steps.** In a game, open and close the overlay fifty times, alternating
**B** at the root and launching another overlay from Ultrahand
(overlay/README.md step 14, M11-1 #285).

**Pass.** The fiftieth open looks like the first, and the game has its
controller back after every close.

### LIFE-06: Overlay open and close ×50 while a sync runs

**Steps.** **Sync now**, then cycle the overlay fifty times while the tick runs
(M11-7, #291).

**Expect.** No screen ever reads *sys-rommsync is not running*. The log has no
`ipc.error` line. If one appears:

```
640 error ipc.error svcReplyAndReceive failed: 2001-0114 (0xe401); dropped 1 session(s); 1 in a row -- the IPC service carries on
```

**Pass.** No `ipc.error` at all. Any that appears: record its result code in
the results table. The loop's handling of each `svcReplyAndReceive` return
(`sysmodule/source/ipc/wait_outcome.hpp`) rests on mesosphere's
`ReplyAndReceiveImpl` semantics (a failed reply returns index -1; a session
closing during the wait returns its own index), which only a console confirms.

### LIFE-07: Exit with Library open, ×4

**Steps.** overlay/README.md step 15: open Settings -> **Library**, leave with
Ultrahand's combo while it is on screen, four times, then open the overlay.

**Pass.** The status screen shows the sysmodule's state, not *sys-rommsync is
not running* (M11-1, #285).

### LIFE-08: A game launched while a sync runs

**Steps.** With auto-sync on: (1) start a large tick, then launch a game;
(2) in a game -- Tico's library screen counts -- press **A** on the sync
screen; let an interval pass. Launch several games with and without
Atmosphère's cheats enabled. Start RetroArch from the album (applet mode).

**Expect.** Under a game:

```
21 info sync.held a game is running; the interval tick waits until it closes
```

and *Syncs when the game closes* on the sync screen.

**Pass.** No save is written while a game runs (#242); no `sync.tick` while the
game stays open, including across an interval; every launch succeeds. Record
whether a tick runs under album-mode RetroArch -- if it does, that is the gap
#242 records, not a regression.

### LIFE-09: The sync after a game exits

**Steps.** Continuing LIFE-08, close the game. In a Tico module, change a save
whose newer copy is on the server, keep the game open over an interval, then
exit through the library.

**Pass.** Within about 5 s exactly **one** `sync.tick`; the save on the card is
the module's own write from exit, uploaded once, and the server's copy was not
downloaded under it.

### LIFE-10: A play session names its title

**Steps.** Start a Tico module, let a tick fall due, play, quit to the library,
close Tico. Repeat with RetroArch launched over a title, and once with a short
game played entirely between two ticks with no press.

**Expect.**

```
22 info play.recorded rom 12 slot autosave: played up to 31 min, title 0100000000ABC000
```

**Pass.** A 16-digit title id -- the module's, not the library's; for RetroArch
the borrowed title's -- and `title unknown` only for the game nothing looked at
(#261). Record each module's title id.

---

## SAVE -- saves on the card and on the server

### SAVE-01: Tico round trip with a PC RetroArch

**Steps.** Through the disposable RomM: play a homebrew SNES game in Tico and
save; sync; load it in RetroArch on a PC (Snes9x `.srm`, Cloud Sync to RomM);
save there; sync the console; load in Tico.

**Pass.** Each side loads the other's progress; the console's previous bytes
are under `/config/rommsync/.backup/` before the download replaced them.

### SAVE-02: RetroArch on the Switch, round trip

**Steps.** SAVE-01 with the Switch's RetroArch instead of Tico.

**Pass.** As SAVE-01, the save in `/retroarch/cores/savefiles/<CoreFolder>/`.

### SAVE-03: A conflict, kept from the server

**Steps.** Change the same save on the console and on the PC between two syncs.
Sync the console. On the Conflicts screen choose *Keep server*.

**Pass.** The server's bytes land on the card, the card's previous bytes are in
`.backup/` first, and the entry leaves the list.

### SAVE-04: A conflict, kept local

**Steps.** As SAVE-03, choose *Keep local*. Repeat once with a game running
(the choice is held until it closes, #275).

**Pass.** The card's bytes become the server's head; nothing on the card is
overwritten; under a game nothing is written until it closes.

### SAVE-05: Power cut during a save write

**Steps.** On a spare card with a throwaway save whose newer copy is on the
server, hold the power button to cut power mid-download of that save. Boot and
let one tick run. Do it five times.

**Pass.** Every time, the save holds either its old bytes or the server's; if
the server's, `.backup/` holds the old ones. The log has no *the card would not
confirm the copy is on it* (#203: `fsdevCommitDevice("sdmc:")` answers).

### SAVE-06: A full card

**Steps.** Fill the spare card with a dummy file. (1) Queue a rom bigger than
the space left. (2) Queue two roms that each fit alone but not together, or
fill the card while one transfers. (3) Let a save download need a backup on a
card with no room.

**Expect.**

```
9 error save.failed download rom 1234, slot autosave: the previous bytes could not be preserved: the card is full
```

**Pass.** (1) fails at once with *the card has N free, and this rom needs M*
and no content request in RomM's log; (2) the row says *the card is full* (not
*the card refused the write*: libnx maps Horizon's error to `ENOSPC`);
(3) the save is not overwritten. Record any `backup.pruned` line.

### SAVE-07: First-time restores land where the emulator reads

**Steps.** #44's *First-time restores* list (#237), on the disposable
collection: a save the card never held, for Tico 0.8 with one account, Tico
before 0.8, two unnamed accounts, `play_with = retroarch`, PSX into
DuckStation, DC/NDS/Saturn, and a file already at the target path.

**Pass.** Each lands at the path #237 names and the emulator loads it; with two
unnamed accounts nothing is placed and the log says why; a file already there
is left byte for byte, with no backup written.

### SAVE-08: Tico's layout on a real card

**Steps.** #44's *What #231 assumed of Tico 0.8* list: the per-account folder,
two accounts, an empty `users/`, Tico's folder names, what sits beside a save,
Snes9x's `.rtc`.

**Pass.** Each matches `core/src/tico.cpp` and `core/src/config.cpp`; any that
does not is corrected there and in #231 in the same change.

### SAVE-09: RetroArch's layout on a real card

**Steps.** #44's *What #230 took from RetroArch's source* list: the config
file's path, the default save and state folders, the `<CoreFolder>` names,
Cloud Sync's menu; and #229's question of which PSX and NDS cores are installed
by default.

**Pass.** Each matches `core/include/rommsync/retroarch_cfg.hpp`; with Cloud
Sync on the settings screen says *RetroArch syncs itself* and no RetroArch save
is uploaded.

### SAVE-10: The card's format and the 4 GiB ceiling

**Steps.** Read the boot log's card-format line on a FAT32 and on an exFAT card.
Queue a rom over 4 GiB.

**Expect.**

```
2 info boot rommsync: card format unknown (this console reads exFAT, so it could be either); a file on it stops at 4 GiB
```

**Pass.** The reason in brackets is not *fsIsExFatSupported was refused* (the
NPDM grants it; a console with no exFAT driver reads `fat32` instead); the big
rom is refused with *a file on a FAT32 card stops at 4 GiB* on either card.

### SAVE-11: Captures for the save-format adapters

**Steps.** Copy one real save from each Tico module off the card (PSX `.mcd`, a
PSP save folder, N64's split files, a DC VMU, an NDS save), and
`sdmc:/tico/config/` after switching GB from Gambatte to mGBA. Homebrew only.

**Pass.** Each is attached to #262 (adapters) and #229 (module choice). Never a
save from a commercial rom in the tree.

---

## MEM -- memory and co-residents

### MEM-01: The co-resident set

**Steps.** Install sys-clk, MissionControl, ldn_mitm, emuiibo, a SaltyNX-type
module and nx-ovlloader at 4 MB. Read `svcGetResourceLimitCurrentValue` on the
System limit before and after enabling `sys-rommsync`. Play a game for an hour
with sync on a short interval.

**Pass.** The delta is within the resident footprint of 2,351,104 bytes
([DEVELOPMENT.md](DEVELOPMENT.md#what-the-process-costs-the-pool),
`scripts/image-budget.sh`); every module keeps working; no fatal (M11-12, #296).

### MEM-02: An applet during a sync

**Steps.** Start a tick with a download, then open the eShop and the web browser
applet; close them.

**Pass.** Both applets open and work; the tick and the download complete; no
fatal.

### MEM-03: The overlay at a 4 MB heap

**Steps.** With nx-ovlloader's heap at 4 MB, run OVL-01 in full; repeat
spot-checks at 6 MB and 8 MB.

**Pass.** Every screen opens and works at 4 MB.

---

## CLK -- the console's clock

### CLK-01: A clock a week ahead

**Steps.** Settings -> *Synchronise Clock via Internet* off; set the date a week
ahead. Change a save whose upload the server would decide by timestamp, and one
whose newer copy is on the server. Sync.

**Expect.** The status screen's **Clock** row reads *7 days ahead; uploads held*.

```
14 warn clock.skew the console clock is 7 days ahead of the server's clock; saves the server would choose by comparing timestamps are not uploaded until the console's date and time are set
15 warn save.failed upload rom 12, slot autosave: held, not uploaded: the console clock is 7 days ahead of the server's clock, and the server chose this upload by comparing timestamps; set the console's date and time, and it goes next sync
```

**Pass.** One `save.failed ... held, not uploaded` per timestamp-decided upload;
the download still lands, backed up first (M11-11, #295).

### CLK-02: The clock corrected

**Steps.** Continuing CLK-01, set the clock right and sync.

**Expect.**

```
16 info clock.skew the console clock agrees with the server's clock; held uploads go again
```

**Pass.** The held uploads go, once each.

### CLK-03: An RTC reset, over https

**Steps.** Set the date before 2026 (or let the RTC reset) with an `https://`
server. Sync.

**Expect.**

```
2 error net.tls_clock GET /api/heartbeat: TLS failure; the server's certificate was refused; the console clock is not set: it reads a date before 2026 -- set the console's date and time, or let it synchronise over the internet
```

**Pass.** `net.tls_clock`, not `net.tls`; the Clock row reads *Not set; uploads
held*.

### CLK-04: A non-UTC timezone and the card's mtimes

**Steps.** Set the console to a zone far from UTC (UTC+9). Note a UTC instant,
write a save at it, and read its `st_mtime` the way
`sysmodule/source/card.cpp` does -- the `updated_at` the next negotiate sends
shows it (RomM's access log), and so does PRE-02's selftest run in that zone
(its report's *platform.mtime_utc* row). Then repeat LIFE-10's
first step in that zone.

**Pass.** The answer -- UTC or local -- is recorded in
[SYNC_PROTOCOL.md](SYNC_PROTOCOL.md#the-consoles-clock-295). If local, the
conversion is filed as a bug. A title of `unknown` only in that zone is the
same skew, reported against #261.

---

## OVL -- the overlay

### OVL-01: The overlay's script, end to end

**Steps.** [overlay/README.md](../overlay/README.md#manual-verification-m8-2)
steps 1-17, in order, in handheld and then docked: the status, settings, sync,
library, conflicts, pairing and log screens; re-pair; the hide/show, cycling,
library-exit, busy-sysmodule and hold-to-repeat steps.

**Pass.** Every step draws what it names. Any layout constant that had to move
is recorded, with the screen file it is in.

### OVL-02: The diagnostics bundle

**Steps.** Settings -> **Log**, press **Y** (M11-20, #304).

**Expect.**

```
212 info diagnostics.bundle written: /config/rommsync/diagnostics-20261118-110640.txt
```

**Pass.** The file exists, holds versions, config, state counts, backup usage,
the last 200 log lines and crash-report names, and no token, `device_code` or
URL credential. Its `atmosphere` line is the version the console's Atmosphère
reports, and its `emummc` line is `yes (partition)`, `yes (file)` or
`no (sysMMC)` as this card boots -- neither starts with `not read:` (#322).

---

## DBG -- what a failure leaves behind

### DBG-01: Collect everything after any FAIL

**Steps.** After any FAIL, before rebooting if the console still answers: press
**Y** on the log screen (OVL-02); then copy off the card
`/config/rommsync/rommsync.log` and `rommsync.log.old`, the diagnostics bundle,
`/atmosphere/crash_reports/` and `/atmosphere/fatal_reports/` (any report for
`4200000000524D53` or the overlay). Never `config.ini`'s secrets, `token.dat` or
`device.dat` ([TROUBLESHOOTING.md](TROUBLESHOOTING.md#what-to-attach-to-a-bug-report)).

**Pass.** Every FAIL in a run has its files named in its row's *Evidence*.

### DBG-02: A standalone gdbstub, when a hang needs one

**Steps.** Optional, for a hang nothing else explains. In
`/atmosphere/config/system_settings.ini`, under `[atmosphere]`, set
`enable_standalone_gdbstub = u8!0x1` and `enable_htc = u8!0x0`; reboot; attach
`aarch64-none-elf-gdb` over the network and dump the sysmodule's threads.

**Pass.** The backtrace is attached to the FAIL. Set both back afterwards.

---

## What the launch gate cites

M12-0's (#305) console rows each cite these scenarios as their evidence, by id,
from [HARDWARE_RESULTS.md](HARDWARE_RESULTS.md). Every scenario is cited by at
least one row.

| Launch-gate row | Scenarios |
|---|---|
| `selftest` | PRE-02, CLK-04 |
| `smoke` | PRE-01, PRE-02, PRE-03, INST-01, INST-02, INST-03, INST-04, INST-05, BOOT-01, BOOT-02, BOOT-03, NET-07, NET-08, LIFE-08, LIFE-09, LIFE-10, SAVE-01, SAVE-02, SAVE-03, SAVE-04, SAVE-05, SAVE-06, SAVE-07, SAVE-08, SAVE-09, SAVE-10, SAVE-11, DBG-01 |
| `overlay` | OVL-01, OVL-02, INST-06, LIFE-05, LIFE-06, LIFE-07, MEM-03 |
| `soak` | LIFE-01, LIFE-02, LIFE-03, LIFE-04, LIFE-05, LIFE-06, NET-01, NET-02, NET-03, NET-04, MEM-01, MEM-02, CLK-01, CLK-02, CLK-03, SAVE-05, DBG-01, DBG-02 |
| `matrix` | PRE-02, INST-01, INST-02, INST-03, BOOT-01, NET-06, NET-09, MEM-03, OVL-01 |
| `servers` | NET-05, NET-06, CLK-03 |
| `beta` | INST-01, INST-02, INST-03, SAVE-01, SAVE-03, SAVE-04, OVL-02, DBG-01 |

## Where #44's checklists went

#44 (M8-2) gathered console checks from a dozen issues as they landed. They are
this plan's scenarios now; #44's sections stay as the record of why each was
asked.

| #44 section | Scenarios |
|---|---|
| Order of operations (steps 1-3) | PRE-01, PRE-02, INST-02, SAVE-01 |
| The overlay's part of this, from M4-1 (#23) | OVL-01, INST-06 |
| Two things #35 could not establish off-console | INST-01, INST-02 |
| The sync screen's part of this, from M4-2 (#24) | OVL-01 |
| What M4-4 (#26) landed for this | OVL-01 |
| The troubleshooting guide's part of this, from M7-3 (#38) | NET-05, NET-06, SAVE-08, SAVE-09, DBG-01 |
| Added by M10 (cross-device interop, #226) | SAVE-01, SAVE-02, SAVE-08, SAVE-09, SAVE-11, LIFE-08, LIFE-10, BOOT-01 |
| The overlay's part of this, from M9-6 (#209) | OVL-01, LIFE-05 |
| The artifacts' part of this, from M9-12 (#211) | INST-01, PRE-03 |
| No sync under a running game, from M10-16 (#242) | LIFE-08, LIFE-09 |
| Which title was played, from M10-26 (#261) | LIFE-10, CLK-04 |
| First-time restores, from M10-11 (#237) | SAVE-07 |
| From the 2026-11-13 launch-readiness audit | PRE-02, LIFE-07 |
| Two questions to answer on the first boot | NET-07, NET-08 |
| Two more measurements, from #229 | SAVE-09, SAVE-11 |
| The SD card's refusals, from M9-13 (#202) | SAVE-06, SAVE-10 |
| Durability on the card, from M9-14 (#203) | SAVE-05 |
| What #230 took from RetroArch's source | SAVE-09 |
| What #231 assumed of Tico 0.8 | SAVE-08 |
| The pairing screen between approval and *Paired* | OVL-01 |
