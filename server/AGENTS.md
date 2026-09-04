# server/ — API contract and the test fixture

Nothing here runs on a Switch, and nothing here is built with devkitPro.

- **`contract/romm-openapi-5.2.0.json`** is the source of truth for the API.
  Check it before writing a request or assuming a response field. Do not edit it
  by hand — regenerate it from a RomM instance.
- **`contract/captures/`** is what the API docs quote: real responses, captured
  from the fixture. Read these before trusting a field's type or nullability —
  the OpenAPI snapshot declares neither that a save's `content_hash` is an MD5
  nor that an uploaded save comes back renamed.
- **`probe_contract.py`** prints real response shapes from a live RomM, and
  writes `contract/captures/` with `--capture`. Run it against the Docker
  fixture. **Never against a production RomM** — `--sync-scenarios` uploads
  saves and refuses a non-loopback URL without an explicit override.
- **`testing/`** is the fixture: a real RomM 5.2.0 in Docker plus the fault proxy.
  See `docker-compose.yml` and `fault_proxy.py`.

## Rules

- `roms.manifest` is fetched in public CI: homebrew and freely redistributable
  ROMs only, always checksum-pinned. Never a commercial ROM.
- The compose stack is disposable by design — ports and project name come from
  `.env`, per worktree. Never point it at a production volume.
- Fixtures that no real ROM can provide (a large file for Range resume, a
  multi-file rom) are generated deterministically in `make_fixtures.py`, so
  hashes are stable across machines.
