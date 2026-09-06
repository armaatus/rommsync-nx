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
  }
  return "overwritten";
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
  restored_ = false;
  last_restore_ = {};
  issued_ = Command::Kind::kNone;
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
    command.kind = Command::Kind::kRestore;
    command.entry_id = restore_wanted_;
    restoring_ = restore_wanted_;
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

void ConflictsModel::OnRestored(const conflicts::RestoreReport& report) {
  if (issued_ != Command::Kind::kRestore) {
    return;
  }
  issued_ = Command::Kind::kNone;
  const std::int64_t restored = restoring_;
  restore_wanted_ = 0;
  restoring_ = 0;
  restored_ = true;
  last_restore_ = report;
  // Back to the entry, with the outcome on it. Staying on the confirmation
  // would invite a second press of a thing that has already happened.
  mode_ = ConflictsMode::kDetail;

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
  if (refused == Command::Kind::kRestore) {
    // `RestoreBackup` is documented never to fail at the transport (ipc.hpp), so
    // a refusal here is the two halves disagreeing about the contract. Reported
    // as a restore that did not happen, which is the true and actionable half.
    restore_wanted_ = 0;
    restoring_ = 0;
    restored_ = true;
    last_restore_ = {};
    last_restore_.outcome = conflicts::RestoreOutcome::kWriteFailed;
    last_restore_.message = ipc::ToString(error);
    mode_ = ConflictsMode::kDetail;
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
  issued_ = Command::Kind::kNone;
  loading_ = false;
  restore_wanted_ = 0;
  restoring_ = 0;
  link_ = link;
  sysmodule_interface_ = sysmodule_interface;
}

void ConflictsModel::OnLinkRestored() {
  if (link_ == Link::kOk) {
    return;
  }
  link_ = Link::kOk;
  Reset();
}

void ConflictsModel::MoveSelection(int delta) {
  if (mode_ != ConflictsMode::kList || rows_.empty()) {
    return;
  }
  const int last = static_cast<int>(rows_.size()) - 1;
  const int before = selected_;
  selected_ = std::clamp(selected_ + delta, 0, last);
  if (selected_ != before) {
    // **Scrolling is the retry.** A page that failed part way down a list left
    // `page_error_` set, and `Next()` gates every later fetch on it, so without
    // this the rest of the history is unreachable for the life of the screen.
    // A on a loaded list opens the row under it, so it cannot also be the retry;
    // moving toward the missing rows is the gesture that already means "I want
    // more of these".
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
      restored_ = false;
      mode_ = ConflictsMode::kConfirm;
      return;
    }
    case ConflictsMode::kConfirm: {
      const ipc::ConflictRow* row = Current();
      if (row == nullptr || RestorabilityOf(*row) != Restorability::kReady) {
        mode_ = ConflictsMode::kDetail;
        return;
      }
      if (restoring_ == 0 && restore_wanted_ == 0) {
        restore_wanted_ = row->entry.id;
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

  view.can_restore = restorable == Restorability::kReady && !restored_;

  if (mode_ == ConflictsMode::kConfirm) {
    view.headline = "Put this console's bytes back at " + entry.sd_path + "?";
    view.hint = std::string(RestoreMeaning()) + " A confirms, B cancels.";
    view.tone = Tone::kWarn;
    return view;
  }

  if (restored_) {
    view.headline = RestoreOutcomeText(last_restore_);
    view.tone = RestoreOutcomeTone(last_restore_.outcome);
    view.hint = last_restore_.message.empty() ? std::string("B goes back to the list")
                                              : last_restore_.message;
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
