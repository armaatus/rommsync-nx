// When a tick runs, and how long the client waits after one that did not work.
//
// Every scenario here drives `sync::Scheduler` with an **injected** steady clock
// and an injected wall clock, so an interval, a suspend and a backoff are all
// arithmetic rather than a wait. That is the whole reason the scheduler owns no
// thread: a suite that had to sit out thirty minutes to prove a thirty-minute
// interval is a suite nobody runs, and the cases that matter here -- a console
// asleep for eleven hours, a clock corrected backwards, a server that has been
// down all day -- cannot be produced any other way.
//
// None of it needs a server, and none of it may ever grow one: what these
// scenarios pin is that the schedule does **not** fire, which is a claim about
// requests that were never made.
//
//   backoff   -- the one shared curve: doubles, caps, is jittered, never under
//                the floor -- and `PairingSession` uses this same object
//   interval  -- exactly one tick per interval, and not one per poll
//   ondemand  -- `interval_min = 0` fires on boot and on `SyncNow`, never on a timer
//   disabled  -- `[sync] enabled = false` never fires, at all, ever
//   suspend   -- a clock advanced past several intervals is ONE catch-up tick
//   skew      -- a wall clock that jumps backwards is neither a storm nor a stall
//   failures  -- a failed tick reschedules on the backoff; the next success
//                returns to the floor
//   tls       -- a handshake failure is capped rather than retried forever
//   idle      -- many simulated hours of an offline or switched-off console cost
//                a bounded, small number of ticks
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/backoff.hpp"
#include "rommsync/scheduler.hpp"

namespace {

namespace http = rommsync::http;
namespace retry = rommsync::retry;
namespace sync = rommsync::sync;

using std::chrono::hours;
using std::chrono::milliseconds;
using std::chrono::minutes;
using std::chrono::seconds;
using sync::Decision;
using sync::Scheduler;
using sync::SchedulerConfig;
using sync::TickOutcome;
using sync::Trigger;

/// Both clocks a scheduler reads, driven by hand.
///
/// They advance **together** by default, which is a console that is awake, and
/// apart on purpose in `suspend` and `skew` -- which is the whole point of there
/// being two of them.
class Clocks {
 public:
  std::chrono::steady_clock::time_point steady() const {
    return std::chrono::steady_clock::time_point{} + steady_;
  }
  std::chrono::system_clock::time_point wall() const {
    return std::chrono::system_clock::time_point{} + wall_;
  }

  /// Time passing on a console that is awake.
  void Advance(milliseconds by) {
    steady_ += by;
    wall_ += by;
  }

  /// A suspend: the console's clock kept going and the monotonic one did not.
  void Suspend(milliseconds by) { wall_ += by; }

  /// Somebody set the clock, or NTP corrected it. Monotonic time is untouched,
  /// which is exactly what a correction looks like from inside the process.
  void SetWallBy(milliseconds by) { wall_ += by; }

  Scheduler::SteadyClock steady_fn() { return [this] { return steady(); }; }
  Scheduler::WallClock wall_fn() { return [this] { return wall(); }; }

 private:
  /// Far enough past `kEarliestCredibleWall` that the wall clock is believed --
  /// a console whose clock has never been set is `skew`'s business, not every
  /// other scenario's. 2024-01-01T00:00:00Z.
  milliseconds wall_{seconds{1'704'067'200}};

  /// Deliberately not zero: a steady clock that starts at its own epoch would
  /// let an off-by-one against an uninitialised member pass.
  milliseconds steady_{hours{9}};
};

SchedulerConfig ConfigWith(minutes interval) {
  SchedulerConfig config;
  config.interval = interval;
  // The floor a scenario asserts against. Short enough to write down, and the
  // jitter is off so a delay is a number rather than a range -- `backoff` is
  // where the jitter itself is checked.
  config.backoff.floor = seconds{30};
  config.backoff.cap = minutes{10};
  config.backoff.jitter = 0.0;
  return config;
}

/// Run the scheduler forward for `span`, in `step` slices, feeding every tick it
/// asks for the same `outcome`. Answers how many ticks that was.
///
/// This is the shape the sysmodule's worker has -- poll, run, report -- with the
/// sleep replaced by advancing the clock, so what a scenario measures is what a
/// console would actually do rather than what the object would say if asked.
int RunFor(Scheduler& scheduler, Clocks& clocks, milliseconds span, milliseconds step,
           TickOutcome outcome, http::Error transport = http::Error::kNone,
           std::vector<Trigger>* triggers = nullptr) {
  int ticks = 0;
  for (milliseconds elapsed{0}; elapsed < span; elapsed += step) {
    // Poll until it stops asking for ticks, the way a worker does before it goes
    // back to sleep.
    for (Decision decision = scheduler.Poll(); decision.run(); decision = scheduler.Poll()) {
      ++ticks;
      if (triggers != nullptr) {
        triggers->push_back(decision.trigger);
      }
      scheduler.Finished(outcome, transport);
      if (ticks > 10'000) {
        // A storm. The scenario's own assertion says what was wrong; this is
        // only here so a red run ends rather than hangs.
        return ticks;
      }
    }
    clocks.Advance(step);
  }
  return ticks;
}

// --- scenarios ---------------------------------------------------------------

/// The one backoff curve, and the promise every caller of it relies on.
void Backoff(checks::Checks& c) {
  {
    // No jitter: the doubling and the cap, exactly.
    retry::Backoff backoff({seconds{30}, minutes{10}, 0.0});
    c.Expect(backoff.Fail() == seconds{30}, "the first delay is the floor");
    c.Expect(backoff.Fail() == seconds{60}, "and the second doubles it");
    c.Expect(backoff.Fail() == seconds{120}, "and the third doubles again");
    for (int i = 0; i < 20; ++i) {
      backoff.Fail();
    }
    c.Expect(backoff.delay() == minutes{10},
             "the doubling saturates at the cap rather than overflowing");
    c.ExpectEq(backoff.failures(), 23, "and the failures are counted");
    backoff.Succeed();
    c.ExpectEq(backoff.failures(), 0, "a success clears the count");
    c.Expect(backoff.delay() == milliseconds{0}, "and the delay with it");
    c.Expect(backoff.Fail() == seconds{30}, "so the next failure starts at the floor again");
  }

  {
    // The jitter, at both ends of the range it is given. The roll is injected
    // because a delay drawn from a generator is a delay nothing can assert on.
    double roll = 0.0;
    retry::Backoff backoff({seconds{30}, minutes{10}, 0.25}, [&roll] { return roll; });
    c.Expect(backoff.Fail() == seconds{30},
             "a roll of zero is the undelayed curve, so the floor is a floor");
    backoff.Succeed();
    roll = 1.0;
    c.Expect(backoff.Fail() == milliseconds{37'500}, "and a roll of one adds the whole quarter");
    c.Expect(backoff.Fail() > backoff.Base(1), "a jittered delay is never below its own curve");
  }

  {
    // Two consoles that failed on the same instant must not retry on the same
    // instant. Nothing here asserts a distribution -- what is checked is that
    // the delay moves at all, which is the property a fixed curve does not have.
    retry::Backoff left({seconds{30}, minutes{10}, 0.5});
    bool differed = false;
    const milliseconds first = left.Fail();
    for (int i = 0; i < 32 && !differed; ++i) {
      left.Succeed();
      differed = left.Fail() != first;
    }
    c.Expect(differed, "the built-in generator actually spreads the retries");
  }

  {
    // A cap under the floor is a configuration that cannot be honoured. The
    // floor is the promise; the cap is the courtesy.
    retry::Backoff backoff({seconds{60}, seconds{5}, 0.0});
    c.Expect(backoff.Fail() == seconds{60},
             "a cap below the floor is read as the floor rather than undercutting it");
  }
}

/// A non-zero interval fires exactly once per interval, whatever the poll rate.
void Interval(checks::Checks& c) {
  Clocks clocks;
  Scheduler scheduler(ConfigWith(minutes{30}), clocks.steady_fn(), clocks.wall_fn());

  std::vector<Trigger> triggers;
  Decision first = scheduler.Poll();
  c.Expect(first.trigger == Trigger::kBoot, "the first tick is the boot one");
  scheduler.Finished(TickOutcome::kCompleted);

  // Six hours, polled every minute: a scheduler that fired per poll would run
  // 360 ticks, and one that ignored the interval would run none.
  const int ticks = RunFor(scheduler, clocks, hours{6} + minutes{1}, minutes{1},
                           TickOutcome::kCompleted, http::Error::kNone, &triggers);
  c.ExpectEq(ticks, 12, "six hours at a thirty-minute interval is twelve ticks");
  bool all_interval = true;
  for (const Trigger trigger : triggers) {
    all_interval = all_interval && trigger == Trigger::kInterval;
  }
  c.Expect(all_interval, "and every one of them is an interval tick");

  // The sleep it hands back is the time actually remaining, not a fixed poll.
  scheduler.RequestNow();
  c.Expect(scheduler.Poll().run(), "a tick to measure the interval from");
  scheduler.Finished(TickOutcome::kCompleted);
  clocks.Advance(minutes{10});
  const Decision waiting = scheduler.Poll();
  c.Expect(!waiting.run() && !waiting.parked, "mid-interval it asks to be woken later");
  c.Expect(waiting.sleep_for == minutes{20}, "and the wait is what is left of the interval");
}

/// `interval_min = 0` is boot and on demand, and no timer at all.
void OnDemand(checks::Checks& c) {
  Clocks clocks;
  Scheduler scheduler(ConfigWith(minutes{0}), clocks.steady_fn(), clocks.wall_fn());

  const Decision boot = scheduler.Poll();
  c.Expect(boot.trigger == Trigger::kBoot, "a zero interval still boots");
  scheduler.Finished(TickOutcome::kCompleted);

  c.ExpectEq(RunFor(scheduler, clocks, hours{48}, minutes{5}, TickOutcome::kCompleted), 0,
             "and then never fires on a timer, for two days");
  c.Expect(scheduler.Poll().parked, "it parks rather than asking to be polled again");

  c.Expect(scheduler.RequestNow(), "the overlay's Sync now is taken");
  const Decision asked = scheduler.Poll();
  c.Expect(asked.trigger == Trigger::kOnDemand, "and is the next thing to run");
  scheduler.Finished(TickOutcome::kCompleted);
  c.Expect(scheduler.Poll().parked, "one press is one tick, not a schedule");

  // Two presses before the tick starts are one tick, which is what
  // `ipc::Engine::RequestSync` answering false for "already running" means.
  scheduler.RequestNow();
  scheduler.RequestNow();
  c.Expect(scheduler.Poll().run(), "the first press runs");
  scheduler.Finished(TickOutcome::kCompleted);
  c.Expect(scheduler.Poll().parked, "and the second is the same tick, not another one");
}

/// The switch. A disabled scheduler never fires -- not on boot, not on a timer,
/// and not when something asks it to.
void Disabled(checks::Checks& c) {
  Clocks clocks;
  SchedulerConfig config = ConfigWith(minutes{5});
  config.enabled = false;
  Scheduler scheduler(config, clocks.steady_fn(), clocks.wall_fn());

  c.Expect(scheduler.Poll().parked, "a disabled scheduler parks on the first poll");
  c.ExpectEq(RunFor(scheduler, clocks, hours{24}, minutes{1}, TickOutcome::kCompleted), 0,
             "and fires nothing over a day at a five-minute interval");
  c.Expect(!scheduler.RequestNow(), "a Sync now against the switch is refused, not queued");
  c.Expect(scheduler.Poll().parked, "so nothing is waiting when it is switched back on");

  // Switched on: the interval starts now rather than reaching back over the day
  // the console spent disabled.
  config.enabled = true;
  scheduler.Reconfigure(config);
  const Decision resumed = scheduler.Poll();
  c.Expect(resumed.trigger == Trigger::kBoot,
           "the first tick after the switch is the boot tick it never got");
  scheduler.Finished(TickOutcome::kCompleted);
  c.ExpectEq(RunFor(scheduler, clocks, minutes{4}, minutes{1}, TickOutcome::kCompleted), 0,
             "and the interval runs from there, not from the day before");
}

/// Eleven hours asleep is one catch-up tick, not twenty-two.
void Suspend(checks::Checks& c) {
  Clocks clocks;
  Scheduler scheduler(ConfigWith(minutes{30}), clocks.steady_fn(), clocks.wall_fn());
  c.Expect(scheduler.Poll().trigger == Trigger::kBoot, "boot");
  scheduler.Finished(TickOutcome::kCompleted);

  // The console sleeps. `steady_clock` does not advance across a suspend, which
  // is the whole reason the rule is stated on the wall clock.
  clocks.Suspend(hours{11});

  int ticks = 0;
  for (Decision decision = scheduler.Poll(); decision.run(); decision = scheduler.Poll()) {
    ++ticks;
    scheduler.Finished(TickOutcome::kCompleted);
    if (ticks > 100) {
      break;
    }
  }
  c.ExpectEq(ticks, 1, "eleven hours of suspend is exactly one catch-up tick");
  c.Expect(!scheduler.Poll().run(), "and the interval starts again from the wake");

  // A monotonic-only scheduler would have fired here instead: the steady clock
  // has not moved at all, so the leftover of its timer is still running.
  clocks.Advance(minutes{29});
  c.Expect(!scheduler.Poll().run(), "twenty-nine minutes after the catch-up is not due yet");
  clocks.Advance(minutes{2});
  c.Expect(scheduler.Poll().trigger == Trigger::kInterval, "and thirty-one minutes is");
}

/// A wall clock that was never set, or that is corrected, must not produce a
/// negative interval or a storm.
void Skew(checks::Checks& c) {
  {
    Clocks clocks;
    Scheduler scheduler(ConfigWith(minutes{30}), clocks.steady_fn(), clocks.wall_fn());
    c.Expect(scheduler.Poll().trigger == Trigger::kBoot, "boot");
    scheduler.Finished(TickOutcome::kCompleted);

    // NTP puts the clock back by a year. The monotonic lower bound is what is
    // left, and it says nothing is due.
    clocks.SetWallBy(-hours{24 * 365});
    c.Expect(!scheduler.Poll().run(), "a year backwards is not an elapsed interval");
    c.ExpectEq(RunFor(scheduler, clocks, minutes{25}, minutes{5}, TickOutcome::kCompleted), 0,
               "and it does not turn into a storm either");
    clocks.Advance(minutes{10});
    c.Expect(scheduler.Poll().trigger == Trigger::kInterval,
             "the monotonic clock still gets the interval right afterwards");
  }

  {
    // The other correction: a console whose clock was never set, ticking, and
    // then set forward by decades. That is one catch-up tick and no more --
    // and it is why `kEarliestCredibleWall` refuses to read a difference off a
    // last tick that was stamped before it.
    Clocks clocks;
    Scheduler scheduler(ConfigWith(minutes{30}), clocks.steady_fn(), clocks.wall_fn());
    c.Expect(scheduler.Poll().trigger == Trigger::kBoot, "boot");
    scheduler.Finished(TickOutcome::kCompleted);
    clocks.SetWallBy(hours{24 * 365 * 20});
    int ticks = 0;
    for (Decision decision = scheduler.Poll(); decision.run() && ticks < 100;
         decision = scheduler.Poll()) {
      ++ticks;
      scheduler.Finished(TickOutcome::kCompleted);
    }
    c.ExpectEq(ticks, 1, "twenty years forward is one tick, not twenty years of them");
  }
}

/// A tick that did not work reschedules on the backoff, and the next one that
/// does returns the delay to the floor.
void Failures(checks::Checks& c) {
  Clocks clocks;
  Scheduler scheduler(ConfigWith(minutes{30}), clocks.steady_fn(), clocks.wall_fn());
  c.Expect(scheduler.Poll().trigger == Trigger::kBoot, "boot");

  scheduler.Finished(TickOutcome::kOffline);
  c.ExpectEq(scheduler.failures(), 1, "an offline tick is a failure");
  c.Expect(scheduler.backoff() == seconds{30}, "and waits the floor");
  const Decision waiting = scheduler.Poll();
  c.Expect(!waiting.run() && !waiting.parked, "nothing runs inside the backoff window");
  c.Expect(waiting.sleep_for == seconds{30}, "and the wait is the backoff");

  clocks.Advance(seconds{31});
  c.Expect(scheduler.Poll().trigger == Trigger::kInterval,
           "the retry fires when the backoff is up, without waiting for the interval");
  scheduler.Finished(TickOutcome::kOffline);
  c.Expect(scheduler.backoff() == seconds{60}, "and the next wait doubles");

  clocks.Advance(minutes{2});
  c.Expect(scheduler.Poll().run(), "and the second retry fires");
  scheduler.Finished(TickOutcome::kCompleted);
  c.ExpectEq(scheduler.failures(), 0, "one success clears the run of failures");
  c.Expect(scheduler.backoff() == milliseconds{0}, "and the delay with it");
  c.Expect(!scheduler.Poll().run(), "and the ordinary interval is what is left");

  // A refusal backs off on the same curve. Asking a server that has said no
  // once every interval forever is the thing this prevents.
  scheduler.Finished(TickOutcome::kRefused);
  c.ExpectEq(scheduler.failures(), 1, "a refusal is backed off, not retried at the interval");

  // A cancelled tick is a shutdown and records nothing at all.
  Scheduler other(ConfigWith(minutes{30}), clocks.steady_fn(), clocks.wall_fn());
  other.Finished(TickOutcome::kCanceled);
  c.ExpectEq(other.failures(), 0, "a cancelled tick is not a failure");

  // `kRescanNeeded` is the sweep having restored a save the scan could not have
  // seen: run again at once, and bounded so it cannot spin.
  Scheduler rescanning(ConfigWith(minutes{30}), clocks.steady_fn(), clocks.wall_fn());
  c.Expect(rescanning.Poll().run(), "boot");
  int rescans = 0;
  for (int i = 0; i < 10; ++i) {
    rescanning.Finished(TickOutcome::kRescanNeeded);
    if (!rescanning.Poll().run()) {
      break;
    }
    ++rescans;
  }
  c.ExpectEq(rescans, 2, "a rescan runs again immediately, and no more than twice in a row");
  c.ExpectEq(rescanning.failures(), 1, "past which it is treated as a failure and backed off");
}

/// A handshake failure is a configuration fault, and is capped rather than
/// retried every thirty seconds for the life of the console.
void Tls(checks::Checks& c) {
  Clocks clocks;
  Scheduler scheduler(ConfigWith(minutes{30}), clocks.steady_fn(), clocks.wall_fn());

  for (int attempt = 0; attempt < 3; ++attempt) {
    const Decision decision = scheduler.Poll();
    c.Expect(decision.run(), "the handshake is attempted");
    scheduler.Finished(TickOutcome::kOffline, http::Error::kTls);
    clocks.Advance(hours{1});
  }
  c.Expect(scheduler.tls_parked(), "three failed handshakes park the schedule");
  c.Expect(scheduler.Poll().parked, "so nothing is due, however long the console runs");
  c.ExpectEq(RunFor(scheduler, clocks, hours{24}, minutes{5}, TickOutcome::kOffline,
                    http::Error::kTls),
             0, "and a day of it costs no handshakes at all");

  // The two things that mean somebody has been at the problem.
  c.Expect(scheduler.RequestNow(), "an explicit Sync now is still taken");
  c.Expect(scheduler.Poll().trigger == Trigger::kOnDemand, "and runs");
  scheduler.Finished(TickOutcome::kOffline, http::Error::kTls);
  c.Expect(scheduler.tls_parked(), "a failure on that attempt parks it again");

  SchedulerConfig changed = ConfigWith(minutes{30});
  scheduler.Reconfigure(changed);
  c.Expect(!scheduler.tls_parked(), "and a settings change lifts the park");
  c.Expect(!scheduler.Poll().run(), "which resumes the ordinary backoff rather than firing at once");
  clocks.Advance(hours{1});
  c.Expect(scheduler.Poll().run(), "and tries again once that is up");

  // An ordinary offline tick does not count towards it: a console on a train
  // must not end up parked.
  Scheduler offline(ConfigWith(minutes{30}), clocks.steady_fn(), clocks.wall_fn());
  for (int attempt = 0; attempt < 8; ++attempt) {
    while (!offline.Poll().run()) {
      clocks.Advance(minutes{20});
    }
    offline.Finished(TickOutcome::kOffline, http::Error::kConnectFailed);
  }
  c.Expect(!offline.tls_parked(), "eight unreachable ticks are not a certificate problem");
}

/// What a day of a console nobody is using costs.
///
/// #33 proved the *tick* sends nothing with the switch off. What is left, and
/// what actually costs a battery, is the schedule not firing -- and, for the
/// console that is merely offline, the retries being a handful over a day rather
/// than one every thirty seconds.
void Idle(checks::Checks& c) {
  {
    Clocks clocks;
    SchedulerConfig config = ConfigWith(minutes{15});
    config.enabled = false;
    Scheduler scheduler(config, clocks.steady_fn(), clocks.wall_fn());

    int wakeups = 0;
    for (hours elapsed{0}; elapsed < hours{72}; elapsed += hours{1}) {
      const Decision decision = scheduler.Poll();
      c.Expect(!decision.run(), "a switched-off console never has a tick due");
      if (!decision.parked) {
        ++wakeups;
      }
      clocks.Advance(hours{1});
    }
    c.ExpectEq(wakeups, 0, "three days switched off is not one deadline to wake for");
  }

  {
    // Offline for a day, with the real jittered curve rather than the flat one
    // the other scenarios use -- what is asserted is a bound, and the bound has
    // to hold with the jitter on.
    Clocks clocks;
    SchedulerConfig config;
    config.interval = minutes{15};
    Scheduler scheduler(config, clocks.steady_fn(), clocks.wall_fn());

    const int ticks =
        RunFor(scheduler, clocks, hours{24}, minutes{1}, TickOutcome::kOffline);
    // The curve is 30s, 1m, 2m ... capped at 30 minutes, so a day is the eight
    // or so attempts it takes to reach the cap plus one every half hour after.
    c.Expect(ticks <= 64, "a day offline is tens of attempts, not one a minute");
    c.Expect(ticks >= 8, "and it does keep trying rather than giving up");
    c.Expect(scheduler.backoff() >= minutes{30},
             "with the delay saturated at the cap by the end of it");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "interval";
  checks::Checks checks;

  if (scenario == "backoff") {
    Backoff(checks);
  } else if (scenario == "interval") {
    Interval(checks);
  } else if (scenario == "ondemand") {
    OnDemand(checks);
  } else if (scenario == "disabled") {
    Disabled(checks);
  } else if (scenario == "suspend") {
    Suspend(checks);
  } else if (scenario == "skew") {
    Skew(checks);
  } else if (scenario == "failures") {
    Failures(checks);
  } else if (scenario == "tls") {
    Tls(checks);
  } else if (scenario == "idle") {
    Idle(checks);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() != 0) {
    std::cerr << scenario << ": " << checks.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << scenario << ": ok\n";
  return 0;
}
