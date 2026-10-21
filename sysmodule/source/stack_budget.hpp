// The main thread's stack, and the deepest JSON document it can be handed
// (M9-18, #216).
//
// The main thread is the one stack this process does not choose in code: the
// npdm sizes it (`main_thread_stack_size` in `sys-rommsync.json`), and it runs
// the two things that parse whatever they are given -- `SdEngine::Load`, which
// reads `queue.json`, `auth.json` and `device.dat` off the card at boot, and
// the IPC loop, which decodes every payload the overlay sends. `json::Parser`
// recurses once per nesting level, so the deepest document `json::kMaxDepth`
// lets through is the deepest stack either of them can reach.
//
// At 0x8000 and a `kMaxDepth` of 64 those did not fit: `-fcallgraph-info`
// over the sysmodule measured the chain at 33,472 bytes against 32,768, and a
// Horizon stack overflow is a data abort with no crash report. `kMaxDepth` is
// 16 now, which costs nothing real -- RomM's deepest typed response nests 7
// deep (`stack.contract`) -- and puts the same chain at ~12.5 KiB.
//
// The worker, pairing and PSC threads do not get this number; their stacks are
// `kThreadStackBytes` and `kWatcherStackBytes` (`sized_thread.hpp`).
//
// **The margin is half the stack.** The figures below are what GCC reports for
// every frame on the deepest *direct* chain, and that is a floor on the truth:
// an indirect call has no target a static walk can follow (`fs::FileSystem`
// and `http::HttpClient` are interfaces), and newlib's and libnx's frames
// count as zero. `scripts/stack-budget.py` says which edges it does resolve.
//
// What holds each number:
//
//   * `stack.fit` (every push): the npdm says `kMainThreadStackBytes`, and the
//     `static_assert` below holds with today's `kMaxDepth`.
//   * `stack.measured` (where devkitPro is): a cross-compile with
//     `-fcallgraph-info=su`, walked from `main`, stays under both ceilings.
//   * `stack.contract` (every push): `kMaxDepth` stays at least twice the
//     deepest shape a pinned RomM snapshot promises, so lowering it again is
//     caught where it would start refusing real responses.
#pragma once

#include <cstddef>

#include "rommsync/json.hpp"

namespace rommsync::sysmodule::stack {

/// `main_thread_stack_size` in `sys-rommsync.json`. One number in two files,
/// and `stack.fit` fails when they disagree.
inline constexpr std::size_t kMainThreadStackBytes = 0x8000;

/// One nesting level: `Parser::ParseValue` plus the heavier of `ParseObject`
/// and `ParseArray`. Measured at 432 bytes (144 + 288) in
/// `devkitpro/devkita64:20260219` at `-Os`; #216 measured 496 before #201 and
/// #204 moved the build, which is why the ceiling is not the measurement.
inline constexpr std::size_t kJsonLevelBytes = 512;

/// Everything on the deepest parsing chain that does not repeat per level:
/// the frames from `main` down to `json::Parse`, and the deepest tail below the
/// last level (`ParseString` -> `ParseUnicodeEscape` -> `ParseHex4` and a
/// string allocation). Measured at 5,392 bytes, through `main` (1,952) ->
/// the IPC loop -> `ipc::Dispatch` (400) -> an `SdEngine` handler ->
/// `auth::LoadOrCreateDeviceIdentity` -> `json::Parse` (320).
inline constexpr std::size_t kMainRestBytes = 0x1800;

/// The deepest document the parser accepts costs `kMaxDepth + 1` levels: one
/// per nesting it lets through, and the frame that refuses the next.
inline constexpr std::size_t kDeepestParseBytes =
    (static_cast<std::size_t>(json::kMaxDepth) + 1) * kJsonLevelBytes + kMainRestBytes;

static_assert(kMainThreadStackBytes % 0x1000 == 0,
              "npdmtool wants the main thread's stack page-aligned");
static_assert(kDeepestParseBytes <= kMainThreadStackBytes / 2,
              "the deepest JSON document json::kMaxDepth accepts no longer fits in half the "
              "main thread's stack; the other half is the margin for what no static walk sees "
              "(#216)");

}  // namespace rommsync::sysmodule::stack
