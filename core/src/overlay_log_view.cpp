#include "rommsync/overlay_log_view.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

#include "rommsync/ipc.hpp"
#include "rommsync/log.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {
namespace {

/// Where the whole log is. The SD-root path, which is what a user with a card
/// reader opens -- not the `sdmc:` one the sysmodule writes through.
std::string WhereTheRestIs() {
  return std::string("The whole log is ") + log::kLogSdPath + " on the SD card";
}

}  // namespace

LogView RenderLog(const ipc::LogTail& tail) {
  LogView view;
  view.lines = tail.lines;
  view.hint = WhereTheRestIs();
  if (tail.total <= 0 || tail.lines.empty()) {
    // A fact, and the right one: a sysmodule that answered and has logged
    // nothing is running and has not got as far as a tick.
    view.headline = "Nothing has been logged since it started";
    return view;
  }
  const std::string shown = std::to_string(tail.lines.size());
  if (static_cast<std::int64_t>(tail.lines.size()) >= tail.total) {
    view.headline = "The whole log since it started: " + shown +
                    (tail.lines.size() == 1 ? " line" : " lines");
    return view;
  }
  // A tail. Said, because a user reading the first line shown would otherwise
  // take it for the first thing that happened.
  view.headline = "The last " + shown + " of " + std::to_string(tail.total) + " lines";
  return view;
}

std::size_t ScrollLog(std::size_t back, int rows, std::size_t lines, std::size_t room) {
  const std::size_t most = lines > room ? lines - room : 0;
  std::size_t moved = back > most ? most : back;
  if (rows < 0) {
    const std::size_t up = static_cast<std::size_t>(-static_cast<long long>(rows));
    moved = most - moved < up ? most : moved + up;
  } else {
    const std::size_t down = static_cast<std::size_t>(rows);
    moved = moved < down ? 0 : moved - down;
  }
  return moved;
}

LogNotice RenderDiagnostics(DiagnosticsOutcome outcome, const std::string& sd_path) {
  switch (outcome) {
    case DiagnosticsOutcome::kWritten:
      // The path a card reader shows, because the next thing the user does is
      // find this file on a computer and drag it into a bug report.
      return {"Wrote " + sd_path + " -- attach it to your bug report", Tone::kGood};
    case DiagnosticsOutcome::kWriteFailed:
      return {"Could not write diagnostics: the SD card refused the file", Tone::kBad};
    case DiagnosticsOutcome::kOutdated:
      return {"This sysmodule cannot write diagnostics; update sys-rommsync", Tone::kWarn};
    case DiagnosticsOutcome::kNoAnswer:
      break;
  }
  return {"The sysmodule did not answer; look in /config/rommsync/ in a moment", Tone::kWarn};
}

LogView RenderLogUnreachable(Link link, std::uint32_t sysmodule_interface) {
  const StatusView status = RenderUnreachable(link, sysmodule_interface);
  LogView view;
  view.link = status.link;
  view.headline = status.headline;
  view.tone = status.tone;
  // The sysmodule's own sentence when it has one -- it says what to do -- and
  // otherwise the card, which holds the log whether or not anything is running.
  view.hint = status.hint.empty() ? WhereTheRestIs() : status.hint;
  return view;
}

}  // namespace rommsync::overlay
