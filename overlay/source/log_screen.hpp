// The log screen: the end of `rommsync.log`, read off the sysmodule.
//
// M9-6 (#209). M7-3 (#38) put `GetLog` on the service so a user could read why
// a sync did not happen without an SD reader, and nothing here called it. What
// the screen says is `rommsync::overlay::LogView`'s, in `core/`, and where it
// puts it is `log_paint.cpp`; both run under `ctest -R overlay.log`. What is
// left here is the `tsl::Gui` and the poll.
//
// **Nothing here has ever run.** Overlay UI is verified last, on hardware,
// after the M8-1 gate (overlay/AGENTS.md); reading a log off a television is
// M8-2 (#44).
#pragma once

#include <tesla.hpp>

#include "ipc_client.hpp"
#include "lifecycle.hpp"
#include "rommsync/overlay_log_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// One `tsl::Gui` drawing one `LogView`.
///
/// The client is borrowed rather than owned: the overlay holds one session for
/// as long as it is open, and every screen shares it (overlay/AGENTS.md).
class LogScreen : public tsl::Gui {
 public:
  explicit LogScreen(IpcClient& client);

  tsl::elm::Element* createUI() override;

  /// Called once per frame by Tesla, and it is one `GetLog` a second rather
  /// than one a frame: the tail is up to `ipc::kMaxLogLines` lines to decode,
  /// and a log moves at the speed a tick does, not at sixty a second.
  void update() override;

 private:
  void Poll();

  /// Draw `view_` into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  IpcClient& client_;

  /// The session handshake, and which of the unreachable sentences a failed
  /// call means. Shared with every other screen (`screen_frame.hpp`).
  ScreenFrame frame_{client_};

  /// When the next `GetLog` goes out (`lifecycle.hpp`).
  Cadence cadence_;

  /// Starts as "not running" rather than as an empty screen: the first frame
  /// is drawn before the first poll returns.
  LogView view_ = RenderLogUnreachable(Link::kNotRunning);
};

}  // namespace rommsync::overlay
