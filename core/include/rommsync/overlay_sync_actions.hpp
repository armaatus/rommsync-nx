// The overlay's sync screen -- the switch and "Sync now" -- decided here and
// drawn over there.
//
// The same split as `overlay_status_view.hpp` and `overlay_pairing_view.hpp`,
// for the same reason: what a screen *says* is a decision, and a decision put
// inside a `tsl::Gui` is untestable until someone has a console. So `overlay/`
// takes a `SyncActionsView` and turns it into libultrahand elements, and
// everything that could be wrong about the screen is
// `ctest -R overlay.sync_actions`.
//
// What is a decision here is **which control may be pressed**. A "Sync now" on
// a console that would refuse it is a spinner that never moves; a greyed one on
// a console that would have accepted it is a button that lies. Both are
// decided by `PredictSyncNow`, which walks the same remedy order
// `ipc::ServiceCore::SyncNow` walks -- and the test holds the two against each
// other rather than against a second copy of the table, because a screen that
// predicts a refusal the sysmodule does not give is the failure this file
// exists to prevent.
//
// **Two different "off"s, and keeping them apart is the whole point** (#24).
// ovl-sysmodules toggles the boot flag under `/atmosphere/contents/<TID>/flags/`
// and the process then does not exist; this switch is a runtime pause, with the
// sysmodule resident, idle, and still answering IPC. Rendering both as
// "disabled" hides a sysmodule that failed to start, so they are `Link` and
// `enabled` here and they never collapse into one sentence. Nothing in this
// file, and nothing under `overlay/`, reads or writes that flag --
// compatibility with it is M6-2 (#33).
//
// **The screen writes nothing.** The sysmodule owns `config.ini`
// (docs/ARCHITECTURE.md); this decides what to ask for and what the answer
// looked like. The per-emulator toggles ARCHITECTURE.md lists on this screen
// are config values and belong to the settings screen (#26) and the write path
// (#30); there is no second config writer here.
//
// Hard rule 4 applies as it does to the rest of `core/`: no libnx header, no
// `Result`, and no libultrahand type. What crosses is strings and enums, so the
// renderer picks the colour for a `Tone` and this file never names one.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

/// Whether a control may be pressed, and what a press that may not does.
enum class ControlState {
  /// Pressing it sends the command.
  kLive,

  /// Pressing it sends nothing and draws `Button::refusal`. The command would
  /// be refused, and the screen already knows which refusal -- sending it to
  /// hear the same answer back is a round trip whose only effect is to make the
  /// sentence arrive a frame later.
  kBlocked,

  /// There is nothing to press against: the sysmodule could not be reached, so
  /// there is no state to change and no tick to ask for. The headline is
  /// already saying why, which is why this one carries no sentence of its own --
  /// a second copy of "sys-rommsync is not running" beside the first reads as
  /// two different problems.
  kInert,
};
const char* ToString(ControlState state);

/// One control: what it is called, whether it may be pressed, and the sentence
/// a press that may not gets.
struct Button {
  ControlState state = ControlState::kInert;

  /// What the button says. Never empty, in any state -- a control drawn with no
  /// label is indistinguishable from a screen that failed to read something.
  std::string label;

  /// Why a press would be refused. Non-empty exactly when `state` is
  /// `kBlocked`: a refusal is a sentence on screen, never a silent no-op (#24).
  std::string refusal;

  Tone refusal_tone = Tone::kWarn;
};

/// The answer to the last command this screen sent, so the screen can say what
/// pressing did.
///
/// It is carried in rather than latched inside the view because the view is a
/// pure function of what the sysmodule reports: `SyncNow` returns as soon as
/// the tick is queued, so the `Status` on the very next frame may not have
/// `sync_in_progress` set yet, and a press with no visible effect for two
/// frames is a button a user presses twice.
struct LastCommand {
  enum class Kind {
    kNone,        ///< nothing has been pressed since the screen opened
    kSetEnabled,  ///< `SetEnabled` answered; `write` is what it did
    kSyncNow,     ///< `SyncNow` answered; `sync` is what it did
  };

  Kind kind = Kind::kNone;

  /// `kSetEnabled` only. `kApplied` draws no notice at all -- the switch having
  /// moved *is* the feedback, and a "that worked" beside it is a line a user
  /// learns to stop reading. Anything else is "that did not take", including
  /// `kInvalid`: a boolean is always a legal `[sync] enabled`, but a
  /// `config.ini` the sysmodule cannot read answers with one, and both leave the
  /// switch where it was (M5-3, #30).
  ipc::WriteOutcome write = ipc::WriteOutcome::kApplied;

  /// `kSetEnabled` only: `Status::enabled` as it stood when that answer came
  /// back.
  ///
  /// It is what makes a refusal a sentence rather than a *permanent* one. The
  /// notice stands while it is still true, and a later `Status` that disagrees
  /// means the switch has moved since -- by a retry, by the settings screen
  /// (#26), by anything -- so "that did not take" is no longer about the state
  /// the screen is showing, and a sentence a user cannot act on is worse than
  /// none.
  bool enabled_then = false;

  /// `kSyncNow` only. Every one of the five is drawn, including `kAccepted` --
  /// which is dropped again as soon as `sync_in_progress` makes it visible, so
  /// "Sync started" does not stand over a tick that finished ten minutes ago.
  ipc::SyncOutcome sync = ipc::SyncOutcome::kAccepted;

  /// `SyncNow` answered `outcome`.
  static LastCommand SyncNow(ipc::SyncOutcome outcome);

  /// `SetEnabled` answered `outcome`, over a console reporting `enabled_now`.
  ///
  /// Named rather than braced because the two arms are exclusive: a positional
  /// `LastCommand{Kind::kSyncNow, kApplied, false, outcome}` has to fill in a
  /// `WriteOutcome` that means nothing, and a filler value is one somebody
  /// eventually reads.
  static LastCommand SetEnabled(ipc::WriteOutcome outcome, bool enabled_now);
};

/// Everything the sync screen draws, and nothing about how.
struct SyncActionsView {
  Link link = Link::kOk;

  /// The one sentence at the top. Never empty, in any state.
  std::string headline;

  /// What the user can do about it, or empty when there is nothing to do.
  std::string hint;

  /// The headline's tone.
  Tone tone = Tone::kNeutral;

  /// `[sync] enabled` **as the sysmodule reports it**, never as anything was
  /// asked for. The switch is drawn from this, which is why `SetEnabled`
  /// answers with the effective state (`ipc::EnabledResult`): a screen that
  /// drew what it asked for would show a switch that did not move.
  ///
  /// False when `link` is not `kOk`, where it means nothing -- read
  /// `toggle.state` first.
  bool enabled = false;

  /// The switch. Live whenever the sysmodule can be reached: a boolean is
  /// always a legal `[sync] enabled` and `SetEnabled` refuses none, so greying
  /// it on an unconfigured console would withhold a write that takes.
  Button toggle;

  /// "Sync now". Blocked in exactly the four cases `SyncNow` refuses.
  Button sync_now;

  /// What the last press produced, or empty when there is nothing to say.
  /// `LastCommand` decides which.
  std::string notice;
  Tone notice_tone = Tone::kNeutral;

  /// The rows under the headline, in draw order. Empty when `link` is not
  /// `kOk`: every one of them would be a value this overlay does not have.
  std::vector<Line> lines;
};

/// What `SyncNow` would answer for this console.
///
/// The same four checks in the same order as `ipc::ServiceCore::SyncNow`, which
/// is not the order the fields happen to be in: each answer is the *first*
/// thing the user has to fix, so a console with no server is never told its
/// problem is a switch. `overlay.sync_actions` pins this against `ServiceCore`
/// itself rather than against a restatement of the table.
///
/// `kAccepted` means only that the command would be accepted -- this predicts
/// nothing about how the sync then goes.
ipc::SyncOutcome PredictSyncNow(const ipc::Status& status);

/// The sentence for each `SyncOutcome`, and how it should read.
///
/// Published because the screen refuses a press locally rather than sending a
/// command it knows the answer to: a refusal the screen predicted and one the
/// sysmodule answered with must be the same words, or the same console says two
/// different things about the same problem depending on a race.
std::string SyncOutcomeText(ipc::SyncOutcome outcome);
Tone SyncOutcomeTone(ipc::SyncOutcome outcome);

/// The screen for a `Status` the sysmodule answered with, and the answer to the
/// last thing pressed.
SyncActionsView RenderSyncActions(const ipc::Status& status, const LastCommand& last = {});

/// The screen for a sysmodule that could not be asked, or could not be
/// understood. `link` must not be `kOk`; the wording is the status screen's,
/// because it is the same diagnosis.
///
/// Both controls come back `kInert`, which is a different screen from
/// `enabled == false`: one says the sysmodule is not there, the other says it
/// is there and paused (#24).
SyncActionsView RenderSyncActionsUnreachable(Link link, std::uint32_t sysmodule_interface = 0);

}  // namespace rommsync::overlay
