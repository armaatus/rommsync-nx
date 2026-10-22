// The two names one save state goes by: Tico's and RetroArch's (#239).
//
// A state is a core's memory, so it loads only in the core that wrote it
// (`place::LoadsState`). Where Tico runs a libretro module, that core is the
// one RetroArch runs -- Snes9x, Gambatte, mGBA -- and the bytes are the same.
// The names are not:
//
//   | Slot              | Tico file      | RetroArch file |
//   |-------------------|----------------|----------------|
//   | menu slot k (1-5) | `.state{k-1}`  | `.state{k}`    |
//   | Auto              | `.state5`      | `.state.auto`  |
//   | RetroArch slot 0  | (none)         | `.state`       |
//
// RomM keys a state on `(rom_id, file_name)`, so a state crosses between the
// two only under one name. With `[sync] map_state_slots` on, a libretro
// module's Tico state is synced under RetroArch's name and placed back under
// Tico's: a game Tico suspended to its Auto slot resumes in RetroArch's on
// another device, and back. Off -- the default, #226 D4 -- each keeps its own
// name, and Tico's `.state1`-`.state5` share names with RetroArch's slots a slot
// apart.
//
// An isolated module (`tico-duckstation`) is never mapped: no RetroArch core
// loads its states, so there is no RetroArch name for it to take.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "rommsync/config.hpp"

namespace rommsync::scan {

/// RetroArch's name for the Tico state `tico_name`: `Game.state5` ->
/// `Game.state.auto`, `Game.state0` -> `Game.state1`. Nullopt for a name that
/// is not one of Tico's six slots. The extension is matched in any case and
/// written lower-case; the stem is kept as it is.
std::optional<std::string> RetroArchStateName(std::string_view tico_name);

/// The inverse: Tico's name for the RetroArch state `retroarch_name`. Nullopt
/// for RetroArch's slot 0 (`.state`), slots past 5, and anything that is not a
/// RetroArch state name -- a slot Tico has no file for.
std::optional<std::string> TicoStateName(std::string_view retroarch_name);

/// `<stem>.state0` to `<stem>.state5`: the names Tico loads a state from.
bool IsTicoStateName(std::string_view name);

/// `<stem>.state`, `<stem>.state<N>` for N >= 1, `<stem>.state.auto`: the names
/// RetroArch loads a state from. Never `.state0`, which RetroArch spells
/// `.state`.
bool IsRetroArchStateName(std::string_view name);

/// The name a state file is matched to its rom by: `Game.state.auto` ->
/// `Game.state`, so the rom lookup strips one extension as it does for any
/// other state. Every other name unchanged.
std::string StateMatchName(std::string_view name);

/// Whether a Tico state of `emulator_id` is synced under RetroArch's name:
/// `[sync] map_state_slots` is on and the id is a libretro core RomM's table
/// knows.
bool MapsStateSlots(const config::Config& config, std::string_view emulator_id);

}  // namespace rommsync::scan
