#include "rommsync/atomic_file.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace rommsync::io {
namespace {

constexpr const char* kTempSuffix = ".tmp";
constexpr const char* kPreviousSuffix = ".old";

bool Exists(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  std::fclose(file);
  return true;
}

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

std::string TempPathFor(std::string_view path) { return std::string(path) + kTempSuffix; }

std::string PreviousPathFor(std::string_view path) { return std::string(path) + kPreviousSuffix; }

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
  // bytes never left the buffer" from "the close itself failed".
  const bool flushed = written == contents.size() && std::fflush(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!flushed || !closed) {
    std::remove(temp.c_str());
    return {WriteError::kWriteFailed, Describe(temp, "could not be written completely")};
  }

  // The commit. Everything above touched only the temp file, so a failure up to
  // this point leaves whatever `path` already held exactly as it was.
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
      std::remove(temp.c_str());
      return {WriteError::kCommitFailed, Describe(path, "could not be moved aside to " + previous)};
    }
  }
  if (std::rename(temp.c_str(), path.c_str()) != 0) {
    std::string detail = "could not be replaced by " + temp;
    // Putting the previous record back is the whole reason a caller can treat a
    // failed write as costing only the new record. When even *that* fails the
    // destination is gone, and saying so is the difference between a caller
    // reading `.old` deliberately and one wondering why its file vanished.
    if (replacing && std::rename(previous.c_str(), path.c_str()) != 0) {
      detail += ", and the previous record could not be put back -- it is at " + previous;
    }
    std::remove(temp.c_str());
    return {WriteError::kCommitFailed, Describe(path, detail)};
  }
  std::remove(previous.c_str());
  return {};
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

bool Shred(const std::string& path) {
  bool gone = ShredOne(path);
  gone = ShredOne(TempPathFor(path)) && gone;
  gone = ShredOne(PreviousPathFor(path)) && gone;
  return gone;
}

}  // namespace rommsync::io
