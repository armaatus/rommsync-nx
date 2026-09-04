# sys-rommsync (sysmodule)

The background engine. **Not yet implemented** — this directory is where the
sysmodule sources land. Build on a devkitPro dev machine, never on the RomM
server.

## Responsibilities

- Device-code auth + token lifecycle ([../docs/AUTH.md](../docs/AUTH.md))
- HTTPS via the Horizon `ssl` service ([../docs/DEVELOPMENT.md](../docs/DEVELOPMENT.md#tls-in-a-sysmodule))
- Save sync: negotiate → execute → complete ([../docs/SYNC_PROTOCOL.md](../docs/SYNC_PROTOCOL.md))
- Download queue worker (Range resume, hash verify) → emulator folders
- Scheduler (boot / interval / on-demand)
- IPC service for the overlay

## Planned structure

```
Makefile
config.json           # sysmodule metadata + heap sizing
source/
  main.cpp            # service loop, scheduler
  http/               # HttpClient interface + ssl-service backend
  auth/               # device-code flow, token store
  sync/               # engine, matcher, state.db
  download/           # queue worker
  ipc/                # service definition (shared with overlay)
  config/             # config.ini parser + folder map
include/
```

## First task

Start with the **off-console harness**, not hardware. The core engine here is
built and proven natively (host build + a real RomM in docker) before any of it
runs on a Switch — see [`../docs/TESTING.md`](../docs/TESTING.md).

- **M0-2** — `HttpClient` interface + native (libcurl) backend.
- **M0-5** — host harness + fault-injection scenarios to run the engine
  end-to-end against the docker RomM.
- **M0-1** — the sysmodule `ssl`-service TLS question is a *de-risking spike*
  (Ryujinx-first, off the boot path), not a prerequisite for the logic above.

See milestone M0 in [`../ISSUES.md`](../ISSUES.md).
