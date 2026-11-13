#include "sync_pump.hpp"

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_sync_actions.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

SyncPump::SyncPump(IpcClient& client) : client_(client) {}

void SyncPump::Frame() {
  const Poller::Event event = poller_.Frame();
  switch (event.kind) {
    case Poller::Event::Kind::kNone:
      return;
    case Poller::Event::Kind::kUnreachable:
      Unreachable(event.link);
      return;
    case Poller::Event::Kind::kStillApplying:
      // Out past the budget, or its answer lost to another screen's call. Not a
      // failure and not "not responding": the write is the sysmodule's, and the
      // next `Status` says what it changed (#299).
      last_ = LastCommand::Applying();
      Refresh();
      return;
    case Poller::Event::Kind::kSent:
      Answered(event.command, event.rc);
      return;
    case Poller::Event::Kind::kRead:
      break;
  }

  ipc::Status status;
  Result rc = event.rc;
  if (R_SUCCEEDED(rc)) {
    rc = client_.Decode(ipc::DecodeStatus, &status);
  }
  if (R_FAILED(rc)) {
    // `GetStatus` is documented never to fail, so a failure is the transport or
    // a payload this build cannot read -- and which of the two is `Diagnose`'s
    // to say, not this screen's.
    Unreachable(frame_.Diagnose(rc));
    return;
  }

  status_ = status;
  // The one thing a `Status` cannot say on its own, observed here because it is
  // a transition rather than a state: a tick that has not started yet and one
  // that has finished both report `sync_in_progress == false`, so "Sync
  // started" would come back when the sync ended. What to *do* about it is
  // still the view model's (`overlay_sync_actions.hpp`); this only records that
  // it happened.
  if (last_.kind == LastCommand::Kind::kSyncNow && PromisesTick(last_.sync) &&
      status_.sync_in_progress) {
    last_.sync_seen_running = true;
  }
  if (last_.kind == LastCommand::Kind::kApplying && !poller_.sending()) {
    // A press whose answer was lost: this `Status` is after it, and is the
    // whole of what can be said about it now.
    last_ = LastCommand{};
  }
  Refresh();
}

void SyncPump::PressSyncNow() {
  if (view_.sync_now.state == ControlState::kBlocked) {
    // Refused here rather than sent to be refused there: the screen already
    // knows which refusal it would get, and `overlay.sync_actions` holds that
    // prediction against `ipc::ServiceCore::SyncNow` itself.
    //
    // Nothing is written to `last_`, because the sentence is already on screen
    // and was before the press -- `Draw` paints `sync_now.refusal` under the
    // greyed prompt whenever the button is blocked. Recording it as a notice
    // too would draw the same words twice. This is not the silent no-op #24
    // forbids; the refusal is permanent rather than momentary.
    return;
  }
  // Sent on the next frame and answered on a later one. A press while one is
  // already out goes nowhere: the first is still being answered.
  poller_.Send(ipc::Command::kSyncNow, ipc::EncodeEmpty());
}

void SyncPump::PressToggle() {
  // The view model never blocks this control -- `SetEnabled` refuses no
  // boolean, so there is no refusal to predict (`overlay_sync_actions.hpp`) --
  // and `kInert` was handled by the caller. Anything else is live. Asked for
  // the opposite of what is drawn *now*, so a second press while the first is
  // out is not a second flip; it is refused by `Send`.
  poller_.Send(ipc::Command::kSetEnabled, ipc::EncodeEnabled(!view_.enabled));
}

void SyncPump::Answered(ipc::Command command, Result rc) {
  if (command == ipc::Command::kSyncNow) {
    ipc::SyncOutcome outcome = ipc::SyncOutcome::kAccepted;
    if (R_SUCCEEDED(rc)) {
      rc = client_.Decode(ipc::DecodeSyncOutcome, &outcome);
    }
    if (R_FAILED(rc)) {
      Unreachable(frame_.Diagnose(rc));
      return;
    }
    // Whatever it says, including `kAccepted`: the command returns as soon as
    // the tick is queued, so the next `Status` may not have `sync_in_progress`
    // set yet, and a press with nothing on screen is a press a user repeats.
    last_ = LastCommand::SyncNow(outcome);
    Refresh();
    return;
  }

  ipc::EnabledResult result;
  if (R_SUCCEEDED(rc)) {
    rc = client_.Decode(ipc::DecodeEnabledResult, &result);
  }
  if (R_FAILED(rc)) {
    // A failing `Result` means the call did not happen. A refused *write* is a
    // successful call carrying `kWriteFailed`, which is the whole reason the
    // outcome rides in the payload (`ipc::WriteOutcome`).
    Unreachable(frame_.Diagnose(rc));
    return;
  }
  // The state that took, read back off the new configuration -- never the state
  // that was asked for. Written into `status_` so the switch moves now rather
  // than on the next poll; the next `GetStatus` overwrites it with the same
  // fact.
  status_.enabled = result.enabled;
  last_ = LastCommand::SetEnabled(result.outcome, status_.enabled);
  Refresh();
}

void SyncPump::Unreachable(Link link) {
  // `last_` is dropped rather than kept: it is the answer to a press on a
  // session that is now gone, and a notice about it over "sys-rommsync is not
  // running" reads as two problems. Dropped on a failed press too, or the
  // previous press's answer would be put back by the next successful poll,
  // describing a command two actions ago.
  last_ = LastCommand{};
  view_ = RenderSyncActionsUnreachable(link, frame_.sysmodule_interface());
}

void SyncPump::Refresh() { view_ = RenderSyncActions(status_, last_); }

}  // namespace rommsync::overlay
