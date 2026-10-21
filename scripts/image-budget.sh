#!/usr/bin/env bash
# The sysmodule's memory budget, measured off the linked image (M9-15, #204).
#
#   scripts/image-budget.sh [--root DIR] [--image-budget BYTES] [--ovl-budget BYTES]
#
# Run inside devkitpro/devkita64, after `make -C sysmodule` and `make -C overlay`:
# the `switch-build` CI job runs it over its checkout, and `ctest -R
# switch.builds` runs it over the copy it built. It reads the ELFs in place and
# builds nothing -- so over a worktree that has built before, `make clean`
# first: make tracks no flags, and objects left from an older switch.mk link
# into an image these ceilings were never set for (a stale tree measured an
# .eh_frame of 33,632 bytes where a clean one has 27,288).
#
# Why the image and not the source: the heap is a `.bss` array
# (`g_inner_heap`, sysmodule/source/main.cpp), so the toolchain's own
# section sizes are the exact number the loader will map -- the heap included.
# `ctest -R heap` checks main.cpp's table against main.cpp's constants; this
# checks the constants against what was actually linked, and the whole against
# a budget nobody can move without editing the line below that states it.
#
# What it holds, each a failure on its own:
#
#   mapped image  page_align(text) + page_align(rodata) + page_align(data+bss),
#                 the three LOAD segments an NSO maps, under IMAGE_BUDGET. This
#                 is the number Atmosphere's sysmodule pool pays.
#   .bss tie      `.bss` holds `kInnerHeapSize` (read out of main.cpp) and at
#                 most BSS_OTHER besides it: a heap that grew without the
#                 constant moving, and a constant that moved without the heap,
#                 both land here (M9-2, #207).
#   .eh_frame     under EH_FRAME_CEILING in the sysmodule (#200).
#   .text         under TEXT_CEILING in both targets: a revert to -O2 overshoots
#                 in both (#200).
#   .ovl          the overlay's file, under OVL_BUDGET (#211). Not the
#                 sysmodule's pool at all: nx-ovlloader reads the whole file
#                 into its own heap, and see the ceiling below for why that
#                 needs one.
#
# `--image-budget` and `--ovl-budget` exist so a test can show each ceiling
# bites. The CI step passes no flag, and `ctest -R switch.ci_requires_artifacts`
# refuses one.
set -euo pipefail

# The budget for the whole mapped image: 2.25 MiB.
#
# Measured at 2,080,768 bytes (1.98 MiB) when this was set, of which the heap
# is 1,441,792 -- so this is ~272 KiB of headroom, a little over one 256 KiB
# `state.db` term. Growth past it is a decision, made here with its reason
# beside it, and not something a PR does by accident. Atmosphere leaves third-
# party sysmodules a shared pool (7 MB on HOS 21.0.0+, docs/DEVELOPMENT.md#what-the-image-costs),
# and this process is one tenant of it.
IMAGE_BUDGET=$(( 2304 * 1024 ))
# What `.bss` may hold besides the heap: every other file-scope object.
# ~17 KiB today (libnx's and newlib's state, the log ring's bookkeeping).
BSS_OTHER=$(( 64 * 1024 ))
EH_FRAME_CEILING=$(( 32 * 1024 ))
TEXT_CEILING=$(( 640 * 1024 ))

# The overlay's .ovl, as a file: 1 MiB.
#
# nx-ovlloader loads an overlay by reading the whole file into a heap that
# defaults to 4 MB, and that heap is also everything the overlay allocates while
# it runs -- libultrahand's font, theme and list state among it. Its loadNro()
# still carries "todo: Detect whether NRO fits into heap or not", so an overlay
# that has outgrown it does not fail with a size: it fails as a short read
# (2347-0004) that looks like the loader's fault. Measured at 647,228 bytes
# when this was set (after #200's -Os); 1 MiB leaves ~390 KiB of growth, and
# going past it is a decision made here, with its reason beside it.
OVL_BUDGET=$(( 1024 * 1024 ))

ROOT=.
while [ $# -gt 0 ]; do
  case "$1" in
    --root) ROOT="$2"; shift 2 ;;
    --image-budget)
      # Checked, because a non-number makes the `-gt` below an error inside an
      # `if`, which is false: the ceiling would pass without being applied.
      [[ "${2:-}" =~ ^[0-9]+$ ]] || { echo "--image-budget takes a byte count" >&2; exit 2; }
      IMAGE_BUDGET="$2"; shift 2 ;;
    --ovl-budget)
      [[ "${2:-}" =~ ^[0-9]+$ ]] || { echo "--ovl-budget takes a byte count" >&2; exit 2; }
      OVL_BUDGET="$2"; shift 2 ;;
    *) echo "usage: $0 [--root DIR] [--image-budget BYTES] [--ovl-budget BYTES]" >&2; exit 2 ;;
  esac
done

BIN="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin"
SYSMODULE="$ROOT/sysmodule/sys-rommsync.elf"
OVERLAY="$ROOT/overlay/ovl-rommsync.elf"
OVL="$ROOT/overlay/ovl-rommsync.ovl"
MAIN="$ROOT/sysmodule/source/main.cpp"

failures=0
error() { echo "::error::$*" >&2; failures=$(( failures + 1 )); }

for file in "$SYSMODULE" "$OVERLAY" "$OVL" "$MAIN"; do
  [ -s "$file" ] || { echo "::error::no $file; build sysmodule and overlay first" >&2; exit 1; }
done
[ -x "$BIN/aarch64-none-elf-size" ] ||
  { echo "::error::no devkitA64 at $BIN; run this inside devkitpro/devkita64" >&2; exit 1; }

# section ELF NAME -- the size of section NAME, by the toolchain's own `size -A`.
section() {
  "$BIN/aarch64-none-elf-size" -A "$1" | awk -v want="$2" '$1 == want { print $2 }'
}

# The LOAD segments' in-memory sizes, each rounded up to the 4 KiB page the
# loader maps it in. Exactly three, because an NSO has exactly three segments;
# a linker script that produced another shape is one this sum no longer models.
segments="$("$BIN/aarch64-none-elf-readelf" -lW "$SYSMODULE" | awk '$1 == "LOAD" { print $6 }')"
count="$(grep -c . <<<"$segments" || true)"
if [ "$count" -ne 3 ]; then
  echo "::error::$SYSMODULE has $count LOAD segments, not the three an NSO maps" >&2
  exit 1
fi
mapped=0
names=(text rodata data+bss)
i=0
declare -a pages
while read -r memsz; do
  bytes=$(( memsz ))
  pages[i]=$(( (bytes + 0xFFF) & ~0xFFF ))
  mapped=$(( mapped + pages[i] ))
  i=$(( i + 1 ))
done <<<"$segments"

heap="$(sed -n 's/^constexpr size_t kInnerHeapSize = \(0x[0-9A-Fa-f]*\);.*/\1/p' "$MAIN")"
[ -n "$heap" ] || { echo "::error::no kInnerHeapSize in $MAIN" >&2; exit 1; }
heap=$(( heap ))

bss="$(section "$SYSMODULE" .bss)"
eh="$(section "$SYSMODULE" .eh_frame)"
text="$(section "$SYSMODULE" .text)"
ovl_text="$(section "$OVERLAY" .text)"
ovl_bytes="$(wc -c <"$OVL" | tr -d '[:space:]')"
for value in bss eh text ovl_text; do
  [ -n "${!value}" ] || { echo "::error::no $value section in the linked image" >&2; exit 1; }
done

if [ "$mapped" -gt "$IMAGE_BUDGET" ]; then
  error "the sysmodule maps $mapped bytes, past its $IMAGE_BUDGET-byte budget" \
        "(scripts/image-budget.sh, #204)"
fi
if [ "$bss" -lt "$heap" ]; then
  error "the sysmodule's .bss is $bss bytes and main.cpp declares a $heap-byte" \
        "inner heap; the heap is not in .bss"
elif [ $(( bss - heap )) -gt "$BSS_OTHER" ]; then
  error "the sysmodule's .bss is $bss bytes against a declared heap of $heap;" \
        "$(( bss - heap )) bytes of it is something other than the heap, more than" \
        "the $BSS_OTHER this allows (#207, #204)"
fi
if [ "$eh" -ge "$EH_FRAME_CEILING" ]; then
  error "the sysmodule's .eh_frame is $eh bytes, past the $EH_FRAME_CEILING ceiling (#200)"
fi
if [ "$text" -ge "$TEXT_CEILING" ]; then
  error "the sysmodule's .text is $text bytes, past $TEXT_CEILING -- is switch.mk still -Os? (#200)"
fi
if [ "$ovl_text" -ge "$TEXT_CEILING" ]; then
  error "the overlay's .text is $ovl_text bytes, past $TEXT_CEILING -- is switch.mk still -Os? (#200)"
fi
if [ "$ovl_bytes" -gt "$OVL_BUDGET" ]; then
  error "ovl-rommsync.ovl is $ovl_bytes bytes, past the $OVL_BUDGET nx-ovlloader's" \
        "heap is budgeted to read (scripts/image-budget.sh, #211)"
fi

# Printed whether or not it passed, so the number is in every build's log and a
# change in it is visible on the PR that made it, not only when it fails.
report() {
  echo "| | bytes | ceiling |"
  echo "|---|---:|---:|"
  for i in 0 1 2; do
    echo "| segment ${names[i]} (paged) | ${pages[i]} | |"
  done
  echo "| **mapped image** | **$mapped** | $IMAGE_BUDGET |"
  echo "| \`.bss\` | $bss | $(( heap + BSS_OTHER )) |"
  echo "| of which the heap (\`kInnerHeapSize\`) | $heap | |"
  echo "| \`.eh_frame\` | $eh | $EH_FRAME_CEILING |"
  echo "| sysmodule \`.text\` | $text | $TEXT_CEILING |"
  echo "| overlay \`.text\` | $ovl_text | $TEXT_CEILING |"
  echo "| overlay \`.ovl\` (file) | $ovl_bytes | $OVL_BUDGET |"
}
report
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  { echo "### sys-rommsync image budget"; echo; report; } >>"$GITHUB_STEP_SUMMARY"
fi

if [ "$failures" -gt 0 ]; then
  exit 1
fi
echo "ok: the sysmodule maps $mapped of its $IMAGE_BUDGET-byte budget" \
     "($(( IMAGE_BUDGET - mapped )) bytes to spare)"
