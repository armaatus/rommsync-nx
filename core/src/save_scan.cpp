#include "rommsync/save_scan.hpp"

#include "rommsync/state_names.hpp"
#include "rommsync/tico.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rommsync::scan {
namespace {

/// ASCII-only lowering, deliberately: a platform slug and a save extension are
/// ASCII, and a locale-aware `tolower` on a UTF-8 byte is how a Turkish locale
/// turns `I` into something that matches nothing.
char LowerAscii(char character) {
  return (character >= 'A' && character <= 'Z')
             ? static_cast<char>(character - 'A' + 'a')
             : character;
}

std::string LowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char character : text) {
    out.push_back(LowerAscii(character));
  }
  return out;
}

/// RomM's `SLOTTED_SAVE_EXTENSION`: the one save file its RetroArch path files
/// as a version in a slot. Everything beside it -- `.rtc` -- it keeps out, "since
/// a slot holds one save" (`handler/sync/retroarch/sync_handler.py`, 5.4.0).
constexpr std::string_view kSlottedExtension = "srm";

/// The slot for a save whose normalised extension is `extension`. See `SlotFor`.
std::string SlotForExtension(std::string_view slot, std::string_view extension) {
  if (extension == kSlottedExtension) {
    return std::string(slot);
  }
  return std::string(slot) + "-" + std::string(extension);
}

/// The largest whole second a `sync::Timestamp` can actually hold.
///
/// `sync.hpp` warns that `system_clock::time_point` cannot be relied on to hold
/// `kMaxTimestampSeconds`, because its tick is implementation-defined and a
/// nanosecond one runs out in 2262 -- which is libstdc++, so it is the Switch
/// toolchain and CI, not a hypothetical. Multiplying a larger mtime into that
/// representation is signed overflow, and the values it wraps to are the
/// dangerous kind: a year-2600 mtime wraps to a *plausible recent* instant that
/// `sync::Validate` accepts and the server then arbitrates as newer than its own
/// copy. So the bound is computed from the clock rather than assumed.
constexpr std::int64_t MaxRepresentableSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(sync::Timestamp::duration::max())
      .count();
}

/// The window an mtime has to be in to mean anything: past the epoch, and
/// spellable both by `FormatTimestamp` and by the clock underneath it.
constexpr std::int64_t kLatestUsableMtime =
    sync::kMaxTimestampSeconds < MaxRepresentableSeconds() ? sync::kMaxTimestampSeconds
                                                           : MaxRepresentableSeconds();

Skip MakeSkip(SkipReason reason, std::string sd_path, std::string message) {
  Skip skip;
  skip.reason = reason;
  skip.sd_path = std::move(sd_path);
  skip.message = std::move(message);
  return skip;
}

/// A file reported *without* a digest, rather than dropped.
///
/// A save the card would not open this once is still the user's save, and the
/// server compares it on timestamps -- less precisely, and it will plan an
/// upload for bytes it may already have, which is why this is counted. A state
/// with no digest is the same shape of answer for a different reason: with no
/// digest this client cannot tell whether the local file has changed, so it
/// re-uploads it.
void RecordUnhashed(ScanResult* result, const std::string& sd_path,
                    const state::HashOutcome& hashed) {
  ++result->unhashed_total;
  if (result->unhashed.size() < kMaxSkipsReported) {
    result->unhashed.push_back(MakeSkip(
        SkipReason::kUnhashed, sd_path,
        hashed.message.empty() ? std::string(state::ToString(hashed.error)) : hashed.message));
  }
}

std::string Join(std::string_view directory, std::string_view name) {
  std::string path(directory);
  if (path.empty() || path.back() != '/') {
    path.push_back('/');
  }
  path.append(name);
  return path;
}

}  // namespace

const char* ToString(SkipReason reason) {
  switch (reason) {
    case SkipReason::kUnmatched:
      return "unmatched";
    case SkipReason::kAmbiguous:
      return "ambiguous";
    case SkipReason::kDuplicateSlot:
      return "duplicate slot";
    case SkipReason::kUnusable:
      return "unusable";
    case SkipReason::kDirectoryFailed:
      return "directory failed";
    case SkipReason::kTooManySaves:
      return "too many saves";
    case SkipReason::kUnhashed:
      return "unhashed";
    case SkipReason::kDuplicateName:
      return "duplicate name";
    case SkipReason::kTooManyStates:
      return "too many states";
    case SkipReason::kOutsideLayout:
      return "outside layout";
    case SkipReason::kTicoAccount:
      return "tico account";
    case SkipReason::kSaveFolder:
      return "save folder";
  }
  return "unknown";
}

std::string Skip::Describe() const {
  std::string out = ToString(reason);
  if (!sd_path.empty()) {
    out += " " + sd_path;
  }
  if (!message.empty()) {
    out += ": " + message;
  }
  return out;
}

std::string ScanResult::DescribeSkipped() const {
  if (skipped_total == 0 && unhashed_total == 0) {
    return {};
  }
  std::string out;
  for (const Skip& skip : skipped) {
    out += skip.Describe();
    out.push_back('\n');
  }
  if (skipped_total > skipped.size()) {
    out += "and " + std::to_string(skipped_total - skipped.size()) + " more skipped\n";
  }
  for (const Skip& save : unhashed) {
    out += save.Describe();
    out.push_back('\n');
  }
  if (unhashed_total > unhashed.size()) {
    out += "and " + std::to_string(unhashed_total - unhashed.size()) +
           " more reported with no digest\n";
  }
  return out;
}

std::string BaseName(std::string_view file_name) {
  // The split is `sync::ExtensionOf`'s, shared rather than restated: a dotfile
  // is a whole name, and a second spelling of that rule is a second answer to
  // "what is this save called".
  return std::string(file_name.substr(0, file_name.size() - sync::ExtensionOf(file_name).size()));
}

std::string SlotFor(std::string_view slot, std::string_view file_name) {
  // The extension, lowercased, with everything that is not a plain identifier
  // character dropped. FAT32 permits a good deal in a name, and the slot is a
  // key the server stores and this client has to reproduce byte for byte on
  // every later tick -- so it is built from a narrow alphabet rather than from
  // whatever the card happened to hold.
  const std::string_view suffix = sync::ExtensionOf(file_name);
  std::string extension;
  if (!suffix.empty()) {
    for (const char character : suffix.substr(1)) {
      const char lowered = LowerAscii(character);
      const bool keep = (lowered >= 'a' && lowered <= 'z') || (lowered >= '0' && lowered <= '9') ||
                        lowered == '-' || lowered == '_';
      if (keep) {
        extension.push_back(lowered);
      }
    }
  }
  if (extension.empty()) {
    // A save with no usable extension still needs a stable slot, and "save" is
    // stable. It cannot collide with an extension-derived one unless the file
    // really is called `x.save`, which pairs the two as the same slot -- the
    // duplicate is then reported rather than silently overwriting through the
    // server.
    extension = "save";
  }
  return SlotForExtension(slot, extension);
}

std::string MigratedSlot(std::string_view slot, std::string_view old_slot) {
  // Every slot a v2 build derived was `<source>-<extension>`, or the bare
  // extension for a folder neither source explained, and the extension was
  // already normalised -- so stripping the source is the whole inverse.
  for (const std::string_view source : {std::string_view("retroarch-"), std::string_view("tico-")}) {
    if (old_slot.size() > source.size() && old_slot.substr(0, source.size()) == source) {
      return SlotForExtension(slot, old_slot.substr(source.size()));
    }
  }
  return SlotForExtension(slot, old_slot);
}

std::optional<std::vector<std::string>> EmulatorFilter(const config::Config& config,
                                                       const std::vector<SaveFile>& saves) {
  std::set<std::string> ids;
  const auto add = [&ids](std::string_view id) {
    for (std::string& alias : RetroArchAliases(id)) {
      ids.insert(std::move(alias));
    }
  };
  for (const SaveFile& save : saves) {
    if (save.emulator.empty()) {
      return std::nullopt;
    }
    add(save.emulator);
  }
  for (const auto& [slug, mapped] : config.platforms) {
    // A platform played in one frontend is restored with it (`place::Placer`),
    // so the other's folder names no core with no save here yet: RomM's web
    // player writes `mupen64plus_next` for n64, and offered to a console that
    // plays n64 in Tico, its save is a first-time restore refused every tick
    // (#238). A save of that core on the card is still named, above.
    const std::optional<config::PlayWith> play_with = config.PlayWithFor(slug);
    for (const std::string& folder : mapped.saves) {
      const SaveSource source = SourceOf(folder);
      if (play_with.has_value() && source.source != Source::kUnknown &&
          (source.source == Source::kTico) != (*play_with == config::PlayWith::kTico)) {
        continue;
      }
      add(ResolveSource(source, slug, config).emulator_id);
    }
  }
  if (ids.empty()) {
    return std::nullopt;
  }
  return std::vector<std::string>(ids.begin(), ids.end());
}

namespace {

/// ` (Disc 1)`, ` (Disc 12)`: the tag a per-disc rom's name ends in, and which
/// DuckStation's one card per game leaves out. Its length, or 0 for none.
std::size_t DiscTagLength(std::string_view name) {
  constexpr std::string_view kOpen = " (Disc ";
  if (name.empty() || name.back() != ')') {
    return 0;
  }
  const std::size_t open = name.rfind(kOpen);
  if (open == std::string_view::npos) {
    return 0;
  }
  const std::size_t first = open + kOpen.size();
  const std::string_view digits = name.substr(first, name.size() - 1 - first);
  if (digits.empty()) {
    return 0;
  }
  for (const char digit : digits) {
    if (digit < '0' || digit > '9') {
      return 0;
    }
  }
  return name.size() - open;
}

}  // namespace

std::string MemoryCardName(std::string_view rom_name, int card) {
  std::string name(rom_name.substr(0, rom_name.size() - DiscTagLength(rom_name)));
  if (card > 1) {
    name += "_" + std::to_string(card);
  }
  return name + ".mcd";
}

std::map<std::string, std::string, std::less<>> PlatformHints(const config::Config& config,
                                                              Folders which) {
  std::map<std::string, std::string, std::less<>> hints;
  for (const auto& [slug, mapped] : config.platforms) {
    const std::vector<std::string>& directories =
        which == Folders::kStates ? mapped.states : mapped.saves;
    for (const std::string& directory : directories) {
      const auto found = hints.find(directory);
      if (found == hints.end()) {
        hints.emplace(directory, slug);
        continue;
      }
      if (found->second != slug) {
        // Two platforms, one folder: it implies neither. Emptied rather than
        // left at the first slug, because "first platform in the map wins" is
        // exactly the guess this module exists not to make.
        found->second.clear();
      }
    }
  }
  return hints;
}

sync::ClientSaveState SaveFile::ToClientSaveState(std::optional<std::string> content_hash) const {
  sync::ClientSaveState state;
  state.rom_id = rom_id;
  state.file_name = server_name.empty() ? file_name : server_name;
  if (!slot.empty()) {
    state.slot = slot;
  }
  if (!emulator.empty()) {
    state.emulator = emulator;
  }
  // Never an empty string: "" and `null` are different values to the server and
  // only one of them is a value, which is why `sync::Validate` refuses the
  // former outright.
  if (content_hash.has_value() && !content_hash->empty()) {
    state.content_hash = std::move(content_hash);
  } else if (!this->content_hash.empty()) {
    state.content_hash = this->content_hash;
  }
  // Fails closed: an mtime outside the window above becomes the epoch, which
  // `sync::Validate` refuses as "an unset clock rather than an mtime". The
  // scan never reaches here with one -- it skips them by name -- but this is
  // the conversion, so the guard against overflowing the clock belongs here
  // rather than in the one caller that happens to check first.
  state.updated_at = sync::Timestamp{};
  if (modified_unix >= sync::kMinTimestampSeconds && modified_unix <= kLatestUsableMtime) {
    state.updated_at += std::chrono::seconds(modified_unix);
  }
  state.file_size_bytes = size_bytes;
  return state;
}

namespace {

void RecordSkip(ScanResult* result, SkipReason reason, std::string sd_path,
                std::string message) {
  ++result->skipped_total;
  if (result->skipped.size() < kMaxSkipsReported) {
    result->skipped.push_back(MakeSkip(reason, std::move(sd_path), std::move(message)));
  }
}

/// One folder a walk reads, and what is known about it before a file in it is.
struct ScanFolder {
  std::string directory;

  /// What the folder says: `SourceOf`, or -- for a folder `retroarch.cfg` led
  /// the scan to -- RetroArch's, whatever the path is called.
  SaveSource source;

  /// The platform the folder implies, or "" for every platform (`PlatformHints`).
  std::string platform_hint;

  /// A rom folder RetroArch keeps saves beside: only its own extensions are read.
  bool beside_roms = false;

  /// Tico's psp folder: a subfolder is a PPSSPP save, said as `kSaveFolder` (#238).
  bool save_folders = false;

  /// Tico's psx folder: a `.mcd` is read as DuckStation names its cards (#238).
  bool memory_cards = false;
};

/// Tico's own folder for `tico_slug`, whatever its layout -- the module, not the
/// id sent, decides how its files are laid out, so an `emulator` override does
/// not change it.
bool TicoFolderFor(const SaveSource& source, std::string_view platform_hint,
                   std::string_view tico_slug) {
  return source.source == Source::kTico && TicoSlugFor(platform_hint) == tico_slug;
}

/// Say `entry` of `directory` is a PSP save when it is one -- a folder in Tico's
/// psp folder (`ppsspp`) other than the module's `backups/` or a dot-folder --
/// and answer whether it was (#238). PSP saves are not synced yet, and a skip
/// by name is what tells a user so.
bool SkipSaveFolder(ScanResult* result, bool ppsspp, std::string_view directory,
                    const fs::Entry& entry) {
  if (!ppsspp || !entry.is_directory || entry.name.empty() || entry.name.front() == '.' ||
      LowerAscii(entry.name) == "backups") {
    return false;
  }
  RecordSkip(result, SkipReason::kSaveFolder, Join(directory, entry.name),
             "PPSSPP keeps each game's save as a folder, and PSP saves are not synced yet "
             "(#238); the adapter to RomM's PSP-<folder>.zip comes after the saves are captured "
             "from a console (#44)");
  return true;
}

/// The rom DuckStation's card `name` belongs to, when its own name is no rom's
/// -- `first` is that lookup, unmatched or ambiguous: card 2 is `<name>_2.mcd`,
/// and one card serves every disc of a game, so it carries no disc tag (#226
/// R5). The disc-1 reading is tried past an ambiguity because a tag-stripped
/// `Game` is what `Game (Disc 1)` and `Game (Disc 2)` both reduce to. `first`
/// comes back when no reading finds a rom; an ambiguity of a reading is
/// returned as one, never guessed past.
roms::Match FindCard(const roms::RomIndex& index, std::string_view name,
                     std::string_view platform_hint, roms::Match first, int* card) {
  if (LowerAscii(sync::ExtensionOf(name)) != ".mcd") {
    return first;
  }
  const std::string base = BaseName(name);
  std::vector<std::pair<std::string, int>> readings;
  if (base.size() > 2 && base.compare(base.size() - 2, 2, "_2") == 0) {
    const std::string stem = base.substr(0, base.size() - 2);
    readings.emplace_back(stem, 2);
    readings.emplace_back(stem + " (Disc 1)", 2);
  }
  readings.emplace_back(base + " (Disc 1)", 1);
  for (const auto& [reading, number] : readings) {
    roms::Match match = index.Find(reading, platform_hint);
    if (match.outcome == roms::MatchOutcome::kUnmatched) {
      continue;
    }
    if (match.matched()) {
      *card = number;
      match.reason += " as DuckStation's card " + std::to_string(number);
    }
    return match;
  }
  return first;
}

/// RetroArch's own name for a save (`.srm`, and the `.rtc` beside it) or a state
/// (`.state`, `.state1`...) -- what a rom folder may hold that is not a rom.
bool IsRetroArchFile(std::string_view name, Folders which) {
  const std::string_view suffix = sync::ExtensionOf(name);
  const std::string extension = LowerAscii(suffix.empty() ? suffix : suffix.substr(1));
  if (which == Folders::kSaves) {
    return extension == "srm" || extension == "rtc";
  }
  // `.state`, `.stateN` and `.state.auto` -- the placer's rule too, so what is
  // placed beside the roms is what is read back there (#239).
  return IsRetroArchStateName(name);
}

/// A file an emulator keeps beside its saves and states that is never one: a
/// `.tmp` it writes before renaming (Tico, #226 R5) -- and one a crash left
/// behind must never be uploaded as the save it was about to replace -- a state's
/// `.png` screenshot (Tico and RetroArch both; `sync::SyncStates` sends it as the
/// state's, read beside the state, #239), and DuckStation's `.restart`. Not a skip: they are not saves
/// this client declined, any more than a rom in a rom folder is.
///
/// Tico's `backups/` needs no rule here: it is a folder, and the walk does not
/// descend into folders.
bool NeverSynced(std::string_view name) {
  const std::string_view suffix = sync::ExtensionOf(name);
  const std::string extension = LowerAscii(suffix.empty() ? suffix : suffix.substr(1));
  return extension == "tmp" || extension == "png" || extension == "restart";
}

SaveSource RetroArchFlat(const std::string& directory) {
  SaveSource source;
  source.source = Source::kRetroArch;
  source.folder = directory;
  source.layout = Layout::kFlat;
  return source;
}

/// A folder one level under a sorted RetroArch folder: a core RomM's table
/// knows, or RetroArch's with no guessed id -- `SourceOf`'s rule for a leaf it
/// does not know, applied to a folder that is RetroArch's by configuration.
SaveSource RetroArchCoreSource(const std::string& directory, std::string_view leaf) {
  SaveSource source;
  source.source = Source::kRetroArch;
  source.folder = directory;
  source.emulator_id = RetroArchCoreId(leaf);
  source.layout = source.emulator_id.empty() ? Layout::kUnknown : Layout::kCoreFolder;
  return source;
}

/// The folders one walk reads, in the order it reads them. See `ScanSaves`.
std::vector<ScanFolder> PlanFolders(const config::Config& config,
                                    const retroarch::Settings& settings, Folders which,
                                    fs::FileSystem& files, ScanResult* result) {
  const bool saves = which == Folders::kSaves;
  const std::map<std::string, std::string, std::less<>> hints = PlatformHints(config, which);
  const auto hint_for = [&hints](const std::string& directory) {
    const auto found = hints.find(directory);
    return found == hints.end() ? std::string() : found->second;
  };

  // Every mapped rom folder, which is one whatever led the scan to it: a
  // `savefile_directory` naming one, or a folder sorted by core above them,
  // still holds roms, and a rom download still stages its `.tmp` there.
  std::set<std::string> mapped_roms;
  for (const auto& [slug, mapped] : config.platforms) {
    for (const std::string& directory : mapped.roms) {
      mapped_roms.insert(LowerAscii(directory));
    }
  }
  const auto is_rom_folder = [&mapped_roms](const std::string& directory) {
    return mapped_roms.count(LowerAscii(directory)) > 0;
  };

  std::vector<ScanFolder> plan;
  // FAT32 ignores case, so `/RetroArch/saves` and `/retroarch/saves` are one
  // folder, and reading it twice makes every save in it a duplicate of itself.
  std::set<std::string> planned;
  const auto add = [&plan, &planned, &is_rom_folder](ScanFolder folder) {
    if (!planned.insert(LowerAscii(folder.directory)).second) {
      return false;
    }
    folder.beside_roms = folder.beside_roms || is_rom_folder(folder.directory);
    plan.push_back(std::move(folder));
    return true;
  };
  // RetroArch's core folders, bounded: the plan is held for the whole walk,
  // beside a listing, so it cannot grow with what a card happens to hold.
  std::size_t core_folders = 0;
  const auto add_core = [&](const std::string& path, std::string_view leaf,
                            const ScanFolder& base) {
    if (planned.count(LowerAscii(path)) > 0) {
      return;  // listed and named by the table both, or one folder in two cases
    }
    if (core_folders >= kMaxRetroArchFolders) {
      RecordSkip(result, SkipReason::kDirectoryFailed, path,
                 "past the " + std::to_string(kMaxRetroArchFolders) +
                     " RetroArch core folders one scan reads; its saves are not read");
      return;
    }
    if (add({path, RetroArchCoreSource(path, leaf), base.platform_hint, base.beside_roms})) {
      ++core_folders;
    }
  };

  const bool enabled = retroarch::Enabled(config, settings);
  const std::string configured =
      saves ? settings.savefile_directory : settings.savestate_directory;
  const std::string fallback =
      saves ? retroarch::kDefaultSavefileDirectory : retroarch::kDefaultSavestateDirectory;
  const bool sorted = saves ? settings.sort_savefiles : settings.sort_savestates;
  const bool by_content =
      saves ? settings.sort_savefiles_by_content : settings.sort_savestates_by_content;
  const bool beside_roms =
      saves ? settings.savefiles_in_content_dir : settings.savestates_in_content_dir;

  // The folder retroarch.cfg names is RetroArch's to lay out, so where the map
  // lists that same folder -- the built-in `/retroarch/saves`, when the file
  // names it, or one a user wrote -- it is decided here and never read flat
  // beside it.
  std::set<std::string> decided = {LowerAscii(configured)};

  if (enabled && !by_content) {
    std::vector<ScanFolder> bases;
    if (beside_roms) {
      // Sorting still applies beside the content: RetroArch takes the content's
      // folder as the save folder first and appends the core's name to it after
      // (`runloop_path_set_redirect`, v1.22.2), so with both on a save is in
      // `<rom folder>/<CoreFolder>/`.
      if (config.sources.retroarch_content_dir) {
        // The map's rom folders, each implying its platform when only one maps it.
        std::map<std::string, std::string, std::less<>> rom_hints;
        std::vector<std::string> rom_folders;
        for (const auto& [slug, mapped] : config.platforms) {
          for (const std::string& directory : mapped.roms) {
            const auto found = rom_hints.find(directory);
            if (found == rom_hints.end()) {
              rom_hints.emplace(directory, slug);
              rom_folders.push_back(directory);
            } else if (found->second != slug) {
              found->second.clear();
            }
          }
        }
        for (const std::string& directory : rom_folders) {
          bases.push_back({directory, RetroArchFlat(directory), rom_hints[directory], true});
        }
      }
    } else {
      std::string directory = configured;
      const auto not_a_folder = [&files](const std::string& path) {
        const fs::ListError error = files.List(path).error;
        return error == fs::ListError::kMissing || error == fs::ListError::kNotADirectory;
      };
      if (LowerAscii(configured) != LowerAscii(fallback) && not_a_folder(configured)) {
        // RetroArch ignores a `savefile_directory` that is not a directory
        // (`path_is_directory`, configuration.c) and writes to its default
        // instead, so that is where the saves are.
        directory = fallback;
        decided.insert(LowerAscii(fallback));
      }
      bases.push_back(
          {directory, RetroArchFlat(directory), hint_for(directory), is_rom_folder(directory)});
    }

    for (ScanFolder& base : bases) {
      if (!sorted) {
        add(std::move(base));
        continue;
      }
      fs::Listing listing = files.List(base.directory);
      if (!listing.ok() && listing.error != fs::ListError::kMissing) {
        RecordSkip(result, SkipReason::kDirectoryFailed, base.directory,
                   listing.message.empty() ? fs::ToString(listing.error) : listing.message);
      }
      std::sort(listing.entries.begin(), listing.entries.end(),
                [](const fs::Entry& left, const fs::Entry& right) {
                  return left.name < right.name;
                });
      // `fs::kMaxDirectoryEntries` keeps the first names, and a capital sorts
      // before a lowercase letter: four hundred stale `Game.srm`s left from
      // before sorting was on are enough to push `mGBA/` out of the listing. So
      // a cut listing of RetroArch's own folder also reads every core folder
      // RomM's table names, whether the listing reached it or not -- a missing
      // one costs an empty `List`, and a core RomM does not know past the cut is
      // the one thing lost, said by the skip above. A rom folder gets no such
      // help: hundreds of roms are its normal state, and every core in the
      // table under every rom folder is more than the bound below allows.
      //
      // Collected and sorted before any is planned, so the order -- which
      // decides a slot two core folders contest -- is by name whether or not
      // the listing was cut.
      std::vector<std::string> cores;
      if (listing.error == fs::ListError::kTooManyEntries && !base.beside_roms) {
        for (const RetroArchCore& core : RetroArchCores()) {
          cores.emplace_back(core.folder);
        }
      }
      for (const fs::Entry& entry : listing.entries) {
        if (entry.name.empty()) {
          continue;
        }
        const std::string path = Join(base.directory, entry.name);
        if (entry.is_directory) {
          // `.netplay` is where a netplay *client* keeps the host's saves, and
          // nothing that starts with a dot is a core. Beside the roms only a
          // core RomM's table knows is one: a folder there is far more often a
          // multi-disc game than a core nobody has heard of.
          const bool core = entry.name.front() != '.' &&
                            (!base.beside_roms || !RetroArchCoreId(entry.name).empty());
          if (core) {
            cores.push_back(entry.name);
          }
          continue;
        }
        if ((base.beside_roms && !IsRetroArchFile(entry.name, which)) ||
            NeverSynced(entry.name)) {
          continue;  // a rom, or not a save at all
        }
        ++result->files_seen;
        RecordSkip(result, SkipReason::kOutsideLayout, path,
                   std::string("RetroArch sorts ") + (saves ? "saves" : "states") +
                       " by core here and reads them from the core's folder, not this file");
      }
      std::sort(cores.begin(), cores.end());
      for (const std::string& leaf : cores) {
        add_core(Join(base.directory, leaf), leaf, base);  // a repeat is dropped by `add`
      }
    }
  }

  // Tico's layout is decided once per root (`/tico/saves`), not per folder:
  // eighteen platforms under one `users/` are one listing and one answer, and
  // two accounts with no choice made are one skip rather than eighteen.
  std::map<std::string, tico::Resolution> roots;
  for (const std::string& directory : saves ? config.SaveScanDirs() : config.StateScanDirs()) {
    SaveSource source = SourceOf(directory);
    if (decided.count(LowerAscii(directory)) > 0 ||
        (!enabled && source.source == Source::kRetroArch)) {
      continue;
    }
    const std::string root = tico::RootOf(directory);
    if (root.empty()) {
      add({directory, std::move(source), hint_for(directory), false});
      continue;
    }
    auto found = roots.find(LowerAscii(root));
    if (found == roots.end()) {
      found = roots.emplace(LowerAscii(root), tico::Resolve(config.tico, files, root)).first;
      if (found->second.layout == tico::Layout::kUndecided) {
        RecordSkip(result, SkipReason::kTicoAccount, found->second.users_dir, found->second.why);
      }
    }
    const tico::Resolution& resolved = found->second;
    if (resolved.layout == tico::Layout::kLegacy) {
      add({directory, std::move(source), hint_for(directory), false});
      continue;
    }
    if (resolved.layout == tico::Layout::kUndecided) {
      continue;  // said once, above
    }

    const std::string account_folder = tico::FolderIn(resolved, directory);
    SaveSource account;
    account.source = Source::kTico;
    account.folder = account_folder;
    account.layout = Layout::kAccountFolder;
    // The platform is the map folder's: `users/<uid>/snes` is snes because
    // `/tico/saves/snes` is.
    add({account_folder, std::move(account), hint_for(directory), false});

    // Left in the old folder, a save is one Tico 0.8 no longer reads -- a
    // module from before 0.8.0 still writing there, or a copy from before the
    // update. Synced, it would contest the account's slot with stale bytes.
    fs::Listing legacy = files.List(directory);
    std::sort(legacy.entries.begin(), legacy.entries.end(),
              [](const fs::Entry& left, const fs::Entry& right) { return left.name < right.name; });
    const bool ppsspp = TicoFolderFor(SourceOf(directory), hint_for(directory), "psp");
    for (const fs::Entry& entry : legacy.entries) {
      // A PSP save left here is not read in either layout; said as what it is.
      if (SkipSaveFolder(result, ppsspp, directory, entry) || entry.is_directory ||
          entry.name.empty() || NeverSynced(entry.name)) {
        continue;
      }
      ++result->files_seen;
      RecordSkip(result, SkipReason::kOutsideLayout, Join(directory, entry.name),
                 "Tico 0.8 keeps " + std::string(saves ? "saves" : "states") + " in " +
                     account_folder +
                     " now and does not read this one; a module installed before 0.8.0 "
                     "writes here until it is reinstalled");
    }
  }

  for (ScanFolder& folder : plan) {
    folder.save_folders = TicoFolderFor(folder.source, folder.platform_hint, "psp");
    folder.memory_cards = TicoFolderFor(folder.source, folder.platform_hint, "psx");
  }
  return plan;
}

/// What `Walk`'s `on_match` did with a file that survived the walk's checks.
enum class Emit {
  kSkipped,    // a skip it recorded itself: a duplicate, an unusable record
  kEmitted,    // a record
  kPastBound,  // a record it would have emitted, but the walk's bound is spent
};

/// The half of both walks that is the same, which is nearly all of it.
///
/// It reads each folder once, sorts its entries, matches the base name against
/// the rom index under the folder's platform hint and range-checks the mtime --
/// then hands what survived to `on_match` with the folder's `SaveSource`, which
/// says what it did with it (`Emit`). Every decision in here is one
/// docs/SYNC_PROTOCOL.md states about the scan rather than about saves in
/// particular, which is why a state gets the same one instead of a second copy
/// that could drift from it.
///
/// `limit` is how many records the caller may emit before the walk stops and
/// says so; the two kinds have different budgets against one shared baseline
/// file (`scan::kMaxSaves`, `scan::kMaxStates`). It counts records, not files:
/// the walk stops at the first record past it, which `on_match` -- told the
/// budget is spent -- decides only once its own duplicate and validity checks
/// have passed. A card holding exactly the bound's saves and a file that is no
/// save after them is a card read whole (#246).
template <class OnMatch>
void Walk(const std::vector<ScanFolder>& folders, Folders which, const roms::RomIndex& index,
          fs::FileSystem& files, std::size_t limit, SkipReason limit_reason,
          const std::string& limit_message, ScanResult* result, const OnMatch& on_match) {
  const auto record_skip = [result](SkipReason reason, std::string sd_path, std::string message) {
    RecordSkip(result, reason, std::move(sd_path), std::move(message));
  };

  std::size_t emitted = 0;
  for (const ScanFolder& folder : folders) {
    const std::string& directory = folder.directory;
    if (!folder.beside_roms) {
      // A rom folder is not one a save sweep may touch: a rom download stages
      // its `.tmp` there, and the sweep removes every `.tmp` it finds.
      result->folders.push_back(directory);
    }
    fs::Listing listing = files.List(directory);
    if (!listing.ok() && listing.error != fs::ListError::kMissing) {
      // A *missing* folder is a normal card -- a platform mapped to a Tico
      // directory the user never created -- so it is silent. Anything else is
      // a folder that is there and whose saves are therefore absent from this
      // tick without anyone having decided that, which has to be said out loud.
      record_skip(SkipReason::kDirectoryFailed, directory,
                  listing.message.empty() ? fs::ToString(listing.error) : listing.message);
      if (listing.entries.empty()) {
        continue;
      }
      // `kTooManyEntries` reports what it did read. Scanning it is better than
      // dropping it: the files that were read are still saves.
    }

    // `readdir` promises no order, and a scan whose duplicate resolution depends
    // on the card's directory layout resolves it differently after an unrelated
    // file is deleted.
    std::sort(listing.entries.begin(), listing.entries.end(),
              [](const fs::Entry& left, const fs::Entry& right) { return left.name < right.name; });

    const std::string_view platform_hint = folder.platform_hint;

    for (const fs::Entry& entry : listing.entries) {
      if (SkipSaveFolder(result, folder.save_folders, directory, entry) || entry.is_directory ||
          entry.name.empty()) {
        continue;  // the walk is not recursive; see file_system.hpp
      }
      if (folder.beside_roms && !IsRetroArchFile(entry.name, which)) {
        continue;  // a rom folder's roms are not files this scan has seen
      }
      if (NeverSynced(entry.name)) {
        continue;
      }
      ++result->files_seen;
      const std::string sd_path = Join(directory, entry.name);

      int card = 1;
      // RetroArch's Auto state, `Game.state.auto`, is matched by `Game` (#239).
      const roms::Match match =
          MatchSave(index, which == Folders::kStates ? StateMatchName(entry.name) : entry.name,
                    platform_hint, folder.memory_cards, &card);
      if (match.outcome == roms::MatchOutcome::kAmbiguous) {
        record_skip(SkipReason::kAmbiguous, sd_path, match.reason);
        continue;
      }
      if (!match.matched()) {
        // With a short index "no rom named X" may be a rom that was never read,
        // and reporting it flatly would send a user looking for a library
        // problem that is really this client's bound.
        record_skip(SkipReason::kUnmatched, sd_path,
                    result->index_truncated
                        ? match.reason + "; the rom index is incomplete, so this may be a rom "
                                         "that was never read rather than one you do not have"
                        : match.reason);
        continue;
      }

      // Range-checked before the record is built rather than left to
      // `sync::Validate`, because the conversion into a `sync::Timestamp` is
      // what overflows and it happens first. A card that reports 2600 is a
      // clock nobody set or a directory entry nobody wrote; either way it is
      // one file's problem, said by name.
      if (entry.modified_unix < sync::kMinTimestampSeconds ||
          entry.modified_unix > kLatestUsableMtime) {
        record_skip(SkipReason::kUnusable, sd_path,
                    "its mtime of " + std::to_string(entry.modified_unix) +
                        " is not an instant this client can report; the card's clock is unset "
                        "or the directory entry is wrong");
        continue;
      }

      const Emit emit = on_match(sd_path, entry, *match.rom, folder.source, card,
                                 emitted >= limit, record_skip);
      if (emit == Emit::kEmitted) {
        ++emitted;
      } else if (emit == Emit::kPastBound) {
        record_skip(limit_reason, sd_path, limit_message);
        result->stopped_at_bound = true;
        return;
      }
    }
  }
}

}  // namespace

roms::Match MatchSave(const roms::RomIndex& index, std::string_view file_name,
                      std::string_view platform_hint, bool memory_cards, int* card) {
  *card = 1;
  roms::Match match = index.Find(BaseName(file_name), platform_hint);
  if (memory_cards && !match.matched()) {
    match = FindCard(index, file_name, platform_hint, std::move(match), card);
  }
  return match;
}

ScanResult ScanSaves(const config::Config& config, const retroarch::Settings& retroarch,
                     const roms::RomIndex& index, fs::FileSystem& files,
                     const state::Baseline& baseline) {
  ScanResult result;
  result.index_truncated = index.truncated();

  // `(rom_id, slot)` is what the server pairs on, so two local files landing on
  // the same pair would overwrite each other *through RomM*, on alternating
  // ticks, forever. The first one in scan order keeps it and the second is
  // reported -- which is only deterministic because the directory order is the
  // config's and each directory's entries are sorted by `Walk`.
  std::set<std::pair<std::int64_t, std::string>> claimed;

  const std::vector<ScanFolder> folders =
      PlanFolders(config, retroarch, Folders::kSaves, files, &result);
  Walk(folders, Folders::kSaves, index, files, kMaxSaves,
       SkipReason::kTooManySaves,
       "the scan stopped at " + std::to_string(kMaxSaves) +
           " saves; the rest of this card was not read",
       &result,
       [&](const std::string& sd_path, const fs::Entry& entry, const roms::Rom& rom,
           const SaveSource& folder_source, int card, bool past_bound,
           const auto& record_skip) {
         // Per rom rather than per folder: RetroArch's flat folder holds every
         // system, and the id is the platform's.
         const SaveSource source = ResolveSource(folder_source, rom.platform_fs_slug, config);
         SaveFile save;
         save.rom_id = rom.id;
         save.sd_path = sd_path;
         save.file_name = entry.name;
         save.slot = SlotFor(config.sync.slot, entry.name);
         // The rom's stem, which is what RomM serves RetroArch and matches its
         // uploads by (#236). The card's own name only if the stem could not be
         // a file name, so a save that synced before still does.
         save.server_name = rom.fs_name_no_ext + std::string(sync::ExtensionOf(entry.name));
         if (card > 1) {
           // DuckStation's second card is a second save of the rom (#238), in a
           // slot of its own and under the name DuckStation gives it.
           save.slot += "-card" + std::to_string(card);
           save.server_name = MemoryCardName(rom.fs_name_no_ext, card);
         }
         if (rom.fs_name_no_ext.empty() || !sync::IsSingleFileName(save.server_name)) {
           save.server_name = entry.name;
         }
         save.emulator = source.emulator_id;
         save.platform_fs_slug = rom.platform_fs_slug;
         save.size_bytes = entry.size_bytes;
         save.modified_unix = entry.modified_unix;

         // Checked here rather than left to the encoder: a save that cannot be
         // expressed faithfully must cost one file, not the whole request body
         // -- `EncodeNegotiateRequest` stops at the first bad entry, so one
         // unreadable mtime would otherwise take the tick's every other save
         // with it. Built once and reused below: it copies three strings, and
         // the scan needs it for the validation, the slot and the mtime alike.
         const sync::ClientSaveState negotiated = save.ToClientSaveState();
         if (const json::Error error = sync::Validate(negotiated); !error.ok()) {
           record_skip(SkipReason::kUnusable, sd_path, error.Describe());
           return Emit::kSkipped;
         }

         if (!claimed.emplace(save.rom_id, save.slot).second) {
           record_skip(SkipReason::kDuplicateSlot, sd_path,
                       "rom " + std::to_string(save.rom_id) + " already has a save in slot \"" +
                           save.slot + "\" this tick; only the first is synced");
           return Emit::kSkipped;
         }
         if (past_bound) {
           return Emit::kPastBound;  // a save, and the first one the walk has no room for
         }

         // Hashed last, after the record is known to be one that will be
         // reported: `ContentHashFor` reads the file when the baseline cannot
         // answer, and reading a save that is about to be skipped is the one
         // cost of this walk that is measured in card I/O rather than in string
         // compares.
         //
         // The path is resolved by the backend, not built here. `sd_path` is
         // SD-root absolute and means nothing to `fopen` on either platform.
         const std::string real_path = files.Resolve(save.sd_path);
         const state::HashOutcome hashed =
             real_path.empty()
                 ? state::HashOutcome{{}, state::HashError::kUnreadable, false,
                                      save.sd_path + ": not a path on this card"}
                 : state::ContentHashFor(baseline, save.rom_id, negotiated.slot, real_path,
                                         negotiated.updated_at, save.size_bytes);
         if (hashed.ok()) {
           save.content_hash = hashed.content_hash;
         } else {
           RecordUnhashed(&result, save.sd_path, hashed);
         }

         result.saves.push_back(std::move(save));
         return Emit::kEmitted;
       });
  return result;
}

ScanResult ScanStates(const config::Config& config, const retroarch::Settings& retroarch,
                      const roms::RomIndex& index, fs::FileSystem& files,
                      const state::Baseline& baseline) {
  ScanResult result;
  result.index_truncated = index.truncated();

  // `(rom_id, file_name)` and deliberately not `(rom_id, emulator, file_name)`:
  // `POST /api/states` upserts on the name alone and *moves* the emulator of the
  // row it finds, so two local states of one rom sharing a name are one server
  // row however different their directories are. Claiming the pair here is what
  // stops the RetroArch copy and the Tico copy taking turns destroying each
  // other through RomM (`SkipReason::kDuplicateName`).
  //
  // `state::Baseline::StateKey` rather than a second spelling of the pair: the
  // baseline is keyed on exactly this, and two spellings of a key are two places
  // to disagree about what pairs with what.
  std::set<state::Baseline::StateKey> claimed;

  const std::vector<ScanFolder> folders =
      PlanFolders(config, retroarch, Folders::kStates, files, &result);
  Walk(folders, Folders::kStates, index, files, kMaxStates,
       SkipReason::kTooManyStates,
       "the scan stopped at " + std::to_string(kMaxStates) +
           " states; the rest of this card was not read",
       &result,
       [&](const std::string& sd_path, const fs::Entry& entry, const roms::Rom& rom,
           const SaveSource& folder_source, int /*card*/, bool past_bound,
           const auto& record_skip) {
         StateFile file;
         file.rom_id = rom.id;
         file.sd_path = sd_path;
         file.file_name = entry.name;
         file.emulator = ResolveSource(folder_source, rom.platform_fs_slug, config).emulator_id;
         // A libretro module's Tico slot, under RetroArch's name for it when the
         // user asked (#239). The key, the upload's name and the baseline's all
         // follow; the file on the card keeps its own.
         if (folder_source.source == Source::kTico && MapsStateSlots(config, file.emulator)) {
           if (std::optional<std::string> mapped = RetroArchStateName(entry.name)) {
             file.file_name = std::move(*mapped);
           }
         }
         file.platform_fs_slug = rom.platform_fs_slug;
         file.size_bytes = entry.size_bytes;
         file.modified_unix = entry.modified_unix;

         // The name is the key on both sides, so it is held to the bar a key has
         // to meet before anything joins it into a URL or a backup path. There
         // is no `sync::Validate` for a state -- states never appear in a
         // negotiate payload -- so this is the whole of it.
         if (!sync::IsSingleFileName(file.file_name)) {
           record_skip(SkipReason::kUnusable, sd_path,
                       "its name is not one this client can use as a pairing key");
           return Emit::kSkipped;
         }

         if (!claimed.emplace(file.rom_id, file.file_name).second) {
           record_skip(SkipReason::kDuplicateName, sd_path,
                       "rom " + std::to_string(file.rom_id) + " already has a state called \"" +
                           file.file_name +
                           "\" this tick; RomM stores one state per name, so only the first is "
                           "synced");
           return Emit::kSkipped;
         }
         if (past_bound) {
           return Emit::kPastBound;  // a state, and the first one the walk has no room for
         }

         const std::string real_path = files.Resolve(file.sd_path);
         const state::HashOutcome hashed =
             real_path.empty()
                 ? state::HashOutcome{{}, state::HashError::kUnreadable, false,
                                      file.sd_path + ": not a path on this card"}
                 : state::ContentHashForState(
                       baseline, file.rom_id, file.file_name, real_path,
                       sync::Timestamp{} + std::chrono::seconds(file.modified_unix),
                       file.size_bytes);
         if (hashed.ok()) {
           file.content_hash = hashed.content_hash;
         } else {
           RecordUnhashed(&result, file.sd_path, hashed);
         }

         result.states.push_back(std::move(file));
         return Emit::kEmitted;
       });
  return result;
}

}  // namespace rommsync::scan
