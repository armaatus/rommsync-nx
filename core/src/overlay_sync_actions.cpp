#include "rommsync/overlay_sync_actions.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {
namespace {

Button Live(std::string label) {
  return Button{ControlState::kLive, std::move(label), std::string(), Tone::kNeutral};
}

Button Blocked(std::string label, ipc::SyncOutcome refused) {
  return Button{ControlState::kBlocked, std::move(label), SyncOutcomeText(refused),
                SyncOutcomeTone(refused)};
}

Button Inert(std::string label) {
  return Button{ControlState::kInert, std::move(label), std::string(), Tone::kNeutral};
}

/// What the switch is called, which is what it *does* rather than what it is:
/// "Auto-sync: On" is already a row, and a button beside it repeating the state
/// leaves a user guessing which way pressing it goes.
std::string ToggleLabel(bool enabled) {
  return enabled ? "Turn auto-sync off" : "Turn auto-sync on";
}

/// The headline, in the same remedy order `PredictSyncNow` walks: each branch
/// names the *first* thing standing between this console and a sync, so a
/// console with no server is never told its problem is a switch.
void SetHeadline(SyncActionsView* view, const ipc::Status& status, ipc::SyncOutcome outcome) {
  switch (outcome) {
    case ipc::SyncOutcome::kNotConfigured:
      view->headline = "No server set";
      view->hint = "Set server.url in config.ini";
      view->tone = Tone::kWarn;
      return;
    case ipc::SyncOutcome::kUnauthenticated:
      // Two sentences rather than one, for the reason `ipc::AuthState` has three
      // states: a console that has never paired needs the pairing flow, and one
      // whose token the server stopped accepting needs to be told that is what
      // happened -- "not paired" on a console that *was* sends the user looking
      // for a step they already did.
      if (status.auth == ipc::AuthState::kNeverPaired) {
        view->headline = "Not paired";
        view->hint = "Pair this console from Settings";
        view->tone = Tone::kWarn;
      } else {
        view->headline = "Sign-in expired";
        view->hint = "Pair this console again";
        view->tone = Tone::kBad;
      }
      return;
    case ipc::SyncOutcome::kDisabled:
      // The switch is off, and the sysmodule is running -- which is the whole
      // distinction #24 exists to keep. `RenderSyncActionsUnreachable` is the
      // other one, and it never says this.
      view->headline = "Sync is off";
      view->hint = "Turn auto-sync on, or sync once now";
      view->tone = Tone::kWarn;
      return;
    case ipc::SyncOutcome::kAlreadyRunning:
      view->headline = "Syncing";
      view->hint = "This tick is running now";
      view->tone = Tone::kNeutral;
      return;
    case ipc::SyncOutcome::kAccepted:
      break;
  }
  view->headline = "Ready to sync";
  view->tone = Tone::kGood;
}

/// The two rows. Both are always drawn, including when the value is the boring
/// one: a screen that omits "Sync" until something has happened looks like a
/// screen that failed to read it (`overlay_status_view.hpp`).
void AddRows(SyncActionsView* view, const ipc::Status& status) {
  view->lines.push_back(Line{"Auto-sync", status.enabled ? "On" : "Off",
                             status.enabled ? Tone::kGood : Tone::kNeutral});
  view->lines.push_back(
      Line{"Sync",
           status.sync_in_progress ? std::string("Running now")
                                   : SyncResultText(status.last_sync_result),
           status.sync_in_progress ? Tone::kNeutral : SyncResultTone(status.last_sync_result)});
}

/// What the last press has to say for itself.
///
/// A `SetEnabled` that took says nothing: the switch having moved is the
/// answer, and a "that worked" beside it is a line a user learns to stop
/// reading. Everything else is a sentence, because a press that changed nothing
/// and said nothing is the silent no-op #24 forbids.
void SetNotice(SyncActionsView* view, const LastCommand& last) {
  switch (last.kind) {
    case LastCommand::Kind::kNone:
      return;
    case LastCommand::Kind::kSetEnabled:
      switch (last.write) {
        case ipc::WriteOutcome::kApplied:
          return;
        case ipc::WriteOutcome::kWriteFailed:
          view->notice = "That did not take: config.ini could not be written";
          break;
        case ipc::WriteOutcome::kInvalid:
          // Reachable, and not treated as a bug: a boolean is always a legal
          // `[sync] enabled`, but a `config.ini` the sysmodule cannot read
          // answers this, and both outcomes leave the switch where it was
          // (M5-3, #30).
          view->notice = "That did not take: the sysmodule refused it";
          break;
      }
      view->notice_tone = Tone::kBad;
      return;
    case LastCommand::Kind::kSyncNow:
      // Including `kAccepted`. `SyncNow` returns as soon as the tick is queued,
      // so the very next `Status` may not have `sync_in_progress` set yet -- and
      // a press with nothing on screen for two frames is a press a user repeats.
      view->notice = SyncOutcomeText(last.sync);
      view->notice_tone = SyncOutcomeTone(last.sync);
      return;
  }
}

}  // namespace

const char* ToString(ControlState state) {
  switch (state) {
    case ControlState::kLive:
      return "live";
    case ControlState::kBlocked:
      return "blocked";
    case ControlState::kInert:
      return "inert";
  }
  return "unknown";
}

ipc::SyncOutcome PredictSyncNow(const ipc::Status& status) {
  // The order `ipc::ServiceCore::SyncNow` walks, and it is the order rather than
  // the checks that matters: reading the same four fields in a different order
  // would tell a console with no server that its problem is a switch.
  // `overlay.sync_actions` holds this against `ServiceCore` itself.
  if (!status.configured) {
    return ipc::SyncOutcome::kNotConfigured;
  }
  if (status.auth != ipc::AuthState::kPaired) {
    return ipc::SyncOutcome::kUnauthenticated;
  }
  if (!status.enabled) {
    return ipc::SyncOutcome::kDisabled;
  }
  // `Engine::RequestSync` returns false for exactly this, and `Status` carries
  // the same fact so the screen can draw it without pressing anything
  // (`ipc::Status::sync_in_progress`).
  if (status.sync_in_progress) {
    return ipc::SyncOutcome::kAlreadyRunning;
  }
  return ipc::SyncOutcome::kAccepted;
}

std::string SyncOutcomeText(ipc::SyncOutcome outcome) {
  switch (outcome) {
    case ipc::SyncOutcome::kAccepted:
      return "Sync started";
    case ipc::SyncOutcome::kAlreadyRunning:
      return "A sync is already running";
    case ipc::SyncOutcome::kNotConfigured:
      return "No server set: set server.url in config.ini";
    case ipc::SyncOutcome::kUnauthenticated:
      return "This console is not paired";
    case ipc::SyncOutcome::kDisabled:
      return "Auto-sync is off";
  }
  return "The sysmodule did not say";
}

Tone SyncOutcomeTone(ipc::SyncOutcome outcome) {
  switch (outcome) {
    case ipc::SyncOutcome::kAccepted:
      return Tone::kGood;
    case ipc::SyncOutcome::kAlreadyRunning:
      // Nothing is wrong: the thing the user asked for is already happening.
      return Tone::kNeutral;
    case ipc::SyncOutcome::kNotConfigured:
    case ipc::SyncOutcome::kUnauthenticated:
    case ipc::SyncOutcome::kDisabled:
      return Tone::kWarn;
  }
  return Tone::kWarn;
}

SyncActionsView RenderSyncActions(const ipc::Status& status, const LastCommand& last) {
  SyncActionsView view;
  view.link = Link::kOk;
  view.enabled = status.enabled;

  const ipc::SyncOutcome outcome = PredictSyncNow(status);
  SetHeadline(&view, status, outcome);

  // The switch is live on every console the sysmodule answers on, including an
  // unconfigured one: `SetEnabled` refuses no boolean, so greying it here would
  // withhold a write that takes and would draw a control that lies about what
  // pressing it does.
  view.toggle = Live(ToggleLabel(status.enabled));

  view.sync_now = outcome == ipc::SyncOutcome::kAccepted ? Live("Sync now")
                                                         : Blocked("Sync now", outcome);

  AddRows(&view, status);
  SetNotice(&view, last);
  return view;
}

SyncActionsView RenderSyncActionsUnreachable(Link link, std::uint32_t sysmodule_interface) {
  // The headline, hint and tone are the status screen's, because it is the same
  // diagnosis and a console that says two different things about one missing
  // sysmodule reads as two problems.
  const StatusView diagnosis = RenderUnreachable(link, sysmodule_interface);

  SyncActionsView view;
  view.link = diagnosis.link;
  view.headline = diagnosis.headline;
  view.hint = diagnosis.hint;
  view.tone = diagnosis.tone;

  // Both inert, and *not* drawn as a switch that is off: `enabled` is a fact
  // this overlay does not have, and drawing "off" for "no answer" is what hides
  // a sysmodule that failed to start behind one a user paused on purpose (#24).
  view.enabled = false;
  view.toggle = Inert("Auto-sync");
  view.sync_now = Inert("Sync now");

  // No rows at all -- every one of them would be a value this overlay does not
  // have, and a zero the user cannot tell from a real one is the failure the
  // whole `Link` enum exists to avoid.
  return view;
}

}  // namespace rommsync::overlay
