// `sdmc:/retroarch/retroarch.cfg`, read for the seven settings that decide where
// RetroArch keeps saves and states -- and whether it syncs them itself.
//
// The folder map in `config.ini` says where a user *thinks* RetroArch's saves
// are. RetroArch decides where they *are*, and it decides in its own file: a
// `savefile_directory`, and on top of it sort-by-core (`saves/<CoreFolder>/`,
// which RomM's own guide tells RetroArch users to turn on, and which is the
// Switch build's default) or saves kept beside the content. A client that reads
// only the map walks a folder RetroArch stopped writing to and reports nothing,
// or worse, reports the stale copy (#230).
//
// **The defaults are the Switch build's, read off RetroArch's source rather than
// remembered** -- v1.22.2, `frontend/drivers/platform_switch.c` and
// `config.def.h`. Its save and state folders are `/retroarch/cores/savefiles`
// and `/retroarch/cores/savestates`, *not* the `/retroarch/saves` this client's
// built-in map has guessed since rc1, and sort-by-core is on unless the file
// says otherwise. Whether the build on a real console matches its source is
// #44's to check.
//
// **Nothing here refuses.** RetroArch may not be installed, its file may be
// half-written -- RetroArch saves it on exit, from another process, while this
// one runs -- and a tick may not block on either. A missing, unreadable,
// oversized or malformed file yields RetroArch's defaults for every setting it
// did not state usably, and a diagnostic saying so.
//
// The file is streamed rather than read whole: it runs to a hundred kilobytes
// or so, is read every tick, and this client keeps seven values of it. Holding
// the rest on a sysmodule heap measured in megabytes would buy nothing.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/config.hpp"

namespace rommsync::retroarch {

/// Where RetroArch keeps its configuration, SD-root absolute: the directory of
/// `retroarch_switch.nro` joined with `retroarch.cfg` (`platform_switch.c`).
inline constexpr const char* kConfigSdPath = "/retroarch/retroarch.cfg";

/// The Switch build's `DEFAULT_DIR_SRAM` and `DEFAULT_DIR_SAVESTATE`: the core
/// directory joined with `savefiles` and `savestates` [#44 verifies on console].
inline constexpr const char* kDefaultSavefileDirectory = "/retroarch/cores/savefiles";
inline constexpr const char* kDefaultSavestateDirectory = "/retroarch/cores/savestates";

/// The largest `retroarch.cfg` that is read. A real one is about a hundred
/// kilobytes; past this the file is a corrupt card region rather than a
/// configuration, and none of it is believed.
inline constexpr std::size_t kMaxConfigBytes = 1024 * 1024;

/// The longest line kept. RetroArch's own path buffers are 4096 bytes, so a
/// longer line is not one it wrote; the line is ignored, and said to be when it
/// set a key read here.
inline constexpr std::size_t kMaxLineBytes = 8192;

/// How many complaints about one file are spelled out before the rest are
/// counted -- the `config::kMaxDiagnostics` reasoning for a file this client
/// does not even own.
inline constexpr std::size_t kMaxDiagnostics = 8;

/// The settings, each at RetroArch's default until the file says otherwise.
struct Settings {
  /// `savefile_directory`, SD-root absolute and normalised. `default` and an
  /// empty value both mean the build's default, as they do to RetroArch.
  std::string savefile_directory = kDefaultSavefileDirectory;
  std::string savestate_directory = kDefaultSavestateDirectory;

  /// `sort_savefiles_enable`: saves go in `<savefile_directory>/<CoreFolder>/`.
  bool sort_savefiles = true;
  bool sort_savestates = true;

  /// `sort_savefiles_by_content_enable`: a folder per content directory, which
  /// this client does not read (`Notes` says so).
  bool sort_savefiles_by_content = false;
  bool sort_savestates_by_content = false;

  /// `savefiles_in_content_dir`: saves beside the rom, which this client reads
  /// only when `[sources] retroarch_content_dir` opts in.
  bool savefiles_in_content_dir = false;
  bool savestates_in_content_dir = false;

  /// `cloud_sync_enable` and `cloud_sync_sync_saves`: RetroArch's own Cloud Sync
  /// is on, and covers saves. Both are needed -- with the second off RetroArch
  /// syncs its configs and leaves the saves to whoever else will.
  bool cloud_sync = false;
  bool cloud_sync_saves = true;

  /// RetroArch is syncing its own saves and states.
  bool SyncsItself() const { return cloud_sync && cloud_sync_saves; }
};

/// The settings and everything wrong with the file they came from.
struct LoadResult {
  Settings value;

  /// Bounded by `kMaxDiagnostics`, plus one line counting the rest. Never a
  /// `kError`: this is not the client's own configuration, and the status
  /// screen's error count is about `config.ini`.
  std::vector<config::Diagnostic> diagnostics;

  /// The file was there and was read, malformed or not.
  bool found = false;
};

/// Parse the contents of a `retroarch.cfg`. Pure.
///
/// The grammar is RetroArch's `config_file`: `key = "value"` or `key = value`,
/// `#` starting a comment, the last assignment of a key winning. A boolean is
/// `true`, `false`, `1` or `0`, as `config_get_bool` reads it; anything else
/// leaves that setting at its default and says so. A directory may carry the
/// `sdmc:` device prefix; one that is not absolute after it is ignored the way
/// RetroArch ignores a directory it cannot use.
LoadResult Parse(std::string_view text);

/// Stream `path` -- a real path, from `fs::FileSystem::Resolve` -- through
/// `Parse`'s rules, never holding more than one line of it.
///
/// A missing file is a `kNotice`: RetroArch not installed, or never started,
/// and its defaults are what it would use. An unreadable or oversized one is a
/// `kWarning`, and every setting is RetroArch's default. An empty `path` is a
/// missing file.
LoadResult Load(const std::string& path);

/// RetroArch's folders are scanned this tick: `[sources] retroarch` forces the
/// answer, and `auto` -- the default -- says yes unless RetroArch syncs itself.
///
/// Two writers to one file is the failure this exists to avoid. RomM's
/// RetroArch device writes no baselines (#226 R2), so a save both clients synced
/// would look like another device's churn on every tick.
bool Enabled(const config::Config& config, const Settings& settings);

/// What the user should know about how RetroArch's folders are read this tick:
/// that RetroArch syncs itself, that both it and this client are syncing, that
/// saves kept beside the roms are not read without opting in, that a
/// per-content-directory layout is not read at all.
///
/// The overlay shows these with `config.ini`'s diagnostics, which is where
/// "RetroArch syncs itself" is read. Empty for a console with nothing to say.
std::vector<config::Diagnostic> Notes(const config::Config& config, const Settings& settings);

}  // namespace rommsync::retroarch
