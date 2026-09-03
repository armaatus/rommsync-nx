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
# --- M0
issue "M0-1: Prototype HTTPS from a sysmodule via the ssl service" "M0 - Foundations & de-risking" "risk,sysmodule" \
  "Spike: TLS GET to RomM /openapi.json from a minimal sysmodule using the Horizon ssl service (+bsd/socket). Measure heap. Go/no-go vs fallbacks. BLOCKS all networked work. See docs/DEVELOPMENT.md."
issue "M0-2: Define the HttpClient interface" "M0 - Foundations & de-risking" "docs,sysmodule" \
  "Interface for GET/POST, multipart upload, streaming download-to-file, timeouts, cancellation. TLS backend must be swappable."
issue "M0-3: Repo skeleton builds in CI" "M0 - Foundations & de-risking" "packaging" \
  "devkitpro docker image; empty sysmodule + overlay compile and emit artifacts. Wire .github/workflows/ci.yml."
issue "M0-4: Confirm auth/sync response shapes against live RomM" "M0 - Foundations & de-risking" "server,docs" \
  "Run server/probe_contract.py --auth --negotiate; paste real init/token/negotiate JSON into docs/API_CONTRACT.md and AUTH.md."
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

echo "done."
