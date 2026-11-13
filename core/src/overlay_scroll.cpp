#include "rommsync/overlay_scroll.hpp"

namespace rommsync::overlay {

ScrollKey HeldScrollKey(bool, bool, bool, bool) { return ScrollKey::kNone; }

int ScrollRepeat::Update(ScrollKey held, Clock::time_point now) {
  key_ = held;
  pressed_ = next_ = seen_ = now;
  return 0;
}

}  // namespace rommsync::overlay
