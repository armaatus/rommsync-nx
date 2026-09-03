#!/usr/bin/env bash
# Seed this worktree's RomM library from the shared ROM cache.
#
# Real ROMs come from server/testing/roms.manifest -- homebrew and freely
# redistributable only, checksum-pinned, downloaded once into the shared cache
# ($ROM_CACHE, see scripts/orca/env.sh) and reused by every worktree. Never add
# a commercial ROM: this script runs in public CI.
#
# Two fixtures are generated instead of downloaded, because no real homebrew rom
# exercises them -- a large file for Range resume (M3-3) and a multi-file rom
# directory for the has_multiple_files skip (M3-4). See make_fixtures.py.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

[ -f .env ] || ./scripts/orca/env.sh
set -a; . ./.env; set +a

MANIFEST="server/testing/roms.manifest"
LIBRARY="server/testing/library"
LARGE_MB="${SEED_LARGE_MB:-120}"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

mkdir -p "$ROM_CACHE"

# --- real roms: fetch into the shared cache, verify, then stage -------------
while IFS=$'\t' read -r want platform name url; do
  case "$want" in ''|\#*) continue ;; esac
  cached="$ROM_CACHE/$name"
  if [ ! -f "$cached" ] || [ "$(sha256 "$cached")" != "$want" ]; then
    echo "  fetching $name"
    curl -sSLf -o "$cached.tmp" "$url"
    got="$(sha256 "$cached.tmp")"
    if [ "$got" != "$want" ]; then
      rm -f "$cached.tmp"
      echo "checksum mismatch for $name: want $want, got $got" >&2
      exit 1
    fi
    mv "$cached.tmp" "$cached"
  fi
  mkdir -p "$LIBRARY/roms/$platform"
  cp -f "$cached" "$LIBRARY/roms/$platform/$name"
done < "$MANIFEST"

# --- generated fixtures ----------------------------------------------------
large="$ROM_CACHE/synthetic-large-${LARGE_MB}mb.bin"
multi="$LIBRARY/roms/psx/Synthetic Two Disc Game"
if [ ! -f "$large" ]; then
  echo "  generating ${LARGE_MB}MiB synthetic rom (Range-resume fixture)"
fi
python3 server/testing/make_fixtures.py "$large" "$LARGE_MB" "$multi" >/dev/null
mkdir -p "$LIBRARY/roms/gba"
cp -f "$large" "$LIBRARY/roms/gba/synthetic-large.gba"

echo "seeded $LIBRARY"
find "$LIBRARY" -type f -print0 | while IFS= read -r -d '' f; do
  printf '  %8s  %s\n' "$(du -h "$f" | cut -f1)" "${f#"$LIBRARY"/}"
done
