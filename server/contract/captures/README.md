# Captured responses — real RomM 5.2.0

Every file here is an unedited response from a live RomM `5.2.0`, produced by

```bash
. ./.env
./.venv/bin/python server/probe_contract.py --url "$ROMM_BASE_URL" \
    --auth --negotiate --sync-scenarios --capture server/contract/captures
```

against **this worktree's disposable docker fixture** (`server/testing/`), never a
production library — CLAUDE.md hard rule 1. `--sync-scenarios` refuses a
non-loopback URL for that reason.

These are the shapes [`docs/API_CONTRACT.md`](../../../docs/API_CONTRACT.md),
[`docs/AUTH.md`](../../../docs/AUTH.md) and
[`docs/SYNC_PROTOCOL.md`](../../../docs/SYNC_PROTOCOL.md) quote, and the ones
`tests/test_contract_captures.py` re-checks against a live server on every
`ctest` run — so a RomM upgrade that changes a field turns the suite red instead
of being discovered on a console.

`romm-openapi-5.2.0.json` next door says what RomM *declares*. These say what it
*does*, which is not always the same: the OpenAPI schema does not tell you that
`content_hash` is an MD5, that an uploaded save comes back under a different
file name, that `expires_at` is null, or that an unapproved poll answers `400`
with the one string that separates "keep polling" from "this pairing is dead".

| File | Call |
|---|---|
| `auth-device-init.json` | `POST /api/auth/device/init` |
| `auth-device-token.json` | `POST /api/auth/device/token` (after approval) |
| `auth-device-token-pending.json` | `POST /api/auth/device/token` **before** approval — the `400` the pairing screen sees on every tick, and a shape the OpenAPI snapshot does not declare |
| `devices-get.json` | `GET /api/devices/{id}` — the `DeviceSchema` for the device **pairing** created, which is the one the client reads. The id field is `id` here |
| `devices-create.json` | `POST /api/devices` — the shape of a call this client deliberately never makes ([API_CONTRACT.md](../../../docs/API_CONTRACT.md#why-post-apidevices-is-the-wrong-call)); the id field is `device_id` |
| `roms-list.json` | `GET /api/roms?limit=1` |
| `saves-post.json` | `POST /api/saves` — one uploaded save (`SaveSchema`) |
| `sync-negotiate-empty.json` | `POST /api/sync/negotiate` with `saves: []` |
| `sync-negotiate-upload.json` | …a save the server does not have |
| `sync-negotiate-no-op.json` | …the same save, unchanged |
| `sync-negotiate-download.json` | …a device that has never seen a server save |
| `sync-negotiate-conflict.json` | …both sides changed since the last sync |
| `sync-negotiate-conflict-same-timestamp.json` | …no sync history, equal timestamps, different content |
| `saves-post-409-no-overwrite.json` | `POST /api/saves` executing a planned `upload` without `overwrite=true` — the refusal, which is a shape the client must handle |
| `sync-complete.json` | `POST /api/sync/sessions/{id}/complete` |

## What is edited, and what is not

Only secrets are touched: `device_code`, `user_code` and `access_token` are
replaced by `<device_code>`, `<user_code>` and `<access_token>` wherever their
text appears — including inside `verification_path_complete`, which carries the
user code in a query string. Everything else is byte-for-byte what the server
sent.

Ids, timestamps and the `probe-<hex>` slot names are fixture values from one
run. They are examples, not constants: the test compares field names and types,
never these values.

## Re-capturing

The probe fails rather than writing a capture whose name does not match the
action it got: a file called `…-conflict` that holds an `upload` is worse than a
red run, because it publishes a wrong shape under a right-looking name. The
`409` scenario is checked the same way.

Do it on a fixture with no saves in it (`GET /api/saves` returns `[]`), or the
negotiate captures pick up `download` operations for saves the scenarios did not
arrange — the probe prints a note when it sees this. The scenarios delete every
save they create, so a clean fixture stays clean.
