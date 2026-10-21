#include "rommsync/placement.hpp"

#include "rommsync/save_scan.hpp"
#include "rommsync/save_source.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rommsync::place {
namespace {

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (char& character : out) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return out;
}

/// A libretro core RomM's table names. Its saves are RetroArch's SRAM format,
/// whichever frontend ran it -- Tico's libretro modules included.
bool IsLibretro(std::string_view id) {
  for (const scan::RetroArchCore& core : scan::RetroArchCores()) {
    if (id == core.id) {
      return true;
    }
  }
  return false;
}

/// The extensions an emulator writes its saves under, as far as this build
/// knows: `primary` is the one a save is written as when the server's name
/// gives no better answer, `also` the others it writes beside it. Both empty is
/// an emulator whose names are not known.
struct Format {
  std::string_view primary;
  std::vector<std::string_view> also;
  bool folders = false;  ///< its saves are directories, which are not placed
};

Format FormatOf(std::string_view id) {
  if (IsLibretro(id)) {
    // RetroArch's SRAM, and the real-time clock some cores keep beside it.
    return {".srm", {".rtc"}, false};
  }
  if (id == "tico-duckstation") {
    return {".mcd", {}, false};
  }
  if (id == "tico-mupen64plus") {
    // EEPROM, SRAM, FlashRAM and the controller pak: four saves of one game,
    // each its own, so only the server's name can say which this is (#238).
    return {"", {".eep", ".sra", ".fla", ".mpk"}, false};
  }
  if (id == "tico-ppsspp") {
    return {"", {}, true};
  }
  return {};
}

/// An extension made only of what a slot is made of (`scan::SlotFor`), so the
/// name the scan reads back gives the slot the server sent.
bool PlainExtension(std::string_view extension) {
  if (extension.size() < 2 || extension.front() != '.') {
    return false;
  }
  for (const char character : extension.substr(1)) {
    const bool plain = (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9') || character == '-' ||
                       character == '_';
    if (!plain) {
      return false;
    }
  }
  return true;
}

Placement Refuse(std::string reason) {
  Placement refused;
  refused.reason = std::move(reason);
  return refused;
}

/// The extension a save of `emulator_id` is written under, or "" and `why`.
std::string ExtensionFor(std::string_view emulator_id, std::string_view server_file_name,
                         std::string* why) {
  if (emulator_id.empty()) {
    *why = "no emulator is known for it, so neither is the name it would load";
    return {};
  }
  const Format format = FormatOf(emulator_id);
  if (format.folders) {
    *why = std::string(emulator_id) + " keeps its saves as folders, which are not placed (#238)";
    return {};
  }
  const std::string theirs = LowerAscii(sync::ExtensionOf(server_file_name));
  if (!format.primary.empty() && theirs == format.primary) {
    return theirs;
  }
  for (const std::string_view also : format.also) {
    if (theirs == also) {
      return theirs;
    }
  }
  if (format.primary == ".srm" && theirs == ".sav") {
    // The same SRAM under another client's name: mGBA's own, a web player's.
    return std::string(format.primary);
  }
  if (format.primary.empty() && format.also.empty()) {
    // A module whose names nobody has read off a console yet [#44]. The save
    // can only have come from that same module (`Compatible`), so the name it
    // uploaded under is the name it writes.
    if (PlainExtension(theirs)) {
      return theirs;
    }
    *why = "the server's copy names no extension " + std::string(emulator_id) +
           " is known to write";
    return {};
  }
  const std::string named = theirs.empty() ? "has no extension" : "is a " + theirs + " file";
  *why = "the server's copy " + named + ", which is not a format " + std::string(emulator_id) +
         " writes";
  return {};
}

}  // namespace

Placement Place(const roms::Rom& rom, std::string_view emulator_id,
                std::string_view server_file_name, const Layout& layout) {
  if (layout.folder.empty() || layout.folder.front() != '/') {
    return Refuse("there is no folder to put it in");
  }
  // The rom's name comes off the server's filesystem, and this is the call that
  // joins it into a path on the card.
  const std::string_view stem = rom.fs_name_no_ext;
  if (stem.empty() || stem == "." || stem == ".." ||
      stem.find_first_of("/\\") != std::string_view::npos ||
      std::any_of(stem.begin(), stem.end(),
                  [](char character) { return static_cast<unsigned char>(character) < 0x20; })) {
    return Refuse("the rom's name is not one a file can be called");
  }
  std::string why;
  const std::string extension = ExtensionFor(emulator_id, server_file_name, &why);
  if (extension.empty()) {
    return Refuse(why);
  }

  std::string path = layout.folder;
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  if (layout.by_core) {
    if (!IsLibretro(emulator_id)) {
      // `RetroArchCoreFolder` would hand the id back unchanged, and a folder
      // named after it is one RetroArch never reads.
      return Refuse("RetroArch sorts saves by core, and RomM's core table has no folder for " +
                    std::string(emulator_id));
    }
    path += "/" + scan::RetroArchCoreFolder(emulator_id);
  }
  path += "/" + std::string(stem) + extension;
  if (path.size() > config::kMaxPathLength) {
    return Refuse("the path would be longer than the " + std::to_string(config::kMaxPathLength) +
                  " characters this client writes");
  }
  Placement placed;
  placed.sd_path = std::move(path);
  return placed;
}

bool Compatible(std::string_view server_emulator, std::string_view target_emulator) {
  if (target_emulator.empty()) {
    return false;
  }
  if (server_emulator == target_emulator) {
    return true;
  }
  if (server_emulator.empty()) {
    return IsLibretro(target_emulator);
  }
  if (!IsLibretro(target_emulator)) {
    return false;
  }
  const std::vector<std::string> aliases = scan::RetroArchAliases(target_emulator);
  return std::find(aliases.begin(), aliases.end(), server_emulator) != aliases.end();
}

Placer::Placer(const config::Config& config, const retroarch::Settings& retroarch,
               const roms::RomIndex& index, fs::FileSystem& files)
    : config_(config), retroarch_(retroarch), index_(index), files_(files) {}

Placer::Target Placer::TargetFor(const roms::Rom& rom, std::string_view server_emulator,
                                 bool saves) {
  Target target;
  const std::string& slug = rom.platform_fs_slug;
  const std::string what = saves ? "saves" : "states";
  const config::PlatformFolders* folders = config_.Platform(slug);
  if (folders == nullptr) {
    target.reason = slug + " is not mapped to any folder";
    return target;
  }
  const std::optional<config::PlayWith> play_with = config_.PlayWithFor(slug);
  if (!play_with.has_value()) {
    target.reason = "no emulator is chosen to play " + slug +
                    " on this console: [platform." + slug +
                    "] play_with = tico or retroarch says which";
    return target;
  }
  target.layout.play_with = *play_with;

  if (*play_with == config::PlayWith::kTico) {
    for (const std::string& folder : saves ? folders->saves : folders->states) {
      if (scan::SourceOf(folder).source == scan::Source::kTico) {
        target.map_folder = folder;
        break;
      }
    }
    const std::string root = tico::RootOf(target.map_folder);
    if (target.map_folder.empty() || root.empty()) {
      target.reason = slug + " is played in Tico and the folder map lists no Tico folder for its " +
                      what;
      return target;
    }
    auto found = roots_.find(LowerAscii(root));
    if (found == roots_.end()) {
      found = roots_.emplace(LowerAscii(root), tico::Resolve(config_.tico, files_, root)).first;
    }
    const tico::Resolution& resolved = found->second;
    if (resolved.layout == tico::Layout::kUndecided) {
      // Writing into the wrong account is someone else's save (#231).
      target.reason = resolved.why.empty()
                          ? "Tico keeps " + what + " for more than one account here, and none is "
                                                   "chosen in [tico] account"
                          : resolved.why;
      return target;
    }
    target.layout.folder = tico::FolderIn(resolved, target.map_folder);
    scan::SaveSource source = scan::SourceOf(target.map_folder);
    source.layout = resolved.layout == tico::Layout::kAccount ? scan::Layout::kAccountFolder
                                                               : scan::Layout::kPlatformFolder;
    target.emulator_id = scan::ResolveSource(source, slug, config_).emulator_id;
  } else {
    if (!retroarch::Enabled(config_, retroarch_)) {
      target.reason = config_.sources.retroarch == config::SourceMode::kOff
                          ? "[sources] retroarch = off, so nothing is written to RetroArch's "
                            "folders"
                          : "RetroArch syncs its own " + what + " itself, so nothing is written "
                                                                "to its folders";
      return target;
    }
    const bool by_content =
        saves ? retroarch_.sort_savefiles_by_content : retroarch_.sort_savestates_by_content;
    const bool beside_roms =
        saves ? retroarch_.savefiles_in_content_dir : retroarch_.savestates_in_content_dir;
    if (by_content) {
      target.reason = "RetroArch keeps " + what +
                      " in a folder per content directory, a layout this client does not read";
      return target;
    }
    if (beside_roms) {
      if (!config_.sources.retroarch_content_dir) {
        target.reason = "RetroArch keeps " + what +
                        " beside the roms, which is read only with [sources] "
                        "retroarch_content_dir = true";
        return target;
      }
      // Where the rom itself is downloaded: RetroArch takes the loaded
      // content's folder.
      target.layout.folder = config_.RomTarget(slug);
      if (target.layout.folder.empty()) {
        target.reason = "RetroArch keeps " + what + " beside the roms, and " + slug +
                        " maps no rom folder";
        return target;
      }
    } else {
      const std::string configured =
          saves ? retroarch_.savefile_directory : retroarch_.savestate_directory;
      const std::string fallback = saves ? retroarch::kDefaultSavefileDirectory
                                         : retroarch::kDefaultSavestateDirectory;
      target.layout.folder = configured;
      if (LowerAscii(configured) != LowerAscii(fallback)) {
        // RetroArch ignores a folder that is not a directory and writes to its
        // default instead, and so does the scan (`scan::ScanSaves`).
        const fs::ListError error = files_.List(configured).error;
        if (error == fs::ListError::kMissing || error == fs::ListError::kNotADirectory) {
          target.layout.folder = fallback;
        }
      }
    }
    target.map_folder = target.layout.folder;
    target.layout.by_core = saves ? retroarch_.sort_savefiles : retroarch_.sort_savestates;
    if (target.layout.by_core && IsLibretro(server_emulator)) {
      // Sorted, the core that wrote it has a folder of its own, and that core
      // is the one that loads it.
      target.emulator_id = std::string(server_emulator);
    } else {
      scan::SaveSource flat;
      flat.source = scan::Source::kRetroArch;
      flat.folder = target.layout.folder;
      flat.layout = scan::Layout::kFlat;
      target.emulator_id = scan::ResolveSource(flat, slug, config_).emulator_id;
    }
  }

  if (target.emulator_id.empty()) {
    target.reason = std::string("no ") + config::ToString(*play_with) + " emulator is known for " +
                    slug;
    return target;
  }
  if (!Compatible(server_emulator, target.emulator_id)) {
    target.reason = "the server's copy was written by " +
                    (server_emulator.empty() ? std::string("an unnamed emulator")
                                             : std::string(server_emulator)) +
                    ", whose " + what + " " + target.emulator_id + " does not read";
    return target;
  }
  return target;
}

std::string Placer::ReadsBackAs(const roms::Rom& rom, std::string_view file_name,
                                const Target& target, bool saves) const {
  // The platform the scan takes the folder to imply. Tico's folder implies the
  // map's platform for it; RetroArch's own folders and core folders imply none
  // unless the map lists them under one; a rom folder implies the platforms
  // that keep roms in it.
  std::string hint;
  if (target.layout.play_with == config::PlayWith::kRetroArch &&
      (saves ? retroarch_.savefiles_in_content_dir : retroarch_.savestates_in_content_dir)) {
    bool first = true;
    for (const auto& [slug, mapped] : config_.platforms) {
      if (std::find(mapped.roms.begin(), mapped.roms.end(), target.map_folder) ==
          mapped.roms.end()) {
        continue;
      }
      hint = first ? slug : (hint == slug ? hint : std::string());
      first = false;
    }
  } else {
    const auto hints =
        scan::PlatformHints(config_, saves ? scan::Folders::kSaves : scan::Folders::kStates);
    const auto found = hints.find(target.map_folder);
    hint = found == hints.end() ? std::string() : found->second;
  }
  const roms::Match match = index_.Find(scan::BaseName(file_name), hint);
  if (!match.matched()) {
    return "the scan would not read it back as this rom's (" + match.reason +
           "), so it would come back as a file nobody syncs";
  }
  if (match.rom->id != rom.id) {
    return "the scan would read it back as another rom's";
  }
  return {};
}

Placement Placer::Save(const sync::SyncOperation& operation) {
  const roms::Rom* rom = index_.ById(operation.rom_id);
  if (rom == nullptr) {
    return Refuse("rom " + std::to_string(operation.rom_id) +
                  " is not in this console's copy of the library");
  }
  if (!operation.slot.has_value()) {
    return Refuse("an archival save pairs with nothing, so a copy written here would be "
                  "uploaded back as a new save");
  }
  const Target target = TargetFor(*rom, operation.emulator.value_or(""), true);
  if (!target.reason.empty()) {
    return Refuse(target.reason);
  }
  Placement placed = Place(*rom, target.emulator_id, operation.file_name, target.layout);
  if (!placed.ok()) {
    return placed;
  }
  const std::string leaf = placed.sd_path.substr(placed.sd_path.rfind('/') + 1);
  const scan::Source source = target.layout.play_with == config::PlayWith::kTico
                                  ? scan::Source::kTico
                                  : scan::Source::kRetroArch;
  const std::string slot = scan::SlotFor(scan::ToString(source), leaf);
  if (slot != *operation.slot) {
    // The server's slot is not quoted: it is whatever another client chose.
    return Refuse("the scan would read it back in slot \"" + slot +
                  "\", not the one the server keeps it in, so the next sync would upload it "
                  "as a second save");
  }
  if (const std::string why = ReadsBackAs(*rom, leaf, target, true); !why.empty()) {
    return Refuse(why);
  }
  return placed;
}

Placement Placer::State(std::int64_t rom_id, std::string_view file_name,
                        std::string_view emulator) {
  const roms::Rom* rom = index_.ById(rom_id);
  if (rom == nullptr) {
    return Refuse("rom " + std::to_string(rom_id) +
                  " is not in this console's copy of the library");
  }
  // The server's name, kept: a state pairs on it. Held to the bar the scan
  // holds a local one to, and to the one a path join needs.
  if (file_name.empty() || !sync::IsSingleFileName(file_name) ||
      file_name.find('\\') != std::string_view::npos ||
      std::any_of(file_name.begin(), file_name.end(),
                  [](char character) { return static_cast<unsigned char>(character) < 0x20; })) {
    return Refuse("the state's name is not one a file can be called");
  }
  const Target target = TargetFor(*rom, emulator, false);
  if (!target.reason.empty()) {
    return Refuse(target.reason);
  }
  std::string path = target.layout.folder;
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  if (target.layout.by_core) {
    if (!IsLibretro(target.emulator_id)) {
      return Refuse("RetroArch sorts states by core, and RomM's core table has no folder for " +
                    target.emulator_id);
    }
    path += "/" + scan::RetroArchCoreFolder(target.emulator_id);
  }
  path += "/" + std::string(file_name);
  if (path.size() > config::kMaxPathLength) {
    return Refuse("the path would be longer than the " + std::to_string(config::kMaxPathLength) +
                  " characters this client writes");
  }
  if (target.layout.play_with == config::PlayWith::kRetroArch &&
      retroarch_.savestates_in_content_dir) {
    // Beside the roms the scan reads only RetroArch's own state names.
    const std::string extension = LowerAscii(sync::ExtensionOf(file_name));
    bool numbered = extension.rfind(".state", 0) == 0;
    for (std::size_t i = 6; numbered && i < extension.size(); ++i) {
      numbered = extension[i] >= '0' && extension[i] <= '9';
    }
    if (!numbered) {
      return Refuse("beside the roms only RetroArch's own .state names are read back");
    }
  }
  if (const std::string why = ReadsBackAs(*rom, file_name, target, false); !why.empty()) {
    return Refuse(why);
  }
  Placement placed;
  placed.sd_path = std::move(path);
  return placed;
}

}  // namespace rommsync::place
