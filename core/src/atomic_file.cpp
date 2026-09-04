#include "rommsync/atomic_file.hpp"

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

/// Overwrite whatever is at `path` with zeroes, then unlink it. See `Shred` for
/// what that does and does not buy on an SD card.
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
    if (replacing) {
      std::rename(previous.c_str(), path.c_str());
    }
    std::remove(temp.c_str());
    return {WriteError::kCommitFailed, Describe(path, "could not be replaced by " + temp)};
  }
  std::remove(previous.c_str());
  return {};
}

ReadResult ReadFile(const std::string& path) {
  ReadResult result;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    result.error = ReadError::kMissing;
    result.message = Describe(path, "could not be opened");
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

ReadResult ReadCommitted(const std::string& path) {
  ReadResult result = ReadFile(path);
  if (result.ok()) {
    return result;
  }
  ReadResult previous = ReadFile(PreviousPathFor(path));
  return previous.ok() ? previous : result;
}

bool Shred(const std::string& path) {
  bool gone = ShredOne(path);
  gone = ShredOne(TempPathFor(path)) && gone;
  gone = ShredOne(PreviousPathFor(path)) && gone;
  return gone;
}

}  // namespace rommsync::io
