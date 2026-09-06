// The library browser: platforms, then roms, then the download queue.
//
// M4-3 (#25), and the only place a user picks something to download without a
// keyboard. What it holds is the *drawing* and the input map; every decision --
// which page to ask for next, what a page that failed does to the pages already
// loaded, whether a row may be downloaded and what a refused press says -- is
// `rommsync::overlay::LibraryBrowserModel`'s, in `core/`, where a host test can
// reach it (`ctest -R overlay.library`). The same split `status_screen.hpp`,
// `sync_screen.hpp` and `pairing_screen.hpp` have, for the same reason.
//
// **The overlay never calls RomM** (docs/ARCHITECTURE.md). Every row comes off a
// `ListNext` the sysmodule answered, so the fault-proxy scenarios belong to the
// sysmodule's issues; this screen's job is to render a page that failed as a
// page that failed, and keep the ones already loaded.
//
// **One command per frame, and it never blocks.** The model hands out one
// command at a time and is told what came back; a page the engine is still
// fetching comes back `pending` rather than waiting on the IPC thread (#31), so
// this asks again on the next frame instead of parking a draw thread on a
// socket.
//
// **Nothing here has ever run.** Overlay UI is one of the few things an emulator
// cannot exercise, so it is verified last, on hardware, after the M8-1 gate
// (overlay/AGENTS.md). Scrolling a real library on a console is M8-2 (#44).
#pragma once

#include <tesla.hpp>

#include "ipc_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_library_model.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// One `tsl::Gui` drawing one `LibraryView`.
///
/// The client is borrowed rather than owned: the overlay holds one session for
/// as long as it is open, and every screen shares it (overlay/AGENTS.md).
class LibraryScreen : public tsl::Gui {
 public:
  explicit LibraryScreen(IpcClient& client);

  /// Every cursor the model still holds is closed here, because a `ListEnd`
  /// that never arrives costs the sysmodule one of a small number of cursor
  /// slots until its TTL reclaims it (#31). Best effort: the destructor cannot
  /// keep pumping if the sysmodule has gone, which is precisely why the
  /// sysmodule reclaims them anyway.
  ~LibraryScreen() override;

  tsl::elm::Element* createUI() override;

  /// Called once per frame by Tesla. At most one IPC command goes out per
  /// frame; the model decides which, and answers `kNone` when the loaded pages
  /// are enough for what is on screen.
  void update() override;

  /// Up/Down move the selection, A activates the row, B leaves the level, and
  /// Y opens the download queue.
  ///
  /// A press on a row the model already knows would be refused sends nothing
  /// and leaves the reason on the row -- the same rule `sync_screen.cpp` keeps
  /// for a blocked button, and what makes `Enqueue` idempotent from this side
  /// (#25). B at the bottom of the stack is not handled, so Tesla's own
  /// binding closes the overlay.
  bool handleInput(u64 keys_down, u64 keys_held, const HidTouchState& touch,
                   HidAnalogStickState left_stick, HidAnalogStickState right_stick) override;

 private:
  /// Send at most one command, and tell the model what came back.
  void Pump();

  /// Send `command`, and report its answer to the model. Split out from `Pump`
  /// because every arm ends the same way -- a `Result` that failed is either a
  /// refusal the sysmodule named or the transport, and only `DecodeError` can
  /// tell those apart.
  void Send(const LibraryBrowserModel::Command& command);

  /// How many prompt rows this view will draw.
  ///
  /// Read before the list is drawn so the space can be held back for them: a
  /// list is longer than the panel in the ordinary case, and prompts drawn
  /// after it would only ever appear on a list short enough not to need
  /// scrolling. One function so the reservation and the drawing cannot
  /// disagree about how many there are.
  s32 PromptRows() const;

  /// Draw `view_` into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  IpcClient& client_;

  /// The session handshake, and which of the two unreachable sentences a failed
  /// call means. Shared with every other screen (`screen_frame.hpp`).
  ScreenFrame frame_{client_};

  LibraryBrowserModel model_;

  /// What the last `Render()` produced. Kept rather than rebuilt in `Draw`,
  /// which is `const` and is called by libtesla rather than by us.
  LibraryView view_ = LibraryView{};

  /// The link the model was last told about, so a session that came back is
  /// noticed once rather than restarting the browser every frame.
  Link link_ = Link::kOk;
};

}  // namespace rommsync::overlay
