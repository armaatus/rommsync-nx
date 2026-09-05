#include "rommsync/host/native_file_system.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace rommsync::host {
namespace {

namespace fs = rommsync::fs;

/// The SD path, with its leading slashes gone and its shape checked.
///
/// A `..` segment is refused rather than resolved, matching
/// `config::NormalizeSdPath`: these paths name folders a human picked, and one
/// that walks out of the card's root is a mistake that must not become a read
/// of the host's home directory.
bool ResolveUnderRoot(const std::filesystem::path& root, std::string_view sd_path,
                      std::filesystem::path* out) {
  std::string relative(sd_path);
  if (relative.find('\0') != std::string::npos) {
    return false;
  }
  while (!relative.empty() && relative.front() == '/') {
    relative.erase(relative.begin());
  }
  std::filesystem::path resolved = root;
  std::size_t start = 0;
  while (start <= relative.size()) {
    const std::size_t slash = relative.find('/', start);
    const std::string_view segment =
        std::string_view(relative).substr(start, slash == std::string::npos ? std::string::npos
                                                                            : slash - start);
    if (segment == ".." ) {
      return false;
    }
    if (!segment.empty() && segment != ".") {
      resolved /= segment;
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  *out = std::move(resolved);
  return true;
}

/// Size and mtime in one `stat`, because `directory_entry`'s `last_write_time`
/// is a `file_clock` whose conversion to `system_clock` is C++20's
/// `clock_cast` -- not something every toolchain this builds on has. Seconds
/// since the epoch is what the interface asks for and what `stat` already
/// holds.
void ReadStat(const std::filesystem::path& path, fs::Entry* entry) {
  struct ::stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return;  // the defaults stand: size 0, and an mtime of 0 that means "unknown"
  }
  if (!entry->is_directory) {
    entry->size_bytes = static_cast<std::int64_t>(info.st_size);
  }
  entry->modified_unix = static_cast<std::int64_t>(info.st_mtime);
}

/// A max-heap on `name`, so the entry the heap gives up is the largest one --
/// which is how the *smallest* `kMaxDirectoryEntries` survive.
bool ByNameDescending(const fs::Entry& left, const fs::Entry& right) {
  return left.name < right.name;
}

class NativeFileSystem final : public fs::FileSystem {
 public:
  explicit NativeFileSystem(std::string root) : root_(std::move(root)) {}

  fs::Listing List(std::string_view sd_path) override {
    fs::Listing listing;
    std::filesystem::path resolved;
    if (!ResolveUnderRoot(root_, sd_path, &resolved)) {
      listing.error = fs::ListError::kMissing;
      listing.message = std::string(sd_path) + ": not a path on this card";
      return listing;
    }

    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(resolved, error);
    if (error || !std::filesystem::exists(status)) {
      listing.error = fs::ListError::kMissing;
      listing.message = std::string(sd_path) + ": no such directory";
      return listing;
    }
    if (!std::filesystem::is_directory(status)) {
      listing.error = fs::ListError::kNotADirectory;
      listing.message = std::string(sd_path) + ": is a file, not a directory";
      return listing;
    }

    std::filesystem::directory_iterator iterator(resolved, error);
    if (error) {
      listing.error = fs::ListError::kUnreadable;
      listing.message = std::string(sd_path) + ": " + error.message();
      return listing;
    }

    const std::filesystem::directory_iterator end;
    bool truncated = false;
    for (; iterator != end; iterator.increment(error)) {
      if (error) {
        listing.error = fs::ListError::kUnreadable;
        listing.message = std::string(sd_path) + ": " + error.message();
        return listing;
      }
      fs::Entry entry;
      entry.name = iterator->path().filename().string();
      // `.` and `..` are not reported by `directory_iterator`, but a symlink to
      // a directory is a directory here: the scanner does not descend, so the
      // only thing this decides is that it is not a save file.
      std::error_code kind_error;
      entry.is_directory = iterator->is_directory(kind_error);
      ReadStat(iterator->path(), &entry);

      // The bound is on what is *held*, not on how far the walk gets: keeping
      // the first `kMaxDirectoryEntries` names in order costs one heap of that
      // size, and keeping the first ones `readdir` offered would make the
      // surviving set depend on the card's directory layout -- which
      // `file_system.hpp` forbids, because the scanner's duplicate-slot rule
      // then changes its mind between ticks.
      listing.entries.push_back(std::move(entry));
      std::push_heap(listing.entries.begin(), listing.entries.end(), ByNameDescending);
      if (listing.entries.size() > fs::kMaxDirectoryEntries) {
        std::pop_heap(listing.entries.begin(), listing.entries.end(), ByNameDescending);
        listing.entries.pop_back();
        truncated = true;
      }
    }

    if (truncated) {
      listing.error = fs::ListError::kTooManyEntries;
      listing.message = std::string(sd_path) + ": more than " +
                        std::to_string(fs::kMaxDirectoryEntries) +
                        " entries; the first " + std::to_string(fs::kMaxDirectoryEntries) +
                        " by name were read and the rest were not";
    }
    return listing;
  }

  std::string Resolve(std::string_view sd_path) const override {
    std::filesystem::path resolved;
    if (!ResolveUnderRoot(root_, sd_path, &resolved)) {
      return {};
    }
    return resolved.string();
  }

 private:
  std::filesystem::path root_;
};

}  // namespace

std::unique_ptr<fs::FileSystem> MakeNativeFileSystem(std::string root) {
  return std::make_unique<NativeFileSystem>(std::move(root));
}

}  // namespace rommsync::host
