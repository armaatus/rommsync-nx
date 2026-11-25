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

#include <algorithm>
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
/// keeper three seconds of waking rather than a wake every slice for the rest
/// of the boot, through a sleep. Three seconds is `kQuiesceBudget`, which
/// `engine.cpp` holds this to: a sleep's quiesce re-sends for no longer.
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
    std::lock_guard<std::mutex> held(mutex_);
    for (std::size_t i = 0; i < kSlots; ++i) {
      Slot& slot = slots_[i];
      if (slot.token != nullptr) continue;
      slot = Slot{};
      slot.token = token;
      slot.deadline = deadline;
      // Under the lock, so a `Keep(nullptr)` that has returned is never woken.
      if (wake_ != nullptr) wake_(wake_context_);
      return static_cast<int>(i);
    }
    return -1;
  }

  /// Stop watching. Once this returns, no `Fire` is still touching the token.
  /// True when it was the deadline that cancelled it.
  bool Disarm(int slot) {
    if (slot < 0 || static_cast<std::size_t>(slot) >= kSlots) return false;
    std::lock_guard<std::mutex> held(mutex_);
    const bool expired = slots_[static_cast<std::size_t>(slot)].expired;
    slots_[static_cast<std::size_t>(slot)] = Slot{};
    return expired;
  }

  /// Cancel every lookup past its deadline, and re-send the cancel of every
  /// cancelled one that is due. How long until this is next owed a call, or
  /// nothing when no lookup needs it -- the keeper then waits for `Arm`'s wake
  /// alone, and costs nothing.
  ///
  /// `Cancel` runs here under this lock, and the token's `Abort` under the
  /// token's: so this is called holding no lock an `Abort` takes, as any
  /// `Cancel` is.
  std::optional<Clock::duration> Fire(Clock::time_point now) {
    std::lock_guard<std::mutex> held(mutex_);
    std::optional<Clock::time_point> next;
    const auto owed = [&next](Clock::time_point at) {
      next = next.has_value() ? std::min(*next, at) : at;
    };
    for (Slot& slot : slots_) {
      if (slot.token == nullptr) continue;
      if (!slot.token->canceled()) {
        if (now < slot.deadline) {
          owed(slot.deadline);
          continue;
        }
        slot.expired = true;
        slot.token->Cancel();
        slot.resend_at = now + kLookupResend;
      } else if (!slot.resend_at.has_value()) {
        // Cancelled by somebody else -- a sleep -- a moment ago. That cancel
        // went out with it; the first re-send is a slice behind it.
        slot.resend_at = now + kLookupResend;
      } else if (slot.resends < kLookupResends && now >= *slot.resend_at) {
        ++slot.resends;
        slot.token->Cancel();
        slot.resend_at = now + kLookupResend;
      }
      if (slot.resends < kLookupResends) owed(*slot.resend_at);
    }
    if (!next.has_value()) return std::nullopt;
    return *next > now ? *next - now : Clock::duration::zero();
  }

  /// `wake(context)` on every `Arm`, so a keeper waiting on `Fire`'s last
  /// answer looks again. One keeper; a null `wake` leaves none, and deadlines
  /// are then kept only while somebody quiesces.
  void Keep(void (*wake)(void*), void* context) {
    std::lock_guard<std::mutex> held(mutex_);
    wake_ = wake;
    wake_context_ = context;
  }

 private:
  struct Slot {
    http::CancelToken* token = nullptr;
    Clock::time_point deadline{};
    /// When the cancel is next sent again; empty until it has been sent once.
    std::optional<Clock::time_point> resend_at;
    int resends = 0;
    bool expired = false;
  };

  std::mutex mutex_;
  std::array<Slot, kSlots> slots_{};
  void (*wake_)(void*) = nullptr;
  void* wake_context_ = nullptr;
};

/// The process's lookups. One, because the keeper is one thread.
inline LookupWatch& Lookups() {
  static LookupWatch watch;
  return watch;
}

}  // namespace rommsync::sysmodule
