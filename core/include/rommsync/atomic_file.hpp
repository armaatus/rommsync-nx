// Small files that are written completely or not at all.
//
// `token.dat` (token_store.hpp) and `device.dat` (device_identity.hpp) are both
// records the client cannot re-derive on its own: losing one costs a human at a
// browser, losing the other duplicates the console in RomM's device list. Both
// are written the same way, and the way is not obvious enough to have two
// copies of -- see `WriteAtomically` for why the commit is two renames rather
// than one.
//
// `WriteAtomically` is not for a *large* file: it takes the contents as a
// `string_view`, so the whole record is in memory. `CopyAtomically` is the same
// guarantee for a file that will not fit -- a save state is tens of megabytes
// and the sysmodule's inner heap is 512 KiB -- and it streams. Downloads stage
// through `http::DownloadTarget`'s own `.part` file and land with
// `CommitStaged`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rommsync::io {

/// The platform's "these bytes are on the card", not merely in a buffer.
///
/// `rename` gives atomicity; it gives nothing about durability, and neither
/// `fsync` nor Horizon's `fsFsCommit` is reachable from `core/`, which has only
/// standard headers (core/AGENTS.md). So the platform layer installs one of
/// these and the two writers below call it on the staged file -- after it is
/// closed and *before* the rename that publishes it.
///
/// **What it buys is hard rule 2 on a console that loses power.**
/// `CopyAtomically` is how a save's previous bytes reach `.backup/` before the
/// save is overwritten. Without this the rename can be durable while the bytes
/// it published are not, leaving a backup that reads as present and holds
/// nothing -- which is exactly the state the backup exists to rule out
/// (docs/SYNC_PROTOCOL.md#backups).
///
/// It takes the **path** rather than an open handle because the two platforms
/// do not agree on what durability is a property of: the host syncs one file
/// descriptor, and Horizon commits the whole `sdmc:` filesystem
/// (`fsdevCommitDevice`; devkitA64's newlib exports no `fsync` at all). A hook
/// that ignores the path and commits everything is a correct implementation of
/// this contract.
///
/// Returning false fails the write. That is the right direction for the caller
/// this exists for: bytes that could not be put on the card are not a backup,
/// and a backup that did not happen must stop the overwrite rather than be
/// counted as one.
///
/// Null -- the default -- keeps the older, weaker promise: no reader ever sees
/// half a file. Nothing here fails for want of a hook.
using FileSync = bool (*)(const std::string& path);

/// Install the hook, process-wide.
///
/// Meant to be called once from `main` before the engine's threads exist:
/// `rommsync::host::InstallPosixFileSync()` on a laptop, the Horizon equivalent
/// in `sysmodule/source/main.cpp`. Stored in an atomic so a late call cannot be
/// a data race, not so that swapping it mid-write is a supported thing to do.
void SetFileSync(FileSync sync);

/// The installed hook, or null. For a test that wants to put its own back.
FileSync GetFileSync();

/// Why a write did not complete.
///
/// `kOpenFailed` and `kWriteFailed` leave the destination exactly as it was --
/// that is the guarantee `WriteAtomically` exists for. `kCommitFailed` is the
/// one that can cost something: the commit moves the record already in place
/// aside first, and if putting it back fails too, the destination is gone and
/// the previous record is under `PreviousPathFor(path)`. The `message` says so
/// when it happens, and a reader that consults the previous name -- as both
/// callers here do -- finds it.
enum class WriteError {
  kNone,
  kOpenFailed,    ///< the temp file could not be created -- usually a missing directory
  kWriteFailed,   ///< the bytes did not all reach the disk
  kCommitFailed,  ///< the rename onto the destination failed; see above
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
  kMissing,     ///< there is no such file (ENOENT/ENOTDIR), and nothing was ever written
  kUnreadable,  ///< it exists, and the bytes could not be got out of it
};

/// Stable, log-friendly name. Never null.
const char* ToString(ReadError error);

struct ReadResult {
  std::string contents;
  ReadError error = ReadError::kNone;
  std::string message;

  bool ok() const { return error == ReadError::kNone; }
};

/// True when `path` can be opened for reading.
///
/// What it is: the question a caller asks before naming a file it is about to
/// create -- is this backup name already taken, is there a save here to
/// preserve at all. What it is *not*: a lock. Nothing stops the answer from
/// changing before the next call, and no caller here needs it to; on a console
/// the engine is the only writer of these paths.
///
/// A file that exists and refuses to open reads as absent, which is the one way
/// this differs from `ReadFile`'s careful `kMissing`/`kUnreadable` split. Every
/// caller of this one goes on to open the file and gets the honest answer then.
bool Exists(const std::string& path);

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
/// What `rename` gives is atomicity; durability is `FileSync`'s, and only when
/// the platform layer has installed one. With no hook the promise here is the
/// weaker one -- no reader ever sees a partial record -- and with one the bytes
/// are on the card before the rename publishes them. A hook that refuses is
/// `kWriteFailed`, and the destination is untouched.
WriteResult WriteAtomically(const std::string& path, std::string_view contents);

/// Move a file that is already staged on disk onto `path`, atomically.
///
/// `WriteAtomically`'s commit, on its own, for the bytes it cannot hold: a save
/// arrives by download rather than as a string, and `http::DownloadTarget` has
/// already put a complete copy of it somewhere. The rename dance is the same
/// one and the reasoning behind it is the same one -- Horizon's rename refuses
/// an existing destination, so a file already at `path` is moved to
/// `PreviousPathFor(path)` first and put back if the second rename fails.
///
/// `staged` is consumed: it is renamed away on success and removed on failure,
/// so a caller never has to decide what to do with a half-committed pair. That
/// matches `WriteAtomically`, whose temp file does not survive a failed commit
/// either -- a failure costs the new contents and never the old ones.
///
/// `kOpenFailed` when there is nothing at `staged`; every other outcome is a
/// commit that did not happen, named the way `WriteAtomically` names it.
WriteResult CommitStaged(const std::string& staged, const std::string& path);

/// Why a copy did not complete.
///
/// The source outcomes are split from the destination ones because the caller
/// this exists for -- backing a save up before overwriting it -- reads them
/// differently: `kSourceMissing` means there was nothing at that path to
/// protect and the overwrite may go ahead, while every other value means the
/// previous bytes are *not* safe and it may not.
enum class CopyError {
  kNone,
  kSourceMissing,     ///< there is no file at `from` (ENOENT/ENOTDIR)
  kSourceUnreadable,  ///< there is, and the bytes could not be got out of it
  kOpenFailed,        ///< the temp file could not be created -- usually a missing directory
  kWriteFailed,       ///< the bytes did not all reach the disk
  kCommitFailed,      ///< the rename onto the destination failed
};

/// Stable, log-friendly name. Never null.
const char* ToString(CopyError error);

struct CopyResult {
  CopyError error = CopyError::kNone;

  /// For logs. Names both paths and what went wrong, never the contents.
  std::string message;

  /// Bytes that reached the destination. Zero on every failure.
  std::uint64_t bytes_copied = 0;

  bool ok() const { return error == CopyError::kNone; }
};

/// Copy `from` onto `to` so that no reader ever sees a partial copy.
///
/// Streamed through a 4 KiB stack chunk rather than read whole, which is the
/// difference between this and `WriteAtomically(to, ReadFile(from).contents)`:
/// the file being copied is a *save*, the sysmodule's inner heap is 512 KiB,
/// and buffering a save state would be a `bad_alloc` on the console and a green
/// test on a laptop (core/AGENTS.md).
///
/// The bytes go to `TempPathFor(to)` and are committed with `CommitStaged`, so
/// a failure part way through leaves whatever was at `to` exactly as it was.
/// The directory must already exist, for the reason `WriteAtomically` gives.
///
/// It is a copy and not a move: the point of the caller is that both files
/// exist afterwards.
///
/// **Durability is `FileSync`'s**, and worth restating here because of what this
/// one copies. Without a hook installed, a card yanked between the copy and the
/// overwrite can leave the rename durable and the copied bytes not -- a backup
/// that reads as present and holds nothing, which is the one state hard rule 2
/// is supposed to rule out. With one, the bytes are on the card before the
/// rename, and a refusal to put them there is `kWriteFailed` rather than a
/// backup that did not happen being counted as one
/// (docs/SYNC_PROTOCOL.md#backups).
CopyResult CopyAtomically(const std::string& from, const std::string& to);

/// Read a whole file.
///
/// **`kMissing` means ENOENT, and nothing else.** A file that exists and refuses
/// to open -- a full handle table, `sdmc:` not mounted yet, a permission -- is
/// `kUnreadable`, because a caller answers "there is nothing here" by creating
/// something, and creating a `device.dat` over an identifier that was merely
/// unreachable for a moment registers the console in RomM a second time. Only
/// the first is safe to answer by writing.
///
/// A read that fails half way yields no contents at all rather than the prefix
/// that did arrive: half a record read as a whole one is the failure this
/// module exists to rule out.
ReadResult ReadFile(const std::string& path);

/// Why a *bounded* read did not produce contents. `ReadError` plus the one
/// outcome only a bound has.
enum class BoundedRead {
  kOk,
  kMissing,     ///< there is no such file (ENOENT/ENOTDIR)
  kUnreadable,  ///< it exists, and the bytes could not be got out of it
  kTooLarge,    ///< it is bigger than the caller said it could be; nothing is returned
};

/// Stable, log-friendly name. Never null.
const char* ToString(BoundedRead outcome);

/// Read at most `limit` bytes of `path`, or nothing.
///
/// `ReadFile` deliberately reads whatever is there, which is right for
/// `token.dat` and `device.dat` -- records this client wrote itself, whose size
/// it therefore knows. `config.ini` is a file a human and a card reader both get
/// to touch, and `state.db` is one a yanked card can leave pointing at anything,
/// so those stop instead: a corrupt FAT32 directory entry claiming four
/// gigabytes has to be a named refusal, not a `bad_alloc` before `main` gets
/// anywhere. One byte past the bound is enough to tell "at the limit" from "over
/// it" without holding the rest, so `out` is left empty on `kTooLarge`.
///
/// `kMissing` draws the same line `ReadFile` does, for the same reason: only
/// ENOENT and ENOTDIR mean nothing was ever written here.
BoundedRead ReadBounded(const std::string& path, std::size_t limit, std::string* out);

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
