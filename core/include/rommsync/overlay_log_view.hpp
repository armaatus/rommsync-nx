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
};

/// What `GetLog` answered, as a screen.
LogView RenderLog(const ipc::LogTail& tail);

/// The screen for a sysmodule that did not answer: the same sentences every
/// other screen uses (`RenderUnreachable`). `link` must not be `kOk`.
LogView RenderLogUnreachable(Link link, std::uint32_t sysmodule_interface = 0);

}  // namespace rommsync::overlay
