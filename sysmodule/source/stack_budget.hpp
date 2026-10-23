// The main thread's stack, and the deepest JSON document it can be handed
// (M9-18, #216).
//
// The main thread is the one stack this process does not choose in code: the
// npdm sizes it (`main_thread_stack_size` in `sys-rommsync.json`), and it runs
// the two things that parse whatever they are given -- `SdEngine::Load`, which
// reads `queue.json` and `auth.json` off the card at boot, and the IPC loop,
// which decodes every payload the overlay sends and runs the handlers it
// names. `json::Parser` recurses once per nesting level, so the deepest
// document `json::kMaxDepth` lets through is the deepest stack either reaches.
//
// At 0x8000 and a `kMaxDepth` of 64 those did not fit: `-fcallgraph-info`
// over the sysmodule measured the chain at 33,472 bytes against 32,768, and a
// Horizon stack overflow is a data abort with no crash report. `kMaxDepth` is
// 16 now, which costs nothing real -- RomM's deepest typed response nests 7
// deep (`stack.contract`) -- and puts the same chain at ~12.5 KiB.
//
// The worker, pairing and PSC threads do not get this number; their stacks are
// `kThreadStackBytes` and `kWatcherStackBytes` (`sized_thread.hpp`). They are
// held the same way, from their own `SizedThread::Enter` (M9-31, #268): the
// worker and pairing threads parse too, so each has a rest ceiling at the
// bottom of this file and the same half-the-stack rule; the PSC watcher parses
// nothing, which `stack.measured` asserts, and its whole deepest chain is held
// to half its stack there.
//
// **The margin is half the stack.** The figures below are what GCC reports for
// every frame on the deepest *direct* chain -- the worker's and pairing
// thread's ceilings are recorded without them, for the reason `sized_thread.hpp`
// gives -- and that is a floor on the truth:
// an indirect call has no target a static walk can follow (`fs::FileSystem`
// and `http::HttpClient` are interfaces), and newlib's and libnx's frames
// count as zero. `scripts/stack-budget.py` says which edges it does resolve.
//
// What holds each number:
//
//   * `stack.fit` (every push): the npdm says `kMainThreadStackBytes`, the
//     `static_assert`s below hold with today's `kMaxDepth`, and nothing in the
//     tree starts a thread except through `SizedThread`.
//   * `stack.measured` (every push, in the `switch-build` job): a
//     cross-compile with `-fcallgraph-info=su`, walked from `main` and from
//     each `SizedThread::Enter`, stays under every ceiling here and inside
//     half of each thread's stack.
//   * `stack.contract` (every push): `kMaxDepth` stays at least twice the
//     deepest shape a pinned RomM snapshot promises, so lowering it again is
//     caught where it would start refusing real responses.
#pragma once

#include <cstddef>

#include "rommsync/json.hpp"
#include "sized_thread.hpp"

namespace rommsync::sysmodule::stack {

/// `main_thread_stack_size` in `sys-rommsync.json`. One number in two files,
/// and `stack.fit` fails when they disagree.
inline constexpr std::size_t kMainThreadStackBytes = 0x8000;

/// One nesting level: `Parser::ParseValue` plus the heavier of `ParseObject`
/// and `ParseArray`. Measured at 288 bytes under LTO (#253) in
/// `devkitpro/devkita64:20260219` at `-Os`, where the level is one
/// `ParseValue` with its callees inlined; 432 (144 + 288) without LTO, and
/// #216 measured 496 before #201 and #204 moved the build. The ceiling still
/// holds the build without LTO, so turning it off is not a stack question.
inline constexpr std::size_t kJsonLevelBytes = 448;

/// Everything on the deepest parsing chain that does not repeat per level:
/// the frames from `main` down to the parser, and the deepest tail below the
/// last level. Measured at 6,880 bytes under LTO (#253), through `main`
/// (2,608) -> `SdEngine::Load` (2,320) -> `download::ParseQueue` (704) ->
/// `json::ParseEach` (640) -> `Parser::ParseEach` (256), with a string
/// append below the last level: cross-unit inlining folds callees' frames
/// into `main`'s and `Load`'s, which is the cost #253 named. 5,488 without
/// LTO, through the IPC loop -> `ipc::Dispatch` -> an `SdEngine` handler.
inline constexpr std::size_t kMainRestBytes = 0x2000;

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

/// What `kMainRestBytes` is for the main thread, for the worker: everything on
/// its deepest parsing chain beside the levels, from
/// `SizedThread::Enter<&SdEngine::RunWorker>` down through the tick to
/// whichever response it parses (#268). A ceiling, not a measurement --
/// `stack.measured` prints the figure it holds this above, and a quoted figure
/// here would be one more number to go stale.
inline constexpr std::size_t kWorkerRestBytes = 0x5000;

/// The same for the pairing thread, from
/// `SizedThread::Enter<&SdEngine::DrivePairing>` through
/// `auth::PairingSession` to the pairing endpoints' responses (#268).
inline constexpr std::size_t kPairingRestBytes = 0x1000;

static_assert((static_cast<std::size_t>(json::kMaxDepth) + 1) * kJsonLevelBytes +
                      kWorkerRestBytes <=
                  kThreadStackBytes / 2,
              "the deepest JSON document no longer fits in half the worker's stack (#268)");
static_assert((static_cast<std::size_t>(json::kMaxDepth) + 1) * kJsonLevelBytes +
                      kPairingRestBytes <=
                  kThreadStackBytes / 2,
              "the deepest JSON document no longer fits in half the pairing thread's stack "
              "(#268)");

}  // namespace rommsync::sysmodule::stack
