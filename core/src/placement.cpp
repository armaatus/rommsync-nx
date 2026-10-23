#include "rommsync/placement.hpp"

#include "rommsync/config_text.hpp"
#include "rommsync/save_scan.hpp"
#include "rommsync/save_source.hpp"
#include "rommsync/state_names.hpp"

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

/// A refusal because the file is not this console's to load (#239).
Placement Foreign(std::string reason) {
  Placement refused = Refuse(std::move(reason));
  refused.foreign = true;
  return refused;
}

/// The extension a save of `emulator_id` is written under, or "" and `why`.
std::string ExtensionFor(std::string_view emulator_id, std::string_view server_file_name,
                         std::string* why) {
  if (emulator_id.empty()) {
    *why = "no emulator is known for it, so neither is the name it would load";
    return {};
  }
  // The emulator's format, from the one table that says (#258,
  // `scan::SaveFormatOf`): a libretro core's, or a Tico module's own.
  const scan::SaveFormat format = scan::SaveFormatOf(emulator_id);
  if (format.folders) {
    *why = std::string(emulator_id) + " keeps its saves as folders, which are not placed (#238)";
    return {};
  }
  const std::string theirs = LowerAscii(sync::ExtensionOf(server_file_name));
  if (format.Writes(theirs)) {
    return theirs;
  }
  if (std::string_view(format.primary) == ".srm" && theirs == ".sav") {
    // The same SRAM under another client's name: mGBA's own, a web player's.
    return format.primary;
  }
  if (!format.Known()) {
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
    if (!scan::IsRetroArchCore(emulator_id)) {
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
  if (server_emulator.empty()) {
    return scan::IsRetroArchCore(target_emulator);
  }
  return scan::SameCore(server_emulator, target_emulator);
}

bool LoadsState(std::string_view server_emulator, std::string_view target_emulator) {
  return scan::SameCore(server_emulator, target_emulator);
}

Placer::Placer(const config::Config& config, const retroarch::Settings& retroarch,
               const roms::RomIndex& index, fs::FileSystem& files)
    : config_(config), retroarch_(retroarch), index_(index), files_(files) {}

Placer::Target Placer::TargetFor(const roms::Rom& rom, std::string_view server_emulator,
                                 scan::Folders which) {
  Target target;
  const bool saves = which == scan::Folders::kSaves;
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
    std::string map_folder;
    for (const std::string& folder : saves ? folders->saves : folders->states) {
      if (scan::SourceOf(folder).source == scan::Source::kTico) {
        map_folder = folder;
        break;
      }
    }
    const std::string root = map_folder.empty() ? std::string() : tico::RootOf(map_folder);
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
    target.layout.folder = tico::FolderIn(resolved, map_folder);
    scan::SaveSource source = scan::SourceOf(map_folder);
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
    const retroarch::Folder layout = saves ? retroarch_.Saves() : retroarch_.States();
    if (layout.sort_by_content) {
      target.reason = "RetroArch keeps " + what +
                      " in a folder per content directory, a layout this client does not read";
      return target;
    }
    if (layout.in_content_dir) {
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
      // The scan's own rule for where RetroArch writes (#258).
      target.layout.folder = retroarch::EffectiveDirectory(layout, files_);
    }
    target.layout.by_core = layout.sort_by_core;
    if (target.layout.by_core && scan::IsRetroArchCore(server_emulator)) {
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
  if (saves ? !Compatible(server_emulator, target.emulator_id)
            : !LoadsState(server_emulator, target.emulator_id)) {
    target.foreign = true;
    target.reason = "the server's copy was written by " +
                    (server_emulator.empty() ? std::string("an unnamed emulator")
                                             : std::string(server_emulator)) +
                    ", whose " + what + " " + target.emulator_id + " does not read";
    return target;
  }
  return target;
}

scan::FolderPlan& Placer::PlanOf(scan::Folders which) {
  std::optional<scan::FolderPlan>& plan =
      which == scan::Folders::kSaves ? saves_plan_ : states_plan_;
  if (!plan.has_value()) {
    plan = scan::PlanFolders(config_, retroarch_, which, files_, nullptr);
  }
  return *plan;
}

void Placer::CountMadeFolder(const std::string& sd_path, scan::Folders which) {
  std::string directory;
  std::string leaf;
  if (!fs::SplitPath(sd_path, &directory, &leaf)) {
    return;
  }
  scan::FolderPlan& plan = PlanOf(which);  // planned already, by `ReadsBackAs`
  const std::string wanted = LowerAscii(directory);
  for (const scan::ScanFolder& folder : plan.folders) {
    if (LowerAscii(folder.directory) == wanted) {
      return;  // planned already: there on the card, or claimed earlier this tick
    }
  }
  // Not planned, yet read: a core folder this placement makes (#269). Only
  // this kind's plan is told. Where saves and states share a sorted folder the
  // states plan still sees the saves' folders, since the engine places and
  // fetches every save before it makes the states plan, on its first `State`.
  std::optional<scan::ScanFolder> made = scan::FolderIn(plan, directory);
  if (made.has_value()) {
    plan.folders.push_back(std::move(*made));
    ++plan.core_folders;
  }
}

std::string Placer::ReadsBackAs(const roms::Rom& rom, const std::string& sd_path,
                                scan::Folders which, int card) {
  // The scan's own plan of folders, and its own rules within one, so what is
  // placed is what it reads back (#258): a rom folder reads only RetroArch's
  // names, a listing cut at `fs::kMaxDirectoryEntries` reads only what it
  // reaches, and a folder the walk does not plan is not read at all.
  std::string directory;
  std::string leaf;
  if (!fs::SplitPath(sd_path, &directory, &leaf)) {
    return "there is no folder to put it in";
  }
  const std::optional<scan::ScanFolder> folder = scan::FolderIn(PlanOf(which), directory);
  if (!folder.has_value()) {
    return "the scan does not read " + directory + ", so it would come back as a file nobody syncs";
  }
  if (scan::RuleFor(*folder, leaf, which) != scan::FileRule::kRead) {
    return "the scan does not read a file of that kind in " + directory +
           (folder->beside_roms ? ", a rom folder" : "") +
           ", so it would come back as a file nobody syncs";
  }
  // The folder the new entry lands in: the file's own, or -- for a folder made
  // for it -- the first one up that is there.
  std::string holder = directory;
  fs::Listing listing = files_.List(holder);
  while (listing.error == fs::ListError::kMissing && holder.rfind('/') != 0 &&
         holder.rfind('/') != std::string::npos) {
    holder.resize(holder.rfind('/'));
    listing = files_.List(holder);
  }
  if (listing.error == fs::ListError::kTooManyEntries) {
    return holder + " holds more than the " + std::to_string(fs::kMaxDirectoryEntries) +
           " entries the scan lists, which it keeps by name, so a new one there may never come "
           "back or may push out a save that syncs now";
  }
  int read_card = 1;
  // A state is matched as the scan matches one: RetroArch's `.state.auto` by
  // its rom's name (#239).
  const roms::Match match = scan::MatchSave(
      index_, which == scan::Folders::kSaves ? leaf : scan::StateMatchName(leaf),
      folder->platform_hint, folder->memory_cards, &read_card);
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
  const Target target = TargetFor(*rom, operation.emulator.value_or(""), scan::Folders::kSaves);
  if (!target.reason.empty()) {
    return Refuse(target.reason);
  }
  // A module with memory cards names one after the game, not the disc, and
  // card 2 `_2` (#238): `scan::MemoryCardName`, which is the scan's own rule
  // turned round.
  const bool memory_cards = scan::SaveFormatOf(target.emulator_id).memory_cards;
  const int card = memory_cards ? CardOfSlot(*operation.slot) : 1;
  Placement placed =
      !memory_cards ? Place(*rom, target.emulator_id, operation.file_name, target.layout)
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
    if (LowerAscii(sync::ExtensionOf(operation.file_name)) == ".sav" &&
        LowerAscii(sync::ExtensionOf(leaf)) == ".srm") {
      // #237's (1), decided in #258: a `.sav` kept in a slot of its own stays
      // there. Written as the `.srm` the emulator loads, the scan would read it
      // back in another slot -- a second save of the game, uploaded next tick
      // while this row is offered again; kept as `.sav`, nothing here loads it.
      return Refuse("the server keeps it as a .sav in a slot of its own; as the .srm " +
                    target.emulator_id +
                    " loads, the scan would read it back in slot \"" + slot +
                    "\" as a second save, so it stays on the server");
    }
    // The server's slot is not quoted: it is whatever another client chose.
    return Refuse("the scan would read it back in slot \"" + slot +
                  "\", not the one the server keeps it in, so the next sync would upload it "
                  "as a second save");
  }
  // A card kept under a disc-2 rom is named for the game, which the scan reads
  // back as disc 1's: refused here, so it never lands on disc 1's card.
  if (const std::string why = ReadsBackAs(*rom, placed.sd_path, scan::Folders::kSaves, card);
      !why.empty()) {
    return Refuse(why);
  }
  CountMadeFolder(placed.sd_path, scan::Folders::kSaves);
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
  const Target target = TargetFor(*rom, emulator, scan::Folders::kStates);
  if (!target.reason.empty()) {
    return target.foreign ? Foreign(target.reason) : Refuse(target.reason);
  }
  // The name the target frontend loads the slot from. A state in a slot it has
  // no file for is a state it cannot load, so it is passed over like another
  // core's -- and a file it would never read is not left in its folder (#239).
  std::string name(file_name);
  if (target.layout.play_with == config::PlayWith::kTico) {
    if (scan::MapsStateSlots(config_, target.emulator_id)) {
      const std::optional<std::string> tico = scan::TicoStateName(file_name);
      if (!tico.has_value()) {
        return Foreign("it is in a RetroArch slot Tico has no slot for");
      }
      name = *tico;
    }
    if (!scan::IsTicoStateName(name)) {
      return Foreign(std::string("Tico loads a state only as .state0 to .state5") +
                     (config_.sync.map_state_slots
                          ? ""
                          : "; [sync] map_state_slots = true maps RetroArch's slots onto them"));
    }
  } else if (!scan::IsRetroArchStateName(name)) {
    // Also every name the scan reads beside the roms, where only RetroArch's
    // own are read back.
    return Foreign("RetroArch loads a state only as .state, .state<N> or .state.auto");
  }
  Placement placed = JoinUnder(target.layout, target.emulator_id, name);
  if (!placed.ok()) {
    return placed;
  }
  if (const std::string why = ReadsBackAs(*rom, placed.sd_path, scan::Folders::kStates);
      !why.empty()) {
    return Refuse(why);
  }
  CountMadeFolder(placed.sd_path, scan::Folders::kStates);
  return placed;
}

}  // namespace rommsync::place
