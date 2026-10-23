// The overlay's settings screen -- the whole effective configuration, every
// complaint the parser had about it, and the presses the screen owns --
// decided here and drawn over there.
//
// M4-4 (#26), and the place a bad `config.ini` stops being invisible. A console
// with no keyboard and no log a user can read has exactly one way to learn that
// line 9 was dropped, and this is it: `Diagnostic::Describe()` in front of a
// person, beside the value that took effect instead.
//
// The same split `overlay_status_view.hpp`, `overlay_sync_actions.hpp` and
// `overlay_library_model.hpp` have, for the same reason -- what a screen *says*
// is a decision, and a decision put inside a `tsl::Gui` is untestable until
// somebody has a console. `overlay/` turns a `SettingsView` into libultrahand
// elements; everything that could be wrong about the screen is
// `ctest -R overlay.settings`.
//
// **Nothing under `overlay/` writes `config.ini`** (docs/ARCHITECTURE.md). The
// write path is the sysmodule's -- `SetConfig` and `config::ApplyEdit`, M5-3
// (#30) -- and a row this screen changes in place carries the one assignment a
// press sends there (`SettingsRow::change`). Since M10-23 (#264) that is the
// settings that are a choice between a few words: `[sync] on_server_delete` and
// `[sources]`; since M10-31 (#271) also `[sync] map_state_slots`, a platform's
// `play_with`, and `[tico] account` -- a picker over the accounts the last tick
// found, one row each. `[sync] slot` is shown and not offered, the reason in
// its note. Every other row carries `editable` to mark where a value the write
// path takes *could* become editable, and no way to change it.
//
// **A `Diagnostic` about `server.url` never quotes the URL** (`config.hpp`), and
// this file may not helpfully re-add it: a URL is the one configured field that
// can carry a credential, and a screen that drew the value beside the complaint
// would undo the one precaution the parser takes. `overlay.settings` asserts it
// rather than leaving it reviewed, because it is the only leak this screen can
// cause.
//
// Hard rule 4 applies as it does to the rest of `core/`: no libnx header, no
// `Result`, and no libultrahand type. What crosses is strings and enums, so the
// renderer picks the colour for a `Tone` and this file never names one.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "rommsync/overlay_sync_actions.hpp"

namespace rommsync::overlay {

/// A screen this one can push.
///
/// **This overlay had no navigation at all until M4-4.** `main.cpp` opened on
/// the status screen and nothing pushed any other, so `SyncScreen` (#24),
/// `LibraryScreen` (#25) and `PairingScreen` (#27) all compiled and
/// `--gc-sections` dropped them from the image. #24, #25 and #27 each recorded
/// it and left it here rather than each writing a root menu in a parallel
/// worktree -- which is the merge conflict CLAUDE.md says to serialise around.
/// So the menu is one section of this screen, and this enum is what a row on it
/// means.
enum class Destination {
  kNone,  ///< the row opens nothing
  kSync,
  kLibrary,
  kPairing,

  /// What a sync overwrote, and putting it back (M7-1, #36).
  ///
  /// **The one row on this menu that is conditional.** `[sync] conflict_show`
  /// is documented as hiding the conflicts screen, and this is where it does
  /// it: with the setting off the row is not drawn, so the screen has no way in.
  /// The *recording* is untouched -- the sysmodule writes an entry and a backup
  /// either way, and turning the setting back on shows every one of them
  /// (conflict_log.hpp). The `[sync]` section below still reports the value, so
  /// a user who cannot find the screen can see why in the same place.
  kConflicts,

  /// The end of `rommsync.log`, read off the sysmodule (`GetLog`) rather than
  /// the card (M9-6, #209) -- the first thing a bug report needs, on the one
  /// console with no keyboard and no card reader.
  kLog,
};
const char* ToString(Destination destination);

/// What a row is, which is what a press on it does.
enum class SettingsRowKind {
  /// A menu row: a press opens `destination`.
  kNavigate,

  /// A scalar setting: `[sync] interval_min`, `[server] url`. Selectable only
  /// when it carries a `change`.
  kValue,

  /// One line of the folder map -- one `roms`/`saves`/`states` path, or the
  /// absence of any.
  kFolder,
};

/// One drawn row.
struct SettingsRow {
  SettingsRowKind kind = SettingsRowKind::kValue;

  /// Never empty, in any state. A row drawn with no text is indistinguishable
  /// from a screen that failed to read something (overlay/AGENTS.md).
  std::string label;

  /// Never empty either, and never a bare `config.ini` value where that would
  /// mislead: `interval_min == 0` reads as "only on boot and on demand" rather
  /// than as "every 0 minutes", and a `roms` key with no paths reads "nowhere"
  /// rather than as a blank the eye skips.
  std::string value;

  /// What is worth knowing about this row: which `roms` folder is written to,
  /// which of them are only checked, why a platform is skipped. Empty when
  /// there is nothing to add.
  ///
  /// **Never the value of `server.url`.** See the header note.
  std::string note;

  Tone tone = Tone::kNeutral;

  /// A press on this row does something: a menu row, or a row with a `change`.
  bool selectable = false;

  /// Where a press goes. `kNone` for everything that is not a menu row.
  Destination destination = Destination::kNone;

  /// What a press on this row sends to `SetConfig`: the setting's **next**
  /// value, in `config.ini`'s own words, so a few presses walk every value and
  /// come round again (#264). Empty on every row a press does not change.
  ///
  /// The next value rather than a picker because the overlay has no dialog,
  /// and because every setting offered this way is a choice between two or
  /// three words that are all safe to land on: a press that overshoots is
  /// undone by pressing again, and the sysmodule backs up before any save is
  /// removed whichever `on_server_delete` is in force.
  ///
  /// The one exception is `[tico] account` (#271), where a cycle would put
  /// every account in force on the way to the one wanted: there each account
  /// the card holds is a row, and its `change` names exactly it.
  std::optional<config::Assignment> change;

  /// What the A prompt says over this row -- "Open", "Set to Keep" -- or empty
  /// on a row a press does nothing on.
  std::string press_label;

  /// M5-3 (#30) can write this key. Marked, and offered only where `change`
  /// is set: the moment `overlay/` opens `config.ini` the invariant that the
  /// sysmodule owns writes is gone (docs/ARCHITECTURE.md).
  ///
  /// **The renderer deliberately draws nothing for it**, and that is not an
  /// omission: every row that is not a `kNavigate` is editable but `[sync]
  /// slot` (#271), so a marker would be on nearly every configuration row on
  /// the screen and would separate nothing. What separates the rows a press
  /// changes is the A prompt drawn over them (`press_label`). The flag is the
  /// model saying which keys `config::ApplyEdit` accepts, held by
  /// `overlay.settings` rather than by a screenshot.
  bool editable = false;
};

/// A run of rows under one heading.
struct SettingsSection {
  /// `Screens`, `[server]`, `[platform.snes]`. Never empty.
  std::string title;

  /// Why the section has no rows, or what is unusual about the ones it has.
  /// Empty when there is nothing to say.
  std::string note;

  Tone note_tone = Tone::kNeutral;

  /// Empty exactly when `note` explains why: a platform mapped nowhere, or a
  /// folder map too large to have been sent.
  std::vector<SettingsRow> rows;
};

/// What the last "Re-pair" press produced.
///
/// **Nothing here is a prediction.** The button sends `StartPair` *before*
/// `Unpair`, which is the opposite of the order docs/AUTH.md describes. #26 was
/// told to build `StartPairing` first *or* gate the button on it, and this was
/// the gate: `StartPairing` answered `kUnavailable` unconditionally, so a button
/// that discarded the token first would have left a console that could not pair
/// again from the overlay at all. M1-6 (#123) built it, and the order stays --
/// an attempt is still refusable, for want of a `server.url` or of an HTTP
/// transport, and the half that can refuse for free is the one to ask first.
enum class RepairOutcome {
  kNone,  ///< nothing has been pressed since the screen opened

  /// `StartPair` answered `kNotConfigured`. Nothing was discarded.
  kNotConfigured,

  /// `StartPair` answered `kUnavailable`: this build has no HTTP transport to
  /// reach the server with, so it cannot start a pairing. A console has one
  /// since M1-7 (#126), so this is the answer a build with the wiring removed
  /// gives. Nothing was discarded, which is the whole point of asking first.
  kUnavailable,

  /// `StartPair` refused for some other reason. Nothing was discarded.
  kRefused,

  /// The pairing started and `Unpair` then failed. The console still holds the
  /// token it had, which is the one outcome a user has to be told about --
  /// everything else leaves the console exactly as it was.
  kUnpairFailed,
};
std::string RepairOutcomeText(RepairOutcome outcome);
Tone RepairOutcomeTone(RepairOutcome outcome);

/// Which of the three "nothing was discarded" outcomes a refused `StartPair` is.
///
/// Here rather than in `settings_screen.cpp` for the reason the sentences are
/// (overlay/AGENTS.md): reading a refusal is a decision, and a decision inside
/// a `tsl::Gui` is untestable until somebody has a console. It is the same
/// shape `EnqueueRefusalText` has on the library screen.
///
/// `kNotConfigured` and `kUnavailable` are the two a user can act on -- one is a
/// missing `server.url`, the other a build with no transport to reach one with
/// -- and everything else is one sentence, because there is nothing
/// different to do about any of them. **`kOk` is `kRefused` too**: this is only
/// ever asked about a `StartPair` that failed, and a refusal that named no error
/// is still a refusal, not a success.
RepairOutcome RepairOutcomeFor(ipc::Error error);

/// Where the "Re-pair" button is between presses.
///
/// Carried in rather than latched inside the view, for `LastCommand`'s reason
/// (`overlay_sync_actions.hpp`): the view is a pure function of what the
/// sysmodule reports, and this is the one piece of it the sysmodule cannot
/// report.
struct RepairState {
  /// The button has been pressed once and is waiting for the second press.
  ///
  /// Two presses because the action is destructive and the console has no
  /// dialog: "Re-pair" discards a working pairing, and a single press on a
  /// screen a user is scrolling through is one thumb away from an evening of
  /// re-pairing.
  bool confirming = false;

  RepairOutcome outcome = RepairOutcome::kNone;
};

/// What the last press on a setting row produced (#264).
///
/// Carried in rather than latched inside the view, for `RepairState`'s reason:
/// the view is a pure function of what the sysmodule reports, and the answer
/// to one `SetConfig` is not part of the next `GetConfig`.
struct ChangeState {
  /// A change has been sent since the screen opened. Nothing is said until one.
  bool sent = false;

  /// What was sent: the row's `change` at the moment of the press.
  config::Assignment assignment;

  /// What `SetConfig` answered.
  ipc::ConfigResult result;
};

/// Everything the settings screen draws, and nothing about how.
struct SettingsView {
  Link link = Link::kOk;

  /// The one sentence at the top. Never empty, in any state.
  ///
  /// `configured() == false` is this sentence whatever else is wrong with the
  /// file: a console with no server does nothing at all, so no other complaint
  /// is the first thing to fix.
  std::string headline;

  /// What the user can do about it, or empty when there is nothing to do.
  std::string hint;

  /// The headline's tone.
  Tone tone = Tone::kNeutral;

  /// Every complaint the parser had, in the order `GetConfig` sent them, with
  /// `kError` first.
  ///
  /// `Line::label` is the severity (`config::ToString`) and `Line::value` is
  /// exactly `Diagnostic::Describe()` -- exactly, because the line number and
  /// the section are the difference between a fix and an evening, and a screen
  /// that reworded them would be a second grammar to keep in step.
  ///
  /// Bounded by `config::kMaxDiagnostics` with a final line saying how many did
  /// not fit. `GetConfig` has usually trimmed harder than that already
  /// (`ipc::kMaxDiagnosticsInPayload` is 8) and says so in a `kNotice` of its
  /// own, which is rendered like any other.
  std::vector<Line> complaints;

  /// The menu and the configuration, in draw order. Empty when `link` is not
  /// `kOk`: every row would be a value this overlay does not have.
  std::vector<SettingsSection> sections;

  /// The one action this screen owns. Never `kLive` on a console with no
  /// server, where the sysmodule would answer `kNotConfigured` anyway --
  /// blocked rather than absent, because it is still the control a user came
  /// here to find (#26).
  Button repair;

  /// What the last press produced -- on "Re-pair" or on a setting row -- or what
  /// the next "Re-pair" will do while a confirmation is pending. Empty when
  /// there is nothing to say.
  std::string notice;
  Tone notice_tone = Tone::kNeutral;
};

/// The screen for the configuration the sysmodule answered with.
///
/// `GetConfig` never fails -- an unconfigured console has a config, and it is
/// the one this screen most needs to draw (`ipc::ConfigView`) -- so there is no
/// arm here for "could not be read". A sysmodule that could not be *asked* is
/// `RenderSettingsUnreachable`.
SettingsView RenderSettings(const ipc::ConfigView& config, const RepairState& repair = {},
                            const ChangeState& change = {});

/// The screen for a sysmodule that could not be asked, or could not be
/// understood. `link` must not be `kOk`; the wording is the status screen's,
/// because it is the same diagnosis.
///
/// The menu is not drawn either. Every screen it reaches renders the same
/// sentence from the same session, so a row that pushed one would be a way to
/// read "sys-rommsync is not running" a second time.
SettingsView RenderSettingsUnreachable(Link link, std::uint32_t sysmodule_interface = 0);

/// "30 minutes", "1 minute", "only on boot and on demand" for `0`.
///
/// Published because it is the one value on this screen that is not the number
/// in the file, and because `0` is a documented setting rather than a mistake
/// (docs/CONFIG.md). The clamps are applied upstream (`config::ParseConfig`),
/// so this renders what it is given.
std::string FormatInterval(int interval_min);

}  // namespace rommsync::overlay
