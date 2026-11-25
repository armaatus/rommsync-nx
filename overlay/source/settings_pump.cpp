#include "settings_pump.hpp"

#include <utility>

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_settings_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

SettingsPump::SettingsPump(IpcClient& client) : client_(client) {}

void SettingsPump::Frame() {
  const Poller::Event event = poller_.Frame();
  switch (event.kind) {
    case Poller::Event::Kind::kNone:
      return;
    case Poller::Event::Kind::kUnreachable:
      ShowUnreachable(event.link);
      return;
    case Poller::Event::Kind::kStillApplying:
      // Out past the budget, or its answer lost to a call that took the line.
      // Neither is a failure: the write is the sysmodule's, and the next
      // `GetConfig` says what is in force (#299's rule).
      if (event.command == ipc::Command::kSetConfig) {
        change_ = ChangeState{};
        change_.applying = true;
      } else if (event.command == ipc::Command::kUnpair && !poller_.sending()) {
        // The `StartPair` before it was answered, so an attempt is running
        // whatever became of the old token, and the pairing screen is where
        // the user reads its code. Waiting here would strand the press.
        pairing_ = true;
      } else {
        repair_.applying = true;
      }
      Refresh();
      return;
    case Poller::Event::Kind::kSent:
      Answered(event.command, event.rc);
      return;
    case Poller::Event::Kind::kRead:
      Read(event.rc);
      return;
  }
}

void SettingsPump::Read(Result rc) {
  ipc::ConfigView config;
  if (R_SUCCEEDED(rc)) {
    rc = client_.Decode(ipc::DecodeConfigView, &config);
  }
  if (R_FAILED(rc)) {
    // `GetConfig` is documented never to fail -- an unconfigured console has a
    // configuration -- so a failure is the transport or a payload this build
    // cannot read, and which of the two is `Diagnose`'s to say.
    ShowUnreachable(frame_.Diagnose(rc));
    return;
  }
  config_ = std::move(config);
  if (!config_.config.configured()) {
    // The console lost the server the second press was going to pair with, so
    // the first press goes with it. Only `confirming`: an outcome from a press
    // that already happened is still what the user needs to read.
    repair_.confirming = false;
  }
  if (!poller_.sending()) {
    // A press whose answer was lost: this read is after it, and is the whole
    // of what can be said about it now.
    change_.applying = false;
    repair_.applying = false;
  }
  Refresh();
}

void SettingsPump::PressChange(config::Assignment assignment) {
  if (poller_.sending()) {
    return;
  }
  // A half-pressed "Re-pair" does not survive another press: its second press
  // would otherwise land after the user has moved on to something else.
  repair_ = RepairState{};
  // The sysmodule writes the card and re-reads it (M5-3, #30); this screen
  // sends the one assignment and draws the answer.
  ipc::ConfigEdit edit;
  edit.assignments.push_back(assignment);
  pending_ = std::move(assignment);
  poller_.Send(ipc::Command::kSetConfig, ipc::EncodeConfigEdit(edit));
  Refresh();
}

void SettingsPump::PressRepair() {
  if (view_.repair.state == ControlState::kBlocked || poller_.sending()) {
    // Refused here rather than sent to be refused there: the screen already
    // knows the answer (`SetRepair`), and the refusal is already under the
    // greyed prompt. The same rule the sync screen keeps for a blocked button.
    return;
  }
  // The answer to an earlier setting press is not what this button did.
  change_ = ChangeState{};
  if (!repair_.confirming) {
    // The first press only asks. The action discards a working pairing and this
    // console has no dialog to put in front of it.
    repair_.confirming = true;
    repair_.outcome = RepairOutcome::kNone;
    Refresh();
    return;
  }
  repair_.confirming = false;
  // The half that can refuse for free, first: a refused `StartPair` writes
  // nothing and touches no token, so asking before discarding costs a console
  // that cannot pair nothing at all (`settings_screen.hpp`).
  poller_.Send(ipc::Command::kStartPair, ipc::EncodeEmpty());
  Refresh();
}

void SettingsPump::Answered(ipc::Command command, Result rc) {
  if (command == ipc::Command::kSetConfig) {
    // A failing `Result` means the call did not happen, and a refused edit is a
    // successful call carrying `kInvalid` -- `ipc::WriteOutcome` says why it
    // has to be that way round.
    ipc::ConfigResult result;
    if (R_SUCCEEDED(rc)) {
      rc = client_.Decode(ipc::DecodeConfigResult, &result);
    }
    if (R_FAILED(rc)) {
      ShowUnreachable(frame_.Diagnose(rc));
      return;
    }
    change_ = ChangeState{true, std::move(pending_), std::move(result)};
    // The read that follows -- the `Poller` asks at once after a press -- is
    // what the row then shows: what the sysmodule has in force.
    Refresh();
    return;
  }

  repair_.applying = false;
  if (R_SUCCEEDED(rc) && command == ipc::Command::kStartPair) {
    auth::PairingStatus pairing;
    rc = client_.Decode(auth::ParsePairingStatus, &pairing);
  }
  if (R_FAILED(rc)) {
    ipc::Error error = ipc::Error::kOk;
    if (DecodeError(rc, &error)) {
      // A refusal the sysmodule named, which is not a transport failure --
      // drawing `kUnavailable` as "sys-rommsync is not running" is exactly
      // what `DecodeError` exists to prevent (#25). A refused `StartPair`
      // discarded nothing; a refused `Unpair` left the old token in place.
      repair_.outcome = command == ipc::Command::kStartPair ? RepairOutcomeFor(error)
                                                            : RepairOutcome::kUnpairFailed;
      Refresh();
      return;
    }
    ShowUnreachable(frame_.Diagnose(rc));
    return;
  }
  if (command == ipc::Command::kStartPair) {
    // A pairing really is starting, so the old token goes. Sent ahead of the
    // next read.
    poller_.Send(ipc::Command::kUnpair, ipc::EncodeEmpty());
    return;
  }
  // The code, the address and the countdown are the pairing screen's (#27),
  // and it polls `GetPairState` off the attempt that has just started.
  pairing_ = true;
}

bool SettingsPump::TakePairing() {
  const bool taken = pairing_;
  pairing_ = false;
  return taken;
}

void SettingsPump::ShowUnreachable(Link link) {
  repair_ = RepairState{};
  change_ = ChangeState{};
  view_ = RenderSettingsUnreachable(link, frame_.sysmodule_interface());
  ++revision_;
}

void SettingsPump::Refresh() {
  view_ = RenderSettings(config_, repair_, change_);
  ++revision_;
}

}  // namespace rommsync::overlay
