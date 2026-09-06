#include "rommsync/file_system.hpp"

namespace rommsync::fs {

const char* ToString(ListError error) {
  switch (error) {
    case ListError::kNone:
      return "none";
    case ListError::kMissing:
      return "missing";
    case ListError::kNotADirectory:
      return "not a directory";
    case ListError::kUnreadable:
      return "unreadable";
    case ListError::kTooManyEntries:
      return "too many entries";
  }
  return "unknown";
}

const char* ToString(MakeDirError error) {
  switch (error) {
    case MakeDirError::kNone:
      return "none";
    case MakeDirError::kNotOnThisCard:
      return "not on this card";
    case MakeDirError::kNotADirectory:
      return "not a directory";
    case MakeDirError::kUnwritable:
      return "unwritable";
  }
  return "unknown";
}

bool SplitPath(const std::string& sd_path, std::string* directory, std::string* leaf) {
  const std::size_t slash = sd_path.rfind('/');
  if (slash == std::string::npos || slash + 1 >= sd_path.size()) {
    return false;
  }
  *directory = slash == 0 ? "/" : sd_path.substr(0, slash);
  *leaf = sd_path.substr(slash + 1);
  return true;
}

const Entry* Directories::Find(const std::string& sd_path, std::string* why) {
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
  // entries by name, so the file may well be in it, and dropping the row for a
  // folder that is merely large re-hashes that file on every tick from then on.
  // Only a listing that does not hold the file is a failure.
  for (const Entry& entry : listing_.entries) {
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

}  // namespace rommsync::fs
