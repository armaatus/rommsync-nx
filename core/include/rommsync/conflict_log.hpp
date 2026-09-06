// What a sync did to a save that changed on both sides, and how to put the old
// bytes back.
//
// Hard rule 2 says a save is copied under `.backup/` before it is overwritten.
// That guarantee is worth nothing to a player who cannot find the copy: an SD
// card in a console has no file manager, and `4-retroarch-srm-1757000000.srm`
// names neither the game nor the day. This is the index that makes the
// directory legible -- one entry per overwrite, written as it happens, read
// back by the overlay (M7-1, #36) -- and the restore that uses it.
//
// ## What gets an entry
//
// Every write that replaced bytes a player could have wanted:
//
//   - a save `conflict`, resolved keep-both (`OperationOutcome::kKeptBoth`);
//   - a save `download` that landed on a file that was already there
//     (`kDownloaded` with a backup beside it);
//   - a **state** download that replaced one (`StateOutcome::kDownloaded`);
//   - a state the two sides could not tell apart (`StateOutcome::kKeptBoth`),
//     which replaced nothing and is here because it is the one case where the
//     player has two copies and the client will never choose between them.
//
// An `upload` and a `no_op` get none: nothing on the card changed.
//
// ## The backup path is stored, never derived
//
// `sync::ExecutePlan` and `sync::SyncStates` each report the exact path they
// wrote (`OperationResult::backup_sd_path`, `StateOperationResult::backup_sd_path`),
// uniquifier and all. Rebuilding it here from `rom_id`, the slot and a
// timestamp would produce a different name the moment `BackUpFirst` had to step
// past an occupied one -- and a restore from a path that names somebody else's
// backup is the failure this file exists to prevent.
//
// ## `[sync] conflict_show` hides the screen, never the recording
//
// Nothing below reads the configuration. The toggle is the overlay's -- it
// decides whether the menu offers the screen -- and a console that had it off
// for a month still has every entry and every backup when it goes on. Recording
// is what makes the backups findable, so a toggle that stopped it would turn
// hard rule 2 back into a directory of unnamed files.
//
// ## Where the halves live
//
// This header is the **entry and the file**: the type, the format, the bounds
// and the store. `conflict_record.hpp` is what fills it in from a sync report
// and what restores from it, and it is a separate header on purpose -- filling
// an entry in needs `sync_execute.hpp` and `state_sync.hpp`, reading one needs
// nothing, and `ipc.hpp` carries this type across the wire. A boundary header
// that dragged the executor in behind it would put the whole sync engine in
// front of every screen that draws a row.
//
// Hard rule 4 applies as it does to the rest of `core/`: no libnx header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/json.hpp"

namespace rommsync::conflicts {

/// Where the history lives, SD-root absolute -- `state.db`'s neighbour and
/// `.backup/`'s, for `sync::kStateSdPath`'s reason: docs/ARCHITECTURE.md puts
/// the client's own records under `/config/rommsync`, and a path spelled again
/// in each caller is a path two of them eventually spell differently.
inline constexpr const char* kHistorySdPath = "/config/rommsync/conflicts.db";

/// The same file's name on its own, for the caller that has the directory
/// already -- `state::kStateFileName`'s shape, and its reason.
inline constexpr const char* kHistoryFileName = "conflicts.db";

/// The first line of a well-formed file, `<magic> <version>`.
inline constexpr const char* kFormatMagic = "rommsync-conflicts";
inline constexpr int kFormatVersion = 1;

/// How many entries are kept, and the largest file that will be read.
///
/// Sized against the sysmodule's heap the way `state::kMaxRecords` is, and
/// deliberately far smaller: `state.db` holds a row per save on the card and
/// peaks near 240 KiB of the ~390 KiB left after the socket transfer memory
/// (docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision), so this has to
/// fit in what is left over beside it *and* beside a transfer.
///
/// Thirty-two is also more than a screen is for. A conflict is the exceptional
/// outcome -- both sides moving past one sync record -- and a card producing
/// thirty-three of them has a problem the newest thirty-two already describe.
///
/// **Dropping an entry never touches its backup.** The oldest row falls off the
/// end of the file; the file it names stays on the card, because deleting a
/// backup is the one thing this module may never do (hard rule 2). It becomes
/// unlisted, not gone.
inline constexpr std::size_t kMaxEntries = 32;
inline constexpr std::size_t kMaxHistoryBytes = 48 * 1024;

/// How long one string on an entry may be.
///
/// A rom's name and a save's file name are the user's data and have no length
/// this client may assume, and every one of them crosses `ipc::kMaxPayloadBytes`
/// on the way to the overlay. Cut on a UTF-8 boundary with `...` in place of the
/// rest, like `lists::Shorten` -- and cut **before** the entry is stored, so the
/// bound above is a bound rather than an estimate.
///
/// `sd_path` and `backup_sd_path` are the exceptions and are *not* cut: they are
/// opened, not read, and a truncated path is a restore that writes somewhere
/// else. An entry whose paths do not fit is refused instead; see `Append`.
inline constexpr std::size_t kMaxTextBytes = 128;

/// How many diagnostics a load keeps. `state::kMaxDiagnostics`'s reasoning.
inline constexpr std::size_t kMaxDiagnostics = 16;

/// Which of the two things that get overwritten this entry is about.
///
/// It is a field rather than two entry types because the screen draws one list:
/// what separates them is which halves of the comparison exist, not what a row
/// looks like. **A state has no slot and no server digest at all** -- RomM
/// computes none -- so a state entry's detail may show a length and an
/// `updated_at` and must never claim a content comparison it did not make.
enum class EntryKind { kSave, kState };
const char* ToString(EntryKind kind);

/// What happened to the file.
enum class Event {
  /// A save `conflict`, resolved keep-both: the server's copy is on the card
  /// and the local bytes are under `.backup/`. `sync::OperationOutcome::kKeptBoth`.
  kConflict,

  /// A download that landed on a file that was already there -- a save or a
  /// state. Not a conflict: the server's copy was the newer one and the client
  /// was told to take it. It is here because the bytes it replaced are just as
  /// gone, and just as backed up.
  kReplaced,

  /// **A state the two sides could not tell apart.** `sync::StateOutcome::kKeptBoth`
  /// is not a conflict that was resolved by overwriting: nothing was
  /// transferred, both copies survive, and there is therefore **no backup path**
  /// -- so this is the one event a restore has nothing to do with. It is worth
  /// showing as its own thing rather than folded into `kConflict`, which would
  /// have the screen offer a restore of a file nothing replaced.
  kKeptBoth,
};
const char* ToString(Event event);

/// True when this event replaced bytes, and therefore owes a backup.
bool Overwrote(Event event);

/// One overwrite, as the overlay reads it back.
struct Entry {
  /// Stable for as long as the entry is in the file, and what `RestoreBackup`
  /// addresses. Never reused: it comes off a counter carried in the file, so an
  /// id that fell off the end can never name a later entry -- which is what
  /// lets a stale overlay page be refused rather than acted on.
  std::int64_t id = 0;

  EntryKind kind = EntryKind::kSave;
  Event event = Event::kConflict;

  std::int64_t rom_id = 0;

  /// What to call the game. Empty when the caller had no index to ask -- the
  /// screen falls back to `file_name`, which is always there.
  std::string rom_name;

  /// **The client's own name for the file**, never the server's: RomM renames a
  /// save on ingest and the tagged name is a file no emulator loads
  /// (sync_execute.hpp). For a state it is also the pairing key with `rom_id`.
  std::string file_name;

  /// The save's pairing key. Absent for an archival save **and** for a state,
  /// which are told apart by `kind`: a state is keyed `(rom_id, file_name)` and
  /// RomM gives it no slot at all (state_sync.hpp).
  std::optional<std::string> slot;

  /// `retroarch`, `tico`, or empty when the directory said nothing.
  std::string emulator;

  /// When this happened, whole seconds since the Unix epoch, UTC.
  std::int64_t when = 0;

  /// **The server's own sentence**, carried rather than classified: the screen
  /// shows the user what RomM said. Empty for a state, which is not arbitrated
  /// by the sync endpoint and carries no reason.
  std::string reason;

  /// The local file this replaced, SD-root absolute. What a restore writes.
  std::string sd_path;

  /// The local copy as it was before the overwrite -- what the client reported
  /// to `negotiate`, or what the state scan read.
  std::int64_t local_size_bytes = 0;
  std::string local_content_hash;  ///< MD5, 32 lowercase hex; empty when unhashed
  std::int64_t local_modified = 0;  ///< mtime, whole seconds, UTC

  /// The server copy's MD5. **Absent on every state entry**, and on a save the
  /// server had no copy of. A screen may only draw a content comparison when
  /// this is set.
  std::optional<std::string> server_content_hash;

  /// The server copy's timestamp, exactly as it was sent -- RomM spells it with
  /// a numeric offset (`2026-09-04T11:36:27+00:00`) rather than the `Z` this
  /// client writes, so it is carried as text and never parsed (sync.hpp).
  /// Empty when there was none.
  std::string server_updated_at;

  /// The server copy's length. Set for a state, where it is the only comparison
  /// there is; zero when unknown.
  std::int64_t server_size_bytes = 0;

  /// **The exact path `BackUpFirst` wrote**, SD-root absolute. Empty exactly
  /// when nothing was overwritten -- a `kKeptBoth` state. Never re-derived; see
  /// the header note.
  std::string backup_sd_path;

  /// True when this entry could be restored if the backup is still there.
  bool restorable() const { return Overwrote(event) && !backup_sd_path.empty(); }
};

/// One entry as a JSON object -- **the row in `conflicts.db` and the item in an
/// `ipc::ConflictPage`, spelled once**.
///
/// The alternative is a second copy of eighteen field names in `ipc.cpp`, which
/// is the drift `ipc::list_keys` exists to prevent one level down: a producer
/// and a consumer that spell `server_updated_at` differently do not fail to
/// build, they draw a screen of blanks.
std::string SerializeEntry(const Entry& entry);

/// Read one back, or say what was wrong with it. `why` is set on false.
///
/// Strict in `json::Reader`'s way: every field must be present and of the right
/// type. A defaulted `backup_sd_path` would be an entry that says it can be
/// restored and cannot, and a defaulted `sd_path` would be a restore aimed at
/// nothing.
bool ParseEntry(const json::Value& object, Entry* out, std::string* why);

/// The whole file, version line included, ending in a newline.
///
/// `next_id` rides in the header line so ids keep increasing across a boot and
/// across the oldest entries falling off the end. A counter rebuilt from the
/// rows would restart at one on an emptied history and hand a live overlay page
/// an id naming a different entry.
std::string SerializeHistory(const std::vector<Entry>& entries, std::int64_t next_id);

/// A history and everything wrong with the file it came from.
struct LoadedHistory {
  /// Newest first. Always usable: empty is a perfectly good history, and it is
  /// what every failure here produces.
  std::vector<Entry> entries;

  /// The id the next entry takes. At least `1`, and always greater than every
  /// id in `entries`.
  std::int64_t next_id = 1;

  /// In the order they were found, bounded by `kMaxDiagnostics`. A first boot
  /// produces exactly one, saying there is no file yet.
  std::vector<std::string> diagnostics;
};

/// Parse the contents of a `conflicts.db`.
///
/// Pure: no filesystem, no clock. **A row that will not read is skipped and the
/// rest are kept**, which is where this parts company with `state::ParseBaseline`
/// -- and the reason is what the two files are. A baseline is an optimisation
/// whose rows are only meaningful together, so a partial one is discarded and
/// re-hashed at no cost. A history row is a *pointer at a backup*, and throwing
/// away thirty-one good ones because the thirty-second was truncated is
/// throwing away the only index a player has to files they may want back. Each
/// skip is named in `diagnostics`.
///
/// A missing or wrong header line is still fatal: those bytes are not this file.
LoadedHistory ParseHistory(std::string_view text);

/// Read `path` and parse it.
///
/// A *missing* file falls back to `io::PreviousPathFor(path)`, the same recovery
/// `state::LoadBaseline` makes and for its reason: the one moment `conflicts.db`
/// legitimately does not exist is the window between `io::WriteAtomically`'s two
/// renames.
LoadedHistory LoadHistory(const std::string& path);

/// Why writing the history did not work. Mirrors `state::StoreError`.
enum class StoreError {
  kNone,
  kTooLarge,      ///< the serialized file exceeds `kMaxHistoryBytes`
  kUnusableEntry, ///< the entry names no rom, no file, or a path that does not fit
  kOpenFailed,    ///< the temp file could not be created -- usually a missing directory
  kWriteFailed,   ///< the bytes did not all reach the card; the destination is untouched
  kCommitFailed,  ///< the rename onto `path` failed; see atomic_file.hpp
};
const char* ToString(StoreError error);

struct StoreResult {
  StoreError error = StoreError::kNone;

  /// For logs. Names the path and what went wrong, never a file's contents.
  std::string message;

  /// The id the entry was given. Zero when nothing was appended.
  std::int64_t id = 0;

  bool ok() const { return error == StoreError::kNone; }
};

/// The history as the card holds it, and the appends that keep the two in step.
///
/// **Every append writes the file**, the way `download::Queue` persists after
/// every state transition and for its reason: the entry exists to point at a
/// backup that has just been written, and a power cut between the overwrite and
/// a deferred flush is exactly the moment the pointer is worth most. It costs
/// one `io::WriteAtomically` per conflict, which is a rare event by
/// construction.
///
/// One instance per engine. Nothing here is thread-safe: it is touched from the
/// IPC thread and from whatever drives a tick, and the caller owns that seam --
/// `ipc::Engine`'s contract, the same way `lists::Service` states it.
class History {
 public:
  /// `path` is SD-root absolute and is resolved by the caller's
  /// `fs::FileSystem` -- except that this class opens it directly, because the
  /// records under `/config/rommsync` are read and written by `io::` throughout
  /// (`state::SaveBaseline`, `auth::SaveToken`). Pass the resolved host path.
  explicit History(std::string path);

  const std::string& path() const { return path_; }

  /// Read the file. Returns what was wrong with it, for a log.
  std::vector<std::string> Load();

  /// Newest first, so `entries()[0]` is the last thing that happened.
  const std::vector<Entry>& entries() const { return entries_; }
  std::size_t size() const { return entries_.size(); }

  /// The entry with that id, or nullptr. Valid until the next append.
  const Entry* Find(std::int64_t id) const;

  /// Append `entry`, giving it the next id, and write the file.
  ///
  /// **The entry is in memory whether or not the write worked.** A failed write
  /// costs the card its copy of the index, not the running console's: the
  /// overlay can still show what just happened, and the next append tries
  /// again. Losing the in-memory entry as well would mean a card that would not
  /// write turned a backup that exists into a backup nobody can name.
  StoreResult Append(Entry entry);


 private:
  /// Write the whole file. The caller has already changed `entries_`.
  StoreResult Persist();

  std::string path_;
  std::vector<Entry> entries_;
  std::int64_t next_id_ = 1;
};

/// Cut `text` to `kMaxTextBytes` on a UTF-8 boundary, `...` in place of the
/// rest. Returns it unchanged when it already fits.
std::string Shorten(std::string_view text);

/// What a restore did.
///
/// Here rather than in `conflict_record.hpp` because it crosses the IPC wire:
/// the overlay asks the sysmodule to restore and draws the answer, and the two
/// halves share one spelling of it (`ipc.hpp`).
enum class RestoreOutcome {
  kRestored,

  /// No entry with that id: it fell off the end of the history, or the overlay
  /// is holding a page from before a reboot. Refused rather than guessed at --
  /// see `Entry::id`.
  kNoSuchEntry,

  /// The entry replaced nothing, so there is nothing to put back. A
  /// `Event::kKeptBoth` state.
  kNothingToRestore,

  /// The backup named by the entry is not on the card any more -- deleted by
  /// hand, or a card swapped. **The screen draws this before a press** rather
  /// than finding out at write time; it is still checked at restore time,
  /// because the screen's answer is one poll old.
  kBackupMissing,

  /// The bytes about to be replaced could not be copied first. **Nothing was
  /// written**, which is hard rule 2 doing its job one level up: a restore is a
  /// save overwrite and owes a backup exactly as a download does.
  kBackupFailed,

  /// The backup was made and the restore did not land. Whatever is at `sd_path`
  /// is what was there before -- `io::CopyAtomically` stages and commits, so
  /// there is no third state.
  kWriteFailed,
};
const char* ToString(RestoreOutcome outcome);

struct RestoreReport {
  RestoreOutcome outcome = RestoreOutcome::kNoSuchEntry;

  /// For the screen and for a log. Names what happened and where, never the
  /// contents of a save.
  std::string message;

  /// The backup of the bytes this restore *replaced*, SD-root absolute. Empty
  /// when there was no file at `sd_path` to protect, which is success with
  /// nothing to show for it (`sync::BackUpFirst`).
  std::string backup_sd_path;

  bool ok() const { return outcome == RestoreOutcome::kRestored; }
};

}  // namespace rommsync::conflicts
