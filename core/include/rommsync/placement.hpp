// Where a save or a state the console has never held is written: a first-time
// restore (#237).
//
// Everything else the engine writes goes where the scan found it. This is the
// one write with nothing on the card to go by, and the plan cannot say either:
// the server's `file_name` carries RomM's datetime tag
// (`Super Mario World (USA) [2026-10-01_12-00-00].srm`) and the folder depends
// on which emulator the user plays the system in. So the answer is built here,
// from three things:
//
//   - **the emulator** -- `[platform.<slug>] play_with`, Tico by default (#226
//     D5). Not the first folder of the map: that is `/retroarch/saves` for
//     every platform, a folder neither Tico nor a default RetroArch reads.
//   - **its folder, as the card is laid out this tick** -- Tico's through
//     `tico::Resolve` (`users/<uid>/<slug>`, or the folder before 0.8.0, and
//     never one of several accounts nobody chose); RetroArch's through
//     `retroarch.cfg` (`savefile_directory`, `<CoreFolder>/` when it sorts by
//     core, the rom's folder when it keeps saves beside the content).
//   - **the name the emulator loads** -- the rom's `fs_name_no_ext` and the
//     extension the emulator writes, never the server's name.
//
// **A placement must be read back as what it is.** A file this client writes
// and the next scan then reads as a different rom, or in a different slot, is
// a second save of that game that the next tick uploads -- and the row it came
// from is offered again, every tick. So `Placer` places only where the scan
// will find the file, match it to the same rom and give it the same slot; and
// only a save whose bytes the target emulator reads: an isolated `tico-*`
// format never goes to RetroArch, nor a RetroArch core's to a Tico module that
// is not that core (#226 D3). Anything else is placed nowhere, with a sentence
// saying why -- which is what `sync::ExecuteOptions::place` and
// `sync::StateSyncOptions::place` turn into a named failure.
//
// Placing **creates**. Whether something is already at the path is the
// executor's question, asked against the card at the moment of the write
// (`sync::ExecutePlan`, `sync::SyncStates`): a file there is one the scan did
// not pair with this row, and it is not this module's to replace.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "rommsync/config.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/retroarch_cfg.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/tico.hpp"

namespace rommsync::place {

/// Where a placement goes, or why it goes nowhere. Exactly one is set.
struct Placement {
  /// SD-root absolute: `/tico/saves/users/<uid>/snes/Super Mario World (USA).srm`.
  std::string sd_path;

  /// Why there is no path, in a sentence for the log and the overlay. Never
  /// quotes the server's file name or slot -- whatever another client chose --
  /// and the rom is the caller's to name. An emulator id RomM accepted may be
  /// named.
  std::string reason;

  /// The emulator the file at `sd_path` is written for, and so what the next
  /// scan reads it back as: the lineage the executor holds the server's copy to
  /// (#238, `scan::Interchangeable`). Empty when the placer does not say, and
  /// `sync::ExecuteOptions::reads_as` is asked instead.
  std::string emulator;

  /// No path because the file is not this console's to load (#239): a state
  /// written by a core the platform is not played in here, or in a slot the
  /// frontend it is played in has no file for. `reason` still says which. A
  /// caller that filters the server's rows by emulator passes over it rather
  /// than failing it -- it is the normal shape of a library several devices
  /// share, not something a user can fix.
  bool foreign = false;

  bool ok() const { return !sd_path.empty(); }
};

/// The folder one emulator keeps a platform's saves (or states) in this tick.
struct Layout {
  config::PlayWith play_with = config::PlayWith::kTico;

  /// SD-root absolute. Tico: `tico::FolderIn`'s answer. RetroArch: its save or
  /// state directory, or the rom's folder when it keeps them beside the content.
  std::string folder;

  /// RetroArch sorts by core: the file goes in `<folder>/<CoreFolder>/`.
  bool by_core = false;
};

/// The path a save written by `emulator_id` for `rom` takes under `layout`:
/// `<folder>[/<CoreFolder>]/<fs_name_no_ext><ext>`. Pure.
///
/// The extension is the emulator's (`.srm` for a libretro core, `.mcd` for
/// DuckStation), not the server's: RomM's name is whatever the uploading client
/// called it. `server_file_name` only chooses between extensions the emulator
/// itself writes -- Snes9x's `.rtc` beside its `.srm`, Mupen64Plus's four -- and,
/// for an isolated module whose names this build does not know [#44], supplies
/// the one that same module uploaded it under. A `.sav` is SRAM under another
/// client's name and is written as `.srm`; any other extension a known emulator
/// does not write is refused rather than renamed, since renaming does not make
/// the bytes the format.
///
/// Refused, too: a rom name that is empty, `.`/`..` or holds a separator -- it
/// comes off the server, and this is the call that joins it into a path -- a
/// RetroArch core folder for an id RomM's table does not know, and a path past
/// `config::kMaxPathLength`.
Placement Place(const roms::Rom& rom, std::string_view emulator_id,
                std::string_view server_file_name, const Layout& layout);

/// True when a save `server_emulator` wrote is bytes `target_emulator` reads:
/// the same id, or two ids RomM files under one RetroArch core folder
/// (`scan::RetroArchAliases`). A save with no emulator is taken as libretro SRAM
/// -- RetroArch's flat folder uploads with none -- so it goes to a libretro core
/// and never to an isolated `tico-*` format. Pure.
bool Compatible(std::string_view server_emulator, std::string_view target_emulator);

/// True when a *state* `server_emulator` wrote is one `target_emulator` loads:
/// the same id, or two ids RomM files under one RetroArch core folder. Stricter
/// than `Compatible`, because a state is a core's memory rather than the game's
/// SRAM: two cores' states never load in each other, and a state that does not
/// say which core wrote it loads in none this client can name (#239). Pure.
bool LoadsState(std::string_view server_emulator, std::string_view target_emulator);

/// The placements of one tick, against one card.
///
/// Holds references: everything it is built from must outlive it. Reads the
/// card only when asked to place something -- `tico::Resolve` once per Tico
/// root, `retroarch.cfg`'s folder once -- so a tick with nothing to place costs
/// nothing.
class Placer {
 public:
  Placer(const config::Config& config, const retroarch::Settings& retroarch,
         const roms::RomIndex& index, fs::FileSystem& files);

  /// Where the save `operation` names goes, for an operation no local save
  /// matched. Refuses an archival (`null`-slot) save: it pairs with nothing, so
  /// a placed copy is a new save on the next tick.
  Placement Save(const sync::SyncOperation& operation);

  /// Where a server state no local state claimed goes, in the folder the save
  /// rule picks. Under the server's own `file_name` -- a state pairs on
  /// `(rom_id, file_name)`, so a renamed one is a different state -- except in
  /// Tico's folder under `[sync] map_state_slots`, where RetroArch's name is
  /// Tico's for the same slot and the scan maps it back (#239,
  /// `scan::TicoStateName`).
  ///
  /// Only a state the target core loads (`LoadsState`), and only under a name
  /// that frontend loads one from (`scan::IsTicoStateName`,
  /// `scan::IsRetroArchStateName`); anything else is `foreign`.
  Placement State(std::int64_t rom_id, std::string_view file_name, std::string_view emulator);

 private:
  struct Target {
    Layout layout;
    std::string map_folder;     ///< the map's folder the layout came from, for its platform hint
    std::string emulator_id;    ///< the emulator the file is written for
    std::string reason;         ///< set when there is no target
    bool foreign = false;       ///< ...because the server's copy is not that emulator's
  };

  Target TargetFor(const roms::Rom& rom, std::string_view server_emulator, bool saves);

  /// The rom the scan would match `file_name` in `target`'s folder to, or why
  /// it would match none. Empty reason: `rom` is the match, as DuckStation's
  /// card `card` where the folder is Tico's psx (#238).
  std::string ReadsBackAs(const roms::Rom& rom, std::string_view file_name, const Target& target,
                          bool saves, int card = 1) const;

  const config::Config& config_;
  const retroarch::Settings& retroarch_;
  const roms::RomIndex& index_;
  fs::FileSystem& files_;
  std::map<std::string, tico::Resolution> roots_;  ///< by lowercased root
};

}  // namespace rommsync::place
