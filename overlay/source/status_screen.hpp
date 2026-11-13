// The status screen: connection, last sync, counts, current download, queue.
//
// The first real screen, and the frame the other four M4 screens are built on
// (M4-1, #23). What it holds is the *drawing* -- an `OverlayFrame` and a
// renderer callback. Every decision about what the screen says is
// `rommsync::overlay::StatusView`'s, in `core/`, where a host test can reach it
// (`ctest -R overlay.status`); this file turns those lines into pixels and
// nothing else. That split is the same one `sysmodule/source/ipc/` has, for the
// same reason: what cannot be tested before the M8-1 gate is kept to a size a
// person can check by reading it.
//
// The drawing itself is split again by M9-7 (#198): the layout is
// `status_paint.cpp`, which takes a `DrawList` and runs under
// `ctest -R overlay.draw`, and what is left in this file is a `tsl::Gui`, a
// `CustomDrawer` callback and an adapter that replays commands into
// `tsl::gfx::Renderer`. That last part has still never run -- overlay UI is one
// of the few things an emulator cannot exercise, so it is verified last, on
// hardware, after the M8-1 gate (overlay/AGENTS.md).
#pragma once

#include <tesla.hpp>

#include "ipc_client.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "status_pump.hpp"

namespace rommsync::overlay {

/// One `tsl::Gui` drawing one `StatusView`.
///
/// The client is borrowed rather than owned: the overlay holds one session for
/// as long as it is open, and the other M4 screens share it (overlay/AGENTS.md).
class StatusScreen : public tsl::Gui {
 public:
  explicit StatusScreen(IpcClient& client);

  tsl::elm::Element* createUI() override;

  /// Called once per frame by Tesla. The IPC is `StatusPump`'s
  /// (`status_pump.hpp`), where a host test can reach it.
  void update() override;

  /// Y opens the settings screen, which is this overlay's root menu.
  ///
  /// One binding rather than a screen of its own: until M4-4 (#26) nothing
  /// pushed any gui at all, so `SyncScreen`, `LibraryScreen` and
  /// `PairingScreen` compiled and `--gc-sections` dropped them from the image.
  /// The status screen is where the overlay opens, so it is where the way in
  /// has to be -- and the settings screen carries the rest of the menu, because
  /// putting it here would make a second one (#26).
  bool handleInput(u64 keys_down, u64 keys_held, const HidTouchState& touch,
                   HidAnalogStickState left_stick, HidAnalogStickState right_stick) override;

 private:
  /// Draw the pump's view into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  IpcClient& client_;

  /// The handshake, the poll, and the view they produce (`status_pump.hpp`).
  StatusPump pump_{client_};
};

}  // namespace rommsync::overlay
