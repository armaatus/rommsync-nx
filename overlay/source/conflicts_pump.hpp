// The conflicts screen's IPC: what `ConflictsScreen::update()` does, without
// the `tsl::Gui` around it (M9-6, #209).
//
// It was `ConflictsScreen::Pump` and `Send`, and it moved here for the reason
// `status_paint.cpp` exists: what is left in a `tsl::Gui` has never run, and
// "a slow restore does not hold the frame loop" is a claim about exactly this
// code -- the command it sends, and what it does with the answer.
// `ctest -R overlay.restore` drives it through the real `IpcClient` against a
// restore that is still copying. It names no libultrahand type, which
// `overlay.portable` holds it to.
//
// **No frame waits for an answer** (M11-24, #329). The model's command goes
// out through a `Poller` and is collected on a later frame, so a page, a poll
// of a restore and a press each cost a frame a mutex and a decode, whatever
// the sysmodule is doing. A press out past the budget says *still applying*.
#pragma once

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_conflicts_view.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// The conflicts screen's frame.
class ConflictsPump {
 public:
  explicit ConflictsPump(IpcClient& client);

  ConflictsPump(const ConflictsPump&) = delete;
  ConflictsPump& operator=(const ConflictsPump&) = delete;

  /// `update()`: collect what came back, tell the model, and hand the client
  /// the model's next command. Never waits on the sysmodule.
  void Frame();

  /// What the screen's input moves, and what it renders.
  ConflictsModel& model() { return model_; }
  const ConflictsModel& model() const { return model_; }

 private:
  /// `LibraryPump`'s three, for the same reasons.
  void Handle(const Poller::Event& event);
  bool SendNext();
  void Answered(ipc::Command command, Result rc);
  void Unreachable(Link link);

  IpcClient& client_;
  ScreenFrame frame_{client_};
  Poller poller_{frame_, client_, Poller::kSendsOnly};
  ConflictsModel model_;

  /// The link the model was last told about, so a session that came back is
  /// noticed once rather than restarting the screen every frame.
  Link link_ = Link::kOk;
};

}  // namespace rommsync::overlay
