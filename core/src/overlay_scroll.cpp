#include "rommsync/overlay_scroll.hpp"

#include <algorithm>

namespace rommsync::overlay {
namespace {

/// What one step of `key` is worth, signed.
int StepOf(ScrollKey key) {
  switch (key) {
    case ScrollKey::kUp:
      return -1;
    case ScrollKey::kDown:
      return 1;
    case ScrollKey::kPageUp:
      return -kPageRows;
    case ScrollKey::kPageDown:
      return kPageRows;
    case ScrollKey::kNone:
      break;
  }
  return 0;
}

}  // namespace

ScrollKey HeldScrollKey(bool up, bool down, bool page_up, bool page_down) {
  if (page_up != page_down) {
    return page_up ? ScrollKey::kPageUp : ScrollKey::kPageDown;
  }
  if (up != down) {
    return up ? ScrollKey::kUp : ScrollKey::kDown;
  }
  return ScrollKey::kNone;
}

int ScrollRepeat::Update(ScrollKey held, Clock::time_point now) {
  const bool gap = now - seen_ > kHoldGap;
  seen_ = now;
  if (held == ScrollKey::kNone) {
    key_ = ScrollKey::kNone;
    return 0;
  }
  if (held != key_ || gap) {
    // A new press: it moves now, so a tap is never lost to the delay.
    key_ = held;
    pressed_ = now;
    next_ = now + kRepeatDelay;
    return StepOf(held);
  }
  if (now < next_) {
    return 0;
  }
  // On schedule, the next repeat is one interval after this one was due, so the
  // rate does not drift by however late each frame lands. A frame later than a
  // whole interval starts the schedule again from now rather than owing the
  // repeats it missed.
  next_ = now - next_ >= kRepeatInterval ? now + kRepeatInterval : next_ + kRepeatInterval;
  const auto repeating = now - (pressed_ + kRepeatDelay);
  const auto doublings = repeating / kAccelerateEvery;
  int factor = 1;
  for (auto doubled = decltype(doublings){0}; doubled < doublings && factor < kMaxRepeatStep;
       ++doubled) {
    factor *= 2;
  }
  return StepOf(held) * std::min(factor, kMaxRepeatStep);
}

}  // namespace rommsync::overlay
