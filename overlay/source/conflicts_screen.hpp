// The conflicts screen: what a sync overwrote, and putting it back.
//
// M7-1 (#36), and the user-facing half of hard rule 2. A mandatory
// pre-overwrite backup is only a safety net if a human can find one and restore
// it without an SD reader, and `.backup/<rom_id>-<slot>-<unix seconds>.<ext>`
// names neither the game nor the day.
//
// What it holds is the *drawing* and the input map; every decision -- which
// page to ask for, whether a row can be restored, what the confirmation says,
// what a finished restore reads as -- is `rommsync::overlay::ConflictsModel`'s,
// in `core/`, where a host test can reach it (`ctest -R overlay.conflicts`). The
// same split every other screen in this directory has.
//
// **The overlay restores nothing itself.** A restore is a save overwrite that
// owes a backup first, so it lives on the service (`conflicts::Restore`) and
// this asks for it by entry id (docs/ARCHITECTURE.md §2).
//
// **The server stays the source of truth** (hard rule 3), and the confirmation
// says so: a restore writes the local file, and the next sync arbitrates. The
// wording is `overlay::RestoreMeaning()`'s so the confirmation and the test
// cannot come to say different things.
//
// **`[sync] conflict_show` is what hides this screen**, and it hides it from
// the *menu* -- `settings_screen.cpp` does not draw the row when it is off.
// Nothing here reads the setting, and the sysmodule records conflicts either
// way: a toggle that stopped the recording would turn hard rule 2 back into a
// directory of unnamed files (conflict_log.hpp).
//
// **Nothing here has ever run.** Overlay UI is one of the few things an
// emulator cannot exercise, so it is verified last, on hardware, after the M8-1
// gate (overlay/AGENTS.md); pressing restore on a console is M8-2 (#44).
#pragma once

#include <tesla.hpp>

#include "ipc_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_conflicts_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// One `tsl::Gui` drawing one `ConflictsView`.
///
/// The client is borrowed rather than owned: the overlay holds one session for
/// as long as it is open, and every screen shares it (overlay/AGENTS.md).
class ConflictsScreen : public tsl::Gui {
 public:
  explicit ConflictsScreen(IpcClient& client);

  tsl::elm::Element* createUI() override;

  /// Called once per frame by Tesla. At most one IPC command goes out per
  /// frame; the model answers `kNone` once the loaded page covers the screen,
  /// so a screen sitting still costs nothing.
  ///
  /// There is no cursor to close on the way out, which is why this screen has
  /// no destructor where `LibraryScreen` has one: `ListConflicts` is answered
  /// from a bounded vector the sysmodule already holds, so an abandoned screen
  /// leaves nothing behind (`ipc::ConflictQuery`).
  void update() override;

  /// Up/Down move the selection, A opens an entry and then confirms the
  /// restore, B steps back out.
  ///
  /// A press on an entry the model already knows cannot be restored sends
  /// nothing and leaves the reason on the screen -- the rule every screen here
  /// keeps for a control that would be refused.
  bool handleInput(u64 keys_down, u64 keys_held, const HidTouchState& touch,
                   HidAnalogStickState left_stick, HidAnalogStickState right_stick) override;

 private:
  /// Send at most one command, and tell the model what came back.
  void Pump();

  /// Send `command`, and report its answer to the model.
  void Send(const ConflictsModel::Command& command);

  /// How many prompt rows this view will draw. Read before the body so the
  /// space can be held back for them -- `library_screen.cpp`'s reason: a list
  /// longer than the panel would otherwise push every prompt off the bottom.
  s32 PromptRows() const;

  /// Draw `view_` into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  IpcClient& client_;

  /// The session handshake, and which of the two unreachable sentences a failed
  /// call means. Shared with every other screen (`screen_frame.hpp`).
  ScreenFrame frame_{client_};

  ConflictsModel model_;

  /// What the last `Render()` produced. Kept rather than rebuilt in `Draw`,
  /// which is `const` and is called by libtesla rather than by us.
  ConflictsView view_ = ConflictsView{};

  /// The link the model was last told about, so a session that came back is
  /// noticed once rather than restarting the screen every frame.
  Link link_ = Link::kOk;
};

}  // namespace rommsync::overlay
