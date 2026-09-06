// How long to wait before trying again -- once, for every retry loop in the
// client.
//
// There was already one of these: `auth::PairingSession::BackOff()` doubled the
// server's poll interval per consecutive failure and capped it at
// `PairingConfig::max_poll_backoff`. That policy is right and this file is it,
// lifted out so the scheduler (M7-2, #37) shares it rather than growing a
// second one that drifts.
//
// **Jitter is the thing the lifted version adds.** Several consoles on one LAN
// come back from a router reboot at the same instant, fail at the same instant,
// and -- with a deterministic doubling -- retry at the same instant, forever.
// That is a self-inflicted thundering herd against a RomM that is very likely
// still coming up. A spread of a few seconds costs nothing and breaks the
// lockstep permanently, because the delays never re-converge.
//
// **The jitter goes upward, and the cap bounds the doubling rather than the
// delay.** Both are deliberate, and both are the opposite of the obvious
// reading:
//
//   - Downward jitter subtracted from the floor would put the first retry --
//     the one where every console is in lockstep -- *below* the floor, and
//     clamping it back to the floor is exactly the case where jitter was needed
//     and where it does nothing.
//   - Clamping the jittered delay to the cap would put every console back in
//     lockstep the moment they all saturate, which on a server that is down for
//     an hour is every console. So the ceiling on the *delay* is
//     `cap * (1 + jitter)`, and it is written down here rather than discovered.
//
// Nothing here reads a clock or sleeps. It answers "how long", and the caller --
// which owns a clock and a thread -- decides when that is up. That is what makes
// it testable without waiting anything out, which is the same reason
// `auth::Gate` and `sync::Scheduler` are shaped this way.
//
// `auth::Gate::backoff()` is deliberately **not** this. It paces "may this
// console ask at all", in tens of seconds, over a credential the server has
// stopped accepting; this paces a retry loop inside one flow. They are two
// questions and folding them together would answer neither well (auth_gate.hpp).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace rommsync::retry {

/// The shape of one backoff curve.
struct BackoffConfig {
  /// The first delay, and the value nothing ever goes below.
  ///
  /// A caller whose floor is the server's own answer -- `PairingSession`, whose
  /// floor is the `interval` RomM asked for -- sets this from that answer rather
  /// than from a constant, because undercutting it earns `slow_down`
  /// (docs/AUTH.md).
  std::chrono::milliseconds floor{30'000};

  /// Where the doubling stops. Never below `floor`; a config that says otherwise
  /// is read as `floor`.
  std::chrono::milliseconds cap{300'000};

  /// How much of the delay may be added on top, as a fraction: `0.25` is "up to
  /// a quarter longer". Zero disables jitter and makes the curve exactly the one
  /// `PairingSession` had before this file existed.
  ///
  /// Clamped into `[0, 1]`. Larger than one is not a spread, it is a second
  /// doubling with a worse name.
  double jitter = 0.25;
};

/// The delay a retry loop should wait, and the count behind it.
///
/// Not thread-safe, and deliberately not synchronised, for `auth::Gate`'s
/// reason: every owner of one is a single loop, and a mutex here would suggest
/// two loops may share one curve without saying what the second one should make
/// of the first one's count.
class Backoff {
 public:
  /// Where the jitter comes from: a value in `[0, 1)`.
  ///
  /// Injectable so a test can pin the arithmetic -- a delay that is a random
  /// number is a delay no test can assert on, and the one thing worth asserting
  /// is precisely that the jitter never pushes the result outside its bounds.
  /// Null means this object's own generator, seeded from `steady_clock`.
  using Random = std::function<double()>;

  explicit Backoff(BackoffConfig config = {}, Random random = nullptr);

  /// Replace the curve **without forgetting the failures so far**.
  ///
  /// It exists for the one caller whose floor is not known when the object is
  /// built: `PairingSession` learns its floor from the `interval` the server
  /// answers `init` with. A caller that wants the count cleared too calls
  /// `Succeed()` after this.
  void Configure(const BackoffConfig& config);

  const BackoffConfig& config() const { return config_; }

  /// Record one more consecutive failure and answer how long to wait for it.
  ///
  /// The first call answers `floor` plus jitter, the second twice `floor` plus
  /// jitter, and so on until the doubling saturates at `cap`. It never
  /// overflows: the doubling stops the moment it reaches `cap`, so a loop that
  /// has failed ten thousand times is still answering `cap` plus jitter rather
  /// than a negative duration.
  std::chrono::milliseconds Fail();

  /// Back to the start: no failures, and `delay()` is zero.
  void Succeed();

  /// Consecutive failures since the last `Succeed()`. Saturates rather than
  /// wrapping, for `Fail()`'s reason.
  int failures() const { return failures_; }

  /// What the last `Fail()` answered, or zero when there have been none since
  /// the last `Succeed()`. The caller that has already scheduled its wake-up
  /// does not have to remember the number.
  std::chrono::milliseconds delay() const { return delay_; }

  /// The **undelayed** curve at `failures` consecutive failures: the doubling,
  /// capped, with no jitter on it.
  ///
  /// This is the number the jitter is a fraction of, and it is exposed because
  /// it is the only part of the answer a test can state exactly. `failures <= 0`
  /// is zero.
  std::chrono::milliseconds Base(int failures) const;

 private:
  /// A value in `[0, 1)` from `random_`, or from the built-in generator.
  ///
  /// A source that answers outside the range is clamped rather than trusted: it
  /// is injected, so it is a test's mistake, and a negative one would produce a
  /// delay under the floor -- the one thing this class promises never to do.
  double Roll();

  BackoffConfig config_;
  Random random_;

  /// The built-in generator: xorshift64*, seeded from `steady_clock`.
  ///
  /// Not `<random>`: `std::mt19937` is 2.5 KiB of state per object on a heap
  /// that is 512 KiB in total (sysmodule/AGENTS.md), and `std::random_device` on
  /// devkitA64 is not a source anybody has checked. What this has to be is
  /// *different between two consoles*, and a steady clock read at the moment
  /// they each first failed is already that -- consoles do not boot on the same
  /// nanosecond, and the seed only has to spread a retry, never to be a secret.
  std::uint64_t state_ = 0;

  int failures_ = 0;
  std::chrono::milliseconds delay_{0};
};

}  // namespace rommsync::retry
