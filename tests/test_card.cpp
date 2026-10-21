// `sysmodule/source/card.cpp`, compiled for the host (M9-13, #202).
//
// The console's `fs::FileSystem` was 191 lines that only devkitPro ever built.
// Nothing in it is Horizon-specific -- it is `<dirent.h>`, `<sys/stat.h>` and
// `<sys/statvfs.h>` over the `sdmc:` devoptab -- so it compiles here unchanged,
// rooted at a scratch directory instead of `sdmc:` through `MakeSdCardAt`, and
// the rules it and the host backend must agree on are checked against the file
// that ships rather than against its twin.
//
//   resolve  -- an SD path becomes `<mount>/...`; `..` and NUL are refused
//   list     -- names, kinds, sizes, mtimes; `.`/`..` never; missing vs a file
//   bounded  -- past `kMaxDirectoryEntries`, the first entries *by name* survive
//   mkdir    -- every parent; an existing directory is success; a file is not
//   volume   -- the per-file ceiling by format, and the free space on the card
//   fat32    -- given a mounted FAT32 image (tests/test_fat_volume.sh): the host
//               backend detects it, a 4 GiB file is EFBIG, a full image is
//               ENOSPC, and a download onto it is refused as `kDiskFull`
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "card.hpp"
#include "checks.hpp"
#include "rig.hpp"
#include "rommsync/disk.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/host/native_file_system.hpp"

namespace fs = rommsync::fs;
namespace io = rommsync::io;
namespace http = rommsync::http;

namespace {

/// A fresh directory standing in for `sdmc:`.
std::string Mount(const std::string& name) {
  const std::string path = rig::ScratchDir() + "/card-" + name;
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path;
}

void Put(const std::string& path, const std::string& bytes) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}

void Resolve(checks::Checks& c) {
  const std::string mount = Mount("resolve");
  const auto card = rommsync::sysmodule::MakeSdCardAt(mount, fs::Format::kUnknown);
  c.ExpectEq(card->Resolve("/retroarch/saves/Game.srm"), mount + "/retroarch/saves/Game.srm",
             "an SD path lands under the mount");
  c.ExpectEq(card->Resolve("/a/./b//c"), mount + "/a/b/c", "`.` and empty segments are dropped");
  c.ExpectEq(card->Resolve("/"), mount, "the root is the mount itself");
  c.ExpectEq(card->Resolve("/a/../b"), std::string(), "a `..` is refused, not resolved");
  c.ExpectEq(card->Resolve(std::string("/a\0b", 4)), std::string(), "and so is a NUL");
}

void List(checks::Checks& c) {
  const std::string mount = Mount("list");
  const auto card = rommsync::sysmodule::MakeSdCardAt(mount, fs::Format::kUnknown);
  Put(mount + "/saves/b.srm", "12345");
  Put(mount + "/saves/a.srm", "1");
  std::filesystem::create_directories(mount + "/saves/folder");

  const fs::Listing listing = card->List("/saves");
  c.Expect(listing.ok(), "a directory lists: " + listing.message);
  c.ExpectEq(listing.entries.size(), std::size_t{3}, "three entries, and no `.` or `..`");
  for (const fs::Entry& entry : listing.entries) {
    c.Expect(entry.name != "." && entry.name != "..", "never `.` or `..`");
    if (entry.name == "b.srm") {
      c.ExpectEq(entry.size_bytes, std::int64_t{5}, "a file's size is read");
      c.Expect(!entry.is_directory, "and it is a file");
      c.Expect(entry.modified_unix > 0, "with an mtime");
    } else if (entry.name == "folder") {
      c.Expect(entry.is_directory, "a directory is reported as one");
      c.ExpectEq(entry.size_bytes, std::int64_t{0}, "with no size");
    }
  }

  const fs::Listing missing = card->List("/nowhere");
  c.Expect(missing.error == fs::ListError::kMissing, "a folder that is not there is missing");
  const fs::Listing file = card->List("/saves/a.srm");
  c.Expect(file.error == fs::ListError::kNotADirectory, "a file is not a directory");
  const fs::Listing escape = card->List("/saves/../..");
  c.Expect(escape.error == fs::ListError::kMissing, "a path off the card is refused");
  c.Expect(!escape.message.empty(), "with a sentence");
}

void Bounded(checks::Checks& c) {
  const std::string mount = Mount("bounded");
  const auto card = rommsync::sysmodule::MakeSdCardAt(mount, fs::Format::kUnknown);
  // Created in *descending* name order, so a backend that kept what `readdir`
  // handed it first would keep the wrong ones on any filesystem that returns
  // creation order.
  const std::size_t total = fs::kMaxDirectoryEntries + 3;
  for (std::size_t i = total; i-- > 0;) {
    char name[32];
    std::snprintf(name, sizeof(name), "/many/%05zu", i);
    Put(mount + name, "");
  }
  const fs::Listing listing = card->List("/many");
  c.Expect(listing.error == fs::ListError::kTooManyEntries, "an oversized folder says so");
  c.ExpectEq(listing.entries.size(), fs::kMaxDirectoryEntries, "and holds exactly the bound");
  bool first_by_name = true;
  for (const fs::Entry& entry : listing.entries) {
    if (entry.name >= "04096") {
      first_by_name = false;
    }
  }
  c.Expect(first_by_name, "the entries kept are the first by name, whatever order readdir used");
}

void MakeDir(checks::Checks& c) {
  const std::string mount = Mount("mkdir");
  const auto card = rommsync::sysmodule::MakeSdCardAt(mount, fs::Format::kUnknown);
  c.Expect(card->CreateDirectory("/config/rommsync/.backup").ok(),
           "a directory three levels deep is created");
  c.Expect(std::filesystem::is_directory(mount + "/config/rommsync/.backup"),
           "with every parent it needed");
  c.Expect(card->CreateDirectory("/config/rommsync/.backup").ok(),
           "one that is already there is success");
  Put(mount + "/config/file", "x");
  c.Expect(card->CreateDirectory("/config/file").error == fs::MakeDirError::kNotADirectory,
           "a file in the way is named");
  c.Expect(card->CreateDirectory("/a/../b").error == fs::MakeDirError::kNotOnThisCard,
           "a path off the card is refused before anything is tried");

  // A card that will not take it. Not as root, which ignores the mode.
  if (::geteuid() != 0) {
    std::filesystem::create_directories(mount + "/locked");
    ::chmod((mount + "/locked").c_str(), 0555);
    const fs::MakeDirResult refused = card->CreateDirectory("/locked/inside");
    ::chmod((mount + "/locked").c_str(), 0755);
    c.Expect(refused.error == fs::MakeDirError::kUnwritable, "a refusing card is unwritable");
    c.Expect(refused.message.find("/locked/inside") != std::string::npos,
             "and the sentence names the SD path, not the mount");
  }
}

void Volume(checks::Checks& c) {
  const std::string mount = Mount("volume");
  std::filesystem::create_directories(mount + "/roms");

  // The format is the console's to say -- `main.cpp` asks libnx -- so what is
  // checked here is what the card does with the answer.
  const auto fat = rommsync::sysmodule::MakeSdCardAt(mount, fs::Format::kFat32);
  const fs::Volume fat_volume = fat->VolumeOf("/roms");
  c.Expect(fat_volume.format == fs::Format::kFat32, "a FAT32 card says so");
  c.ExpectEq(fat_volume.max_file_bytes, fs::kFat32MaxFileBytes, "and stops a file at 4 GiB - 1");
  c.Expect(fat_volume.free_bytes > 0, "and reports the space left on it");

  const auto unknown = rommsync::sysmodule::MakeSdCardAt(mount, fs::Format::kUnknown);
  c.ExpectEq(unknown->VolumeOf("/roms").max_file_bytes, fs::kFat32MaxFileBytes,
             "a card whose format could not be read is held to FAT32's ceiling, not none");

  const auto exfat = rommsync::sysmodule::MakeSdCardAt(mount, fs::Format::kExFat);
  c.ExpectEq(exfat->VolumeOf("/roms").max_file_bytes, std::uint64_t{0},
             "an exFAT card has no ceiling a rom reaches");

  const fs::Volume off = fat->VolumeOf("/a/../b");
  c.ExpectEq(off.free_bytes, std::int64_t{-1}, "a path off the card has no free space to report");
  const fs::Volume nowhere = fat->VolumeOf("/not/made/yet");
  c.ExpectEq(nowhere.free_bytes, std::int64_t{-1},
             "nor does a folder that does not exist -- unknown, which refuses nothing");
}

/// Given a FAT32 volume mounted at `mount` -- `test_fat_volume.sh` makes one.
void Fat32(checks::Checks& c, const std::string& mount) {
  const auto native = rommsync::host::MakeNativeFileSystem(mount);
  const fs::Volume volume = native->VolumeOf("/");
  c.Expect(volume.format == fs::Format::kFat32,
           std::string("the host backend reads the image as FAT32 -- got ") +
               fs::ToString(volume.format));
  c.ExpectEq(volume.max_file_bytes, fs::kFat32MaxFileBytes, "and holds a file to 4 GiB - 1");
  c.Expect(volume.free_bytes > 0, "with the space left on it");

  const auto card = rommsync::sysmodule::MakeSdCardAt(mount, fs::Format::kFat32);
  const fs::Volume console = card->VolumeOf("/");
  c.Expect(console.free_bytes > 0 && console.free_bytes <= volume.free_bytes + (1 << 20),
           "the console's card reads the same free space off the same image");

  // The ceiling, for real and without writing 4 GiB: one byte at offset
  // 4 GiB - 1, which would make the file 4 GiB. The seek goes *to* the ceiling
  // rather than past it, because Linux refuses a seek past a filesystem's
  // largest file with EINVAL, which is a different question.
  {
    const std::string path = mount + "/past-the-ceiling.bin";
    std::FILE* file = io::OpenForWriting(path, "wb");
    c.Expect(file != nullptr, "a file opens on the image");
    if (file != nullptr) {
      bool refused = std::fseek(file, 0xFFFFFFFFL, SEEK_SET) != 0;
      int why = errno;
      if (!refused) {
        errno = 0;
        const char byte = 'x';
        refused = io::Write(file, &byte, 1) != 1 || std::fflush(file) != 0;
        why = errno;
      }
      std::fclose(file);
      c.Expect(refused, "a byte that would make the file 4 GiB is refused");
      c.Expect(io::RefusalOf(why) == io::Refusal::kFileTooLarge,
               std::string("as a file too large, not a full card -- errno ") +
                   std::to_string(why));
    }
    std::remove(path.c_str());
  }

  // A real full card: fill it, then ask the download backend for a body.
  {
    const std::string filler = mount + "/filler.bin";
    std::FILE* file = io::OpenForWriting(filler, "wb");
    std::vector<char> block(1 << 20, '\0');
    int why = 0;
    while (file != nullptr) {
      errno = 0;
      if (io::Write(file, block.data(), block.size()) != block.size() || std::fflush(file) != 0) {
        why = errno;
        break;
      }
    }
    if (file != nullptr) {
      std::fclose(file);
    }
    c.Expect(io::RefusalOf(why) == io::Refusal::kFull,
             std::string("filling the image ends in a full card -- errno ") + std::to_string(why));

    const std::unique_ptr<http::HttpClient> client = rig::MakeClient();
    const std::string base = rig::BaseUrl();
    if (rig::Reachable(*client, base)) {
      const std::string asset = rig::DiscoverLargeAsset(*client, base);
      http::Request request;
      request.url = base + asset;
      request.timeout = std::chrono::milliseconds{0};
      const std::string destination = mount + "/rom.bin";
      const http::Result result = client->Download(request, rig::DownloadTo(destination));
      c.Expect(result.error == http::Error::kDiskFull,
               std::string("a download onto the full image is named a full card -- got ") +
                   http::ToString(result.error) + " (" + result.message + ")");
      c.Expect(!std::filesystem::exists(destination), "and leaves no file where the body goes");
      std::remove(http::PartialPathFor(destination).c_str());
    } else {
      std::cerr << "  (rig unreachable: the download onto a full image was not tried)\n";
    }
    std::remove(filler.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "resolve";
  checks::Checks checks;
  if (scenario == "resolve") {
    Resolve(checks);
  } else if (scenario == "list") {
    List(checks);
  } else if (scenario == "bounded") {
    Bounded(checks);
  } else if (scenario == "mkdir") {
    MakeDir(checks);
  } else if (scenario == "volume") {
    Volume(checks);
  } else if (scenario == "fat32" && argc > 2) {
    Fat32(checks, argv[2]);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() != 0) {
    std::cerr << "card." << scenario << ": " << checks.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << "card." << scenario << " ok\n";
  return 0;
}
