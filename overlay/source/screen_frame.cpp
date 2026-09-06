#include "screen_frame.hpp"

#include <cstdint>

#include "ipc_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

tsl::Color ColorFor(Tone tone) {
  switch (tone) {
    case Tone::kGood:
      return tsl::gfx::Renderer::a(tsl::healthyRamTextColor);
    case Tone::kWarn:
      return tsl::gfx::Renderer::a(tsl::warningTextColor);
    case Tone::kBad:
      return tsl::gfx::Renderer::a(tsl::badRamTextColor);
    case Tone::kNeutral:
      break;
  }
  return tsl::gfx::Renderer::a(tsl::defaultTextColor);
}

tsl::Color MutedColor() { return tsl::gfx::Renderer::a(tsl::infoTextColor); }

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
  if (R_FAILED(client_.GetInterfaceVersion(&sysmodule_interface_))) {
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
  return rc == MalformedResponse() ? Link::kUnreadable : Reopen();
}

Link ScreenFrame::Reopen() {
  // The session is not trusted after a transport failure, so it is dropped and
  // re-opened here rather than on the next frame. See the note on `Diagnose`.
  client_.Close();
  version_checked_ = false;
  return R_SUCCEEDED(client_.Open()) ? Link::kUnreadable : Link::kNotRunning;
}

}  // namespace rommsync::overlay
