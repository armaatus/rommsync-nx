#include "rommsync/disk.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>

namespace rommsync::io {
namespace {

class Stdio final : public Disk {
 public:
  std::FILE* Open(const std::string& path, const char* mode) override {
    return std::fopen(path.c_str(), mode);
  }

  std::size_t Write(std::FILE* file, const void* data, std::size_t size) override {
    return std::fwrite(data, 1, size, file);
  }

  int Rename(const std::string& from, const std::string& to) override {
    return std::rename(from.c_str(), to.c_str());
  }

  int Remove(const std::string& path) override { return std::remove(path.c_str()); }
};

/// Null means `StdioDisk()`, so nothing has to run before the first write.
std::atomic<Disk*> g_disk{nullptr};

}  // namespace

Disk& StdioDisk() {
  // A function-local static rather than a namespace-scope one: `config` and
  // `state_db` write from `main`, and the order namespace-scope objects in two
  // translation units are built in is not one this should depend on.
  static Stdio stdio;
  return stdio;
}

Disk* SetDisk(Disk* disk) { return g_disk.exchange(disk, std::memory_order_acq_rel); }

Disk& CurrentDisk() {
  Disk* installed = g_disk.load(std::memory_order_acquire);
  return installed != nullptr ? *installed : StdioDisk();
}

std::FILE* OpenForWriting(const std::string& path, const char* mode) {
  return CurrentDisk().Open(path, mode);
}

std::size_t Write(std::FILE* file, const void* data, std::size_t size) {
  return CurrentDisk().Write(file, data, size);
}

int Rename(const std::string& from, const std::string& to) {
  return CurrentDisk().Rename(from, to);
}

int Remove(const std::string& path) { return CurrentDisk().Remove(path); }

const char* ToString(Refusal refusal) {
  switch (refusal) {
    case Refusal::kNone:
      return "none";
    case Refusal::kFull:
      return "full";
    case Refusal::kFileTooLarge:
      return "file_too_large";
    case Refusal::kRefused:
      return "refused";
  }
  return "refused";
}

Refusal RefusalOf(int error_number) {
  switch (error_number) {
    case ENOSPC:
      return Refusal::kFull;
    case EFBIG:
      return Refusal::kFileTooLarge;
    default:
      return Refusal::kRefused;
  }
}

const char* Explain(Refusal refusal) {
  switch (refusal) {
    case Refusal::kNone:
      return "the card took the write";
    case Refusal::kFull:
      return "the card is full";
    case Refusal::kFileTooLarge:
      return "the file is larger than the card's format allows in one file";
    case Refusal::kRefused:
      return "the card refused the write";
  }
  return "the card refused the write";
}

}  // namespace rommsync::io
