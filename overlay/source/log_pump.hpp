// The log screen's IPC: what `LogScreen::update()` and its Y press send,
// without the `tsl::Gui` around them (M11-24, #329).
//
// Split out of `log_screen.cpp` for `status_pump.hpp`'s reason: the screen
// waited, within the budget, for a `GetLog` once a second and for
// `WriteDiagnostics` on the press, on the draw thread, and what is left in a
// `tsl::Gui` is out of every test's reach. Here it goes through a `Poller`,
// and `ctest -R overlay.frames` times it. It names no libultrahand type, which
// `overlay.portable` holds it to.
#pragma once

#include <cstddef>

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_log_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// The log screen's frame and its one press.
class LogPump {
 public:
  /// Frames between two `GetLog`s: about a second at 60 Hz. The tail is up to
  /// `ipc::kMaxLogLines` lines to decode, and a log moves at the speed a tick
  /// does, not at sixty a second.
  static constexpr int kFramesBetweenReads = 60;

  explicit LogPump(IpcClient& client);

  LogPump(const LogPump&) = delete;
  LogPump& operator=(const LogPump&) = delete;

  /// `update()`. Never waits on the sysmodule.
  void Frame();

  /// Y: ask for the diagnostics bundle (#304). The answer arrives on a later
  /// frame; past the budget the notice says *still applying* until then.
  void PressDiagnostics();

  /// Scroll by `rows` (`ScrollRepeat`), against a panel holding `room` lines.
  void Scroll(int rows, std::size_t room);

  const LogView& view() const { return view_; }

 private:
  /// What the press answered.
  void Answered(Result rc);

  /// `view_` from a read that answered `rc`.
  void Read(Result rc);

  IpcClient& client_;
  ScreenFrame frame_{client_};
  Poller poller_;

  /// Starts as "not running" rather than as an empty screen: the first frame
  /// is drawn before the first poll returns.
  LogView view_ = RenderLogUnreachable(Link::kNotRunning);

  /// How far the log is scrolled up from its end (#304).
  std::size_t back_ = 0;

  /// What the last Y came to; outlives every poll's redraw.
  LogNotice notice_;
};

}  // namespace rommsync::overlay
