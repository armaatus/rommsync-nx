// M7-1 (#36): the conflict history, the restore, and the screen over them.
//
// Hard rule 2 says a save is copied under `.backup/` before it is overwritten.
// These scenarios are about the half of that guarantee a *person* touches: an
// index that makes the directory legible, and a restore that puts the old bytes
// back without becoming the same failure one level up.
//
// Six of the seven need no server and must stay checked with docker stopped --
// the format, the recorders, the restore, an interrupted one, the service
// behind the two IPC commands, and the screen. `recorded` is the one that needs
// RomM, because the acceptance criterion is a **real** `conflict` producing an
// entry with both hashes and a backup path that exists, and a hand-built plan
// cannot prove that.
//
//   store        -- the file: round-trip, bounded, survives a reboot, and what a
//                   bad row costs (one row, not the file)
//   record       -- a tick's reports become entries; both conflict reasons; the
//                   state a keep-both never backs up; `conflict_show` is not
//                   consulted anywhere in the recording path
//   restore      -- the old bytes exactly, **and a new backup written first**,
//                   which the sandbox audit checks rather than this file
//   keep_local   -- #249: a save conflict's Keep local restores the local bytes
//                   and arms its (rom_id, slot); anything else is refused, and a
//                   plan that answered the key spends it -- one that did not
//                   upload it reported as overtaken (#273); a superseded
//                   conflict and a fifth choice are refused (#273)
//   late_write   -- #270: a write landing between a restore's backup and its
//                   copy stays on the card, and the restore stands down
//   interrupted  -- a restore killed mid-write leaves the old bytes, never a
//                   splice, and no `.tmp` after the sweep
//   service      -- both commands through `ipc::Dispatch` over a real `SdEngine`
//   overlay      -- the screen's decisions, and the greps overlay/AGENTS.md owes
//   recorded     -- a real conflict from the docker RomM, both reasons, and a
//                   restore of what it backed up
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include "crash_disk.hpp"
#include "engine.hpp"
#include "faulty_disk.hpp"
#include "harness.hpp"
#include "rig.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/conflict_log.hpp"
#include "rommsync/conflict_record.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/overlay_conflicts_view.hpp"
// Included beside it deliberately: both live in `rommsync::overlay` and
// `settings_screen.cpp` includes both, so a constant spelled the same way in
// the two is a redefinition. Without this line only the devkitPro build finds
// it, and that build runs after the host suite is already green.
#include "rommsync/overlay_library_model.hpp"
#include "rommsync/sync_execute.hpp"
#include "rommsync/sync_tick.hpp"

namespace {

// Inside the unnamed namespace rather than at file scope, and that is not a
// style choice: `<unistd.h>` -- which `fork()` and `setrlimit()` come from --
// declares a global `sync()`, so `namespace sync = rommsync::sync;` at file
// scope is a redefinition of that name. In here the alias simply shadows it for
// every scenario below, and nothing outside uses either.
namespace auth = rommsync::auth;
namespace conflicts = rommsync::conflicts;
namespace crypto = rommsync::crypto;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace io = rommsync::io;
namespace ipc = rommsync::ipc;
namespace overlay = rommsync::overlay;
namespace scan = rommsync::scan;
namespace state = rommsync::state;
namespace sync = rommsync::sync;
namespace sysmodule = rommsync::sysmodule;

using harness::Fixture;
using harness::Sandbox;
using harness::SavePath;
using harness::StatePath;

constexpr std::int64_t kWhen = 1'757'000'000;

/// Enough rounds to walk past `kConflictMaxEmptyPages` and see the loop stop.
constexpr int kConflictMaxEmptyPagesRounds = overlay::kConflictMaxEmptyPages + 2;

/// A clock that does not move, for every scenario that pins a stamp.
///
/// Not a convenience: a backup name carries a second, and a test that let the
/// clock run would pass or fail on how fast the machine is
/// (`test_sync_execute.cpp`'s `OptionsAt`, for its reason).
conflicts::RecordOptions At(std::int64_t unix_seconds) {
  conflicts::RecordOptions options;
  options.now = [unix_seconds]() {
    return sync::Timestamp{} + std::chrono::seconds{unix_seconds};
  };
  return options;
}

sync::Timestamp Moment(std::int64_t unix_seconds) {
  return sync::Timestamp{} + std::chrono::seconds{unix_seconds};
}

conflicts::Entry SaveEntry(std::int64_t rom_id, std::string file_name) {
  conflicts::Entry entry;
  entry.kind = conflicts::EntryKind::kSave;
  entry.event = conflicts::Event::kConflict;
  entry.rom_id = rom_id;
  entry.rom_name = "Game";
  entry.file_name = std::move(file_name);
  entry.slot = "retroarch-srm";
  entry.emulator = "retroarch";
  entry.when = kWhen;
  entry.reason = "Both sides changed since last sync";
  entry.sd_path = SavePath("game.srm");
  entry.local_size_bytes = 10;
  entry.local_content_hash = crypto::Md5Hex("device v2\n");
  entry.local_modified = kWhen - 60;
  entry.server_content_hash = crypto::Md5Hex("server v2\n");
  entry.server_updated_at = "2026-09-04T11:36:27+00:00";
  entry.backup_sd_path = std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000000.srm";
  return entry;
}

/// A finished restore of `entry_id`, as `GetRestoreState` answers it.
ipc::RestoreState Done(std::int64_t entry_id, const conflicts::RestoreReport& report) {
  ipc::RestoreState state;
  state.phase = ipc::RestorePhase::kDone;
  state.entry_id = entry_id;
  state.report = report;
  return state;
}

/// A restore of `entry_id` the sysmodule has taken and not finished.
ipc::RestoreState Running(std::int64_t entry_id) {
  ipc::RestoreState state;
  state.phase = ipc::RestorePhase::kRunning;
  state.entry_id = entry_id;
  return state;
}

// --- store --------------------------------------------------------------------
//
// The file. Needs no server, and must stay checked with docker stopped: what it
// pins is what a *reader* does with bytes a yanked card left behind, and a
// healthy RomM cannot produce one of those.

void Store(checks::Checks& c) {
  Sandbox sandbox(c, "conflicts-store");
  const std::string path = sandbox.Host(std::string(conflicts::kHistorySdPath));

  conflicts::History history(path);
  c.Expect(history.Load().size() == 1,
           "a console with no conflicts.db says so once and carries on");
  c.ExpectEq(history.size(), std::size_t{0}, "...with an empty history, which is a history");

  const conflicts::StoreResult first = history.Append(SaveEntry(4, "game.srm"));
  c.Expect(first.ok(), "the first entry is written: " + first.message);
  c.ExpectEq(first.id, std::int64_t{1}, "and takes the first id");

  conflicts::Entry second = SaveEntry(5, "other.srm");
  second.kind = conflicts::EntryKind::kState;
  second.slot.reset();
  second.event = conflicts::Event::kKeptBoth;
  second.reason.clear();
  second.server_content_hash.reset();
  second.server_size_bytes = 4096;
  second.backup_sd_path.clear();
  c.Expect(history.Append(second).ok(), "and a state entry beside it");

  c.ExpectEq(history.size(), std::size_t{2}, "both are held");
  c.Expect(history.entries()[0].rom_id == 5, "newest first, which is the order the screen draws");

  // A reboot. The acceptance criterion is that the history survives one, so it
  // is read back through a second object rather than asserted in memory.
  conflicts::History reopened(path);
  const std::vector<std::string> notes = reopened.Load();
  c.Expect(notes.empty(), "a well-formed history loads with nothing to complain about");
  c.ExpectEq(reopened.size(), std::size_t{2}, "and both entries survived the reboot");
  const conflicts::Entry& save = reopened.entries()[1];
  c.ExpectEq(save.reason, std::string("Both sides changed since last sync"),
             "the server's own sentence round-trips");
  c.Expect(save.server_content_hash.has_value() &&
               *save.server_content_hash == crypto::Md5Hex("server v2\n"),
           "and so does the server's digest");
  c.Expect(save.restorable(), "a conflict with a backup path is restorable");
  const conflicts::Entry& state = reopened.entries()[0];
  c.Expect(state.kind == conflicts::EntryKind::kState, "the state is read back as a state");
  c.Expect(!state.server_content_hash.has_value(),
           "with no server digest at all -- RomM computes none for a state");
  c.Expect(!state.restorable(), "and a keep-both replaced nothing, so nothing can be put back");
  c.ExpectEq(state.server_size_bytes, std::int64_t{4096},
             "its length is the whole of what the server side is");

  // Ids keep increasing across the reboot. A counter rebuilt from the rows would
  // restart at one on an emptied history and hand a live overlay page an id
  // naming a different conflict.
  const conflicts::StoreResult third = reopened.Append(SaveEntry(6, "third.srm"));
  c.ExpectEq(third.id, std::int64_t{3}, "the next id carries across a reboot");

  // Bounded, and the bound does not touch the backups.
  {
    Sandbox bounded(c, "conflicts-bound");
    conflicts::History full(bounded.Host(std::string(conflicts::kHistorySdPath)));
    full.Load();
    for (std::size_t index = 0; index < conflicts::kMaxEntries + 8; ++index) {
      conflicts::Entry entry = SaveEntry(4, "game" + std::to_string(index) + ".srm");
      c.Expect(full.Append(std::move(entry)).ok(), "an entry is appended");
    }
    c.ExpectEq(full.size(), conflicts::kMaxEntries, "the history stops at its cap");
    c.Expect(full.entries().front().file_name ==
                 "game" + std::to_string(conflicts::kMaxEntries + 7) + ".srm",
             "keeping the newest");
    // The writer's own bound has to be reachable back through the reader, or the
    // writer produces a file the reader discards -- `core.state_db`'s assertion,
    // here.
    const std::string text = io::ReadFile(full.path()).contents;
    c.Expect(text.size() <= conflicts::kMaxHistoryBytes,
             "a full history serializes to less than the byte bound: " +
                 std::to_string(text.size()));
    conflicts::History back(full.path());
    back.Load();
    c.ExpectEq(back.size(), conflicts::kMaxEntries, "...and reads back whole");
  }

  // A row that will not read costs that row and nothing else, which is where
  // this format parts company with `state.db` -- see `ParseHistory`. Thirty-one
  // good pointers at real backups are not worth throwing away over one.
  {
    conflicts::Entry kept = SaveEntry(4, "kept.srm");
    kept.id = 7;
    const conflicts::LoadedHistory partial = conflicts::ParseHistory(
        std::string(conflicts::kFormatMagic) + " " +
        std::to_string(conflicts::kFormatVersion) + " 9\n" +
        conflicts::SerializeEntry(kept) + "\n" + "{\"id\":\n");
    c.ExpectEq(partial.entries.size(), std::size_t{1}, "the readable row is kept");
    c.Expect(!partial.diagnostics.empty(), "and the unreadable one is named");
    c.Expect(partial.diagnostics[0].find("still on the card") != std::string::npos,
             "...saying the backup it pointed at was not deleted with it: " +
                 partial.diagnostics[0]);
  }

  // #240: where else the losing bytes are -- on the server as an archival row,
  // or on this card alone -- round-trips. And a row written before #240, which
  // has neither field, still reads: it is an index to a real backup, and the
  // file's version is not bumped for a field an older build can ignore.
  {
    conflicts::Entry on_server = SaveEntry(4, "kept.srm");
    on_server.id = 3;
    on_server.local_copy = conflicts::LocalCopy::kOnServer;
    on_server.archive_save_id = 77;
    conflicts::Entry card_only = SaveEntry(5, "card.srm");
    card_only.id = 2;
    card_only.local_copy = conflicts::LocalCopy::kCardOnly;
    conflicts::Entry before = SaveEntry(6, "before.srm");
    before.id = 1;
    // Nothing recorded is written as nothing at all: the row rc1 wrote.
    const std::string old_row = conflicts::SerializeEntry(before);
    c.Expect(old_row.find("local_copy") == std::string::npos &&
                 old_row.find("archive_save_id") == std::string::npos,
             "an entry with nothing recorded spells neither key: " + old_row);
    const conflicts::LoadedHistory read = conflicts::ParseHistory(
        std::string(conflicts::kFormatMagic) + " " + std::to_string(conflicts::kFormatVersion) +
        " 9\n" + conflicts::SerializeEntry(on_server) + "\n" +
        conflicts::SerializeEntry(card_only) + "\n" + old_row + "\n");
    c.ExpectEq(read.entries.size(), std::size_t{3}, "all three rows read back");
    if (read.entries.size() == 3) {
      c.Expect(read.entries[0].local_copy == conflicts::LocalCopy::kOnServer &&
                   read.entries[0].archive_save_id == 77,
               std::string("a copy on the server round-trips with its row: ") +
                   conflicts::ToString(read.entries[0].local_copy));
      c.Expect(read.entries[1].local_copy == conflicts::LocalCopy::kCardOnly &&
                   read.entries[1].archive_save_id == 0,
               std::string("a copy on the card alone round-trips: ") +
                   conflicts::ToString(read.entries[1].local_copy));
      c.Expect(read.entries[2].local_copy == conflicts::LocalCopy::kUnrecorded,
               std::string("a pre-#240 row reads as not recorded: ") +
                   conflicts::ToString(read.entries[2].local_copy));
    }
  }

  // A header that is not this file's is fatal, because those bytes are not this
  // file. Naming the expected header rather than quoting what was found: a
  // corrupt card region holds anything.
  {
    const conflicts::LoadedHistory alien = conflicts::ParseHistory("rommsync-state 2\n{}\n");
    c.Expect(alien.entries.empty(), "a file with somebody else's header yields no entries");
    c.Expect(!alien.diagnostics.empty(), "and says so");
  }

  // An entry naming no path is refused rather than stored: `sd_path` is opened,
  // not read, and an entry that cannot say where to write is a restore aimed at
  // nothing.
  {
    conflicts::History strict(sandbox.Host("/config/rommsync/strict.db"));
    strict.Load();
    conflicts::Entry blank = SaveEntry(4, "game.srm");
    blank.sd_path.clear();
    const conflicts::StoreResult refused = strict.Append(blank);
    c.Expect(refused.error == conflicts::StoreError::kUnusableEntry,
             std::string("an entry with no path is refused: ") +
                 conflicts::ToString(refused.error));
    c.ExpectEq(strict.size(), std::size_t{0}, "and nothing was stored");
  }

  // The strings that cross the IPC wire are cut before they are stored, so the
  // page bound is a bound rather than an estimate.
  {
    conflicts::History cut(sandbox.Host("/config/rommsync/cut.db"));
    cut.Load();
    conflicts::Entry long_name = SaveEntry(4, "game.srm");
    long_name.rom_name = std::string(conflicts::kMaxTextBytes * 3, 'x');
    c.Expect(cut.Append(long_name).ok(), "an entry with a very long rom name is stored");
    c.Expect(cut.entries()[0].rom_name.size() <= conflicts::kMaxTextBytes + 3,
             "with the name cut to what a row may carry: " +
                 std::to_string(cut.entries()[0].rom_name.size()));
  }
}

// --- record -------------------------------------------------------------------
//
// A tick's reports become entries. Hand-built plans, so this needs no server and
// stays checked with docker stopped; `recorded` is the same claim against a real
// RomM.

/// The plan and the report a resolved conflict produces, as `ExecutePlan` builds
/// them. Built here rather than executed so both reasons can be covered without
/// two round trips.
void AddConflict(sync::SyncPlan* plan, sync::ExecutionReport* report, std::int64_t rom_id,
                 const std::string& slot, const char* reason, sync::Reason classified,
                 const std::string& backup) {
  sync::SyncOperation operation;
  operation.action = sync::Action::kConflict;
  operation.rom_id = rom_id;
  operation.slot = slot;
  operation.file_name = "server-side-name [2026-09-04_11-12-27].srm";
  operation.emulator = std::string("retroarch");
  operation.reason = classified;
  operation.reason_text = reason;
  operation.server_content_hash = crypto::Md5Hex("server\n");
  operation.server_updated_at = std::string("2026-09-04T11:36:27+00:00");
  plan->operations.push_back(std::move(operation));

  sync::OperationResult result;
  result.action = sync::Action::kConflict;
  result.rom_id = rom_id;
  result.slot = slot;
  result.outcome = sync::OperationOutcome::kKeptBoth;
  result.sd_path = SavePath("game.srm");
  result.backup_sd_path = backup;
  report->operations.push_back(std::move(result));
}

sync::ClientSaveState Reported(std::int64_t rom_id, const std::string& slot,
                               const std::string& file_name, const std::string& bytes) {
  sync::ClientSaveState save;
  save.rom_id = rom_id;
  save.file_name = file_name;
  save.slot = slot;
  save.emulator = std::string("retroarch");
  save.content_hash = crypto::Md5Hex(bytes);
  save.updated_at = Moment(kWhen - 120);
  save.file_size_bytes = static_cast<std::int64_t>(bytes.size());
  return save;
}

void Record(checks::Checks& c) {
  Sandbox sandbox(c, "conflicts-record");
  conflicts::History history(sandbox.Host(std::string(conflicts::kHistorySdPath)));
  history.Load();

  sync::SyncPlan plan;
  sync::ExecutionReport report;
  // **Both conflict reasons**, which is the acceptance criterion a `switch` on
  // the first one swallows: the second is the no-history conflict, and on a
  // conflict the default branch is the one that can overwrite a save.
  AddConflict(&plan, &report, 4, "retroarch-srm", "Both sides changed since last sync",
              sync::Reason::kBothChanged, std::string(harness::kBackupDir) + "/4-a.srm");
  AddConflict(&plan, &report, 5, "retroarch-state", "Same timestamp but different content",
              sync::Reason::kSameTimestampDifferentContent,
              std::string(harness::kBackupDir) + "/5-a.srm");

  // A download that replaced a file, and one that replaced nothing. Only the
  // first is an entry: nothing on the card changed for the second, so there is
  // nothing a player could want back.
  {
    sync::SyncOperation operation;
    operation.action = sync::Action::kDownload;
    operation.rom_id = 6;
    operation.slot = "retroarch-srm";
    operation.reason_text = "The server's copy is newer";
    operation.server_content_hash = crypto::Md5Hex("server\n");
    plan.operations.push_back(operation);
    sync::OperationResult replaced;
    replaced.action = sync::Action::kDownload;
    replaced.rom_id = 6;
    replaced.slot = "retroarch-srm";
    replaced.outcome = sync::OperationOutcome::kDownloaded;
    replaced.sd_path = SavePath("six.srm");
    replaced.backup_sd_path = std::string(harness::kBackupDir) + "/6-a.srm";
    report.operations.push_back(replaced);

    plan.operations.push_back(operation);
    sync::OperationResult placed = replaced;
    placed.rom_id = 7;
    placed.backup_sd_path.clear();
    plan.operations.back().rom_id = 7;
    report.operations.push_back(placed);
  }

  // ...and an upload, which touches nothing on the card.
  {
    sync::SyncOperation operation;
    operation.action = sync::Action::kUpload;
    operation.rom_id = 8;
    operation.slot = "retroarch-srm";
    plan.operations.push_back(operation);
    sync::OperationResult uploaded;
    uploaded.action = sync::Action::kUpload;
    uploaded.rom_id = 8;
    uploaded.slot = "retroarch-srm";
    uploaded.outcome = sync::OperationOutcome::kUploaded;
    report.operations.push_back(uploaded);
  }

  const std::vector<sync::ClientSaveState> reported = {
      Reported(4, "retroarch-srm", "four.srm", "device four\n"),
      Reported(5, "retroarch-state", "five.state", "device five\n"),
      Reported(6, "retroarch-srm", "six.srm", "device six\n"),
  };

  // #240: the first conflict's local copy went up as an archival row, the
  // second's did not.
  report.operations[0].archive = sync::ArchiveOutcome::kKept;
  report.operations[0].archive_save_id = 77;
  report.operations[1].archive = sync::ArchiveOutcome::kFailed;

  conflicts::RecordOptions options = At(kWhen);
  options.rom_name = [](std::int64_t rom_id) { return "Rom " + std::to_string(rom_id); };
  const std::size_t recorded = conflicts::RecordSaves(&history, plan, report, reported, options);
  c.ExpectEq(recorded, std::size_t{3},
             "two conflicts and the download that replaced a file, and nothing else");

  // Newest first, so the two conflicts are at the end.
  const conflicts::Entry& same_timestamp = history.entries()[1];
  const conflicts::Entry& both_changed = history.entries()[2];
  c.ExpectEq(both_changed.reason, std::string("Both sides changed since last sync"),
             "the history conflict is recorded with the server's sentence");
  c.ExpectEq(same_timestamp.reason, std::string("Same timestamp but different content"),
             "...and so is the no-history one, which a default branch would swallow");
  c.ExpectEq(both_changed.file_name, std::string("four.srm"),
             "the entry carries THIS console's file name, not the server's tagged one");
  c.ExpectEq(both_changed.rom_name, std::string("Rom 4"), "and whatever the caller calls the rom");
  c.ExpectEq(both_changed.local_content_hash, crypto::Md5Hex("device four\n"),
             "the local digest is the one step 0 reported -- the card now holds the server's");
  c.Expect(both_changed.server_content_hash.has_value() &&
               *both_changed.server_content_hash == crypto::Md5Hex("server\n"),
           "and the server's comes off the plan, where the executor left it");
  c.ExpectEq(both_changed.backup_sd_path, std::string(harness::kBackupDir) + "/4-a.srm",
             "the backup path is the one M2-5 wrote, stored exactly");
  c.ExpectEq(both_changed.when, kWhen, "stamped from the clock it was given");
  c.Expect(both_changed.local_copy == conflicts::LocalCopy::kOnServer &&
               both_changed.archive_save_id == 77,
           std::string("a conflict records that its local copy is on the server, and as which "
                       "row: ") +
               conflicts::ToString(both_changed.local_copy));
  c.Expect(same_timestamp.local_copy == conflicts::LocalCopy::kCardOnly,
           std::string("...or that the server holds none: ") +
               conflicts::ToString(same_timestamp.local_copy));
  c.Expect(history.entries()[0].local_copy == conflicts::LocalCopy::kUnrecorded,
           "a replacement was never a question of keeping both");
  c.Expect(history.entries()[0].event == conflicts::Event::kReplaced,
           "a download that landed on an existing file is a replacement, not a conflict");

  // The states half. A `kKeptBoth` transferred nothing and has no backup; a
  // `kDownloaded` with one is a replacement.
  {
    sync::StateSyncReport states;
    states.ran = true;
    scan::StateFile scanned;
    scanned.rom_id = 9;
    scanned.sd_path = StatePath("nine.state");
    scanned.file_name = "nine.state";
    scanned.emulator = "retroarch";
    scanned.content_hash = crypto::Md5Hex("device state\n");
    scanned.size_bytes = 14;
    scanned.modified_unix = kWhen - 300;
    states.scan.states.push_back(scanned);

    sync::StateOperationResult kept;
    kept.action = sync::StateAction::kKeepBoth;
    kept.rom_id = 9;
    kept.file_name = "nine.state";
    kept.outcome = sync::StateOutcome::kKeptBoth;
    kept.sd_path = StatePath("nine.state");
    kept.server.id = 77;
    kept.server.rom_id = 9;
    kept.server.file_name = "nine.state";
    kept.server.emulator = "retroarch";
    kept.server.file_size_bytes = 4096;
    kept.server.updated_at = Moment(kWhen - 30);
    states.operations.push_back(kept);

    sync::StateOperationResult overwritten = kept;
    overwritten.action = sync::StateAction::kDownload;
    overwritten.outcome = sync::StateOutcome::kDownloaded;
    overwritten.backup_sd_path = std::string(harness::kBackupDir) + "/9-state-nine-1757000000.state";
    states.operations.push_back(overwritten);

    c.ExpectEq(conflicts::RecordStates(&history, states, options), std::size_t{2},
               "a keep-both and a replacement are both worth showing");
    const conflicts::Entry& replaced = history.entries()[0];
    const conflicts::Entry& both = history.entries()[1];
    c.Expect(both.event == conflicts::Event::kKeptBoth,
             "the keep-both is its own thing, not a conflict that was resolved");
    c.Expect(both.backup_sd_path.empty(), "...with no backup, because nothing was overwritten");
    c.Expect(!both.restorable(), "so the screen has nothing to offer for it");
    c.Expect(!replaced.server_content_hash.has_value(),
             "a state entry never carries a server digest -- RomM computes none");
    c.ExpectEq(replaced.server_size_bytes, std::int64_t{4096},
               "what it carries instead is the server's length");
    c.ExpectEq(replaced.local_content_hash, crypto::Md5Hex("device state\n"),
               "and this console's own digest, from the scan");
    c.ExpectEq(replaced.emulator, std::string("retroarch"),
               "the emulator comes off the scan and the server row -- it is NOT on "
               "StateOperationResult, which is what #36 assumed");
    c.Expect(replaced.restorable(), "a state that was replaced can be put back");
  }

  // M10-7 (#233): a save removed because RomM deleted it is listed, so the
  // player can find its backup and put it back. A kept delete changed nothing
  // on the card, and one with nothing to remove has no backup: neither is an
  // entry.
  {
    Sandbox deleted(c, "conflicts-record-deleted");
    const std::unique_ptr<fs::FileSystem> card =
        rommsync::host::MakeNativeFileSystem(deleted.root().string());
    conflicts::History removals(deleted.Host(std::string(conflicts::kHistorySdPath)));
    removals.Load();

    sync::SyncPlan delete_plan;
    sync::ExecutionReport delete_report;
    const std::string removed_backup =
        std::string(harness::kBackupDir) + "/10-retroarch-srm-1757000000.srm";
    for (const auto& [rom_id, outcome, backup] :
         std::vector<std::tuple<std::int64_t, sync::OperationOutcome, std::string>>{
             {10, sync::OperationOutcome::kRemoved, removed_backup},
             {11, sync::OperationOutcome::kKept, std::string()},
             {12, sync::OperationOutcome::kRemoved, std::string()}}) {
      sync::SyncOperation operation;
      operation.action = sync::Action::kDelete;
      operation.rom_id = rom_id;
      operation.slot = "retroarch-srm";
      operation.file_name = "ten.srm";
      operation.reason = sync::Reason::kDeletedOnServer;
      operation.reason_text = sync::ReasonText(sync::Reason::kDeletedOnServer);
      delete_plan.operations.push_back(operation);
      sync::OperationResult result;
      result.action = sync::Action::kDelete;
      result.rom_id = rom_id;
      result.slot = "retroarch-srm";
      result.outcome = outcome;
      result.sd_path = SavePath("ten.srm");
      result.backup_sd_path = backup;
      delete_report.operations.push_back(result);
    }
    const std::vector<sync::ClientSaveState> deleted_reported = {
        Reported(10, "retroarch-srm", "ten.srm", "the save RomM deleted\n")};
    c.ExpectEq(conflicts::RecordSaves(&removals, delete_plan, delete_report, deleted_reported,
                                      options),
               std::size_t{1}, "a removal with a backup is an entry, and nothing else is");
    if (removals.size() == 1) {
      const conflicts::Entry& removal = removals.entries()[0];
      c.Expect(removal.event == conflicts::Event::kDeleted,
               std::string("listed as a deletion: ") + conflicts::ToString(removal.event));
      c.ExpectEq(removal.reason, std::string("Save was deleted on the server"),
                 "with the server's sentence");
      c.ExpectEq(removal.backup_sd_path, removed_backup, "and the backup it can be restored from");
      c.ExpectEq(removal.local_content_hash, crypto::Md5Hex("the save RomM deleted\n"),
                 "and the digest of the bytes that were removed");
      c.Expect(removal.restorable(), "a removed save can be put back");
      conflicts::History reread(deleted.Host(std::string(conflicts::kHistorySdPath)));
      reread.Load();
      c.Expect(reread.size() == 1 && reread.entries()[0].event == conflicts::Event::kDeleted,
               "and conflicts.db reads the deletion back as one");

      // ...and is. Nothing is at the path any more, so the restore replaces
      // nothing and writes the bytes back.
      c.Expect(deleted.Write(removed_backup, "the save RomM deleted\n"), "the backup is on the card");
      conflicts::RestoreOptions restore;
      restore.backup_dir = harness::kBackupDir;
      restore.now = []() { return Moment(kWhen + 600); };
      const conflicts::RestoreReport back =
          conflicts::Restore(*card, removals, removal.id, restore);
      c.Expect(back.ok(), "the removed save is restored: " + back.message);
      c.ExpectEq(deleted.Read(SavePath("ten.srm")), std::string("the save RomM deleted\n"),
                 "byte for byte");
    }
  }

  // **`conflict_show` is not in the recording path at all.** Grepped rather than
  // reasoned about: the setting hides the screen, and a console that had it off
  // for a month has to list every conflict when it goes on.
  for (const char* path : {ROMMSYNC_CONFLICT_LOG_SRC, ROMMSYNC_CONFLICT_RECORD_SRC,
                           ROMMSYNC_CONFLICT_LOG_HDR, ROMMSYNC_CONFLICT_RECORD_HDR}) {
    std::ifstream source(path);
    c.Expect(source.good(), std::string("readable: ") + path);
    std::string line;
    int at = 0;
    while (std::getline(source, line)) {
      ++at;
      const std::size_t found = line.find("conflict_show");
      // Named in the prose, which is the point of the prose; never read.
      const bool prose = line.find("///") != std::string::npos ||
                         line.find("//") != std::string::npos;
      c.Expect(found == std::string::npos || prose,
               std::string(path) + ":" + std::to_string(at) +
                   " reads conflict_show; the setting hides the screen, never the recording");
    }
  }
}

// --- restore ------------------------------------------------------------------
//
// The bytes back, and a backup of what they replaced written **first**. The
// second half is not asserted here on purpose: `harness::Sandbox`'s teardown
// audit is what checks it, so a restore that wrote a backup afterwards, or to
// the wrong place, or holding the wrong bytes, fails without this file having to
// remember to look.

void Restore(checks::Checks& c) {
  Sandbox sandbox(c, "conflicts-restore");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  const std::string previous = "the bytes this console had, and wants back\n";
  const std::string server = "the server's copy, which the sync put here\n";

  // The card as a resolved conflict left it: the server's copy at the save
  // path, and the local bytes under `.backup/`.
  sandbox.SeedSave(SavePath("game.srm"), server);
  const std::string backup = std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000000.srm";
  c.Expect(sandbox.Write(backup, previous), "the backup M2-5 wrote is on the card");

  conflicts::History history(sandbox.Host(std::string(conflicts::kHistorySdPath)));
  history.Load();
  conflicts::Entry entry = SaveEntry(4, "game.srm");
  entry.backup_sd_path = backup;
  const std::int64_t id = history.Append(entry).id;
  c.Expect(id != 0, "the conflict is in the history");

  conflicts::RestoreOptions options;
  options.backup_dir = harness::kBackupDir;
  options.now = []() { return Moment(kWhen + 500); };

  const conflicts::RestoreReport done = conflicts::Restore(*files, history, id, options);
  c.Expect(done.ok(), std::string("the restore ran: ") + conflicts::ToString(done.outcome) + " -- " +
                          done.message);
  c.ExpectEq(sandbox.Read(SavePath("game.srm")), previous,
             "the pre-conflict bytes are back, exactly");
  c.Expect(!done.backup_sd_path.empty(),
           "and a backup of what it replaced was written: " + done.message);
  c.ExpectEq(sandbox.Read(done.backup_sd_path), server,
             "...holding the server's copy, which is what a restore must not eat");
  c.Expect(done.backup_sd_path != backup,
           "to a name of its own rather than over the backup it restored from");
  c.ExpectEq(sandbox.Read(backup), previous, "the original backup is untouched");
  c.Expect(!std::filesystem::exists(sandbox.Host(SavePath("game.srm")) + ".tmp"),
           "and nothing staged is left beside the save");
  c.Expect(done.message.find("next sync") != std::string::npos,
           "the sentence says the server still decides -- hard rule 3: " + done.message);

  // The entry stays put, so a user can still see what they restored from.
  c.ExpectEq(history.size(), std::size_t{1}, "the history is not rewritten by a restore");

  // An entry whose backup is gone is a named refusal rather than a write.
  {
    Sandbox gone(c, "conflicts-restore-gone");
    const std::unique_ptr<fs::FileSystem> card =
        rommsync::host::MakeNativeFileSystem(gone.root().string());
    gone.SeedSave(SavePath("game.srm"), server);
    conflicts::History missing(gone.Host(std::string(conflicts::kHistorySdPath)));
    missing.Load();
    conflicts::Entry orphan = SaveEntry(4, "game.srm");
    orphan.backup_sd_path = std::string(harness::kBackupDir) + "/4-retroarch-srm-9999.srm";
    const std::int64_t orphan_id = missing.Append(orphan).id;
    const conflicts::RestoreReport refused =
        conflicts::Restore(*card, missing, orphan_id, options);
    c.Expect(refused.outcome == conflicts::RestoreOutcome::kBackupMissing,
             std::string("a backup that is gone is named: ") + conflicts::ToString(refused.outcome));
    c.ExpectEq(gone.Read(SavePath("game.srm")), server, "and the save is exactly as it was");
    c.Expect(refused.backup_sd_path.empty(), "with no backup written for a restore that did not run");
  }

  // A keep-both state replaced nothing, so there is nothing to put back -- and
  // pressing restore on one must not write a file.
  {
    Sandbox both(c, "conflicts-restore-kept");
    const std::unique_ptr<fs::FileSystem> card =
        rommsync::host::MakeNativeFileSystem(both.root().string());
    conflicts::History kept(both.Host(std::string(conflicts::kHistorySdPath)));
    kept.Load();
    conflicts::Entry entry_kept = SaveEntry(9, "nine.state");
    entry_kept.kind = conflicts::EntryKind::kState;
    entry_kept.event = conflicts::Event::kKeptBoth;
    entry_kept.slot.reset();
    entry_kept.backup_sd_path.clear();
    entry_kept.sd_path = StatePath("nine.state");
    const conflicts::RestoreReport nothing =
        conflicts::Restore(*card, kept, kept.Append(entry_kept).id, options);
    c.Expect(nothing.outcome == conflicts::RestoreOutcome::kNothingToRestore,
             std::string("a keep-both has nothing to restore: ") +
                 conflicts::ToString(nothing.outcome));
  }

  // An id the history does not hold -- a stale overlay page, or a reboot -- is
  // refused rather than guessed at.
  {
    const conflicts::RestoreReport stale = conflicts::Restore(*files, history, 9'999, options);
    c.Expect(stale.outcome == conflicts::RestoreOutcome::kNoSuchEntry,
             std::string("an id that is not in the history is refused: ") +
                 conflicts::ToString(stale.outcome));
  }

  // A **state's** restore uses the state discriminator, so one rom's save and
  // its state never come to share a backup name.
  {
    Sandbox states(c, "conflicts-restore-state");
    const std::unique_ptr<fs::FileSystem> card =
        rommsync::host::MakeNativeFileSystem(states.root().string());
    const std::string state_previous = "the state this console had\n";
    states.SeedState(StatePath("nine.state"), "the server's state\n");
    const std::string state_backup =
        std::string(harness::kBackupDir) + "/9-state-nine-1757000000.state";
    c.Expect(states.Write(state_backup, state_previous), "the state's backup is on the card");

    conflicts::History log(states.Host(std::string(conflicts::kHistorySdPath)));
    log.Load();
    conflicts::Entry state_entry = SaveEntry(9, "nine.state");
    state_entry.kind = conflicts::EntryKind::kState;
    state_entry.event = conflicts::Event::kReplaced;
    state_entry.slot.reset();
    state_entry.reason.clear();
    state_entry.server_content_hash.reset();
    state_entry.sd_path = StatePath("nine.state");
    state_entry.backup_sd_path = state_backup;

    const conflicts::RestoreReport put_back =
        conflicts::Restore(*card, log, log.Append(state_entry).id, options);
    c.Expect(put_back.ok(), "the state is restored: " + put_back.message);
    c.ExpectEq(states.Read(StatePath("nine.state")), state_previous, "byte for byte");
    // `state-nine`, not `state-nine.state`: the extension is already the
    // backup's own suffix, so `sync::StateBackupDiscriminator` leaves it off.
    c.Expect(put_back.backup_sd_path.find("-state-nine-") != std::string::npos,
             "and the backup it wrote first carries the state discriminator: " +
                 put_back.backup_sd_path);
  }
}

// --- late_write ---------------------------------------------------------------
//
// #270: #263's guarantee, held to the restore. A restore starts from the overlay
// over a game that may be running, and `SdEngine::RunRestore` does not ask #242's
// hold -- so a game can write the file after the restore has backed it up and
// before the backup it restores from is copied over it. That write is in no copy
// anywhere, and the restore has to see it and stand down.

/// When the game writes: as the restore's own backup is published, or once the
/// backup's bytes have been staged beside the file -- after the slow part, where
/// a check placed before the copy would already have passed.
enum class Late { kAfterBackup, kDuringCopy };

/// One restore of `entry` with the game writing `late` over `sd_path` at `when`.
/// `keep` routes it through Keep local instead, which must arm nothing for a
/// restore that did not land.
void RestoreUnderLateWrite(checks::Checks& c, const std::string& label, conflicts::Entry entry,
                           bool keep, Late when = Late::kAfterBackup) {
  Sandbox sandbox(c, "conflicts-late-" + label);
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string previous = "the bytes this console had, and wants back\n";
  const std::string server = "the server's copy, which the sync put here\n";
  const std::string late = "progress the game wrote after the restore's backup\n";

  sandbox.SeedSave(entry.sd_path, server);
  c.Expect(sandbox.Write(entry.backup_sd_path, previous), label + ": the backup is on the card");
  conflicts::History history(sandbox.Host(std::string(conflicts::kHistorySdPath)));
  history.Load();
  const std::int64_t id = history.Append(entry).id;

  conflicts::RestoreOptions options;
  options.backup_dir = harness::kBackupDir;
  options.now = []() { return Moment(kWhen + 500); };

  conflicts::RestoreReport done;
  state::Baseline::Key key{0, std::nullopt};
  {
    // `Reseed`: from here on the late bytes are the file, and the teardown audit
    // holds the restore to them.
    const auto write = [&] { sandbox.Reseed(entry.sd_path, late); };
    std::optional<faulty::LateWrite> game;
    if (when == Late::kAfterBackup) {
      game.emplace(write);
    } else {
      game.emplace(write, io::TempPathFor(sandbox.Host(entry.sd_path)));
    }
    done = keep ? conflicts::KeepLocal(*files, history, id, {}, &key, options)
                : conflicts::Restore(*files, history, id, options);
    c.Expect(game->fired(), label + ": the game wrote between the backup and the commit");
  }

  c.ExpectEq(sandbox.Read(entry.sd_path), late,
             label + ": the write that landed after the backup is still on the card");
  c.Expect(!done.ok(), label + ": and the restore did not claim to have run: " + done.message);
  c.ExpectEq(std::string(conflicts::ToString(done.outcome)), std::string("changed_since_backup"),
             label + ": named as a file that moved since its backup -- " + done.message);
  c.Expect(!done.backup_sd_path.empty() && sandbox.Read(done.backup_sd_path) == server,
           label + ": the restore's own backup is left, holding what it was taken of: " +
               done.backup_sd_path);
  c.ExpectEq(sandbox.Read(entry.backup_sd_path), previous,
             label + ": and the backup it would have restored from is untouched");
  c.Expect(!std::filesystem::exists(io::TempPathFor(sandbox.Host(entry.sd_path))),
           label + ": with nothing staged left beside the file");
  if (keep) {
    c.Expect(key.first == 0 && !key.second.has_value(),
             label + ": and Keep local armed nothing, so no upload overrides the server for it");
  }
}

void LateWrite(checks::Checks& c) {
  conflicts::Entry save = SaveEntry(4, "game.srm");
  RestoreUnderLateWrite(c, "save", save, /*keep=*/false);

  // Keep local is the restore plus an armed key; the key is what would send the
  // file over the server's copy, so it must not be armed over a write it skipped.
  RestoreUnderLateWrite(c, "keep-local", save, /*keep=*/true);

  // The same write landing while the backup's bytes stream into place beside
  // the file: for a state that copy is the long part, so the check has to come
  // after it rather than before.
  RestoreUnderLateWrite(c, "during-copy", save, /*keep=*/false, Late::kDuringCopy);

  // A state: tens of megabytes on a console, and what RetroArch writes on a
  // hotkey -- the likelier of the two to be written mid-restore.
  conflicts::Entry state_entry = SaveEntry(9, "nine.state");
  state_entry.kind = conflicts::EntryKind::kState;
  state_entry.event = conflicts::Event::kReplaced;
  state_entry.slot.reset();
  state_entry.reason.clear();
  state_entry.server_content_hash.reset();
  state_entry.sd_path = StatePath("nine.state");
  state_entry.backup_sd_path = std::string(harness::kBackupDir) + "/9-state-nine-1757000000.state";
  RestoreUnderLateWrite(c, "state", state_entry, /*keep=*/false);
}

// --- keep_local ---------------------------------------------------------------
//
// #249: the player's Keep local on a save conflict. The local bytes go back on
// the card through the restore -- backed up first, as any overwrite -- and the
// conflict's `(rom_id, slot)` is what the next upload of them goes over the
// server's copy with. Anything that is not a save conflict is refused before a
// byte is written, and the armed keys are spent by the plan that answered them.

/// One operation's result, for `SpendKeepLocal`.
sync::OperationResult Ran(std::int64_t rom_id, std::string slot, sync::Action action,
                          sync::OperationOutcome outcome,
                          sync::OperationError error = sync::OperationError::kNone) {
  sync::OperationResult result;
  result.rom_id = rom_id;
  result.slot = std::move(slot);
  result.action = action;
  result.outcome = outcome;
  result.error = error;
  return result;
}

void KeepLocal(checks::Checks& c) {
  const std::string previous = "the bytes this console had, and chose to keep\n";
  const std::string server = "the server's copy, which the conflict put here\n";
  conflicts::RestoreOptions options;
  options.backup_dir = harness::kBackupDir;
  options.now = []() { return Moment(kWhen + 500); };

  // A save conflict: the local bytes go back, the server's copy is backed up
  // first, and the key the next upload goes over the server's copy with is
  // handed back.
  {
    Sandbox sandbox(c, "conflicts-keep-local");
    const std::unique_ptr<fs::FileSystem> files =
        rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(SavePath("game.srm"), server);
    const std::string backup =
        std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000000.srm";
    c.Expect(sandbox.Write(backup, previous), "the conflict's backup is on the card");
    conflicts::History history(sandbox.Host(std::string(conflicts::kHistorySdPath)));
    history.Load();
    conflicts::Entry entry = SaveEntry(4, "game.srm");
    entry.backup_sd_path = backup;
    const std::int64_t id = history.Append(entry).id;

    state::Baseline::Key key{0, std::nullopt};
    const conflicts::RestoreReport kept =
        conflicts::KeepLocal(*files, history, id, {}, &key, options);
    c.Expect(kept.ok(), std::string("Keep local restored the local bytes: ") +
                            conflicts::ToString(kept.outcome) + " -- " + kept.message);
    c.ExpectEq(sandbox.Read(SavePath("game.srm")), previous, "the local bytes are back, exactly");
    c.Expect(!kept.backup_sd_path.empty() && sandbox.Read(kept.backup_sd_path) == server,
             "and the server's copy it replaced was backed up first: " + kept.message);
    c.Expect(key == state::Baseline::Key{4, std::string("retroarch-srm")},
             "the key is the conflict's (rom_id, slot)");
    c.Expect(kept.message.find("over the server's copy") != std::string::npos,
             "and the sentence says the next sync puts them over the server's copy: " +
                 kept.message);
  }

  // Not a save conflict: refused, nothing written, no key.
  {
    Sandbox sandbox(c, "conflicts-keep-local-refused");
    const std::unique_ptr<fs::FileSystem> files =
        rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(SavePath("game.srm"), server);
    const std::string backup =
        std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000000.srm";
    c.Expect(sandbox.Write(backup, previous), "a backup is on the card");
    conflicts::History history(sandbox.Host(std::string(conflicts::kHistorySdPath)));
    history.Load();

    conflicts::Entry replaced = SaveEntry(4, "game.srm");
    replaced.event = conflicts::Event::kReplaced;
    replaced.backup_sd_path = backup;
    conflicts::Entry state_entry = SaveEntry(4, "game.state");
    state_entry.kind = conflicts::EntryKind::kState;
    state_entry.event = conflicts::Event::kReplaced;
    state_entry.slot.reset();
    state_entry.backup_sd_path = backup;
    conflicts::Entry slotless = SaveEntry(4, "game.srm");
    slotless.slot.reset();
    slotless.backup_sd_path = backup;
    for (const auto& [what, entry] :
         {std::pair<std::string, conflicts::Entry>{"a download that replaced a save", replaced},
          {"a save state", state_entry},
          {"a conflict with no slot", slotless}}) {
      state::Baseline::Key key{0, std::nullopt};
      const conflicts::RestoreReport refused =
          conflicts::KeepLocal(*files, history, history.Append(entry).id, {}, &key, options);
      c.Expect(refused.outcome == conflicts::RestoreOutcome::kNotAConflict,
               what + " is not a conflict Keep local applies to: " +
                   conflicts::ToString(refused.outcome));
      c.Expect(key.first == 0, what + ": and no key is armed");
    }
    c.ExpectEq(sandbox.Read(SavePath("game.srm")), server, "and nothing was written");

    // A conflict whose backup is gone is the restore's own refusal.
    conflicts::Entry orphan = SaveEntry(4, "game.srm");
    orphan.backup_sd_path = std::string(harness::kBackupDir) + "/4-retroarch-srm-9.srm";
    state::Baseline::Key key{0, std::nullopt};
    const conflicts::RestoreReport missing =
        conflicts::KeepLocal(*files, history, history.Append(orphan).id, {}, &key, options);
    c.Expect(missing.outcome == conflicts::RestoreOutcome::kBackupMissing,
             std::string("a conflict whose backup is gone is named: ") +
                 conflicts::ToString(missing.outcome));
    c.Expect(key.first == 0, "and arms nothing");
  }

  // Armed once per save.
  {
    std::vector<state::Baseline::Key> keep;
    sync::ArmKeepLocal(&keep, {4, std::string("autosave")});
    sync::ArmKeepLocal(&keep, {4, std::string("autosave")});
    c.ExpectEq(keep.size(), std::size_t{1}, "a second Keep local of one save arms it once");
  }

  // #273: Keep local acts on the save's current conflict only. An older entry
  // for the same `(rom_id, slot)` -- a later conflict, a download over it --
  // names bytes the card has since moved on from, and arming the override for
  // them would put a stale copy over a server head nobody chose to replace.
  // Refused before a byte is written; the newest entry still works.
  {
    Sandbox sandbox(c, "conflicts-keep-local-superseded");
    const std::unique_ptr<fs::FileSystem> files =
        rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(SavePath("game.srm"), server);
    const std::string older_backup =
        std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000000.srm";
    const std::string newer_backup =
        std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000600.srm";
    const std::string newer = "this console's bytes at the second conflict\n";
    c.Expect(sandbox.Write(older_backup, previous) && sandbox.Write(newer_backup, newer),
             "both conflicts' backups are on the card");
    conflicts::History history(sandbox.Host(std::string(conflicts::kHistorySdPath)));
    history.Load();
    conflicts::Entry first = SaveEntry(4, "game.srm");
    first.backup_sd_path = older_backup;
    const std::int64_t older = history.Append(first).id;
    // Another rom's save, and the same rom's other slot, are not this save:
    // neither supersedes it.
    conflicts::Entry other_rom = SaveEntry(5, "other.srm");
    other_rom.sd_path = SavePath("other.srm");
    history.Append(other_rom);
    conflicts::Entry other_slot = SaveEntry(4, "game.srm");
    other_slot.slot = "autosave";
    history.Append(other_slot);
    state::Baseline::Key key{0, std::nullopt};
    {
      std::vector<state::Baseline::Key> unused;
      // Asked against a history whose only later entries are other saves', the
      // older conflict is still current -- so what refuses it below is the
      // second conflict and nothing else. Checked without writing: a full list
      // stops it right after the supersession test.
      for (std::int64_t rom = 100; rom < 100 + static_cast<std::int64_t>(sync::kMaxKeepLocal);
           ++rom) {
        sync::ArmKeepLocal(&unused, {rom, std::string("autosave")});
      }
      const conflicts::RestoreReport current =
          conflicts::KeepLocal(*files, history, older, unused, &key, options);
      c.Expect(current.outcome == conflicts::RestoreOutcome::kKeepLocalFull,
               std::string("other saves' later entries do not supersede it: ") +
                   conflicts::ToString(current.outcome));
    }

    conflicts::Entry second = SaveEntry(4, "game.srm");
    second.backup_sd_path = newer_backup;
    const std::int64_t newest = history.Append(second).id;

    const conflicts::RestoreReport refused =
        conflicts::KeepLocal(*files, history, older, {}, &key, options);
    c.Expect(refused.outcome == conflicts::RestoreOutcome::kSuperseded,
             std::string("Keep local on a conflict a newer one replaced is refused: ") +
                 conflicts::ToString(refused.outcome) + " -- " + refused.message);
    c.ExpectEq(std::string(conflicts::ToString(refused.outcome)), std::string("superseded"),
               "and named on the wire as superseded");
    c.Expect(refused.message.find("newer") != std::string::npos,
             "with the reason in its sentence: " + refused.message);
    c.Expect(key.first == 0, "no key is armed");
    c.ExpectEq(sandbox.Read(SavePath("game.srm")), server, "and nothing was written");

    // A later download over the save supersedes it the same way: the card holds
    // a server copy no conflict was ever weighed against.
    conflicts::Entry downloaded = SaveEntry(4, "game.srm");
    downloaded.event = conflicts::Event::kReplaced;
    downloaded.backup_sd_path = newer_backup;
    history.Append(downloaded);
    const conflicts::RestoreReport overtaken =
        conflicts::KeepLocal(*files, history, newest, {}, &key, options);
    c.Expect(overtaken.outcome == conflicts::RestoreOutcome::kSuperseded,
             std::string("a conflict a download has since gone over is superseded too: ") +
                 conflicts::ToString(overtaken.outcome));
    c.Expect(key.first == 0, "and arms nothing");
    c.ExpectEq(sandbox.Read(SavePath("game.srm")), server, "and writes nothing");

    // Under a game, a superseded entry is still refused as superseded: that
    // holds whatever the player does, where the game is theirs to close.
    conflicts::RestoreOptions under_game = options;
    under_game.game_running = true;
    const conflicts::RestoreReport stale =
        conflicts::KeepLocal(*files, history, newest, {}, &key, under_game);
    c.Expect(stale.outcome == conflicts::RestoreOutcome::kSuperseded,
             std::string("superseded comes before a running game: ") +
                 conflicts::ToString(stale.outcome));

    // The newest conflict of a save is still Keep local's to take -- and under a
    // game it is taken but **held** (#275): the game would save over the kept
    // bytes when it closes, so nothing is written now, and the key comes back
    // for the caller to land once the game has closed.
    conflicts::Entry third = SaveEntry(4, "game.srm");
    third.backup_sd_path = newer_backup;
    const std::int64_t third_id = history.Append(third).id;
    const conflicts::RestoreReport running =
        conflicts::KeepLocal(*files, history, third_id, {}, &key, under_game);
    c.Expect(running.outcome == conflicts::RestoreOutcome::kHeld,
             std::string("the newest conflict under a game is held, not refused: ") +
                 conflicts::ToString(running.outcome) + " -- " + running.message);
    c.ExpectEq(std::string(conflicts::ToString(running.outcome)), std::string("held"),
               "and named on the wire as held");
    c.Expect(!running.ok(), "held is not restored: no bytes are on the card yet");
    c.Expect(running.message.find("close") != std::string::npos,
             "and its sentence says when it lands: " + running.message);
    c.Expect(key.first == 4 && key.second == std::optional<std::string>("retroarch-srm"),
             "the key comes back, for the caller to hold");
    c.ExpectEq(sandbox.Read(SavePath("game.srm")), server, "and nothing is written");
    c.Expect(running.backup_sd_path.empty(), "not even a backup: nothing is replaced yet");
    key = {0, std::nullopt};
    const conflicts::RestoreReport kept =
        conflicts::KeepLocal(*files, history, third_id, {}, &key, options);
    c.Expect(kept.ok(), std::string("the save's newest conflict is kept: ") +
                            conflicts::ToString(kept.outcome) + " -- " + kept.message);
    c.ExpectEq(sandbox.Read(SavePath("game.srm")), newer, "with its own bytes");
  }

  // #273: the fifth choice is refused, not the oldest evicted. An evicted choice
  // has already put its bytes on the card, and with its override gone their
  // next upload meets the slot guard and resolves as Keep server -- the
  // player's explicit answer undone without a word. A refusal writes nothing,
  // and the choice can be made again once a sync has spent one.
  {
    Sandbox sandbox(c, "conflicts-keep-local-full");
    const std::unique_ptr<fs::FileSystem> files =
        rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(SavePath("game.srm"), server);
    const std::string backup =
        std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000000.srm";
    c.Expect(sandbox.Write(backup, previous), "the conflict's backup is on the card");
    conflicts::History history(sandbox.Host(std::string(conflicts::kHistorySdPath)));
    history.Load();
    conflicts::Entry entry = SaveEntry(4, "game.srm");
    entry.backup_sd_path = backup;
    const std::int64_t id = history.Append(entry).id;

    std::vector<state::Baseline::Key> armed;
    for (std::int64_t rom = 100; rom < 100 + static_cast<std::int64_t>(sync::kMaxKeepLocal);
         ++rom) {
      c.Expect(sync::ArmKeepLocal(&armed, {rom, std::string("autosave")}),
               "a choice under the bound is armed");
    }
    state::Baseline::Key key{0, std::nullopt};
    const conflicts::RestoreReport full =
        conflicts::KeepLocal(*files, history, id, armed, &key, options);
    c.Expect(full.outcome == conflicts::RestoreOutcome::kKeepLocalFull,
             std::string("a fifth Keep local while four wait is refused: ") +
                 conflicts::ToString(full.outcome) + " -- " + full.message);
    c.ExpectEq(std::string(conflicts::ToString(full.outcome)), std::string("keep_local_full"),
               "named on the wire as keep_local_full");
    c.Expect(full.message.find(std::to_string(sync::kMaxKeepLocal)) != std::string::npos,
             "and says how many are waiting: " + full.message);
    c.Expect(key.first == 0, "no key is armed");
    c.ExpectEq(sandbox.Read(SavePath("game.srm")), server, "and nothing was written");

    // Arming refuses past the bound too, rather than letting the oldest go.
    c.Expect(!sync::ArmKeepLocal(&armed, {4, std::string("retroarch-srm")}),
             "ArmKeepLocal refuses a fifth");
    c.ExpectEq(armed.size(), sync::kMaxKeepLocal, "so at most kMaxKeepLocal are held");
    c.Expect(sync::FindByKey(armed, state::Baseline::Key{100, std::string("autosave")}) !=
                 nullptr,
             "and the oldest is still there");

    // A save already armed takes no new place, so choosing it again is not the
    // fifth.
    armed.back() = {4, std::string("retroarch-srm")};
    const conflicts::RestoreReport again =
        conflicts::KeepLocal(*files, history, id, armed, &key, options);
    c.Expect(again.ok(), std::string("a save already armed may be chosen again at the bound: ") +
                             conflicts::ToString(again.outcome) + " -- " + again.message);
    c.Expect(sync::ArmKeepLocal(&armed, key), "and arming it again is no refusal");
    c.ExpectEq(armed.size(), sync::kMaxKeepLocal, "nor a second place");
  }

  // Spent by the plan that answered it -- an upload that went or was refused,
  // or any other action, which means the server asked for no upload -- and
  // kept by one that never reached the server or never named it.
  {
    const auto key = [](std::int64_t rom) {
      return state::Baseline::Key{rom, std::string("autosave")};
    };
    std::vector<state::Baseline::Key> keep = {key(1), key(2), key(3), key(4), key(5), key(6),
                                              key(7)};
    // Key 7 was armed after the plan went out: the plan carried no override
    // for it, so its upload did not spend it.
    const std::vector<state::Baseline::Key> sent = {key(1), key(2), key(3), key(4), key(5),
                                                    key(6)};
    sync::ExecutionReport executed;
    executed.operations = {
        Ran(1, "autosave", sync::Action::kUpload, sync::OperationOutcome::kUploaded),
        Ran(2, "autosave", sync::Action::kUpload, sync::OperationOutcome::kFailed,
            sync::OperationError::kTransferFailed),
        Ran(3, "autosave", sync::Action::kDownload, sync::OperationOutcome::kDownloaded),
        Ran(5, "autosave", sync::Action::kUpload, sync::OperationOutcome::kCanceled,
            sync::OperationError::kCanceled),
        Ran(6, "autosave", sync::Action::kUpload, sync::OperationOutcome::kFailed,
            sync::OperationError::kRefused),
        Ran(7, "autosave", sync::Action::kUpload, sync::OperationOutcome::kUploaded),
        Ran(8, "autosave", sync::Action::kNoOp, sync::OperationOutcome::kNoOp),
    };
    keep.push_back(key(8));
    std::vector<state::Baseline::Key> sent_too = sent;
    sent_too.push_back(key(8));
    const std::vector<sync::OvertakenKeepLocal> overtaken =
        sync::SpendKeepLocal(&keep, sent_too, executed);
    c.Expect(keep == std::vector<state::Baseline::Key>{key(2), key(4), key(5), key(7)},
             "the upload that went, the refused one, the download and the no-op spend theirs; the "
             "transfer that failed, the cancelled one, the save the plan never named and the "
             "choice armed after it went out keep theirs (" + std::to_string(keep.size()) +
                 " left)");
    // #273: the download spent its choice with nothing sent, and says so --
    // the caller logs it, so the choice is overtaken out loud rather than
    // silently dropped. The uploads, sent or refused, had their override, and
    // the no-op is the server already holding the bytes.
    c.ExpectEq(overtaken.size(), std::size_t{1}, "one choice was overtaken by the plan");
    c.Expect(!overtaken.empty() && overtaken[0].key == key(3) &&
                 overtaken[0].action == sync::Action::kDownload,
             "the one a download answered");
  }

  // An upload that failed before its request was made -- the card could not
  // be read or hashed, or the save was not there -- never carried its
  // override to the server, so it keeps it as a failed transfer does. Spending
  // it would send the next upload with overwrite=false into the slot guard's
  // 409, and the player's choice would resolve as Keep server.
  {
    const auto key = [](std::int64_t rom) {
      return state::Baseline::Key{rom, std::string("autosave")};
    };
    std::vector<state::Baseline::Key> keep = {key(1), key(2)};
    const std::vector<state::Baseline::Key> sent = keep;
    sync::ExecutionReport executed;
    executed.operations = {
        Ran(1, "autosave", sync::Action::kUpload, sync::OperationOutcome::kFailed,
            sync::OperationError::kUnreadableCard),
        Ran(2, "autosave", sync::Action::kUpload, sync::OperationOutcome::kFailed,
            sync::OperationError::kNoLocalSave),
    };
    sync::SpendKeepLocal(&keep, sent, executed);
    c.Expect(keep == sent,
             "an upload the card could not read, or had no save for, keeps its Keep local (" +
                 std::to_string(keep.size()) + " left)");
  }
}

// --- interrupted --------------------------------------------------------------
//
// A restore killed mid-write. The guarantee is that whatever is at the save path
// is the old bytes or the new ones -- never a splice -- and that the `.tmp` an
// interrupted copy leaves is swept rather than committed.
//
// The kill is `RLIMIT_FSIZE` rather than a timer, for `test_token_store.cpp`'s
// reason: a child that may write only a few KiB, asked to copy far more than
// that, is killed by `SIGXFSZ` *inside* the write, at an offset the kernel
// picks -- no sleeping, no racing, and no cleanup on the way out. A timing-based
// kill would pass on a fast machine by never landing in the window.

void Interrupted(checks::Checks& c) {
#ifdef ROMMSYNC_CAN_FORK
  Sandbox sandbox(c, "conflicts-interrupted");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  const std::string server = "the server's copy, which must survive a killed restore\n";
  // Bigger than the limit below and bigger than `CopyAtomically`'s 4 KiB chunk,
  // so the kill lands part way through a multi-chunk copy rather than before the
  // first write.
  std::string previous;
  previous.reserve(64 * 1024);
  for (int index = 0; index < 64 * 1024; ++index) {
    previous += static_cast<char>('a' + (index % 26));
  }

  sandbox.SeedSave(SavePath("game.srm"), server);
  const std::string backup = std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000000.srm";
  c.Expect(sandbox.Write(backup, previous), "a large backup is on the card");

  conflicts::History history(sandbox.Host(std::string(conflicts::kHistorySdPath)));
  history.Load();
  conflicts::Entry entry = SaveEntry(4, "game.srm");
  entry.backup_sd_path = backup;
  const std::int64_t id = history.Append(entry).id;

  conflicts::RestoreOptions options;
  options.backup_dir = harness::kBackupDir;
  options.now = []() { return Moment(kWhen + 500); };

  std::fflush(nullptr);  // nothing of ours may be flushed twice by the child
  const pid_t child = fork();
  if (child < 0) {
    c.Expect(false, "the test can fork");
    return;
  }
  if (child == 0) {
    // No core dump: the kill is the point, and a crash dump per run is noise in
    // CI and megabytes on a laptop.
    const rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    const rlimit limit{8192, 8192};
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
      _exit(2);
    }
    conflicts::Restore(*files, history, id, options);
    _exit(0);  // reached only if the kill did not happen; the parent checks
  }

  int status = 0;
  c.Expect(waitpid(child, &status, 0) == child, "the child is reaped");
  c.Expect(WIFSIGNALED(status) != 0,
           "the child was killed by a signal rather than returning -- otherwise this test is "
           "checking a completed restore");

  // The guarantee. Whatever is at the save path, it is one of the two whole
  // files and never a mixture of them.
  const std::string after = sandbox.Read(SavePath("game.srm"));
  c.Expect(after == server || after == previous,
           "an interrupted restore leaves the old bytes or the new ones, never a splice");
  // The kill landed inside the copy of the *backup*, which is written before the
  // save is touched, so in practice the save is untouched. Asserted separately
  // because it is the stronger half and the one hard rule 2 is about.
  c.ExpectEq(after, server, "...and here it is the old ones: nothing was committed");

  // The debris a killed copy leaves is a truncated `.tmp` beside the save. It
  // must never be committed, and the sweep every tick already runs is what
  // removes it (`sync::RecoverStaging`).
  const sync::RecoveryReport swept =
      sync::RecoverStaging(*files, {std::string(harness::kSavesDir), harness::kBackupDir});
  c.Expect(swept.saves_restored == 0, "no save had to be recovered from a .old");
  c.Expect(!std::filesystem::exists(sandbox.Host(SavePath("game.srm")) + ".tmp"),
           "and no .tmp is left beside the save");
  c.ExpectEq(sandbox.Read(SavePath("game.srm")), server, "the save is still the server's copy");

  // ...and the next restore goes through anyway, which is what says the debris
  // cost a tick rather than the file.
  const conflicts::RestoreReport again = conflicts::Restore(*files, history, id, options);
  c.Expect(again.ok(), "a restore after an interrupted one works: " + again.message);
  c.ExpectEq(sandbox.Read(SavePath("game.srm")), previous, "and puts the bytes back whole");
  c.Expect(!std::filesystem::exists(sandbox.Host(SavePath("game.srm")) + ".tmp"),
           "leaving no .tmp behind");
#else
  c.Expect(false, "this scenario needs fork(); the host rig is POSIX (docs/TESTING.md)");
#endif
}

// --- crash --------------------------------------------------------------------
//
// M9-14 (#203): `restore`, with a power cut at every call it makes. A restore is
// the overwrite a *person* starts, through `CopyAtomically` rather than a
// download, and is held to the same replay: old or new, and never new without
// the old bytes under `.backup/`. Then again with the card refusing to confirm
// `.backup/` itself, which must stop the restore before it writes.

void Crash(checks::Checks& c) {
  const std::string previous = "the bytes this console had, and wants back\n";
  const std::string server = "the server's copy, which a power cut must not cost\n";
  const std::string backup = std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000000.srm";
  const crash::Overwrite expected{SavePath("game.srm"), server, previous, harness::kBackupDir};

  conflicts::RestoreOptions options;
  options.backup_dir = harness::kBackupDir;
  options.now = []() { return Moment(kWhen + 500); };

  for (const bool confirmed : {true, false}) {
    Sandbox sandbox(c, confirmed ? "conflicts-crash" : "conflicts-crash-unconfirmed");
    const std::unique_ptr<fs::FileSystem> files =
        rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(SavePath("game.srm"), server);
    sandbox.Write(backup, previous);
    conflicts::History history(sandbox.Host(std::string(conflicts::kHistorySdPath)));
    history.Load();
    conflicts::Entry entry = SaveEntry(4, "game.srm");
    entry.backup_sd_path = backup;
    const std::int64_t id = history.Append(entry).id;

    std::set<std::string> refused;
    if (!confirmed) refused.insert(harness::kBackupDir);
    crash::Recorder recorder(sandbox.root(), refused);
    const conflicts::RestoreReport done = conflicts::Restore(*files, history, id, options);
    if (confirmed) {
      c.Expect(done.ok(), std::string("the restore ran: ") + conflicts::ToString(done.outcome) +
                              " -- " + done.message);
    } else {
      c.Expect(recorder.refused() > 0, "the card refused to confirm the backup directory");
      c.Expect(done.outcome == conflicts::RestoreOutcome::kBackupFailed,
               std::string("a restore whose backup the card would not confirm does not run: ") +
                   conflicts::ToString(done.outcome) + " -- " + done.message);
      c.ExpectEq(sandbox.Read(SavePath("game.srm")), server, "and the save is untouched");
    }
    crash::ExpectSurvives(c, recorder, expected, rig::ScratchDir() + "/crash-restore",
                          confirmed ? "a restore" : "a restore whose backup was not confirmed");
  }
}

// --- service ------------------------------------------------------------------
//
// Both commands through `ipc::Dispatch` over a real `SdEngine`, which is the
// acceptance criterion: nothing here needs a console (hard rule 1), and what is
// asserted is a response on the wire rather than a return value.

/// One command, through the table the console runs.
ipc::Error Call(ipc::ServiceCore& core, ipc::Command command, const std::string& request,
                std::string* response) {
  return ipc::Dispatch(core, static_cast<std::uint32_t>(command), request, response);
}

void Service(checks::Checks& c) {
  Sandbox sandbox(c, "conflicts-service");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string directory = sandbox.Host(harness::kConfigDir) + "/";

  const std::string previous = "the bytes this console had\n";
  const std::string server = "the server's copy\n";
  sandbox.SeedSave(SavePath("game.srm"), server);
  const std::string backup = std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000000.srm";
  c.Expect(sandbox.Write(backup, previous), "the backup is on the card");

  // Written before the engine boots, so what the service answers with is a file
  // an *earlier* run left -- which is the whole point of persisting it.
  {
    conflicts::History history(directory + conflicts::kHistoryFileName);
    history.Load();
    conflicts::Entry entry = SaveEntry(4, "game.srm");
    entry.backup_sd_path = backup;
    c.Expect(history.Append(entry).ok(), "an entry is on the card before the engine starts");
    conflicts::Entry orphan = SaveEntry(5, "orphan.srm");
    orphan.backup_sd_path = std::string(harness::kBackupDir) + "/5-retroarch-srm-1.srm";
    c.Expect(history.Append(orphan).ok(), "...and one whose backup is not there");
  }

  sysmodule::SdEngine engine;
  engine.Load(directory);
  engine.UseCard(files.get());
  ipc::ServiceCore core(engine);

  std::string response;
  ipc::ConflictQuery query;
  query.offset = 0;
  query.limit = ipc::kMaxConflictPage;
  c.Expect(Call(core, ipc::Command::kListConflicts, ipc::EncodeConflictQuery(query), &response) ==
               ipc::Error::kOk,
           "ListConflicts is answered");
  const ipc::Decoded<ipc::ConflictPage> page = ipc::DecodeConflictPage(response);
  c.Expect(page.ok(), "and the page decodes: " + page.error.Describe());
  c.ExpectEq(page.value.entries.size(), std::size_t{2}, "both entries are on the page");
  c.ExpectEq(page.value.total, 2, "and the total says how many there are altogether");
  c.Expect(!page.value.has_more, "with nothing after it");
  // Newest first: the orphan was appended second.
  c.Expect(!page.value.entries[0].backup_present,
           "an entry whose backup is gone is marked before a press, not at write time");
  c.Expect(page.value.entries[1].backup_present, "and one whose backup is there is not");
  c.ExpectEq(page.value.entries[1].entry.reason,
             std::string("Both sides changed since last sync"),
             "the server's sentence crossed the wire whole");

  // An offset past the end is an empty page rather than a refusal: a history
  // that shrank under an open screen is normal.
  query.offset = 2;
  c.Expect(Call(core, ipc::Command::kListConflicts, ipc::EncodeConflictQuery(query), &response) ==
               ipc::Error::kOk,
           "an offset past the end is still answered");
  c.Expect(ipc::DecodeConflictPage(response).value.entries.empty(), "...with an empty page");

  // The restore, through the wire. This engine has no worker, so it runs on the
  // caller and the answer is already `kDone` -- the shape a console whose
  // worker never started gets, and the one a worker's poll ends in (#209).
  const std::int64_t entry_id = page.value.entries[1].entry.id;
  c.Expect(Call(core, ipc::Command::kRestoreBackup, ipc::EncodeEntryId(entry_id), &response) ==
               ipc::Error::kOk,
           "RestoreBackup never fails at the transport -- the outcome is in the answer");
  const ipc::Decoded<ipc::RestoreState> state = ipc::DecodeRestoreState(response);
  c.Expect(state.ok(), "the state decodes: " + state.error.Describe());
  c.Expect(state.value.phase == ipc::RestorePhase::kDone && state.value.entry_id == entry_id,
           std::string("with no worker, the restore is done when it answers: ") +
               ipc::ToString(state.value.phase));
  const conflicts::RestoreReport& report = state.value.report;
  c.Expect(report.outcome == conflicts::RestoreOutcome::kRestored,
           std::string("and says it restored: ") + conflicts::ToString(report.outcome) + " -- " +
               report.message);
  c.ExpectEq(sandbox.Read(SavePath("game.srm")), previous, "the bytes are back on the card");
  c.Expect(!report.backup_sd_path.empty() && sandbox.Read(report.backup_sd_path) == server,
           "and the server's copy was backed up first: " + report.message);
  c.Expect(Call(core, ipc::Command::kGetRestoreState, ipc::EncodeEmpty(), &response) ==
                   ipc::Error::kOk &&
               ipc::DecodeRestoreState(response).value.report.outcome ==
                   conflicts::RestoreOutcome::kRestored,
           "and the poll answers the same outcome afterwards");

  // An entry whose backup is gone refuses, and the refusal is an outcome rather
  // than a failing call -- so a screen has a sentence for it.
  const std::int64_t orphan_id = page.value.entries[0].entry.id;
  c.Expect(Call(core, ipc::Command::kRestoreBackup, ipc::EncodeEntryId(orphan_id), &response) ==
               ipc::Error::kOk,
           "a refused restore is still a successful call");
  c.Expect(ipc::DecodeRestoreState(response).value.report.outcome ==
               conflicts::RestoreOutcome::kBackupMissing,
           "with the reason in the answer");

  // A `conflict_show` of false loses neither an entry nor a backup: the setting
  // hides the *screen*, and the sysmodule must not lie about what it recorded.
  {
    ipc::ConfigEdit edit;
    edit.assignments.push_back({"sync", "conflict_show", "false", /*remove=*/false});
    c.Expect(Call(core, ipc::Command::kSetConfig, ipc::EncodeConfigEdit(edit), &response) ==
                 ipc::Error::kOk,
             "conflict_show is turned off");
    c.Expect(ipc::DecodeConfigResult(response).value.outcome == ipc::WriteOutcome::kApplied,
             "and the write took");
    query.offset = 0;
    c.Expect(Call(core, ipc::Command::kListConflicts, ipc::EncodeConflictQuery(query),
                  &response) == ipc::Error::kOk,
             "the history is still served");
    c.ExpectEq(ipc::DecodeConflictPage(response).value.entries.size(), std::size_t{2},
               "...with every entry still on it");
    c.Expect(sandbox.Exists(backup), "and every backup still on the card");
  }

  // **A build with no `fs::FileSystem` still looks.** `SdEngine` opens `sdmc:`
  // paths directly for every other record it holds, so answering
  // `backup_present` blind would draw every entry as restorable and fail at the
  // press -- the outcome #36 exists to replace. Here the sandbox is not under
  // `sdmc:`, so an engine with no card reports every backup as missing, which is
  // the honest answer for a card it cannot reach.
  {
    sysmodule::SdEngine cardless;
    cardless.Load(directory);
    ipc::ServiceCore blind(cardless);
    query.offset = 0;
    c.Expect(Call(blind, ipc::Command::kListConflicts, ipc::EncodeConflictQuery(query),
                  &response) == ipc::Error::kOk,
             "an engine with no card still serves the history");
    const ipc::ConflictPage seen = ipc::DecodeConflictPage(response).value;
    c.ExpectEq(seen.entries.size(), std::size_t{2}, "with every entry on it");
    for (const ipc::ConflictRow& row : seen.entries) {
      c.Expect(!row.backup_present,
               "and never claims a backup is there when it could not find it");
    }
  }

  // A reboot: the entries and the ids survive it.
  {
    sysmodule::SdEngine rebooted;
    rebooted.Load(directory);
    rebooted.UseCard(files.get());
    ipc::ServiceCore after(rebooted);
    query.offset = 0;
    c.Expect(Call(after, ipc::Command::kListConflicts, ipc::EncodeConflictQuery(query),
                  &response) == ipc::Error::kOk,
             "a fresh engine answers from the card");
    const ipc::ConflictPage reread = ipc::DecodeConflictPage(response).value;
    c.ExpectEq(reread.entries.size(), std::size_t{2}, "with both entries");
    c.ExpectEq(reread.entries[1].entry.id, entry_id, "and the same ids");
  }
}

// --- overlay ------------------------------------------------------------------
//
// The screen's decisions, and the three rules overlay/AGENTS.md holds every
// screen to. Only a grep can say the last three.

ipc::ConflictRow Row(conflicts::Entry entry, bool backup_present) {
  ipc::ConflictRow row;
  row.entry = std::move(entry);
  row.entry.id = row.entry.id == 0 ? 1 : row.entry.id;
  row.backup_present = backup_present;
  return row;
}

/// A save a download replaced, which is what a plain restore is for. A save
/// *conflict*'s confirmation is the Keep server / Keep local / Skip choice
/// instead (#249), so the restore's own scenarios use one of these.
conflicts::Entry ReplacedEntry(std::int64_t rom_id, std::string file_name) {
  conflicts::Entry entry = SaveEntry(rom_id, std::move(file_name));
  entry.event = conflicts::Event::kReplaced;
  entry.reason.clear();
  return entry;
}

/// A model with one restorable entry loaded, opened, confirmed and pressed --
/// so its next `Next()` is the restore.
overlay::ConflictsModel OneRestorable(std::int64_t entry_id) {
  overlay::ConflictsModel model;
  model.Next();
  ipc::ConflictPage page;
  page.offset = 0;
  page.total = 1;
  conflicts::Entry entry = ReplacedEntry(4, "polled.srm");
  entry.id = entry_id;
  page.entries = {Row(entry, true)};
  model.OnPage(page);
  model.Activate();  // open
  model.Activate();  // confirm
  model.Activate();  // press
  return model;
}

void Overlay(checks::Checks& c) {
  // A screen with nothing to show says so. A blank one is indistinguishable
  // from an overlay that failed to read something (overlay/AGENTS.md).
  {
    overlay::ConflictsModel model;
    const overlay::ConflictsModel::Command first = model.Next();
    c.Expect(first.kind == overlay::ConflictsModel::Command::Kind::kListConflicts,
             "the first frame asks for the first page");
    c.ExpectEq(first.query.offset, 0, "from the start");
    ipc::ConflictPage empty;
    empty.offset = 0;
    model.OnPage(empty);
    const overlay::ConflictsView view = model.Render();
    c.Expect(!view.headline.empty(), "a console that has overwritten nothing still has a headline");
    c.Expect(view.headline.find("Nothing has been overwritten") != std::string::npos,
             "...and it is a fact rather than an error: " + view.headline);
    c.ExpectEq(view.selected, -1, "with no selection, because there are no rows");
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "and nothing more to ask for");
  }

  // A page, a detail, a confirmation, and a restore.
  {
    overlay::ConflictsModel model;
    model.Next();
    ipc::ConflictPage page;
    page.offset = 0;
    page.total = 3;
    conflicts::Entry ready = ReplacedEntry(4, "game.srm");
    ready.id = 3;
    conflicts::Entry gone = SaveEntry(5, "gone.srm");
    gone.id = 2;
    conflicts::Entry kept = SaveEntry(9, "nine.state");
    kept.id = 1;
    kept.kind = conflicts::EntryKind::kState;
    kept.event = conflicts::Event::kKeptBoth;
    kept.slot.reset();
    kept.reason.clear();
    kept.server_content_hash.reset();
    kept.server_size_bytes = 4096;
    kept.backup_sd_path.clear();
    page.entries = {Row(ready, true), Row(gone, false), Row(kept, false)};
    model.OnPage(page);

    overlay::ConflictsView view = model.Render();
    c.ExpectEq(view.rows.size(), std::size_t{3}, "every entry is drawn");
    c.Expect(view.rows[0].restorable == overlay::Restorability::kReady, "the first can be restored");
    c.Expect(view.rows[1].restorable == overlay::Restorability::kBackupGone,
             "the second's backup is gone");
    c.Expect(view.rows[2].restorable == overlay::Restorability::kNothingToRestore,
             "and the keep-both replaced nothing");
    c.Expect(view.rows[2].note.find("Nothing was overwritten") != std::string::npos,
             "which is the sentence for a keep-both, and only for one: " + view.rows[2].note);
    c.Expect(view.rows[1].note.find("no longer on the card") != std::string::npos,
             "a conflict whose backup is gone says the backup is gone, not that the save was "
             "left alone: " + view.rows[1].note);
    for (const overlay::ConflictListRow& row : view.rows) {
      c.Expect(!row.label.empty(), "every row has a label, in every state");
      if (row.restorable != overlay::Restorability::kReady) {
        c.Expect(!row.note.empty(), "and a row that will not restore always says why");
      }
    }
    c.Expect(!view.can_restore, "the list itself restores nothing");

    // An overwrite that recorded no backup path at all -- a `BackUpFirst` that
    // failed after the write would produce one. It must not read as "nothing
    // was overwritten", which would tell a user their save is intact when it is
    // the one that was replaced.
    {
      overlay::ConflictsModel unbacked;
      unbacked.Next();
      ipc::ConflictPage orphaned;
      orphaned.offset = 0;
      orphaned.total = 1;
      conflicts::Entry lost = SaveEntry(4, "lost.srm");
      lost.id = 11;
      lost.backup_sd_path.clear();
      orphaned.entries = {Row(lost, false)};
      unbacked.OnPage(orphaned);
      const overlay::ConflictsView drawn = unbacked.Render();
      c.Expect(drawn.rows[0].restorable == overlay::Restorability::kBackupGone,
               "a conflict with no backup path recorded is unrestorable, not untouched");
      c.Expect(drawn.rows[0].note.find("Nothing was overwritten") == std::string::npos,
               "and never says the save was left alone: " + drawn.rows[0].note);
    }

    // #240: a conflict whose losing copy could not be put on the server says so
    // -- on the row, before a press, and in the detail -- and one that is there
    // names the row it is in.
    {
      overlay::ConflictsModel archive;
      archive.Next();
      ipc::ConflictPage two;
      two.offset = 0;
      two.total = 2;
      conflicts::Entry card_only = SaveEntry(4, "card.srm");
      card_only.id = 22;
      card_only.local_copy = conflicts::LocalCopy::kCardOnly;
      conflicts::Entry on_server = SaveEntry(5, "server.srm");
      on_server.id = 21;
      on_server.local_copy = conflicts::LocalCopy::kOnServer;
      on_server.archive_save_id = 77;
      two.entries = {Row(card_only, true), Row(on_server, true)};
      archive.OnPage(two);
      overlay::ConflictsView drawn = archive.Render();
      c.Expect(drawn.rows[0].note.find("server holds no copy") != std::string::npos,
               "the row of a card-only copy says the server holds none: " + drawn.rows[0].note);
      c.Expect(drawn.rows[0].tone == overlay::Tone::kWarn, "as a warning");
      c.Expect(drawn.rows[1].note.find("server holds no copy") == std::string::npos,
               "and a copy that is on the server is not said to be missing: " +
                   drawn.rows[1].note);

      const auto detail_value = [](const overlay::ConflictsView& view, const std::string& label) {
        for (const overlay::ConflictDetailLine& line : view.detail) {
          if (line.label == label) {
            return line.value;
          }
        }
        return std::string();
      };
      archive.Activate();
      drawn = archive.Render();
      const std::string card_line = detail_value(drawn, "On the server");
      c.Expect(card_line.find("no copy") != std::string::npos,
               "the detail says the server holds no copy: " + card_line);
      archive.Back();
      archive.MoveSelection(1);
      archive.Activate();
      drawn = archive.Render();
      const std::string server_line = detail_value(drawn, "On the server");
      c.Expect(server_line.find("77") != std::string::npos,
               "the detail names the archival row: " + server_line);
    }

    // A press on an entry whose backup is gone opens it and stops there.
    model.MoveSelection(1);
    model.Activate();
    view = model.Render();
    c.Expect(view.mode == overlay::ConflictsMode::kDetail, "A opens the entry");
    c.Expect(!view.can_restore, "which cannot be restored, so A offers nothing");
    model.Activate();
    c.Expect(model.Render().mode == overlay::ConflictsMode::kDetail,
             "and a second press does not open a confirmation for it");
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "nothing is sent for a press the screen already knows would fail");
    c.Expect(model.Back(), "B leaves the detail");

    // The state's detail must not claim a comparison it did not make.
    model.MoveSelection(1);
    model.Activate();
    view = model.Render();
    bool says_no_digest = false;
    for (const overlay::ConflictDetailLine& line : view.detail) {
      c.Expect(!line.label.empty() && !line.value.empty(),
               "every detail line carries text on both sides");
      if (line.value.find("computes no digest") != std::string::npos) {
        says_no_digest = true;
      }
      c.Expect(line.value != crypto::Md5Hex("server v2\n"),
               "a state entry never shows a server digest");
    }
    c.Expect(says_no_digest,
             "a state's detail says out loud that the two were never compared by content");
    c.Expect(model.Back(), "B leaves it");

    // The one that can be restored: detail, confirmation, then the command.
    model.MoveSelection(-2);
    model.Activate();
    view = model.Render();
    c.Expect(view.mode == overlay::ConflictsMode::kDetail, "A opens it");
    c.Expect(view.can_restore, "and this one can be restored");
    model.Activate();
    view = model.Render();
    c.Expect(view.mode == overlay::ConflictsMode::kConfirm, "a second A asks for confirmation");
    c.Expect(view.hint.find(overlay::RestoreMeaning()) != std::string::npos,
             "which says the server still decides -- hard rule 3: " + view.hint);
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "and confirming has not been asked for yet, so nothing is sent");

    model.Activate();
    const overlay::ConflictsModel::Command restore = model.Next();
    c.Expect(restore.kind == overlay::ConflictsModel::Command::Kind::kRestore,
             "the third A sends the restore");
    c.ExpectEq(restore.entry_id, std::int64_t{3}, "for the entry that was open");

    conflicts::RestoreReport report;
    report.outcome = conflicts::RestoreOutcome::kRestored;
    report.backup_sd_path = "/config/rommsync/.backup/4-retroarch-srm-1757000500.srm";
    model.OnRestoreState(Done(3, report));
    view = model.Render();
    c.Expect(view.mode == overlay::ConflictsMode::kDetail,
             "the screen comes back to the entry rather than staying on the confirmation");
    c.Expect(view.headline.find("Restored") != std::string::npos,
             "and says what happened: " + view.headline);
    c.Expect(!view.can_restore, "with the restore no longer on offer");
    // Nothing is re-read: a restore rewrites no entry and deletes no backup, so
    // every loaded row is as true as it was (`OnRestoreState`).
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "and nothing needs re-reading, because a restore changed no entry");
  }

  // #249: a save conflict's confirmation is a choice -- Keep server, which
  // is what the sync already did and the default; Keep local, which is the one
  // command here that ends in `overwrite=true`; and Skip, which decides nothing.
  {
    const auto conflict_model = [](std::int64_t entry_id) {
      overlay::ConflictsModel model;
      model.Next();
      ipc::ConflictPage page;
      page.offset = 0;
      page.total = 1;
      conflicts::Entry entry = SaveEntry(4, "chosen.srm");
      entry.id = entry_id;
      page.entries = {Row(entry, true)};
      model.OnPage(page);
      return model;
    };

    overlay::ConflictsModel model = conflict_model(61);
    model.Activate();
    overlay::ConflictsView view = model.Render();
    c.Expect(view.mode == overlay::ConflictsMode::kDetail && view.keep_choice,
             "a save conflict's detail offers the choice rather than a plain restore");
    c.Expect(view.can_restore, "while its backup is there to keep");
    model.Activate();
    view = model.Render();
    c.Expect(view.mode == overlay::ConflictsMode::kConfirm, "A opens the choice");
    std::vector<overlay::ConflictChoice> offered;
    for (const overlay::ConflictChoiceRow& row : view.choices) {
      offered.push_back(row.choice);
      c.Expect(!row.label.empty() && !row.meaning.empty(), "every choice has a label and a meaning");
    }
    c.Expect(offered == std::vector<overlay::ConflictChoice>{overlay::ConflictChoice::kKeepServer,
                                                             overlay::ConflictChoice::kKeepLocal,
                                                             overlay::ConflictChoice::kSkip},
             "the three choices, in that order");
    c.ExpectEq(view.choice, 0, "with Keep server -- what the sync already did -- selected");
    c.Expect(view.hint.find(overlay::ChoiceMeaning(overlay::ConflictChoice::kKeepServer)) !=
                 std::string::npos,
             "and the hint says what the selected one means: " + view.hint);
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "nothing is sent for opening it");

    // Keep server needs no action: nothing is sent, and the screen says so.
    model.Activate();
    view = model.Render();
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "Keep server sends nothing -- the server's copy is already on the card");
    c.Expect(view.mode == overlay::ConflictsMode::kDetail &&
                 view.headline.find("server's copy") != std::string::npos,
             "and says the server's copy stays: " + view.headline);

    // Keep local is the one that is sent.
    model.Activate();
    model.MoveSelection(1);
    view = model.Render();
    c.ExpectEq(view.choice, 1, "Down moves to Keep local");
    c.Expect(view.hint.find(overlay::ChoiceMeaning(overlay::ConflictChoice::kKeepLocal)) !=
                 std::string::npos,
             "whose meaning is on the screen before the press: " + view.hint);
    c.Expect(std::string(overlay::ChoiceMeaning(overlay::ConflictChoice::kKeepLocal))
                     .find("over the server's copy") != std::string::npos,
             "and says it goes over the server's copy -- the override, named");
    model.MoveSelection(5);
    c.ExpectEq(model.Render().choice, 2, "the choice is clamped at the last");
    model.MoveSelection(-1);
    model.Activate();
    const overlay::ConflictsModel::Command keep = model.Next();
    c.Expect(keep.kind == overlay::ConflictsModel::Command::Kind::kKeepLocal,
             "Keep local sends KeepLocal, not a plain restore");
    c.ExpectEq(keep.entry_id, std::int64_t{61}, "for the entry that was open");
    model.OnRestoreState(Running(61));
    view = model.Render();
    c.Expect(view.headline.find("Keeping") != std::string::npos,
             "a taken Keep local reads as one in progress: " + view.headline);
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kRestoreState,
             "and is polled like a restore");
    conflicts::RestoreReport report;
    report.outcome = conflicts::RestoreOutcome::kRestored;
    model.OnRestoreState(Done(61, report));
    view = model.Render();
    c.Expect(view.mode == overlay::ConflictsMode::kDetail &&
                 view.headline.find("next sync") != std::string::npos,
             "the outcome says the next sync puts it on the server: " + view.headline);
    c.Expect(view.tone == overlay::Tone::kGood, "as a success");

    // A refusal names itself.
    {
      overlay::ConflictsModel refused = conflict_model(62);
      refused.Activate();
      refused.Activate();
      refused.MoveSelection(1);
      refused.Activate();
      refused.Next();
      conflicts::RestoreReport not_one;
      not_one.outcome = conflicts::RestoreOutcome::kNotAConflict;
      refused.OnRestoreState(Done(62, not_one));
      const overlay::ConflictsView drawn = refused.Render();
      c.Expect(drawn.tone == overlay::Tone::kBad &&
                   drawn.headline.find("only for a save conflict") != std::string::npos,
               "a Keep local the sysmodule refused says why: " + drawn.headline);
    }

    // #275: Keep local under a game is held. The screen says it is chosen and
    // when it lands -- not a refusal, and not done -- and the row reads as
    // chosen once the player is back on the list.
    {
      overlay::ConflictsModel held = conflict_model(64);
      held.Activate();
      held.Activate();
      held.MoveSelection(1);
      held.Activate();
      held.Next();
      conflicts::RestoreReport waiting;
      waiting.outcome = conflicts::RestoreOutcome::kHeld;
      waiting.message = "game.srm: Keep local is chosen, and lands when the game closes";
      held.OnRestoreState(Done(64, waiting));
      overlay::ConflictsView drawn = held.Render();
      c.Expect(drawn.mode == overlay::ConflictsMode::kDetail &&
                   drawn.headline.find("close the game") != std::string::npos,
               "a held Keep local says it lands when the game closes: " + drawn.headline);
      c.Expect(drawn.tone == overlay::Tone::kGood, "as a choice made, not a refusal");
      c.Expect(!drawn.can_restore, "and offers no second press");
      c.Expect(held.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
               "nor polls: it is answered");
      held.Back();
      drawn = held.Render();
      c.Expect(!drawn.rows.empty() &&
                   drawn.rows[0].note.find("Keep local") != std::string::npos &&
                   drawn.rows[0].note.find("close the game") != std::string::npos,
               "the row is drawn as chosen, held for the game: " +
                   (drawn.rows.empty() ? std::string() : drawn.rows[0].note));
      held.Activate();
      drawn = held.Render();
      c.Expect(drawn.headline.find("Keep local") != std::string::npos && !drawn.can_restore,
               "and its detail says so rather than offering the choice again: " +
                   drawn.headline);
      held.Activate();
      c.Expect(held.Render().mode == overlay::ConflictsMode::kDetail,
               "A on a chosen entry opens no choice");
    }

    // A page from the sysmodule says which rows are chosen, held or waiting,
    // and each reads as chosen, not as open.
    for (const auto& [state, says] :
         {std::pair{ipc::KeepLocalState::kHeld, "close the game"},
          std::pair{ipc::KeepLocalState::kWaiting, "next sync"}}) {
      overlay::ConflictsModel model;
      model.Next();
      ipc::ConflictPage page;
      page.total = 1;
      conflicts::Entry entry = SaveEntry(4, "game.srm");
      entry.id = 65;
      ipc::ConflictRow row = Row(entry, true);
      row.keep_local = state;
      page.entries = {row};
      model.OnPage(page);
      const overlay::ConflictsView list = model.Render();
      c.Expect(!list.rows.empty() && list.rows[0].note.find(says) != std::string::npos &&
                   list.rows[0].tone == overlay::Tone::kGood,
               std::string("a ") + ipc::ToString(state) + " row is drawn as chosen: " +
                   (list.rows.empty() ? std::string() : list.rows[0].note));
      model.Activate();
      const overlay::ConflictsView detail = model.Render();
      c.Expect(!detail.can_restore && detail.headline.find(says) != std::string::npos,
               std::string("and its detail says what is left to happen: ") + detail.headline);
    }

    // #273's refusals, each in its own words and none a success.
    for (const auto& [outcome, says] :
         {std::pair{conflicts::RestoreOutcome::kSuperseded, "newer"},
          std::pair{conflicts::RestoreOutcome::kKeepLocalFull, "waiting for a sync"}}) {
      conflicts::RestoreReport report;
      report.outcome = outcome;
      const std::string text = overlay::RestoreOutcomeText(report);
      c.Expect(text.find("Nothing was written") != std::string::npos &&
                   text.find(says) != std::string::npos,
               std::string(conflicts::ToString(outcome)) + " says why, and that nothing was "
                                                           "written: " + text);
      c.Expect(overlay::RestoreOutcomeTone(outcome) == overlay::Tone::kBad,
               std::string(conflicts::ToString(outcome)) + " is drawn as a refusal");
    }

    // Slow is not gone, for Keep local as for a restore: asked about, never
    // sent twice.
    {
      overlay::ConflictsModel slow = conflict_model(63);
      slow.Activate();
      slow.Activate();
      slow.MoveSelection(1);
      slow.Activate();
      c.Expect(slow.Next().kind == overlay::ConflictsModel::Command::Kind::kKeepLocal,
               "the Keep local goes out");
      slow.OnUnreachable(overlay::Link::kNotResponding);
      slow.OnLinkRestored();
      c.Expect(slow.Next().kind == overlay::ConflictsModel::Command::Kind::kRestoreState,
               "and one that ran out of budget is asked about, not pressed again");
    }

    // Skip decides nothing: back to the list, nothing sent.
    {
      overlay::ConflictsModel skip = conflict_model(64);
      skip.Activate();
      skip.Activate();
      skip.MoveSelection(2);
      skip.Activate();
      c.Expect(skip.Render().mode == overlay::ConflictsMode::kList,
               "Skip goes back to the list");
      c.Expect(skip.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
               "and sends nothing");
    }

    // B on the choice is a cancel, and sends nothing either.
    {
      overlay::ConflictsModel back = conflict_model(65);
      back.Activate();
      back.Activate();
      back.MoveSelection(1);
      c.Expect(back.Back(), "B leaves the choice");
      c.Expect(back.Render().mode == overlay::ConflictsMode::kDetail, "for the detail");
      c.Expect(back.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
               "without sending the choice it was on");
    }

    // A replaced save is not a conflict: its confirmation is the plain restore.
    {
      overlay::ConflictsModel plain = OneRestorable(66);
      c.Expect(plain.Render().choices.empty() && !plain.Render().keep_choice,
               "a save a download replaced offers a restore, not the choice");
    }
  }

  // A refused page is a page that failed, and says so with a way out.
  {
    overlay::ConflictsModel model;
    model.Next();
    model.OnRefused(ipc::Error::kInternal);
    const overlay::ConflictsView view = model.Render();
    c.Expect(!view.headline.empty() && !view.hint.empty(),
             "a failed page has both a headline and something to do about it");
    c.Expect(view.tone == overlay::Tone::kBad, "and reads as a failure");
    c.Expect(view.hint.find("press A") != std::string::npos,
             "on an empty list A is the retry: " + view.hint);
    model.Activate();
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kListConflicts,
             "and A asks again");
  }

  // A page that failed **part way down a loaded list** must not make the rest of
  // the history unreachable, and must not tell the user to press A -- which on a
  // loaded list opens the row under the selection.
  {
    overlay::ConflictsModel model;
    model.Next();
    ipc::ConflictPage first;
    first.offset = 0;
    first.total = 12;
    first.has_more = true;
    for (int index = 0; index < ipc::kMaxConflictPage; ++index) {
      conflicts::Entry entry = SaveEntry(4, "page-one-" + std::to_string(index) + ".srm");
      entry.id = 100 + index;
      first.entries.push_back(Row(entry, true));
    }
    model.OnPage(first);
    c.ExpectEq(model.loaded(), static_cast<std::size_t>(ipc::kMaxConflictPage),
               "the first page is loaded");

    model.MoveSelection(ipc::kMaxConflictPage);  // to the bottom; asks for the next
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kListConflicts,
             "scrolling toward the end asks for the next page");
    model.OnRefused(ipc::Error::kInternal);

    overlay::ConflictsView view = model.Render();
    c.ExpectEq(view.rows.size(), static_cast<std::size_t>(ipc::kMaxConflictPage),
               "the rows already loaded stay -- a failed page is a failed page");
    c.Expect(view.hint.find("press A") == std::string::npos,
             "and the hint does not tell the user to press A, which opens a row: " + view.hint);
    c.Expect(view.hint.find("scroll") != std::string::npos,
             "...it says what actually retries: " + view.hint);
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "nothing is asked for while the failure stands");

    // **At the bottom, where the failure actually lands.** The fetch that can
    // fail is only issued within `kConflictPrefetchRows` of the last loaded row,
    // so the selection is pinned by `clamp` when the refusal arrives. A retry
    // that needed the index to *change* would leave every further Down press
    // inert exactly where the hint says to press it.
    c.ExpectEq(model.Render().selected, ipc::kMaxConflictPage - 1,
               "the selection is at the last loaded row, where clamp pins it");
    model.MoveSelection(1);
    c.ExpectEq(model.Render().selected, ipc::kMaxConflictPage - 1, "...and does not move");
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kListConflicts,
             "but the press is still the retry: a pinned selection is the common case, "
             "not the exception");

    // ...and one retry per press rather than one per frame: nothing asks again
    // until the user presses again.
    model.OnRefused(ipc::Error::kInternal);
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "a second refusal stops the asking until the next press");

    model.MoveSelection(-1);
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kListConflicts,
             "and scrolling the other way is a retry too: the rest of the history is "
             "reachable again");
  }

  // A restore of an entry **below the first page** keeps the detail and its
  // outcome. A model that re-read the list here would fetch offset 0, replace
  // the loaded rows, and lose the entry the user is looking at.
  {
    overlay::ConflictsModel model;
    model.Next();
    ipc::ConflictPage first;
    first.offset = 0;
    first.total = 12;
    first.has_more = true;
    for (int index = 0; index < ipc::kMaxConflictPage; ++index) {
      conflicts::Entry entry = ReplacedEntry(4, "first-" + std::to_string(index) + ".srm");
      entry.id = 200 + index;
      first.entries.push_back(Row(entry, true));
    }
    model.OnPage(first);
    // The prefetch fires within `kConflictPrefetchRows` of the end, not at the
    // top of a freshly loaded page.
    model.MoveSelection(ipc::kMaxConflictPage - overlay::kConflictPrefetchRows);
    const overlay::ConflictsModel::Command prefetch = model.Next();
    c.Expect(prefetch.kind == overlay::ConflictsModel::Command::Kind::kListConflicts,
             "scrolling toward the end asks for the second page");
    c.ExpectEq(prefetch.query.offset, ipc::kMaxConflictPage, "starting where the first ended");
    ipc::ConflictPage second;
    second.offset = ipc::kMaxConflictPage;
    second.total = 12;
    for (int index = 0; index < 4; ++index) {
      conflicts::Entry entry = ReplacedEntry(5, "second-" + std::to_string(index) + ".srm");
      entry.id = 300 + index;
      second.entries.push_back(Row(entry, true));
    }
    model.OnPage(second);
    c.ExpectEq(model.loaded(), std::size_t{12}, "both pages are loaded");

    model.MoveSelection(9 - (ipc::kMaxConflictPage - overlay::kConflictPrefetchRows));
    model.Activate();        // open it
    model.Activate();        // confirm
    model.Activate();        // send
    const overlay::ConflictsModel::Command sent = model.Next();
    c.Expect(sent.kind == overlay::ConflictsModel::Command::Kind::kRestore,
             "the restore is sent for an entry on the second page");
    c.ExpectEq(sent.entry_id, std::int64_t{301}, "the one that was open");

    conflicts::RestoreReport report;
    report.outcome = conflicts::RestoreOutcome::kRestored;
    report.backup_sd_path = "/config/rommsync/.backup/4-retroarch-srm-1757000500.srm";
    model.OnRestoreState(Done(301, report));
    const overlay::ConflictsView after = model.Render();
    c.Expect(after.mode == overlay::ConflictsMode::kDetail, "the detail stays open");
    c.Expect(after.headline.find("Restored") != std::string::npos,
             "showing what happened, not that the entry has gone: " + after.headline);
    c.ExpectEq(model.loaded(), std::size_t{12},
               "and the loaded rows are untouched -- a restore rewrites no entry");
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "so nothing needs re-reading");
  }

  // ...except the one fact a restore does settle: a backup it could not find.
  {
    overlay::ConflictsModel model;
    model.Next();
    ipc::ConflictPage page;
    page.offset = 0;
    page.total = 1;
    conflicts::Entry entry = ReplacedEntry(4, "vanished.srm");
    entry.id = 44;
    page.entries = {Row(entry, true)};
    model.OnPage(page);
    model.Activate();
    model.Activate();
    model.Activate();
    model.Next();
    conflicts::RestoreReport gone;
    gone.outcome = conflicts::RestoreOutcome::kBackupMissing;
    model.OnRestoreState(Done(44, gone));
    c.Expect(model.Back(), "back to the list");
    const overlay::ConflictsView view = model.Render();
    c.Expect(view.rows[0].restorable == overlay::Restorability::kBackupGone,
             "a restore that could not find the backup marks the row it was for");
  }

  // **M9-6 (#209): the restore is polled, not waited for.** `RestoreBackup`
  // answers the moment the sysmodule has taken it, and the screen says so and
  // asks `GetRestoreState` until the copy is done -- a frame at a time, so
  // nothing the overlay draws waits on the card.
  {
    overlay::ConflictsModel model = OneRestorable(51);
    const overlay::ConflictsModel::Command sent = model.Next();
    c.Expect(sent.kind == overlay::ConflictsModel::Command::Kind::kRestore,
             "the confirmed press sends the restore");
    model.OnRestoreState(Running(51));
    overlay::ConflictsView view = model.Render();
    c.Expect(view.headline.find("Restoring") != std::string::npos,
             "a taken restore reads as one in progress: " + view.headline);
    c.Expect(!view.can_restore, "and is not on offer a second time while it runs");
    for (int frame = 0; frame < 3; ++frame) {
      const overlay::ConflictsModel::Command poll = model.Next();
      c.Expect(poll.kind == overlay::ConflictsModel::Command::Kind::kRestoreState,
               "every frame while it runs asks where it stands");
      model.OnRestoreState(Running(51));
    }
    model.Next();
    conflicts::RestoreReport report;
    report.outcome = conflicts::RestoreOutcome::kRestored;
    model.OnRestoreState(Done(51, report));
    view = model.Render();
    c.Expect(view.headline.find("Restored") != std::string::npos,
             "and the outcome lands when it is done: " + view.headline);
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "after which nothing more is asked");
  }

  // Leaving the entry while it runs does not drag the user back to it: the
  // outcome is kept for that entry, and the list stays where they went.
  {
    overlay::ConflictsModel model = OneRestorable(52);
    model.Next();
    model.OnRestoreState(Running(52));
    c.Expect(model.Back(), "B leaves the running restore's entry");
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kRestoreState,
             "and the restore is still followed from the list");
    conflicts::RestoreReport report;
    report.outcome = conflicts::RestoreOutcome::kRestored;
    model.OnRestoreState(Done(52, report));
    c.Expect(model.Render().mode == overlay::ConflictsMode::kList,
             "a restore finishing does not pull the screen back to its entry");
  }

  // Somebody else's restore is running: the press started nothing, and says so
  // rather than drawing "Restoring" over the wrong entry.
  {
    overlay::ConflictsModel model = OneRestorable(53);
    model.Next();
    model.OnRestoreState(Running(999));
    const overlay::ConflictsView view = model.Render();
    c.Expect(view.headline.find("Nothing was written") != std::string::npos,
             "a press while another restore runs wrote nothing, and says so: " + view.headline);
    c.Expect(view.hint.find("another restore") != std::string::npos,
             "...and why: " + view.hint);
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "and nothing is polled for a restore this screen did not start");
  }

  // **A sysmodule that is slow to answer is not one that went away** (#209,
  // found in review). The press's answer ran out of the client's budget, so
  // whether the restore was taken is unknown -- but the session was never
  // dropped, so nothing about the screen is stale. It keeps its rows and its
  // entry, never sends the restore twice, and asks where it stands.
  {
    overlay::ConflictsModel model = OneRestorable(55);
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kRestore,
             "the press goes out");
    model.OnUnreachable(overlay::Link::kNotResponding);
    overlay::ConflictsView view = model.Render();
    c.Expect(view.link == overlay::Link::kNotResponding && !view.headline.empty(),
             "the screen says the sysmodule is not responding");
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "and asks it nothing more while it is not");
    model.OnLinkRestored();
    const overlay::ConflictsModel::Command next = model.Next();
    c.Expect(next.kind == overlay::ConflictsModel::Command::Kind::kRestoreState,
             "once it answers again, the screen asks where the restore stands -- it does not "
             "press again for the user");
    c.ExpectEq(model.loaded(), std::size_t{1}, "and the history it had loaded is still there");
    conflicts::RestoreReport report;
    report.outcome = conflicts::RestoreOutcome::kRestored;
    model.OnRestoreState(Done(55, report));
    view = model.Render();
    c.Expect(view.mode == overlay::ConflictsMode::kDetail &&
                 view.headline.find("Restored") != std::string::npos,
             "so the outcome lands on the entry it was for: " + view.headline);
  }

  // ...while a page that ran out of budget is simply asked for again.
  {
    overlay::ConflictsModel model;
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kListConflicts,
             "the first page goes out");
    model.OnUnreachable(overlay::Link::kNotResponding);
    model.OnLinkRestored();
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kListConflicts,
             "and is asked again once the sysmodule answers");
  }

  // The sysmodule forgot the restore -- it restarted mid-copy. The screen does
  // not wait for ever and does not claim an outcome it never heard.
  {
    overlay::ConflictsModel model = OneRestorable(54);
    model.Next();
    model.OnRestoreState(Running(54));
    model.Next();
    model.OnRestoreState(ipc::RestoreState{});
    const overlay::ConflictsView view = model.Render();
    c.Expect(view.headline.find("lost track") != std::string::npos,
             "a restore the sysmodule no longer knows about is said to be lost: " +
                 view.headline);
    c.Expect(view.tone == overlay::Tone::kWarn, "as a warning rather than a success");
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "and the polling stops");
  }

  // A producer that echoes the wrong offset forever must not be asked forever.
  {
    overlay::ConflictsModel model;
    for (int round = 0; round < kConflictMaxEmptyPagesRounds; ++round) {
      if (model.Next().kind != overlay::ConflictsModel::Command::Kind::kListConflicts) {
        break;
      }
      ipc::ConflictPage wrong;
      wrong.offset = 99;  // not what was asked for
      wrong.has_more = true;
      conflicts::Entry entry = SaveEntry(4, "wrong.srm");
      entry.id = 5;
      wrong.entries = {Row(entry, true)};
      model.OnPage(wrong);
    }
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "a producer answering into the wrong place is not asked once a frame forever");
    c.ExpectEq(model.loaded(), std::size_t{0}, "and none of its pages were appended");
  }

  // A sysmodule that is not there is a first-class state, drawn with the same
  // three sentences every other screen uses.
  {
    overlay::ConflictsModel model;
    model.OnUnreachable(overlay::Link::kNotRunning);
    const overlay::ConflictsView view = model.Render();
    c.Expect(view.link == overlay::Link::kNotRunning, "the link is reported");
    c.Expect(!view.headline.empty(), "with a sentence rather than a blank screen");
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "and nothing is sent to a sysmodule that is not there");
    model.OnLinkRestored();
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kListConflicts,
             "a session that came back starts the list again");
  }

  // A producer that answered an empty page with `has_more` forever would have
  // the overlay asking at a page per frame. It is bounded.
  {
    overlay::ConflictsModel model;
    for (int round = 0; round < overlay::kConflictMaxEmptyPages + 2; ++round) {
      if (model.Next().kind != overlay::ConflictsModel::Command::Kind::kListConflicts) {
        break;
      }
      ipc::ConflictPage empty;
      empty.offset = 0;
      empty.has_more = true;
      model.OnPage(empty);
    }
    c.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
             "a producer answering empty pages forever is not asked forever");
  }

  // The three rules overlay/AGENTS.md holds every screen to. Only a grep can say
  // them, and they are about the code rather than about its behaviour.
  const std::string screen_dir = ROMMSYNC_OVERLAY_SOURCE_DIR;
  for (const std::filesystem::directory_entry& file :
       std::filesystem::directory_iterator(screen_dir)) {
    if (!file.is_regular_file()) {
      continue;
    }
    std::ifstream source(file.path());
    std::string line;
    int at = 0;
    while (std::getline(source, line)) {
      ++at;
      const std::string where = file.path().filename().string() + ":" + std::to_string(at);
      const bool comment = line.find("//") != std::string::npos;
      // The sysmodule owns every write to `config.ini` (docs/ARCHITECTURE.md).
      c.Expect(comment || line.find("SaveConfig") == std::string::npos,
               where + " writes config.ini; the sysmodule owns that");
      c.Expect(comment || line.find("WriteConfig") == std::string::npos,
               where + " writes config.ini; the sysmodule owns that");
      // ...and the overlay restores nothing itself: a restore is a save
      // overwrite that owes a backup first, and that lives on the service.
      c.Expect(comment || line.find("conflicts::Restore(") == std::string::npos,
               where + " restores a backup itself; that is the sysmodule's (#36)");
      c.Expect(comment || line.find("BackUpFirst") == std::string::npos,
               where + " writes a backup itself; that is the sysmodule's");
    }
  }
  // ...and the view model names no libnx or libultrahand type (hard rule 4).
  for (const char* path : {ROMMSYNC_CONFLICTS_VIEW_HDR, ROMMSYNC_CONFLICTS_VIEW_SRC}) {
    std::ifstream source(path);
    c.Expect(source.good(), std::string("readable: ") + path);
    std::string line;
    int at = 0;
    while (std::getline(source, line)) {
      ++at;
      const bool comment = line.find("//") != std::string::npos;
      const std::string where = std::string(path) + ":" + std::to_string(at);
      c.Expect(comment || line.find("switch.h") == std::string::npos, where + " includes libnx");
      c.Expect(comment || line.find("tesla.hpp") == std::string::npos,
               where + " includes libultrahand");
      c.Expect(comment || line.find("tsl::") == std::string::npos,
               where + " names a libultrahand type");
    }
  }
}

// --- recorded -----------------------------------------------------------------
//
// The acceptance criterion no hand-built plan can meet: a **real** `conflict`
// from the docker RomM, executed by the engine's own code, producing an entry
// with both hashes, the server's reason string, and a backup path that exists --
// and both reasons, because the second is the one a `default:` branch swallows.

/// Negotiate, read the plan with the engine's own parser, and keep only the
/// operations for `slots`. `test_sync_execute.cpp`'s `PlanFor`, for its reason:
/// a shared fixture answers with every save this device has no history for.
bool PlanFor(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const sync::SyncNegotiatePayload& payload,
             const std::vector<std::string>& slots, sync::SyncPlan* out) {
  const http::Result result = harness::Negotiate(checks, client, base, fixture, payload);
  // One active session per device, and every scenario here shares the fixture's
  // (#76).
  harness::CloseSession(client, base, fixture, result.response.body);
  if (result.response.status != 200) {
    checks.Expect(false, "the negotiation is answered: HTTP " +
                             std::to_string(result.response.status) + " " + result.response.body);
    return false;
  }
  const auth::Parsed<sync::SyncPlan> parsed = sync::ParseNegotiateResponse(result.response.body);
  if (!parsed.ok()) {
    checks.Expect(false, "the engine reads the plan: " + parsed.error.Describe());
    return false;
  }
  out->session_id = parsed.value.session_id;
  for (const std::string& slot : slots) {
    for (const sync::SyncOperation& operation : parsed.value.operations) {
      if (operation.slot.has_value() && *operation.slot == slot) {
        out->operations.push_back(operation);
      }
    }
  }
  if (out->operations.size() != slots.size()) {
    checks.Expect(false, "the plan carries an operation for each of this run's slots: " +
                             result.response.body);
    return false;
  }
  return true;
}

auth::StoredToken TokenFor(const std::string& base, const Fixture& fixture) {
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  return token;
}

void Recorded(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "conflicts-recorded");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  // Two conflicts in one plan, one of each reason.
  //
  //   `both_changed`   -- a sync record, and each side moved past it. Arranged
  //                       with `device_id` on the first upload and a `PUT` for
  //                       the second: a second `POST` would be a new save row
  //                       with no history, which is the other conflict entirely.
  //   `same_timestamp` -- no sync record, equal timestamps, different hashes.
  const std::string history_slot = harness::UniqueSlot("m7-1-both");
  const std::string notime_slot = harness::UniqueSlot("m7-1-same-ts");
  const std::string history_name = "both-changed.srm";
  const std::string notime_name = "same-timestamp.srm";
  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");

  harness::Save history_row;
  rig::WriteFile(staged, "server v1\n");
  if (!harness::UploadSave(client, base, fixture, rom.id, history_slot, "m7-1", staged,
                           history_name, /*with_device=*/true, &history_row)) {
    checks.Expect(false, "the first server copy was stored");
    return;
  }
  harness::PassASecond();
  const std::string history_server = "server v2, longer\n";
  rig::WriteFile(staged, history_server);
  harness::Save moved;
  if (!harness::ReplaceSave(client, base, fixture, history_row.id, staged, history_name, &moved)) {
    checks.Expect(false, "the server copy moved forward in place");
    harness::DeleteSave(client, base, fixture, history_row.id);
    return;
  }

  const std::string notime_server = "server copy\n";
  rig::WriteFile(staged, notime_server);
  harness::Save notime_row;
  if (!harness::UploadSave(client, base, fixture, rom.id, notime_slot, "m7-1", staged, notime_name,
                           /*with_device=*/false, &notime_row)) {
    checks.Expect(false, "the second server copy was stored");
    harness::DeleteSave(client, base, fixture, history_row.id);
    return;
  }
  sync::Timestamp notime_when;
  if (!harness::ParseServerTimestamp(notime_row.updated_at, &notime_when)) {
    checks.Expect(false, "the server's updated_at parses: " + notime_row.updated_at);
    harness::DeleteSave(client, base, fixture, history_row.id);
    harness::DeleteSave(client, base, fixture, notime_row.id);
    return;
  }

  const std::string history_local = "device v2\n";
  const std::string notime_local = "device copy\n";
  sandbox.SeedSave(SavePath(history_name), history_local);
  sandbox.SeedSave(SavePath(notime_name), notime_local);

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(
      rom.id, history_name, history_slot, "m7-1", crypto::Md5Hex(history_local),
      std::chrono::system_clock::now() + std::chrono::seconds{300},
      static_cast<std::int64_t>(history_local.size())));
  payload.saves.push_back(harness::LocalSave(
      rom.id, notime_name, notime_slot, "m7-1", crypto::Md5Hex(notime_local), notime_when,
      static_cast<std::int64_t>(notime_local.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {history_slot, notime_slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, history_row.id);
    harness::DeleteSave(client, base, fixture, notime_row.id);
    return;
  }
  checks.Expect(plan.operations[0].reason == sync::Reason::kBothChanged,
                std::string("the first is the history conflict: ") + plan.operations[0].reason_text);
  checks.Expect(plan.operations[1].reason == sync::Reason::kSameTimestampDifferentContent,
                std::string("the second is the no-history one: ") + plan.operations[1].reason_text);

  sync::ExecuteOptions execute;
  execute.backup_dir = harness::kBackupDir;
  execute.now = []() { return Moment(kWhen); };
  const std::vector<sync::SaveTarget> targets = {
      {rom.id, history_slot, SavePath(history_name), history_name},
      {rom.id, notime_slot, SavePath(notime_name), notime_name},
  };
  const sync::ExecutionReport report =
      sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, targets, execute);
  checks.ExpectEq(report.completed, 2,
                  "both conflicts resolved" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
  if (report.operations.size() != 2) {
    harness::DeleteSave(client, base, fixture, history_row.id);
    harness::DeleteSave(client, base, fixture, notime_row.id);
    return;
  }

  conflicts::History history(sandbox.Host(std::string(conflicts::kHistorySdPath)));
  history.Load();
  conflicts::RecordOptions options = At(kWhen);
  options.rom_name = [&rom](std::int64_t rom_id) {
    return rom_id == rom.id ? rom.fs_name : std::string();
  };
  checks.ExpectEq(conflicts::RecordSaves(&history, plan, report, payload.saves, options),
                  std::size_t{2}, "a real conflict of each reason produces an entry");

  // Newest first, so [1] is the history conflict and [0] is the no-history one.
  const conflicts::Entry& both = history.entries()[1];
  const conflicts::Entry& same = history.entries()[0];
  checks.ExpectEq(both.reason, std::string("Both sides changed since last sync"),
                  "the entry carries the sentence RomM actually sent");
  checks.ExpectEq(same.reason, std::string("Same timestamp but different content"),
                  "...and so does the other one");
  checks.ExpectEq(both.local_content_hash, crypto::Md5Hex(history_local),
                  "the local MD5 is this console's");
  checks.Expect(both.server_content_hash.has_value() &&
                    *both.server_content_hash == moved.content_hash,
                "and the server's is RomM's own, so a human can tell the copies apart");
  checks.Expect(!both.server_updated_at.empty(), "with the server's timestamp beside it");
  checks.Expect(!both.backup_sd_path.empty() && sandbox.Exists(both.backup_sd_path),
                "and a backup path that exists: " + both.backup_sd_path);
  checks.ExpectEq(sandbox.Read(both.backup_sd_path), history_local,
                  "holding exactly the bytes this console had");
  checks.ExpectEq(sandbox.Read(SavePath(history_name)), history_server,
                  "while the card holds the server's copy");

  // ...and the restore puts them back, backing up the server's copy on the way.
  // The sandbox teardown audit is what checks that half.
  conflicts::RestoreOptions restore;
  restore.backup_dir = harness::kBackupDir;
  restore.now = []() { return Moment(kWhen + 500); };
  const conflicts::RestoreReport done = conflicts::Restore(*files, history, both.id, restore);
  checks.Expect(done.ok(), std::string("the restore ran: ") + conflicts::ToString(done.outcome) +
                               " -- " + done.message);
  checks.ExpectEq(sandbox.Read(SavePath(history_name)), history_local,
                  "the pre-conflict bytes are back, exactly");
  checks.Expect(!done.backup_sd_path.empty() &&
                    sandbox.Read(done.backup_sd_path) == history_server,
                "and the server's copy is under .backup/ rather than gone: " + done.message);

  harness::DeleteSave(client, base, fixture, history_row.id);
  harness::DeleteSave(client, base, fixture, notime_row.id);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "store";
  const std::string base = rig::BaseUrl();

  if (scenario == "store" || scenario == "record" || scenario == "restore" ||
      scenario == "interrupted" || scenario == "service" || scenario == "overlay" ||
      scenario == "crash" || scenario == "keep_local" || scenario == "late_write") {
    checks::Checks checks;
    if (scenario == "store") {
      Store(checks);
    } else if (scenario == "record") {
      Record(checks);
    } else if (scenario == "restore") {
      Restore(checks);
    } else if (scenario == "interrupted") {
      Interrupted(checks);
    } else if (scenario == "crash") {
      Crash(checks);
    } else if (scenario == "late_write") {
      LateWrite(checks);
    } else if (scenario == "keep_local") {
      KeepLocal(checks);
    } else if (scenario == "service") {
      Service(checks);
    } else {
      Overlay(checks);
    }
    if (checks.failures() == 0) {
      std::cout << "conflicts." << scenario << " ok\n";
    }
    return checks.failures() == 0 ? 0 : 1;
  }

  rig::Checks checks;
  const std::unique_ptr<http::HttpClient> client = rig::MakeClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  rig::DisarmFault(*client, base);

  Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    return rig::kSkip;
  }
  // A session an earlier scenario left open is one this scenario's negotiate has
  // to cancel, and that cancel races with the session it just created (#76).
  harness::CloseOpenSessions(*client, base, fixture);

  harness::Rom rom;
  if (!harness::FindRom(*client, base, fixture, "gb240p.gb", &rom)) {
    std::cerr << "the fixture library holds no roms\n"
                 "  scan it with: ./.venv/bin/python server/testing/provision.py\n";
    return rig::kSkip;
  }

  if (scenario == "recorded") {
    Recorded(checks, *client, base, fixture, rom);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  rig::DisarmFault(*client, base);
  harness::ExpectDisarmed(checks, *client, base, "the scenario left the proxy disarmed");
  harness::DeleteArchivalCopies(*client, base, fixture, rom.id);  // #240

  if (checks.failures() == 0) {
    std::cout << "conflicts." << scenario << " ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
