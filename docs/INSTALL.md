# Installing rommsync-nx

This is the one page written for the person holding the console. Everything
else in `docs/` is written for contributors.

It takes you from "I downloaded the zip" to "my saves are syncing", and assumes
you have installed homebrew on a Switch before and have never seen RomM.

> **Not yet validated on hardware.** No part of this procedure has been followed
> on a real console: this project does not touch hardware until the v1 gate
> (issue [#43](https://github.com/armaatus/rommsync-nx/issues/43)) passes. The
> first real-console run is **M8-2**
> ([#44](https://github.com/armaatus/rommsync-nx/issues/44)), and that is what
> removes this note. Until then, treat every step below as reviewed on paper —
> the file names and paths are checked against the release zip on every build,
> the console's reaction to them is not.

---

## Before you start

- **A modded console running Atmosphère**, new enough to load sysmodules out of
  `atmosphere/contents/` — the folder this release installs into, and the one
  Atmosphère has used since **0.14.0**, when that folder was renamed from
  `titles`.
  Any current 1.x is far past that. That floor is the layout's, not a tested
  number: nothing in this repo pins a minimum, and establishing the real one is
  part of M8-2 ([#44](https://github.com/armaatus/rommsync-nx/issues/44)).
- **Ultrahand or Tesla already installed and working.** The control UI is an
  overlay; if your overlay menu does not open today, fix that first — none of
  what follows is visible without it.
- **[ovl-sysmodules](https://github.com/ppkantorski/ovl-sysmodules) installed.**
  That is the overlay that starts, stops and boot-toggles sysmodules, and it is
  the only supported way to turn `sys-rommsync` on. rommsync-nx does not ship
  its own boot switch.
- **A RomM 5.2.0 server the console can reach over the network**, and an account
  on it you can sign into from a phone or a computer. Make a dedicated RomM user
  for the Switch if you can — it is the thing you revoke later without touching
  your main account ([SECURITY.md](SECURITY.md#tokens--scopes)).

You will not need a keyboard on the console, and you will never type a RomM
account secret into it. That is the entire reason pairing works the way it does.

---

## 1. Unpack the zip

Unzip the release onto the **root** of your SD card, keeping the folder
structure. There is no wrapper directory: the archive merges into the
`atmosphere/` and `switch/` folders your card already has.

Five files land, and nothing else changes:

```
README.txt
LICENSE
atmosphere/contents/4200000000524D53/exefs.nsp
config/rommsync/config.ini.example
switch/.overlays/ovl-rommsync.ovl
```

- `atmosphere/contents/4200000000524D53/exefs.nsp` is the sysmodule.
  `4200000000524D53` is its title id, and `exefs.nsp` is the only name
  Atmosphère loads — a file named anything else in that folder installs
  cleanly, boots, and does nothing at all.
- `switch/.overlays/ovl-rommsync.ovl` is the overlay. It only appears in the
  menu from that folder.
- `config/rommsync/config.ini.example` is a starting configuration. It is an
  *example*: the file the client actually reads is `config/rommsync/config.ini`,
  which you make in step 3.
- `README.txt` and `LICENSE` land at the **root** of the card under those names,
  so an unrelated `README.txt` or `LICENSE` already sitting there is replaced.

There is **no `flags/` directory** in the archive at all — not an empty one, not
one holding a disabled flag. `atmosphere/contents/4200000000524D53/` arrives
holding `exefs.nsp` and nothing else.

That is deliberate. `flags/boot2.flag` is what makes Atmosphère launch the
sysmodule at boot, so shipping it would mean the next boot after you unpacked
the zip started a sysmodule that has no server, no token, and a folder map you
have not checked yet — reading and writing save folders before you had any
chance to look at which ones. Installed first, started when you say so.

---

## 2. Enable the sysmodule

The sysmodule ships **off**. Unpacking the zip installs it; it does not start
it, and it will not start on the next boot either until you say so.

Open **ovl-sysmodules**, find `sys-rommsync`, and turn it on. That is the whole
step — but read the rest of this section before you do, because there are two
different switches with two different meanings and mistaking one for the other
is the single most common way this goes wrong.

### The two switches

| Switch | Where it lives | What it decides |
|---|---|---|
| **ovl-sysmodules' boot toggle** | `atmosphere/contents/4200000000524D53/flags/boot2.flag`, plus a launch/terminate for *right now* | whether the sysmodule **process exists at all** |
| **ovl-rommsync's enable switch** | `[sync] enabled` in `config/rommsync/config.ini` | whether a running sysmodule **syncs** |

They are not two spellings of the same thing. `boot2.flag` is ovl-sysmodules'
to write: do not create it by hand, and rommsync-nx never writes it itself —
two overlays writing the same flag is how they end up disagreeing about what is
on. (Whether ovl-sysmodules also *creates* the `flags/` directory, which the
archive deliberately does not ship, is still being confirmed —
[#33](https://github.com/armaatus/rommsync-nx/issues/33). If your toggle appears
to do nothing, that is the first thing to report.)

### The four states

| What you have | Boot toggle | Enable switch | What is happening |
|---|---|---|---|
| **Not installed** | — | — | The files are not on the card. The overlay is not in the menu. |
| **Installed, not set to boot** | off | (unread) | There is no process. Nothing syncs and nothing answers the overlay, which says **"sys-rommsync is not running"** rather than mislabelling it *disabled* — a switch that does nothing is worse than a missing one. |
| **Running, sync disabled** | on | off | The process is resident and answers the overlay, and nothing syncs. Automatic syncs stop, and the engine refuses an on-demand one with **"Sync is off"** rather than starting it — you are told which switch to flip instead of being shown a spinner that never moves. (The **Sync now** button that asks for one arrives with [#24](https://github.com/armaatus/rommsync-nx/issues/24); see step 5.) |
| **Running, sync enabled** | on | on | The normal state: syncs on boot and on the timer — and on demand too, once [#24](https://github.com/armaatus/rommsync-nx/issues/24) lands **Sync now**. |

The row that catches people is the second one. Turning rommsync-nx's *own*
enable switch on while the boot toggle is off does nothing, because there is no
process to turn on — and the switch you are looking at is being drawn from a
file, not from a running program. Turn the sysmodule on in ovl-sysmodules
first.

---

## 3. Point it at your server

Copy the example to the real file and set one key:

```
config/rommsync/config.ini.example   →   config/rommsync/config.ini
```

```ini
[server]
url = https://romm.example.com
```

That is the only key you must set. Everything else has a working default, and
the file only needs to hold your **overrides**.

Use `https://` if your RomM has it. Plain `http://` works and sends your token
in the clear on every request — see [Security](#security) below.

Once the overlay's Settings screen lands
([#26](https://github.com/armaatus/rommsync-nx/issues/26)) you will be able to
set the server URL from the console instead. Today, the file is the way.

The full key list — the sync interval, the download options, and the folder map
that decides where roms land and which folders saves are read from — is in
[CONFIG.md](CONFIG.md). It is not repeated here.

---

## 4. Pair the console

Pairing is a device-code flow: the console shows a short code, you approve it
once in a browser, and the console gets a token. No account secret is ever
entered on the console, and there is nothing to type with a joystick but a
button press.

> **Not reachable yet.** The pairing screen is built, but the only thing that
> opens it is **Re-pair** on the Settings screen, which arrives with
> [#26](https://github.com/armaatus/rommsync-nx/issues/26). Until that lands
> there is no button in the overlay that starts this. The six steps below are
> the release that ships once it does.

Six steps:

1. Reboot (or launch the sysmodule from ovl-sysmodules) so `sys-rommsync` is
   actually running.
2. Open your overlay menu and choose **ovl-rommsync**.
3. Press **Pair**. The screen says **"Contacting the server"** while it asks RomM
   for a code.
4. The screen then shows an **address** and an **eight-character code**. Open
   that address in a browser on a phone or a computer, signed in to RomM.
5. Enter the code and approve it.
6. The overlay switches to **Paired** on its own. Nothing else to press.

The token is written to `config/rommsync/token.dat` and the console's identifier
to `config/rommsync/device.dat`. Neither is in the release zip, so neither is
touched by an upgrade.

### What the pairing screen can say

The overlay has one line for every state the pairing can be in. The three ways
it can fail are three different sentences on purpose, because only one of them
is worth reporting as a bug:

| The screen says | State | What it means |
|---|---|---|
| Not paired | `idle` | Nothing has been started. Press **Pair**. |
| Contacting the server | `starting` | Asking RomM for a code. There is nothing to show yet; this can take as long as your request timeout. |
| Pair this console | `pending` | A code is live. Go to the address shown and enter it. |
| Paired | `approved` | Done. The token is stored and the sysmodule is authenticated. |
| Pairing was refused | `denied` | Somebody refused this code in the RomM web UI. Press **Start over** and approve the new code. |
| The code expired | `expired` | Nobody entered the code before it ran out. Press **Start over** for a fresh one. |
| Pairing failed | `failed` | Your server answered something this client cannot act on. This is the one worth a bug report; the screen carries the reason. |

What the console does between step 4 and step 6 — how often it polls, how it
backs off, and what each answer from RomM means — is in [AUTH.md](AUTH.md).
You do not need any of it to pair.

---

## 5. The first sync

With the sysmodule running, paired, and `[sync] enabled = true`, a sync happens
on boot and then on the interval in `config.ini`. You will also be able to ask
for one on demand with **Sync now**, once
[#24](https://github.com/armaatus/rommsync-nx/issues/24) lands it. That button
needs the enable switch on: with `[sync] enabled = false` it answers
**"Sync is off"** rather than syncing.

Two directions, and they use different folders:

- **Roms land** in the *first* `roms` folder mapped for that platform, under the
  name the file has on RomM's own filesystem. A platform with no mapping, or one
  mapped with no `roms` folder, is skipped with a reason — never guessed into a
  folder.
- **Saves are read from** every `saves` folder mapped for that platform, and
  reconciled with the server. The server is the source of truth on a conflict,
  and an overwritten save is backed up first, always.

Which folders those are is the folder map in
[CONFIG.md](CONFIG.md#defaults--the-folder-map), and there is one warning worth
repeating here:

> **Check the Tico paths on your own card.** The built-in RetroArch defaults are
> correct. The Tico defaults are best-guesses, and a wrong `saves` folder is a
> platform whose saves are silently never found. Correct them in `config.ini`
> before you rely on the sync.

One rule to read there before you edit the map: a `[platform.…]` section
replaces that platform's defaults rather than adding to them
([CONFIG.md](CONFIG.md#a-platform-section-replaces-that-platforms-defaults)).

---

## 6. Upgrading

Download the newer release and unzip it over the top, onto the SD root, exactly
as in step 1.

The five files from step 1 are replaced. Nothing else is, because nothing else
is in the archive:

| File | What happens |
|---|---|
| `config/rommsync/config.ini` | survives — your settings are kept |
| `config/rommsync/token.dat` | survives — you stay paired |
| `config/rommsync/device.dat` | survives — the console keeps its identity |
| `config/rommsync/state.db` | survives — the sync baseline is kept |
| `config/rommsync/queue.json` | survives — a queued download is still queued |
| `atmosphere/contents/4200000000524D53/flags/boot2.flag` | survives — the sysmodule stays enabled |

The only file the upgrade overwrites that you might have edited is
`config/rommsync/config.ini.example`, which nothing reads.

---

## 7. Uninstalling and un-pairing

Two things, and doing only the first leaves a live token on your server.

**Remove the client.** Turn `sys-rommsync` off in ovl-sysmodules, then delete
the two artifacts:

```
atmosphere/contents/4200000000524D53/exefs.nsp
switch/.overlays/ovl-rommsync.ovl
```

`config/rommsync/` is left behind on purpose; delete it too if you are done for
good.

**Revoke the token on the server.** Deleting `config/rommsync/token.dat` stops
*this console* from using it and nothing more — a token that has been on an SD
card should be treated as recoverable from that card. Revoke it in RomM's
client-tokens UI. That is the only thing that actually makes it stop working,
and it is why a dedicated RomM user is worth the two minutes.

The overlay's **Re-pair** does the client half for you: it discards the stored
token and restarts the flow from step 4. It arrives with the Settings screen
([#26](https://github.com/armaatus/rommsync-nx/issues/26)), like everything else
that opens the pairing screen — see the note in step 4. It leaves `device.dat`
alone, so RomM recognises the same console rather than collecting a new one each
time. See [AUTH.md](AUTH.md#re-pairing--revocation).

---

## Security

Three things are worth knowing before you expose any of this to a network. Each
is one paragraph here and a full section in [SECURITY.md](SECURITY.md).

**Plain `http://` sends your token in the clear.** Every request carries the
bearer token, so anything between the console and RomM can read it on an
unencrypted connection. On a home LAN you may decide that is fine; across the
internet it is not. Put RomM behind an HTTPS reverse proxy rather than
port-forwarding it directly, or keep it off the public internet entirely with a
VPN or a tailnet —
[SECURITY.md](SECURITY.md#exposing-romm-safely) has the recipe.

**A self-signed certificate is imported, not waved through.** If your RomM's
certificate is one no public CA signed, the answer is to import that certificate
so the console checks against a CA you chose. Verification stays on. There is no
setting in this client that turns it off, and there will not be — see
[SECURITY.md](SECURITY.md#a-self-signed-certificate-on-a-home-server).

**`token.dat` sits unencrypted on the card.** Horizon's FAT32 has no permission
bits, so there is nothing to restrict and no honest way to hide the file from
other homebrew or from anyone who pulls the card. The mitigation is not secrecy,
it is scope and revocation: the token is granted the documented minimum, it
belongs to a user you can revoke, and revoking it is what ends it —
[SECURITY.md](SECURITY.md#token-at-rest-on-the-sd).

---

## When it does not work

There is a dedicated troubleshooting guide, arriving in M7-3 —
[#38](https://github.com/armaatus/rommsync-nx/issues/38). It is where the
symptom-by-symptom answers live, and this page will link it here rather than
growing a second copy.

Until it lands, the three things worth checking first are all in this page:
the overlay saying **"sys-rommsync is not running"** is the boot toggle
([step 2](#2-enable-the-sysmodule)), **"No server set"** is `[server] url`
([step 3](#3-point-it-at-your-server)), and saves that never appear are almost
always the folder map ([step 5](#5-the-first-sync)).
