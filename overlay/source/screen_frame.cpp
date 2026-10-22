#include "screen_frame.hpp"

#include <cstdint>
#include <string>

#include "draw_list.hpp"
#include "ipc_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

Link ScreenFrame::Ready() {
  if (!client_.open()) {
    if (R_FAILED(client_.Open())) {
      // No `rommsync` port. Not installed, not enabled, or it aborted at boot --
      // and it is the state a user who forgot to switch it on is in, so it is
      // drawn as a screen rather than reported as an error.
      return Link::kNotRunning;
    }
    version_checked_ = false;
  }
  if (version_checked_) {
    return Link::kOk;
  }

  sysmodule_interface_ = 0;
  const Result rc = client_.GetInterfaceVersion(&sysmodule_interface_);
  if (rc == NotResponding()) {
    // Nothing came back in time. The session is the worker's until it does, so
    // it is neither reopened nor judged: the next frame asks again (#209).
    return Link::kNotResponding;
  }
  if (R_FAILED(rc)) {
    // Not "not running": the port answered, so something is there. Which of the
    // two sentences is true is decided the same way a failed typed call decides
    // it -- by whether the port is still openable.
    return Reopen();
  }
  if (sysmodule_interface_ != ipc::kVersion) {
    // Left unlatched, so this is re-derived on every frame rather than
    // remembered: a user who exits the overlay, updates the sysmodule and comes
    // back gets a working screen without rebooting.
    return Link::kIncompatible;
  }
  version_checked_ = true;
  return Link::kOk;
}

Link ScreenFrame::Diagnose(Result rc) {
  // It answered, and this build could not read the answer. Distinct from a
  // sysmodule that is gone, and distinct from a half-parsed payload: the screen
  // says so instead of drawing defaulted fields as though they were numbers
  // (`overlay_status_view.hpp`).
  if (rc == MalformedResponse()) {
    return Link::kUnreadable;
  }
  // It did not answer at all, and the call may still be out. **Not a reopen**:
  // `Reopen` closes the session a worker is still dispatching on, and a call
  // that is merely slow would come back to a handle that is gone. It clears by
  // itself on the first call that is answered (#209).
  if (rc == NotResponding()) {
    return Link::kNotResponding;
  }
  return Reopen();
}

Link ScreenFrame::Reopen() {
  // The session is not trusted after a transport failure, so it is dropped and
  // re-opened here rather than on the next frame. See the note on `Diagnose`.
  client_.Close();
  version_checked_ = false;
  return R_SUCCEEDED(client_.Open()) ? Link::kUnreadable : Link::kNotRunning;
}

}  // namespace rommsync::overlay
