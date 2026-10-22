// Which emulator wrote a save or a state, and what RomM should be told it was.
//
// RomM pairs saves and prunes their lineages by the `emulator` a client sends
// (#226 R1): a lineage is `(emulator, lower(extension))`, the web player and
// RetroArch's own Cloud Sync look a save up by its core id, and `emulators` on
// negotiate filters by it. So the id is not a label. Sent as `tico` or
// `retroarch` -- what this client did before M10 -- every Switch save sits in a
// lineage no other device shares. Sent as the wrong core, a save is offered to a
// core whose bytes it is not.
//
// The answer comes from three places, in this order:
//
//   1. **The folder, when it names a core.** RetroArch's sort-by-core layout
//      keeps `saves/<CoreFolder>/`, and a folder RomM's table knows is ground
//      truth: those bytes were written by that core, whatever else is configured.
//   2. **`[platform.<slug>] emulator` in config.ini**, for everything a folder
//      does not say -- chiefly which Tico module runs a system the user has
//      changed, such as mGBA for GB. Tico keeps that choice in
//      `sdmc:/tico/config/` in a form nobody has read off a console yet [#44];
//      until then the override is the only way to say it.
//   3. **The built-in tables**: the libretro core id where the bytes are
//      RetroArch-compatible, and a `tico-*` id where they are not (#226 D3), so
//      an incompatible format is isolated rather than offered to a core that
//      would misread it.
//
// None of it decides the *slot*: since #236 that is the configured shared one
// (`scan::SlotFor`), whoever wrote the save. The id is what keeps two lineages
// in that slot apart -- RomM prunes per `(emulator, extension)`, and negotiate's
// `emulators` filter (`scan::EmulatorFilter`) offers only the ones this console
// can load.
//
// The RetroArch table mirrors RomM 5.4.0's
// `backend/handler/sync/retroarch/emulator_names.py`, pinned verbatim at
// `server/contract/romm-5.4.0-emulator_names.py`; `source.retroarch_table` diffs
// the two entry for entry, so a RomM that adds a core is a red test rather than
// a silent mismatch.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/config.hpp"

namespace rommsync::scan {

/// The emulator a folder belongs to, as far as its path says.
enum class Source {
  kUnknown,    ///< a folder neither emulator's layout explains
  kRetroArch,  ///< a path segment named `retroarch`
  kTico,       ///< a path segment named `tico`
};

/// `retroarch`, `tico`, or "" for `kUnknown`. Never null. A name for logs and
/// tests -- never an `emulator`, and since #236 no part of a slot (a version-2
/// `state.db` row's slot still carries it; `scan::MigratedSlot` strips it).
const char* ToString(Source source);

/// How the source lays its saves out, which decides what a folder can say.
enum class Layout {
  kUnknown,        ///< nothing is assumed: no built-in id, only an override
  kFlat,           ///< RetroArch's one `saves/` (or `states/`) for every system
  kCoreFolder,     ///< RetroArch's sort-by-core `saves/<CoreFolder>/`
  kPlatformFolder, ///< Tico's `saves/<slug>/`, the layout before 0.8.0
  kAccountFolder,  ///< Tico 0.8's `saves/users/<uid>/<slug>/` (`tico::Resolve`)
};

/// Where a save came from, and the emulator id that follows from it.
struct SaveSource {
  Source source = Source::kUnknown;

  /// The rom's RomM `platform_fs_slug`. Empty from `SourceOf`, which sees a
  /// folder and not a rom -- RetroArch's flat folder holds every system.
  std::string platform_fs_slug;

  /// RomM's `emulator`: a libretro core id, a `tico-*` id, or an override.
  /// Empty when nothing says, which is sent as `null` and never as "".
  std::string emulator_id;

  /// The folder, SD-root absolute, as the folder map lists it.
  std::string folder;

  Layout layout = Layout::kUnknown;
};

/// What `folder` alone says: its source, its layout and -- for a RetroArch core
/// folder RomM's table knows -- the core.
///
/// A segment named `retroarch` or `tico` anywhere in the path names the source,
/// so `/emulators/retroarch/saves` is recognised as well as the default; the
/// comparison ignores case because FAT32 does. A RetroArch folder is flat when
/// its last segment is `saves`, `states` or `retroarch`, and a core folder when
/// the last segment is a name RomM's table knows. Any other leaf -- say
/// `saves/YabaSanshiro` -- is `Layout::kUnknown`: it could as well be a folder
/// somebody made, and a guessed core there is a lineage nobody else is in.
SaveSource SourceOf(std::string_view folder);

/// `folder`, completed for one rom's platform under `config`: the folder's own
/// core if it named one, else the platform's override, else -- for a folder
/// whose layout is known -- the built-in id for that source and platform, else
/// empty.
SaveSource ResolveSource(const SaveSource& folder, std::string_view platform_fs_slug,
                         const config::Config& config);

/// The built-in id for a save from `source` on `platform_fs_slug` when the
/// folder does not name a core; empty when there is none.
///
/// Tico's is its module's (`TicoModuleFor`). RetroArch's is the core a flat
/// `saves/` on that system is most likely written by: the same libretro core as
/// Tico's module where there is one, so the two meet in one lineage, and
/// otherwise the core RomM's web player uses for that system [#44 verifies the
/// Switch build's defaults]. A `Source::kUnknown` folder has none.
std::string DefaultEmulatorId(Source source, std::string_view platform_fs_slug);

// --- isolated formats (#238) --------------------------------------------------

/// True for an id whose save format only that emulator reads: a `tico-*` id
/// (#226 D3). DuckStation's `.mcd`, Mupen64Plus's split N64 files, PPSSPP's
/// folders, and Flycast's, DraStic's and YabaSanshiro's files are not what any
/// libretro core reads, so a save under one of these ids syncs Switch to Switch
/// and nowhere else until an adapter is proven against #44's captures.
bool IsIsolated(std::string_view emulator_id);

/// True when bytes RomM holds under emulator `server` may be written where this
/// client's scan reads a save as `local`. Either may be "", which is `null`:
/// nobody said.
///
/// An isolated format meets only itself: a `tico-*` save is never written where
/// anything else reads, and nothing else -- an unnamed save included -- is ever
/// written over one. Everything else is allowed, as it was before #238: two
/// libretro cores sharing a slot is RomM's pairing and the `emulators` filter's
/// to narrow (#236), not this rule's.
bool Interchangeable(std::string_view local, std::string_view server);

/// The emulator id the scan would report for a save at `sd_path` of a rom on
/// `platform_fs_slug`: `ResolveSource` over `SourceOf` of the file's folder.
/// Empty when nothing says, or when `sd_path` has no folder.
///
/// What a save placed at `sd_path` will be read back as on the next tick, and so
/// the lineage a download landing there joins -- the question `Interchangeable`
/// is asked about a first-time restore (`sync::ExecuteOptions::reads_as`), for a
/// placer that does not name its emulator. From the path alone, so a folder the
/// scan reads by configuration rather than by name -- RetroArch's content dir
/// under `/tico/roms` -- reads as its path says; `place::Placer` names its own.
std::string EmulatorAt(std::string_view sd_path, std::string_view platform_fs_slug,
                       const config::Config& config);

// --- RomM's rule for an id ----------------------------------------------------

/// RomM's `EMULATOR_MAX_LENGTH`, in characters.
inline constexpr std::size_t kMaxEmulatorIdLength = 50;

/// True when RomM 5.4 will take `id` as an `emulator` unchanged.
///
/// RomM refuses (400) an emulator that `sanitize_filename` would change and one
/// past 50 characters (422), because it is a directory in the save's stored path.
/// So: not empty, at most 50 characters (code points, not bytes), none of
/// `\ / : | * ? " < >`, no control character, no leading or trailing space, and
/// not `.` or `..`. Sets `why` to a user-facing reason on refusal; it quotes
/// nothing, since the caller already knows the value it asked about.
bool ValidEmulatorId(std::string_view id, std::string* why);

// --- RetroArch: RomM's core-folder table ----------------------------------------

/// One `RETROARCH_DIR_BY_ROMM_EMULATOR` entry.
struct RetroArchCore {
  const char* id;      ///< RomM's `emulator`, `snes9x`
  const char* folder;  ///< RetroArch's save folder for that core, `Snes9x`
};

/// The whole table, in RomM's order -- which matters: a folder two ids share
/// answers with the first one listed.
const std::vector<RetroArchCore>& RetroArchCores();

/// `to_romm_emulator`, for a folder RomM knows: `Beetle PSX HW` -> `mednafen_psx_hw`.
/// Empty for a folder it does not, where RomM would store the name verbatim --
/// see `SourceOf` for why this client does not. Case-insensitive.
std::string RetroArchCoreId(std::string_view folder);

/// `to_retroarch_dir_name`: `mednafen_psx` -> `Beetle PSX`; an unknown id unchanged.
std::string RetroArchCoreFolder(std::string_view id);

/// `retroarch_aliases`: every `emulator` that syncs through the same RetroArch
/// folder as `id`, `id` and the folder name included, sorted. Empty for "".
std::vector<std::string> RetroArchAliases(std::string_view id);

/// True for an id RomM's core table names: a libretro core, whose saves are
/// RetroArch's SRAM format whichever frontend ran it -- Tico's libretro modules
/// included.
bool IsRetroArchCore(std::string_view id);

/// True when bytes `written_by` wrote are ones `loaded_by` loads as its own:
/// the same id, or two ids RomM files under one RetroArch core folder. False
/// when either is empty. The core-identity half of `place::Compatible` and
/// `place::LoadsState`.
bool SameCore(std::string_view written_by, std::string_view loaded_by);

// --- save formats (#238) ----------------------------------------------------------

/// How an emulator names its saves, as far as this build knows. All empty is
/// an emulator whose names nobody has read off a console yet [#44].
struct SaveFormat {
  /// The extension a save is written as when the server's name gives no better
  /// answer; "" when only the server's name can say which of `also` it is.
  const char* primary = "";

  /// The others it writes beside it, each a save of its own; unused ones null.
  std::array<const char*, 4> also = {};

  /// Each save is a folder (PPSSPP): never placed, and a loose file where the
  /// folders are kept is no save of its.
  bool folders = false;

  /// One memory card per game, named by `MemoryCardName` (DuckStation).
  bool memory_cards = false;

  /// `extension` -- `.srm`, lowercased -- is one this emulator writes.
  bool Writes(std::string_view extension) const;

  /// Anything at all is known about its names.
  bool Known() const { return *primary != '\0' || also[0] != nullptr || folders; }
};

/// A libretro core's: RetroArch's `.srm`, and the `.rtc` some keep beside it.
const SaveFormat& LibretroFormat();

/// The format a save under `emulator_id` is in: `LibretroFormat` for a core
/// RomM's table names, a Tico module's own for its isolated `tico-*` id, and
/// the all-empty unknown one for anything else.
SaveFormat SaveFormatOf(std::string_view emulator_id);

// --- Tico: one module per system ------------------------------------------------

/// The module Tico runs for one of its systems. Adding one is a row of
/// `kTicoModules` and nothing else: the scan and the placer read its format
/// from here.
struct TicoModule {
  const char* slug;         ///< Tico's folder name, `master-system`
  const char* module;       ///< the module, `Genesis Plus GX`
  const char* emulator_id;  ///< `genesis_plus_gx`, or `tico-*` for an isolated format

  /// For an isolated module, how it names its saves. A libretro module's is
  /// `LibretroFormat`, whatever this says.
  SaveFormat format = {};
};

/// The module for Tico slug `tico_slug`, or nullptr when Tico runs no such
/// system. Takes Tico's spelling; `TicoSlugFor` turns RomM's into it.
const TicoModule* TicoModuleFor(std::string_view tico_slug);

/// RomM's `platform_fs_slug` in Tico's spelling, where the two differ (#226 R5):
/// `dreamcast`->`dc`, `sms`->`master-system`, `gamegear`->`game-gear`,
/// `segacd`->`sega-cd`, `ngc`->`gc`, `arcade`->`fbneo`. Anything else unchanged.
///
/// It is also the name of Tico's folder for the system -- `/tico/roms/<it>`,
/// `/tico/saves/<it>` -- which is how the built-in folder map spells them
/// (`config::DefaultPlatforms`, #231).
std::string TicoSlugFor(std::string_view romm_slug);

}  // namespace rommsync::scan
