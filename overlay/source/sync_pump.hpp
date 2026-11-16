// The sync screen's IPC: what `SyncScreen::update()` and its two presses send,
// without the `tsl::Gui` around them (M11-15, #299).
//
// Split out of `sync_screen.cpp` for `status_pump.hpp`'s reason. It names no
// libultrahand type, which `overlay.portable` holds it to.
#pragma once

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_sync_actions.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// The sync screen's frame and its two presses.
class SyncPump {
 public:
  explicit SyncPump(IpcClient& client);

  SyncPump(const SyncPump&) = delete;
  SyncPump& operator=(const SyncPump&) = delete;

  /// `update()`. Never waits on the sysmodule: the poll and the presses go
  /// through `Poller`, and a frame only collects what came back.
  void Frame();

  /// Send `SyncNow`, or draw the refusal this screen already knows is coming.
  /// The answer arrives on a later frame.
  void PressSyncNow();

  /// Send `SetEnabled` for the opposite of what the sysmodule currently
  /// reports. The switch moves when the answer arrives, to whatever it says;
  /// past the budget the screen says *still applying* until then (#299).
  void PressToggle();

  const SyncActionsView& view() const { return view_; }

 private:
  /// What a press answered.
  void Answered(ipc::Command command, Result rc);

  /// Draw `link` instead.
  void Unreachable(Link link);

  /// `view_` from `status_` and `last_`. Every path that changes either of them
  /// ends here, so the screen is one function of the two rather than a set of
  /// places that each write a view.
  void Refresh();

  IpcClient& client_;
  ScreenFrame frame_{client_};
  Poller poller_{frame_, client_, ipc::Command::kGetStatus};

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

  /// What the last answer produced. Blank until the first, for
  /// `StatusPump::view_`'s reason: both controls are `kInert` and not drawn.
  SyncActionsView view_;
};

}  // namespace rommsync::overlay
