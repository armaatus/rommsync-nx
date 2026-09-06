// When a sync tick runs, and nothing about what one does.
//
// The triggers are fixed by docs/ARCHITECTURE.md §1 -- boot once the network is
// up, a configurable interval, and an explicit request from the overlay -- and
// this is the one object that owns all three. `sync::RunTick` is the tick
// (sync_tick.hpp); this decides when to call it and how long to wait after one
// that did not work.
//
// Nothing here makes a request, reads a file or sleeps. It is asked "what now"
// and told "here is how that went", which is what makes every case below
// checkable against an injected clock rather than by waiting out an interval.
// The thread, the condition variable and the `http::CancelToken` belong to the
// caller -- on a console, `sysmodule::SdEngine`'s worker.
//
// ## The three things this file is really about
//
// **Idle cost.** A sysmodule that wakes up every thirty seconds to decide it has
// nothing to do is a battery cost with nothing to show for it, and a console
// spends most of its life either asleep or with `[sync] enabled` off. So the
// answer to "nothing to do" is `Decision::parked` -- sleep until something wakes
// you, with no deadline at all -- and not a short poll. A parked scheduler is
// woken by `RequestNow()`, by `Reconfigure()`, or by the caller's own shutdown,
// and by nothing else.
//
// **Suspend.** The console suspends and `steady_clock` does not advance across
// it, so a monotonic timer that was half way through a thirty-minute interval
// wakes up still half way through it, however many hours later. The rule is
// therefore stated on the **wall clock**: an interval has elapsed when the wall
// clock says it has, whatever the monotonic one thinks. A suspend of eleven
// hours is one interval elapsed, not twenty-two -- `Poll()` fires **once** and
// the missed ticks are missed, which docs/SYNC_PROTOCOL.md's failure & safety
// rules already say is harmless. A burst of them would not be.
//
// **A clock that lies.** The wall clock is the console's, so it can be unset,
// and it can be corrected by hours in either direction. A backwards jump must
// not produce a negative interval, and a forwards one must not produce a tick
// storm. Both are handled by taking the *later* of the two clocks' answers:
// `steady_clock` is a lower bound that can never go backwards, and the wall
// clock only ever adds the time a suspend hid. Neither of them ever reaches a
// save's timestamp -- SYNC_PROTOCOL.md refuses an epoch `updated_at`, and the
// only clock a save is stamped from is `ExecuteOptions::now`.
//
// ## What this deliberately does not decide
//
// **`[sync] enabled` is not re-decided here.** `sync::TickOptions::enabled` is
// the gate one level down (M6-2, #33), and the scheduler passes
// `config.sync.enabled` into it rather than growing a second switch. What the
// scheduler owns is the *other* half of that promise: a disabled scheduler never
// fires at all, because a timer that woke up every interval to be refused would
// still be the wakeups the idle-cost rule forbids.
//
// **A 401 is not classified here.** `auth::Gate` owns what a rejected call means
// over time and how long to wait while the credentials are merely suspect
// (auth_gate.hpp, M1-4). `TickOutcome::kUnauthorized` is passed straight through
// as "not my pacing to decide" -- see `Finished`.
//
// **`http::Error` is not classified a second time.** `sync::TickOutcome` is
// already the classification, split on exactly what a scheduler would do
// (sync_tick.hpp). The one bit it drops -- and the one this file asks for
// separately -- is whether a failure to reach the server was a *TLS* failure,
// because that one does not fix itself. See `SchedulerConfig::max_tls_attempts`.
#pragma once

#include <chrono>
#include <functional>

#include "rommsync/backoff.hpp"
#include "rommsync/http.hpp"
#include "rommsync/sync_tick.hpp"

namespace rommsync::sync {

/// Why a tick is being run.
///
/// Carried out of `Poll()` because the three are not interchangeable to the
/// caller: a boot tick happens once, an on-demand one is a user waiting at a
/// screen, and an interval one is nobody waiting at all.
enum class Trigger {
  /// Nothing is due. Read `Decision` for what to do instead.
  kNone,

  /// The first tick after start, once the network is up. `[sync] on_boot`.
  kBoot,

  /// `[sync] interval_min` has elapsed since the last completed tick -- or a
  /// backoff has, which is the same trigger deliberately: a retry is the
  /// schedule catching up, not a fourth kind of tick.
  kInterval,

  /// The overlay's "Sync now" (`ipc::Engine::RequestSync`), or the caller
  /// telling the scheduler that the last tick has to be run again.
  kOnDemand,
};

/// Stable, log-friendly name. Never null.
const char* ToString(Trigger trigger);

/// How patient the schedule is, and what parks it.
struct SchedulerConfig {
  /// `[sync] enabled`. False parks the scheduler outright: no boot tick, no
  /// interval, and an on-demand request is dropped rather than queued.
  ///
  /// Dropped rather than queued because the console the user switched off is
  /// not one that should sync the instant it is switched back on -- and because
  /// `ipc::ServiceCore::SyncNow` already answers `kDisabled` in front of this,
  /// so a request reaching a disabled scheduler is a second caller and not the
  /// overlay (M6-2, #33).
  bool enabled = true;

  /// `[sync] on_boot`. False means the first tick is one interval away rather
  /// than immediate.
  bool on_boot = true;

  /// `[sync] interval_min`, as a duration. **Zero is boot-and-on-demand only**,
  /// which is the documented meaning of `0` and not a degenerate case
  /// (config.hpp). The caller clamps it to
  /// `config::kMinIntervalMinutes`..`kMaxIntervalMinutes`; this clamps a
  /// negative one to zero rather than trusting it, because a negative interval
  /// is a tick storm.
  std::chrono::minutes interval{30};

  /// The retry curve after a tick that did not complete. The floor is what an
  /// offline console retries at, so it is measured in tens of seconds rather
  /// than in the seconds `sync::CallPolicy` uses inside one call: this is "when
  /// may the whole tick be attempted again", and it runs on a battery.
  retry::BackoffConfig backoff{std::chrono::milliseconds{30'000},
                               std::chrono::milliseconds{1'800'000}, 0.25};

  /// How many consecutive TLS failures before the interval is parked.
  ///
  /// **A TLS failure is a configuration fault, not a blip.** A certificate the
  /// console does not trust, a hostname that does not match, a proxy doing
  /// TLS interception -- none of them come right on their own, and a client
  /// that re-handshakes every thirty seconds forever is burning a battery on an
  /// answer it already has. So after this many the schedule parks, and what
  /// restarts it is a person: an explicit "Sync now", or a configuration change
  /// (`Reconfigure`), which is what changing `server.url` or installing a
  /// certificate looks like from here.
  ///
  /// It is deliberately *not* a `Block` in `auth::Gate`: nothing is wrong with
  /// the credentials, and sending the user to a pairing screen over a
  /// certificate would be the wrong sentence entirely.
  int max_tls_attempts = 3;

  /// How many times in a row a tick may answer `TickOutcome::kRescanNeeded`
  /// before it is treated as a failure instead.
  ///
  /// A rescan is not a failure: the sweep put a save back that the scan could
  /// not have seen, so the caller scans again and runs another tick, and the
  /// second one finds nothing to restore (sync_tick.hpp). Two is one more than
  /// that ever needs. The bound exists because "run again immediately" with no
  /// ceiling is the one shape in this file that could spin.
  int max_rescans = 2;
};

/// What the worker should do, right now.
///
/// Exactly one of the three is meaningful: a `trigger` other than `kNone` means
/// run a tick; `parked` means sleep with no deadline; otherwise sleep for
/// `sleep_for` and ask again.
struct Decision {
  Trigger trigger = Trigger::kNone;

  /// Nothing is scheduled. Wait to be woken -- there is no deadline worth
  /// setting, and setting one anyway is the idle cost this file exists to
  /// refuse.
  bool parked = false;

  /// How long until the next thing worth waking for. Zero when `trigger` is set
  /// or `parked` is true.
  ///
  /// It is an upper bound on the sleep and never a promise that something will
  /// be due: a caller woken early by `RequestNow()` simply asks again.
  std::chrono::milliseconds sleep_for{0};

  bool run() const { return trigger != Trigger::kNone; }
};

/// The one thing that decides when a tick runs.
///
/// Not thread-safe, for `auth::Gate`'s reason: a sysmodule has one worker, and
/// the object it shares with the IPC thread is the engine, which owns the mutex
/// and the condition variable. `RequestNow()` is the one call that arrives from
/// another thread, and the caller makes it under that mutex.
class Scheduler {
 public:
  /// Monotonic, for everything that must not move when a user sets the console
  /// clock: the backoff deadline, and the lower bound on an elapsed interval.
  using SteadyClock = std::function<std::chrono::steady_clock::time_point()>;

  /// The console's own clock, for the one thing `steady_clock` cannot see: the
  /// hours a suspend hid. Read only as a *difference* between two ticks, and
  /// never as a timestamp anything is stamped with.
  using WallClock = std::function<std::chrono::system_clock::time_point()>;

  /// Null clocks mean `steady_clock::now` and `system_clock::now`.
  explicit Scheduler(SchedulerConfig config = {}, SteadyClock steady = nullptr,
                     WallClock wall = nullptr);

  const SchedulerConfig& config() const { return config_; }

  /// Take a new configuration, mid-schedule.
  ///
  /// The last tick is not forgotten, so shortening the interval can make one due
  /// immediately and lengthening it can push one out -- both of which are what a
  /// user who just changed the setting expects to happen.
  ///
  /// It also clears a TLS park: a configuration change is the only evidence this
  /// object ever gets that somebody has been at the problem.
  void Reconfigure(const SchedulerConfig& config);

  /// The overlay asked for a tick. Idempotent until the tick starts: two presses
  /// are one tick, which is what `ipc::Engine::RequestSync` answering `false`
  /// for "one is already running" means one level up.
  ///
  /// **Refused while `enabled` is false**, and answers whether it was taken so
  /// the caller can say so.
  bool RequestNow();

  /// Whether a request is waiting to be picked up by the next `Poll()`.
  bool requested() const { return on_demand_; }

  /// What to do now. Clears the on-demand flag when it answers `kOnDemand`, and
  /// marks the boot tick as taken when it answers `kBoot` -- so a caller that
  /// asks twice without running anything gets the second decision, not the first
  /// one again.
  Decision Poll();

  /// Record how the tick `Poll()` handed out ended.
  ///
  /// This is the whole of the failure policy, and it is written against
  /// `TickOutcome` rather than against an HTTP status because that enum is
  /// already split on what a scheduler would do (sync_tick.hpp):
  ///
  ///   - `kCompleted`, `kPartial`, `kUnreported` -- the tick ran. The backoff is
  ///     reset to the floor and the interval starts again from now. `kPartial`
  ///     is **not** a failure to retry sooner: there is no retry inside a tick
  ///     by design, and the next negotiation plans the leftovers again.
  ///   - `kOffline` -- transient. Back off and try again; nothing was written.
  ///   - `kRefused` -- an answer that does not change on a second attempt. Backs
  ///     off on the same curve, which is the "harder than usual" the outcome
  ///     asks for, because the alternative is asking a server that has said no
  ///     once every interval forever.
  ///   - `kUnauthorized` -- **not backed off here.** `auth::Gate` owns what a
  ///     rejected call means and how long to wait for it, and the caller consults
  ///     `blocked()` before the next tick. The *schedule* is restamped anyway,
  ///     because the tick did run and did reach the server -- and because a
  ///     schedule that did not restamp would make the very next `Poll()` fire
  ///     again, which is a tight loop against a caller that forgot to ask the
  ///     gate.
  ///   - `kCanceled` -- shutdown. Nothing is recorded: the process is going away
  ///     and the next boot decides for itself.
  ///   - `kDisabled` -- the gate below refused it, which on a scheduler that
  ///     never fires when disabled means the configuration moved under the tick.
  ///     Neither a success nor a failure; the next `Poll()` parks.
  ///   - `kRescanNeeded` -- run again immediately, up to
  ///     `max_rescans` consecutive times, then treat it as a failure.
  ///
  /// `transport` is the transport failure behind an offline tick when the caller
  /// knows it (`Negotiation::transport`), and `http::Error::kNone` when it does
  /// not. Only `kTls` changes anything -- see `SchedulerConfig::max_tls_attempts`.
  void Finished(TickOutcome outcome, http::Error transport = http::Error::kNone);

  /// The same, reading both off a tick's result.
  void Finished(const TickResult& result);

  /// Consecutive failed ticks since the last one that ran.
  int failures() const { return backoff_.failures(); }

  /// What the last failure asked to wait, or zero. For a log line and for the
  /// suite.
  std::chrono::milliseconds backoff() const { return backoff_.delay(); }

  /// The interval is parked because the last `max_tls_attempts` ticks all failed
  /// the handshake. Only `RequestNow()` and `Reconfigure()` clear it.
  bool tls_parked() const { return tls_failures_ >= config_.max_tls_attempts; }

  /// When the last tick that *ran* finished, on the console's clock. Zero before
  /// there has been one.
  ///
  /// For `ipc::EngineSnapshot::last_sync_at`, which is the one place this value
  /// is allowed to be read as a timestamp rather than as a difference.
  std::chrono::system_clock::time_point last_tick_at() const { return last_wall_; }

 private:
  using Steady = std::chrono::steady_clock::time_point;
  using Wall = std::chrono::system_clock::time_point;

  Steady SteadyNow() const;
  Wall WallNow() const;

  /// How long since the last tick, as the *later* of what the two clocks say.
  ///
  /// `steady_clock` is the lower bound and can never go backwards; the wall
  /// clock is what saw the suspend. Taking the later of the two is what makes a
  /// backwards correction harmless -- it is simply ignored -- without a special
  /// case for it.
  std::chrono::milliseconds Elapsed(Steady steady_now, Wall wall_now) const;

  /// Remember that a tick just ran, on both clocks.
  void MarkRan(Steady steady_now, Wall wall_now);

  SchedulerConfig config_;
  SteadyClock steady_;
  WallClock wall_;

  retry::Backoff backoff_;

  /// Set the moment `Poll()` hands out the boot tick, so the boot trigger
  /// happens once per process however many times it is asked. A boot tick that
  /// *fails* retries on the backoff, as an interval tick.
  bool booted_ = false;

  bool on_demand_ = false;

  /// When the backoff is up. Meaningful only while `backoff_.failures() > 0`.
  Steady retry_at_{};

  /// The last tick that ran, on both clocks. `last_wall_` is at the epoch until
  /// there has been one -- and stays there when the console's clock is not set,
  /// which is what `Elapsed` reads as "the wall clock has nothing to add".
  Steady last_steady_{};
  Wall last_wall_{};

  /// Consecutive `kTls` failures, and consecutive `kRescanNeeded` answers.
  int tls_failures_ = 0;
  int rescans_ = 0;
};

}  // namespace rommsync::sync
