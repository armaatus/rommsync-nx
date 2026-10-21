#include "rommsync/save_source.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace rommsync::scan {
namespace {

/// ASCII-only lowering, for save_scan.cpp's reason: a path segment compared
/// against `retroarch` must not depend on the locale.
std::string LowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char character : text) {
    out.push_back((character >= 'A' && character <= 'Z')
                      ? static_cast<char>(character - 'A' + 'a')
                      : character);
  }
  return out;
}

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

/// The RetroArch core a system's flat `saves/` is filed under, keyed by Tico's
/// spelling of the system so the two tables line up.
///
/// The libretro systems repeat Tico's module on purpose -- the same core on two
/// devices is one lineage on the server, which is the point of M10. The others
/// are the cores RomM's web player uses for those systems, so a flat RetroArch
/// save at least meets the browser's.
struct RetroArchDefault {
  const char* slug;
  const char* emulator_id;
};

constexpr RetroArchDefault kRetroArchDefaults[] = {
    {"nes", "fceumm"},
    {"snes", "snes9x"},
    {"gb", "gambatte"},
    {"gbc", "gambatte"},
    {"gba", "mgba"},
    {"fbneo", "fbneo"},
    {"genesis", "genesis_plus_gx"},
    {"master-system", "genesis_plus_gx"},
    {"game-gear", "genesis_plus_gx"},
    {"sega-cd", "genesis_plus_gx"},
    {"n64", "mupen64plus_next"},
    {"psx", "pcsx_rearmed"},
    {"dc", "flycast"},
    {"nds", "melonds"},
    {"psp", "ppsspp"},
};

// #229's table. A `tico-*` id is a format no libretro core shares (#226 D3):
// Mupen64Plus is built without libretro and splits N64 saves by type,
// DuckStation's `.mcd` is not a Beetle `.srm` until somebody proves it is, and
// the rest are standalone emulators whose files nobody has compared yet. An
// adapter that rewrites one into the other comes after #44 captures real files.
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
    {"n64", "Mupen64Plus", "tico-mupen64plus"},
    {"psx", "DuckStation", "tico-duckstation"},
    {"dc", "Flycast", "tico-flycast"},
    {"nds", "DraStic", "tico-drastic"},
    {"saturn", "YabaSanshiro", "tico-yabasanshiro"},
    {"psp", "PPSSPP", "tico-ppsspp"},
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

const char* ToString(Layout layout) {
  switch (layout) {
    case Layout::kFlat:
      return "flat";
    case Layout::kCoreFolder:
      return "core folder";
    case Layout::kPlatformFolder:
      return "platform folder";
    case Layout::kUnknown:
      break;
  }
  return "unknown";
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
  if (source == Source::kTico) {
    const TicoModule* module = TicoModuleFor(slug);
    return module == nullptr ? std::string() : std::string(module->emulator_id);
  }
  if (source == Source::kRetroArch) {
    for (const RetroArchDefault& entry : kRetroArchDefaults) {
      if (slug == entry.slug) {
        return entry.emulator_id;
      }
    }
  }
  return {};
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
    case Source::kRetroArch:
      where.emulator_id = segments.empty() ? std::string() : RetroArchCoreId(segments.back());
      where.layout = where.emulator_id.empty() ? Layout::kFlat : Layout::kCoreFolder;
      break;
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
  if (resolved.layout == Layout::kCoreFolder && !resolved.emulator_id.empty()) {
    return resolved;
  }
  const std::string_view chosen = config.EmulatorOverride(platform_fs_slug);
  resolved.emulator_id =
      chosen.empty() ? DefaultEmulatorId(folder.source, platform_fs_slug) : std::string(chosen);
  return resolved;
}

}  // namespace rommsync::scan
