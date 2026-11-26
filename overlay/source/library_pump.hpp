// The library browser's IPC: what `LibraryScreen::update()` and its destructor
// send, without the `tsl::Gui` around them (M11-1, #285).
//
// Split out of `library_screen.cpp` for `conflicts_pump.cpp`'s reason: what is
// left in a `tsl::Gui` has never run, and the claim #285 is about -- a screen
// destroyed after the overlay has stopped opens no session -- is a claim about
// exactly this code. `ctest -R overlay.cycles` destroys a library holding a
// cursor on every exit path libtesla's loop has. It names no libultrahand
// type, which `overlay.portable` holds it to.
//
// **A frame never waits for a page** (M11-24, #329). It used to send the
// model's command and wait for it, within the budget, on the draw thread --
// and on a console a `ListNext` can be RomM serving the page, so a held R
// through a long platform was a stalled frame per page. Now the command goes
// through a `Poller` and the answer is collected on a later frame; while a page
// is out the selection keeps moving through the rows already loaded and rests
// on the "Loading..." row at their edge. `ctest -R overlay.frames` times it.
#pragma once

#include <cstdint>

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_library_model.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// The library screen's frame, and its teardown.
class LibraryPump {
 public:
  explicit LibraryPump(IpcClient& client);

  LibraryPump(const LibraryPump&) = delete;
  LibraryPump& operator=(const LibraryPump&) = delete;

  /// `update()`: collect what came back, tell the model, and hand the client
  /// the model's next command. Never waits on the sysmodule.
  void Frame();

  /// The screen's teardown: every cursor the model still holds closed, best
  /// effort, because a `ListEnd` that never arrives costs the sysmodule one of
  /// a small number of cursor slots until its TTL reclaims it (#31).
  ///
  /// It waits, within the budget per call, as it always has: the screen is
  /// going, and the cursors are the sysmodule's to get back. The call that is
  /// out is waited for first, because it may be the `ListBegin` opening one.
  void Close();

  /// What the screen's input moves, and what it renders.
  LibraryBrowserModel& model() { return model_; }
  const LibraryBrowserModel& model() const { return model_; }

 private:
  /// One event off the line, told to the model.
  void Handle(const Poller::Event& event);

  /// Hand the model's next command to the line, noticing a session that came
  /// back first. Whether one was handed over.
  bool SendNext();

  /// What `command` answered, told to the model.
  void Answered(ipc::Command command, Result rc);

  /// Draw `link` instead, and remember it was drawn.
  void Unreachable(Link link);

  /// Remember `link` was lost, and whether the session may have gone with it.
  void Lost(Link link);

  /// The link is back: the model starts again, ending first the cursors still
  /// open if the session is the one they were opened on (#343).
  void Restored();

  IpcClient& client_;
  ScreenFrame frame_{client_};
  Poller poller_{frame_, client_, Poller::kSendsOnly};
  LibraryBrowserModel model_;

  /// The link the model was last told about, so a session that came back is
  /// noticed once rather than restarting the browser every frame.
  Link link_ = Link::kOk;

  /// Whether every link lost since the last restore was `kNotResponding`, so
  /// the session the model's cursors are open on is the one that comes back
  /// (#343).
  bool same_session_ = true;
};

/// How many rows around the selection the screen is handed (#298): the two of
/// context `library_screen.cpp` draws above it, and more below than a panel
/// holds.
inline constexpr int kViewRowsAbove = 2;
inline constexpr int kViewRowsBelow = 24;

/// `view` re-rendered from `model` if anything changed since `*seen`, and
/// whether it was. A frame on which nothing changed copies nothing (#298).
bool RenderLibrary(const LibraryBrowserModel& model, LibraryView* view, std::uint64_t* seen);

}  // namespace rommsync::overlay
