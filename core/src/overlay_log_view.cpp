#include "rommsync/overlay_log_view.hpp"

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
