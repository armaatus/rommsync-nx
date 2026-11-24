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
#   footprint     the mapped image, the main thread's stack (the npdm's
#                 `main_thread_stack_size`, which the kernel allocates at
#                 launch) and the TLS page, against RESIDENT_FOOTPRINT: the
#                 one number docs/DEVELOPMENT.md and INSTALL quote for what this
#                 process costs the pool (M11-12, #296). It fails both ways --
#                 over the number, and more than RESIDENT_SLACK under it, which
#                 is a number that stopped being a measurement.
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
# was 1,441,792 -- ~272 KiB of headroom, a little over one 256 KiB `state.db`
# term. #246 then raised the heap by 128 KiB (`kInnerHeapSize` 0x180000), and
# M10's code took most of the rest: 2,355,200 bytes at the whole-branch review
# (2026-10-22), 4 KiB under. Dropping the six `snprintf` calls that linked
# newlib's formatter (`switch.builds` now refuses it) gave back 40 KiB of
# .text: 2,310,144 bytes, 48 KiB under, and 2,314,240 after the merges that
# followed. #253's LTO and integer-only JSON numbers (no fast_float) took it to
# 2,215,936: 140 KiB under.
# Growth past it is a decision, made here with its reason beside it, and not
# something a PR does by accident. Atmosphere adds 7 MB to the system pool on
# HOS 21.x and 22.x, shared by every custom sysmodule and its own
# (docs/DEVELOPMENT.md#what-the-image-costs reconciles the two figures once
# quoted), and this process is one tenant of it.
IMAGE_BUDGET=$(( 2304 * 1024 ))
# What the process costs the custom-sysmodule pool, resident, for its whole
# life (M11-12, #296): the mapped image above -- whose `.bss` is the inner heap,
# and the inner heap holds the three thread stacks and the bsd transfer memory
# -- plus the main thread's npdm-sized stack and the TLS page its four threads
# share. `ssl` costs nothing here: its buffers live in the `ssl` sysmodule's own
# process (docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision).
#
# Measured at 2,318,336 bytes when this was set (mapped 2,281,472) and recorded
# 24 KiB over it, so a small change does not have to restate it. Raised one page,
# from 2,342,912, when #340's state budget (+2,432 bytes of text) and #341
# (+~256) together took the text segment past 0x91000 to 0x91100: the
# maintainer accepted the page rather than a second trim (#338). Growth past it
# is the same decision as the image budget's, and moving it is an edit here and
# in the two documents `ctest -R heap.footprint` holds to it. A footprint that
# falls more than RESIDENT_SLACK under it fails too: then the number the docs
# give the M12 co-resident run is no longer what the build costs.
RESIDENT_FOOTPRINT=$(( 2347008 ))
RESIDENT_SLACK=$(( 64 * 1024 ))
# One page: a thread's TLS slot is 0x200 bytes, eight to a page, and the main
# thread, the pairing thread, the worker and the PSC watcher are four.
TLS_BYTES=$(( 4096 ))
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
    --footprint)
      [[ "${2:-}" =~ ^[0-9]+$ ]] || { echo "--footprint takes a byte count" >&2; exit 2; }
      RESIDENT_FOOTPRINT="$2"; shift 2 ;;
    *) echo "usage: $0 [--root DIR] [--image-budget BYTES] [--ovl-budget BYTES] [--footprint BYTES]" >&2; exit 2 ;;
  esac
done

BIN="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin"
SYSMODULE="$ROOT/sysmodule/sys-rommsync.elf"
OVERLAY="$ROOT/overlay/ovl-rommsync.elf"
OVL="$ROOT/overlay/ovl-rommsync.ovl"
MAIN="$ROOT/sysmodule/source/main.cpp"
NPDM="$ROOT/sysmodule/sys-rommsync.json"

failures=0
error() { echo "::error::$*" >&2; failures=$(( failures + 1 )); }

for file in "$SYSMODULE" "$OVERLAY" "$OVL" "$MAIN" "$NPDM"; do
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

# The main thread's stack, as the npdm the .nsp was built from declares it.
main_stack="$(sed -n 's/^[[:space:]]*"main_thread_stack_size":[[:space:]]*"\(0x[0-9A-Fa-f]*\)".*/\1/p' "$NPDM")"
[ -n "$main_stack" ] || { echo "::error::no main_thread_stack_size in $NPDM" >&2; exit 1; }
main_stack=$(( main_stack ))
resident=$(( mapped + main_stack + TLS_BYTES ))

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
if [ "$resident" -gt "$RESIDENT_FOOTPRINT" ]; then
  error "the sysmodule's resident footprint is $resident bytes, past the" \
        "$RESIDENT_FOOTPRINT scripts/image-budget.sh records and the docs quote (#296)"
elif [ $(( RESIDENT_FOOTPRINT - resident )) -gt "$RESIDENT_SLACK" ]; then
  error "the sysmodule's resident footprint is $resident bytes, more than" \
        "$RESIDENT_SLACK under the $RESIDENT_FOOTPRINT recorded; the recorded number" \
        "is stale -- lower it here and in the docs (#296)"
fi
if [ "$bss" -lt "$heap" ]; then
  error "the sysmodule's .bss is $bss bytes and main.cpp declares a $heap-byte" \
        "inner heap; the heap is not in .bss"
elif [ $(( bss - heap )) -gt "$BSS_OTHER" ]; then
  error "the sysmodule's .bss is $bss bytes against a declared heap of $heap;" \
        "$(( bss - heap )) bytes of it is something other than the heap, more than" \
        "the $BSS_OTHER this allows (#207, #204)"
fi
if [ "$eh" -gt "$EH_FRAME_CEILING" ]; then
  error "the sysmodule's .eh_frame is $eh bytes, past the $EH_FRAME_CEILING ceiling (#200)"
fi
if [ "$text" -gt "$TEXT_CEILING" ]; then
  error "the sysmodule's .text is $text bytes, past $TEXT_CEILING -- is switch.mk still -Os? (#200)"
fi
if [ "$ovl_text" -gt "$TEXT_CEILING" ]; then
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
  echo "| main thread stack (npdm) | $main_stack | |"
  echo "| TLS page | $TLS_BYTES | |"
  echo "| **resident footprint** | **$resident** | $RESIDENT_FOOTPRINT |"
  echo "| \`.bss\` | $bss | $(( heap + BSS_OTHER )) |"
  echo "| of which the heap (\`kInnerHeapSize\`) | $heap | |"
  echo "| \`.eh_frame\` | $eh | $EH_FRAME_CEILING |"
  echo "| sysmodule \`.text\` | $text | $TEXT_CEILING |"
  echo "| overlay \`.text\` | $ovl_text | $TEXT_CEILING |"
  echo "| overlay \`.ovl\` (file) | $ovl_bytes | $OVL_BUDGET |"
}
report
# The same numbers as benchmark-action's `customSmallerIsBetter` JSON, for the
# nightly soak to track over time (#250): growth that stays under every ceiling
# above is still growth, and it is visible there before it trips one. An
# environment variable rather than a flag, because the `switch-build` job may
# pass this script none (`switch.ci_requires_artifacts`).
if [ -n "${IMAGE_BUDGET_JSON:-}" ]; then
  {
    echo "["
    echo "  {\"name\": \"sysmodule mapped image\", \"unit\": \"bytes\", \"value\": $mapped, \"extra\": \"budget $IMAGE_BUDGET\"},"
    echo "  {\"name\": \"sysmodule .bss\", \"unit\": \"bytes\", \"value\": $bss, \"extra\": \"heap $heap\"},"
    echo "  {\"name\": \"sysmodule .text\", \"unit\": \"bytes\", \"value\": $text, \"extra\": \"ceiling $TEXT_CEILING\"},"
    echo "  {\"name\": \"overlay .ovl\", \"unit\": \"bytes\", \"value\": $ovl_bytes, \"extra\": \"budget $OVL_BUDGET\"}"
    echo "]"
  } >"$IMAGE_BUDGET_JSON"
fi
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  { echo "### sys-rommsync image budget"; echo; report; } >>"$GITHUB_STEP_SUMMARY"
fi

if [ "$failures" -gt 0 ]; then
  exit 1
fi
echo "ok: the sysmodule maps $mapped of its $IMAGE_BUDGET-byte budget" \
     "($(( IMAGE_BUDGET - mapped )) bytes to spare)"
echo "ok: the sysmodule's resident footprint is $resident of the $RESIDENT_FOOTPRINT recorded"
