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

**M0-1: prototype HTTPS from a sysmodule** using the `ssl` service. This
de-risks the whole project — do it before anything else. See milestone M0 in
[`../ISSUES.md`](../ISSUES.md).
