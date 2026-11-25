// The name lookups in flight, and the time each must have answered by (#335).
//
// A lookup on the console is one IPC to `sfdnsres` that returns when the
// resolver does: nothing on the calling thread can shorten it, and libnx gives
// it no timeout. What it does give is a cancel (`resolverCancel`), which #290
// armed on the request's `CancelToken` so a sleep could end the lookup. A
// lookup nobody cancelled still had no deadline: a resolver that never answers
// held the tick for as long as the console stayed awake.
//
// So a lookup is cancelled at its deadline by another thread, through the same
// `CancelToken::Abort` a sleep fires. That thread is not a new one -- the inner
// heap has 4 KiB to spare and a thread costs 12 (`main.cpp`'s table) -- but the
// PSC watcher, which spends its life parked in a `waitMulti` that can be given a
// timeout and one more event (`power_psc.cpp`). It is the *keeper*: it waits
// for as long as `Fire` says, and `Arm` wakes it to look again.
//
// **A cancel sent once may be lost.** The token is read before the IPC goes
// out, and a cancel that lands after that read but before `sfdnsres` has the
// request names a handle it does not know yet; nothing says it remembers one.
// So a cancelled lookup is cancelled again every `kLookupResend` until it
// returns, `kLookupResends` times: the second lands on a request `sfdnsres`
// holds. Whoever is awake re-sends -- the keeper, and the quiescing thread while
// it waits for the worker (`SdEngine::Quiesce`), which is the same thread on the
// console and is not parked in its `waitMulti` while it quiesces.
//
// Names no libnx type, so the host build compiles it and `sleep.*` drives it.
// Header-only because `engine.cpp` reaches it too, from nine test targets.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>

#include "rommsync/http.hpp"

namespace rommsync::sysmodule {

/// How often a cancelled lookup that has not returned is cancelled again: the
/// slice every other phase of an open reads its token at (`kOpenSlice`).
inline constexpr std::chrono::milliseconds kLookupResend{100};

/// ...and how many times, so a resolver that ignores every cancel costs the
/// keeper three seconds of waking -- `kQuiesceBudget` -- rather than a wake
/// every slice for the rest of the boot, through a sleep.
inline constexpr int kLookupResends = 30;

class LookupWatch {
 public:
  using Clock = std::chrono::steady_clock;

  /// Lookups that can be watched at once: one per thread that opens
  /// connections -- the worker, the pairing thread, the IPC thread -- and one
  /// spare. A lookup past them is unwatched, which is what every lookup was
  /// before #335.
  static constexpr std::size_t kSlots = 4;

  /// Watch `token` until `Disarm`: cancel it at `deadline`, and cancel it again
  /// while it has been cancelled and has not been disarmed. -1 when every slot
  /// is taken. `token` must outlive the `Disarm`.
  int Arm(http::CancelToken* token, Clock::time_point deadline) {
    (void)token;
    (void)deadline;
    return -1;
  }

  /// Stop watching. Once this returns, no `Fire` is still touching the token.
  /// True when it was the deadline that cancelled it.
  bool Disarm(int slot) {
    (void)slot;
    return false;
  }

  /// Cancel every lookup past its deadline, and re-send the cancel of every
  /// cancelled one that is due. How long until this is next owed a call, or
  /// nothing when no lookup needs it -- the keeper then waits for `Arm`'s wake
  /// alone, and costs nothing.
  std::optional<Clock::duration> Fire(Clock::time_point now) {
    (void)now;
    return std::nullopt;
  }

  /// `wake(context)` on every `Arm`, so a keeper waiting on `Fire`'s last
  /// answer looks again. One keeper; a null `wake` leaves none, and deadlines
  /// are then kept only while somebody quiesces.
  void Keep(void (*wake)(void*), void* context) {
    (void)wake;
    (void)context;
  }
};

/// The process's lookups. One, because the keeper is one thread.
inline LookupWatch& Lookups() {
  static LookupWatch watch;
  return watch;
}

}  // namespace rommsync::sysmodule
