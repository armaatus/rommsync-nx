// The save-overwrite protocol on lazyfs, a filesystem with a page cache it can
// be told to lose (#250). The driver half; tests/test_lazyfs.sh mounts lazyfs,
// arms its faults and calls this.
//
// `tests/crash_disk.hpp` is a model: it records what an overwrite asked of the
// disk and enumerates what a card that does not journal may have kept. Whether
// that model is faithful to a real page cache is a question a model cannot
// answer about itself. lazyfs (dsrhaslab/lazyfs, VLDB'24 "When Amnesia
// Strikes") is a FUSE filesystem that holds every write in its own cache until
// an `fsync` and can be told to drop what is unsynced -- a power cut -- or to
// persist half of one write, or some of a run of writes, and die. So the same
// protocol runs on it, cut at every call, and the same judge reads what is left.
//
//   test_lazyfs seed CARD           the card before the overwrite: the save, and
//                                   the folders it and its backup go in. Written
//                                   to lazyfs's *root*, before it is mounted, so
//                                   it is durable by construction.
//   test_lazyfs run CARD CUT FIFO DONE [overwrite|download]
//                                   the overwrite on the mount at CARD:
//                                   `overwrite` is `BackUpFirst` then
//                                   `WriteAtomically` (a restore, a state);
//                                   `download` is a download's order -- the
//                                   body staged in 16 KiB writes as the host
//                                   backend stages it, then `BackUpFirst`, then
//                                   `CommitStaged`. Before its CUT-th call to
//                                   `io::Disk` (1-based), it tells lazyfs to drop
//                                   its unsynced data (FIFO), waits for lazyfs to
//                                   say it has (DONE), and dies: the power is
//                                   cut. CUT 0 cuts nothing and prints how many
//                                   calls there were; one past the last cuts
//                                   after the overwrite has returned.
//   test_lazyfs judge CARD [new]    after the boot sweep, read lazyfs's root: the
//                                   save is its old bytes or its new ones, and
//                                   the new ones only beside a backup of the old
//                                   (`crash::OldOrNew`). With `new`, it must be
//                                   the new bytes: the overwrite returned, so it
//                                   is on the card.
//   test_lazyfs backup              the backup's file name, for a torn fault
//                                   aimed at it.
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "crash_disk.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/disk.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/sync_execute.hpp"
#include "rommsync/sync_tick.hpp"

namespace {

namespace io = rommsync::io;
namespace rsync = rommsync::sync;

constexpr const char* kSave = "/retroarch/saves/crash.srm";
constexpr const char* kSavesDir = "/retroarch/saves";
constexpr const char* kBackupDir = "/config/rommsync/.backup";
constexpr std::int64_t kWhen = 1'757'000'100;

/// 256 KiB each -- a large SRAM save -- so the staged save reaches lazyfs as
/// more than one write (FUSE's `max_write` is 128 KiB): `torn-seq` only tears a
/// run of two or more, and a run of one is nothing to tear.
std::string Bytes(char seed) {
  std::string bytes(256 * 1024, '\0');
  for (std::size_t at = 0; at < bytes.size(); ++at) {
    bytes[at] = static_cast<char>(seed + static_cast<char>(at % 61));
  }
  return bytes;
}

const std::string& OldBytes() {
  static const std::string bytes = Bytes('a');
  return bytes;
}
const std::string& NewBytes() {
  static const std::string bytes = Bytes('A');
  return bytes;
}

std::string Host(const std::string& card, const std::string& sd_path) {
  return (std::filesystem::path(card) / crash::RelativeOf(sd_path)).string();
}

/// Forwards every call, and cuts the power in front of one of them.
class Cutter final : public io::Disk {
 public:
  Cutter(int cut, std::string fifo, std::string done)
      : cut_(cut), fifo_(std::move(fifo)), done_(std::move(done)) {
    previous_ = io::SetDisk(this);
  }
  ~Cutter() override { io::SetDisk(previous_); }

  std::FILE* Open(const std::string& path, const char* mode) override {
    Next();
    return Inner().Open(path, mode);
  }
  std::size_t Write(std::FILE* file, const void* data, std::size_t size) override {
    Next();
    return Inner().Write(file, data, size);
  }
  int Rename(const std::string& from, const std::string& to) override {
    Next();
    return Inner().Rename(from, to);
  }
  int Remove(const std::string& path) override {
    Next();
    return Inner().Remove(path);
  }
  bool Sync(const std::string& path) override {
    Next();
    return Inner().Sync(path);
  }

  int calls() const { return calls_; }

  /// Drop what lazyfs holds unsynced, wait until it has, and die without
  /// another word to the disk: no destructor, no `fclose` flushing a buffer.
  [[noreturn]] void Cut() const {
    std::cout << "the power goes after " << calls_ << " calls" << std::endl;
    std::FILE* command = std::fopen(fifo_.c_str(), "w");
    if (command == nullptr) {
      std::cerr << "cannot open lazyfs's fault fifo " << fifo_ << "\n";
      ::_exit(2);
    }
    std::fputs("lazyfs::clear-cache\n", command);
    std::fclose(command);
    std::FILE* answer = std::fopen(done_.c_str(), "r");
    char line[128] = {};
    const bool cleared = answer != nullptr && std::fgets(line, sizeof line, answer) != nullptr &&
                         std::string(line).rfind("finished::clear-cache", 0) == 0;
    if (!cleared) {
      std::cerr << "lazyfs did not say it had cleared its cache\n";
      ::_exit(2);
    }
    ::_exit(10);
  }

 private:
  void Next() {
    ++calls_;
    if (calls_ == cut_) {
      --calls_;  // the call in front of which the power goes is not made
      Cut();
    }
  }
  io::Disk& Inner() const { return previous_ != nullptr ? *previous_ : io::StdioDisk(); }

  int cut_;
  std::string fifo_;
  std::string done_;
  int calls_ = 0;
  io::Disk* previous_ = nullptr;
};

int Seed(const std::string& card) {
  std::error_code error;
  std::filesystem::create_directories(Host(card, kSavesDir), error);
  std::filesystem::create_directories(Host(card, kBackupDir), error);
  std::FILE* file = std::fopen(Host(card, kSave).c_str(), "wb");
  if (file == nullptr) {
    return 1;
  }
  const bool wrote = std::fwrite(OldBytes().data(), 1, OldBytes().size(), file) == OldBytes().size();
  return std::fclose(file) == 0 && wrote ? 0 : 1;
}

/// What the host backend does with a download's body, through the same calls:
/// `<staged>.part` in 16 KiB writes, closed, synced, and renamed onto `staged`.
/// `synced` false leaves the sync out, which is the bug #203 found in that
/// backend -- and what `unsynced` runs, so the check is seen to catch it.
bool Fetch(const std::string& staged, bool synced) {
  const std::string partial = staged + ".part";
  std::FILE* file = io::OpenForWriting(partial, "wb");
  if (file == nullptr) {
    return false;
  }
  bool wrote = true;
  constexpr std::size_t kChunk = 16 * 1024;
  for (std::size_t at = 0; wrote && at < NewBytes().size(); at += kChunk) {
    const std::size_t size = std::min(kChunk, NewBytes().size() - at);
    wrote = io::Write(file, NewBytes().data() + at, size) == size;
  }
  return std::fclose(file) == 0 && wrote && (!synced || io::Sync(partial)) &&
         io::Rename(partial, staged) == 0;
}

/// `overwrite` is `BackUpFirst` then `WriteAtomically`: a restore, a state.
/// `download` is a download's order (sync_execute.cpp): fetch to the save's
/// staging file, back up, then `CommitStaged`. `unsynced` is `download` with
/// the body never synced, which the judge must refuse at some cut.
int Run(const std::string& card, int cut, const std::string& fifo, const std::string& done,
        const std::string& protocol) {
  const std::unique_ptr<rommsync::fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(card);
  Cutter cutter(cut, fifo, done);
  const std::string staged = io::TempPathFor(Host(card, kSave));
  const bool fetching = protocol != "overwrite";
  const bool fetched = !fetching || Fetch(staged, protocol == "download");
  std::string backup;
  std::string message;
  rsync::OperationError backed_up = rsync::OperationError::kNone;
  if (fetched) {
    backed_up = rsync::BackUpFirst(*files, kBackupDir, kSave, 7, "slot-1", "crash.srm", kWhen,
                                   &backup, &message);
  }
  bool replaced = false;
  if (fetched && backed_up == rsync::OperationError::kNone) {
    replaced = fetching
                   ? io::CommitStaged(staged, Host(card, kSave)).ok()
                   : io::WriteAtomically(Host(card, kSave), NewBytes()).ok();
  }
  if (cut == cutter.calls() + 1) {
    cutter.Cut();  // after the last call: what the overwrite returned is on the card
  }
  std::cout << "calls " << cutter.calls() << "\n";
  if (backed_up != rsync::OperationError::kNone || !replaced) {
    // A lazyfs that has torn a write and died answers ENOTCONN from here on;
    // the judge decides whether what it left is acceptable.
    std::cout << "the overwrite did not finish: " << rsync::ToString(backed_up) << " " << message
              << "\n";
    return 3;
  }
  return 0;
}

int Judge(const std::string& card, bool must_be_new) {
  const std::unique_ptr<rommsync::fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(card);
  rsync::RecoverStaging(*files, {kSavesDir, kBackupDir});
  crash::Snapshot before;
  before.dirs = {"", "retroarch", "retroarch/saves", "config", "config/rommsync",
                 "config/rommsync/.backup"};
  before.files[crash::RelativeOf(kSave)] = OldBytes();
  const crash::Tree after = crash::Read(card).files;
  std::string verdict = crash::OldOrNew({kSave, OldBytes(), NewBytes(), kBackupDir})(after, before);
  if (verdict.empty() && must_be_new) {
    const auto save = after.find(crash::RelativeOf(kSave));
    if (save == after.end() || save->second != NewBytes()) {
      verdict = "the overwrite returned, and the save is not its new bytes";
    }
  }
  if (!verdict.empty()) {
    std::cout << "FAIL: " << verdict << "\n";
    for (const auto& [path, bytes] : after) {
      std::cout << "  " << path << ": " << bytes.size() << " bytes\n";
    }
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  rommsync::host::InstallPosixFileSync();
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "seed" && argc == 3) {
    return Seed(argv[2]);
  }
  if (mode == "run" && (argc == 6 || argc == 7)) {
    const std::string protocol = argc == 7 ? argv[6] : "overwrite";
    if (protocol != "overwrite" && protocol != "download" && protocol != "unsynced") {
      std::cerr << "no such protocol: " << protocol << "\n";
      return 2;
    }
    return Run(argv[2], std::atoi(argv[3]), argv[4], argv[5], protocol);
  }
  if (mode == "judge" && (argc == 3 || argc == 4)) {
    return Judge(argv[2], argc == 4 && std::string(argv[3]) == "new");
  }
  if (mode == "backup" && argc == 2) {
    std::cout << rsync::BackupFileName(7, std::string("slot-1"), "crash.srm", kWhen) << "\n";
    return 0;
  }
  std::cerr << "usage: test_lazyfs seed CARD | run CARD CUT FIFO DONE [overwrite|download|unsynced] | "
               "judge CARD [new] | backup\n";
  return 2;
}
