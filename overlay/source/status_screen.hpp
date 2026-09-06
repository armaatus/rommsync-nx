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
// **Nothing here has ever run.** Overlay UI is one of the few things an emulator
// cannot exercise, so it is verified last, on hardware, after the M8-1 gate
// (overlay/AGENTS.md). What is checked today is that it cross-compiles and that
// the payloads it reads round-trip natively.
#pragma once

#include <tesla.hpp>

#include "ipc_client.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// One `tsl::Gui` drawing one `StatusView`.
///
/// The client is borrowed rather than owned: the overlay holds one session for
/// as long as it is open, and the other M4 screens share it (overlay/AGENTS.md).
class StatusScreen : public tsl::Gui {
 public:
  explicit StatusScreen(IpcClient& client);

  tsl::elm::Element* createUI() override;

  /// Called once per frame by Tesla. One `GetStatus` per frame is the rate this
  /// contract was shaped for -- `ipc::Status` carries the current download so
  /// the screen is one round trip rather than two, and the sysmodule answers it
  /// off a snapshot it already holds (`ipc::Engine::Snapshot`).
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
  /// Ask, and turn whatever came back -- including nothing -- into `view_`.
  void Poll();

  /// What the card says about the two switches, re-reading it if this poll is
  /// the one due to.
  ///
  /// Not an accessor: it is the poll's own step, and it stats three files on the
  /// polls where the countdown has run out. Named for that -- `card()` beside
  /// `card_` would read as free.
  ///
  /// `link` is what decides whether to look at all. Only `Link::kNotRunning`
  /// uses the answer: `RenderUnreachable` discards the card for `kUnreadable`
  /// and `kIncompatible`, where a session that opened has already proved the
  /// module installed and running.
  ///
  /// Only ever consulted when the sysmodule did not answer: a session that
  /// opened proves the module is installed and running, and reading it off the
  /// card as well would be two sources for one fact (`card_probe.hpp`).
  const CardState& CardThisPoll(Link link);

  /// Draw `view_` into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  IpcClient& client_;

  /// The session handshake, and which of the two unreachable sentences a failed
  /// call means. Shared with every other screen (`screen_frame.hpp`).
  ScreenFrame frame_{client_};

  /// What the last poll produced. Starts as "not running" rather than as an
  /// empty screen: the first frame is drawn before the first poll returns, and
  /// a blank one there is indistinguishable from a broken overlay.
  StatusView view_ = RenderUnreachable(Link::kNotRunning);

  /// The last look at the card, and how many polls until the next one. Zero so
  /// the first poll that needs it reads it rather than drawing a default.
  CardState card_;
  int probe_countdown_ = 0;
};

}  // namespace rommsync::overlay
