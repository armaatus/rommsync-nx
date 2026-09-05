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
  kTooManyEntries, ///< more than `kMaxDirectoryEntries`; the first ones by name are in `entries`
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

/// **A truncated listing keeps the lexicographically first entries, not the
/// first ones `readdir` happened to hand over.** Which entries survive the
/// bound has to be a property of the directory rather than of the order it is
/// stored in: the save scanner resolves a contested `(rom_id, slot)` in favour
/// of the first file it sees, so a *selection* that changes between ticks makes
/// two files take turns overwriting each other through the server -- the exact
/// failure sorting each listing was meant to rule out. A backend that cannot
/// enumerate cheaply enough to do that must report `kUnreadable` instead.


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

  /// The platform path `sd_path` names -- `sdmc:/retroarch/saves/Game.srm` on
  /// Horizon, a path under the card's root on the host.
  ///
  /// It exists because the engine's *file* operations take a real path and only
  /// the backend knows the prefix: `io::ReadFile` and `state::HashFile` open
  /// with `<cstdio>`, which is portable, while the mapping in front of them is
  /// not. Without this the scanner could name a save it had no way to let
  /// anything else open -- which is the same save reported with no digest, and
  /// therefore uploaded on every tick.
  ///
  /// Empty when `sd_path` is not a path on this card. That is a refusal, not a
  /// failure to find: a caller must not fall back to the SD path, because on
  /// the host it would resolve against the process's working directory.
  virtual std::string Resolve(std::string_view sd_path) const = 0;

 protected:
  FileSystem() = default;
};

}  // namespace rommsync::fs
