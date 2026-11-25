// The overlay's share of nx-ovlloader's heap, held off-console (#337).
//
// nx-ovlloader reads the `.ovl` into a heap that defaults to 4 MiB, maps it
// there with its `.bss`, and hands the overlay what is left as its own heap --
// out of which libnx's framebuffer, libtesla's and libultrahand's state, and
// everything this code allocates all come. Nothing reports that running out:
// the overlay fails as a short read at load (#211), or as an allocation that
// does not return, on a console.
//
// No single test can measure the whole of what is ours -- the image exists only
// inside the devkitPro container, the heap only in a host test -- so each term
// is held by the test that can measure it, against its share of one budget:
//
//   | term                                     | constant             | held by                   |
//   |------------------------------------------|----------------------|---------------------------|
//   | the linked image, paged, with `.bss`     | `kImageBudget`       | `scripts/image-budget.sh` |
//   | the peak of `operator new`, worst screen | `kHeapPeakBudget`    | `overlay.scroll`          |
//   | the IPC worker's stack                   | `kWorkerStackBudget` | `ipc_client.cpp`'s assert |
//
// The worst screen is the Library at row 2000 of a 2500-rom platform: its
// window holds `kMaxLoadedRows` rows, and no other screen holds a list.
//
// What the budget does not count, and docs/DEVELOPMENT.md#what-the-overlay-costs
// says so: the framebuffer (two 448x720 RGBA4444 buffers, ~1.3 MiB, which libnx
// allocates from this heap), libtesla's and libultrahand's own allocations --
// the `tsl::elm` tree a screen builds among them -- and anything allocated with
// `malloc` rather than `new`. Whether the whole fits is
// HARDWARE_TEST_PLAN's MEM-03, on a console.
#pragma once

#include <cstddef>

namespace rommsync::overlay {

/// nx-ovlloader's heap, as it ships (MEM-03 runs at this size).
inline constexpr std::size_t kLoaderHeapBytes = 0x400000;

/// What this build may cost of it: 1.25 MiB. Measured at 956,116 bytes when
/// set (image 647,168, heap peak 292,564, stack 16,384), so ~346 KiB of
/// headroom; the ~2.75 MiB beside it is the framebuffer's and libultrahand's.
/// Moving it is a decision, made here with its reason, and in DEVELOPMENT.md.
inline constexpr std::size_t kFootprintBudget = 0x140000;

/// The peak of what the overlay's code holds through `operator new`, on the
/// worst screen: 384 KiB, against 292,564 bytes measured on a 64-bit macOS
/// host. libc++ keeps a string of up to 22 bytes inline where the console's
/// libstdc++ keeps 15, so a host measurement can understate the console's by
/// the rom names between the two: the headroom is for that first.
inline constexpr std::size_t kHeapPeakBudget = 0x60000;

/// `IpcClient`'s worker stack, which newlib allocates from the same heap.
inline constexpr std::size_t kWorkerStackBudget = 0x4000;

/// What is left for the linked image: 880 KiB, against 647,168 bytes.
inline constexpr std::size_t kImageBudget = 0xDC000;

static_assert(kImageBudget + kHeapPeakBudget + kWorkerStackBudget == kFootprintBudget,
              "the three terms of the overlay's budget add up to it");
static_assert(kFootprintBudget < kLoaderHeapBytes / 2,
              "the framebuffer and libultrahand need the other half of the loader's heap");

}  // namespace rommsync::overlay
