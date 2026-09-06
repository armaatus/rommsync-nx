// The overlay's status screen, decided here and drawn over there.
//
// `ovl-rommsync` is a thin client (overlay/AGENTS.md), and this is the half of
// "thin" that is still a decision: which sentence a never-paired console gets,
// what a download with no declared length shows instead of a percentage, and
// what goes where a timestamp would go on a console that has never synced. None
// of that needs a framebuffer, so none of it lives behind one -- `overlay/`
// takes a `StatusView` and turns its lines into libultrahand elements, and
// everything that could be wrong about the screen is `ctest -R overlay.status`.
//
// Hard rule 4 applies as it does to the rest of `core/`: no libnx header, no
// `Result`, and no libultrahand type. What crosses is strings and enums, so the
// renderer picks the colour for a `Tone` and this file never names one.
//
// **The guarantee this exists for**: every `Line::value` is non-empty in every
// state. An overlay that drew an empty string where "Never" belongs looks like
// an overlay that failed to read something, and a user cannot tell that apart
// from a console that has simply not synced yet.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rommsync/ipc.hpp"

namespace rommsync::overlay {

/// Whether there is a status to draw at all, and why not.
///
/// IPC-unreachable is a state of the screen rather than an error on top of one:
/// an overlay on a console where the sysmodule is not running has nothing to
/// poll and has to say so, and a toast over an empty status screen says it in
/// the one place a user will have stopped looking.
enum class Link {
  kOk,

  /// `smGetService` found no `rommsync` port. The sysmodule is not running --
  /// not installed, not enabled, or it aborted at boot.
  kNotRunning,

  /// It answered, and this build could not decode the answer. Reported as one
  /// state rather than as a half-filled `Status`, which is the whole point:
  /// three defaulted fields render as a working console with odd numbers.
  kUnreadable,

  /// `GetInterfaceVersion` disagreed with `ipc::kVersion`. A different sentence
  /// from `kUnreadable` because there is something the user can do about it,
  /// and because command 0's encoding is frozen precisely so this is knowable
  /// before anything else is decoded (`ipc.hpp`).
  kIncompatible,
};
const char* ToString(Link link);

/// How a line should read to the eye. The renderer maps these onto colours;
/// `core/` names none.
enum class Tone { kNeutral, kGood, kWarn, kBad };
const char* ToString(Tone tone);

/// One drawn row: a label on the left, a value on the right.
struct Line {
  std::string label;

  /// Never empty. See the header note.
  std::string value;

  Tone tone = Tone::kNeutral;
};

/// The download bar, or the absence of one.
struct Progress {
  enum class Kind {
    kNone,  ///< nothing is downloading; draw no bar

    /// A download the server declared no length for (#22). The bar moves
    /// without a percentage, and `permille` is meaningless: synthesising one
    /// from `bytes_done` alone is the way this gets got wrong.
    kIndeterminate,

    kFraction,
  };

  Kind kind = Kind::kNone;

  /// The file, never a path. Empty only when `kind` is `kNone`.
  std::string label;

  /// "12.1 MiB of 48.0 MiB", or just "12.1 MiB" when there is no total.
  std::string caption;

  /// 0..1000, and only meaningful for `kFraction`. Per mille rather than a
  /// float so a renderer that scales it to a pixel width does integer maths.
  int permille = 0;
};

/// Everything the status screen draws, and nothing about how.
struct StatusView {
  Link link = Link::kOk;

  /// The one sentence at the top. Never empty, in any state.
  std::string headline;

  /// What the user can do about it, or empty when there is nothing to do.
  std::string hint;

  /// The headline's tone.
  Tone tone = Tone::kNeutral;

  /// The rows under it, in draw order.
  std::vector<Line> lines;

  Progress progress;
};

/// The screen for a `Status` the sysmodule answered with.
///
/// `now_unix` is whole Unix seconds, the same clock `Status::last_sync_at` is
/// on, and it is a parameter rather than a call to the clock so the relative
/// time this renders is a fact a test can pin. A `now_unix` before
/// `last_sync_at` -- a console whose clock moved, which is ordinary on a Switch
/// that has been offline -- renders as "just now" rather than as a negative
/// interval.
StatusView Render(const ipc::Status& status, std::int64_t now_unix);

/// The screen for a sysmodule that could not be asked, or could not be
/// understood. `link` must not be `kOk`.
///
/// `sysmodule_interface` is what `GetInterfaceVersion` answered, and is shown
/// only for `kIncompatible`, where the two numbers are the whole diagnosis.
StatusView RenderUnreachable(Link link, std::uint32_t sysmodule_interface = 0);

/// What the SD card says about the two switches, for a console whose sysmodule
/// did not answer (M6-2, #33).
///
/// There are **two** switches and they mean different things: ovl-sysmodules'
/// boot toggle -- `atmosphere/contents/<TID>/flags/boot2.flag` -- decides
/// whether the process exists at all, and `[sync] enabled` in `config.ini`
/// decides whether a resident process syncs. An overlay that collapses them
/// hands the user a switch that does nothing, so the four states are drawn
/// apart: not installed, installed but not set to boot, running with sync off,
/// running with sync on. The first two are the ones only the card can tell
/// apart, and this is what carries them.
///
/// **Every field is the card's, read by the overlay, and none of it is live.**
/// The overlay owns no writes to either switch (`overlay/AGENTS.md`, and
/// `boot2.flag` is ovl-sysmodules' file): it reads them to explain a silence.
/// `core/` names no path here for hard rule 4's reason -- the SD prefix and the
/// title id belong to the side that has them -- so the peeks are the caller's
/// and this is the decision made about their answers.
struct CardState {
  /// `atmosphere/contents/<TID>/exefs.nsp` is on the card. False is the "not
  /// installed" state, and it is the one worth saying out loud: nothing else on
  /// this screen would tell a user that the zip never landed.
  bool installed = false;

  /// `atmosphere/contents/<TID>/flags/boot2.flag` is on the card, so
  /// Atmosphère launches the sysmodule at boot. Written by ovl-sysmodules and
  /// by nothing here.
  bool set_to_boot = false;

  /// `config.ini` was read. False means there is no configuration to report --
  /// a card with no `config/rommsync/config.ini` yet, or one that would not
  /// parse -- and `sync_enabled` says nothing in that case.
  bool config_read = false;

  /// `[sync] enabled`, as the file holds it. Only meaningful with
  /// `config_read`, and never drawn as the live state: the process this would
  /// describe is not running.
  bool sync_enabled = false;
};

/// The screen for a sysmodule that did not answer, with what the card adds.
///
/// `link` must not be `kOk`. For everything but `Link::kNotRunning` the card
/// says nothing the link does not already -- a sysmodule that answered
/// something unreadable is installed and running by definition -- so those
/// three are exactly `RenderUnreachable(link, sysmodule_interface)`.
///
/// For `kNotRunning` it is the difference between "the files are not on the
/// card" and "they are, and the boot toggle is off", which are one sentence
/// apart and a different thing to do about each.
StatusView RenderUnreachable(Link link, const CardState& card,
                             std::uint32_t sysmodule_interface = 0);

/// "4 minutes ago", "3 days ago", "Never" for `then_unix == 0`.
///
/// Published because it is what the screen is made of and because it is the
/// piece with the arithmetic in it; the M4 screens that show a time show this
/// one rather than each rounding differently.
std::string FormatRelativeTime(std::int64_t then_unix, std::int64_t now_unix);

/// "Finished", "Partly finished", "Failed", "Not yet" for a console that has
/// never synced.
///
/// Published for `FormatRelativeTime`'s reason: the sync screen (M4-2, #24)
/// names the last sync's outcome too, and a second copy of this switch is a
/// second place for the two screens to disagree about the same result.
/// `kPartial` is its own sentence there as it is here -- a tick that uploaded
/// four saves and failed the fifth is not a failed sync (`ipc::SyncResult`).
std::string SyncResultText(ipc::SyncResult result);
Tone SyncResultTone(ipc::SyncResult result);

/// "12.1 MiB". Binary units, one decimal above the byte, so a bar's caption and
/// a size in a list never disagree about the same number.
std::string FormatBytes(std::int64_t bytes);

}  // namespace rommsync::overlay
