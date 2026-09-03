#!/usr/bin/env bash
#
# Create labels, milestones, and issues for rommsync-nx on GitHub.
# Requires: gh (https://cli.github.com), authenticated (`gh auth login`),
# run from inside the repo after it has a GitHub remote (`gh repo create`).
#
# Idempotent-ish: labels/milestones are created if missing; issues are created
# every run, so only run the issue section ONCE (or delete dupes). Use --dry-run
# to preview.

set -euo pipefail
DRY=${1:-}

run() { if [ "$DRY" = "--dry-run" ]; then echo "DRY: $*"; else eval "$*"; fi; }

echo "== labels =="
declare -A labels=(
  [sysmodule]=1f6feb [overlay]=8957e5 [server]=0e8a16 [auth]=b60205
  [sync]=fbca04 [download]=0052cc [ipc]=5319e7 [config]=006b75
  [packaging]=c2e0c6 [docs]=cccccc [risk]=d93f0b [good-first-issue]=7057ff
  [test-harness]=5def9e
)
for name in "${!labels[@]}"; do
  run "gh label create '$name' --color '${labels[$name]}' --force >/dev/null"
done

echo "== milestones =="
milestones=(
  "M0 - Foundations & de-risking"
  "M1 - Authentication"
  "M2 - Save sync engine"
  "M3 - Downloads"
  "M4 - Overlay"
  "M5 - Config & IPC"
  "M6 - Packaging & CI"
  "M7 - Polish"
  "M8 - Hardware bring-up (after proven v1)"
)
for m in "${milestones[@]}"; do
  # gh has no native milestone create; use the API. Ignore "already_exists".
  run "gh api repos/{owner}/{repo}/milestones -f title='$m' >/dev/null 2>&1 || true"
done

# Helper: issue "title" "milestone title" "label,label" "body"
issue() {
  local title="$1" ms="$2" lbls="$3" body="$4"
  run "gh issue create --title \"$title\" --milestone \"$ms\" --label \"$lbls\" --body \"$body\""
}

echo "== issues =="
# --- M0  (off-console harness first; hardware-ish TLS spike is de-risked, not a blocker)
issue "M0-1: Sysmodule TLS feasibility via ssl service — Ryujinx-first, off the boot path" "M0 - Foundations & de-risking" "risk,sysmodule,test-harness" \
  "De-risking spike (not a blocker): a manually-launched NRO (not a sysmodule) does a TLS GET over the Horizon ssl service, run in Ryujinx against docker RomM; only if Ryujinx can't exercise ssl, run on a backup SD. Measure heap; go/no-go vs fallbacks. See docs/DEVELOPMENT.md#tls-in-a-sysmodule."
issue "M0-2: HttpClient interface + native (libcurl) backend for the host harness" "M0 - Foundations & de-risking" "sysmodule,test-harness,docs" \
  "GET/POST/multipart, streaming download-to-file, timeouts, cancellation, Range. Swappable TLS backend; ship the native (libcurl) backend so all networked logic is testable off-console."
issue "M0-3: CI builds host harness (runs tests) + Switch skeleton (artifacts)" "M0 - Foundations & de-risking" "packaging,test-harness" \
  "Two jobs: native build runs the test suite against the mock RomM; devkitpro build compiles empty sysmodule + overlay to artifacts (not run). Wire .github/workflows/ci.yml."
issue "M0-4: Capture real RomM auth/sync response shapes (docker RomM, never production)" "M0 - Foundations & de-risking" "server,docs,test-harness" \
  "Run server/probe_contract.py --auth --negotiate against a docker RomM (never production); paste real init/token/negotiate JSON into docs/API_CONTRACT.md and AUTH.md; feed the mock fixtures."
issue "M0-5: Host test harness + mock RomM server (fully offline)" "M0 - Foundations & de-risking" "server,test-harness,sync" \
  "Native rig wiring the core engine to a scriptable mock RomM that forces every edge case (401, conflict, partial failure, Range resume, multi-file skip). Backbone of proving v1 before hardware."
issue "M0-6: docker-compose RomM fixture for integration/fidelity tests" "M0 - Foundations & de-risking" "server,test-harness" \
  "One-command real RomM 5.2.0 on a throwaway volume, seeded with sample roms/saves; used to capture shapes, confirm the mock matches reality, and back the Ryujinx tier. Never a production DB."
issue "M0-7: No real hardware / no production data until proven v1 policy + M0 exit gate" "M0 - Foundations & de-risking" "docs,risk,test-harness" \
  "Write docs/TESTING.md, update DEVELOPMENT.md, define the v1 gate M8 depends on. M0 done = every component below the Horizon glue proven on host + mock + docker in CI, no console/production needed."
# --- M1
issue "M1-1: Device-code init/poll/token" "M1 - Authentication" "auth,sysmodule" "Implement init -> show code via IPC -> poll token. Persist token+expiry."
issue "M1-2: Verify & code against init/token response fields" "M1 - Authentication" "auth,docs" "Depends on M0-4."
issue "M1-3: Register device and cache device_id" "M1 - Authentication" "auth,sysmodule" "POST /api/devices, allow_existing."
issue "M1-4: Token refresh + 401 handling" "M1 - Authentication" "auth" "On 401 mark unauthenticated, signal overlay to re-pair."
issue "M1-5: Stable client id + secure token store" "M1 - Authentication" "auth,config" "See docs/SECURITY.md."
# --- M2
issue "M2-1: Pin SyncNegotiatePayload.saves[] fields" "M2 - Save sync engine" "sync,docs" "Typed struct from the snapshot."
issue "M2-2: Local scan + rom matching" "M2 - Save sync engine" "sync" "fs_name_no_ext, platform scoping, ambiguity handling."
issue "M2-3: SHA1 hashing + state.db baseline" "M2 - Save sync engine" "sync" "Skip unchanged files."
issue "M2-4: negotiate call + parse plan" "M2 - Save sync engine" "sync" ""
issue "M2-5: Execute plan (upload/download/conflict/noop) with backups" "M2 - Save sync engine" "sync" "Mandatory pre-overwrite backup; atomic writes."
issue "M2-6: complete call + persist baseline" "M2 - Save sync engine" "sync" ""
issue "M2-7: Offline/partial-failure safety" "M2 - Save sync engine" "sync" "Abort clean; accurate operations_failed; retry next tick."
issue "M2-8: Save states support (opt-in)" "M2 - Save sync engine" "sync" "Behind sync.states flag."
# --- M3
issue "M3-1: Platform->folder map" "M3 - Downloads" "download,config" "Defaults + config.ini overrides."
issue "M3-2: Download queue + worker" "M3 - Downloads" "download" "queue.json + worker loop."
issue "M3-3: Range-resume download + hash verify" "M3 - Downloads" "download" "Stream to file; verify sha1."
issue "M3-4: Handle multi-file roms" "M3 - Downloads" "download" "Skip/flag has_multiple_files cleanly."
issue "M3-5: Download progress over IPC" "M3 - Downloads" "download,ipc" ""
# --- M4
issue "M4-1: Minimal overlay showing status via IPC" "M4 - Overlay" "overlay,ipc" "libultrahand .ovl with ULTR signature."
issue "M4-2: Enable/disable + Sync now" "M4 - Overlay" "overlay" ""
issue "M4-3: Library browser + enqueue downloads" "M4 - Overlay" "overlay,download" "Paged."
issue "M4-4: Settings screen" "M4 - Overlay" "overlay,config" "Server URL, interval, states, folder overrides, re-pair."
issue "M4-5: Pairing screen" "M4 - Overlay" "overlay,auth" "Show user_code + verification URL, live state."
# --- M5
issue "M5-1: config.ini parser + validation" "M5 - Config & IPC" "config" "See docs/CONFIG.md."
issue "M5-2: Define IPC service" "M5 - Config & IPC" "ipc" "Commands in docs/DEVELOPMENT.md. Shared interface."
issue "M5-3: Live config edits from overlay" "M5 - Config & IPC" "ipc,config" "Sysmodule persists."
issue "M5-4: Paged list streaming over IPC" "M5 - Config & IPC" "ipc" "Platforms, roms, queue."
# --- M6
issue "M6-1: Install layout + release zip" "M6 - Packaging & CI" "packaging" "sysmodule + overlay drop onto SD."
issue "M6-2: ovl-sysmodules toggle compatibility" "M6 - Packaging & CI" "packaging" ""
issue "M6-3: Versioning + Releases from CI" "M6 - Packaging & CI" "packaging" ""
issue "M6-4: End-user install + pairing guide" "M6 - Packaging & CI" "docs" ""
# --- M7
issue "M7-1: Conflict UX in overlay" "M7 - Polish" "sync,overlay" "List, keep-both surfacing."
issue "M7-2: Backoff + battery-friendly scheduling" "M7 - Polish" "sysmodule" "No sync in sleep; resume on wake/network."
issue "M7-3: Troubleshooting guide" "M7 - Polish" "docs" "401s, folder mismatches, Tico paths."
issue "M7-4: Play-session recording (optional)" "M7 - Polish" "sync" "me.write, /api/play-sessions."
# --- M8  (real hardware, last, gated on a proven v1)
issue "M8-1: v1 gate — do not touch hardware until this passes" "M8 - Hardware bring-up (after proven v1)" "risk" \
  "Checklist gate: sync/downloads/auth/config+IPC all green on host + docker RomM; ssl backend proven in Ryujinx NRO; backups verified by tests; tagged v1 build; NAND/SD backup ready. Nothing in M8 starts until every box is checked."
issue "M8-2: First real-console smoke test (backup SD, non-boot NRO first)" "M8 - Hardware bring-up (after proven v1)" "sysmodule,risk" \
  "On a backup SD/emuMMC: run the manually-launched NRO first, then install the sysmodule disabled, then first sync against a disposable collection with .backup/ populated. Never the production library until all pass."

echo "done."
