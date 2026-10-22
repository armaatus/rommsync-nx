#include "rommsync/atomic_file.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>

#include "rommsync/disk.hpp"

namespace rommsync::io {
namespace {

/// The platform hook, or null. See `SetFileSync`: set once from `main`, and
/// atomic so that a late call cannot race the first write rather than because
/// swapping it mid-write is supported.
std::atomic<FileSync> g_file_sync{nullptr};

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

/// "could not be written completely", and what the card said when it was the
/// card that refused -- "the card is full" is the half of the sentence a user
/// can act on (#202). `card_refused` is whether the write or the flush failed,
/// which is when `error_number` holds the `errno` they left; a close or a sync
/// that failed after them says nothing `errno` can be trusted for.
std::string Incomplete(bool card_refused, int error_number) {
  std::string sentence = "could not be written completely";
  if (card_refused) {
    sentence += ": ";
    sentence += Explain(RefusalOf(error_number));
  }
  return sentence;
}

/// Whether a failed open's `errno` means the path names nothing. Only ENOENT
/// and ENOTDIR do; every other refusal -- a handle table that is full, `sdmc:`
/// not mounted yet at boot, a permission -- is a bad moment, and a bad moment
/// is not evidence that nothing was ever written here.
bool Absent(int error_number) { return error_number == ENOENT || error_number == ENOTDIR; }

/// Overwrite whatever is at `path` with zeroes, then unlink it.
///
/// Best-effort, and `Shred` says how little that is worth: without an `fsync`
/// the standard library does not expose, the zeroes may never reach the card at
/// all, since a filesystem is free to drop dirty pages belonging to an inode
/// that is about to be unlinked. The unlink is the part that does the work.
bool ShredOne(const std::string& path) {
  std::FILE* file = OpenForWriting(path, "r+b");
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
        if (Write(file, zeroes, static_cast<std::size_t>(chunk)) !=
            static_cast<std::size_t>(chunk)) {
          break;
        }
        written += chunk;
      }
      std::fflush(file);
    }
    std::fclose(file);
  }
  Remove(path);
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
    Remove(previous);
    if (Rename(path, previous) != 0) {
      Remove(staged);
      return {WriteError::kCommitFailed, Describe(path, "could not be moved aside to " + previous)};
    }
  }
  if (Rename(staged, path) != 0) {
    std::string detail = "could not be replaced by " + staged;
    // Putting the previous record back is the whole reason a caller can treat a
    // failed write as costing only the new record. When even *that* fails the
    // destination is gone, and saying so is the difference between a caller
    // reading `.old` deliberately and one wondering why its file vanished.
    if (replacing && Rename(previous, path) != 0) {
      detail += ", and the previous record could not be put back -- it is at " + previous;
    }
    Remove(staged);
    return {WriteError::kCommitFailed, Describe(path, detail)};
  }
  Remove(previous);

  // After the fact on purpose: the rename has happened, so reporting a failure
  // here would describe a commit that did not occur. What it buys is the *name*
  // being as durable as the bytes already are -- see `DirectoryOf`. A platform
  // whose hook commits the whole device (Horizon's does) has already done this
  // and pays for it twice, which is cheaper than the alternative of a second
  // hook nobody would remember to call.
  //
  // Not ignored, though. It used to be, and the crash replay found what that
  // cost: a backup whose directory the card would not confirm was counted as a
  // backup, and the save overwrite in *another* directory could survive a power
  // cut that the backup's name did not (#203). So the answer goes back to the
  // caller, and the one that depends on it -- `sync::BackUpFirst` -- reads it.
  WriteResult committed;
  const std::string directory = DirectoryOf(path);
  if (!directory.empty()) {
    committed.name_durable = Sync(directory);
  }
  return committed;
}

namespace {

/// The body of both writers: `write` puts the whole record into the temp file
/// and says whether every byte went, and the rest -- flush, close, sync, commit
/// -- is the same whether the record came in one piece or many.
WriteResult WriteStaged(const std::string& path, const std::function<bool(std::FILE*)>& write) {
  const std::string temp = TempPathFor(path);

  std::FILE* file = OpenForWriting(temp, "wb");
  if (file == nullptr) {
    return {WriteError::kOpenFailed,
            Describe(temp, "could not be created (does the directory exist?)")};
  }

  errno = 0;
  const bool written = write(file);
  // The flush has to happen while the handle is still open: fclose reports a
  // failed flush too, but by then there is nothing left to distinguish "the
  // bytes never left the buffer" from "the close itself failed". The sync comes
  // after the close, by path, because that is the contract both platforms can
  // hold (`FileSync`).
  const bool flushed = written && std::fflush(file) == 0;
  // Read before `fclose` can overwrite it: what the card said, if it said no.
  const int refused = flushed ? 0 : errno;
  const bool closed = std::fclose(file) == 0;
  // After the close and before the commit below, which is the only ordering
  // that means anything: the bytes have to be on the card before the rename
  // publishes them.
  const bool synced = flushed && closed && Sync(temp);
  if (!flushed || !closed || !synced) {
    Remove(temp);
    return {WriteError::kWriteFailed, Describe(temp, Incomplete(!flushed, refused))};
  }

  // Everything above touched only the temp file, so a failure up to this point
  // leaves whatever `path` already held exactly as it was.
  return CommitStaged(temp, path);
}

}  // namespace

WriteResult WriteAtomically(const std::string& path, std::string_view contents) {
  return WriteStaged(path, [contents](std::FILE* file) {
    return Write(file, contents.data(), contents.size()) == contents.size();
  });
}

WriteResult WriteAtomically(const std::string& path,
                            const std::function<bool(std::string* piece)>& next) {
  return WriteStaged(path, [&next](std::FILE* file) {
    std::string piece;
    for (;;) {
      piece.clear();
      if (!next(&piece)) {
        return true;
      }
      if (Write(file, piece.data(), piece.size()) != piece.size()) {
        return false;
      }
    }
  });
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
    // The caller this exists for reads `Absent` as there being nothing here to
    // preserve, and anything else must not be: a full handle table is a bad
    // moment, and a bad moment is not permission to overwrite a save without a
    // backup.
    const bool absent = Absent(errno);
    result.error = absent ? CopyError::kSourceMissing : CopyError::kSourceUnreadable;
    result.message = Describe(from, absent ? "does not exist" : "could not be opened");
    return result;
  }

  const std::string temp = TempPathFor(to);
  std::FILE* destination = OpenForWriting(temp, "wb");
  if (destination == nullptr) {
    std::fclose(source);
    result.error = CopyError::kOpenFailed;
    result.message = Describe(temp, "could not be created (does the directory exist?)");
    return result;
  }

  // 4 KiB, the convention in this directory: the main thread gets a 32 KiB
  // stack on the console, half of it the deepest parse's, so a buffer sized
  // for a desktop compiles here and overflows there (core/AGENTS.md).
  char buffer[4096];
  std::uint64_t copied = 0;
  bool short_write = false;
  int refused = 0;
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), source)) > 0) {
    errno = 0;
    if (Write(destination, buffer, got) != got) {
      short_write = true;
      refused = errno;
      break;
    }
    copied += got;
  }
  const bool read_failed = std::ferror(source) != 0;
  std::fclose(source);
  errno = 0;
  const bool flushed = std::fflush(destination) == 0;
  if (!short_write && !flushed) {
    refused = errno;
  }
  const bool closed = std::fclose(destination) == 0;

  if (read_failed) {
    Remove(temp);
    result.error = CopyError::kSourceUnreadable;
    result.message = Describe(from, "could not be read to the end");
    return result;
  }
  // Only once the bytes are known to be all there, and before the commit below
  // -- which is the whole point of this copy: the previous bytes have to be on
  // the card before the save they came from is overwritten. Syncing a temp file
  // that is about to be deleted would just be a slow way to fail.
  const bool synced = !short_write && flushed && closed && Sync(temp);
  if (short_write || !flushed || !closed || !synced) {
    Remove(temp);
    result.error = CopyError::kWriteFailed;
    result.message = Describe(temp, Incomplete(short_write || !flushed, refused));
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
  result.name_durable = committed.name_durable;
  return result;
}

ReadResult ReadFile(const std::string& path) {
  ReadResult result;
  errno = 0;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    // The distinction is load-bearing, not tidiness: a caller answers "there is
    // no file" by creating one, and `device.dat` is a file that must never be
    // created over an existing identifier (`Absent`).
    const bool absent = Absent(errno);
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
  return ReadBounded(path, limit, out, nullptr);
}

BoundedRead ReadBounded(const std::string& path, std::size_t limit, std::string* out,
                        const std::function<BoundedRead(std::FILE* file)>& past_limit) {
  out->clear();
  errno = 0;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    // `ReadFile`'s distinction, for its reason: answering a bad moment with "no
    // file" would silently run the console on defaults.
    return Absent(errno) ? BoundedRead::kMissing : BoundedRead::kUnreadable;
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
  if (too_large && past_limit != nullptr) {
    // Released, not cleared: the caller streams the rest because it cannot hold
    // it, and the bound's worth of text would otherwise sit beside it.
    std::string().swap(*out);
    // `fseek` rather than `rewind`, which cannot say it failed: a reader handed
    // a file mid-way would see no header and call it corrupt.
    const BoundedRead streamed =
        std::fseek(file, 0, SEEK_SET) == 0 ? past_limit(file) : BoundedRead::kUnreadable;
    std::fclose(file);
    return streamed;
  }
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
    Remove(path_);
  }
}

}  // namespace rommsync::io
