#include "rommsync/atomic_file.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace rommsync::io {
namespace {

/// The platform hook, or null. See `SetFileSync`: set once from `main`, and
/// atomic so that a late call cannot race the first write rather than because
/// swapping it mid-write is supported.
std::atomic<FileSync> g_file_sync{nullptr};

/// Put the closed file at `path` on the card, if the platform said how.
///
/// Called after the handle is closed and before the rename that publishes the
/// file: flushing and closing move the bytes from stdio to the OS, and this is
/// what moves them from the OS to the card. With no hook installed the answer is
/// "yes" and the promise stays the weaker one.
bool Synced(const std::string& path) {
  const FileSync hook = g_file_sync.load(std::memory_order_relaxed);
  return hook == nullptr || hook(path);
}

/// The directory `path` is in, or empty when the path names no directory.
///
/// For the second half of durability: `rename` publishes a *name*, and on POSIX
/// a name is not durable until the directory holding it is synced. Syncing the
/// file's bytes alone leaves the window `CopyAtomically` exists to close -- a
/// backup whose data landed and whose directory entry did not, followed by a
/// save overwrite in a *different* directory that did.
std::string DirectoryOf(const std::string& path) {
  const std::string::size_type slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return {};
  }
  // A path directly under the root keeps the slash: `/` is the directory, `` is
  // not a path at all.
  return slash == 0 ? std::string("/") : path.substr(0, slash);
}

constexpr const char* kTempSuffix = ".tmp";
constexpr const char* kPreviousSuffix = ".old";

std::string Describe(const std::string& path, std::string_view what) {
  return path + ": " + std::string(what);
}

/// Overwrite whatever is at `path` with zeroes, then unlink it.
///
/// Best-effort, and `Shred` says how little that is worth: without an `fsync`
/// the standard library does not expose, the zeroes may never reach the card at
/// all, since a filesystem is free to drop dirty pages belonging to an inode
/// that is about to be unlinked. The unlink is the part that does the work.
bool ShredOne(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "r+b");
  if (file != nullptr) {
    long size = 0;
    if (std::fseek(file, 0, SEEK_END) == 0) {
      size = std::ftell(file);
    }
    if (size > 0 && std::fseek(file, 0, SEEK_SET) == 0) {
      char zeroes[512] = {};
      for (long written = 0; written < size;) {
        const long chunk = size - written < static_cast<long>(sizeof(zeroes))
                               ? size - written
                               : static_cast<long>(sizeof(zeroes));
        if (std::fwrite(zeroes, 1, static_cast<std::size_t>(chunk), file) !=
            static_cast<std::size_t>(chunk)) {
          break;
        }
        written += chunk;
      }
      std::fflush(file);
    }
    std::fclose(file);
  }
  std::remove(path.c_str());
  return !Exists(path);
}

}  // namespace

void SetFileSync(FileSync sync) { g_file_sync.store(sync, std::memory_order_relaxed); }

FileSync GetFileSync() { return g_file_sync.load(std::memory_order_relaxed); }

const char* ToString(WriteError error) {
  switch (error) {
    case WriteError::kNone:
      return "none";
    case WriteError::kOpenFailed:
      return "open_failed";
    case WriteError::kWriteFailed:
      return "write_failed";
    case WriteError::kCommitFailed:
      return "commit_failed";
  }
  return "none";
}

const char* ToString(ReadError error) {
  switch (error) {
    case ReadError::kNone:
      return "none";
    case ReadError::kMissing:
      return "missing";
    case ReadError::kUnreadable:
      return "unreadable";
  }
  return "none";
}

bool Exists(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  std::fclose(file);
  return true;
}

std::string TempPathFor(std::string_view path) { return std::string(path) + kTempSuffix; }

std::string PreviousPathFor(std::string_view path) { return std::string(path) + kPreviousSuffix; }

WriteResult CommitStaged(const std::string& staged, const std::string& path) {
  if (!Exists(staged)) {
    return {WriteError::kOpenFailed, Describe(staged, "is not there to commit")};
  }

  const std::string previous = PreviousPathFor(path);
  const bool replacing = Exists(path);
  if (replacing) {
    // A stale one has to go first, because the rename below is the Horizon one
    // that refuses an existing destination. Only when there is something to
    // move aside, though: a `.old` left by an interrupted commit is the only
    // copy of the record when `path` is missing, and removing it before the new
    // one is committed would turn a recoverable interruption into a loss.
    std::remove(previous.c_str());
    if (std::rename(path.c_str(), previous.c_str()) != 0) {
      std::remove(staged.c_str());
      return {WriteError::kCommitFailed, Describe(path, "could not be moved aside to " + previous)};
    }
  }
  if (std::rename(staged.c_str(), path.c_str()) != 0) {
    std::string detail = "could not be replaced by " + staged;
    // Putting the previous record back is the whole reason a caller can treat a
    // failed write as costing only the new record. When even *that* fails the
    // destination is gone, and saying so is the difference between a caller
    // reading `.old` deliberately and one wondering why its file vanished.
    if (replacing && std::rename(previous.c_str(), path.c_str()) != 0) {
      detail += ", and the previous record could not be put back -- it is at " + previous;
    }
    std::remove(staged.c_str());
    return {WriteError::kCommitFailed, Describe(path, detail)};
  }
  std::remove(previous.c_str());

  // Best effort, and after the fact on purpose: the rename has happened, so
  // reporting a failure here would describe a commit that did not occur. What it
  // buys is the *name* being as durable as the bytes already are -- see
  // `DirectoryOf`. A platform whose hook commits the whole device (Horizon's
  // does) has already done this and pays for it twice, which is cheaper than the
  // alternative of a second hook nobody would remember to call.
  const std::string directory = DirectoryOf(path);
  if (!directory.empty()) {
    Synced(directory);
  }
  return {};
}

WriteResult WriteAtomically(const std::string& path, std::string_view contents) {
  const std::string temp = TempPathFor(path);

  std::FILE* file = std::fopen(temp.c_str(), "wb");
  if (file == nullptr) {
    return {WriteError::kOpenFailed,
            Describe(temp, "could not be created (does the directory exist?)")};
  }

  const std::size_t written = std::fwrite(contents.data(), 1, contents.size(), file);
  // The flush has to happen while the handle is still open: fclose reports a
  // failed flush too, but by then there is nothing left to distinguish "the
  // bytes never left the buffer" from "the close itself failed". The sync comes
  // after the close, by path, because that is the contract both platforms can
  // hold (`FileSync`).
  const bool flushed = written == contents.size() && std::fflush(file) == 0;
  const bool closed = std::fclose(file) == 0;
  // After the close and before the commit below, which is the only ordering
  // that means anything: the bytes have to be on the card before the rename
  // publishes them.
  const bool synced = flushed && closed && Synced(temp);
  if (!flushed || !closed || !synced) {
    std::remove(temp.c_str());
    return {WriteError::kWriteFailed, Describe(temp, "could not be written completely")};
  }

  // Everything above touched only the temp file, so a failure up to this point
  // leaves whatever `path` already held exactly as it was.
  return CommitStaged(temp, path);
}

const char* ToString(CopyError error) {
  switch (error) {
    case CopyError::kNone:
      return "none";
    case CopyError::kSourceMissing:
      return "source_missing";
    case CopyError::kSourceUnreadable:
      return "source_unreadable";
    case CopyError::kOpenFailed:
      return "open_failed";
    case CopyError::kWriteFailed:
      return "write_failed";
    case CopyError::kCommitFailed:
      return "commit_failed";
  }
  return "none";
}

CopyResult CopyAtomically(const std::string& from, const std::string& to) {
  CopyResult result;
  errno = 0;
  std::FILE* source = std::fopen(from.c_str(), "rb");
  if (source == nullptr) {
    // The same line `ReadFile` draws, and the caller this exists for reads it
    // the same way: only ENOENT and ENOTDIR mean there is nothing here to
    // preserve. A full handle table is a bad moment, and a bad moment is not
    // permission to overwrite a save without a backup.
    const int why = errno;
    const bool absent = why == ENOENT || why == ENOTDIR;
    result.error = absent ? CopyError::kSourceMissing : CopyError::kSourceUnreadable;
    result.message = Describe(from, absent ? "does not exist" : "could not be opened");
    return result;
  }

  const std::string temp = TempPathFor(to);
  std::FILE* destination = std::fopen(temp.c_str(), "wb");
  if (destination == nullptr) {
    std::fclose(source);
    result.error = CopyError::kOpenFailed;
    result.message = Describe(temp, "could not be created (does the directory exist?)");
    return result;
  }

  // 4 KiB, the convention in this directory: the main thread gets a 16 KiB
  // stack on the console, so a buffer sized for a desktop compiles here and
  // overflows there (core/AGENTS.md).
  char buffer[4096];
  std::uint64_t copied = 0;
  bool short_write = false;
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), source)) > 0) {
    if (std::fwrite(buffer, 1, got, destination) != got) {
      short_write = true;
      break;
    }
    copied += got;
  }
  const bool read_failed = std::ferror(source) != 0;
  std::fclose(source);
  const bool flushed = std::fflush(destination) == 0;
  const bool closed = std::fclose(destination) == 0;

  if (read_failed) {
    std::remove(temp.c_str());
    result.error = CopyError::kSourceUnreadable;
    result.message = Describe(from, "could not be read to the end");
    return result;
  }
  // Only once the bytes are known to be all there, and before the commit below
  // -- which is the whole point of this copy: the previous bytes have to be on
  // the card before the save they came from is overwritten. Syncing a temp file
  // that is about to be deleted would just be a slow way to fail.
  const bool synced = !short_write && flushed && closed && Synced(temp);
  if (short_write || !flushed || !closed || !synced) {
    std::remove(temp.c_str());
    result.error = CopyError::kWriteFailed;
    result.message = Describe(temp, "could not be written completely");
    return result;
  }

  const WriteResult committed = CommitStaged(temp, to);
  if (!committed.ok()) {
    result.error = committed.error == WriteError::kOpenFailed ? CopyError::kOpenFailed
                                                              : CopyError::kCommitFailed;
    result.message = committed.message;
    return result;
  }
  result.bytes_copied = copied;
  return result;
}

ReadResult ReadFile(const std::string& path) {
  ReadResult result;
  errno = 0;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    // The distinction is load-bearing, not tidiness: a caller answers "there is
    // no file" by creating one, and `device.dat` is a file that must never be
    // created over an existing identifier. Only ENOENT and ENOTDIR mean the
    // path cannot name anything; every other refusal -- a handle table that is
    // full, `sdmc:` not mounted yet at boot, a permission -- is a bad moment,
    // and a bad moment is not evidence that nothing was ever written here.
    const int why = errno;
    const bool absent = why == ENOENT || why == ENOTDIR;
    result.error = absent ? ReadError::kMissing : ReadError::kUnreadable;
    result.message = Describe(path, absent ? "does not exist" : "could not be opened");
    return result;
  }

  char buffer[512];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    result.contents.append(buffer, got);
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) {
    result.contents.clear();
    result.error = ReadError::kUnreadable;
    result.message = Describe(path, "could not be read");
  }
  return result;
}

const char* ToString(BoundedRead outcome) {
  switch (outcome) {
    case BoundedRead::kOk:
      return "ok";
    case BoundedRead::kMissing:
      return "missing";
    case BoundedRead::kUnreadable:
      return "unreadable";
    case BoundedRead::kTooLarge:
      return "too_large";
  }
  return "ok";
}

BoundedRead ReadBounded(const std::string& path, std::size_t limit, std::string* out) {
  out->clear();
  errno = 0;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    // The same distinction ReadFile draws, for the same reason: only ENOENT and
    // ENOTDIR mean nothing was ever written here. A full handle table or an
    // `sdmc:` that is not mounted yet is a bad moment, and answering one with
    // "no file" would silently run the console on defaults.
    const int why = errno;
    return (why == ENOENT || why == ENOTDIR) ? BoundedRead::kMissing : BoundedRead::kUnreadable;
  }

  char buffer[4096];
  std::size_t got = 0;
  bool too_large = false;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (out->size() + got > limit) {
      too_large = true;
      break;
    }
    out->append(buffer, got);
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (too_large) {
    out->clear();
    return BoundedRead::kTooLarge;
  }
  if (failed) {
    out->clear();
    return BoundedRead::kUnreadable;
  }
  return BoundedRead::kOk;
}

bool Shred(const std::string& path) {
  bool gone = ShredOne(path);
  gone = ShredOne(TempPathFor(path)) && gone;
  gone = ShredOne(PreviousPathFor(path)) && gone;
  return gone;
}

StagedFile::~StagedFile() {
  if (!path_.empty()) {
    std::remove(path_.c_str());
  }
}

}  // namespace rommsync::io
