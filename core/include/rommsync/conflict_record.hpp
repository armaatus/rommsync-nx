// Filling the conflict history in, and putting the old bytes back.
//
// `conflict_log.hpp` is the entry and the file. This is the half that needs the
// sync engine: the two recorders, which turn a tick's reports into entries as
// the tick executes, and `Restore`, which is a save overwrite in its own right
// and owes hard rule 2 the same backup a download does.
//
// Split from the store for the reason the store says: `ipc.hpp` carries a
// `conflicts::Entry` across the wire, and a boundary header that pulled
// `sync_execute.hpp` in behind it would put the whole executor in front of
// every screen that draws a row.
//
// ## A restore is itself an overwrite
//
// `Restore` backs the file up *first*, through the same `sync::BackUpFirst`
// M2-5 uses, and only then commits with `io::CopyAtomically`. A restore that
// ate the server's copy would be the bug backups exist to prevent, one level
// up.
//
// **The server stays the source of truth** (hard rule 3). A restore writes the
// local file and nothing else: the next negotiation arbitrates, and will most
// likely plan an `upload`. The user is choosing which bytes to offer, not
// overruling RomM -- which is a sentence the screen owes them, and
// `overlay_conflicts_view.hpp` is where it is written.
//
// Hard rule 4 applies as it does to the rest of `core/`: no libnx header, and
// every file touched goes through `fs::FileSystem`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rommsync/conflict_log.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/state_sync.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/sync_execute.hpp"

namespace rommsync::conflicts {

/// What a recorder needs that is not on a sync report.
struct RecordOptions {
  /// What to call a rom. Null, or an answer that is empty, leaves `rom_name`
  /// empty and the screen falls back to the file name, which is always there.
  ///
  /// Injected rather than taking a `roms::RomIndex`, because the index this
  /// client builds carries `fs_name_no_ext` and **no display name at all**
  /// (rom_index.hpp) -- so what a caller can supply differs by caller, and a
  /// signature naming the index would pin the worse of the two answers.
  std::function<std::string(std::int64_t)> rom_name;

  /// The clock an entry is stamped from. Injected for the test that pins the
  /// stamp, which cannot be written against a clock that moves. Null means
  /// `std::chrono::system_clock::now`.
  std::function<sync::Timestamp()> now;
};

/// Record every overwrite in `report`, and answer how many entries that was.
///
/// `plan` is the plan `report` came from: the server's `reason`,
/// `server_content_hash` and `server_updated_at` are on the **operation** and
/// not on the result, because the executor deliberately does not copy the
/// plan's fields into its report (sync_execute.hpp). `reported` is what step 0
/// sent to `negotiate` and is where the *local* size, MD5 and mtime come from
/// -- the file itself has already been replaced by the time this runs, so
/// reading it now would describe the server's bytes.
///
/// Operations are paired with the plan positionally: `ExecutePlan` produces one
/// result per operation, in the plan's order, up to a cancellation.
std::size_t RecordSaves(History* history, const sync::SyncPlan& plan,
                        const sync::ExecutionReport& report,
                        const std::vector<sync::ClientSaveState>& reported,
                        const RecordOptions& options = {});

/// The same for the states half of a tick (M2-8, #17).
///
/// A state has no `sync::SyncOperation` behind it and **no server digest at
/// all**, so the local facts come from `report.scan` and the server's from
/// `StateOperationResult::server`. `StateOutcome::kKeptBoth` is recorded as
/// `Event::kKeptBoth` with no backup path; a `kDownloaded` with a backup beside
/// it is `Event::kReplaced`.
std::size_t RecordStates(History* history, const sync::StateSyncReport& report,
                         const RecordOptions& options = {});

struct RestoreOptions {
  /// Where the new backup goes. Overridable so a test can look at one somewhere
  /// other than the card's real config directory.
  std::string backup_dir = sync::kBackupDir;

  /// The clock the new backup's name is stamped from. Null means
  /// `std::chrono::system_clock::now`.
  std::function<sync::Timestamp()> now;
};

/// Put the bytes `entry_id` names back on the card.
///
/// Four steps, in this order and no other:
///
///   1. the entry is found, and is one that replaced something;
///   2. the backup it names is still there;
///   3. **the file at `sd_path` is copied into `backup_dir` first**, through
///      `sync::BackUpFirst` -- the same call M2-5 makes, with the save's slot or
///      the state's name as the discriminator, so one rom's save and its state
///      never come to share a backup name;
///   4. and only then is the backup copied onto `sd_path` with
///      `io::CopyAtomically`, which stages at `io::TempPathFor` and commits with
///      two renames. An interruption leaves the old bytes or the new ones and
///      never a splice; the `.tmp` is removed on a failure, or swept by
///      `sync::RecoverStaging` when the process died before it could be.
///
/// The history is **not** updated: the entry stays exactly as it was, so a user
/// who restores can still see what they restored from. What the new backup is
/// called comes back in `RestoreReport::backup_sd_path`.
RestoreReport Restore(fs::FileSystem& files, const History& history, std::int64_t entry_id,
                      const RestoreOptions& options = {});

}  // namespace rommsync::conflicts
