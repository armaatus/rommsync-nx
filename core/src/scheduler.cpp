#include "rommsync/scheduler.hpp"

#include <algorithm>
#include <utility>

namespace rommsync::sync {
namespace {

/// The earliest wall-clock reading this file will believe a *previous* tick
/// happened at: 2020-01-01T00:00:00Z.
///
/// A console whose clock has never been set answers somewhere near the epoch, and
/// two readings taken from an unset clock are still a usable difference -- but a
/// clock that is set *between* two ticks turns that difference into fifty-five
/// years, which is one catch-up tick for nothing. Refusing to use the wall clock
/// at all when the last tick was stamped before this leaves `steady_clock`
/// driving, which is exactly right for a console that has not suspended.
///
/// It is never used to stamp anything. The one clock a save's `updated_at` comes
/// from is `ExecuteOptions::now`, and docs/SYNC_PROTOCOL.md refuses an epoch one
/// there for this same reason.
constexpr std::chrono::seconds kEarliestCredibleWall{1'577'836'800};

}  // namespace

const char* ToString(Trigger trigger) {
  switch (trigger) {
    case Trigger::kNone:
      return "none";
    case Trigger::kBoot:
      return "boot";
    case Trigger::kInterval:
      return "interval";
    case Trigger::kOnDemand:
      return "on_demand";
  }
  return "unknown";
}

Scheduler::Scheduler(SchedulerConfig config, SteadyClock steady, WallClock wall)
    : config_(config), steady_(std::move(steady)), wall_(std::move(wall)), backoff_(config.backoff) {
  if (config_.interval < std::chrono::minutes::zero()) {
    config_.interval = std::chrono::minutes::zero();
  }
}

Scheduler::Steady Scheduler::SteadyNow() const {
  return steady_ ? steady_() : std::chrono::steady_clock::now();
}

Scheduler::Wall Scheduler::WallNow() const {
  return wall_ ? wall_() : std::chrono::system_clock::now();
}

void Scheduler::Reconfigure(const SchedulerConfig& config) {
  config_ = config;
  if (config_.interval < std::chrono::minutes::zero()) {
    config_.interval = std::chrono::minutes::zero();
  }
  backoff_.Configure(config_.backoff);
  // Somebody has been at the settings, which is the only news this object ever
  // gets that a certificate problem may have been dealt with. See
  // `SchedulerConfig::max_tls_attempts`.
  tls_failures_ = 0;
}

bool Scheduler::RequestNow() {
  if (!config_.enabled) {
    return false;
  }
  on_demand_ = true;
  return true;
}

std::chrono::milliseconds Scheduler::Elapsed(Steady steady_now, Wall wall_now) const {
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(steady_now - last_steady_);
  if (elapsed < std::chrono::milliseconds::zero()) {
    // `steady_clock` cannot go backwards, so this is an injected clock in a
    // test. Zero rather than a negative interval, which is the one shape that
    // would make everything below fire at once.
    elapsed = std::chrono::milliseconds::zero();
  }
  if (last_wall_.time_since_epoch() >= kEarliestCredibleWall) {
    const auto by_wall =
        std::chrono::duration_cast<std::chrono::milliseconds>(wall_now - last_wall_);
    // Only ever *later*. A wall clock corrected backwards answers a negative
    // difference here and is simply ignored; the monotonic lower bound stands.
    elapsed = std::max(elapsed, by_wall);
  }
  return elapsed;
}

void Scheduler::MarkRan(Steady steady_now, Wall wall_now) {
  last_steady_ = steady_now;
  last_wall_ = wall_now;
}

Decision Scheduler::Poll() {
  Decision decision;
  if (!config_.enabled) {
    // The switch is off, so nothing is scheduled and there is no deadline worth
    // waking for. `sync::TickOptions::enabled` refuses a tick one level down
    // (M6-2, #33); this is the half that costs no wakeups at all.
    //
    // Anything that was waiting to run is **dropped here, not queued**: a
    // request or a rescan that survived the switch would fire the instant it
    // went back on, which is the thing `SchedulerConfig::enabled` says must not
    // happen. Dropping it in `Poll` rather than in `Reconfigure` catches the
    // request that arrives while it is already off as well.
    on_demand_ = false;
    rerun_ = false;
    decision.parked = true;
    return decision;
  }

  const Steady steady_now = SteadyNow();
  const Wall wall_now = WallNow();

  if (on_demand_) {
    // In front of the backoff and the TLS park, both deliberately: a user at the
    // settings screen pressing "Sync now" is the one event that is allowed to
    // ignore a wait this object imposed, and on a TLS fault it is the only thing
    // that ever tries again.
    //
    // It also consumes the boot tick, which is not a special case: a tick is
    // about to run, and running a second one a moment later because nobody had
    // ticked *yet* would be two ticks for one event.
    on_demand_ = false;
    booted_ = true;
    decision.trigger = Trigger::kOnDemand;
    return decision;
  }

  if (rerun_) {
    // The last tick asked to be run again -- a sweep restored a save the scan
    // could not have seen -- so this is the same scheduled tick continuing, not
    // a new trigger. `kInterval` and not `kOnDemand`: nobody pressed anything,
    // and `requested()` must not read true when no user asked.
    rerun_ = false;
    decision.trigger = Trigger::kInterval;
    return decision;
  }

  if (!booted_) {
    booted_ = true;
    if (config_.on_boot) {
      decision.trigger = Trigger::kBoot;
      return decision;
    }
    // No boot tick, so the interval runs from start rather than from an epoch
    // that would make one due immediately -- which is the boot tick the user
    // switched off.
    MarkRan(steady_now, wall_now);
  }

  if (tls_parked()) {
    // In front of the backoff, which is what makes the park a park: a pending
    // retry deadline would otherwise fire one more handshake at a certificate
    // that is not going to change. The pending deadline is kept rather than
    // cleared, so a `Reconfigure` that lifts the park resumes the ordinary
    // backoff instead of firing the instant a user touches the settings screen.
    decision.parked = true;
    return decision;
  }

  if (backoff_.failures() > 0) {
    // A retry is due whether or not the interval is: the tick that failed is one
    // the schedule still owes.
    if (steady_now < retry_at_) {
      decision.sleep_for =
          std::chrono::duration_cast<std::chrono::milliseconds>(retry_at_ - steady_now);
      return decision;
    }
    decision.trigger = Trigger::kInterval;
    return decision;
  }

  if (config_.interval == std::chrono::minutes::zero()) {
    // The documented meaning of `interval_min = 0`: boot and on demand, and no
    // timer at all (config.hpp).
    decision.parked = true;
    return decision;
  }

  const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(config_.interval);
  const std::chrono::milliseconds elapsed = Elapsed(steady_now, wall_now);
  if (elapsed >= interval) {
    // **Once**, however many intervals fit in `elapsed`. `Finished` restamps
    // both clocks, so a suspend of eleven hours costs one catch-up tick and not
    // twenty-two -- see the header.
    decision.trigger = Trigger::kInterval;
    return decision;
  }
  decision.sleep_for = interval - elapsed;
  return decision;
}

void Scheduler::Finished(TickOutcome outcome, http::Error transport) {
  const Steady steady_now = SteadyNow();
  const Wall wall_now = WallNow();

  switch (outcome) {
    case TickOutcome::kCompleted:
    case TickOutcome::kPartial:
    case TickOutcome::kUnreported:
      // It ran. Whatever it did not manage is the next negotiation's, not a
      // reason to come back sooner (sync_execute.hpp).
      backoff_.Succeed();
      tls_failures_ = 0;
      rescans_ = 0;
      MarkRan(steady_now, wall_now);
      return;

    case TickOutcome::kRescanNeeded:
      // Not a failure: the sweep restored a save the scan could not have seen,
      // so the caller scans again and runs another tick. Bounded because "run
      // again immediately" is the one answer here that could spin.
      if (rescans_ < config_.max_rescans) {
        ++rescans_;
        rerun_ = true;
        return;
      }
      rescans_ = 0;
      break;

    case TickOutcome::kOffline:
    case TickOutcome::kRefused:
      rescans_ = 0;
      break;

    case TickOutcome::kUnauthorized:
      // The credentials are `auth::Gate`'s to pace, not this object's, so there
      // is no backoff here (auth_gate.hpp). The *schedule* is restamped anyway,
      // because the tick did run and did reach the server -- and because a
      // schedule that did not restamp would fire again on the very next `Poll()`,
      // which is a tight loop against a caller that forgot to consult the gate.
      backoff_.Succeed();
      rescans_ = 0;
      MarkRan(steady_now, wall_now);
      return;

    case TickOutcome::kCanceled:
      // The process is going away. Nothing recorded: the next boot decides.
      return;

    case TickOutcome::kDisabled:
      // The configuration moved under the tick. The next `Poll()` parks on the
      // switch, and there is nothing to back off from -- nothing was attempted.
      rescans_ = 0;
      return;
  }

  if (transport == http::Error::kTls) {
    // A certificate the console does not trust does not become trusted by
    // asking again in thirty seconds. Counted, and the count is what parks the
    // interval -- see `SchedulerConfig::max_tls_attempts`.
    if (tls_failures_ < config_.max_tls_attempts) {
      ++tls_failures_;
    }
  } else {
    tls_failures_ = 0;
  }
  retry_at_ = steady_now + backoff_.Fail();
}

void Scheduler::Finished(const TickResult& result) {
  Finished(result.outcome, result.negotiated.transport);
}

}  // namespace rommsync::sync
