// The loop between `psc:m` and this process. See power.hpp for the contract and
// for the three ways other projects have got it wrong.
#include "power.hpp"

namespace rommsync::sysmodule::power {

bool Quiesces(State state) {
  return state == State::kSleepReady || state == State::kEssentialServicesSleepReady ||
         state == State::kShutdownReady;
}

bool Resumes(State state) {
  // Not `kEssentialServicesAwake`; the header says why, and it is the difference
  // between resuming when the card is back and resuming when it is not.
  return state == State::kMinimumAwake || state == State::kFullAwake;
}

void Watcher::Run() {
  State state = State::kFullAwake;
  while (module_.NextRequest(&state)) {
    // **The sink first, the acknowledgement second, always.** The other order
    // tells PSC the card is free while a save is still being written to it,
    // which is hard rule 2 defeated by two lines in the wrong order (CLAUDE.md).
    // What keeps that honest is `Sink::Quiesce` being bounded: a wait here that
    // never returned would be a console that never sleeps, with no fatal and no
    // crash report (sys-con#155).
    if (Quiesces(state) && !quiesced_) {
      quiesced_ = true;
      sink_.Quiesce();
    } else if (Resumes(state) && quiesced_) {
      quiesced_ = false;
      sink_.Resume();
    }
    // Unconditional, and outside the branch above on purpose: a state this build
    // does not recognise still gets an answer. An unanswered request is a
    // transition that never finishes, which is a whole console frozen with
    // nothing anywhere saying why.
    module_.Acknowledge(state);
  }

  // **Never leave the process suspended.** The loop above ends for two reasons:
  // `Module::Stop()`, which is a caller going away, and a subscription that has
  // failed and unregistered (`power_psc.cpp`). The second is the dangerous one:
  // if it happened between a `SleepReady` and the wake, nothing is ever going to
  // deliver that wake, and a sink left quiesced is a client that has stopped
  // working for the rest of the boot with nothing on any screen saying why.
  //
  // Resuming is the right way to fail. The console is either awake already or
  // about to be -- PSC does not stop at `SleepReady` -- so the worst this costs
  // is a tick issued a moment early, against the alternative of a sysmodule that
  // is silently inert until the next reboot.
  if (quiesced_) {
    quiesced_ = false;
    sink_.Resume();
  }
}

}  // namespace rommsync::sysmodule::power
