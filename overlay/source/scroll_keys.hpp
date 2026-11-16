// Which buttons scroll a list, for the screens that draw one (#298).
//
// The repeat itself is `core/`'s (`rommsync/overlay_scroll.hpp`), where a test
// can hold a key down with a fake clock; this is only the part that has to name
// a libnx button, and it is one place so the library and the conflicts screen
// cannot come to disagree about what L does.
#pragma once

#include <switch.h>

#include "rommsync/overlay_scroll.hpp"

namespace rommsync::overlay {

/// The list key this frame's buttons hold: the D-pad or either stick for a row,
/// L or R for a page.
///
/// `keys_down` as well as `keys_held`, because a press shorter than a frame is
/// in the first and never in the second, and a tap must never be lost.
inline ScrollKey ScrollKeyOf(u64 keys_down, u64 keys_held) {
  const u64 keys = keys_down | keys_held;
  return HeldScrollKey((keys & HidNpadButton_AnyUp) != 0, (keys & HidNpadButton_AnyDown) != 0,
                       (keys & HidNpadButton_L) != 0, (keys & HidNpadButton_R) != 0);
}

}  // namespace rommsync::overlay
