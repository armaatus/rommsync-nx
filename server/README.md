# server/

Server-side material. This is the only part of the project that runs on the
machine hosting RomM — the Switch components live in `../sysmodule` and
`../overlay` and are built elsewhere.

## Contents

- **`contract/romm-openapi-5.2.0.json`** — a snapshot of the target RomM's
  OpenAPI spec. **Source of truth** for the API contract. Regenerate on upgrade:
  ```bash
  curl -s http://localhost:1515/openapi.json -o contract/romm-openapi-<version>.json
  ```
- **`probe_contract.py`** — validates the sync contract against a *live* RomM and
  prints real response shapes. See its header for usage. Run the read-only check
  any time; run `--auth --negotiate` to exercise the full flow (safe: sends an
  empty save list and never calls `/complete`).

## Preparing RomM for the Switch client

1. **Dedicated user / token.** Create a RomM user (or client token) just for the
   Switch so it can be revoked independently. Grant only the scopes in
   [`../docs/API_CONTRACT.md`](../docs/API_CONTRACT.md#scopes-to-request).

2. **Curate a collection.** Make a RomM collection (e.g. `Handheld`) and add the
   games you want on the Switch — the client can mirror it.

3. **Expose over HTTPS.** The Switch reaches RomM from any network, so put it
   behind TLS. Do **not** port-forward `1515` raw. Example Caddy:

   ```caddyfile
   romm.example.com {
       reverse_proxy localhost:1515
   }
   ```

   Even better, keep RomM off the public internet and reach it over WireGuard /
   Tailscale. See [`../docs/SECURITY.md`](../docs/SECURITY.md).

## Note on the old FTP bridge

An earlier, superseded approach (server polls the Switch's FTP server to sync
saves) lives at `/docker/romm-switch-bridge` on the origin host. It still works
as a **LAN-only fallback** but is not part of this project — the on-device client
here replaces it and works from any network without exposing the console.
