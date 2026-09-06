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
#include "rommsync/overlay_pairing_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// One `tsl::Gui` drawing one `PairingView`.
///
/// The client is borrowed rather than owned: the overlay holds one session for
/// as long as it is open, and every screen shares it (overlay/AGENTS.md).
class PairingScreen : public tsl::Gui {
 public:
  explicit PairingScreen(IpcClient& client);

  tsl::elm::Element* createUI() override;

  /// Called once per frame by Tesla. One `GetPairState` per frame is what the
  /// command was shaped for: it answers off a snapshot and never waits on a
  /// socket (`ipc.hpp`).
  void update() override;

  /// A: start a pairing attempt, or start a new one over a dead code. Both are
  /// the same `StartPair` -- `PairingSession::Begin()` discards whatever the
  /// last attempt left behind, which is exactly what "Start over" means.
  bool handleInput(u64 keys_down, u64 keys_held, const HidTouchState& touch,
                   HidAnalogStickState left_stick, HidAnalogStickState right_stick) override;

 private:
  /// Ask for the state, and turn whatever came back -- including nothing --
  /// into `view_`.
  void Poll();

  /// Send `StartPair` and render whatever it answered.
  ///
  /// A failure here is either a refusal or a dead session, and telling them
  /// apart is done by asking `GetStatus` rather than by decoding the Horizon
  /// `Result`. The result does carry the answer -- `MAKERESULT(420,
  /// ipc::Error)` -- but reading it would put a second copy of the sysmodule's
  /// result module in this directory, and a constant duplicated across the two
  /// halves is exactly the drift the shared `ipc.hpp` exists to prevent.
  /// `GetStatus` never fails, so a `GetStatus` that *does* fail is the
  /// transport; and one that succeeds already says whether there is a
  /// `server.url`, which is the only distinction this screen draws between
  /// refusals.
  void Start();

  /// The port and the version handshake, which every command on a fresh session
  /// waits for. Answers false when they could not be completed, having already
  /// put the reason in `view_`.
  bool Ready();

  /// Draw `view_` into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  IpcClient& client_;

  /// The session handshake, and which of the two unreachable sentences a failed
  /// call means. Shared with every other screen (`screen_frame.hpp`).
  ScreenFrame frame_{client_};

  /// What the last poll produced. Starts as "not running" rather than as an
  /// empty screen: the first frame is drawn before the first poll returns, and
  /// a blank one there is indistinguishable from a broken overlay.
  PairingView view_ = RenderPairingUnreachable(Link::kNotRunning);

  /// Set by a `StartPair` the sysmodule refused, and cleared only by the next
  /// press or by a transport failure.
  ///
  /// Without it the frame after the button press draws what `StartPair`
  /// answered and the *next* frame's `GetPairState` overwrites it, so a refusal
  /// flashes for one frame and vanishes. It cannot key off `kIdle`: `StartPair`
  /// refuses before `PairingSession::Begin()` runs, so the session reports
  /// whatever the last attempt left -- `kExpired` after a Start over -- and
  /// only a first Pair from a genuinely idle session would be held.
  bool blocked_ = false;
};

}  // namespace rommsync::overlay
