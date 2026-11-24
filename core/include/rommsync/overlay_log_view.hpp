// The overlay's log screen -- the end of `rommsync.log`, without an SD reader.
//
// M7-3 (#38) put `GetLog` on the IPC service so "the overlay can show the last
// N lines", and until M9-6 (#209) nothing in the overlay called it: the command
// was dispatched and had no client, and a user had to pull the card to read why
// a sync did not happen. This is what that screen says; `overlay/source/`
// `log_paint.cpp` is where it puts it and `log_screen.cpp` the `tsl::Gui`
// around both -- the split every screen here has (overlay/AGENTS.md).
//
// The lines are drawn as the sysmodule rendered them, ordinal, level, event
// tag and all: docs/TROUBLESHOOTING.md is keyed on the event tag, and a user
// reading one off the panel into a bug report is the point.
//
// Hard rule 4 applies as it does to the rest of `core/`: no libnx header, no
// libultrahand type.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

/// Everything the log screen draws, and nothing about how.
struct LogView {
  Link link = Link::kOk;

  /// How much of the log this is. Never empty, in any state.
  std::string headline;

  /// Where the rest of it is, or what to do about a sysmodule that is not
  /// there.
  std::string hint;

  Tone tone = Tone::kNeutral;

  /// Oldest first, as `ipc::LogTail` carries them. The painter keeps the
  /// newest when the panel is short, because the end of a log is the part
  /// that says why the last tick failed.
  std::vector<std::string> lines;

  /// How many of the newest lines are scrolled off the bottom (#304). Zero is
  /// the end of the log, which is where the screen opens. The painter clamps it
  /// to what the panel can show; `ScrollLog` is how it moves.
  std::size_t back = 0;

  /// What the last "Write diagnostics" press came to, drawn under the hint.
  /// Empty until one is pressed. Set by the screen, not by `RenderLog`, so a
  /// poll that redraws the lines does not take it away.
  std::string notice;
  Tone notice_tone = Tone::kNeutral;
};

/// `back` after a held key worth `rows` rows (`ScrollRepeat::Update`; negative
/// is up, towards older lines). Clamped so the panel is always full: never past
/// the oldest line, never below the newest. `room` is how many lines the panel
/// holds, which `PaintLog` answers.
std::size_t ScrollLog(std::size_t back, int rows, std::size_t lines, std::size_t room);

/// How a "Write diagnostics" press ended, in the terms the screen has a
/// sentence for. The overlay reads a `Result`; this names no libnx type.
enum class DiagnosticsOutcome {
  kWritten,      ///< the bundle is on the card
  kWriteFailed,  ///< the sysmodule tried, and the card refused it
  kOutdated,     ///< the sysmodule predates `WriteDiagnostics` (`kUnknownCommand`)
  kNoAnswer,     ///< anything else: not running, not answering, a payload this build cannot read
  kApplying,     ///< out past the budget and not answered yet (#329); the answer replaces it
};

/// One sentence and its tone.
struct LogNotice {
  std::string text;
  Tone tone = Tone::kNeutral;
};

/// What the screen says after a press. `sd_path` is the bundle's, on `kWritten`.
LogNotice RenderDiagnostics(DiagnosticsOutcome outcome, const std::string& sd_path);

/// What `GetLog` answered, as a screen.
LogView RenderLog(const ipc::LogTail& tail);

/// The screen for a sysmodule that did not answer: the same sentences every
/// other screen uses (`RenderUnreachable`). `link` must not be `kOk`.
LogView RenderLogUnreachable(Link link, std::uint32_t sysmodule_interface = 0);

}  // namespace rommsync::overlay
