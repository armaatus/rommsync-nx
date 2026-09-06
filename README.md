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
>
> **Test-first, hardware-last.** No real Switch and no production RomM is touched
> until a v1 is fully proven off-console — on a native host harness against a
> throwaway **real** RomM in docker (with a fault-injecting proxy forcing the
> failure paths), then Ryujinx. See [`docs/TESTING.md`](docs/TESTING.md).

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

## Installing it (users)

[`docs/INSTALL.md`](docs/INSTALL.md) is the one page written for the person
holding the console: unpacking the release zip onto the SD, enabling the
sysmodule with ovl-sysmodules, pointing it at your RomM, and pairing without
typing anything secret on the console. Nothing in it has been run on hardware
yet — that is M8-2.

When a sync does not happen,
[`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) is the page: every failure
this client knows how to have, the line it writes in
`sdmc:/config/rommsync/rommsync.log` when it has one, and what to do about it.

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
| `sysmodule/` | `sys-rommsync` — the background engine. Skeleton: builds and packages, does nothing yet |
| `overlay/` | `ovl-rommsync` — the libultrahand overlay. Skeleton: builds and packages, draws nothing yet |
| `switch.mk` | Shared devkitPro rules for both Switch targets |
| `core/` | The portable engine — auth, sync, downloads, config, state. Builds and is tested natively. |
| `server/` | API snapshot (source of truth), contract-probe script, and the docker RomM test fixture |
| `orca.yaml` | Per-worktree provisioning: isolated RomM, seeded fixtures, ready-to-run build |
| `ISSUES.md` | The full milestone + issue backlog |
| `scripts/create_issues.sh` | Creates the GitHub milestones/labels/issues via `gh` |
| `CLAUDE.md` | Working agreement for agents and contributors — rules, commands, PR flow |

## Getting started (contributors)

Read, in order: [`docs/WORKFLOW.md`](docs/WORKFLOW.md) (how work happens here —
this project is built by agents in parallel worktrees, and the loop is worth
understanding before the code) → [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) →
[`docs/API_CONTRACT.md`](docs/API_CONTRACT.md) →
[`docs/SYNC_PROTOCOL.md`](docs/SYNC_PROTOCOL.md) →
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) →
[`docs/TESTING.md`](docs/TESTING.md). Then start with milestone **M0** — it builds
the off-console test harness (host + real docker RomM) that every later milestone
is developed and proven against before any hardware.

Quick start once a worktree exists:

```bash
cmake -S . -B build && cmake --build build
./scripts/orca/compose.sh up -d
ctest --test-dir build --output-on-failure
```

The two Switch targets are built separately, with devkitPro rather than CMake
(see [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md#toolchain)); nothing built there
is ever run before the hardware gate in M8:

```bash
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest \
  bash -lc 'make -C sysmodule && make -C overlay'
```

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
