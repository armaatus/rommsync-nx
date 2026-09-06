// The overlay's conflicts screen -- what a sync overwrote, and putting it back.
//
// The same split `overlay_status_view.hpp`, `overlay_pairing_view.hpp`,
// `overlay_sync_actions.hpp` and `overlay_library_model.hpp` have, for the same
// reason: what a screen *says* is a decision, and a decision put inside a
// `tsl::Gui` is untestable until someone has a console.
// `overlay/source/conflicts_screen.cpp` turns a `ConflictsView` into
// libultrahand elements and sends what `Next()` asks for; everything that could
// be wrong about the screen is `ctest -R overlay.conflicts`.
//
// Like the library browser this one has **state** -- a page, a selection, a
// detail view and a confirmation -- so it is a small state machine rather than a
// `Render(status)`.
//
// ## The loop
//
//   Command command = model.Next();     // kNone: nothing to do this frame
//   ... send it ...
//   model.OnPage(page);                 // ListConflicts answered
//   model.OnRestored(report);           // RestoreBackup answered
//   model.OnRefused(error);             // the sysmodule refused it
//   model.OnUnreachable(link);          // the transport, or the handshake
//
// One command per frame and one in flight at a time, which is what the overlay
// can do: it draws on one thread and a `cmif` call from it is synchronous.
//
// ## What it never does
//
// **It never restores anything itself.** The overlay holds no logic
// (docs/ARCHITECTURE.md §2), and a restore is a save overwrite that owes a
// backup first -- so it lives on the service (`conflicts::Restore`) and this
// asks for it by id. The screen renders and calls.
//
// **It never decides whether the screen exists.** `[sync] conflict_show` is
// read off `GetConfig` by the *settings* screen, which is the root menu: with it
// off, the row that pushes this screen is not drawn. Nothing here consults it,
// and nothing in the sysmodule filters the history for it -- see
// conflict_log.hpp for why the recording is never the thing that gets turned
// off.
//
// ## The sentence the screen owes the user
//
// **The server is the source of truth** (hard rule 3). A restore writes the
// local file and nothing else; the next negotiation arbitrates and will most
// likely plan an `upload`. So the confirmation says what is actually being
// chosen -- which bytes to *offer* -- rather than implying the user has
// overruled RomM. `RestoreMeaning()` is that sentence, published so the screen,
// the confirmation and the test cannot each invent their own.
//
// Hard rule 4 applies as it does to the rest of `core/`: no libnx header, no
// libultrahand type. What crosses is strings and enums, so the renderer picks
// the colour for a `Tone` and this file names none.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/conflict_log.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

/// What the screen is showing.
enum class ConflictsMode {
  /// The entries, newest first, one line each.
  kList,

  /// One entry, both sides of it.
  kDetail,

  /// The detail, with "restore these bytes?" over it. A separate mode rather
  /// than a flag because it is what makes the restore a *confirmed* press: the
  /// screen the user is looking at when they press A the second time is the one
  /// naming the file that is about to be replaced.
  kConfirm,
};
const char* ToString(ConflictsMode mode);

/// Whether A on this row would restore anything, and why not.
///
/// Decided before a press, off the row the sysmodule sent, so a screen never
/// offers a restore it already knows will fail -- `PredictEnqueue`'s shape
/// (`overlay_library_model.hpp`) applied to the one command here that writes.
enum class Restorability {
  /// The backup is named and the sysmodule found it on the card.
  kReady,

  /// Nothing was overwritten, so there is nothing to put back: a state both
  /// sides kept. Not a failure -- it is the policy working.
  kNothingToRestore,

  /// The entry names a backup and it is not on the card any more. Deleted by
  /// hand, or a card swapped. **Drawn as unrestorable rather than tried** (#36).
  kBackupGone,
};
const char* ToString(Restorability restorability);

/// One drawn row of the list.
struct ConflictRow {
  /// What `RestoreBackup` takes. Never zero.
  std::int64_t entry_id = 0;

  /// The left-hand text: the rom, or the file when the history had no rom name.
  /// **Never empty, in any state** -- `overlay_status_view.hpp`'s guarantee, for
  /// its reason: a row drawn as an empty string is indistinguishable from an
  /// overlay that failed to read something.
  std::string label;

  /// The right-hand text: what happened, and the file it happened to.
  std::string value;

  /// The second line: the server's reason, or why this row cannot be restored.
  /// Non-empty for every `restorable` that is not `kReady`, which is the half of
  /// the guarantee that holds -- a row that will not restore always says why.
  std::string note;

  Tone tone = Tone::kNeutral;

  Restorability restorable = Restorability::kReady;

  /// A press opens the detail. True for every row: even one that cannot be
  /// restored is worth reading, because the detail is where the backup's path
  /// is, and a user with a card reader can find it there.
  bool selectable = true;
};

/// One line of the detail view: a label, a value, and how it should read.
struct ConflictDetailLine {
  /// Never empty.
  std::string label;

  /// Never empty either. A fact the history did not have reads as "not
  /// recorded" rather than as a blank -- and a **state** reads "RomM computes
  /// no digest for a save state" where a content comparison would be, because
  /// claiming one it did not make is the one thing this view may not do.
  std::string value;

  Tone tone = Tone::kNeutral;
};

/// Everything the conflicts screen draws, and nothing about how.
struct ConflictsView {
  Link link = Link::kOk;
  ConflictsMode mode = ConflictsMode::kList;

  /// "Conflicts", or the file the detail is about. Never empty.
  std::string title;

  /// The one sentence under it. Never empty, in any state, including a console
  /// that has overwritten nothing: "Nothing has been overwritten" is a fact, and
  /// a blank screen is not.
  std::string headline;

  /// What the user can do about it, or empty when there is nothing to do.
  std::string hint;

  Tone tone = Tone::kNeutral;

  /// `kList`.
  std::vector<ConflictRow> rows;

  /// `kDetail` and `kConfirm`.
  std::vector<ConflictDetailLine> detail;

  /// Into `rows`, and `-1` exactly when `rows` is empty.
  int selected = -1;

  /// `Back()` would move within this screen rather than close it. True in
  /// `kDetail` and `kConfirm`.
  bool can_go_back = false;

  /// A on this view would restore something.
  ///
  /// A field rather than something the renderer works out from `mode` and
  /// `tone`: whether a restore is on offer is a decision -- the entry's backup
  /// has to be named *and* still on the card -- and a screen that re-derived it
  /// would be the second place that decision lives. False on the list, on an
  /// entry that cannot be restored, and on one whose restore has already run.
  bool can_restore = false;
};

/// What a confirmation says the restore actually does.
///
/// Published for `SyncOutcomeText`'s reason: the confirmation, the result line
/// and the test must be the same words, or the same console says two different
/// things about what a press meant.
std::string_view RestoreMeaning();

/// The sentence for a `Restorability` that is not `kReady`, and its tone. Empty
/// for `kReady`, which has nothing to explain.
std::string RestorabilityText(Restorability restorability);
Tone RestorabilityTone(Restorability restorability);

/// The sentence a finished restore gets, whatever it did.
///
/// Every outcome has one, including the two that wrote nothing: a screen that
/// drew a blank after a press is a screen that looks broken.
std::string RestoreOutcomeText(const conflicts::RestoreReport& report);
Tone RestoreOutcomeTone(conflicts::RestoreOutcome outcome);

/// The screen's whole state: the loaded page, the selection, and what the last
/// press produced.
class ConflictsModel {
 public:
  /// The one command the screen should send this frame.
  struct Command {
    enum class Kind { kNone, kListConflicts, kRestore };

    Kind kind = Kind::kNone;

    /// `kListConflicts`.
    ipc::ConflictQuery query;

    /// `kRestore`.
    std::int64_t entry_id = 0;
  };

  ConflictsModel();

  /// What to send now, and what the next `On...` is understood to be answering.
  ///
  /// `kNone` means there is nothing to do this frame. Calling it again without
  /// answering the last one returns the same command: the model hands out one at
  /// a time, and a caller that dropped one is handed it again rather than one
  /// whose press went missing.
  Command Next();

  /// `ListConflicts` answered `page`.
  ///
  /// A page whose `offset` is not the one that was asked for is ignored: the
  /// history can shrink under an open screen, and a page answered into the wrong
  /// place would leave a hole in the list rather than a short one.
  void OnPage(const ipc::ConflictPage& page);

  /// `RestoreBackup` answered. The screen stays on the entry and says what
  /// happened; the list is re-read, because the entry's backup situation has
  /// changed and so has the card.
  void OnRestored(const conflicts::RestoreReport& report);

  /// The sysmodule refused the command `Next()` handed out.
  void OnRefused(ipc::Error error);

  /// The command did not reach the sysmodule, or its answer could not be read.
  /// `link` must not be `kOk`; it is `ScreenFrame::Diagnose`'s verdict.
  void OnUnreachable(Link link, std::uint32_t sysmodule_interface = 0);

  /// The link came back. Everything loaded is dropped and the list is read
  /// again from the start: a page from before the outage is not a page this
  /// session would answer with.
  void OnLinkRestored();

  /// Move the selection by `delta` rows, clamped at both ends. Also what asks
  /// for the next page: the model fetches ahead of the selection rather than on
  /// a scroll event, so a screen that draws more rows at once still works.
  void MoveSelection(int delta);

  /// A. Open the selected entry, or -- from the confirmation -- send the
  /// restore.
  ///
  /// A press on a confirmation for an entry that is not `kReady` sends nothing
  /// and leaves the reason on the screen, which is `PredictSyncNow`'s shape: the
  /// screen already knows which refusal it would get, and a round trip whose only
  /// effect is to make the same sentence arrive a frame later is one it does not
  /// make.
  void Activate();

  /// B. Leave the confirmation for the detail, or the detail for the list.
  ///
  /// False when there is nowhere left to go, which is the screen telling Tesla
  /// to pop it.
  bool Back();

  ConflictsView Render() const;

  /// How many entries have been loaded. For the test rather than a screen.
  std::size_t loaded() const { return rows_.size(); }

 private:
  /// The row the detail and the confirmation are about, or nullptr.
  const ipc::ConflictRow* Current() const;

  /// Everything loaded, dropped. The next `Next()` asks for the first page.
  void Reset();

  std::vector<ipc::ConflictRow> rows_;

  /// The offset the next page starts at.
  std::int32_t offset_ = 0;

  /// How many the sysmodule says there are altogether, for "3 of 12".
  std::int32_t total_ = 0;

  /// Another page would follow. True until a page says otherwise, so a screen
  /// that has loaded nothing still asks for its first page.
  bool has_more_ = true;

  /// A page is in flight.
  bool loading_ = false;

  /// The next page to arrive replaces the loaded rows rather than extending
  /// them. Set after a restore, which changes what is on the card and therefore
  /// every row's `backup_present`. The rows stay on screen until then, so a
  /// restore is not a screen that blinks empty.
  bool replace_rows_ = false;

  /// Consecutive pages that came back empty with `has_more` still set. A
  /// producer that answered that forever would have the overlay asking forever,
  /// at a page per frame -- `overlay_library_model.hpp`'s `kMaxEmptyPages`, and
  /// its reasoning.
  int empty_pages_ = 0;

  /// What went wrong with the last page. `kOk` when nothing did.
  ipc::Error page_error_ = ipc::Error::kOk;

  int selected_ = 0;

  ConflictsMode mode_ = ConflictsMode::kList;

  /// The entry the detail is about, kept by id rather than by index: the list
  /// can be re-read under an open detail, and an index would then point at a
  /// different conflict.
  std::int64_t opened_ = 0;

  /// A restore waiting to be sent. Zero when there is none.
  std::int64_t restore_wanted_ = 0;

  /// The restore that is out, so its answer lands on the right entry.
  std::int64_t restoring_ = 0;

  /// What the last restore produced, and whether there is one to draw.
  bool restored_ = false;
  conflicts::RestoreReport last_restore_;

  /// What `Next()` last handed out, so `OnRefused` knows what was refused.
  Command::Kind issued_ = Command::Kind::kNone;

  Link link_ = Link::kOk;

  /// What `GetInterfaceVersion` last answered, for `kIncompatible`'s hint.
  std::uint32_t sysmodule_interface_ = 0;
};

/// How far ahead of the selection the next page is asked for.
///
/// `overlay_library_model.hpp`'s `kPrefetchRows`, at this screen's page size --
/// and spelled apart from it because both headers live in `rommsync::overlay`
/// and the settings screen includes both. Two constants of one name in one
/// namespace is a redefinition the *host* build never sees, because no host
/// translation unit includes both; `switch.mk` compiles the screen that does.
inline constexpr int kConflictPrefetchRows = 4;

/// How many consecutive empty pages with `has_more` still set are tolerated
/// before the list is treated as ended. Named apart for the reason above.
inline constexpr int kConflictMaxEmptyPages = 4;

}  // namespace rommsync::overlay
