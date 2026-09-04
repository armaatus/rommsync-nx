// Small files that are written completely or not at all.
//
// `token.dat` (token_store.hpp) and `device.dat` (device_identity.hpp) are both
// records the client cannot re-derive on its own: losing one costs a human at a
// browser, losing the other duplicates the console in RomM's device list. Both
// are written the same way, and the way is not obvious enough to have two
// copies of -- see `WriteAtomically` for why the commit is two renames rather
// than one.
//
// Nothing here is for a *large* file. Downloads stream to a `.part` file
// through `http::DownloadTarget`; this reads and writes whole strings.
#pragma once

#include <string>
#include <string_view>

namespace rommsync::io {

/// Why a write did not complete. Every value leaves the destination untouched.
enum class WriteError {
  kNone,
  kOpenFailed,    ///< the temp file could not be created -- usually a missing directory
  kWriteFailed,   ///< the bytes did not all reach the disk
  kCommitFailed,  ///< the rename onto the destination failed
};

/// Stable, log-friendly name. Never null.
const char* ToString(WriteError error);

struct WriteResult {
  WriteError error = WriteError::kNone;

  /// For logs. Names the path and what went wrong, never the contents -- these
  /// files hold bearer tokens.
  std::string message;

  bool ok() const { return error == WriteError::kNone; }
};

/// Why a read did not produce contents.
enum class ReadError {
  kNone,
  kMissing,     ///< it could not be opened: no such file, or nothing may open it
  kUnreadable,  ///< it opened, and the bytes could not be got out of it
};

/// Stable, log-friendly name. Never null.
const char* ToString(ReadError error);

struct ReadResult {
  std::string contents;
  ReadError error = ReadError::kNone;
  std::string message;

  bool ok() const { return error == ReadError::kNone; }
};

/// Where a write stages the new contents, and where it parks the old ones.
///
/// Public because a caller that recovers from an interrupted commit has to name
/// them, and because a test that proves nothing is left behind has to look.
std::string TempPathFor(std::string_view path);
std::string PreviousPathFor(std::string_view path);

/// Write `contents` to `path` so that no reader ever sees half of it.
///
/// The bytes go to `TempPathFor(path)`, are flushed and closed, and only then
/// renamed onto `path`. Every failure before that rename leaves whatever `path`
/// already held completely intact, which is the guarantee that matters: a
/// failed write costs the *new* record, never the working one.
///
/// **The commit is two renames, because Horizon's is not a replace.** POSIX
/// `rename` replaces the destination atomically; `fsFsRenameFile`, which
/// libnx's `fsdev` maps it to, refuses a destination that already exists -- and
/// on a re-pair the destination always exists. So a record already in place is
/// moved to `PreviousPathFor(path)` first, on *both* platforms deliberately: a
/// fallback taken only on the console is a path no host test ever runs, and the
/// v1 gate would be the first thing to see it. The cost is one moment where
/// `path` does not exist and the previous record is under the other name, which
/// is what `ReadCommitted` is for.
///
/// The directory must already exist -- creating `sdmc:/config/rommsync/` is the
/// platform layer's job, not something the portable engine can do with only
/// standard headers. A missing one is `kOpenFailed`, named.
///
/// What `rename` gives is atomicity, not durability: surviving a power cut
/// needs an `fsync` the C++ standard library does not expose. The promise here
/// is that no reader ever sees a partial record.
WriteResult WriteAtomically(const std::string& path, std::string_view contents);

/// Read a whole file. A file that could not be opened is `kMissing`, which is a
/// different thing from one that opened and then failed to read -- only the
/// first is consistent with "nothing was ever written here", and only the first
/// is safe to answer by creating something new. A read that fails half way
/// yields no contents at all rather than the prefix that did arrive.
ReadResult ReadFile(const std::string& path);

/// The same, falling back to `PreviousPathFor(path)`.
///
/// The one moment `path` does not exist is between `WriteAtomically`'s two
/// renames, and what is sitting under the other name then is the previous
/// complete record. Reading it is the difference between an interruption nobody
/// notices and one that costs the user a re-pair. A missing file with nothing
/// to fall back to still reports itself as missing.
ReadResult ReadCommitted(const std::string& path);

/// Remove `path` and the `.tmp`/`.old` an interrupted commit can leave beside
/// it, overwriting each with zeroes first.
///
/// The overwrite is worth exactly what it is worth: it removes the bytes any
/// reader of the file system can see, and on a wear-levelling SD card it does
/// not promise the old sectors are gone (docs/SECURITY.md). It is here because
/// "Re-pair" has to genuinely discard the old token, and unlinking `token.dat`
/// while `token.dat.old` still holds the same bearer token would not.
///
/// True when nothing of the three is left, including when there was nothing to
/// begin with.
bool Shred(const std::string& path);

}  // namespace rommsync::io
