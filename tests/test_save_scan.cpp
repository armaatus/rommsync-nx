// Step 0 of docs/SYNC_PROTOCOL.md: the SD card turned into matched saves.
//
// The scenarios split the way the module does. The first four need no server
// because the thing they pin is a *decision* -- what a base name is, what a slot
// is, and above all that an ambiguous name is skipped rather than guessed. Those
// must stay checked with docker stopped: a wrong match is not a failed tick, it
// is one game's save uploaded over another game's, and that rule going quiet is
// the worst way for this file to rot.
//
// The last two need the real RomM 5.2.0, because the two things a fake index
// cannot answer are the two the issue asks about: that `GET /api/roms` answers
// an **envelope** which has to be paged -- a rom on the last page is exactly the
// one a save needs -- and that the fields this client matches on
// (`fs_name_no_ext`, `platform_fs_slug`) are the fields a live server actually
// sends for the seeded library.
//
//   names        -- base names, emulators, slots, and which folder implies what
//   ambiguous    -- one name on two platforms, in a hintless folder, is skipped
//   walked_once  -- RetroArch's flat saves/, listed under a dozen platforms
//   unusable     -- a file sync::Validate would refuse costs one file, not the tick
//   library      -- a Sandbox scanned against the docker library, end to end
//   paging       -- limit=1 over a six-rom library still finds the last page
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "harness.hpp"
#include "rommsync/config.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/save_scan.hpp"
#include "rommsync/sync.hpp"

namespace {

namespace config = rommsync::config;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace roms = rommsync::roms;
namespace scan = rommsync::scan;
namespace sync = rommsync::sync;

using harness::Fixture;
using harness::Sandbox;

/// An mtime any card would produce: 2024-01-01T00:00:00Z. Anything non-zero
/// does, but a fixed one keeps a failure message readable.
constexpr std::int64_t kMtime = 1704067200;

// --- a directory listing without a directory ---------------------------------

/// The `FileSystem` the decision scenarios scan.
///
/// It also *counts* the calls, which is what makes `walked_once` a check rather
/// than an assumption: the deduplication it proves lives in
/// `config::SaveScanDirs()`, and a scanner that walked the folder map directly
/// would still produce the right records -- just a dozen copies of each.
class FakeFileSystem final : public fs::FileSystem {
 public:
  void AddFile(const std::string& directory, const std::string& name, std::int64_t size,
               std::int64_t modified) {
    fs::Entry entry;
    entry.name = name;
    entry.size_bytes = size;
    entry.modified_unix = modified;
    listings_[directory].entries.push_back(std::move(entry));
  }

  void AddDirectory(const std::string& directory, const std::string& name) {
    fs::Entry entry;
    entry.name = name;
    entry.is_directory = true;
    listings_[directory].entries.push_back(std::move(entry));
  }

  fs::Listing List(std::string_view sd_path) override {
    const std::string key(sd_path);
    ++calls_[key];
    const auto found = listings_.find(key);
    if (found == listings_.end()) {
      fs::Listing missing;
      missing.error = fs::ListError::kMissing;
      missing.message = key + ": no such directory";
      return missing;
    }
    return found->second;
  }

  int CallsFor(const std::string& directory) const {
    const auto found = calls_.find(directory);
    return found == calls_.end() ? 0 : found->second;
  }

 private:
  std::map<std::string, fs::Listing> listings_;
  std::map<std::string, int> calls_;
};

roms::Rom MakeRom(std::int64_t id, std::string name, std::string platform) {
  roms::Rom rom;
  rom.id = id;
  rom.fs_name_no_ext = name;
  rom.fs_name_no_tags = std::move(name);
  rom.platform_fs_slug = std::move(platform);
  return rom;
}

/// The `SaveFile` for `file_name`, or nullptr. Scenarios assert on fields rather
/// than on positions, so a change in scan order is not a red run by itself.
const scan::SaveFile* FindSave(const scan::ScanResult& result, std::string_view file_name) {
  for (const scan::SaveFile& save : result.saves) {
    if (save.file_name == file_name) {
      return &save;
    }
  }
  return nullptr;
}

bool SkippedFor(const scan::ScanResult& result, std::string_view sd_path,
                scan::SkipReason reason) {
  for (const scan::Skip& skip : result.skipped) {
    if (skip.sd_path == sd_path && skip.reason == reason) {
      return true;
    }
  }
  return false;
}

// --- names --------------------------------------------------------------------

void Names(::checks::Checks& checks) {
  checks.ExpectEq(scan::BaseName("Game (USA).srm"), std::string("Game (USA)"),
                  "one extension is stripped");
  checks.ExpectEq(scan::BaseName("Game"), std::string("Game"),
                  "a name with no extension is its own base");
  // Not `""`: a dotfile's whole name is its name, and an empty base would match
  // nothing on a real library and everything on a corrupt one.
  checks.ExpectEq(scan::BaseName(".DS_Store"), std::string(".DS_Store"),
                  "a leading dot is not an extension");
  // The honest answer. Stripping until something matches is how `Game.nes.srm`
  // becomes a match for a `Game.nes` the user does not have.
  checks.ExpectEq(scan::BaseName("Game.state.auto"), std::string("Game.state"),
                  "only the last extension is stripped");

  checks.ExpectEq(scan::EmulatorFor("/retroarch/saves"), std::string("retroarch"),
                  "the default RetroArch folder names its emulator");
  checks.ExpectEq(scan::EmulatorFor("/tico/saves/gba"), std::string("tico"),
                  "so does Tico's");
  checks.ExpectEq(scan::EmulatorFor("/emulators/RetroArch/saves"), std::string("retroarch"),
                  "any segment, and FAT32 is case-insensitive");
  checks.ExpectEq(scan::EmulatorFor("/saves"), std::string(),
                  "a folder that names no emulator implies none");

  checks.ExpectEq(scan::SlotFor("retroarch", "Game (USA).srm"), std::string("retroarch-srm"),
                  "the slot is the emulator and the extension");
  // The reason the emulator is in there at all: RomM pairs on `(rom_id, slot)`
  // alone, so a RetroArch `.srm` and a Tico `.srm` for one rom that both mapped
  // to `srm` would overwrite each other through the server, forever.
  checks.Expect(scan::SlotFor("tico", "Game (USA).srm") != scan::SlotFor("retroarch",
                                                                         "Game (USA).srm"),
                "two emulators' .srm for one rom are different slots");
  checks.ExpectEq(scan::SlotFor("retroarch", "Game"), std::string("retroarch-save"),
                  "a save with no extension still gets a stable, non-empty slot");
  checks.ExpectEq(scan::SlotFor("retroarch", "Game.SRM"), std::string("retroarch-srm"),
                  "the slot does not change with the case the card happened to use");

  const config::Config defaults = config::Defaults();
  const std::map<std::string, std::string, std::less<>> hints = scan::PlatformHints(defaults);
  const auto flat = hints.find("/retroarch/saves");
  checks.Expect(flat != hints.end() && flat->second.empty(),
                "the flat RetroArch folder is listed under many platforms and implies none");
  const auto tico = hints.find("/tico/saves/gba");
  checks.Expect(tico != hints.end() && tico->second == "gba",
                "a folder listed under exactly one platform implies it");
}

// --- ambiguous ----------------------------------------------------------------

void Ambiguous(::checks::Checks& checks) {
  roms::RomIndex index;
  index.Add(MakeRom(1, "Rampart", "nes"));
  index.Add(MakeRom(2, "Rampart", "snes"));
  index.Add(MakeRom(3, "Solstice", "nes"));

  config::Config config = config::Defaults();
  FakeFileSystem files;
  files.AddFile("/retroarch/saves", "Rampart.srm", 32768, kMtime);
  files.AddFile("/retroarch/saves", "Solstice.srm", 8192, kMtime);
  // The same name again, this time under a folder mapped to exactly one
  // platform. Nothing about the *name* changed; the folder is the whole
  // difference, which is the point.
  files.AddFile("/tico/saves/snes", "Rampart.srm", 32768, kMtime);

  const scan::ScanResult result = scan::ScanSaves(config, index, files);

  checks.ExpectEq(result.files_seen, static_cast<std::size_t>(3), "three files were looked at");
  checks.Expect(SkippedFor(result, "/retroarch/saves/Rampart.srm", scan::SkipReason::kAmbiguous),
                "a name on two platforms in a hintless folder is skipped, not guessed");
  checks.Expect(FindSave(result, "Rampart.srm") != nullptr &&
                    FindSave(result, "Rampart.srm")->rom_id == 2,
                "the same name under /tico/saves/snes is the SNES rom");
  checks.ExpectEq(result.saves.size(), static_cast<std::size_t>(2),
                  "the unambiguous files still sync");

  // The skip has to be actionable: a user resolves this by mapping a folder, so
  // the message names the platforms it landed between.
  for (const scan::Skip& skip : result.skipped) {
    if (skip.reason != scan::SkipReason::kAmbiguous) {
      continue;
    }
    checks.Expect(skip.message.find("nes") != std::string::npos &&
                      skip.message.find("snes") != std::string::npos,
                  "the reason names the platforms the name landed on");
    checks.Expect(skip.Describe().find("Rampart.srm") != std::string::npos,
                  "and the file it is about");
  }
  checks.Expect(result.DescribeSkipped().find("ambiguous") != std::string::npos,
                "the log line says what kind of skip it was");
}

// --- walked_once --------------------------------------------------------------

void WalkedOnce(::checks::Checks& checks) {
  roms::RomIndex index;
  index.Add(MakeRom(1, "gb240p", "gb"));

  const config::Config config = config::Defaults();
  // The default map lists /retroarch/saves under every platform it knows, which
  // is not a quirk of the defaults -- RetroArch really does keep one flat saves
  // folder for every system.
  std::size_t listed_under = 0;
  for (const auto& [slug, folders] : config.platforms) {
    (void)slug;
    for (const std::string& directory : folders.saves) {
      if (directory == "/retroarch/saves") {
        ++listed_under;
      }
    }
  }
  checks.Expect(listed_under > 1, "the fixture for this scenario: many platforms, one folder");

  FakeFileSystem files;
  files.AddFile("/retroarch/saves", "gb240p.srm", 512, kMtime);
  files.AddDirectory("/retroarch/saves", "RetroArch-Core-Options");

  const scan::ScanResult result = scan::ScanSaves(config, index, files);

  checks.ExpectEq(files.CallsFor("/retroarch/saves"), 1,
                  "a folder listed under a dozen platforms is read once");
  checks.ExpectEq(result.saves.size(), static_cast<std::size_t>(1), "and yields one record");
  checks.ExpectEq(result.files_seen, static_cast<std::size_t>(1),
                  "a subdirectory is not a save; the walk does not descend");
}

// --- unusable -----------------------------------------------------------------

void Unusable(::checks::Checks& checks) {
  roms::RomIndex index;
  index.Add(MakeRom(1, "gb240p", "gb"));
  index.Add(MakeRom(2, "nova", "nes"));

  const config::Config config = config::Defaults();
  FakeFileSystem files;
  // An mtime of 0 is what a console with an unset clock reports. The server
  // would read it as "very old" and answer with a download over a save that may
  // be the only copy, so `sync::Validate` refuses it -- and this scenario is
  // that refusal costing one file rather than the tick, since
  // `EncodeNegotiateRequest` stops at the first bad entry.
  files.AddFile("/retroarch/saves", "gb240p.srm", 512, 0);
  files.AddFile("/retroarch/saves", "nova.srm", 512, kMtime);

  const scan::ScanResult result = scan::ScanSaves(config, index, files);

  checks.Expect(SkippedFor(result, "/retroarch/saves/gb240p.srm", scan::SkipReason::kUnusable),
                "a save with an unset mtime is skipped with a reason");
  checks.ExpectEq(result.saves.size(), static_cast<std::size_t>(1),
                  "and the other save still syncs");

  sync::SyncNegotiatePayload payload;
  for (const scan::SaveFile& save : result.saves) {
    payload.saves.push_back(save.ToClientSaveState());
  }
  const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
  checks.Expect(encoded.ok(), "what the scan did emit encodes: " + encoded.error.Describe());

  // Two files for one rom in one folder land on the same `(rom_id, slot)`, which
  // the server pairs on -- so they would overwrite each other through RomM on
  // alternating ticks. The first in scan order keeps it.
  FakeFileSystem duplicates;
  duplicates.AddFile("/retroarch/saves", "nova.srm", 512, kMtime);
  duplicates.AddFile("/tico/saves/nes", "nova.srm", 512, kMtime);
  duplicates.AddFile("/tico/saves/nes", "nova.sav", 512, kMtime);
  const scan::ScanResult mixed = scan::ScanSaves(config, index, duplicates);
  checks.ExpectEq(mixed.saves.size(), static_cast<std::size_t>(3),
                  "the same rom under two emulators, and two extensions, are three slots");

  // Two spellings of one extension in one folder are one slot, because the slot
  // is lowercased. The entries are sorted by name, so `nova.SRM` is seen first
  // (`S` sorts before `s`) and keeps the slot; which one wins matters less than
  // that the same one wins on every tick.
  duplicates.AddFile("/retroarch/saves", "nova.SRM", 512, kMtime);
  const scan::ScanResult clashing = scan::ScanSaves(config, index, duplicates);
  checks.Expect(SkippedFor(clashing, "/retroarch/saves/nova.srm", scan::SkipReason::kDuplicateSlot),
                "a second file claiming a taken (rom_id, slot) is reported, not synced");
  checks.ExpectEq(clashing.saves.size(), mixed.saves.size(),
                  "and the clash costs exactly the one file");
  const scan::ScanResult replayed = scan::ScanSaves(config, index, duplicates);
  checks.Expect(SkippedFor(replayed, "/retroarch/saves/nova.srm", scan::SkipReason::kDuplicateSlot),
                "the same file loses the slot on the next tick, not the other one");
}

// --- library ------------------------------------------------------------------

void Library(::checks::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture) {
  roms::FetchOptions options;
  options.base_url = base;
  options.bearer_token = fixture.token;
  const roms::FetchResult fetched = roms::FetchRomIndex(client, options);
  checks.Expect(fetched.ok(), "the rom index fetched: " + fetched.message);
  if (!fetched.ok()) {
    return;
  }
  checks.Expect(fetched.index.size() >= 4,
                "the seeded library is there; re-seed with ./server/testing/seed.sh");

  Sandbox sandbox(checks, "save-scan");
  sandbox.MakeDirs("/tico/saves/gba");
  // Named after the seeded roms' `fs_name_no_ext`, because that is what an
  // emulator writes: it names a save after the rom file it loaded.
  sandbox.Write("/retroarch/saves/240pee.srm", "nes sram");
  sandbox.Write("/retroarch/saves/gb240p.srm", "gb sram");
  sandbox.Write("/retroarch/saves/nova.sav", "nes sram, other extension");
  sandbox.Write("/retroarch/saves/notes.txt", "not a save of anything");
  sandbox.Write("/tico/saves/gba/240pee_mb.srm", "gba sram");

  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const config::Config config = config::Defaults();
  const scan::ScanResult result = scan::ScanSaves(config, fetched.index, *files);

  checks.ExpectEq(result.files_seen, static_cast<std::size_t>(5), "every file was looked at");
  checks.ExpectEq(result.saves.size(), static_cast<std::size_t>(4),
                  "four of them are saves of roms in the library");
  // One record per file even though /retroarch/saves is listed under every
  // platform the defaults know.
  checks.ExpectEq(result.skipped_total, static_cast<std::size_t>(1), "and one is not");
  checks.Expect(SkippedFor(result, "/retroarch/saves/notes.txt", scan::SkipReason::kUnmatched),
                "the file that matches no rom is reported by name");

  struct Expected {
    const char* file_name;
    const char* rom_base;
    const char* platform;
    const char* emulator;
    const char* slot;
  };
  const Expected kExpected[] = {
      {"240pee.srm", "240pee", "nes", "retroarch", "retroarch-srm"},
      {"gb240p.srm", "gb240p", "gb", "retroarch", "retroarch-srm"},
      {"nova.sav", "nova", "nes", "retroarch", "retroarch-sav"},
      {"240pee_mb.srm", "240pee_mb", "gba", "tico", "tico-srm"},
  };
  for (const Expected& expected : kExpected) {
    const scan::SaveFile* save = FindSave(result, expected.file_name);
    if (save == nullptr) {
      checks.Expect(false, std::string(expected.file_name) + " was not matched to a rom");
      continue;
    }
    const roms::Rom* rom = fetched.index.ById(save->rom_id);
    checks.Expect(rom != nullptr, std::string(expected.file_name) + " names a rom in the index");
    if (rom != nullptr) {
      checks.ExpectEq(rom->fs_name_no_ext, std::string(expected.rom_base),
                      "matched on the live server's fs_name_no_ext");
      checks.ExpectEq(rom->platform_fs_slug, std::string(expected.platform),
                      "and it is the rom on the right platform");
    }
    checks.ExpectEq(save->platform_fs_slug, std::string(expected.platform),
                    "the record carries the rom's platform");
    checks.ExpectEq(save->emulator, std::string(expected.emulator),
                    "and the emulator the folder it came from belongs to");
    checks.ExpectEq(save->slot, std::string(expected.slot), "and a slot derived from both");
    checks.Expect(save->modified_unix > 0, "and the mtime the card gave up");
    checks.Expect(save->size_bytes > 0, "and its size");
    checks.Expect(save->file_name.find('/') == std::string::npos,
                  "file_name is a name; the server joins it into a path");
    checks.Expect(save->sd_path.find(save->file_name) != std::string::npos,
                  "and sd_path is where the scan found it");
  }

  // The whole point of the module: what it emits is a negotiate body.
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  for (const scan::SaveFile& save : result.saves) {
    payload.saves.push_back(save.ToClientSaveState());
  }
  const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
  checks.Expect(encoded.ok(), "every emitted record encodes: " + encoded.error.Describe());
  checks.Expect(encoded.body.find("\"content_hash\":null") != std::string::npos ||
                    encoded.body.find("content_hash") == std::string::npos,
                "the hash is left null for M2-3 rather than invented here");

  // A second scan of an unchanged card must produce the same slots, or the
  // server sees a new save every tick.
  const scan::ScanResult again = scan::ScanSaves(config, fetched.index, *files);
  checks.ExpectEq(again.saves.size(), result.saves.size(), "a rescan sees the same saves");
  for (const scan::SaveFile& save : again.saves) {
    const scan::SaveFile* first = FindSave(result, save.file_name);
    checks.Expect(first != nullptr && first->slot == save.slot && first->rom_id == save.rom_id,
                  "and pairs each one with the same (rom_id, slot)");
  }
}

// --- paging -------------------------------------------------------------------

void Paging(::checks::Checks& checks, http::HttpClient& client, const std::string& base,
            const Fixture& fixture) {
  roms::FetchOptions whole;
  whole.base_url = base;
  whole.bearer_token = fixture.token;
  const roms::FetchResult all = roms::FetchRomIndex(client, whole);
  checks.Expect(all.ok(), "the library fetched in one page: " + all.message);
  if (!all.ok() || all.index.empty()) {
    checks.Expect(false, "the fixture library is empty; scan it with provision.py");
    return;
  }

  // The rom the last page holds. A client that reads `items` and stops has the
  // first page only, and this is the save that then never matches.
  const roms::Rom& last = all.index.roms().back();

  roms::FetchOptions paged = whole;
  paged.page_size = 1;
  const roms::FetchResult one_at_a_time = roms::FetchRomIndex(client, paged);
  checks.Expect(one_at_a_time.ok(), "a limit of one still fetched: " + one_at_a_time.message);
  checks.ExpectEq(one_at_a_time.index.size(), all.index.size(),
                  "paging by one finds the whole library");
  checks.Expect(!one_at_a_time.index.truncated(), "and does not stop early");

  const roms::Match found =
      one_at_a_time.index.Find(last.fs_name_no_ext, last.platform_fs_slug);
  checks.Expect(found.matched(), "a rom on the last page is matched: " + found.reason);
  if (found.matched()) {
    checks.ExpectEq(found.rom->id, last.id, "and it is the same rom");
  }

  // The envelope itself, because the mistake this guards is reading the body as
  // a bare array -- which parses as nothing and reads as "the library ended".
  roms::Page page;
  const rommsync::json::Error error = roms::ParsePage("[{\"id\":1}]", &page);
  checks.Expect(!error.ok(), "a bare array is refused by name, not read as an empty library");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "names";
  const std::string base = rig::BaseUrl();

  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);

  // The tally lives above every scenario for the reason test_harness.cpp spells
  // out: a `Sandbox` reports its teardown audit into it.
  rig::Checks checks;

  if (scenario == "names" || scenario == "ambiguous" || scenario == "walked_once" ||
      scenario == "unusable") {
    if (scenario == "names") {
      Names(checks);
    } else if (scenario == "ambiguous") {
      Ambiguous(checks);
    } else if (scenario == "walked_once") {
      WalkedOnce(checks);
    } else {
      Unusable(checks);
    }
    if (checks.failures() == 0) {
      std::cout << "scan." << scenario << " ok\n";
    }
    return checks.failures() == 0 ? 0 : 1;
  }

  const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  rig::DisarmFault(*client, base);

  Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    return rig::kSkip;
  }

  if (scenario == "library") {
    Library(checks, *client, base, fixture);
  } else if (scenario == "paging") {
    Paging(checks, *client, base, fixture);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() == 0) {
    std::cout << "scan." << scenario << " ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
