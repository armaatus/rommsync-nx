#include "card.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine.hpp"

namespace rommsync::sysmodule {
namespace {

/// `/retroarch/saves` -> `sdmc:/retroarch/saves`, or empty.
///
/// A `..` segment is **refused rather than resolved**, the same line the host
/// backend draws and for the same reason: these paths name folders a human
/// picked in `config.ini`, and one that walks out of the card's root is a
/// mistake that must not become a write somewhere else. `.` and empty segments
/// are dropped; a NUL is refused, because every call below stops at one and the
/// path that got used would not be the path that was checked.
std::string ResolveOnCard(std::string_view sd_path) {
  if (sd_path.find('\0') != std::string_view::npos) {
    return {};
  }
  std::string resolved = kSdRoot;
  std::size_t start = 0;
  while (start <= sd_path.size()) {
    const std::size_t slash = sd_path.find('/', start);
    const std::string_view segment = sd_path.substr(
        start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (segment == "..") {
      return {};
    }
    if (!segment.empty() && segment != ".") {
      resolved += '/';
      resolved.append(segment);
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return resolved;
}

/// Size and mtime in one `stat`. A failure leaves the defaults: size zero, and
/// an mtime of zero that `fs::Entry` documents as "the backend could not read
/// one" -- which `sync::Validate` then refuses as an unset clock rather than
/// treating as a very old save.
void ReadStat(const std::string& path, fs::Entry* entry) {
  struct ::stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return;
  }
  if (!entry->is_directory) {
    entry->size_bytes = static_cast<std::int64_t>(info.st_size);
  }
  entry->modified_unix = static_cast<std::int64_t>(info.st_mtime);
}

/// A max-heap on `name`, so the entry the heap gives up is the largest one --
/// which is how the *smallest* `kMaxDirectoryEntries` survive. See
/// `file_system.hpp`: which entries survive the bound has to be a property of
/// the directory and not of the order `readdir` happened to hand them over, or
/// the save scanner's duplicate-slot rule changes its mind between ticks.
bool ByNameDescending(const fs::Entry& left, const fs::Entry& right) {
  return left.name < right.name;
}

class SdCard final : public fs::FileSystem {
 public:
  fs::Listing List(std::string_view sd_path) override {
    fs::Listing listing;
    const std::string resolved = ResolveOnCard(sd_path);
    if (resolved.empty()) {
      listing.error = fs::ListError::kMissing;
      listing.message = std::string(sd_path) + ": not a path on this card";
      return listing;
    }

    ::DIR* directory = ::opendir(resolved.c_str());
    if (directory == nullptr) {
      // `opendir` cannot say "it is a file" and "it is not there" apart on this
      // devoptab, so a `stat` is what decides which sentence the caller gets --
      // a mapped folder that does not exist yet is a normal card, and a mapped
      // folder that is a file is a `config.ini` worth complaining about.
      struct ::stat info {};
      if (::stat(resolved.c_str(), &info) == 0 && !S_ISDIR(info.st_mode)) {
        listing.error = fs::ListError::kNotADirectory;
        listing.message = std::string(sd_path) + ": is a file, not a directory";
      } else {
        listing.error = fs::ListError::kMissing;
        listing.message = std::string(sd_path) + ": no such directory";
      }
      return listing;
    }

    bool truncated = false;
    while (const ::dirent* found = ::readdir(directory)) {
      // Never reported, because every caller would have to filter them and one
      // that forgot would ask the server to write to a directory
      // (`fs::Entry::name`).
      if (std::strcmp(found->d_name, ".") == 0 || std::strcmp(found->d_name, "..") == 0) {
        continue;
      }
      fs::Entry entry;
      entry.name = found->d_name;
      const std::string child = resolved + "/" + entry.name;
      // `d_type` is filled by fsdev, but a `stat` is needed anyway for the size
      // and the mtime, so the kind comes off the same call rather than off two
      // sources that could disagree.
      struct ::stat info {};
      if (::stat(child.c_str(), &info) == 0) {
        entry.is_directory = S_ISDIR(info.st_mode);
      }
      ReadStat(child, &entry);

      listing.entries.push_back(std::move(entry));
      std::push_heap(listing.entries.begin(), listing.entries.end(), ByNameDescending);
      if (listing.entries.size() > fs::kMaxDirectoryEntries) {
        std::pop_heap(listing.entries.begin(), listing.entries.end(), ByNameDescending);
        listing.entries.pop_back();
        truncated = true;
      }
    }
    ::closedir(directory);

    if (truncated) {
      listing.error = fs::ListError::kTooManyEntries;
      listing.message = std::string(sd_path) + ": more than " +
                        std::to_string(fs::kMaxDirectoryEntries) + " entries; the first " +
                        std::to_string(fs::kMaxDirectoryEntries) +
                        " by name were read and the rest were not";
    }
    return listing;
  }

  fs::MakeDirResult CreateDirectory(std::string_view sd_path) override {
    fs::MakeDirResult result;
    const std::string resolved = ResolveOnCard(sd_path);
    if (resolved.empty()) {
      result.error = fs::MakeDirError::kNotOnThisCard;
      result.message = std::string(sd_path) + ": not a path on this card";
      return result;
    }

    // Every parent, because the one directory this exists for --
    // `sdmc:/config/rommsync/.backup/` -- sits under one that may not be there
    // either on a first boot. `kSdRoot` itself is skipped: `sdmc:` is the mount
    // and not a directory anything creates.
    std::size_t at = resolved.find('/', std::strlen(kSdRoot));
    while (at != std::string::npos) {
      const std::string parent = resolved.substr(0, at);
      ::mkdir(parent.c_str(), 0777);
      at = resolved.find('/', at + 1);
    }
    if (::mkdir(resolved.c_str(), 0777) == 0) {
      return result;
    }

    // A directory that is already there is success, not `kNotADirectory`: every
    // caller wants it to exist afterwards rather than to have been the one that
    // made it, and a tick runs this on entry every time (`file_system.hpp`).
    struct ::stat info {};
    if (::stat(resolved.c_str(), &info) == 0) {
      if (S_ISDIR(info.st_mode)) {
        return result;
      }
      result.error = fs::MakeDirError::kNotADirectory;
      result.message = std::string(sd_path) + ": is a file, not a directory";
      return result;
    }
    result.error = fs::MakeDirError::kUnwritable;
    result.message = std::string(sd_path) + ": the card would not create it";
    return result;
  }

  std::string Resolve(std::string_view sd_path) const override { return ResolveOnCard(sd_path); }
};

}  // namespace

std::unique_ptr<fs::FileSystem> MakeSdCard() { return std::make_unique<SdCard>(); }

}  // namespace rommsync::sysmodule
