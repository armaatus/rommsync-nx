// `power::Watcher` -- the PSC contract, without PSC (M9-4, #208).
//
// The console's half of this is `psc:m`, which is a service and so unreachable
// from a laptop. What is reachable is everything the module is *for*: that a
// request is acknowledged exactly once, that the quiesce happens **before** the
// acknowledgement rather than after it, that the two sleep states in a row are
// one suspend and not two, and that a state this build has never heard of is
// still answered. Each of those is a console that hangs, crashes or sleeps with
// a save half written when it is wrong, and none of them needs a Switch to
// check.
//
//   states  -- a whole Awake -> SleepReady -> ... -> Awake, in order
//   once    -- one acknowledgement per request, and the sink untouched by a
//              state neither `Quiesces` nor `Resumes` claims
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "checks.hpp"
#include "power.hpp"
#include "power_fake.hpp"

namespace power = rommsync::sysmodule::power;

namespace {

/// A `power::Sink` that writes down what it was asked and when.
///
/// The order against the acknowledgements is the whole point, so both go into
/// one list: "quiesce" here and "ack SleepReady" from the scenario, read back as
/// a sequence. A sink that recorded only counts would pass with the quiesce
/// happening after the acknowledgement, which is the bug this issue is about.
class Recording final : public power::Sink {
 public:
  void Quiesce() override { Note("quiesce"); }
  void Resume() override { Note("resume"); }

  std::vector<std::string> seen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }

 private:
  void Note(std::string what) {
    std::lock_guard<std::mutex> lock(mutex_);
    seen_.push_back(std::move(what));
  }

  mutable std::mutex mutex_;
  std::vector<std::string> seen_;
};

/// How long a scenario waits for an acknowledgement that should arrive. Generous
/// because it is only ever spent on a failing run: a working watcher answers
/// within the time it takes to wake a thread.
constexpr std::chrono::milliseconds kAckBudget{5000};

/// One suspend and one wake, in the order PSC sends them.
///
/// `SleepReady` then `EssentialServicesSleepReady` on the way down and
/// `EssentialServicesAwake` then `MinimumAwake` then `FullAwake` on the way back
/// -- five requests, five acknowledgements, and exactly one quiesce and one
/// resume between them. Two quiesces would be this process doing the whole
/// tear-down twice for one sleep; two resumes would be it starting work in the
/// middle of a wake sequence.
void States(checks::Checks& c) {
  power_fake::Scripted module;
  Recording sink;
  {
    power_fake::Running running(module, sink);
    for (const power::State state :
         {power::State::kSleepReady, power::State::kEssentialServicesSleepReady,
          power::State::kEssentialServicesAwake, power::State::kMinimumAwake,
          power::State::kFullAwake}) {
      module.Deliver(state);
    }
    c.Expect(module.AwaitAcks(5, kAckBudget), "every request is answered");
  }

  const std::vector<power::State> acked = module.acknowledged();
  c.ExpectEq(acked.size(), std::size_t{5}, "one acknowledgement per request, and no more");
  if (acked.size() == 5) {
    c.Expect(acked[0] == power::State::kSleepReady &&
                 acked[1] == power::State::kEssentialServicesSleepReady &&
                 acked[2] == power::State::kEssentialServicesAwake &&
                 acked[3] == power::State::kMinimumAwake && acked[4] == power::State::kFullAwake,
             "and each one names the state it is for -- `pscPmModuleAcknowledge` carries it "
             "into cmd 4 on 5.1.0 and up (power_psc.cpp)");
  }

  // The sink, which is what a console would feel. The second sleep state and the
  // last two wake states are the same suspend and the same wake.
  const std::vector<std::string> seen = sink.seen();
  c.ExpectEq(seen.size(), std::size_t{2},
             "one suspend and one wake for one sleep, however many states it took");
  if (seen.size() == 2) {
    c.ExpectEq(seen[0], std::string("quiesce"), "the console goes quiet first");
    c.ExpectEq(seen[1], std::string("resume"), "and comes back afterwards");
  }
}

/// The acknowledgement waits for the quiesce, and a state nobody knows is still
/// answered.
///
/// The first half is the ordering hard rule 2 rests on: a module that
/// acknowledged and *then* stopped writing has told PSC the card is free while a
/// save is still being written to it. The check is a sink that blocks -- the
/// acknowledgement may not appear while it is in there.
///
/// The second half is the other direction. `pscPmModuleGetRequest` hands over
/// whatever PSC sent; a value this build does not recognise must still be
/// acknowledged, because an unanswered request is a console that will not sleep
/// (sys-con#155) -- and it must not be guessed at, because guessing is a module
/// that tears its card access down on a state that was never about sleep.
void Once(checks::Checks& c) {
  /// A quiesce that does not return until the scenario lets it go.
  class Held final : public power::Sink {
   public:
    void Quiesce() override {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        inside_ = true;
        arrived_.notify_all();
        released_.wait(lock, [this] { return release_; });
      }
    }

    void Resume() override {
      std::lock_guard<std::mutex> lock(mutex_);
      ++resumes_;
    }

    bool AwaitQuiesce() {
      std::unique_lock<std::mutex> lock(mutex_);
      return arrived_.wait_for(lock, kAckBudget, [this] { return inside_; });
    }

    void Release() {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        release_ = true;
      }
      released_.notify_all();
    }

    int resumes() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return resumes_;
    }

   private:
    mutable std::mutex mutex_;
    std::condition_variable arrived_;
    std::condition_variable released_;
    bool inside_ = false;
    bool release_ = false;
    int resumes_ = 0;
  };

  power_fake::Scripted module;
  Held sink;
  {
    power_fake::Running running(module, sink);
    module.Deliver(power::State::kSleepReady);
    c.Expect(sink.AwaitQuiesce(), "the watcher quiesces the process");
    c.Expect(!module.AwaitAcks(1, std::chrono::milliseconds{200}),
             "and does not acknowledge while it is still going quiet -- an acknowledgement "
             "here is PSC told the card is free with a save still being written to it");
    sink.Release();
    c.Expect(module.AwaitAcks(1, kAckBudget), "the acknowledgement follows the quiesce");

    // A state out of the enum, which is what a firmware this build predates
    // would send. Answered, and the sink left alone.
    module.Deliver(static_cast<power::State>(9));
    c.Expect(module.AwaitAcks(2, kAckBudget),
             "a state this build has never heard of is acknowledged anyway -- an unanswered "
             "request is a console that never finishes the transition (sys-con#155)");
  }

  c.ExpectEq(sink.resumes(), 0, "and it is not mistaken for a wake");
  const std::vector<power::State> acked = module.acknowledged();
  c.ExpectEq(acked.size(), std::size_t{2}, "two requests, two acknowledgements");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "states";
  checks::Checks checks;
  if (scenario == "states") {
    States(checks);
  } else if (scenario == "once") {
    Once(checks);
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
