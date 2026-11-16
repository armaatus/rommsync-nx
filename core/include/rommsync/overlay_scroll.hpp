// Hold-to-repeat for the overlay's lists, decided here and read over there
// (M11-14, #298).
//
// libtesla repeats a held direction only by moving focus between focusable
// elements (`requestFocus`), and every list screen here draws into one
// `CustomDrawer`, which is never focused. So a held D-pad moved one row per
// press: five hundred roms were five hundred presses. The repeat is therefore
// this project's own, and it lives in `core/` for the reason every overlay
// decision does -- a rate is arithmetic over time, and arithmetic inside a
// `tsl::Gui` is untestable until someone has a console. `ctest -R
// overlay.library` drives it with a fake clock.
//
// The screen turns its buttons into a `ScrollKey` (`HeldScrollKey`), asks
// `ScrollRepeat::Update` how many rows that is worth this frame, and moves the
// selection by the answer. Nothing here knows what a row is.
//
// Hard rule 4: no libnx header, no `HidNpadButton`. The screen reads the
// buttons; this reads booleans.
#pragma once

#include <chrono>

namespace rommsync::overlay {

/// What the user is holding, reduced to the one thing a list does with it.
enum class ScrollKey { kNone, kUp, kDown, kPageUp, kPageDown };

/// The one key a frame's buttons mean.
///
/// A page key beats a row key -- holding R while the stick rests slightly down
/// is a page jump, not a crawl -- and two opposite keys of the same kind cancel,
/// because neither direction is what the user meant.
ScrollKey HeldScrollKey(bool up, bool down, bool page_up, bool page_down);

/// How long a key is held before it starts repeating. Long enough that a tap
/// is one row; libtesla's own focus repeat waits about as long.
inline constexpr std::chrono::milliseconds kRepeatDelay{300};

/// The time between two repeats once they start: 20 a second, and three
/// frames at 60 Hz exactly, so the rate does not beat against the frame rate.
inline constexpr std::chrono::milliseconds kRepeatInterval{50};

/// Every this long of repeating, one repeat moves twice as far.
inline constexpr std::chrono::milliseconds kAccelerateEvery{1000};

/// The most one repeat is multiplied by, reached after four seconds held. A
/// held page key passes row 2,000 of a platform in under four (#298 asked for
/// "a few"); at 16 it moves 160 rows a repeat, 3,200 a second, which is about
/// what the browser loads at one 64-row page a frame -- so a held key rides the
/// end of what is loaded rather than running far past it.
inline constexpr int kMaxRepeatStep = 16;

/// How many rows one L/R moves. About a panel's worth, less the two rows of
/// context the library keeps above its selection, so a page jump never skips a
/// row the user has not seen go past.
inline constexpr int kPageRows = 10;

/// A frame later than this after the last one is a new press, not a held key.
///
/// `handleInput` does not run while the overlay is hidden, so a key held across
/// a hide would otherwise come back already accelerated -- the overlay's
/// `Cadence` rule (`overlay/AGENTS.md`) applied to input.
inline constexpr std::chrono::milliseconds kHoldGap{250};

/// One list's held key, and how far it is worth moving this frame.
class ScrollRepeat {
 public:
  using Clock = std::chrono::steady_clock;

  /// Rows to move the selection by now, signed: negative is up. Zero on a frame
  /// that is between repeats.
  ///
  /// The first frame of a press moves at once -- a tap is never lost to the
  /// delay -- then nothing until `kRepeatDelay`, then one step every
  /// `kRepeatInterval`, doubling every `kAccelerateEvery` up to
  /// `kMaxRepeatStep`. A row key's step is one row, a page key's `kPageRows`.
  /// A frame that arrives late fires once rather than catching up: a stalled
  /// frame is not a reason to jump.
  int Update(ScrollKey held, Clock::time_point now);

 private:
  ScrollKey key_ = ScrollKey::kNone;
  Clock::time_point pressed_{};
  Clock::time_point next_{};
  Clock::time_point seen_{};
};

}  // namespace rommsync::overlay
