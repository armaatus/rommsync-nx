// Turning a finished plan into the two things that outlive it: the counts RomM
// is told, and the rows `state.db` keeps. See sync_finish.hpp for why the
// baseline is written first.
#include "rommsync/sync_finish.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rommsync/file_system.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/sync_execute.hpp"

namespace rommsync::sync {
namespace {

using Key = state::Baseline::Key;

/// Append a warning, up to `state::kMaxDiagnostics` of them.
///
/// Bounded exactly as `state::LoadBaseline`'s diagnostics are, and for the same
/// reason: a card whose whole save folder went unreadable must not answer with
/// one string per save.
void AddWarning(std::vector<std::string>* into, std::string line) {
  if (into->size() < state::kMaxDiagnostics) {
    into->push_back(std::move(line));
  }
}

/// How a save is named in a warning. The rom and the slot, which is the pair
/// RomM keys a save on and the pair a reader can go and look at -- never the
/// file's own name, which is a game title a user chose.
std::string Name(std::int64_t rom_id, const std::optional<std::string>& slot) {
  return "rom " + std::to_string(rom_id) + ", slot " + (slot.has_value() ? *slot : "<none>");
}

/// The directory part of an SD path, and the leaf under it.
///
/// `/retroarch/saves/Game.srm` -> `/retroarch/saves` + `Game.srm`. A path with
/// no separator has no directory to list and is refused by the caller.
bool SplitPath(const std::string& sd_path, std::string* directory, std::string* leaf) {
  const std::size_t slash = sd_path.rfind('/');
  if (slash == std::string::npos || slash + 1 >= sd_path.size()) {
    return false;
  }
  *directory = slash == 0 ? "/" : sd_path.substr(0, slash);
  *leaf = sd_path.substr(slash + 1);
  return true;
}

/// The most recent directory listing, and nothing older.
///
/// `core/` has no single-file stat -- `fs::FileSystem` reads directories and
/// resolves paths, and nothing else -- so the mtime and the size of a save this
/// tick just rewrote come out of a listing of its folder.
///
/// **One entry, not a map.** A `fs::Listing` may hold up to
/// `fs::kMaxDirectoryEntries` names, each of them a heap `std::string`, and the
/// sysmodule's inner heap is 512 KiB (core/AGENTS.md). Keeping one listing per
/// directory for the length of a tick is unbounded in exactly the dimension
/// that is scarce, and it would be unbounded on the tick that is already the
/// most expensive -- the one that downloaded a lot of saves. Keeping the last
/// one costs a single listing and still collapses the case that matters, which
/// is several saves in the same folder: a plan is executed in the server's
/// order, and the server groups a rom's saves together.
class Directories {
 public:
  explicit Directories(fs::FileSystem& files) : files_(&files) {}

  /// The entry for `sd_path`, or nullptr with the reason in `why`.
  ///
  /// The pointer is into this object and is invalidated by the next call for a
  /// different directory, which is why the one caller reads it immediately.
  const fs::Entry* Find(const std::string& sd_path, std::string* why) {
    std::string directory;
    std::string leaf;
    if (!SplitPath(sd_path, &directory, &leaf)) {
      *why = "\"" + sd_path + "\" is not a path with a directory in it";
      return nullptr;
    }
    if (directory != directory_) {
      // Assigned rather than kept alongside the old one: the previous listing's
      // strings are freed before the next one is read.
      listing_ = files_->List(directory);
      directory_ = directory;
    }
    // Searched before `error` is consulted, on the scanner's stance
    // (`scan::ScanSaves`): a `kTooManyEntries` listing still holds the first
    // entries by name, so the save may well be in it, and dropping the row for a
    // folder that is merely large re-hashes that save on every tick from then
    // on. Only a listing that does not hold the file is a failure.
    for (const fs::Entry& entry : listing_.entries) {
      if (!entry.is_directory && entry.name == leaf) {
        return &entry;
      }
    }
    if (!listing_.ok()) {
      *why = "its directory could not be read: " + listing_.message;
      return nullptr;
    }
    *why = "it is no longer at " + sd_path;
    return nullptr;
  }

 private:
  fs::FileSystem* files_;

  /// Empty until the first listing. `SplitPath` never produces an empty
  /// directory -- the shortest it answers is `/` -- so this cannot collide with
  /// a real one.
  std::string directory_;
  fs::Listing listing_;
};

/// What a save looks like on the card right now.
struct LocalFacts {
  std::string content_hash;
  Timestamp mtime{};
  std::int64_t file_size_bytes = 0;
};

/// Re-stat and re-hash a save whose bytes this tick replaced.
///
/// Everything the tick reported about this file describes the bytes that are no
/// longer there, so all three fields come from the card. A row built from the
/// reported ones would claim a digest for bytes nothing ever hashed -- and
/// worse, would claim it against an mtime and a size that no longer match, which
/// is a row that lies and then never gets used.
bool ReadBack(fs::FileSystem& files, Directories& directories, const std::string& sd_path,
              LocalFacts* out, std::string* why) {
  if (sd_path.empty()) {
    *why = "the operation named no local file";
    return false;
  }
  const fs::Entry* entry = directories.Find(sd_path, why);
  if (entry == nullptr) {
    return false;
  }
  if (entry->modified_unix < kMinTimestampSeconds || entry->modified_unix > kMaxTimestampSeconds) {
    // The same window `Validate` holds a save's mtime to. `SaveBaseline` would
    // skip the row anyway; refusing it here is what names the save.
    *why = "its mtime of " + std::to_string(entry->modified_unix) +
           " is not one a save can claim";
    return false;
  }
  const std::string resolved = files.Resolve(sd_path);
  if (resolved.empty()) {
    *why = sd_path + " is not a path on this card";
    return false;
  }
  const state::HashOutcome hashed = state::HashFile(resolved);
  if (!hashed.ok()) {
    *why = hashed.message;
    return false;
  }
  out->content_hash = hashed.content_hash;
  out->mtime = Timestamp{} + std::chrono::seconds{entry->modified_unix};
  out->file_size_bytes = entry->size_bytes;
  return true;
}

/// Drop rows nothing mentioned this tick, but only as far as the bound.
///
/// The failure this exists for is the one #11 named, arriving from the other
/// side: rows for saves that have been deleted from the card accumulate, and a
/// baseline over `state::kMaxRecords` is one `SaveBaseline` refuses **whole**.
/// Nothing is then written, and the library is re-hashed on that tick and every
/// tick after it, silently.
///
/// Only as far as the bound, because a row nothing mentioned is *usually* a
/// deleted save and is also what one unreadable folder produces -- and a save
/// dropped from the baseline over a folder that failed to list once costs that
/// save a re-hash. Keeping them until keeping them costs the file is the trade
/// that loses least in both directions.
void TrimToFit(const SyncPlan& plan, const std::map<Key, const ClientSaveState*>& local,
               BaselineUpdate* update) {
  // Counted before `SaveBaseline` has skipped the rows it cannot read back,
  // where that function counts after -- so a baseline of 520 rows, 15 of them
  // unusable, is trimmed here although it would have fitted there. The
  // asymmetry is deliberate rather than unnoticed: which rows the writer will
  // skip is the writer's own predicate and is not exported, and erring towards
  // trimming costs a handful of saves one re-hash while erring the other way
  // costs the whole file (see the header).
  if (update->value.size() <= state::kMaxRecords) {
    return;
  }
  // Built here rather than beside `local`, because it is a second copy of every
  // slot string in the plan and the ordinary tick never needs it: a baseline
  // inside the bound is trimmed by nothing.
  std::set<Key> planned;
  for (const SyncOperation& operation : plan.operations) {
    planned.insert(Key{operation.rom_id, operation.slot});
  }

  std::vector<Key> stale;
  for (const auto& row : update->value.rows()) {
    if (planned.count(row.first) == 0 && local.count(row.first) == 0) {
      stale.push_back(row.first);
    }
  }
  for (const Key& key : stale) {
    if (update->value.size() <= state::kMaxRecords) {
      break;
    }
    update->value.Erase(key.first, key.second);
    ++update->dropped;
    AddWarning(&update->warnings, Name(key.first, key.second) +
                               ": dropped from the baseline to keep it inside the " +
                               std::to_string(state::kMaxRecords) +
                               " rows a baseline can be read back with; this save was neither "
                               "reported nor planned this tick");
  }
  if (update->value.size() > state::kMaxRecords) {
    // Nothing left that this tick does not know about, so the file cannot be
    // made writable from here. `SaveBaseline` refuses it and says so, but its
    // message names a count and not the reason -- and the reason is that this
    // card has outgrown a bound that moves with the sysmodule's heap.
    AddWarning(&update->warnings,
        "this tick reported and planned " + std::to_string(update->value.size()) +
            " saves, more than the " + std::to_string(state::kMaxRecords) +
            " a baseline can be read back with, and none of the rows is stale enough to drop; "
            "the baseline cannot be written until scan::kMaxSaves, state::kMaxRecords and "
            "kInnerHeapSize are raised together");
  }
}

/// Set what the row records about the *server's* copy.
///
/// `record` arrives holding whatever the previous row knew, and this decides how
/// much of that survives. Which side may erase differs by direction, and that is
/// the whole reason this is not one assignment:
///
///   - An **upload** put this client's bytes on the server, so the digest is the
///     one that went up. Its new `updated_at` is the server's own clock and is
///     in no response this client reads (`OperationResult` carries the save id
///     and nothing else), so it is *cleared* rather than guessed from the local
///     mtime -- and cleared rather than inherited, because the stored one
///     describes the copy the upload just replaced. An absent timestamp costs
///     the next arbitration a fallback; a wrong one costs it the answer.
///   - A **download** or a keep-both took the copy the plan described, so what
///     the plan could not supply is cleared too: the stored value describes an
///     older server copy than the bytes now on the card.
///   - A **no_op** moved neither side, so everything it does not restate is
///     still true and is kept.
void ApplyServerHalf(const SyncOperation& operation, const OperationResult& done,
                     const std::map<Key, const ClientSaveState*>& local,
                     state::SaveRecord* record, std::vector<std::string>* warnings) {
  if (done.outcome == OperationOutcome::kUploaded) {
    const auto save = local.find(Key{done.rom_id, done.slot});
    record->server_content_hash = std::nullopt;
    // Held to the same shape the download branch holds the server's digest to.
    // `state::Usable` refuses a whole row over an unusable `server_content_hash`,
    // so copying an uppercase or a SHA1 one in here would cost this save its
    // *local* half as well and re-hash it on every tick -- the silent failure
    // this module is written against. Nothing in this signature promises
    // `reported` came through `sync::Validate`.
    if (save != local.end() && save->second->content_hash.has_value() &&
        IsContentHash(*save->second->content_hash)) {
      record->server_content_hash = save->second->content_hash;
    }
    record->server_updated_at = std::nullopt;
    return;
  }

  const bool keep_unstated = done.outcome == OperationOutcome::kNoOp;
  if (operation.server_content_hash.has_value() &&
      IsContentHash(*operation.server_content_hash)) {
    record->server_content_hash = operation.server_content_hash;
  } else if (!keep_unstated) {
    record->server_content_hash = std::nullopt;
  }

  if (!operation.server_updated_at.has_value()) {
    if (!keep_unstated) {
      record->server_updated_at = std::nullopt;
    }
    return;
  }
  // Carried as text on the plan because nothing there compares it; a row stores
  // seconds, so this is where it has to become one.
  //
  // A spelling this build cannot read never fails the row -- the local half is
  // what saves the re-hash and is still good either way -- but it does not leave
  // the field alone either, and which of the two happens is the same split the
  // rest of this function makes. A `no_op` restated nothing it could read, so
  // whatever the last sync stored is still true and stays. A download or a
  // keep-both replaced the bytes on the card, so a stored value now describes an
  // older server copy than the save does and is cleared rather than left to be
  // read as current.
  const std::optional<Timestamp> when = ParseTimestamp(*operation.server_updated_at);
  if (when.has_value()) {
    record->server_updated_at = when;
    return;
  }
  if (!keep_unstated) {
    record->server_updated_at = std::nullopt;
  }
  AddWarning(warnings,
             Name(done.rom_id, done.slot) + ": the server's updated_at \"" +
                 *operation.server_updated_at +
                 "\" is not a timestamp this build can read; the row " +
                 (keep_unstated ? "keeps whatever the last sync stored"
                                : "is stored without a server timestamp"));
}

/// Whether this outcome earned its save a new row.
///
/// The four in `sync_execute.hpp`'s list and no others: `kFailed` and
/// `kNotUnderstood` keep the previous row so the next tick retries, and
/// `kCanceled` did not happen at all.
bool Advances(OperationOutcome outcome) {
  return outcome == OperationOutcome::kUploaded || outcome == OperationOutcome::kDownloaded ||
         outcome == OperationOutcome::kKeptBoth || outcome == OperationOutcome::kNoOp;
}

/// Whether this outcome replaced the bytes on the card.
bool Rewrites(OperationOutcome outcome) {
  return outcome == OperationOutcome::kDownloaded || outcome == OperationOutcome::kKeptBoth;
}

}  // namespace

CompletionCounts CountsFor(const ExecutionReport& report) {
  CompletionCounts counts;
  counts.operations_completed = report.completed;
  // `not_understood` goes here rather than nowhere. It is work the server
  // planned and this client did not do, and the only two fields `complete`
  // has are "did it" and "did not" -- so the one that is true is this one.
  // Folding it into `completed` would tell RomM the client did something it
  // does not even know the name of (docs/SYNC_PROTOCOL.md step 3).
  counts.operations_failed = report.failed + report.not_understood;
  return counts;
}

BaselineUpdate AdvanceBaseline(state::Baseline previous, const SyncPlan& plan,
                               const ExecutionReport& report,
                               const std::vector<ClientSaveState>& reported,
                               fs::FileSystem& files) {
  BaselineUpdate update;
  // Moved, not copied. `kMaxStateBytes` of rows costs more parsed than it does
  // as text (state_db.hpp), and holding the old baseline and the new one at once
  // -- on the tick that also holds the plan, the report and a directory listing
  // -- is a peak the 512 KiB inner heap does not have to spare.
  update.value = std::move(previous);

  // What the tick told the server about each local save. Still exactly right
  // for every save this tick did not rewrite -- including, deliberately, the
  // ones it failed to upload: nothing touched those files.
  std::map<Key, const ClientSaveState*> local;
  for (const ClientSaveState& save : reported) {
    local[Key{save.rom_id, save.slot}] = &save;
  }

  Directories directories(files);

  for (std::size_t index = 0; index < report.operations.size(); ++index) {
    const OperationResult& done = report.operations[index];
    if (!Advances(done.outcome)) {
      continue;
    }
    // `ExecutePlan` returns one result per operation, in the plan's order. The
    // server's copy is only in the plan, so the two have to be paired -- and a
    // pairing that is wrong would put one save's server digest on another save's
    // row, which is a row that never matches and never says why.
    if (index >= plan.operations.size()) {
      AddWarning(&update.warnings, Name(done.rom_id, done.slot) +
                                ": the report holds more operations than the plan; its baseline "
                                "was left as it was");
      continue;
    }
    const SyncOperation& operation = plan.operations[index];
    if (operation.rom_id != done.rom_id || operation.slot != done.slot) {
      AddWarning(&update.warnings, Name(done.rom_id, done.slot) +
                                ": does not pair with " + Name(operation.rom_id, operation.slot) +
                                " at the same position in the plan; its baseline was left as it "
                                "was");
      continue;
    }

    // Started from the row that is there rather than from nothing. A `no_op`
    // moved neither side, so anything the last sync recorded about the server's
    // copy is still true -- and a fresh record would quietly drop it the first
    // time the plan happened not to carry a field.
    state::SaveRecord record;
    if (const state::SaveRecord* stored = update.value.Find(done.rom_id, done.slot);
        stored != nullptr) {
      record = *stored;
    }
    record.rom_id = done.rom_id;
    record.slot = done.slot;

    ApplyServerHalf(operation, done, local, &record, &update.warnings);

    if (Rewrites(done.outcome)) {
      LocalFacts facts;
      std::string why;
      if (!ReadBack(files, directories, done.sd_path, &facts, &why)) {
        // **Erased, not merely skipped.** This operation replaced the bytes on
        // the card, so whatever row was there describes bytes that are gone --
        // its digest, its mtime and its size all belong to the copy this tick
        // overwrote. Leaving it would be storing a row that is known to be
        // false; that it is also unusable (`ContentHashFor` needs the mtime and
        // the size to match) is another module's doing and not something this
        // one should lean on.
        update.value.Erase(done.rom_id, done.slot);
        AddWarning(&update.warnings, Name(done.rom_id, done.slot) +
                                         ": its new bytes could not be read back (" + why +
                                         "); it gets no row, and is hashed again next tick");
        continue;
      }
      record.content_hash = facts.content_hash;
      record.mtime = facts.mtime;
      record.file_size_bytes = facts.file_size_bytes;
    } else {
      const auto save = local.find(Key{done.rom_id, done.slot});
      if (save == local.end()) {
        AddWarning(&update.warnings, Name(done.rom_id, done.slot) +
                                  ": was not among the saves this tick reported, so there is "
                                  "nothing to record about the local file");
        continue;
      }
      record.content_hash = save->second->content_hash.value_or(std::string());
      record.mtime = save->second->updated_at;
      record.file_size_bytes = save->second->file_size_bytes;
    }

    // No digest, no new row -- and here the previous one is **kept**, which is
    // the opposite of the `ReadBack` failure above and for the opposite reason.
    // Nothing on the card changed: an upload and a no-op both leave the file
    // alone, so a row the last tick stored still describes it, and erasing it
    // would cost this save a re-hash it has not earned. What cannot be written
    // is a row carrying this tick's *missing* digest -- `SaveBaseline` refuses
    // an empty one, and the scanner could never reuse it.
    if (!IsContentHash(record.content_hash)) {
      AddWarning(&update.warnings,
                 Name(done.rom_id, done.slot) +
                     ": has no usable content_hash, so its row was not moved forward; the one the "
                     "last tick stored still stands");
      continue;
    }

    update.value.Set(std::move(record));
    ++update.advanced;
  }

  TrimToFit(plan, local, &update);
  return update;
}

TickCompletion FinishTick(http::HttpClient& client, fs::FileSystem& files,
                          const auth::StoredToken& token, const SyncPlan& plan,
                          const ExecutionReport& report,
                          const std::vector<ClientSaveState>& reported,
                          state::Baseline previous, const FinishOptions& options) {
  TickCompletion tick;

  BaselineUpdate update =
      AdvanceBaseline(std::move(previous), plan, report, reported, files);
  tick.rows_advanced = update.advanced;
  tick.rows_dropped = update.dropped;
  tick.warnings = std::move(update.warnings);

  // First, and unconditionally. The transfers already landed on the server; the
  // thing that is only here is the client's record of what it hashed, and losing
  // that re-uploads saves RomM already has on the next tick.
  const std::string path = files.Resolve(options.state_sd_path);
  if (path.empty()) {
    tick.stored.error = state::StoreError::kOpenFailed;
    tick.stored.message = options.state_sd_path + " is not a path on this card";
  } else {
    tick.stored = state::SaveBaseline(path, update.value);
  }
  // The writer skips a row it could not read back and writes the rest, which is
  // deliberately not an error -- so a caller reading only `ok()` would never
  // learn a save lost its row. They are the same kind of line as the ones above
  // and go in the same place, rather than being left in a second list nobody
  // remembers to read.
  for (const std::string& skipped : tick.stored.skipped) {
    AddWarning(&tick.warnings, "the baseline left a row out: " + skipped);
  }

  tick.counts = CountsFor(report);
  if (report.canceled && options.complete.cancel == nullptr) {
    // The one combination that is almost certainly a caller mistake: the tick
    // was cancelled, and the token that would end this call was not passed on.
    // The call still happens -- the counts are honest and the session is worth
    // closing -- but it can now cost three timeouts and the backoff between
    // them, on the link whose loss is usually why the shutdown happened.
    AddWarning(&tick.warnings,
               "this tick was cancelled but FinishOptions::complete.cancel is null, so the "
               "completion cannot be stopped; pass the same token ExecuteOptions::cancel had");
  }
  tick.reported = CompleteSession(client, token, plan.session_id, tick.counts, options.complete);
  return tick;
}

}  // namespace rommsync::sync
