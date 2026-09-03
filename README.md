# rommsync-nx

**On-device RomM sync for the modded Nintendo Switch.** Browse your self-hosted
[RomM](https://romm.app) library, download games straight into your emulator
folders, play, and have your saves **auto-sync back to the server** — from any
network — so every device and the server always hold the same version.

This is the piece the Switch ecosystem is missing. [SwitchRomM] already does
browse-and-download; **rommsync-nx** adds the thing nothing on Switch does yet:
save sync via RomM's device-sync protocol, running as a background **sysmodule**
you control from an **Ultrahand / Tesla overlay**.

> Status: **planning / server-side groundwork complete.** The Switch components
> are specced and issue-tracked but not yet implemented. The RomM 5.2.0 API this
> targets is captured and verified — see [`docs/API_CONTRACT.md`](docs/API_CONTRACT.md).

## Why a sysmodule + overlay (not just an app)

SwitchRomM is a foreground NRO: you sit in it while it downloads, and it can't
touch your saves after you leave. rommsync-nx splits the job the way the rest of
the Switch homebrew scene does:

- **`sys-rommsync`** — a background **sysmodule**. Authenticates to RomM once
  (device-code flow, no passwords typed on-console), then quietly: runs the
  download queue, and syncs saves on boot / on a timer / on demand. Works over
  the internet because it calls *out* to RomM over HTTPS — nothing is exposed
  inbound on the Switch.
- **`ovl-rommsync`** — an **Ultrahand/Tesla overlay** (`.ovl`) to drive it:
  browse the library and queue games, toggle sync on/off, see last-sync status,
  and edit config. Built on [libultrahand], so it appears in your Ultrahand
  overlay list like sys-clk or ovl-sysmodules.

## The experience we're building

1. Open the overlay, browse your RomM library, tap games to queue.
2. `sys-rommsync` downloads them in the background into the right Tico / RetroArch
   folders.
3. Play offline. On the next sync tick (boot, timer, or "Sync now" in the
   overlay), saves reconcile with RomM — **server is the source of truth**, so
   all your devices converge to the same save.

## Architecture at a glance

```
        Nintendo Switch (Atmosphère)                    Your server
 ┌───────────────────────────────────────┐      ┌───────────────────────┐
 │  ovl-rommsync (.ovl overlay)           │      │   RomM 5.2.0           │
 │     │  IPC (tipc)                      │      │   (HTTPS, public URL)  │
 │     ▼                                  │ HTTPS│                        │
 │  sys-rommsync (sysmodule)  ────────────┼─────▶│  /api/auth/device/*    │
 │   • device-code auth                   │      │  /api/sync/negotiate   │
 │   • download queue → SD emulator dirs  │      │  /api/saves ...        │
 │   • save sync (negotiate/execute/      │      │  /api/roms ...         │
 │     complete)                          │      │                        │
 │   • config + token on SD               │      └───────────────────────┘
 └───────────────────────────────────────┘
```

Full detail in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Repository layout

| Path | What |
|------|------|
| `docs/` | Architecture, the pinned RomM API contract, auth & sync protocol, config, security, dev environment |
| `sysmodule/` | `sys-rommsync` — the background engine (to be built) |
| `overlay/` | `ovl-rommsync` — the libultrahand overlay (to be built) |
| `server/` | Server-side: API snapshot (source of truth) + a contract-probe script that runs against a live RomM |
| `ISSUES.md` | The full milestone + issue backlog |
| `scripts/create_issues.sh` | Creates the GitHub milestones/labels/issues via `gh` |

## Getting started (contributors)

Read, in order: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) →
[`docs/API_CONTRACT.md`](docs/API_CONTRACT.md) →
[`docs/SYNC_PROTOCOL.md`](docs/SYNC_PROTOCOL.md) →
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md). Then pick an issue from milestone
**M1**.

## Credit / prior art

- [SwitchRomM] — the download-side RomM client we build on the ideas of.
- [libultrahand] / [Ultrahand-Overlay] and [ovl-sysmodules] — overlay framework
  and the reference for sysmodule toggling + IPC.
- [RomM] — the server and its device-sync protocol.

[SwitchRomM]: https://github.com/Shalasere/SwitchRomM
[libultrahand]: https://github.com/ppkantorski/libultrahand
[Ultrahand-Overlay]: https://github.com/ppkantorski/Ultrahand-Overlay
[ovl-sysmodules]: https://github.com/ppkantorski/ovl-sysmodules
[RomM]: https://github.com/rommapp/romm
