// The conflicts screen, decided here and drawn in `overlay/source/`.
//
// See overlay_conflicts_view.hpp. What is worth saying here is the one rule the
// detail view is written against: **it may not claim a comparison it did not
// make.** A save has an MD5 on both sides and the two can be set against each
// other; a state has one on this side and nothing at all on the server's,
// because RomM computes no digest for one (state_sync.hpp). So the state's
// detail says so, in a line, rather than leaving the row out and letting the
// eye fill it in.
#include "rommsync/overlay_conflicts_view.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {
namespace {

/// What a row falls back to when the history recorded no rom name. Never a
/// blank: `ConflictListRow::label` is non-empty in every state.
constexpr const char* kUnnamed = "Unnamed";

/// What a value the history did not record reads as. One copy, because half a
/// dozen detail lines answer it and "" in any of them is a screen that looks
/// like it failed to read something.
constexpr const char* kNotRecorded = "not recorded";

/// The line a **state's** server side gets where a content comparison would be.
///
/// It is a sentence rather than an omission on purpose: a detail view that
/// simply had no digest row would read as "the digests matched" to anyone
/// scanning it, and this screen's whole job is to say what the two copies
/// actually are.
constexpr const char* kNoServerDigest =
    "RomM computes no digest for a save state, so the two were never compared by content";

/// A conflict's row when its losing bytes could not be put on the server
/// (#240). Another device cannot recover them, and a lost card loses them.
constexpr const char* kCardOnlyNote =
    "The server holds no copy of this console's bytes -- only the backup on this card does";

std::string Number(std::int64_t value) { return std::to_string(value); }

/// `1234 bytes`. Bytes rather than a rounded unit: the number is here so a user
/// can tell two copies apart, and "2 KB" and "2 KB" are the same string for two
/// files that differ.
///
/// `known` is what separates **an empty file from a size nothing measured** --
/// both are `0` on the entry, and on the one screen whose job is telling two
/// copies apart they are not the same fact. An emulator that truncated a save to
/// nothing is a very good reason to want the backup.
std::string Size(std::int64_t bytes, bool known) {
  if (!known) {
    return kNotRecorded;
  }
  return Number(bytes) + (bytes == 1 ? " byte" : " bytes");
}

std::string Hash(const std::string& digest) {
  return digest.empty() ? std::string(kNotRecorded) : digest;
}

std::string Text(const std::string& value) {
  return value.empty() ? std::string(kNotRecorded) : value;
}

const char* EventText(conflicts::Event event, conflicts::EntryKind kind) {
  switch (event) {
    case conflicts::Event::kConflict:
      return "conflict, kept both";
    case conflicts::Event::kReplaced:
      return kind == conflicts::EntryKind::kState ? "state replaced" : "save replaced";
    case conflicts::Event::kKeptBoth:
      return "both copies kept";
    case conflicts::Event::kDeleted:
      return "deleted on RomM";
  }
  return "overwritten";
}

constexpr ConflictChoice kChoices[] = {ConflictChoice::kKeepServer, ConflictChoice::kKeepLocal,
                                       ConflictChoice::kSkip};
constexpr int kLastChoice = static_cast<int>(std::size(kChoices)) - 1;

const char* ChoiceLabel(ConflictChoice choice) {
  switch (choice) {
    case ConflictChoice::kKeepServer:
      return "Keep server";
    case ConflictChoice::kKeepLocal:
      return "Keep local";
    case ConflictChoice::kSkip:
      return "Skip";
  }
  return "Skip";
}

Restorability RestorabilityOf(const ipc::ConflictRow& row) {
  if (!conflicts::Overwrote(row.entry.event)) {
    // Nothing was replaced -- a state both sides kept. The only case where
    // "there is nothing to put back" is the *policy* rather than a loss.
    return Restorability::kNothingToRestore;
  }
  if (row.entry.backup_sd_path.empty() || !row.backup_present) {
    // An overwrite is here either way, so the sentence has to be the one about a
    // backup that cannot be found -- not "nothing was overwritten", which would
    // tell a user their save is intact when it is the one that was replaced. An
    // entry that recorded no path at all is the same fact one step earlier.
    return Restorability::kBackupGone;
  }
  return Restorability::kReady;
}

}  // namespace

const char* ToString(ConflictsMode mode) {
  switch (mode) {
    case ConflictsMode::kList:
      return "list";
    case ConflictsMode::kDetail:
      return "detail";
    case ConflictsMode::kConfirm:
      return "confirm";
  }
  return "list";
}

const char* ToString(Restorability restorability) {
  switch (restorability) {
    case Restorability::kReady:
      return "ready";
    case Restorability::kNothingToRestore:
      return "nothing_to_restore";
    case Restorability::kBackupGone:
      return "backup_gone";
  }
  return "ready";
}

const char* ToString(ConflictChoice choice) {
  switch (choice) {
    case ConflictChoice::kKeepServer:
      return "keep_server";
    case ConflictChoice::kKeepLocal:
      return "keep_local";
    case ConflictChoice::kSkip:
      return "skip";
  }
  return "skip";
}

std::string_view ChoiceMeaning(ConflictChoice choice) {
  // Hard rule 3, three ways. Keep server is what already happened; Keep local
  // is the player overruling the server, and says so in those words, because
  // it is the one press on this console that does.
  switch (choice) {
    case ConflictChoice::kKeepServer:
      return "The server's copy stays on the card, as the sync left it. Nothing is sent.";
    case ConflictChoice::kKeepLocal:
      return "This console's bytes go back on the card, and the next sync uploads them over "
             "the server's copy. RomM deletes nothing: that copy stays on the server behind "
             "this one.";
    case ConflictChoice::kSkip:
      return "Decide later. Nothing changes, and this choice stays here.";
  }
  return {};
}

std::string_view RestoreMeaning() {
  // Hard rule 3, in the words a user needs before pressing A. It is deliberately
  // not "this wins": nothing here overrules the server, and a console that said
  // so would have a user press restore and then watch the next sync appear to
  // undo it.
  return "This writes the local file only. The server still holds its own copy: "
         "the next sync decides, and will most likely offer these bytes to it.";
}

std::string RestorabilityText(Restorability restorability) {
  switch (restorability) {
    case Restorability::kReady:
      return {};
    case Restorability::kNothingToRestore:
      return "Nothing was overwritten -- both copies are still where they were";
    case Restorability::kBackupGone:
      return "The backup is no longer on the card, so this cannot be undone";
  }
  return {};
}

Tone RestorabilityTone(Restorability restorability) {
  switch (restorability) {
    case Restorability::kReady:
      return Tone::kNeutral;
    case Restorability::kNothingToRestore:
      return Tone::kNeutral;
    case Restorability::kBackupGone:
      return Tone::kWarn;
  }
  return Tone::kNeutral;
}

std::string RestoreOutcomeText(const conflicts::RestoreReport& report) {
  switch (report.outcome) {
    case conflicts::RestoreOutcome::kRestored:
      return report.backup_sd_path.empty()
                 ? std::string("Restored. There was nothing at that path to keep.")
                 : "Restored. What was there is now under " + report.backup_sd_path;
    case conflicts::RestoreOutcome::kNoSuchEntry:
      return "That conflict is no longer in the history";
    case conflicts::RestoreOutcome::kNothingToRestore:
      return "Nothing was overwritten, so there is nothing to put back";
    case conflicts::RestoreOutcome::kBackupMissing:
      return "The backup is no longer on the card";
    case conflicts::RestoreOutcome::kBackupFailed:
      // The message names the reason, and the reason is the whole point: the
      // restore stopped *before* writing, which is hard rule 2 working.
      return "Nothing was written: the bytes this would replace could not be "
             "backed up first";
    case conflicts::RestoreOutcome::kWriteFailed:
      return "The backup was made and the restore did not land. The file is as it was.";
    case conflicts::RestoreOutcome::kNotAConflict:
      return "Nothing was written: Keep local is only for a save conflict";
    case conflicts::RestoreOutcome::kChangedSinceBackup:
      // Almost always a game saving mid-restore (#270), though a file the card
      // would not read back lands here too, so the sentence claims no more than
      // that it changed. The press can be repeated once the game is closed.
      return "Nothing was written: the file changed while this ran. Close the game and try "
             "again.";
  }
  return "The restore did not happen";
}

Tone RestoreOutcomeTone(conflicts::RestoreOutcome outcome) {
  switch (outcome) {
    case conflicts::RestoreOutcome::kRestored:
      return Tone::kGood;
    case conflicts::RestoreOutcome::kNothingToRestore:
      return Tone::kNeutral;
    case conflicts::RestoreOutcome::kNoSuchEntry:
    case conflicts::RestoreOutcome::kBackupMissing:
    case conflicts::RestoreOutcome::kBackupFailed:
    case conflicts::RestoreOutcome::kWriteFailed:
    case conflicts::RestoreOutcome::kNotAConflict:
    case conflicts::RestoreOutcome::kChangedSinceBackup:
      return Tone::kBad;
  }
  return Tone::kBad;
}

ConflictsModel::ConflictsModel() = default;

void ConflictsModel::Reset() {
  rows_.clear();
  offset_ = 0;
  total_ = 0;
  has_more_ = true;
  loading_ = false;
  empty_pages_ = 0;
  page_error_ = ipc::Error::kOk;
  selected_ = 0;
  mode_ = ConflictsMode::kList;
  opened_ = 0;
  restore_wanted_ = 0;
  restoring_ = 0;
  awaiting_ = false;
  lost_ = 0;
  restored_ = false;
  last_restore_ = {};
  keep_local_wanted_ = false;
  keeping_local_ = false;
  last_kept_local_ = false;
  choice_ = 0;
  kept_server_ = 0;
  issued_ = Command::Kind::kNone;
  stale_ = false;
}

const ipc::ConflictRow* ConflictsModel::Current() const {
  for (const ipc::ConflictRow& row : rows_) {
    if (row.entry.id == opened_) {
      return &row;
    }
  }
  return nullptr;
}

ConflictsModel::Command ConflictsModel::Next() {
  Command command;
  if (link_ != Link::kOk) {
    issued_ = Command::Kind::kNone;
    return command;
  }
  // A press that is waiting goes first: the user is looking at a confirmation
  // they have already answered, and a page fetch in front of it would be a
  // frame of nothing happening.
  if (restore_wanted_ != 0) {
    command.kind = keep_local_wanted_ ? Command::Kind::kKeepLocal : Command::Kind::kRestore;
    command.entry_id = restore_wanted_;
    restoring_ = restore_wanted_;
    keeping_local_ = keep_local_wanted_;
    issued_ = command.kind;
    return command;
  }
  // A restore the sysmodule is copying, asked after once a frame. Ahead of a
  // page for `restore_wanted_`'s reason: somebody is watching it say
  // "Restoring", and the poll is one small answer from memory.
  if (awaiting_) {
    command.kind = Command::Kind::kRestoreState;
    command.entry_id = restoring_;
    issued_ = command.kind;
    return command;
  }
  // Fetch ahead of the selection rather than at the end of it, so a user
  // holding down does not stop at the bottom of every page.
  const bool near_end = selected_ + kConflictPrefetchRows >= static_cast<int>(rows_.size());
  if (has_more_ && !loading_ && page_error_ == ipc::Error::kOk && near_end) {
    command.kind = Command::Kind::kListConflicts;
    command.query.offset = offset_;
    command.query.limit = ipc::kMaxConflictPage;
    loading_ = true;
    issued_ = command.kind;
    return command;
  }
  issued_ = Command::Kind::kNone;
  return command;
}

void ConflictsModel::OnPage(const ipc::ConflictPage& page) {
  if (issued_ != Command::Kind::kListConflicts) {
    return;
  }
  issued_ = Command::Kind::kNone;
  loading_ = false;
  page_error_ = ipc::Error::kOk;
  if (page.offset != offset_) {
    // Answered into a place this model did not ask about. The history can shrink
    // under an open screen; appending it anyway would leave a hole rather than a
    // short list, so it is dropped and asked for again from where we are.
    //
    // Counted against `kConflictMaxEmptyPages` for the reason an empty page with
    // `has_more` is: a producer that echoes the wrong offset forever would have
    // the overlay asking once a frame, on the sysmodule's IPC thread, with
    // nothing to draw for it.
    if (++empty_pages_ >= kConflictMaxEmptyPages) {
      has_more_ = false;
    }
    return;
  }
  total_ = page.total;
  if (page.entries.empty()) {
    if (page.has_more && ++empty_pages_ < kConflictMaxEmptyPages) {
      return;  // ask again; see `kConflictMaxEmptyPages`
    }
    has_more_ = false;
    return;
  }
  empty_pages_ = 0;
  for (const ipc::ConflictRow& row : page.entries) {
    if (rows_.size() >= conflicts::kMaxEntries) {
      break;
    }
    rows_.push_back(row);
  }
  offset_ = static_cast<std::int32_t>(rows_.size());
  has_more_ = page.has_more && rows_.size() < conflicts::kMaxEntries;
}

void ConflictsModel::OnRestoreState(const ipc::RestoreState& state) {
  const Command::Kind asked = issued_;
  const bool pressed = asked == Command::Kind::kRestore || asked == Command::Kind::kKeepLocal;
  if (!pressed && asked != Command::Kind::kRestoreState) {
    return;
  }
  issued_ = Command::Kind::kNone;
  restore_wanted_ = 0;

  if (state.entry_id == restoring_ && state.phase == ipc::RestorePhase::kRunning) {
    // Taken, or still copying. The screen says "Restoring" and asks again next
    // frame; the confirmation is behind it, because the press has happened.
    awaiting_ = true;
    if (mode_ == ConflictsMode::kConfirm && opened_ == restoring_) {
      mode_ = ConflictsMode::kDetail;
    }
    return;
  }
  if (state.entry_id == restoring_ && state.phase == ipc::RestorePhase::kDone) {
    FinishRestore(state.report);
    return;
  }
  if (pressed && state.phase == ipc::RestorePhase::kRunning) {
    // Another restore is out -- one started by an earlier open of this overlay
    // -- and this press started nothing. `kBackupFailed`'s promise is exactly
    // what holds: nothing was written.
    conflicts::RestoreReport refused;
    refused.outcome = conflicts::RestoreOutcome::kBackupFailed;
    refused.message = "another restore is still running; try again when it finishes";
    FinishRestore(refused);
    return;
  }
  // The sysmodule no longer knows about the restore this screen is waiting for:
  // it restarted. Whether the copy happened is not something this side can say,
  // so it says that, and stops asking.
  lost_ = restoring_;
  restoring_ = 0;
  awaiting_ = false;
  restored_ = false;
  if (mode_ == ConflictsMode::kConfirm && opened_ == lost_) {
    mode_ = ConflictsMode::kDetail;
  }
}

void ConflictsModel::FinishRestore(const conflicts::RestoreReport& report) {
  const std::int64_t restored = restoring_;
  const bool kept_local = keeping_local_;
  restoring_ = 0;
  keeping_local_ = false;
  awaiting_ = false;
  // The outcome is drawn on the entry it is about, and only there: a user who
  // left for the list while it copied is not pulled back to it, and one who
  // opened a different entry is not shown this one's outcome over it.
  if (opened_ == restored) {
    restored_ = true;
    last_restore_ = report;
    last_kept_local_ = kept_local;
    // Back to the entry, with the outcome on it. Staying on the confirmation
    // would invite a second press of a thing that has already happened.
    if (mode_ == ConflictsMode::kConfirm) {
      mode_ = ConflictsMode::kDetail;
    }
  }

  // **The loaded pages are not re-read**, and that is not laziness. A restore
  // rewrites nothing in the history and deletes nothing from `.backup/`: it
  // writes the save and adds a *new* backup, which is deliberately not an entry
  // (conflict_log.hpp). So every row's `backup_present` is as true as it was.
  //
  // Refetching would also be actively wrong. There is only ever one page in
  // flight and it would start at offset 0, so a restore of the tenth conflict
  // would replace the loaded rows with the first eight -- and the open detail,
  // found by id, would vanish underneath the outcome the user is reading.
  //
  // The one fact that *did* change is the entry this restore could not find.
  if (report.outcome == conflicts::RestoreOutcome::kBackupMissing) {
    for (ipc::ConflictRow& row : rows_) {
      if (row.entry.id == restored) {
        row.backup_present = false;
        break;
      }
    }
  }
}

void ConflictsModel::OnRefused(ipc::Error error) {
  const Command::Kind refused = issued_;
  issued_ = Command::Kind::kNone;
  if (refused == Command::Kind::kRestore || refused == Command::Kind::kRestoreState ||
      refused == Command::Kind::kKeepLocal) {
    // None of them is documented to fail at the transport (ipc.hpp), so a
    // refusal here is the two halves disagreeing about the contract. Reported
    // as a restore that did not happen, which is the true and actionable half.
    restore_wanted_ = 0;
    keep_local_wanted_ = false;
    conflicts::RestoreReport report;
    report.outcome = conflicts::RestoreOutcome::kWriteFailed;
    report.message = ipc::ToString(error);
    FinishRestore(report);
    return;
  }
  if (refused == Command::Kind::kListConflicts) {
    loading_ = false;
    page_error_ = error;
  }
}

void ConflictsModel::OnUnreachable(Link link, std::uint32_t sysmodule_interface) {
  if (link == Link::kOk) {
    return;
  }
  link_ = link;
  sysmodule_interface_ = sysmodule_interface;
  if (link == Link::kNotResponding) {
    // **Slow is not gone** (#209). The client kept the session, so the
    // sysmodule on the far side is the one that answered every page here and
    // nothing loaded is stale -- only the command that ran out of budget is in
    // doubt, and its answer was dropped.
    switch (issued_) {
      case Command::Kind::kListConflicts:
        loading_ = false;  // asked again once it answers
        break;
      case Command::Kind::kRestore:
      case Command::Kind::kKeepLocal:
        // It may have been taken. Asked about, never sent again: a second
        // press the user did not make is a second overwrite of their save.
        restore_wanted_ = 0;
        keep_local_wanted_ = false;
        awaiting_ = true;
        break;
      case Command::Kind::kRestoreState:
      case Command::Kind::kNone:
        break;
    }
    issued_ = Command::Kind::kNone;
    return;
  }
  issued_ = Command::Kind::kNone;
  loading_ = false;
  restore_wanted_ = 0;
  keep_local_wanted_ = false;
  restoring_ = 0;
  keeping_local_ = false;
  awaiting_ = false;
  stale_ = true;
}

void ConflictsModel::OnLinkRestored() {
  if (link_ == Link::kOk) {
    return;
  }
  link_ = Link::kOk;
  if (stale_) {
    // The session was dropped on the way, so the sysmodule answering now may
    // not be the one that answered the pages here.
    Reset();
  }
}

void ConflictsModel::MoveSelection(int delta) {
  if (mode_ == ConflictsMode::kConfirm) {
    const ipc::ConflictRow* row = Current();
    if (row != nullptr && row->entry.save_conflict()) {
      choice_ = std::clamp(choice_ + delta, 0, kLastChoice);
    }
    return;
  }
  if (mode_ != ConflictsMode::kList || rows_.empty()) {
    return;
  }
  const int last = static_cast<int>(rows_.size()) - 1;
  const int before = selected_;
  selected_ = std::clamp(selected_ + delta, 0, last);

  // **Scrolling is the retry.** A page that failed part way down a list left
  // `page_error_` set, and `Next()` gates every later fetch on it, so without
  // this the rest of the history is unreachable for the life of the screen. A on
  // a loaded list opens the row under it, so it cannot also be the retry; moving
  // toward the missing rows is the gesture that already means "I want more of
  // these".
  //
  // **A press toward the end counts even when the selection does not move**, and
  // that is the whole of it: the fetch that can fail is only ever issued within
  // `kConflictPrefetchRows` of the last loaded row, so by the time the failure
  // lands the user is usually already at the bottom, where `std::clamp` pins the
  // index. Retrying only on a *changed* index would leave every further Down
  // press inert at exactly the position the hint tells them to press it, and
  // recovery would need an undocumented Up-then-Down.
  //
  // It is one retry per press and not one per frame: `MoveSelection` is called
  // from `handleInput` on a key event, never from `update()`.
  if (selected_ != before || delta > 0) {
    page_error_ = ipc::Error::kOk;
  }
}

void ConflictsModel::Activate() {
  switch (mode_) {
    case ConflictsMode::kList: {
      if (rows_.empty()) {
        if (page_error_ != ipc::Error::kOk) {
          page_error_ = ipc::Error::kOk;  // A on a failed list asks again
        }
        return;
      }
      opened_ = rows_[static_cast<std::size_t>(selected_)].entry.id;
      restored_ = false;
      if (lost_ != opened_) {
        lost_ = 0;
      }
      mode_ = ConflictsMode::kDetail;
      return;
    }
    case ConflictsMode::kDetail: {
      const ipc::ConflictRow* row = Current();
      if (row == nullptr || RestorabilityOf(*row) != Restorability::kReady) {
        // Nothing to confirm. The row already says why, and asking a user to
        // confirm something the screen knows will not happen is worse than not
        // offering it.
        return;
      }
      if (restoring_ != 0) {
        // One restore at a time, and one is out. Not a confirmation for a
        // press that would be answered "another restore is running".
        return;
      }
      restored_ = false;
      lost_ = 0;
      kept_server_ = 0;
      // Keep server first: it is what the sync already did, so an A pressed
      // through without reading changes nothing.
      choice_ = 0;
      mode_ = ConflictsMode::kConfirm;
      return;
    }
    case ConflictsMode::kConfirm: {
      const ipc::ConflictRow* row = Current();
      if (row == nullptr || RestorabilityOf(*row) != Restorability::kReady) {
        mode_ = ConflictsMode::kDetail;
        return;
      }
      bool keep_local = false;
      if (row->entry.save_conflict()) {
        switch (kChoices[choice_]) {
          case ConflictChoice::kKeepServer:
            // Already done: the sync put the server's copy on the card. The
            // detail says so, and nothing goes to the sysmodule.
            kept_server_ = row->entry.id;
            mode_ = ConflictsMode::kDetail;
            return;
          case ConflictChoice::kSkip:
            mode_ = ConflictsMode::kList;
            opened_ = 0;
            return;
          case ConflictChoice::kKeepLocal:
            keep_local = true;
            break;
        }
      }
      if (restoring_ == 0 && restore_wanted_ == 0) {
        restore_wanted_ = row->entry.id;
        keep_local_wanted_ = keep_local;
      }
      return;
    }
  }
}

bool ConflictsModel::Back() {
  switch (mode_) {
    case ConflictsMode::kConfirm:
      mode_ = ConflictsMode::kDetail;
      return true;
    case ConflictsMode::kDetail:
      mode_ = ConflictsMode::kList;
      opened_ = 0;
      restored_ = false;
      kept_server_ = 0;
      return true;
    case ConflictsMode::kList:
      return false;
  }
  return false;
}

// --- rendering ----------------------------------------------------------------

ConflictsView ConflictsModel::Render() const {
  ConflictsView view;
  view.link = link_;
  view.title = "Conflicts";
  if (link_ != Link::kOk) {
    // The same three sentences every other screen uses, because it is the same
    // diagnosis (`overlay_status_view.hpp`).
    const StatusView status = RenderUnreachable(link_, sysmodule_interface_);
    view.headline = status.headline;
    view.hint = status.hint;
    view.tone = status.tone;
    return view;
  }
  view.mode = mode_;

  if (mode_ == ConflictsMode::kList) {
    for (const ipc::ConflictRow& row : rows_) {
      const conflicts::Entry& entry = row.entry;
      ConflictListRow drawn;
      drawn.entry_id = entry.id;
      drawn.label = entry.rom_name.empty()
                        ? (entry.file_name.empty() ? std::string(kUnnamed) : entry.file_name)
                        : entry.rom_name;
      drawn.value = std::string(EventText(entry.event, entry.kind)) + " -- " + entry.file_name;
      drawn.restorable = RestorabilityOf(row);
      if (drawn.restorable == Restorability::kReady) {
        // The server's own sentence, which is what a user is actually being
        // asked to understand. A state carries none, so it says what it is.
        drawn.note = entry.reason.empty()
                         ? std::string("Press A for both copies and the backup")
                         : entry.reason;
        drawn.tone = entry.event == conflicts::Event::kConflict ? Tone::kWarn : Tone::kNeutral;
        if (entry.local_copy == conflicts::LocalCopy::kCardOnly) {
          // The one fact that outranks the server's sentence (#240): the bytes
          // this console lost are on this card and nowhere else.
          drawn.note = kCardOnlyNote;
          drawn.tone = Tone::kWarn;
        }
      } else {
        drawn.note = RestorabilityText(drawn.restorable);
        drawn.tone = RestorabilityTone(drawn.restorable);
      }
      view.rows.push_back(std::move(drawn));
    }
    view.selected = view.rows.empty() ? -1 : selected_;

    if (page_error_ != ipc::Error::kOk && view.rows.empty()) {
      view.headline = "The conflict history could not be read";
      view.hint = std::string(ipc::ToString(page_error_)) + " -- press A to try again";
      view.tone = Tone::kBad;
      return view;
    }
    if (view.rows.empty()) {
      // A fact, not a blank screen. And the *right* fact: an empty history on a
      // console that syncs is the ordinary case, not a failure.
      view.headline = loading_ ? "Reading the conflict history..."
                               : "Nothing has been overwritten on this console";
      view.hint = loading_ ? std::string()
                           : "A conflict is listed here when a sync replaces a save that "
                             "changed on both sides";
      return view;
    }
    view.headline = "Showing " + Number(static_cast<std::int64_t>(view.rows.size())) + " of " +
                    Number(total_) + (total_ == 1 ? " conflict" : " conflicts");
    if (page_error_ != ipc::Error::kOk) {
      // The rows already loaded are still the history, and they stay. What the
      // hint must not say is "press A", which opens the selected row -- see
      // `MoveSelection`.
      view.hint = std::string("The next page could not be loaded (") +
                  ipc::ToString(page_error_) + ") -- scroll on to try again";
      view.tone = Tone::kWarn;
      return view;
    }
    view.hint = "A opens one; the local bytes are under .backup/";
    return view;
  }

  // --- the detail, and the confirmation over it -------------------------------

  const ipc::ConflictRow* row = Current();
  if (row == nullptr) {
    // The entry left the history while it was open -- it fell off the end, or a
    // reboot happened. Said out loud rather than drawn as an empty detail.
    view.title = "Conflict";
    view.headline = "That conflict is no longer in the history";
    view.hint = "B goes back to the list";
    view.tone = Tone::kWarn;
    view.can_go_back = true;
    return view;
  }
  const conflicts::Entry& entry = row->entry;
  const Restorability restorable = RestorabilityOf(*row);
  view.title = entry.file_name.empty() ? std::string(kUnnamed) : entry.file_name;
  view.can_go_back = true;

  const auto line = [&view](std::string label, std::string value, Tone tone = Tone::kNeutral) {
    view.detail.push_back({std::move(label), std::move(value), tone});
  };

  line("Game", entry.rom_name.empty() ? "rom " + Number(entry.rom_id) : entry.rom_name);
  line("What happened", EventText(entry.event, entry.kind));
  if (entry.kind == conflicts::EntryKind::kSave) {
    line("Slot", entry.slot.has_value() ? *entry.slot : std::string("archival"));
  } else {
    line("Save state", entry.file_name);
  }
  if (!entry.emulator.empty()) {
    line("Emulator", entry.emulator);
  }
  if (!entry.reason.empty()) {
    line("The server said", entry.reason, Tone::kWarn);
  }
  line("On the card", entry.sd_path);

  // Both sides, side by side. This is the whole reason the screen exists, and
  // the reason every value has a sentence when it is missing: a blank next to
  // "This console" reads as "the same as the other one".
  // The local side was recorded when *anything* about it was: a save an
  // emulator truncated has a size of zero, an mtime and a digest.
  const bool local_known = entry.local_size_bytes > 0 || entry.local_modified > 0 ||
                           !entry.local_content_hash.empty();
  line("This console", Size(entry.local_size_bytes, local_known));
  line("  its MD5", Hash(entry.local_content_hash));
  line("  changed", entry.local_modified == 0 ? std::string(kNotRecorded)
                                              : Number(entry.local_modified) + " (unix)");
  // The two labels differ because the two quantities do. A state has no server
  // digest at all, so the row a save spends on one is a *size* here -- and the
  // line under it says why, rather than letting "The server" over a number read
  // as a comparison that was made.
  if (entry.kind == conflicts::EntryKind::kState) {
    // A state's server side is a length and an `updated_at` and nothing else,
    // so "recorded at all" is whether either of them is there.
    line("Server size", Size(entry.server_size_bytes,
                             entry.server_size_bytes > 0 || !entry.server_updated_at.empty()));
    line("  its digest", kNoServerDigest, Tone::kNeutral);
  } else {
    line("Server MD5", entry.server_content_hash.has_value() ? *entry.server_content_hash
                                                             : std::string(kNotRecorded));
  }
  line("  updated", Text(entry.server_updated_at));

  if (entry.backup_sd_path.empty()) {
    line("Backup", "none -- nothing was overwritten");
  } else {
    line("Backup", entry.backup_sd_path,
         restorable == Restorability::kBackupGone ? Tone::kWarn : Tone::kNeutral);
  }
  // Where else this console's bytes are (#240). Drawn only when recorded: an
  // entry from before it says nothing rather than guessing.
  if (entry.local_copy == conflicts::LocalCopy::kOnServer) {
    line("On the server", entry.archive_save_id > 0
                              ? "this console's bytes, as archival save " +
                                    Number(entry.archive_save_id)
                              : std::string("this console's bytes, as an archival save"));
  } else if (entry.local_copy == conflicts::LocalCopy::kCardOnly) {
    line("On the server", "no copy -- the upload failed, so the backup is the only one",
         Tone::kWarn);
  }

  view.can_restore = restorable == Restorability::kReady && !restored_ && restoring_ == 0;
  view.keep_choice = entry.save_conflict();

  if (mode_ == ConflictsMode::kConfirm && view.keep_choice) {
    for (const ConflictChoice choice : kChoices) {
      view.choices.push_back(
          {choice, ChoiceLabel(choice), std::string(ChoiceMeaning(choice))});
    }
    view.choice = choice_;
    view.headline = "Which copy of " + view.title + " do you keep?";
    view.hint = std::string(ChoiceMeaning(kChoices[choice_])) + " A chooses, B cancels.";
    view.tone = Tone::kWarn;
    return view;
  }
  if (mode_ == ConflictsMode::kConfirm) {
    view.headline = "Put this console's bytes back at " + entry.sd_path + "?";
    view.hint = std::string(RestoreMeaning()) + " A confirms, B cancels.";
    view.tone = Tone::kWarn;
    return view;
  }

  if (restoring_ != 0 && restoring_ == entry.id) {
    // Taken, and copying on the sysmodule's worker. A frame is never held for
    // it (#209); this is what the screen says while it is not.
    view.headline = keeping_local_ ? "Keeping this console's copy..." : "Restoring...";
    view.hint = "The bytes there now are backed up first. B goes back to the list.";
    return view;
  }
  if (lost_ != 0 && lost_ == entry.id) {
    view.headline = "sys-rommsync lost track of this restore";
    view.hint = "It may not have run, or it restarted before it finished. Check the file "
                "before restoring again.";
    view.tone = Tone::kWarn;
    return view;
  }
  if (restored_ && last_kept_local_ && last_restore_.ok()) {
    view.headline = "Kept this console's copy. The next sync uploads it over the server's.";
    view.tone = Tone::kGood;
    view.hint = last_restore_.message.empty() ? std::string("B goes back to the list")
                                              : last_restore_.message;
    return view;
  }
  if (restored_) {
    view.headline = RestoreOutcomeText(last_restore_);
    view.tone = RestoreOutcomeTone(last_restore_.outcome);
    view.hint = last_restore_.message.empty() ? std::string("B goes back to the list")
                                              : last_restore_.message;
    return view;
  }
  if (restorable == Restorability::kReady && view.keep_choice) {
    if (kept_server_ == entry.id) {
      view.headline = "Kept the server's copy: it stays on the card, and nothing was sent";
      view.hint = "This console's bytes stay under " + entry.backup_sd_path +
                  ". A chooses again; B goes back to the list";
      return view;
    }
    view.headline = "The server's copy is on the card; this console's is under " +
                    entry.backup_sd_path;
    view.hint = "A chooses which to keep; B goes back to the list";
    return view;
  }
  if (restorable == Restorability::kReady) {
    view.headline = "The bytes this console had are under " + entry.backup_sd_path;
    view.hint = "A puts them back; B goes back to the list";
    return view;
  }
  view.headline = RestorabilityText(restorable);
  view.tone = RestorabilityTone(restorable);
  view.hint = "B goes back to the list";
  return view;
}

}  // namespace rommsync::overlay
