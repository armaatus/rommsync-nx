#include "pairing_pump.hpp"

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_pairing_view.hpp"
#include "rommsync/pairing.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

PairingPump::PairingPump(IpcClient& client) : client_(client) {}

void PairingPump::Frame() {
  const Poller::Event event = poller_.Frame();
  switch (event.kind) {
    case Poller::Event::Kind::kNone:
      return;
    case Poller::Event::Kind::kUnreachable:
      Unreachable(event.link);
      return;
    case Poller::Event::Kind::kStillApplying: {
      // A `StartPair` still out past the budget. `kStarting` is what the
      // sysmodule answers it with anyway -- it hands the init to its own thread
      // -- and it is the state that exists so a press in flight does not read
      // as a button that did not register (#299).
      if (event.command == ipc::Command::kStartPair) {
        auth::PairingStatus starting;
        starting.state = auth::PairingState::kStarting;
        view_ = RenderPairing(starting);
      }
      return;
    }
    case Poller::Event::Kind::kSent:
      Answered(event.command, event.rc);
      return;
    case Poller::Event::Kind::kRead:
      break;
  }

  auth::PairingStatus status;
  Result rc = event.rc;
  if (R_SUCCEEDED(rc)) {
    rc = client_.Decode(auth::ParsePairingStatus, &status);
  }
  if (R_FAILED(rc)) {
    // A payload this build cannot read is distinct from a sysmodule that is
    // gone, and distinct from a half-parsed status: the screen says so instead
    // of drawing defaulted fields as though they were an answer. Which of the
    // two it is is `Diagnose`'s to say.
    Unreachable(frame_.Diagnose(rc));
    return;
  }
  if (blocked_ || poller_.sending()) {
    // A refusal stands until the user presses the button again. `StartPair`
    // refuses *before* `PairingSession::Begin()` runs (`ipc::ServiceCore`), so
    // the session still reports whatever the last attempt left behind --
    // `kIdle` on a first Pair, `kExpired` or `kDenied` on a Start over -- and
    // every one of those would redraw over the reason one poll after the
    // press. `GetPairState` is still asked, so a sysmodule that goes away while
    // the refusal is up is still reported. And a read that was out when the
    // button was pressed is from before the press.
    return;
  }
  view_ = RenderPairing(status);
}

void PairingPump::PressStart() {
  blocked_ = false;
  poller_.Send(ipc::Command::kStartPair, ipc::EncodeEmpty());
}

void PairingPump::Answered(ipc::Command command, Result rc) {
  if (command == ipc::Command::kStartPair) {
    auth::PairingStatus status;
    if (R_SUCCEEDED(rc)) {
      rc = client_.Decode(auth::ParsePairingStatus, &status);
      if (R_SUCCEEDED(rc)) {
        // `kStarting`, almost always: the sysmodule hands the init to its own
        // thread and answers without waiting for it.
        view_ = RenderPairing(status);
        return;
      }
    }
    if (rc == MalformedResponse()) {
      view_ = RenderPairingUnreachable(Link::kUnreadable);
      return;
    }
    // It refused, or the session died. See the note on `PressStart`: asked
    // next, ahead of the next read.
    poller_.Send(ipc::Command::kGetStatus, ipc::EncodeEmpty());
    return;
  }

  // The `GetStatus` that tells a refusal from a dead session.
  ipc::Status console;
  if (R_SUCCEEDED(rc)) {
    rc = client_.Decode(ipc::DecodeStatus, &console);
  }
  if (R_FAILED(rc)) {
    Unreachable(frame_.Diagnose(rc));
    return;
  }
  view_ = RenderPairingBlocked(console.configured ? PairBlock::kRefused : PairBlock::kNoServer);
  blocked_ = true;
}

void PairingPump::Unreachable(Link link) {
  blocked_ = false;
  view_ = RenderPairingUnreachable(link, frame_.sysmodule_interface());
}

}  // namespace rommsync::overlay
