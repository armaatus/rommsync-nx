// The two recorders and the restore. See conflict_record.hpp.
//
// Everything here needs the sync engine's report types; `conflict_log.cpp` next
// to it needs none of them, which is why the module is two files.
#include "rommsync/conflict_record.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"

namespace rommsync::conflicts {
namespace {

std::int64_t NowSeconds(const std::function<sync::Timestamp()>& clock) {
  return sync::UnixSeconds(clock ? clock() : std::chrono::system_clock::now());
}

/// The client's reported copy for `(rom_id, slot)`, or nullptr.
///
/// Matched the way the server pairs them and the way `sync::MatchTarget` does:
/// on `(rom_id, slot)` and never on a name, because the name on an operation is
/// the server's (sync_execute.hpp).
const sync::ClientSaveState* ReportedFor(const std::vector<sync::ClientSaveState>& reported,
                                        std::int64_t rom_id,
                                        const std::optional<std::string>& slot) {
  for (const sync::ClientSaveState& save : reported) {
    if (save.rom_id == rom_id && save.slot == slot) {
      return &save;
    }
  }
  return nullptr;
}

/// The scanned state for `(rom_id, file_name)`, which is how RomM keys one.
const scan::StateFile* ScannedFor(const scan::ScanResult& scan, std::int64_t rom_id,
                                  const std::string& file_name) {
  for (const scan::StateFile& state : scan.states) {
    if (state.rom_id == rom_id && state.file_name == file_name) {
      return &state;
    }
  }
  return nullptr;
}

}  // namespace

std::size_t RecordSaves(History* history, const sync::SyncPlan& plan,
                        const sync::ExecutionReport& report,
                        const std::vector<sync::ClientSaveState>& reported,
                        const RecordOptions& options) {
  const std::int64_t when = NowSeconds(options.now);
  std::size_t recorded = 0;

  for (std::size_t at = 0; at < report.operations.size() && at < plan.operations.size(); ++at) {
    const sync::OperationResult& result = report.operations[at];
    const sync::SyncOperation& operation = plan.operations[at];

    Event event = Event::kConflict;
    if (result.outcome == sync::OperationOutcome::kKeptBoth) {
      event = Event::kConflict;
    } else if (result.outcome == sync::OperationOutcome::kDownloaded &&
               !result.backup_sd_path.empty()) {
      // A download with no backup replaced nothing -- the save was not on the
      // card at all -- so there is nothing for a player to want back.
      event = Event::kReplaced;
    } else {
      continue;
    }

    Entry entry;
    entry.kind = EntryKind::kSave;
    entry.event = event;
    entry.rom_id = result.rom_id;
    entry.slot = result.slot;
    entry.when = when;
    entry.reason = operation.reason_text;
    entry.sd_path = result.sd_path;
    entry.server_content_hash = operation.server_content_hash;
    entry.server_updated_at = operation.server_updated_at.value_or(std::string());
    entry.backup_sd_path = result.backup_sd_path;

    // The local facts are the ones step 0 reported, not the ones on the card:
    // the card now holds the server's copy.
    if (const sync::ClientSaveState* local = ReportedFor(reported, result.rom_id, result.slot);
        local != nullptr) {
      entry.file_name = local->file_name;
      entry.emulator = local->emulator.value_or(std::string());
      entry.local_size_bytes = local->file_size_bytes;
      entry.local_content_hash = local->content_hash.value_or(std::string());
      entry.local_modified = sync::UnixSeconds(local->updated_at);
    } else {
      // No reported save for this pair. The operation still happened and the
      // backup is still real, so the entry is worth having -- what is missing is
      // the left-hand side of the comparison, and the screen says so rather than
      // showing a zero as a size. The server's name is the only one available,
      // and it is marked as such by being all there is.
      entry.file_name = operation.file_name;
      entry.emulator = operation.emulator.value_or(std::string());
    }
    if (entry.file_name.empty()) {
      continue;  // nothing to call it; `Append` would refuse it anyway
    }
    if (options.rom_name) {
      entry.rom_name = options.rom_name(result.rom_id);
    }
    if (history->Append(std::move(entry)).id != 0) {
      ++recorded;
    }
  }
  return recorded;
}

std::size_t RecordStates(History* history, const sync::StateSyncReport& report,
                         const RecordOptions& options) {
  const std::int64_t when = NowSeconds(options.now);
  std::size_t recorded = 0;

  for (const sync::StateOperationResult& result : report.operations) {
    Event event = Event::kKeptBoth;
    if (result.outcome == sync::StateOutcome::kKeptBoth) {
      // Nothing was transferred and both copies survive, so there is no backup
      // and nothing to restore. Recorded anyway: it is the one outcome where the
      // player has two copies and the client will never choose between them.
      event = Event::kKeptBoth;
    } else if (result.outcome == sync::StateOutcome::kDownloaded &&
               !result.backup_sd_path.empty()) {
      event = Event::kReplaced;
    } else {
      continue;
    }

    Entry entry;
    entry.kind = EntryKind::kState;
    entry.event = event;
    entry.rom_id = result.rom_id;
    entry.file_name = result.file_name;
    entry.when = when;
    entry.sd_path = result.sd_path;
    entry.backup_sd_path = result.backup_sd_path;
    // **No `server_content_hash`, ever**: RomM computes no digest for a state
    // (state_sync.hpp), so a detail view may show a length and an `updated_at`
    // and must not claim a content comparison it did not make.
    if (result.server.id != 0) {
      entry.server_size_bytes = result.server.file_size_bytes;
      entry.server_updated_at = sync::FormatTimestamp(result.server.updated_at);
      entry.emulator = result.server.emulator;
    }
    // **`StateOperationResult` carries no `emulator`** -- #36 said it did, and
    // it does not (state_sync.hpp). The two places it is actually written down
    // are the server row above and the scan below, so the entry takes the
    // scanned one, which is the console's own answer and the one a path was
    // derived from.
    if (const scan::StateFile* local = ScannedFor(report.scan, result.rom_id, result.file_name);
        local != nullptr) {
      entry.local_size_bytes = local->size_bytes;
      entry.local_content_hash = local->content_hash;
      entry.local_modified = local->modified_unix;
      if (!local->emulator.empty()) {
        entry.emulator = local->emulator;
      }
    }
    if (entry.file_name.empty() || entry.sd_path.empty()) {
      continue;
    }
    if (options.rom_name) {
      entry.rom_name = options.rom_name(result.rom_id);
    }
    if (history->Append(std::move(entry)).id != 0) {
      ++recorded;
    }
  }
  return recorded;
}

RestoreReport Restore(fs::FileSystem& files, const History& history, std::int64_t entry_id,
                      const RestoreOptions& options) {
  RestoreReport report;

  const Entry* entry = history.Find(entry_id);
  if (entry == nullptr) {
    report.outcome = RestoreOutcome::kNoSuchEntry;
    report.message = "there is no conflict with that id; the history may have moved on since the "
                     "screen last asked";
    return report;
  }
  if (!entry->restorable()) {
    report.outcome = RestoreOutcome::kNothingToRestore;
    report.message = entry->file_name +
                     ": nothing was overwritten, so there is nothing to put back -- both copies "
                     "are still where they were";
    return report;
  }

  const std::string backup = files.Resolve(entry->backup_sd_path);
  const std::string destination = files.Resolve(entry->sd_path);
  if (backup.empty() || destination.empty()) {
    report.outcome = RestoreOutcome::kBackupMissing;
    report.message = entry->backup_sd_path + ": not a path this card can open";
    return report;
  }
  if (!io::Exists(backup)) {
    report.outcome = RestoreOutcome::kBackupMissing;
    report.message = entry->backup_sd_path +
                     ": the backup is not on the card any more, so this conflict cannot be undone";
    return report;
  }

  // **Step 3 of the four, and it is not optional.** A restore is a save
  // overwrite, so the bytes it is about to replace go under `.backup/` first,
  // through the same call M2-5 makes -- with the save's slot or the state's name
  // as the discriminator, so one rom's save and its state never share a name.
  const std::string discriminator = entry->kind == EntryKind::kState
                                        ? sync::StateBackupDiscriminator(entry->file_name)
                                        : sync::SlotBackupDiscriminator(entry->slot);
  const std::int64_t now = NowSeconds(options.now);
  std::string message;
  const sync::OperationError backed_up =
      sync::BackUpFirst(files, options.backup_dir, entry->sd_path, entry->rom_id, discriminator,
                        entry->file_name, now, &report.backup_sd_path, &message);
  if (backed_up != sync::OperationError::kNone) {
    report.outcome = RestoreOutcome::kBackupFailed;
    report.message = "the bytes this restore would replace could not be copied first, so nothing "
                     "was written: " +
                     message;
    report.backup_sd_path.clear();
    return report;
  }

  // Streamed and staged: a save state is tens of megabytes and the sysmodule's
  // inner heap is 512 KiB, and the two-rename commit is what makes an
  // interruption leave the old bytes or the new ones and never a splice.
  const io::CopyResult copied = io::CopyAtomically(backup, destination);
  if (!copied.ok()) {
    report.outcome = RestoreOutcome::kWriteFailed;
    report.message = "the backup could not be written back over " + entry->sd_path + ": " +
                     copied.message;
    return report;
  }

  report.outcome = RestoreOutcome::kRestored;
  report.message =
      entry->file_name + ": the bytes this console had are back at " + entry->sd_path +
      (report.backup_sd_path.empty()
           ? std::string(", and there was nothing there to keep")
           : ", and what was there is under " + report.backup_sd_path) +
      ". The server still holds its own copy: the next sync arbitrates, and will most likely "
      "offer these bytes to it.";
  return report;
}

}  // namespace rommsync::conflicts
