// The library browser's IPC: what `LibraryScreen::update()` and its destructor
// send, without the `tsl::Gui` around them (M11-1, #285).
//
// Split out of `library_screen.cpp` for `conflicts_pump.cpp`'s reason: what is
// left in a `tsl::Gui` has never run, and the claim #285 is about -- a screen
// destroyed after the overlay has stopped opens no session -- is a claim about
// exactly this code. `ctest -R overlay.cycles` destroys a library holding a
// cursor on every exit path libtesla's loop has. It names no libultrahand
// type, which `overlay.portable` holds it to.
#pragma once

#include "ipc_client.hpp"
#include "rommsync/overlay_library_model.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// The handshake, at most one command, and its answer told to `model`.
///
/// `link` is the link the model was last told about, kept by the caller
/// between frames so a session that came back is noticed once rather than
/// restarting the browser every frame.
void PumpLibrary(ScreenFrame& frame, IpcClient& client, LibraryBrowserModel& model, Link& link);

/// The screen's teardown: every cursor the model still holds closed, best
/// effort, because a `ListEnd` that never arrives costs the sysmodule one of a
/// small number of cursor slots until its TTL reclaims it (#31).
void CloseLibrary(ScreenFrame& frame, IpcClient& client, LibraryBrowserModel& model);

}  // namespace rommsync::overlay
