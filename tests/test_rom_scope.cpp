// What a negotiation is about (#241): the roms on the card, in chunks RomM
// accepts, with freshly downloaded roms asked about on their own.
//
//   chunks  -- `sync::ChunkScope`: at most 500 ids a negotiation, every rom in
//              exactly one, every save beside its rom, and the restore apart
//   on_card -- `sync::FindRomsOnCard` against a real directory tree: a rom
//              counts when a file of its name is in one of its platform's
//              mapped `roms` folders, and nothing else does
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rig.hpp"
#include "rommsync/config.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/rom_scope.hpp"
#include "rommsync/sync.hpp"

namespace {

namespace config = rommsync::config;
namespace fs = rommsync::fs;
namespace roms = rommsync::roms;
namespace sync = rommsync::sync;

sync::ClientSaveState SaveFor(std::int64_t rom_id) {
  sync::ClientSaveState save;
  save.rom_id = rom_id;
  save.file_name = "game" + std::to_string(rom_id) + ".srm";
  save.slot = "autosave";
  return save;
}

/// The chunk `rom_id` is in, or -1. Also checks it is in only one.
int ChunkOf(const std::vector<sync::ScopeChunk>& chunks, std::int64_t rom_id, int* seen) {
  int found = -1;
  *seen = 0;
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    const std::vector<std::int64_t>& ids = chunks[index].rom_ids;
    if (std::find(ids.begin(), ids.end(), rom_id) != ids.end()) {
      found = static_cast<int>(index);
      ++*seen;
    }
  }
  return found;
}

void Chunks(checks::Checks& c) {
  // Nothing on the card: one negotiation, scoped to nothing. RomM answers it
  // with nothing to restore -- which is right, there is nothing to restore into.
  {
    const std::vector<sync::ScopeChunk> chunks = sync::ChunkScope({}, {}, {});
    c.ExpectEq(chunks.size(), std::size_t{1}, "an empty card is still one negotiation");
    c.Expect(!chunks.empty() && chunks[0].rom_ids.empty() && chunks[0].saves.empty() &&
                 !chunks[0].restore,
             "scoped to no rom at all, rather than to the whole library");
  }

  // A small card: one chunk, sorted, without repeats, and every save's rom in
  // the scope even when no rom file was found for it.
  {
    const std::vector<sync::ClientSaveState> saves = {SaveFor(7), SaveFor(3)};
    const std::vector<sync::ScopeChunk> chunks = sync::ChunkScope(saves, {9, 7, 9, 0, -4}, {});
    c.ExpectEq(chunks.size(), std::size_t{1}, "a small card is one negotiation");
    if (chunks.size() == 1) {
      c.Expect(chunks[0].rom_ids == std::vector<std::int64_t>{3, 7, 9},
               "its roms and its saves' roms, sorted, once each, and never an id RomM refuses");
      c.Expect(chunks[0].saves == std::vector<std::size_t>{0, 1}, "with both saves");
    }
  }

  // The rom index's bound: 1,024 roms on the card is three negotiations.
  {
    std::vector<std::int64_t> on_card;
    for (std::int64_t id = 1; id <= static_cast<std::int64_t>(roms::kMaxIndexRoms); ++id) {
      on_card.push_back(id * 3);
    }
    const std::vector<sync::ClientSaveState> saves = {SaveFor(3), SaveFor(1500), SaveFor(3072),
                                                      SaveFor(5000)};
    const std::vector<sync::ScopeChunk> chunks = sync::ChunkScope(saves, on_card, {});
    c.ExpectEq(chunks.size(), std::size_t{3}, "1,024 roms are three negotiations of at most 500");
    std::size_t total = 0;
    for (const sync::ScopeChunk& chunk : chunks) {
      c.Expect(chunk.rom_ids.size() <= sync::kMaxNegotiateRomIds, "none past RomM's maxItems");
      c.Expect(!chunk.restore, "and none restoring");
      total += chunk.rom_ids.size();
    }
    c.ExpectEq(total, roms::kMaxIndexRoms + 1,
               "every rom on the card and the one save's rom that is not, once");
    for (std::size_t index = 0; index < saves.size(); ++index) {
      int seen = 0;
      const int chunk = ChunkOf(chunks, saves[index].rom_id, &seen);
      c.ExpectEq(seen, 1, "each save's rom is in exactly one negotiation");
      if (chunk >= 0) {
        const std::vector<std::size_t>& with = chunks[static_cast<std::size_t>(chunk)].saves;
        c.Expect(std::find(with.begin(), with.end(), index) != with.end(),
                 "and the save is sent in that one -- a save sent beside another chunk's rom "
                 "would leave its own chunk offering a download over it");
      }
    }
    std::size_t sent = 0;
    for (const sync::ScopeChunk& chunk : chunks) {
      sent += chunk.saves.size();
    }
    c.ExpectEq(sent, saves.size(), "and no save is sent twice");
  }

  // A rom just downloaded is asked about on its own, with its save, and never
  // also in the ordinary negotiation.
  {
    const std::vector<sync::ClientSaveState> saves = {SaveFor(9), SaveFor(7)};
    const std::vector<sync::ScopeChunk> chunks = sync::ChunkScope(saves, {7, 9}, {9, 9});
    c.ExpectEq(chunks.size(), std::size_t{2}, "the card's roms, and the downloaded one apart");
    if (chunks.size() == 2) {
      c.Expect(chunks[0].rom_ids == std::vector<std::int64_t>{7} && !chunks[0].restore &&
                   chunks[0].saves == std::vector<std::size_t>{1},
               "the ordinary negotiation, without it");
      c.Expect(chunks[1].rom_ids == std::vector<std::int64_t>{9} && chunks[1].restore &&
                   chunks[1].saves == std::vector<std::size_t>{0},
               "and the restoring one, with its save so RomM pairs rather than re-offers it");
    }
  }

  // Only a download: no empty ordinary negotiation in front of it.
  {
    const std::vector<sync::ScopeChunk> chunks = sync::ChunkScope({}, {}, {4});
    c.ExpectEq(chunks.size(), std::size_t{1}, "a download on an otherwise empty card is one");
    c.Expect(!chunks.empty() && chunks[0].restore &&
                 chunks[0].rom_ids == std::vector<std::int64_t>{4},
             "and it restores");
  }
}

roms::Rom MakeRom(std::int64_t id, std::string name, std::string platform) {
  roms::Rom rom;
  rom.id = id;
  rom.fs_name_no_ext = name;
  rom.fs_name_no_tags = std::move(name);
  rom.platform_fs_slug = std::move(platform);
  return rom;
}

class Card {
 public:
  explicit Card(const std::string& label)
      : root_(std::filesystem::path(rig::ScratchDir()) / "rom-scope" / label) {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    std::filesystem::create_directories(root_, error);
    files_ = rommsync::host::MakeNativeFileSystem(root_.string());
  }
  ~Card() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }
  Card(const Card&) = delete;
  Card& operator=(const Card&) = delete;

  void Write(const std::string& sd_path) {
    std::error_code error;
    std::filesystem::create_directories((root_ / sd_path.substr(1)).parent_path(), error);
    rig::WriteFile((root_ / sd_path.substr(1)).string(), "rom bytes");
  }
  void MakeDirs(const std::string& sd_path) {
    std::error_code error;
    std::filesystem::create_directories(root_ / sd_path.substr(1), error);
  }
  fs::FileSystem& files() { return *files_; }

 private:
  std::filesystem::path root_;
  std::unique_ptr<fs::FileSystem> files_;
};

void OnCard(checks::Checks& c) {
  roms::RomIndex index;
  index.Add(MakeRom(1, "nova", "nes"));
  index.Add(MakeRom(2, "240pee", "nes"));
  index.Add(MakeRom(3, "240pee_mb", "gba"));
  index.Add(MakeRom(4, "Super Mario World (USA)", "snes"));
  index.Add(MakeRom(5, "nova", "gb"));
  index.Add(MakeRom(6, "Disc Set (USA)", "psx"));

  Card card("defaults");
  card.Write("/tico/roms/nes/nova.nes");
  card.Write("/tico/roms/nes/240pee.nes.tmp.part");  // a download still under way
  card.Write("/tico/roms/nes/homebrew.nes");          // a rom RomM does not have
  card.Write("/tico/roms/gba/240pee_mb.gba");
  card.MakeDirs("/tico/roms/psx/Disc Set (USA)");     // a multi-file rom is a folder
  // gb, snes: no folder at all.

  const config::Config defaults = config::Defaults();
  const sync::RomsOnCard found = sync::FindRomsOnCard(defaults, index, card.files());
  c.Expect(found.rom_ids == std::vector<std::int64_t>{1, 3, 6},
           "the roms whose file is in their platform's folder, and only those -- not nova on gb, "
           "whose name matches but whose folder is not there, nor a transfer still under way");
  c.Expect(found.truncated.empty(), "nothing was listed short");

  // A user's own folder counts too: the map names where a platform's roms live.
  const config::LoadResult own =
      config::ParseConfig("[platform.nes]\nroms = /tico/roms/nes, /roms/nes\n"
                          "saves = /tico/saves/nes\n");
  card.Write("/roms/nes/240pee.nes");
  const sync::RomsOnCard mapped = sync::FindRomsOnCard(own.value, index, card.files());
  c.Expect(std::find(mapped.rom_ids.begin(), mapped.rom_ids.end(), 2) != mapped.rom_ids.end(),
           "a rom in the second folder the platform maps is on the card");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "chunks";
  checks::Checks c;
  if (scenario == "chunks") {
    Chunks(c);
  } else if (scenario == "on_card") {
    OnCard(c);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }
  if (c.failures() == 0) {
    std::cout << "scope." << scenario << " ok\n";
  }
  return c.failures() == 0 ? 0 : 1;
}
