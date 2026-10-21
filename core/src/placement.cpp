#include "rommsync/placement.hpp"

#include "rommsync/config_text.hpp"
#include "rommsync/save_scan.hpp"
#include "rommsync/save_source.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rommsync::place {
namespace {

using config::text::LowerAscii;

/// A name the server supplied that a path join may take: one component, no
/// control character. Its own check, not `sync::IsSingleFileName`'s, because
/// that one lets a backslash through for the server's sake and a FAT card
/// reads one as a separator.
bool Joinable(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_of("/\\") == std::string_view::npos &&
         std::none_of(name.begin(), name.end(), [](char character) {
           return static_cast<unsigned char>(character) < 0x20;
         });
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

namespace {

/// `<folder>[/<CoreFolder>]/<name>`, for a `name` already checked `Joinable`.
Placement JoinUnder(const Layout& layout, std::string_view emulator_id, std::string_view name) {
  if (layout.folder.empty() || layout.folder.front() != '/') {
    return Refuse("there is no folder to put it in");
  }
  std::string path = layout.folder;
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  if (layout.by_core) {
    if (!IsLibretro(emulator_id)) {
      // `RetroArchCoreFolder` would hand the id back unchanged, and a folder
      // named after it is one RetroArch never reads.
      return Refuse("RetroArch sorts by core, and RomM's core table has no folder for " +
                    std::string(emulator_id));
    }
    path += "/" + scan::RetroArchCoreFolder(emulator_id);
  }
  path += "/" + std::string(name);
  if (path.size() > config::kMaxPathLength) {
    return Refuse("the path would be longer than the " + std::to_string(config::kMaxPathLength) +
                  " characters this client writes");
  }
  Placement placed;
  placed.sd_path = std::move(path);
  return placed;
}

}  // namespace

Placement Place(const roms::Rom& rom, std::string_view emulator_id,
                std::string_view server_file_name, const Layout& layout) {
  // The rom's name comes off the server's filesystem, and this is the call that
  // joins it into a path on the card.
  if (!Joinable(rom.fs_name_no_ext)) {
    return Refuse("the rom's name is not one a file can be called");
  }
  std::string why;
  const std::string extension = ExtensionFor(emulator_id, server_file_name, &why);
  if (extension.empty()) {
    return Refuse(why);
  }
  return JoinUnder(layout, emulator_id, rom.fs_name_no_ext + extension);
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
    const std::string root =
        target.map_folder.empty() ? std::string() : tico::RootOf(target.map_folder);
    if (root.empty()) {
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
                                const Target& target, bool saves, int card) const {
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
  // Tico's psx folder is read the way DuckStation names its cards (#238).
  const bool memory_cards = saves && target.layout.play_with == config::PlayWith::kTico &&
                            scan::TicoSlugFor(hint) == "psx";
  int read_card = 1;
  const roms::Match match = scan::MatchSave(index_, file_name, hint, memory_cards, &read_card);
  if (!match.matched()) {
    return "the scan would not read it back as this rom's (" + match.reason +
           "), so it would come back as a file nobody syncs";
  }
  if (match.rom->id != rom.id) {
    return "the scan would read it back as another rom's";
  }
  if (read_card != card) {
    return "the scan would read it back as another of the game's memory cards";
  }
  return {};
}

namespace {

/// DuckStation's card number for a save in `slot`: the scan files card 2 in
/// `<slot>-card2` (#238).
int CardOfSlot(std::string_view slot) {
  constexpr std::string_view kCard2 = "-card2";
  return slot.size() > kCard2.size() && slot.substr(slot.size() - kCard2.size()) == kCard2 ? 2
                                                                                            : 1;
}

}  // namespace

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
  // DuckStation names a card after the game, not the disc, and card 2 `_2`
  // (#238): `scan::MemoryCardName`, which is the scan's own rule turned round.
  const bool duckstation = target.emulator_id == "tico-duckstation" &&
                           target.layout.play_with == config::PlayWith::kTico;
  const int card = duckstation ? CardOfSlot(*operation.slot) : 1;
  Placement placed =
      !duckstation ? Place(*rom, target.emulator_id, operation.file_name, target.layout)
      : Joinable(rom->fs_name_no_ext)
          ? JoinUnder(target.layout, target.emulator_id,
                      scan::MemoryCardName(rom->fs_name_no_ext, card))
          : Refuse("the rom's name is not one a file can be called");
  if (!placed.ok()) {
    return placed;
  }
  const std::string leaf = placed.sd_path.substr(placed.sd_path.rfind('/') + 1);
  // The slot the scan gives the file under `[sync] slot` (#236): `autosave` for
  // a `.srm` whichever emulator's folder it is in, `autosave-<ext>` otherwise --
  // and `-card2` after it for DuckStation's second card (#238).
  std::string slot = scan::SlotFor(config_.sync.slot, leaf);
  if (card > 1) {
    slot += "-card" + std::to_string(card);
  }
  if (slot != *operation.slot) {
    // The server's slot is not quoted: it is whatever another client chose.
    return Refuse("the scan would read it back in slot \"" + slot +
                  "\", not the one the server keeps it in, so the next sync would upload it "
                  "as a second save");
  }
  // A card kept under a disc-2 rom is named for the game, which the scan reads
  // back as disc 1's: refused here, so it never lands on disc 1's card.
  if (const std::string why = ReadsBackAs(*rom, leaf, target, true, card); !why.empty()) {
    return Refuse(why);
  }
  placed.emulator = target.emulator_id;
  return placed;
}

Placement Placer::State(std::int64_t rom_id, std::string_view file_name,
                        std::string_view emulator) {
  const roms::Rom* rom = index_.ById(rom_id);
  if (rom == nullptr) {
    return Refuse("rom " + std::to_string(rom_id) +
                  " is not in this console's copy of the library");
  }
  // The server's name, kept: a state pairs on it, and this is the call that
  // joins it into a path.
  if (!Joinable(file_name)) {
    return Refuse("the state's name is not one a file can be called");
  }
  const Target target = TargetFor(*rom, emulator, false);
  if (!target.reason.empty()) {
    return Refuse(target.reason);
  }
  Placement placed = JoinUnder(target.layout, target.emulator_id, file_name);
  if (!placed.ok()) {
    return placed;
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
  return placed;
}

}  // namespace rommsync::place
