// The pairing screen's IPC: what `PairingScreen::update()` and its one press
// send, without the `tsl::Gui` around them (M11-15, #299).
//
// Split out of `pairing_screen.cpp` for `status_pump.hpp`'s reason. It names no
// libultrahand type, which `overlay.portable` holds it to.
//
// **This never polls the network.** `GetPairState` reads a snapshot the
// sysmodule already holds; the sysmodule owns the poll interval (docs/AUTH.md).
#pragma once

#include "ipc_client.hpp"
#include "rommsync/overlay_pairing_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// The pairing screen's frame and its one press.
class PairingPump {
 public:
  explicit PairingPump(IpcClient& client);

  PairingPump(const PairingPump&) = delete;
  PairingPump& operator=(const PairingPump&) = delete;

  /// `update()`.
  void Frame();

  /// A: start a pairing attempt, or start a new one over a dead code. Both are
  /// the same `StartPair` -- `PairingSession::Begin()` discards whatever the
  /// last attempt left behind, which is exactly what "Start over" means.
  ///
  /// A refusal and a dead session are told apart by asking `GetStatus` rather
  /// than by decoding the Horizon `Result`: `GetStatus` never fails, so one
  /// that does is the transport; and one that succeeds already says whether
  /// there is a `server.url`, which is the only distinction this screen draws
  /// between refusals.
  void PressStart();

  const PairingView& view() const { return view_; }

 private:
  /// The port and the version handshake, which every command on a fresh session
  /// waits for. Answers false when they could not be completed, having already
  /// put the reason in `view_`.
  bool Ready();

  IpcClient& client_;
  ScreenFrame frame_{client_};

  /// What the last poll produced. Starts as "not running" rather than as an
  /// empty screen: the first frame is drawn before the first poll returns, and
  /// a blank one there is indistinguishable from a broken overlay.
  PairingView view_ = RenderPairingUnreachable(Link::kNotRunning);

  /// Set by a `StartPair` the sysmodule refused, and cleared only by the next
  /// press or by a transport failure.
  ///
  /// Without it the frame after the button press draws what `StartPair`
  /// answered and the *next* poll's `GetPairState` overwrites it, so a refusal
  /// flashes for one frame and vanishes. It cannot key off `kIdle`: `StartPair`
  /// refuses before `PairingSession::Begin()` runs, so the session reports
  /// whatever the last attempt left -- `kExpired` after a Start over -- and
  /// only a first Pair from a genuinely idle session would be held.
  bool blocked_ = false;
};

}  // namespace rommsync::overlay
