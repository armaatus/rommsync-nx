// The pairing screen: the code, the address to type it into, and a countdown.
//
// M4-5 (#27), and the human-facing half of the device-code flow M1-1 built.
// What it holds is the *drawing*; every decision about what the screen says is
// `rommsync::overlay::PairingView`'s, in `core/`, where a host test can reach
// it (`ctest -R overlay.pairing`). The same split `status_screen.hpp` has, for
// the same reason.
//
// **This screen never polls the network.** `GetPairState` reads a snapshot the
// sysmodule already holds and never blocks; the sysmodule owns the poll
// interval, and an overlay that drove `PairingSession::Poll()` on its draw
// thread would undercut the interval RomM asked for, earn `slow_down` on every
// later poll and wedge the pairing until the code expired (docs/AUTH.md).
//
// **Nothing here has ever run.** Overlay UI is one of the few things an emulator
// cannot exercise, so it is verified last, on hardware, after the M8-1 gate
// (overlay/AGENTS.md). Reading the code off a TV is M8-2 (#44).
#pragma once

#include <tesla.hpp>

#include "ipc_client.hpp"
#include "pairing_pump.hpp"
#include "rommsync/overlay_pairing_view.hpp"

namespace rommsync::overlay {

/// One `tsl::Gui` drawing one `PairingView`.
///
/// The client is borrowed rather than owned: the overlay holds one session for
/// as long as it is open, and every screen shares it (overlay/AGENTS.md).
class PairingScreen : public tsl::Gui {
 public:
  explicit PairingScreen(IpcClient& client);

  tsl::elm::Element* createUI() override;

  /// Called once per frame by Tesla. The IPC is `PairingPump`'s
  /// (`pairing_pump.hpp`), where a host test can reach it.
  void update() override;

  /// A: start a pairing attempt, or start a new one over a dead code. Both are
  /// the same `StartPair` -- `PairingSession::Begin()` discards whatever the
  /// last attempt left behind, which is exactly what "Start over" means.
  bool handleInput(u64 keys_down, u64 keys_held, const HidTouchState& touch,
                   HidAnalogStickState left_stick, HidAnalogStickState right_stick) override;

 private:
  /// Draw the pump's view into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  IpcClient& client_;

  /// The poll, the press, and the view they produce (`pairing_pump.hpp`).
  PairingPump pump_{client_};
};

}  // namespace rommsync::overlay
