// Every byte the engine puts on the card, and every name it publishes or
// removes, behind one seam.
//
// `fs::FileSystem` reads directories; it never wrote anything. Writes went
// straight to `<cstdio>` from four places -- `atomic_file.cpp`, the download
// worker, and both HTTP backends' `.part` sinks -- which made the failures an
// SD card actually produces unreachable from a test: a full card, a card that
// stops answering, a short write. A test cannot fill a laptop's disk to prove
// the queue says "full", and it cannot pull the card out (#202).
//
// So every write-side call goes through `io::Disk`, and a test installs a
// decorator that refuses on cue. The stdio one is the default and is what both
// platforms run: `fopen`, `fwrite`, `rename` and `remove` mean the same thing
// over Horizon's `sdmc:` devoptab as they do on a laptop, which is why there is
// no Horizon backend here.
//
// **Why process-wide, like `io::FileSync`, rather than injected.** The callers
// are free functions (`io::WriteAtomically`, `io::CommitStaged`) and two
// `http::HttpClient` backends that hold no filesystem; threading a `Disk&`
// through every signature that reaches them -- `token_store`, `config`,
// `state_db`, `sync_execute`, both backends' constructors -- would carry one
// decision that is the same for the whole process. `atomic_file.hpp`'s
// `SetFileSync` makes the same trade for the same reason.
//
// **What does not go through it**, deliberately: reads (a refusal there is
// already reachable -- a missing file -- and is tested), `log.cpp` (logging is
// best effort and a decorator failing it would be noise), and `fflush`/`fclose`
// (a fault injected at `Write` is deterministic; one at the flush depends on
// stdio's buffer size). The backends still read `errno` after a failed flush,
// because that is where a *real* full card often first shows up.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

namespace rommsync::io {

/// The write side of the card.
///
/// Every method has the stdio function's contract exactly -- including leaving
/// `errno` set on a refusal -- so a call site reads the same as it did, and a
/// decorator that forwards changes nothing.
class Disk {
 public:
  virtual ~Disk() = default;

  Disk(const Disk&) = delete;
  Disk& operator=(const Disk&) = delete;

  /// `std::fopen`, for a file about to be written: `"wb"` or `"r+b"`. Null with
  /// `errno` set.
  virtual std::FILE* Open(const std::string& path, const char* mode) = 0;

  /// `std::fwrite` of bytes. Fewer than `size` with `errno` set means the card
  /// took only that many -- the short write a full card produces.
  virtual std::size_t Write(std::FILE* file, const void* data, std::size_t size) = 0;

  /// `std::rename`. Zero, or non-zero with `errno` set.
  virtual int Rename(const std::string& from, const std::string& to) = 0;

  /// `std::remove`. Zero, or non-zero with `errno` set.
  virtual int Remove(const std::string& path) = 0;

 protected:
  Disk() = default;
};

/// The stdio `Disk`. What runs on both platforms, and what a decorator wraps.
Disk& StdioDisk();

/// Install `disk` process-wide, or null for `StdioDisk()`. Returns the one it
/// replaces, so a scoped decorator can put it back.
///
/// Not owned. A test's, in practice: `main` never calls this. Stored in an
/// atomic so a late call is not a data race, not so that swapping it under a
/// write in flight is supported.
Disk* SetDisk(Disk* disk);

/// The installed `Disk`.
Disk& CurrentDisk();

/// The installed `Disk`'s four calls, for a call site that has no reason to
/// hold it.
std::FILE* OpenForWriting(const std::string& path, const char* mode);
std::size_t Write(std::FILE* file, const void* data, std::size_t size);
int Rename(const std::string& from, const std::string& to);
int Remove(const std::string& path);

/// Why the card refused a write, as far as `errno` can say.
///
/// Three outcomes rather than an `errno`, because what a caller does with them
/// is three different sentences to a user and nothing else: a full card is
/// fixed by deleting something, a file too large for the card's format is not
/// fixed by anything this client does, and everything else -- an I/O error, a
/// card that went away, a folder that is not there -- is "the card refused".
enum class Refusal {
  kFull,          ///< ENOSPC: no space left
  kFileTooLarge,  ///< EFBIG: past what one file may hold -- 4 GiB - 1 on FAT32
  kRefused,       ///< any other refusal
};

/// `errno` after a failed write, classified. Zero is `kRefused`: it is only
/// asked after something failed, and a stdio that failed without saying why
/// still failed.
Refusal RefusalOf(int error_number);

/// One clause for a sentence about the refusal: "the card is full". Never
/// names a path, so it can go anywhere a `QueueEntry::message` goes.
const char* Explain(Refusal refusal);

}  // namespace rommsync::io
