// One frame of the conflicts screen's IPC: what `ConflictsScreen::update()`
// does, without the `tsl::Gui` around it (M9-6, #209).
//
// It was `ConflictsScreen::Pump` and `Send`, and it moved here for the reason
// `status_paint.cpp` exists: what is left in a `tsl::Gui` has never run, and
// "a slow restore does not hold the frame loop" is a claim about exactly this
// code -- the command it sends each frame, and what it does with the answer.
// `ctest -R overlay.restore` drives it through the real `IpcClient` against a
// restore that is still copying. It names no libultrahand type, which
// `overlay.portable` holds it to.
#pragma once

#include "ipc_client.hpp"
#include "rommsync/overlay_conflicts_view.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// The handshake, at most one command, and its answer told to `model`.
///
/// `link` is the link the model was last told about, kept by the caller
/// between frames so a session that came back is noticed once rather than
/// restarting the screen every frame.
void PumpConflicts(ScreenFrame& frame, IpcClient& client, ConflictsModel& model, Link& link);

}  // namespace rommsync::overlay
