// `sdmc:/config/rommsync/state.db` -- what the last sync left behind.
//
// One row per `(rom_id, slot)`: the digest and mtime the client last reported,
// and the ones the server last held. Two things read it. The scanner asks "is
// this file still the one I hashed?" and skips the hashing when the mtime and
// size both match, which is what keeps a tick cheap on a large library. M2-8's
// conflict detection asks what each side looked like at the last sync, which is
// how "both sides changed" is told from "only one did"
// (docs/SYNC_PROTOCOL.md#change-detection-between-ticks).
//
// **It is an optimisation, never a gate.** A `state.db` that is missing,
// truncated or garbage yields an empty baseline and a diagnostic, and the tick
// proceeds by hashing everything -- the server arbitrates either way, so a lost
// baseline costs time and never correctness. That is why nothing here refuses:
// the one thing it must not do is stop a sync.
//
// **Skipping the hash is not skipping the report.** A file whose mtime and size
// still match its row is not re-read, and the *stored* `content_hash` still goes
// into the payload. A save reported without one is a save the server compares on
// timestamps alone, which plans an upload for bytes it already has -- forever
// (sync.hpp).
//
// ## The format
//
// docs/ARCHITECTURE.md used to leave this as "small SQLite or a flat file". It
// is a flat, line-oriented file, because `core/` may include only standard and
// `rommsync/` headers (core/AGENTS.md) so SQLite cannot be linked into the
// portable engine at all, and because a sysmodule heap has no room for one.
//
//     rommsync-state 1
//     {"rom_id":12,"slot":"autosave","content_hash":"...","mtime":1757000000,...}
//     {"rom_id":12,"slot":null,...}
//
// A version line, then one JSON object per row. JSON per line rather than a
// separator-delimited record because a save's `slot` and `file_name` are user
// data that can hold any byte a filesystem allows, and `json::Quote` already
// escapes them exactly once -- inventing a second escaping scheme here is how a
// slot with a tab in it becomes two columns. The rows are read with
// `json::Reader`, the same strict reader every server response goes through, so
// a field of the wrong type is a named diagnostic rather than a default value
// that silently means "unchanged".
//
// Timestamps are whole Unix seconds, not RFC 3339: the engine can *format* that
// (`sync::FormatTimestamp`) and has no parser for it, and the comparison the
// server makes is at second granularity anyway.
//
// The write is `io::WriteAtomically` and the read recovers from
// `io::PreviousPathFor`, the same commit and the same recovery `token.dat`,
// `device.dat` and `config.ini` use -- see atomic_file.hpp for why the commit is
// two renames.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/sync.hpp"

namespace rommsync::state {

/// The file the baseline lives in, relative to `sdmc:/config/rommsync/`.
inline constexpr const char* kStateFileName = "state.db";

/// The first line of a well-formed file, `<magic> <version>`.
inline constexpr const char* kFormatMagic = "rommsync-state";
inline constexpr int kFormatVersion = 1;

/// The largest `state.db` that will be read, and the most rows it may hold.
///
/// The same reasoning as `config::kMaxConfigBytes`: this runs on a sysmodule
/// heap measured in megabytes, from a FAT32 card that gets yanked mid-write, so
/// a corrupt directory entry pointing at four gigabytes of garbage has to be a
/// named refusal rather than a `bad_alloc`. A row is a couple of hundred bytes
/// and a large library has a few thousand saves, so neither bound is reachable
/// by a file this client wrote.
inline constexpr std::size_t kMaxStateBytes = 1024 * 1024;
inline constexpr std::size_t kMaxRecords = 4096;

/// How many diagnostics are kept before the rest are dropped. `config`'s
/// reasoning: a corrupt file must not answer with fifty thousand strings.
inline constexpr std::size_t kMaxDiagnostics = 32;

/// What one save looked like at the end of the last sync.
struct SaveRecord {
  /// The rom this save belongs to. Positive, like `ClientSaveState::rom_id`.
  std::int64_t rom_id = 0;

  /// The pairing key with `rom_id`. `null` is archival/manual-upload, and is a
  /// different row from any slotted one -- exactly as the server pairs them
  /// (sync.hpp). An empty string is neither and is refused.
  std::optional<std::string> slot;

  /// The MD5 the client last reported: 32 lowercase hex digits
  /// (`sync::kContentHashDigits`). Never empty in a stored row -- a row with no
  /// digest is a row that saves no hashing, so it is not worth writing.
  std::string content_hash;

  /// The local file's mtime when that digest was taken, truncated to whole
  /// seconds. Compared against the file's current mtime, at the same
  /// truncation, to decide whether the digest still describes it.
  sync::Timestamp mtime{};

  /// Its size then, in bytes. Checked *with* the mtime rather than instead of
  /// it: a save rewritten within the same second by an emulator that restores
  /// mtimes is caught by the size, and a same-size edit is caught by the mtime.
  std::int64_t file_size_bytes = 0;

  /// What the server last said about its own copy. Absent until a sync has
  /// completed for this save, which is the "no sync history" branch M2-8
  /// arbitrates differently (docs/SYNC_PROTOCOL.md).
  std::optional<sync::Timestamp> server_updated_at;
  std::optional<std::string> server_content_hash;
};

/// The rows, keyed the way the server pairs saves.
///
/// The key is `(rom_id, slot)` and `std::optional`'s ordering puts the null-slot
/// row first, which only matters in that the file comes out in a stable order:
/// a baseline rewritten with nothing changed produces byte-identical contents.
class Baseline {
 public:
  using Key = std::pair<std::int64_t, std::optional<std::string>>;

  /// The row for this save, or nullptr when there is none -- which is the
  /// signal to hash the file, not a reason to guess.
  const SaveRecord* Find(std::int64_t rom_id, const std::optional<std::string>& slot) const;

  /// Insert or replace the row for `record`'s key.
  void Set(SaveRecord record);

  /// Drop the row for a save that is no longer on the card. Returns whether
  /// there was one.
  bool Erase(std::int64_t rom_id, const std::optional<std::string>& slot);

  const std::map<Key, SaveRecord>& rows() const { return rows_; }
  std::size_t size() const { return rows_.size(); }
  bool empty() const { return rows_.empty(); }

 private:
  std::map<Key, SaveRecord> rows_;
};

/// A baseline and everything wrong with the file it came from.
///
/// `value` is always usable: empty is a perfectly good baseline, and it is what
/// every failure here produces. See the header note -- nothing in this module
/// refuses.
struct LoadedBaseline {
  Baseline value;

  /// In the order they were found, bounded by `kMaxDiagnostics`. A first boot
  /// produces exactly one, saying there is no file yet.
  std::vector<std::string> diagnostics;

  /// One line per diagnostic, for a log. Empty when there are none.
  std::string DescribeDiagnostics() const;
};

/// The whole file, version line included, ending in a newline.
std::string SerializeBaseline(const Baseline& baseline);

/// Parse the contents of a `state.db`.
///
/// Pure: no filesystem, no clock. **A malformed file yields an empty baseline,
/// not the rows that happened to parse.** A truncation leaves a prefix that is
/// individually well-formed and collectively a lie -- rows written before the
/// interruption, none of the ones after -- and "some rows survived" is a state
/// no caller can reason about. Since dropping the lot costs only a tick of
/// hashing, that is the cheaper mistake to make.
LoadedBaseline ParseBaseline(std::string_view text);

/// Read `path` and parse it.
///
/// A *missing* file falls back to `io::PreviousPathFor(path)` first, the same
/// recovery `token_store`, `device_identity` and `config` make: the one moment
/// `state.db` legitimately does not exist is the window between
/// `io::WriteAtomically`'s two renames, and the previous baseline is sitting
/// intact under `state.db.old`. A file that exists and will not open does not
/// take it -- that is a bad moment, not a commit window -- and yields an empty
/// baseline, which is safe here in a way it would not be for `device.dat`.
LoadedBaseline LoadBaseline(const std::string& path);

/// Why writing the baseline did not work. Mirrors `auth::StoreError`.
enum class StoreError {
  kNone,
  kUnusableRecord,  ///< a row would not have been readable back; nothing was written
  kOpenFailed,      ///< the temp file could not be created -- usually a missing directory
  kWriteFailed,     ///< the bytes did not all reach the disk; the destination is untouched
  kCommitFailed,    ///< the rename onto `path` failed; see atomic_file.hpp
};

/// Stable, log-friendly name. Never null.
const char* ToString(StoreError error);

struct StoreResult {
  StoreError error = StoreError::kNone;

  /// For logs. Names the path and what went wrong.
  std::string message;

  bool ok() const { return error == StoreError::kNone; }
};

/// Write `baseline` to `path`, atomically.
///
/// A row that could not be read back -- a non-positive `rom_id`, an empty slot,
/// a digest that is not 32 lowercase hex -- is refused before anything is
/// written, on `token_store`'s reasoning: a file that exists and cannot be
/// parsed is worse than no file, because the next boot finds one, discards all
/// of it, and re-hashes the library for the life of the bug.
///
/// Writing the baseline at the end of a tick is M2-6's; this owns the format.
StoreResult SaveBaseline(const std::string& path, const Baseline& baseline);

/// Why a file could not be hashed.
enum class HashError {
  kNone,
  kUnreadable,  ///< it is not there, or the bytes could not be got out of it
};

/// Stable, log-friendly name. Never null.
const char* ToString(HashError error);

/// A save's `content_hash`, and whether the file had to be read for it.
struct HashOutcome {
  /// 32 lowercase hex digits, empty on failure. Goes into
  /// `ClientSaveState::content_hash` whether it was computed or reused.
  std::string content_hash;

  HashError error = HashError::kNone;

  /// The digest came from the baseline and the file was never opened. What
  /// "skip re-hashing an unchanged file" actually means, and the only way a
  /// caller can tell -- the digest itself is identical either way.
  bool reused = false;

  /// For logs. Names the path and what went wrong.
  std::string message;

  bool ok() const { return error == HashError::kNone; }
};

/// Stream `path` through MD5.
///
/// Chunked rather than read whole: a save state is tens of megabytes and the
/// sysmodule heap is small (core/AGENTS.md). The chunk is 4 KiB and stays that
/// way -- `sys-rommsync.json` gives the main thread a 16 KiB stack, so a buffer
/// sized for a desktop is a stack overflow on the console.
///
/// Unlike `LoadBaseline` this has no size bound -- the file being hashed is the
/// user's save, and refusing to hash a large one would report it with no digest,
/// which is the failure the digest exists to prevent.
HashOutcome HashFile(const std::string& path);

/// The `content_hash` for one scanned save: the stored one when the file is
/// unchanged, a fresh `HashFile` otherwise.
///
/// Unchanged means the baseline has a row for `(rom_id, slot)` and both its
/// `mtime` -- at whole-second granularity, which is all the file holds -- and
/// its `file_size_bytes` match what the scanner just stat'd. Both, because
/// either one alone misses a real edit: an emulator that restores mtimes, and a
/// same-size overwrite within the same second.
HashOutcome ContentHashFor(const Baseline& baseline, std::int64_t rom_id,
                           const std::optional<std::string>& slot, const std::string& path,
                           sync::Timestamp mtime, std::int64_t file_size_bytes);

}  // namespace rommsync::state
