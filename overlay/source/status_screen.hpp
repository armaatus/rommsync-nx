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

 private:
  /// Ask, and turn whatever came back -- including nothing -- into `view_`.
  void Poll();

  /// Drop the session, try the port again, and answer with whichever of the two
  /// unreachable states that establishes. Every transport failure goes through
  /// here so that "not running" is only ever said about a port that is actually
  /// gone.
  StatusView Reopen();

  /// Draw `view_` into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  IpcClient& client_;

  /// What the last poll produced. Starts as "not running" rather than as an
  /// empty screen: the first frame is drawn before the first poll returns, and
  /// a blank one there is indistinguishable from a broken overlay.
  StatusView view_ = RenderUnreachable(Link::kNotRunning);

  /// Whether the contract check has passed on the current session. Reset with
  /// every session, because a session that came back is a sysmodule that may
  /// have been replaced since the last one.
  bool version_checked_ = false;
};

}  // namespace rommsync::overlay
