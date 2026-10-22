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
/// unbounded loop.
///
/// **512: a number the heap holds** (M9-19, #217). It was chosen as
/// `state::kMaxRecords`, which M9-20 (#246) lowered to 128 and this did not
/// follow: a folder is listed to *find* saves among a card's other files -- a
/// rom folder read for content-dir saves holds the roms too -- so its listing
/// is not bounded by how many of them are saves. An entry costs ~148 bytes --
/// 56 of `Entry` on aarch64, half again while the vector last grew, and its
/// name's heap string.
/// Two listings at this bound fit under the largest response the heap table
/// already budgets (`sysmodule/source/main.cpp`), so it costs nothing of its
/// own; 4,096 did not, and was never costed.
///
/// A power of two, because a vector that never grows past it then never
/// allocates past it either -- `std::vector` doubles from one -- and a backend's
/// `List` stops at it rather than pushing one more and popping it back.
inline constexpr std::size_t kMaxDirectoryEntries = 512;
static_assert((kMaxDirectoryEntries & (kMaxDirectoryEntries - 1)) == 0,
              "kMaxDirectoryEntries must be a power of two, or a listing at the bound "
              "allocates room past it");

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

/// Why a directory could not be created.
enum class MakeDirError {
  kNone,

  /// `sd_path` is not a path on this card, so nothing was tried.
  ///
  /// Deliberately **not** spelled `kMissing`, which is what its neighbour
  /// `ListError` calls "there is no such path". Here there is no such *card*
  /// path to have, which is a refusal rather than something a caller answers by
  /// creating one -- the same line `fs::FileSystem::Resolve` draws when it
  /// returns empty.
  kNotOnThisCard,

  kNotADirectory,  ///< something is already there and it is a file
  kUnwritable,     ///< the card refused it -- full, read-only, or a bad moment
};

/// Stable, log-friendly name. Never null.
const char* ToString(MakeDirError error);

/// What `CreateDirectory` did, or why it did not.
struct MakeDirResult {
  MakeDirError error = MakeDirError::kNone;

  /// For logs. Names the path and what went wrong.
  std::string message;

  bool ok() const { return error == MakeDirError::kNone; }
};

/// The card's format, as far as a backend can tell.
///
/// It exists for one number: FAT32 holds no file larger than 4 GiB - 1, several
/// rom images are larger, and finding that out at byte 4294967295 costs a whole
/// transfer to learn something the card could have said first (#202).
enum class Format {
  kUnknown,  ///< the backend could not tell -- Horizon cannot, unless the console has no exFAT driver
  kFat32,
  kExFat,
  kOther,    ///< something that is neither, so not bounded at 4 GiB -- a laptop's disk
};

/// Stable, log-friendly name. Never null.
const char* ToString(Format format);

/// FAT32's per-file ceiling: a 32-bit size field, so 4 GiB - 1.
inline constexpr std::uint64_t kFat32MaxFileBytes = 0xFFFFFFFFull;

/// What a volume can hold. Everything here is an answer *before* a write, so
/// every field has an "I do not know" that a caller reads as "do not refuse on
/// my account": a refusal built on a guess would block a download that works.
struct Volume {
  Format format = Format::kUnknown;

  /// The largest file one write may produce, or `0` when nothing known bounds
  /// it. The backend decides, not the caller -- an unknown format is not
  /// automatically unbounded (`sysmodule/source/card.cpp` says why).
  std::uint64_t max_file_bytes = 0;

  /// Bytes this process may still write, or `-1` when the backend could not say.
  std::int64_t free_bytes = -1;
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

  /// Create `sd_path`, and every parent of it that is missing.
  ///
  /// It exists because of one directory: `sdmc:/config/rommsync/.backup/`. No
  /// backup means no overwrite (docs/SYNC_PROTOCOL.md), so on a card that has
  /// never run this client every download would fail with
  /// `sync::OperationError::kBackupFailed` until something made that folder --
  /// and `io::WriteAtomically` cannot, because creating a directory needs a
  /// platform facility and `core/` has only standard headers (core/AGENTS.md).
  /// This is that facility, behind the interface that already owns the card.
  ///
  /// **A directory that is already there is success**, not `kNotADirectory`.
  /// Every caller wants the directory to exist afterwards rather than to have
  /// been the one that made it, and a tick runs this on entry every time.
  virtual MakeDirResult CreateDirectory(std::string_view sd_path) = 0;

  /// Remove `sd_path` if it is an empty directory, and answer whether it went.
  ///
  /// Never more than that: a directory holding anything, a file, or a path that
  /// is not on this card stays, and the answer is false. It exists to take back
  /// a folder a placement made for a download that then failed (#258,
  /// `MadeFolders`), so an emulator does not find an empty system folder the
  /// user never played. A backend that cannot do this cheaply keeps the default,
  /// which removes nothing -- the folder is litter, not a lost save.
  virtual bool RemoveEmptyDirectory(std::string_view sd_path) {
    static_cast<void>(sd_path);
    return false;
  }

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

  /// What the volume holding `sd_path` can take: its format, the largest file
  /// it allows, and the space left on it.
  ///
  /// Asked once per download, before the first byte moves, so a rom the card
  /// cannot hold is a sentence on the queue screen rather than a transfer that
  /// fails at the end of a 4 GiB file (#202). Not pure: a backend that cannot
  /// tell answers the all-unknown `Volume`, which refuses nothing -- and that is
  /// also what every test fake that predates this inherits.
  virtual Volume VolumeOf(std::string_view sd_path) const {
    static_cast<void>(sd_path);
    return {};
  }

 protected:
  FileSystem() = default;
};

/// The directory part of an SD path, and the leaf under it.
///
/// `/retroarch/saves/Game.srm` -> `/retroarch/saves` + `Game.srm`. False for a
/// path with no separator, or one that ends in one: neither has a directory to
/// list and a leaf to find in it.
bool SplitPath(const std::string& sd_path, std::string* directory, std::string* leaf);

/// `name` under `directory`, with one `/` between: `/tico/saves` + `snes`.
std::string JoinPath(std::string_view directory, std::string_view name);

/// The folders one write had to make, taken away again unless it lands (#258).
///
/// A first-time restore makes its folder before it fetches -- the download is
/// staged beside the save -- so a fetch that then fails would leave behind a
/// folder nothing asked for: `users/<uid>/<slug>` for a system never played,
/// `<rom folder>/<CoreFolder>/`. On destruction each folder `Make` created is
/// removed, innermost first, while it is empty (`RemoveEmptyDirectory`); one
/// that was already there, or that holds anything by then, stays. `Keep` once
/// the file is in place.
class MadeFolders {
 public:
  explicit MadeFolders(FileSystem& files) : files_(&files) {}
  ~MadeFolders();

  MadeFolders(const MadeFolders&) = delete;
  MadeFolders& operator=(const MadeFolders&) = delete;

  /// `CreateDirectory(sd_path)`, remembering which of `sd_path` and its parents
  /// were missing before it: one listing per missing folder, and one of the
  /// first that is there.
  MakeDirResult Make(std::string_view sd_path);

  /// The write landed: the folders stay.
  void Keep() { made_.clear(); }

 private:
  FileSystem* files_;
  std::vector<std::string> made_;  ///< outermost first
};

/// The most recent directory listing, and nothing older.
///
/// `core/` has no single-file stat -- `FileSystem` reads directories and
/// resolves paths, and nothing else -- so the mtime and the size of a file the
/// engine just rewrote come out of a listing of its folder.
///
/// **One entry, not a map.** A `Listing` may hold up to `kMaxDirectoryEntries`
/// names, each of them a heap `std::string`, and the sysmodule's inner heap is
/// 1.5 MiB (core/AGENTS.md). Keeping one listing per directory for the length of
/// a tick is unbounded in exactly the dimension that is scarce, and it would be
/// unbounded on the tick that is already the most expensive -- the one that
/// downloaded a lot of files. Keeping the last one costs a single listing and
/// still collapses the case that matters, which is several files in the same
/// folder.
class Directories {
 public:
  explicit Directories(FileSystem& files) : files_(&files) {}

  /// The entry for `sd_path`, or nullptr with the reason in `why`.
  ///
  /// The pointer is into this object and is invalidated by the next call for a
  /// different directory, so a caller reads it immediately.
  const Entry* Find(const std::string& sd_path, std::string* why);

 private:
  FileSystem* files_;

  /// Empty until the first listing. `SplitPath` never produces an empty
  /// directory -- the shortest it answers is `/` -- so this cannot collide with
  /// a real one.
  std::string directory_;
  Listing listing_;
};

}  // namespace rommsync::fs
