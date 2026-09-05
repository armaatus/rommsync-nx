// Reading a directory, behind an interface -- the same split `http.hpp` makes
// for the network.
//
// The save scanner is the first thing in the engine that has to *look at the SD
// card* rather than at a path it was handed, and there is no portable way to do
// that: the host has `<filesystem>`, which `core/` may not include (core/AGENTS.md),
// and Horizon has `fsdev`/`readdir` over `sdmc:`, which the host does not. So
// the engine says what it needs -- the names, kinds, sizes and mtimes in one
// directory -- and the backends say how.
//
// Paths here are the ones `config.hpp` produces: absolute from the SD root,
// `/`-separated, no trailing slash. A backend maps that onto whatever it has --
// `sdmc:` on Horizon, a root directory on the host -- and nothing above this
// line ever learns which.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rommsync::fs {

/// One entry in a directory.
struct Entry {
  /// The leaf name, never a path. `.` and `..` are never reported: every caller
  /// would have to filter them, and one that forgot would ask the server to
  /// write to a directory (see `sync::Validate`'s `file_name` rule).
  std::string name;

  bool is_directory = false;

  /// Zero for a directory, and for a file whose size could not be read.
  std::int64_t size_bytes = 0;

  /// Last modification, in whole seconds since the Unix epoch, UTC.
  ///
  /// `0` means the backend could not read one. That is deliberately the same
  /// value `sync::Validate` refuses as "an unset clock rather than an mtime",
  /// so an unknown mtime cannot become a save that negotiates as very old.
  std::int64_t modified_unix = 0;
};

/// Why a directory could not be read.
enum class ListError {
  kNone,
  kMissing,        ///< there is no such path -- a mapped folder the user never created
  kNotADirectory,  ///< something is there and it is a file
  kUnreadable,     ///< it is a directory and its contents could not be got out of it
  kTooManyEntries, ///< more than `kMaxDirectoryEntries`; what was read is in `entries`
};

/// Stable, log-friendly name. Never null.
const char* ToString(ListError error);

/// How many entries one directory may report.
///
/// The same reasoning as `config::kMaxPlatformSections`: this runs on a
/// sysmodule heap measured in megabytes, over a FAT32 card that gets yanked
/// mid-write, and a corrupt directory region is a plausible source of an
/// unbounded loop. A save folder with four thousand files in it is already far
/// past anything a human arranged.
inline constexpr std::size_t kMaxDirectoryEntries = 4096;

/// The contents of one directory, or the reason there are none.
///
/// A missing directory is not a failure worth stopping a tick for -- a platform
/// mapped to a folder that does not exist yet is a normal card -- so callers
/// read `error` to log it and carry on rather than to abort.
struct Listing {
  std::vector<Entry> entries;
  ListError error = ListError::kNone;

  /// For logs. Names the path and what went wrong; never the file contents.
  std::string message;

  bool ok() const { return error == ListError::kNone; }
};

/// The one directory-reading surface the engine is allowed to use.
///
/// Non-recursive on purpose. The scanner walks the folders a human listed in
/// `config.ini` and nothing below them: a `saves` folder that happens to sit
/// above the whole card would otherwise turn one tick into a full-card walk,
/// and there is no depth at which that is the user's intent.
class FileSystem {
 public:
  virtual ~FileSystem() = default;

  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;

  /// The entries directly under `sd_path`, in whatever order the backend has
  /// them. Callers that need a stable order sort: `readdir` promises none, and
  /// a scan whose output depends on the card's directory layout is a scan whose
  /// output changes when a file is deleted somewhere else.
  virtual Listing List(std::string_view sd_path) = 0;

 protected:
  FileSystem() = default;
};

}  // namespace rommsync::fs
