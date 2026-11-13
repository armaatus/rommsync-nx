#include "pairing_pump.hpp"

#include "ipc_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_pairing_view.hpp"
#include "rommsync/pairing.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

PairingPump::PairingPump(IpcClient& client) : client_(client) {}

bool PairingPump::Ready() {
  const Link link = frame_.Ready();
  if (link == Link::kOk) {
    return true;
  }
  view_ = RenderPairingUnreachable(link, frame_.sysmodule_interface());
  return false;
}

void PairingPump::Frame() {
  if (!Ready()) {
    blocked_ = false;
    return;
  }

  auth::PairingStatus status;
  const Result rc = client_.GetPairState(&status);
  if (R_FAILED(rc)) {
    blocked_ = false;
    // A payload this build cannot read is distinct from a sysmodule that is
    // gone, and distinct from a half-parsed status: the screen says so instead
    // of drawing defaulted fields as though they were an answer. Which of the
    // two it is is `Diagnose`'s to say.
    view_ = RenderPairingUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
    return;
  }

  if (blocked_) {
    // The refusal stands until the user presses the button again. `StartPair`
    // refuses *before* `PairingSession::Begin()` runs (`ipc::ServiceCore`), so
    // the session still reports whatever the last attempt left behind --
    // `kIdle` on a first Pair, `kExpired` or `kDenied` on a Start over -- and
    // every one of those would redraw over the reason one poll after the
    // press. `GetPairState` is still asked, so a sysmodule that goes away while
    // the refusal is up is still reported.
    return;
  }
  view_ = RenderPairing(status);
}

void PairingPump::PressStart() {
  blocked_ = false;
  if (!Ready()) {
    return;
  }

  auth::PairingStatus status;
  const Result rc = client_.StartPair(&status);
  if (R_SUCCEEDED(rc)) {
    // `kStarting`, almost always: the sysmodule hands the init to its own
    // thread and answers without waiting for it (`ipc::Engine::StartPairing`).
    view_ = RenderPairing(status);
    return;
  }
  if (rc == MalformedResponse()) {
    view_ = RenderPairingUnreachable(Link::kUnreadable);
    return;
  }

  // It refused, or the session died. See the note on `PressStart`.
  ipc::Status console;
  const Result status_rc = client_.GetStatus(&console);
  if (R_FAILED(status_rc)) {
    view_ = RenderPairingUnreachable(frame_.Diagnose(status_rc), frame_.sysmodule_interface());
    return;
  }
  view_ = RenderPairingBlocked(console.configured ? PairBlock::kRefused : PairBlock::kNoServer);
  blocked_ = true;
}

}  // namespace rommsync::overlay
