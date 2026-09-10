// The loop between `psc:m` and this process. See power.hpp for the contract and
// for the three ways other projects have got it wrong.
#include "power.hpp"

namespace rommsync::sysmodule::power {

bool Quiesces(State state) {
  return state == State::kSleepReady || state == State::kEssentialServicesSleepReady ||
         state == State::kShutdownReady;
}

bool Resumes(State state) {
  return state == State::kMinimumAwake || state == State::kFullAwake;
}

void Watcher::Run() {
  State state = State::kFullAwake;
  while (module_.NextRequest(&state)) {
    module_.Acknowledge(state);
    if (Quiesces(state) && !quiesced_) {
      quiesced_ = true;
      sink_.Quiesce();
    } else if (Resumes(state) && quiesced_) {
      quiesced_ = false;
      sink_.Resume();
    }
  }
}

}  // namespace rommsync::sysmodule::power
