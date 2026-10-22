# server/ — API contract and the test fixture

Nothing here runs on a Switch, and nothing here is built with devkitPro.

- **`contract/romm-openapi-5.4.0.json`** is the source of truth for the API --
  5.4.0 is the fixture's default since #235 and the oldest RomM the client syncs
  with since #228 -- with **`romm-openapi-5.2.0.json`** beside it (#227), which
  the body of docs/API_CONTRACT.md was written against. Check it before writing
  a request or assuming a response field. Do not edit either by hand —
  `probe_contract.py --openapi` writes one from a live RomM.
- **`contract/romm-5.4.0-emulator_names.py`** is RomM 5.4.0's
  `backend/handler/sync/retroarch/emulator_names.py`, verbatim, so the RetroArch
  core table in `core/src/save_source.cpp` has something to be diffed against
  (`source.retroarch_table`, #229). Re-pin it from the image, never by hand:
  `docker run --rm --entrypoint cat rommapp/romm:<version> /backend/handler/sync/retroarch/emulator_names.py`.
- **`contract/captures/`** is what the API docs quote: real responses, captured
  from the fixture — 5.2.0's at its top level, 5.4.0's under `captures/5.4.0/`. Read these before trusting a field's type or nullability —
  the OpenAPI snapshot declares neither that a save's `content_hash` is an MD5
  nor that an uploaded save comes back renamed.
- **`probe_contract.py`** prints real response shapes from a live RomM, and
  writes `contract/captures/` with `--capture`. Run it against the Docker
  fixture. **Never against a production RomM** — `--sync-scenarios` uploads
  saves and refuses a non-loopback URL without an explicit override.
- **`testing/`** is the fixture: a real RomM in Docker — the version
  `ROMM_VERSION` in `.env` names — plus the fault proxy.
  See `docker-compose.yml` and `fault_proxy.py`.

## Rules

- `roms.manifest` is fetched in public CI: homebrew and freely redistributable
  ROMs only, always checksum-pinned. Never a commercial ROM.
- The compose stack is disposable by design — ports and project name come from
  `.env`, per worktree. Never point it at a production volume.
- Fixtures that no real ROM can provide (a large file for Range resume, a
  multi-file rom, and the nested single-file rom the multi-file skip must not
  fire on) are generated deterministically in `make_fixtures.py`, so hashes are
  stable across machines.
