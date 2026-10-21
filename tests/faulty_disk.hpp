// A card that refuses on cue: the `io::Disk` decorator M9-13 (#202) exists for.
//
// A full SD card, one that throws EIO, and one that is pulled out mid-write are
// ordinary for a rom syncer and unreachable from a laptop without this: nobody
// fills their disk to prove a queue says "full". The engine's writes all go
// through `io::Disk` (disk.hpp), so a test installs one of these for a scope and
// the engine meets the refusal through the same calls it makes on a console.
//
// Deterministic by construction: the refusal is counted in bytes written to the
// files it is aimed at, not in time and not in stdio buffer flushes.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "rommsync/disk.hpp"

namespace faulty {

/// What to refuse, and when.
struct Plan {
  /// Only files whose path contains this are counted and refused. Empty means
  /// every file. `.part` aims it at a transfer in flight and nothing else, which
  /// is how a test proves the *queue* write after it still lands; `/.backup/`
  /// aims it at the copy an overwrite depends on.
  std::string aimed_at;

  /// The `errno` the refusal carries: `ENOSPC` for a full card, `EIO` for one
  /// that stopped answering.
  int error = ENOSPC;

  /// Bytes the aimed-at files take, in total, before the refusal. The write that
  /// crosses it is *short* -- it takes what fits and refuses the rest -- because
  /// that is what a full card does rather than refusing the whole buffer.
  std::uint64_t after = 0;

  /// Once tripped, refuse **everything**: every open, write, rename and remove,
  /// on every path, with `error`. A card that went away does not come back for
  /// the queue file. Cleared only by the decorator going out of scope -- the card
  /// going back in.
  bool gone = false;
};

class Disk final : public rommsync::io::Disk {
 public:
  explicit Disk(Plan plan) : plan_(std::move(plan)) { previous_ = rommsync::io::SetDisk(this); }

  ~Disk() override { rommsync::io::SetDisk(previous_); }

  Disk(const Disk&) = delete;
  Disk& operator=(const Disk&) = delete;

  std::FILE* Open(const std::string& path, const char* mode) override {
    std::lock_guard<std::mutex> hold(mutex_);
    if (Gone()) {
      errno = plan_.error;
      ++refusals_;
      return nullptr;
    }
    std::FILE* file = Inner().Open(path, mode);
    if (file != nullptr) {
      // Erased as well as set: a closed handle's address is reused by a later
      // `fopen`, and the queue file opened after a `.part` must not inherit it.
      if (Aimed(path)) {
        aimed_[file] = path;
      } else {
        aimed_.erase(file);
      }
    }
    return file;
  }

  std::size_t Write(std::FILE* file, const void* data, std::size_t size) override {
    std::lock_guard<std::mutex> hold(mutex_);
    if (Gone()) {
      errno = plan_.error;
      ++refusals_;
      return 0;
    }
    if (aimed_.find(file) == aimed_.end()) {
      return Inner().Write(file, data, size);
    }
    const std::uint64_t room = plan_.after > written_ ? plan_.after - written_ : 0;
    if (size <= room) {
      const std::size_t wrote = Inner().Write(file, data, size);
      written_ += wrote;
      return wrote;
    }
    // The short write: what fits goes down, the rest is refused.
    const std::size_t wrote =
        room == 0 ? 0 : Inner().Write(file, data, static_cast<std::size_t>(room));
    written_ += wrote;
    tripped_ = true;
    ++refusals_;
    errno = plan_.error;
    return wrote;
  }

  int Rename(const std::string& from, const std::string& to) override {
    std::lock_guard<std::mutex> hold(mutex_);
    if (Gone()) {
      errno = plan_.error;
      ++refusals_;
      return -1;
    }
    return Inner().Rename(from, to);
  }

  int Remove(const std::string& path) override {
    std::lock_guard<std::mutex> hold(mutex_);
    if (Gone()) {
      errno = plan_.error;
      ++refusals_;
      return -1;
    }
    return Inner().Remove(path);
  }

  /// Calls refused so far. A scenario asserts this is non-zero, so a fault that
  /// never fired cannot pass as one the engine survived.
  int refusals() const {
    std::lock_guard<std::mutex> hold(mutex_);
    return refusals_;
  }

 private:
  rommsync::io::Disk& Inner() const {
    return previous_ != nullptr ? *previous_ : rommsync::io::StdioDisk();
  }

  bool Aimed(std::string_view path) const {
    return path.find(plan_.aimed_at) != std::string_view::npos;
  }

  bool Gone() const { return plan_.gone && tripped_; }

  Plan plan_;
  rommsync::io::Disk* previous_ = nullptr;
  mutable std::mutex mutex_;
  /// Handles on aimed-at files, keyed by address. Every `fopen` passes through
  /// `Open`, which is what keeps a reused address honest.
  std::map<std::FILE*, std::string> aimed_;
  std::uint64_t written_ = 0;
  bool tripped_ = false;
  int refusals_ = 0;
};

}  // namespace faulty
