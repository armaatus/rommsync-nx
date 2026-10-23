#include "rommsync/save_source.hpp"

#include <set>
#include <string>
#include <vector>

#include "rommsync/config_text.hpp"

namespace rommsync::scan {
namespace {

using config::text::LowerAscii;

/// The `/`-separated segments of an SD path, in order, with empties dropped.
std::vector<std::string_view> Segments(std::string_view sd_path) {
  std::vector<std::string_view> segments;
  std::size_t start = 0;
  while (start <= sd_path.size()) {
    const std::size_t slash = sd_path.find('/', start);
    const std::size_t end = slash == std::string_view::npos ? sd_path.size() : slash;
    if (end > start) {
      segments.push_back(sd_path.substr(start, end - start));
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return segments;
}


// #229's table. A `tico-*` id is a format no libretro core shares (#226 D3):
// Mupen64Plus is built without libretro and splits N64 saves by type,
// DuckStation's `.mcd` is not a Beetle `.srm` until somebody proves it is, and
// the rest are standalone emulators whose files nobody has compared yet. An
// adapter that rewrites one into the other comes after #44 captures real files.
//
// An isolated module's save format is its row's too (#258): Mupen64Plus's four
// files, each a save of its own; DuckStation's one card per game; PPSSPP's
// folders. Flycast, DraStic and YabaSanshiro have none yet -- nobody has read
// their names off a console [#44] -- so a save of theirs keeps the extension
// the same module uploaded it under.
constexpr TicoModule kTicoModules[] = {
    {"nes", "FCEUmm", "fceumm"},
    {"snes", "Snes9x", "snes9x"},
    {"gb", "Gambatte", "gambatte"},
    {"gbc", "Gambatte", "gambatte"},
    {"gba", "mGBA", "mgba"},
    {"fbneo", "FinalBurn Neo", "fbneo"},
    {"genesis", "Genesis Plus GX", "genesis_plus_gx"},
    {"master-system", "Genesis Plus GX", "genesis_plus_gx"},
    {"game-gear", "Genesis Plus GX", "genesis_plus_gx"},
    {"sega-cd", "Genesis Plus GX", "genesis_plus_gx"},
    {"n64", "Mupen64Plus", "tico-mupen64plus", {.also = {".eep", ".sra", ".fla", ".mpk"}}},
    {"psx", "DuckStation", "tico-duckstation", {.primary = ".mcd", .memory_cards = true}},
    {"dc", "Flycast", "tico-flycast"},
    {"nds", "DraStic", "tico-drastic"},
    {"saturn", "YabaSanshiro", "tico-yabasanshiro"},
    {"psp", "PPSSPP", "tico-ppsspp", {.folders = true}},
};

constexpr SaveFormat kLibretroFormat = {.primary = ".srm", .also = {".rtc"}};

/// The RetroArch core a flat `saves/` is filed under, for the systems where
/// Tico's module is *not* a libretro core. Everywhere else it is Tico's core:
/// the same core on two devices is one lineage on the server, which is the
/// point of M10, so that half is derived rather than written twice. These are
/// the cores RomM's web player uses, so a flat RetroArch save at least meets
/// the browser's [#44 checks the Switch build's defaults]. Saturn has none.
struct RetroArchDefault {
  const char* slug;
  const char* emulator_id;
};

constexpr RetroArchDefault kRetroArchOwnCores[] = {
    {"n64", "mupen64plus_next"}, {"psx", "pcsx_rearmed"}, {"dc", "flycast"},
    {"nds", "melonds"},          {"psp", "ppsspp"},
};

/// Where RomM's `platform_fs_slug` and Tico's folder name part ways (#226 R5).
struct SlugSpelling {
  const char* romm;
  const char* tico;
};

constexpr SlugSpelling kTicoSpellings[] = {
    {"dreamcast", "dc"},   {"sms", "master-system"}, {"gamegear", "game-gear"},
    {"segacd", "sega-cd"}, {"ngc", "gc"},            {"arcade", "fbneo"},
};

/// The number of characters in UTF-8 `text`: every byte that does not continue
/// one. A malformed sequence still counts each lead byte, which is the bound
/// erring long.
std::size_t CodePoints(std::string_view text) {
  std::size_t count = 0;
  for (const char byte : text) {
    if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

}  // namespace

const char* ToString(Source source) {
  switch (source) {
    case Source::kRetroArch:
      return "retroarch";
    case Source::kTico:
      return "tico";
    case Source::kUnknown:
      break;
  }
  return "";
}

const std::vector<RetroArchCore>& RetroArchCores() {
  // RomM 5.4.0's `RETROARCH_DIR_BY_ROMM_EMULATOR`, in its order. Edited only by
  // re-pinning server/contract/romm-<version>-emulator_names.py first:
  // `source.retroarch_table` holds this to that file.
  static const std::vector<RetroArchCore> kCores = {
      {"snes9x", "Snes9x"},
      {"bsnes", "bsnes"},
      {"mesen-s", "Mesen-S"},
      {"nestopia", "Nestopia"},
      {"fceumm", "FCEUmm"},
      {"mesen", "Mesen"},
      {"mednafen_psx", "Beetle PSX"},
      {"mednafen_psx_hw", "Beetle PSX HW"},
      {"beetle_psx", "Beetle PSX"},
      {"beetle_psx_hw", "Beetle PSX HW"},
      {"pcsx_rearmed", "PCSX-ReARMed"},
      {"swanstation", "SwanStation"},
      {"gambatte", "Gambatte"},
      {"sameboy", "SameBoy"},
      {"tgbdual", "TGB Dual"},
      {"mgba", "mGBA"},
      {"vba_next", "VBA Next"},
      {"vbam", "VBA-M"},
      {"genesis_plus_gx", "Genesis Plus GX"},
      {"blastem", "BlastEm"},
      {"picodrive", "PicoDrive"},
      {"mupen64plus_next", "Mupen64Plus-Next"},
      {"parallel_n64", "ParaLLEl N64"},
      {"mednafen_saturn", "Beetle Saturn"},
      {"beetle_saturn", "Beetle Saturn"},
      {"kronos", "Kronos"},
      {"mame", "MAME"},
      {"fbneo", "FBNeo"},
      {"fbalpha", "FB Alpha"},
      {"pcsx2", "PCSX2"},
      {"play", "Play!"},
      {"dolphin", "Dolphin"},
      {"flycast", "Flycast"},
      {"redream", "Redream"},
      {"stella", "Stella"},
      {"mednafen_pce", "Beetle PCE"},
      {"mednafen_pce_fast", "Beetle PCE Fast"},
      {"beetle_pce", "Beetle PCE"},
      {"beetle_pce_fast", "Beetle PCE Fast"},
      {"dosbox_pure", "DOSBox-Pure"},
      {"scummvm", "ScummVM"},
      {"ppsspp", "PPSSPP"},
      {"desmume", "DeSmuME"},
      {"melonds", "melonDS"},
      {"citra", "Citra"},
  };
  return kCores;
}

std::string RetroArchCoreId(std::string_view folder) {
  const std::string lowered = LowerAscii(folder);
  // The first match, as RomM's reversed dict comprehension leaves it.
  for (const RetroArchCore& core : RetroArchCores()) {
    if (LowerAscii(core.folder) == lowered) {
      return core.id;
    }
  }
  return {};
}

std::string RetroArchCoreFolder(std::string_view id) {
  for (const RetroArchCore& core : RetroArchCores()) {
    if (id == core.id) {
      return core.folder;
    }
  }
  return std::string(id);
}

std::vector<std::string> RetroArchAliases(std::string_view id) {
  if (id.empty()) {
    return {};
  }
  const std::string folder = RetroArchCoreFolder(id);
  std::set<std::string> aliases = {std::string(id), folder};
  for (const RetroArchCore& core : RetroArchCores()) {
    if (folder == core.folder) {
      aliases.insert(core.id);
    }
  }
  return std::vector<std::string>(aliases.begin(), aliases.end());
}

bool IsRetroArchCore(std::string_view id) {
  for (const RetroArchCore& core : RetroArchCores()) {
    if (id == core.id) {
      return true;
    }
  }
  return false;
}

bool SameCore(std::string_view written_by, std::string_view loaded_by) {
  if (written_by.empty() || loaded_by.empty()) {
    return false;
  }
  if (written_by == loaded_by) {
    return true;
  }
  if (!IsRetroArchCore(loaded_by)) {
    return false;
  }
  const std::vector<std::string> aliases = RetroArchAliases(loaded_by);
  for (const std::string& alias : aliases) {
    if (alias == written_by) {
      return true;
    }
  }
  return false;
}

bool SaveFormat::Writes(std::string_view extension) const {
  if (extension.empty()) {
    return false;
  }
  if (extension == primary) {
    return true;
  }
  for (const char* other : also) {
    if (other != nullptr && extension == other) {
      return true;
    }
  }
  return false;
}

const SaveFormat& LibretroFormat() { return kLibretroFormat; }

SaveFormat SaveFormatOf(std::string_view emulator_id) {
  if (IsRetroArchCore(emulator_id)) {
    return kLibretroFormat;
  }
  if (IsIsolated(emulator_id)) {
    for (const TicoModule& module : kTicoModules) {
      if (emulator_id == module.emulator_id) {
        return module.format;
      }
    }
  }
  return {};
}

const TicoModule* TicoModuleFor(std::string_view tico_slug) {
  for (const TicoModule& module : kTicoModules) {
    if (tico_slug == module.slug) {
      return &module;
    }
  }
  return nullptr;
}

std::string TicoSlugFor(std::string_view romm_slug) {
  for (const SlugSpelling& spelling : kTicoSpellings) {
    if (romm_slug == spelling.romm) {
      return spelling.tico;
    }
  }
  return std::string(romm_slug);
}

std::string DefaultEmulatorId(Source source, std::string_view platform_fs_slug) {
  const std::string slug = TicoSlugFor(platform_fs_slug);
  if (source == Source::kUnknown) {
    return {};
  }
  if (source == Source::kRetroArch) {
    for (const RetroArchDefault& entry : kRetroArchOwnCores) {
      if (slug == entry.slug) {
        return entry.emulator_id;
      }
    }
  }
  const TicoModule* module = TicoModuleFor(slug);
  if (module == nullptr) {
    return {};
  }
  const std::string_view id = module->emulator_id;
  // A `tico-*` id is a format RetroArch never writes, so it is never RetroArch's.
  return source == Source::kRetroArch && id.rfind("tico-", 0) == 0 ? std::string()
                                                                     : std::string(id);
}

bool IsIsolated(std::string_view emulator_id) {
  return emulator_id.rfind("tico-", 0) == 0;
}

bool Interchangeable(std::string_view local, std::string_view server) {
  if (IsIsolated(local) || IsIsolated(server)) {
    return local == server;
  }
  return true;
}

std::string EmulatorAt(std::string_view sd_path, std::string_view platform_fs_slug,
                       const config::Config& config) {
  const std::size_t slash = sd_path.rfind('/');
  if (slash == std::string_view::npos || slash == 0) {
    return {};
  }
  return ResolveSource(SourceOf(sd_path.substr(0, slash)), platform_fs_slug, config).emulator_id;
}

bool ValidEmulatorId(std::string_view id, std::string* why) {
  if (id.empty()) {
    *why = "is empty";
    return false;
  }
  if (CodePoints(id) > kMaxEmulatorIdLength) {
    *why = "is longer than the " + std::to_string(kMaxEmulatorIdLength) +
           " characters RomM allows an emulator";
    return false;
  }
  for (const char character : id) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7f) {
      *why = "contains a control character";
      return false;
    }
  }
  // RomM's `sanitize_filename` would rewrite any of these, and an emulator it
  // would rewrite is one it refuses: the id is a directory in the stored path.
  if (id.find_first_of("\\/:|*?\"<>") != std::string_view::npos) {
    *why = R"(contains one of \ / : | * ? " < >, which RomM refuses in an emulator)";
    return false;
  }
  if (id.front() == ' ' || id.back() == ' ') {
    *why = "starts or ends with a space, which RomM would strip";
    return false;
  }
  if (id == "." || id == "..") {
    *why = "is a directory reference rather than a name";
    return false;
  }
  return true;
}

SaveSource SourceOf(std::string_view folder) {
  SaveSource where;
  where.folder = std::string(folder);
  const std::vector<std::string_view> segments = Segments(folder);
  for (const std::string_view segment : segments) {
    const std::string lowered = LowerAscii(segment);
    if (lowered == "retroarch") {
      where.source = Source::kRetroArch;
      break;
    }
    if (lowered == "tico") {
      where.source = Source::kTico;
      break;
    }
  }
  switch (where.source) {
    case Source::kRetroArch: {
      // Flat only when the folder is one RetroArch keeps for every system --
      // `saves/`, `states/`, or RetroArch's own root. Any other leaf is a folder
      // somebody made or a core RomM's table does not know, and its saves get
      // the override or nothing, never a guessed core.
      const std::string leaf = LowerAscii(segments.back());
      where.emulator_id = RetroArchCoreId(segments.back());
      if (!where.emulator_id.empty()) {
        where.layout = Layout::kCoreFolder;
      } else if (leaf == "saves" || leaf == "states" || leaf == "retroarch") {
        where.layout = Layout::kFlat;
      }
      break;
    }
    case Source::kTico:
      where.layout = Layout::kPlatformFolder;
      break;
    case Source::kUnknown:
      break;
  }
  return where;
}

SaveSource ResolveSource(const SaveSource& folder, std::string_view platform_fs_slug,
                         const config::Config& config) {
  SaveSource resolved = folder;
  resolved.platform_fs_slug = std::string(platform_fs_slug);
  if (resolved.layout == Layout::kCoreFolder) {
    return resolved;
  }
  const std::string_view chosen = config.EmulatorOverride(platform_fs_slug);
  if (!chosen.empty()) {
    resolved.emulator_id = std::string(chosen);
  } else if (resolved.layout != Layout::kUnknown) {
    resolved.emulator_id = DefaultEmulatorId(folder.source, platform_fs_slug);
  }
  return resolved;
}

}  // namespace rommsync::scan
