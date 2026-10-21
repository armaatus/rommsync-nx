// Which emulator wrote a save: the tables and the rules behind `scan::SaveSource`.
//
// Pure, so it never skips. RomM pairs saves and prunes lineages by the
// `emulator` a client sends (#226 R1), so an id that is wrong here is not a
// cosmetic label -- it is every Switch save in a lineage no other device shares,
// or a save filed under a core whose bytes it is not.
//
//   retroarch_table -- the core-folder table is RomM's own, read off the copy of
//                      `emulator_names.py` pinned beside the 5.4.0 contract
//   tico_table      -- every Tico slug, its module and its id; isolated formats
//                      carry a `tico-*` id (#226 D3)
//   ids             -- RomM's rule for an `emulator`, refused before it is sent
//   layouts         -- what a folder alone says about who wrote it
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "checks.hpp"
#include "rommsync/config.hpp"
#include "rommsync/save_source.hpp"

namespace config = rommsync::config;
namespace scan = rommsync::scan;

namespace {

/// The `"id": "Folder",` pairs of `RETROARCH_DIR_BY_ROMM_EMULATOR`, in order.
///
/// Read from the file rather than restated here, so the table in `core/` is
/// checked against what RomM ships rather than against a second copy of what
/// someone remembered it ships.
std::vector<std::pair<std::string, std::string>> PinnedRetroArchTable(bool* found) {
  std::ifstream in(ROMMSYNC_EMULATOR_NAMES);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::size_t begin = text.find("RETROARCH_DIR_BY_ROMM_EMULATOR");
  const std::size_t end = begin == std::string::npos ? begin : text.find('}', begin);
  std::vector<std::pair<std::string, std::string>> pairs;
  *found = begin != std::string::npos && end != std::string::npos;
  if (!*found) {
    return pairs;
  }
  const std::string body = text.substr(begin, end - begin);
  const std::regex entry(R"re("([^"]+)"\s*:\s*"([^"]+)")re");
  for (auto it = std::sregex_iterator(body.begin(), body.end(), entry);
       it != std::sregex_iterator(); ++it) {
    pairs.emplace_back((*it)[1].str(), (*it)[2].str());
  }
  return pairs;
}

void RetroArchTable(checks::Checks& c) {
  bool found = false;
  const std::vector<std::pair<std::string, std::string>> pinned = PinnedRetroArchTable(&found);
  c.Expect(found && pinned.size() > 40,
           "server/contract/romm-5.4.0-emulator_names.py still holds RomM's table");

  const std::vector<scan::RetroArchCore>& ours = scan::RetroArchCores();
  c.ExpectEq(ours.size(), pinned.size(), "the same number of cores as RomM 5.4.0");
  for (std::size_t i = 0; i < ours.size() && i < pinned.size(); ++i) {
    c.ExpectEq(std::string(ours[i].id), pinned[i].first, "core " + std::to_string(i) + "'s id");
    c.ExpectEq(std::string(ours[i].folder), pinned[i].second,
               "core " + pinned[i].first + "'s folder");
  }

  // `to_romm_emulator`: a folder two ids share answers with the *first* listed,
  // which is the `mednafen_*` id RomM's web player stores.
  c.ExpectEq(scan::RetroArchCoreId("Snes9x"), std::string("snes9x"), "Snes9x");
  c.ExpectEq(scan::RetroArchCoreId("Beetle PSX HW"), std::string("mednafen_psx_hw"),
             "a shared folder answers with the first id RomM lists");
  c.ExpectEq(scan::RetroArchCoreId("Beetle Saturn"), std::string("mednafen_saturn"),
             "and again for Saturn");
  c.ExpectEq(scan::RetroArchCoreId("Genesis Plus GX"), std::string("genesis_plus_gx"),
             "a folder name with spaces");
  // FAT32 does not keep case apart, so neither does a folder on it.
  c.ExpectEq(scan::RetroArchCoreId("mgba"), std::string("mgba"),
             "a folder is matched without regard to case");
  c.ExpectEq(scan::RetroArchCoreId("YabaSanshiro"), std::string(),
             "a folder RomM does not know is not a core folder here");

  // `to_retroarch_dir_name`: an id RomM does not know is its own folder.
  c.ExpectEq(scan::RetroArchCoreFolder("mednafen_psx"), std::string("Beetle PSX"), "id to folder");
  c.ExpectEq(scan::RetroArchCoreFolder("tico-ppsspp"), std::string("tico-ppsspp"),
             "an unknown id is unchanged");

  // `retroarch_aliases`: sorted, the id itself and its folder included.
  const std::vector<std::string> beetle = scan::RetroArchAliases("mednafen_psx");
  const std::vector<std::string> expected = {"Beetle PSX", "beetle_psx", "mednafen_psx"};
  c.Expect(beetle == expected, "the ids that share Beetle PSX's folder are aliases");
  c.Expect(scan::RetroArchAliases("tico-duckstation") ==
               std::vector<std::string>{"tico-duckstation"},
           "an id RomM does not know is its own only alias");
  c.Expect(scan::RetroArchAliases("").empty(), "no id has no aliases");
}

void TicoTable(checks::Checks& c) {
  struct Row {
    const char* slug;
    const char* id;
  };
  // The table #229 specifies, row by row.
  const Row kRows[] = {
      {"nes", "fceumm"},
      {"snes", "snes9x"},
      {"gba", "mgba"},
      {"fbneo", "fbneo"},
      {"gb", "gambatte"},
      {"gbc", "gambatte"},
      {"genesis", "genesis_plus_gx"},
      {"master-system", "genesis_plus_gx"},
      {"game-gear", "genesis_plus_gx"},
      {"sega-cd", "genesis_plus_gx"},
      {"n64", "tico-mupen64plus"},
      {"psx", "tico-duckstation"},
      {"dc", "tico-flycast"},
      {"nds", "tico-drastic"},
      {"saturn", "tico-yabasanshiro"},
      {"psp", "tico-ppsspp"},
  };
  for (const Row& row : kRows) {
    const scan::TicoModule* module = scan::TicoModuleFor(row.slug);
    c.Expect(module != nullptr, std::string("Tico runs ") + row.slug);
    if (module == nullptr) {
      continue;
    }
    c.ExpectEq(std::string(module->emulator_id), std::string(row.id),
               std::string(row.slug) + "'s emulator id");
    c.Expect(module->module != nullptr && *module->module != '\0',
             std::string(row.slug) + " names the module that runs it");
    std::string why;
    c.Expect(scan::ValidEmulatorId(module->emulator_id, &why),
             std::string(row.id) + " is an id RomM accepts: " + why);
    // D3: every format that is not a libretro core's is isolated under a
    // `tico-` id, and every libretro one shares RomM's id for that core.
    const bool isolated = std::string(row.id).rfind("tico-", 0) == 0;
    c.Expect(isolated || scan::RetroArchCoreFolder(row.id) != row.id,
             std::string(row.id) + " is either isolated or a core RomM knows");
  }
  c.Expect(scan::TicoModuleFor("vectrex") == nullptr, "a system Tico does not run has no module");

  // RomM spells five systems differently from Tico (#226 R5), and the default
  // map uses RomM's `dreamcast` where Tico's folder is `dc`.
  c.ExpectEq(scan::TicoSlugFor("dreamcast"), std::string("dc"), "dreamcast");
  c.ExpectEq(scan::TicoSlugFor("sms"), std::string("master-system"), "sms");
  c.ExpectEq(scan::TicoSlugFor("gamegear"), std::string("game-gear"), "gamegear");
  c.ExpectEq(scan::TicoSlugFor("segacd"), std::string("sega-cd"), "segacd");
  c.ExpectEq(scan::TicoSlugFor("ngc"), std::string("gc"), "ngc");
  c.ExpectEq(scan::TicoSlugFor("arcade"), std::string("fbneo"), "arcade");
  c.ExpectEq(scan::TicoSlugFor("snes"), std::string("snes"), "a shared spelling is unchanged");

  // The per-platform defaults are what a folder that does not name its core
  // gets: Tico's per-system folder, and RetroArch's flat `saves/`.
  c.ExpectEq(scan::DefaultEmulatorId(scan::Source::kTico, "dreamcast"),
             std::string("tico-flycast"), "a RomM slug reaches Tico's table");
  c.ExpectEq(scan::DefaultEmulatorId(scan::Source::kRetroArch, "snes"), std::string("snes9x"),
             "RetroArch's default SNES core");
  c.ExpectEq(scan::DefaultEmulatorId(scan::Source::kRetroArch, "n64"),
             std::string("mupen64plus_next"), "RetroArch never gets a tico- id");
  c.ExpectEq(scan::DefaultEmulatorId(scan::Source::kUnknown, "snes"), std::string(),
             "a folder neither emulator owns has no default");
  c.ExpectEq(scan::DefaultEmulatorId(scan::Source::kTico, "vectrex"), std::string(),
             "nor does a system with no table entry");
  for (const char* slug : {"nes", "snes", "gb", "gbc", "gba", "genesis", "n64", "psx", "nds",
                           "dreamcast", "psp"}) {
    const std::string tico = scan::DefaultEmulatorId(scan::Source::kTico, slug);
    const std::string retroarch = scan::DefaultEmulatorId(scan::Source::kRetroArch, slug);
    c.Expect(!tico.empty() && !retroarch.empty(),
             std::string("every platform mapped by default has an id under both: ") + slug);
    c.Expect(retroarch.rfind("tico-", 0) != 0,
             std::string("a RetroArch default is a libretro id: ") + slug);
    // Same bytes, same lineage: where Tico's module is a libretro core, the
    // flat RetroArch default is that core, so the two meet on the server.
    if (tico.rfind("tico-", 0) != 0) {
      c.ExpectEq(retroarch, tico, std::string("a libretro system shares its id: ") + slug);
    }
  }
}

void Ids(checks::Checks& c) {
  std::string why;
  c.Expect(scan::ValidEmulatorId("snes9x", &why), "an ordinary id");
  c.Expect(scan::ValidEmulatorId("Genesis Plus GX", &why), "spaces inside are fine");
  c.Expect(scan::ValidEmulatorId(std::string(50, 'a'), &why), "fifty characters is the limit");
  // Characters, not bytes: RomM's `max_length` counts code points.
  std::string fifty_wide;
  for (int i = 0; i < 50; ++i) {
    fifty_wide += "\xC3\xA9";  // é
  }
  c.Expect(scan::ValidEmulatorId(fifty_wide, &why), "fifty two-byte characters are fifty");

  const std::pair<std::string, const char*> kRefused[] = {
      {"", "empty"},
      {std::string(51, 'a'), "51 characters"},
      {"retroarch/cores", "a slash"},
      {"a\\b", "a backslash"},
      {"a:b", "a colon"},
      {"a|b", "a pipe"},
      {"a*b", "an asterisk"},
      {"a?b", "a question mark"},
      {"a\"b", "a quote"},
      {"a<b", "a less-than"},
      {"a>b", "a greater-than"},
      {std::string("a\tb"), "a tab"},
      {std::string("a\x7f"), "DEL"},
      {std::string("a\0b", 3), "a NUL"},
      {" snes9x", "leading space"},
      {"snes9x ", "trailing space"},
      {".", "a dot"},
      {"..", "two dots"},
  };
  for (const auto& [id, what] : kRefused) {
    why.clear();
    c.Expect(!scan::ValidEmulatorId(id, &why), std::string("refused: ") + what);
    c.Expect(!why.empty(), std::string("with a reason: ") + what);
  }
}

void Layouts(checks::Checks& c) {
  const scan::SaveSource flat = scan::SourceOf("/retroarch/saves");
  c.Expect(flat.source == scan::Source::kRetroArch, "RetroArch's folder is RetroArch's");
  c.Expect(flat.layout == scan::Layout::kFlat, "...and one folder for every system is flat");
  c.ExpectEq(flat.emulator_id, std::string(), "a flat folder does not name a core");
  c.ExpectEq(flat.folder, std::string("/retroarch/saves"), "the folder is carried");

  const scan::SaveSource sorted = scan::SourceOf("/retroarch/saves/Beetle PSX HW");
  c.Expect(sorted.layout == scan::Layout::kCoreFolder, "a core's folder is sort-by-core");
  c.ExpectEq(sorted.emulator_id, std::string("mednafen_psx_hw"), "and names the core");

  const scan::SaveSource tico = scan::SourceOf("/tico/saves/gba");
  c.Expect(tico.source == scan::Source::kTico && tico.layout == scan::Layout::kPlatformFolder,
           "Tico keeps one folder per system");
  c.ExpectEq(tico.emulator_id, std::string(), "...whose module is the platform's, not the path's");

  const scan::SaveSource elsewhere = scan::SourceOf("/emulators/RetroArch/saves");
  c.Expect(elsewhere.source == scan::Source::kRetroArch,
           "any segment, and FAT32 is case-insensitive");

  const scan::SaveSource unknown_core = scan::SourceOf("/retroarch/saves/YabaSanshiro");
  c.Expect(unknown_core.source == scan::Source::kRetroArch &&
               unknown_core.layout == scan::Layout::kUnknown,
           "a RetroArch subfolder RomM's table does not know is neither flat nor a core");
  c.ExpectEq(scan::ResolveSource(unknown_core, "saturn", config::Defaults()).emulator_id,
             std::string(), "...so it gets no guessed id");
  c.Expect(scan::SourceOf("/retroarch/states").layout == scan::Layout::kFlat,
           "RetroArch's states folder is flat too");

  const scan::SaveSource neither = scan::SourceOf("/saves/Snes9x");
  c.Expect(neither.source == scan::Source::kUnknown && neither.layout == scan::Layout::kUnknown,
           "a folder neither emulator owns says nothing -- a core-named leaf included");
  c.ExpectEq(std::string(scan::ToString(scan::Source::kRetroArch)), std::string("retroarch"),
             "the source's name, which the slot is still built from until #236");

  config::Config overridden = config::Defaults();
  overridden.emulators["gb"] = "mgba";
  const scan::SaveSource tico_gb =
      scan::ResolveSource(scan::SourceOf("/tico/saves/gb"), "gb", overridden);
  c.ExpectEq(tico_gb.emulator_id, std::string("mgba"), "the override decides Tico's GB module");
  c.ExpectEq(tico_gb.platform_fs_slug, std::string("gb"), "the platform is carried");
  const scan::SaveSource core_gb =
      scan::ResolveSource(scan::SourceOf("/retroarch/saves/Gambatte"), "gb", overridden);
  c.ExpectEq(core_gb.emulator_id, std::string("gambatte"),
             "a core's own folder outranks the override: the path is what wrote it");
  const scan::SaveSource plain_gbc =
      scan::ResolveSource(scan::SourceOf("/tico/saves/gbc"), "gbc", overridden);
  c.ExpectEq(plain_gbc.emulator_id, std::string("gambatte"), "and it is per platform");
  // The override is the platform's emulator wherever the folder does not name
  // a core -- RetroArch's flat folder included (docs/CONFIG.md).
  c.ExpectEq(scan::ResolveSource(scan::SourceOf("/retroarch/saves"), "gb", overridden).emulator_id,
             std::string("mgba"), "the override covers a flat RetroArch folder too");
  c.ExpectEq(scan::ResolveSource(scan::SourceOf("/retroarch/saves/Gearboy"), "gb", overridden)
                 .emulator_id,
             std::string("mgba"), "and a folder whose layout is unknown");
}

/// #238: which lineage may be written where. A `tico-*` format is read by its own
/// module and nothing else, so it meets only itself; everything else is the
/// pairing RomM already does, and not this rule's to narrow.
void Isolation(checks::Checks& c) {
  c.Expect(scan::IsIsolated("tico-duckstation") && scan::IsIsolated("tico-ppsspp"),
           "a tico-* id is an isolated format");
  c.Expect(!scan::IsIsolated("snes9x") && !scan::IsIsolated("mednafen_psx_hw") &&
               !scan::IsIsolated(""),
           "a libretro core, or no id at all, is not");

  struct Row {
    const char* local;
    const char* server;
    bool may;
    const char* why;
  };
  const Row kRows[] = {
      {"tico-duckstation", "tico-duckstation", true, "the same module's card"},
      {"tico-duckstation", "mednafen_psx_hw", false, "a Beetle .srm onto DuckStation's card"},
      {"tico-duckstation", "pcsx_rearmed", false, "any RetroArch psx core onto it"},
      {"pcsx_rearmed", "tico-duckstation", false, "DuckStation's card taken by a RetroArch core"},
      {"mupen64plus_next", "tico-mupen64plus", false, "a split N64 save into RetroArch's .srm"},
      {"tico-duckstation", "", false, "a save nobody named, onto an isolated format"},
      {"", "tico-mupen64plus", false, "an isolated format into a folder nobody named"},
      {"tico-flycast", "tico-drastic", false, "two isolated formats are not each other"},
      {"snes9x", "snes9x", true, "one core, one lineage"},
      {"pcsx_rearmed", "mednafen_psx_hw", true,
       "two RetroArch cores: RomM's pairing, not this rule's"},
      {"snes9x", "", true, "an unnamed save into a libretro folder, as before #238"},
      {"", "", true, "nothing named on either side"},
  };
  for (const Row& row : kRows) {
    c.Expect(scan::Interchangeable(row.local, row.server) == row.may,
             std::string(row.may ? "may: " : "may not: ") + row.why);
  }

  const config::Config defaults = config::Defaults();
  c.ExpectEq(scan::EmulatorAt("/tico/saves/psx/Game.mcd", "psx", defaults),
             std::string("tico-duckstation"), "Tico's psx folder is DuckStation's");
  c.ExpectEq(scan::EmulatorAt("/tico/saves/users/0123456789abcdef0123456789abcdef/psx/Game.mcd",
                              "psx", defaults),
             std::string("tico-duckstation"), "and so is an account's");
  c.ExpectEq(scan::EmulatorAt("/tico/saves/dc/Game.bin", "dreamcast", defaults),
             std::string("tico-flycast"), "RomM's spelling of the platform, Tico's folder");
  c.ExpectEq(scan::EmulatorAt("/retroarch/saves/Game.srm", "psx", defaults),
             std::string("pcsx_rearmed"), "RetroArch's flat folder is its psx default");
  c.ExpectEq(scan::EmulatorAt("/retroarch/saves/Beetle PSX HW/Game.srm", "psx", defaults),
             std::string("mednafen_psx_hw"), "and a core folder is that core");
  c.ExpectEq(scan::EmulatorAt("/somewhere/Game.srm", "psx", defaults), std::string(),
             "a folder neither emulator owns names nothing");
  c.ExpectEq(scan::EmulatorAt("Game.srm", "psx", defaults), std::string(),
             "nor does a bare name");
  config::Config overridden = defaults;
  overridden.emulators["psx"] = "swanstation";
  c.ExpectEq(scan::EmulatorAt("/tico/saves/psx/Game.mcd", "psx", overridden),
             std::string("swanstation"), "the override is what the scan would report");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "retroarch_table";
  checks::Checks c;
  if (scenario == "retroarch_table") {
    RetroArchTable(c);
  } else if (scenario == "tico_table") {
    TicoTable(c);
  } else if (scenario == "ids") {
    Ids(c);
  } else if (scenario == "layouts") {
    Layouts(c);
  } else if (scenario == "isolation") {
    Isolation(c);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }
  return c.failures() == 0 ? 0 : 1;
}
