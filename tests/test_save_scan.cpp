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
//   truncated    -- a directory past the bound keeps a set that does not move
//   hashing      -- the digest is real, reused when the file is unchanged, and
//                   null only when the bytes could not be read
//   library      -- a Sandbox scanned against the docker library, end to end
//   paging       -- limit=1 over a six-rom library still finds the last page
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
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
#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"

namespace {

namespace config = rommsync::config;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace roms = rommsync::roms;
namespace scan = rommsync::scan;
namespace state = rommsync::state;
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
  /// The listings are made up; the *files* are real, under a temp root, because
  /// the scan hashes what it reports and a fake that cannot be opened would
  /// exercise only the failure path.
  explicit FakeFileSystem(std::string_view label) {
    static int serial = 0;
    root_ = std::filesystem::path(rig::ScratchDir()) / "fake-fs" /
            (std::string(label) + "-" + std::to_string(serial++));
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    std::filesystem::create_directories(root_, error);
  }

  ~FakeFileSystem() override {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  FakeFileSystem(const FakeFileSystem&) = delete;
  FakeFileSystem& operator=(const FakeFileSystem&) = delete;

  void AddFile(const std::string& directory, const std::string& name, std::int64_t size,
               std::int64_t modified) {
    fs::Entry entry;
    entry.name = name;
    entry.size_bytes = size;
    entry.modified_unix = modified;
    listings_[directory].entries.push_back(std::move(entry));

    // Bytes that differ per name, so two saves never share a digest by accident.
    std::string bytes(static_cast<std::size_t>(size < 0 ? 0 : size), '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<char>('a' + ((index + name.size()) % 26));
    }
    const std::string path = Resolve(directory + "/" + name);
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    rig::WriteFile(path, bytes);
  }

  /// A file the listing advertises and the card will not open, which is what
  /// makes a save reportable but unhashable.
  void AddUnreadableFile(const std::string& directory, const std::string& name,
                         std::int64_t modified) {
    fs::Entry entry;
    entry.name = name;
    entry.size_bytes = 16;
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

  /// The scanner never creates a directory -- `sync::RunTick` does, for
  /// `.backup/` (#16) -- so this is the interface's requirement met and nothing
  /// more. It makes the real directory, because `Resolve` hands out real paths
  /// and a fake that only remembered would let a caller write into nothing.
  fs::MakeDirResult CreateDirectory(std::string_view sd_path) override {
    fs::MakeDirResult result;
    const std::string path = Resolve(sd_path);
    if (path.empty()) {
      result.error = fs::MakeDirError::kNotOnThisCard;
      result.message = std::string(sd_path) + ": not a path on this card";
      return result;
    }
    std::error_code error;
    std::filesystem::create_directories(path, error);
    std::error_code again;
    if (error && !std::filesystem::is_directory(path, again)) {
      result.error = fs::MakeDirError::kUnwritable;
      result.message = std::string(sd_path) + ": " + error.message();
    }
    return result;
  }

  std::string Resolve(std::string_view sd_path) const override {
    // The same refusal the interface requires and the native backend makes: a
    // fake that resolved a `..` would let a test pass over a backend that does
    // not.
    if (sd_path.find("..") != std::string_view::npos || sd_path.find('\0') != std::string_view::npos) {
      return {};
    }
    std::string relative(sd_path);
    while (!relative.empty() && relative.front() == '/') {
      relative.erase(relative.begin());
    }
    return (root_ / relative).string();
  }

  int CallsFor(const std::string& directory) const {
    const auto found = calls_.find(directory);
    return found == calls_.end() ? 0 : found->second;
  }

 private:
  std::filesystem::path root_;
  std::map<std::string, fs::Listing> listings_;
  std::map<std::string, int> calls_;
};

/// A baseline with nothing in it -- a first boot, where every save is read and
/// hashed. Scenarios that care about the *reuse* build their own.
const state::Baseline kFirstTick;

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

  // The scan must not emit more saves than a baseline can hold: `SaveBaseline`
  // *refuses* a file with more rows than `state::kMaxRecords`, so a card past
  // that bound would write no baseline at all and re-hash the whole library on
  // every tick, silently. The two constants move together (state_db.hpp).
  checks.ExpectEq(scan::kMaxSaves, state::kMaxRecords,
                  "the scan's bound is what state.db can persist");

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
  FakeFileSystem files("files");
  files.AddFile("/retroarch/saves", "Rampart.srm", 32768, kMtime);
  files.AddFile("/retroarch/saves", "Solstice.srm", 8192, kMtime);
  // The same name again, this time under a folder mapped to exactly one
  // platform. Nothing about the *name* changed; the folder is the whole
  // difference, which is the point.
  files.AddFile("/tico/saves/snes", "Rampart.srm", 32768, kMtime);

  const scan::ScanResult result = scan::ScanSaves(config, index, files, kFirstTick);

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

  // A short index changes what "unmatched" means: it may be a rom that was
  // never read rather than one the user does not have, and a scan that reported
  // it flatly would send them hunting for a library problem that is really a
  // client bound.
  roms::RomIndex partial;
  partial.Add(MakeRom(1, "Solstice", "nes"));
  partial.set_truncated(true);
  FakeFileSystem more("more");
  more.AddFile("/retroarch/saves", "Rampart.srm", 64, kMtime);
  const scan::ScanResult short_index = scan::ScanSaves(config, partial, more, kFirstTick);
  checks.Expect(short_index.index_truncated, "the scan carries the index's truncation");
  checks.Expect(!short_index.skipped.empty() &&
                    short_index.skipped.front().message.find("index is incomplete") !=
                        std::string::npos,
                "and says so in the reason an unmatched file carries");
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

  FakeFileSystem files("files");
  files.AddFile("/retroarch/saves", "gb240p.srm", 512, kMtime);
  files.AddDirectory("/retroarch/saves", "RetroArch-Core-Options");

  const scan::ScanResult result = scan::ScanSaves(config, index, files, kFirstTick);

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
  FakeFileSystem files("files");
  // An mtime of 0 is what a console with an unset clock reports. The server
  // would read it as "very old" and answer with a download over a save that may
  // be the only copy, so `sync::Validate` refuses it -- and this scenario is
  // that refusal costing one file rather than the tick, since
  // `EncodeNegotiateRequest` stops at the first bad entry.
  files.AddFile("/retroarch/saves", "gb240p.srm", 512, 0);
  files.AddFile("/retroarch/saves", "nova.srm", 512, kMtime);

  const scan::ScanResult result = scan::ScanSaves(config, index, files, kFirstTick);

  checks.Expect(SkippedFor(result, "/retroarch/saves/gb240p.srm", scan::SkipReason::kUnusable),
                "a save with an unset mtime is skipped with a reason");
  checks.ExpectEq(result.saves.size(), static_cast<std::size_t>(1),
                  "and the other save still syncs");

  // The other end, and the dangerous one. `sync::Timestamp` is a
  // `system_clock::time_point` whose tick is implementation-defined --
  // nanoseconds on libstdc++, which is the Switch toolchain and CI, and
  // microseconds on libc++. A second count past what that tick can hold is
  // signed overflow, and what it wraps to is the dangerous kind: a *plausible
  // recent* instant that `sync::Validate` accepts and the server then
  // arbitrates as newer than its own copy.
  //
  // So the assertion is the invariant rather than a number, because which
  // values overflow is a property of the toolchain: an extreme mtime is either
  // skipped, or it survives *exactly* -- never quietly turned into a different
  // instant.
  const std::int64_t kExtremes[] = {
      20196744074LL,            // year 2610; wraps to roughly 2025 in nanoseconds
      253402300800LL,           // one second past the last instant RomM can read
      1LL << 61,                //
      std::numeric_limits<std::int64_t>::max(),
      -1LL,
  };
  for (const std::int64_t mtime : kExtremes) {
    FakeFileSystem extreme("extreme");
    extreme.AddFile("/retroarch/saves", "nova.srm", 512, mtime);
    const scan::ScanResult scanned = scan::ScanSaves(config, index, extreme, kFirstTick);
    if (scanned.saves.empty()) {
      checks.Expect(SkippedFor(scanned, "/retroarch/saves/nova.srm", scan::SkipReason::kUnusable),
                    "an mtime of " + std::to_string(mtime) + " is skipped as unusable");
      continue;
    }
    const sync::ClientSaveState state = scanned.saves.front().ToClientSaveState();
    checks.ExpectEq(sync::UnixSeconds(state.updated_at), mtime,
                    "an mtime this client did report survived unchanged");
  }

  // The conversion itself fails closed, for a caller that did not range-check
  // first: an instant it cannot hold becomes the epoch, which Validate refuses
  // rather than an arbitrary recent time it would accept.
  scan::SaveFile overflowing;
  overflowing.rom_id = 1;
  overflowing.file_name = "nova.srm";
  overflowing.slot = "retroarch-srm";
  overflowing.modified_unix = std::numeric_limits<std::int64_t>::max();
  checks.Expect(!sync::Validate(overflowing.ToClientSaveState()).ok(),
                "ToClientSaveState refuses to invent an instant it cannot hold");

  sync::SyncNegotiatePayload payload;
  for (const scan::SaveFile& save : result.saves) {
    payload.saves.push_back(save.ToClientSaveState());
  }
  const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
  checks.Expect(encoded.ok(), "what the scan did emit encodes: " + encoded.error.Describe());
  checks.Expect(payload.saves.front().content_hash.has_value(),
                "the save the scan did emit carries a digest, not the timestamps-only null "
                "that has the server plan an upload for bytes it already has");

  // An explicit digest still wins, for a caller that re-read the file itself.
  const std::string digest(sync::kContentHashDigits, 'a');
  const sync::ClientSaveState hashed = result.saves.front().ToClientSaveState(digest);
  checks.Expect(hashed.content_hash.has_value() && *hashed.content_hash == digest,
                "a digest handed to ToClientSaveState overrides the scanned one");
  checks.Expect(sync::Validate(hashed).ok(), "and the record is still one the encoder accepts");

  // "" and null are different values to the server and only one is a value.
  scan::SaveFile bare;
  bare.rom_id = 1;
  bare.file_name = "nova.srm";
  bare.slot = "retroarch-srm";
  bare.modified_unix = kMtime;
  checks.Expect(!bare.ToClientSaveState(std::string()).content_hash.has_value(),
                "an empty digest is null, not a blank value the server would store");

  // Two files for one rom in one folder land on the same `(rom_id, slot)`, which
  // the server pairs on -- so they would overwrite each other through RomM on
  // alternating ticks. The first in scan order keeps it.
  FakeFileSystem duplicates("duplicates");
  duplicates.AddFile("/retroarch/saves", "nova.srm", 512, kMtime);
  duplicates.AddFile("/tico/saves/nes", "nova.srm", 512, kMtime);
  duplicates.AddFile("/tico/saves/nes", "nova.sav", 512, kMtime);
  const scan::ScanResult mixed = scan::ScanSaves(config, index, duplicates, kFirstTick);
  checks.ExpectEq(mixed.saves.size(), static_cast<std::size_t>(3),
                  "the same rom under two emulators, and two extensions, are three slots");

  // Two spellings of one extension in one folder are one slot, because the slot
  // is lowercased. The entries are sorted by name, so `nova.SRM` is seen first
  // (`S` sorts before `s`) and keeps the slot; which one wins matters less than
  // that the same one wins on every tick.
  duplicates.AddFile("/retroarch/saves", "nova.SRM", 512, kMtime);
  const scan::ScanResult clashing = scan::ScanSaves(config, index, duplicates, kFirstTick);
  checks.Expect(SkippedFor(clashing, "/retroarch/saves/nova.srm", scan::SkipReason::kDuplicateSlot),
                "a second file claiming a taken (rom_id, slot) is reported, not synced");
  checks.ExpectEq(clashing.saves.size(), mixed.saves.size(),
                  "and the clash costs exactly the one file");
  const scan::ScanResult replayed = scan::ScanSaves(config, index, duplicates, kFirstTick);
  checks.Expect(SkippedFor(replayed, "/retroarch/saves/nova.srm", scan::SkipReason::kDuplicateSlot),
                "the same file loses the slot on the next tick, not the other one");
}

// --- hashing ------------------------------------------------------------------

void Hashing(::checks::Checks& checks) {
  roms::RomIndex index;
  index.Add(MakeRom(1, "gb240p", "gb"));
  index.Add(MakeRom(2, "nova", "nes"));

  const config::Config config = config::Defaults();
  FakeFileSystem files("hashing");
  files.AddFile("/retroarch/saves", "gb240p.srm", 512, kMtime);
  files.AddFile("/retroarch/saves", "nova.srm", 1024, kMtime);

  const scan::ScanResult first = scan::ScanSaves(config, index, files, kFirstTick);
  checks.ExpectEq(first.saves.size(), static_cast<std::size_t>(2), "both saves are reported");
  checks.ExpectEq(first.unhashed_total, static_cast<std::size_t>(0), "and both are hashed");
  for (const scan::SaveFile& save : first.saves) {
    checks.ExpectEq(save.content_hash, state::HashFile(files.Resolve(save.sd_path)).content_hash,
                    "the digest is the MD5 of the file's bytes");
  }
  // Two files, two digests. A fake that gave every file the same bytes would
  // pass every other check here and prove nothing.
  checks.Expect(first.saves.front().content_hash != first.saves.back().content_hash,
                "and two different saves do not share one");

  // The baseline's job: an unchanged file is not re-read. Proved by storing a
  // digest that is *not* the file's -- if the scan read the file, it would
  // disagree.
  const std::string stored(sync::kContentHashDigits, 'b');
  state::Baseline baseline;
  for (const scan::SaveFile& save : first.saves) {
    state::SaveRecord row;
    row.rom_id = save.rom_id;
    row.slot = save.slot;
    row.content_hash = stored;
    row.mtime = save.ToClientSaveState().updated_at;
    row.file_size_bytes = save.size_bytes;
    baseline.Set(std::move(row));
  }
  const scan::ScanResult reused = scan::ScanSaves(config, index, files, baseline);
  for (const scan::SaveFile& save : reused.saves) {
    checks.ExpectEq(save.content_hash, stored,
                    "an unchanged save reuses the baseline's digest rather than re-reading");
  }

  // ...and a file whose size moved is read again, because either half of
  // (mtime, size) alone misses a real edit.
  FakeFileSystem edited("hashing-edited");
  edited.AddFile("/retroarch/saves", "gb240p.srm", 513, kMtime);
  const scan::ScanResult rehashed = scan::ScanSaves(config, index, edited, baseline);
  checks.ExpectEq(rehashed.saves.size(), static_cast<std::size_t>(1), "the edited save is reported");
  if (!rehashed.saves.empty()) {
    checks.Expect(rehashed.saves.front().content_hash != stored,
                  "and a save whose size changed is hashed again");
  }

  // A file the listing advertises and the card will not open is still reported
  // -- on timestamps, less precisely -- and counted, because that is the state
  // where the server plans an upload for bytes it may already have.
  FakeFileSystem gone("hashing-gone");
  gone.AddUnreadableFile("/retroarch/saves", "nova.srm", kMtime);
  const scan::ScanResult unreadable = scan::ScanSaves(config, index, gone, kFirstTick);
  checks.ExpectEq(unreadable.saves.size(), static_cast<std::size_t>(1),
                  "an unhashable save is still a save");
  checks.ExpectEq(unreadable.skipped_total, static_cast<std::size_t>(0), "and not a skip");
  checks.ExpectEq(unreadable.unhashed_total, static_cast<std::size_t>(1), "but it is counted");
  if (!unreadable.saves.empty()) {
    checks.Expect(unreadable.saves.front().content_hash.empty(),
                  "with no digest rather than an invented one");
    checks.Expect(sync::Validate(unreadable.saves.front().ToClientSaveState()).ok(),
                  "and it still encodes -- a null hash is a documented value");
  }
  checks.Expect(!unreadable.unhashed.empty() &&
                    unreadable.unhashed.front().sd_path == "/retroarch/saves/nova.srm",
                "and the file is named");
  checks.Expect(unreadable.unhashed.front().reason == scan::SkipReason::kUnhashed,
                "under a reason of its own, not one that reads as a skip");
  checks.Expect(unreadable.DescribeSkipped().find("nova.srm") != std::string::npos,
                "and a caller that logs the skips sees it -- it is where the server "
                "uploads bytes it may already have");

  // A save whose path the backend refuses is the same case as one it cannot
  // read, and it must be refused rather than resolved: on the host an
  // unresolved SD path would open against the process's working directory.
  FakeFileSystem escaping("hashing-escaping");
  escaping.AddFile("/retroarch/saves", "nova.srm", 64, kMtime);
  checks.ExpectEq(escaping.Resolve("/retroarch/saves/../../etc/passwd"), std::string(),
                  "a path that walks out of the card is refused, not resolved");
}

// --- truncated ----------------------------------------------------------------

void Truncated(::checks::Checks& checks) {
  // A directory past `kMaxDirectoryEntries`. The bound itself is not the
  // interesting part -- *which* entries survive it is: the scanner resolves a
  // contested `(rom_id, slot)` in favour of the first file it sees, so a
  // selection that depends on the card's directory layout makes two files take
  // turns overwriting each other through the server.
  const std::filesystem::path root =
      std::filesystem::path(rig::ScratchDir()) / "listing-truncation";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root / "retroarch" / "saves", error);

  const std::size_t kExtra = 8;
  for (std::size_t index = 0; index < fs::kMaxDirectoryEntries + kExtra; ++index) {
    std::string name = std::to_string(index);
    name.insert(name.begin(), 6 - name.size(), '0');  // zero-padded, so name order is index order
    rig::WriteFile((root / "retroarch" / "saves" / (name + ".srm")).string(), "x");
  }

  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(root.string());
  const fs::Listing listing = files->List("/retroarch/saves");
  checks.Expect(listing.error == fs::ListError::kTooManyEntries,
                "a directory past the bound says so rather than pretending it is complete");
  checks.ExpectEq(listing.entries.size(), fs::kMaxDirectoryEntries,
                  "and holds exactly the bound");

  std::vector<std::string> names;
  for (const fs::Entry& entry : listing.entries) {
    names.push_back(entry.name);
  }
  std::sort(names.begin(), names.end());
  checks.ExpectEq(names.front(), std::string("000000.srm"), "the entries kept are the first by name");
  std::string last = std::to_string(fs::kMaxDirectoryEntries - 1);
  last.insert(last.begin(), 6 - last.size(), '0');
  checks.ExpectEq(names.back(), last + ".srm",
                  "and they stop exactly at the bound, not wherever readdir did");

  // The same directory read twice gives the same set, which is what makes the
  // duplicate-slot rule above it deterministic.
  // `Resolve` is the half of this interface the *file* operations use, and its
  // refusal is what keeps an SD path from resolving against the host's working
  // directory. Checked on the real backend, not only on the fake.
  checks.Expect(!files->Resolve("/retroarch/saves/000000.srm").empty(),
                "a path on the card resolves to something openable");
  checks.ExpectEq(files->Resolve("/retroarch/../../etc/passwd"), std::string(),
                  "a path that walks out of the card is refused");
  checks.ExpectEq(files->Resolve(std::string("/retroarch/sa\0ves", 17)), std::string(),
                  "and so is one carrying a NUL, which every C API downstream stops at");
  checks.Expect(files->Resolve("/retroarch/saves/000000.srm").rfind(root.string(), 0) == 0,
                "and what it does resolve is under the card's root");

  const fs::Listing again = files->List("/retroarch/saves");
  std::vector<std::string> repeat;
  for (const fs::Entry& entry : again.entries) {
    repeat.push_back(entry.name);
  }
  std::sort(repeat.begin(), repeat.end());
  checks.Expect(repeat == names, "and reading it again gives the same entries");

  std::filesystem::remove_all(root, error);
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
  const scan::ScanResult result = scan::ScanSaves(config, fetched.index, *files, kFirstTick);

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
  for (const scan::SaveFile& save : result.saves) {
    // The digest is M2-3's arithmetic, checked against M2-3's own function on
    // the same bytes rather than against a constant this test made up.
    const state::HashOutcome expected = state::HashFile(sandbox.Host(save.sd_path));
    checks.Expect(expected.ok(), "the sandbox file is readable: " + expected.message);
    checks.ExpectEq(save.content_hash, expected.content_hash,
                    "the record carries the MD5 of the bytes on the card");
    checks.ExpectEq(save.content_hash.size(), sync::kContentHashDigits,
                    "which is 32 hex digits, not a SHA1");
  }
  checks.ExpectEq(result.unhashed_total, static_cast<std::size_t>(0),
                  "and nothing was reported without one");

  // A second scan of an unchanged card must produce the same slots, or the
  // server sees a new save every tick.
  const scan::ScanResult again = scan::ScanSaves(config, fetched.index, *files, kFirstTick);
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

  // Paged by one and paged whole must be the *same library*, not merely the
  // same count: the endpoint's default order is by name, names are not unique
  // in a RomM library, and a tie straddling a page boundary returns one rom
  // twice and another never. Ids are.
  for (const roms::Rom& rom : all.index.roms()) {
    const roms::Rom* same = one_at_a_time.index.ById(rom.id);
    checks.Expect(same != nullptr && same->fs_name_no_ext == rom.fs_name_no_ext,
                  "rom " + std::to_string(rom.id) + " survives being paged one at a time");
  }
  std::vector<std::int64_t> ids;
  for (const roms::Rom& rom : one_at_a_time.index.roms()) {
    checks.Expect(std::find(ids.begin(), ids.end(), rom.id) == ids.end(),
                  "and no rom is returned twice across pages");
    ids.push_back(rom.id);
  }

  // A page past the end of a library that shrank mid-fetch is not the library
  // ending. `offset` beyond `total` is exactly what that leaves behind.
  roms::Page page;
  const rommsync::json::Error empty = roms::ParsePage(
      "{\"items\":[],\"total\":6,\"limit\":200,\"offset\":6}", &page);
  checks.Expect(empty.ok(), "an empty page is a shape this client reads");
  checks.ExpectEq(page.total, static_cast<std::int64_t>(6), "and it still carries the total");

  // The envelope itself, because the mistake this guards is reading the body as
  // a bare array -- which parses as nothing and reads as "the library ended".
  const rommsync::json::Error error = roms::ParsePage("[{\"id\":1}]", &page);
  checks.Expect(!error.ok(), "a bare array is refused by name, not read as an empty library");

  // One oddly named rom must not cost the whole library. RomM derives
  // `fs_name_no_tags` by stripping `(...)` and `[...]`, so a file called
  // `(USA).nes` reduces to nothing -- and a strict read of that field would
  // fail the page, abort the fetch, and leave an index in which *every* save on
  // the card is unmatched, on every tick.
  const std::string kEmptyTags =
      "{\"total\":1,\"limit\":200,\"offset\":0,\"items\":[{\"id\":7,\"fs_name_no_ext\":\"(USA)\","
      "\"fs_name_no_tags\":\"\",\"platform_fs_slug\":\"nes\",\"has_multiple_files\":false}]}";
  roms::Page tagged;
  checks.Expect(roms::ParsePage(kEmptyTags, &tagged).ok(),
                "a rom whose tag-stripped name is empty is read, not fatal");
  checks.ExpectEq(tagged.roms.size(), static_cast<std::size_t>(1), "and it is in the page");

  // Still a shape this client refuses when the field is the wrong *type* or
  // absent -- lenient about empty is not lenient about missing.
  roms::Page broken;
  checks.Expect(!roms::ParsePage("{\"total\":1,\"limit\":1,\"offset\":0,\"items\":[{\"id\":7,"
                                 "\"fs_name_no_ext\":42,\"fs_name_no_tags\":\"x\","
                                 "\"platform_fs_slug\":\"nes\",\"has_multiple_files\":false}]}",
                                 &broken)
                     .ok(),
                "a name that is not a string is still refused by name");
  checks.Expect(!roms::ParsePage("{\"total\":1,\"limit\":1,\"offset\":0,\"items\":[{\"id\":7,"
                                 "\"fs_name_no_tags\":\"x\",\"platform_fs_slug\":\"nes\","
                                 "\"has_multiple_files\":false}]}",
                                 &broken)
                     .ok(),
                "and so is a missing one");

  // An empty key matches no base name, so the lenient read costs that one rom
  // and nothing else.
  roms::RomIndex odd;
  odd.Add(tagged.roms.front());
  checks.Expect(!odd.Find("nova", "").matched(), "an empty key does not match a real name");
  checks.Expect(odd.Find("(USA)", "").matched(), "and the rom is still findable by the other");
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
      scenario == "unusable" || scenario == "truncated" || scenario == "hashing") {
    if (scenario == "names") {
      Names(checks);
    } else if (scenario == "ambiguous") {
      Ambiguous(checks);
    } else if (scenario == "walked_once") {
      WalkedOnce(checks);
    } else if (scenario == "truncated") {
      Truncated(checks);
    } else if (scenario == "hashing") {
      Hashing(checks);
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
