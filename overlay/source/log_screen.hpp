// The log screen: the end of `rommsync.log`, read off the sysmodule.
//
// M9-6 (#209). M7-3 (#38) put `GetLog` on the service so a user could read why
// a sync did not happen without an SD reader, and nothing here called it. What
// the screen says is `rommsync::overlay::LogView`'s, in `core/`, and where it
// puts it is `log_paint.cpp`; both run under `ctest -R overlay.log`. The poll
// and the Y press are `log_pump.cpp`'s, and no frame waits for either (M11-24,
// #329). What is left here is the `tsl::Gui`.
//
// **Nothing here has ever run.** Overlay UI is verified last, on hardware,
// after the M8-1 gate (overlay/AGENTS.md); reading a log off a television is
// M8-2 (#44).
#pragma once

#include <tesla.hpp>

#include <cstddef>

#include "ipc_client.hpp"
#include "log_pump.hpp"
#include "rommsync/overlay_log_view.hpp"
#include "rommsync/overlay_scroll.hpp"

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
  /// than one a frame (`LogPump::kFramesBetweenReads`), never waited for.
  void update() override;

  /// Up/Down scroll the log and L/R a page, repeating while held (#298's
  /// `ScrollRepeat`); Y writes the diagnostics bundle (#304). B is not handled,
  /// so Tesla pops the screen.
  bool handleInput(u64 keys_down, u64 keys_held, const HidTouchState& touch,
                   HidAnalogStickState left_stick, HidAnalogStickState right_stick) override;

 private:
  /// Draw the pump's view into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  /// The poll, the press and the view (`log_pump.hpp`).
  LogPump pump_;

  /// The held key, and how many rows it is worth this frame (#298).
  ScrollRepeat repeat_;

  /// How many lines the panel held when it was last drawn: `Draw` is const and
  /// called by libtesla, and this is the one thing it learns that input needs.
  mutable std::size_t room_ = 0;
};

}  // namespace rommsync::overlay
