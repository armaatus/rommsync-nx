// The sync screen: the switch that pauses the engine, and "Sync now".
//
// M4-2 (#24), and the two controls the overlay exists for. What it holds is the
// *drawing* and the two button presses; every decision about which control may
// be pressed and what a refused press says is
// `rommsync::overlay::SyncActionsView`'s, in `core/`, where a host test can
// reach it (`ctest -R overlay.sync_actions`). The same split
// `status_screen.hpp` and `pairing_screen.hpp` have, for the same reason.
//
// **The switch is a runtime pause, not the boot flag.** ovl-sysmodules toggles
// `/atmosphere/contents/<TID>/flags/` and the process then does not exist;
// this flips `[sync] enabled`, with the sysmodule resident, idle and still
// answering IPC. Nothing in this directory reads or writes that flag --
// compatibility with it is M6-2 (#33) -- and nothing here writes `config.ini`
// either: `SetEnabled` asks the sysmodule, which owns every write
// (docs/ARCHITECTURE.md). `overlay.sync_actions` greps this directory for both.
//
// **Neither press blocks.** `SyncNow` returns as soon as the tick is queued and
// `SetEnabled` returns once the file has been written and re-read (M5-3, #30);
// the screen polls `GetStatus` for what happened next, and never waits on a
// sync (`ipc.hpp`).
//
// **Nothing here has ever run.** Overlay UI is one of the few things an emulator
// cannot exercise, so it is verified last, on hardware, after the M8-1 gate
// (overlay/AGENTS.md). Pressing these two controls on a console is M8-2 (#44);
// `overlay/README.md` carries the script.
#pragma once

#include <tesla.hpp>

#include "ipc_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_sync_actions.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// One `tsl::Gui` drawing one `SyncActionsView`.
///
/// The client is borrowed rather than owned: the overlay holds one session for
/// as long as it is open, and every screen shares it (overlay/AGENTS.md).
class SyncScreen : public tsl::Gui {
 public:
  explicit SyncScreen(IpcClient& client);

  tsl::elm::Element* createUI() override;

  /// Called once per frame by Tesla. One `GetStatus` per frame is the rate the
  /// contract was shaped for: the sysmodule answers it off a snapshot it
  /// already holds (`ipc::Engine::Snapshot`).
  void update() override;

  /// A sends `SyncNow`; X flips `[sync] enabled`.
  ///
  /// A press on a control the view model called `kBlocked` sends nothing and
  /// draws the sentence instead -- the screen already knows which refusal it
  /// would get, and a round trip whose only effect is to make the same sentence
  /// arrive a frame later is one this screen does not make. A press on a
  /// `kInert` control is not handled at all, so B still leaves the screen and
  /// Tesla keeps its own bindings.
  bool handleInput(u64 keys_down, u64 keys_held, const HidTouchState& touch,
                   HidAnalogStickState left_stick, HidAnalogStickState right_stick) override;

 private:
  /// Ask, and turn whatever came back -- including nothing -- into `view_`.
  void Poll();

  /// Send `SyncNow`, or draw the refusal this screen already knows is coming.
  void PressSyncNow();

  /// Send `SetEnabled` for the opposite of what the sysmodule currently
  /// reports, and redraw the switch as whatever came back.
  void PressToggle();

  /// `view_` from `status_` and `last_`. Every path that changes either of them
  /// ends here, so the screen is one function of the two rather than a set of
  /// places that each write a view.
  void Refresh();

  /// Draw `view_` into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  IpcClient& client_;

  /// The session handshake, and which of the two unreachable sentences a failed
  /// call means. Shared with every other screen (`screen_frame.hpp`).
  ScreenFrame frame_{client_};

  /// The last status the sysmodule answered with.
  ///
  /// Kept rather than rendered and dropped, because a press has to be decided
  /// against it: which refusal a blocked "Sync now" draws is a function of the
  /// console's state, and re-asking for it on the button press would be a round
  /// trip inside an input handler.
  ipc::Status status_;

  /// What the last press produced. Cleared to `kNone` only by a `SetEnabled`
  /// that took -- see `LastCommand`.
  LastCommand last_;

  /// What the last poll produced. Starts as "not running" rather than as an
  /// empty screen: the first frame is drawn before the first poll returns, and
  /// a blank one there is indistinguishable from a broken overlay.
  SyncActionsView view_ = RenderSyncActionsUnreachable(Link::kNotRunning);
};

}  // namespace rommsync::overlay
