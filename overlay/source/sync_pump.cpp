#include "sync_pump.hpp"

#include "ipc_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_sync_actions.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

SyncPump::SyncPump(IpcClient& client) : client_(client) {}

void SyncPump::Frame() {
  // The port and the version handshake, both of which every screen needs and
  // none of which is this screen's own (`screen_frame.hpp`).
  const Link link = frame_.Ready();
  if (link != Link::kOk) {
    // Dropped rather than kept: `last_` is the answer to a press on a session
    // that is now gone, and a notice about it over "sys-rommsync is not
    // running" reads as two problems.
    last_ = LastCommand{};
    view_ = RenderSyncActionsUnreachable(link, frame_.sysmodule_interface());
    return;
  }

  ipc::Status status;
  const Result rc = client_.GetStatus(&status);
  if (R_FAILED(rc)) {
    // `GetStatus` is documented never to fail, so a failure is the transport or
    // a payload this build cannot read -- and which of the two is `Diagnose`'s
    // to say, not this screen's.
    last_ = LastCommand{};
    view_ = RenderSyncActionsUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
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

  ipc::SyncOutcome outcome = ipc::SyncOutcome::kAccepted;
  const Result rc = client_.SyncNow(&outcome);
  if (R_FAILED(rc)) {
    // Dropped, not left: the previous press's answer would otherwise survive
    // the failure and be put back by the next successful poll, describing a
    // command two actions ago.
    last_ = LastCommand{};
    view_ = RenderSyncActionsUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
    return;
  }
  // Whatever it says, including `kAccepted`: the command returns as soon as the
  // tick is queued, so the next `Status` may not have `sync_in_progress` set
  // yet, and a press with nothing on screen for two frames is a press a user
  // repeats.
  last_ = LastCommand::SyncNow(outcome);
  Refresh();
}

void SyncPump::PressToggle() {
  // The view model never blocks this control -- `SetEnabled` refuses no
  // boolean, so there is no refusal to predict (`overlay_sync_actions.hpp`) --
  // and `kInert` was handled by the caller. Anything else is live.
  ipc::EnabledResult result;
  const Result rc = client_.SetEnabled(!view_.enabled, &result);
  if (R_FAILED(rc)) {
    // A failing `Result` means the call did not happen. A refused *write* is a
    // successful call carrying `kWriteFailed`, which is the whole reason the
    // outcome rides in the payload (`ipc::WriteOutcome`). `last_` goes for the
    // reason it goes in `PressSyncNow`.
    last_ = LastCommand{};
    view_ = RenderSyncActionsUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
    return;
  }

  // The state that took, read back off the new configuration -- never the state
  // that was asked for. Written into `status_` so the switch moves on this
  // frame rather than on the next poll; the next `GetStatus` overwrites it with
  // the same fact.
  status_.enabled = result.enabled;
  last_ = LastCommand::SetEnabled(result.outcome, status_.enabled);
  Refresh();
}

void SyncPump::Refresh() { view_ = RenderSyncActions(status_, last_); }

}  // namespace rommsync::overlay
