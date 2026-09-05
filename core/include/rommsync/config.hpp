// `sdmc:/config/rommsync/config.ini` -- the one file a human edits.
//
// Everything the engine does is aimed by this file: which server to talk to,
// whether to sync at all, and -- the part that actually costs an evening when it
// is wrong -- which SD folders each RomM platform maps to. docs/CONFIG.md is the
// user-facing description of the format; this header is what it means.
//
// **Nothing here can refuse to produce a config.** The sysmodule reads this file
// at boot and "never block boot" is a rule (CLAUDE.md), so a stray line, a typo
// in a boolean, or a whole file of garbage yields the built-in defaults plus a
// `Diagnostic` saying what was dropped -- never an empty config and never an
// abort. The reasoning is the same one behind `json::Reader` reporting *which*
// field was wrong, pushed one step further: a console with no screen and no
// keyboard cannot be talked through a parse error, so the client stays up,
// keeps working from defaults, and shows the complaint in the overlay.
//
// The one thing that is *not* defaulted is `server.url`. There is no sensible
// fallback for "which RomM", so an absent or unusable one leaves `configured()`
// false and the client idle, which is the correct behaviour on a first boot and
// the only honest one after a bad edit.
//
// Writes belong to the sysmodule, not here (docs/ARCHITECTURE.md): the overlay
// asks it to change a value over IPC and it persists the file. This module only
// reads. Serialising a `Config` back to `config.ini` is M5-3's.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace rommsync::config {

/// The file the configuration lives in, relative to `sdmc:/config/rommsync/`.
inline constexpr const char* kConfigFileName = "config.ini";

/// The largest `config.ini` that will be read.
///
/// A bound rather than "read the file" because this runs at boot on a sysmodule
/// heap measured in megabytes, and the file is on a FAT32 card that gets yanked
/// mid-write: a corrupt directory entry pointing at four gigabytes of garbage
/// must be a named refusal, not a `bad_alloc` before `main` gets anywhere. The
/// real thing is a few hundred bytes; a fully-expanded folder map for every
/// platform RomM knows is a few kilobytes.
inline constexpr std::size_t kMaxConfigBytes = 256 * 1024;

/// How many `[platform.<slug>]` sections are honoured, and how many directories
/// one `roms`/`saves`/`states` key may list.
///
/// The same reasoning as `kMaxConfigBytes` and `kMaxDiagnostics`, applied to what
/// the parse *builds* rather than what it reads: a corrupt card region full of
/// `[platform.a1]` headers is well under the byte bound and still turns into tens
/// of thousands of map entries on a heap that does not have them. RomM ships
/// around two hundred platforms and no folder needs sixty-four directories, so
/// neither bound can be reached by a file anyone wrote on purpose.
inline constexpr std::size_t kMaxPlatformSections = 256;
inline constexpr std::size_t kMaxPathsPerKey = 64;

/// The longest SD path accepted in the folder map. Horizon's `FS_MAX_PATH` is
/// 0x301 including the terminator, and a path longer than that cannot be opened
/// on the console however well-formed it looks here.
inline constexpr std::size_t kMaxPathLength = 768;

/// Bounds on `SyncConfig::interval_min`. Zero is not a violation of the floor --
/// it is the documented "only on boot and on demand".
///
/// The ceiling is a week. It exists because a value larger than the uptime of
/// any console is indistinguishable from "never", and a user who writes one
/// means "rarely", which a week satisfies -- so it is clamped rather than
/// rejected. Rejecting would fall back to 30 minutes, which is the opposite of
/// what they asked for.
inline constexpr int kMinIntervalMinutes = 0;
inline constexpr int kMaxIntervalMinutes = 7 * 24 * 60;

/// `[server]`.
struct ServerConfig {
  /// Origin, with any trailing slash removed: `https://romm.example.com`, or
  /// `http://romm.lan:8080/romm` when RomM sits under a path on a reverse
  /// proxy. Empty when there is no usable one, which is what `configured()`
  /// reports.
  ///
  /// Never carries `user:password@`: see `NormalizeServerUrl`.
  std::string url;
};

/// `[sync]`.
struct SyncConfig {
  bool enabled = true;

  /// Minutes between automatic syncs. `0` means only on boot and on demand.
  int interval_min = 30;

  bool on_boot = true;
  bool saves = true;

  /// Off by default: a save state is a snapshot of an emulator core's memory,
  /// so it survives neither a core update nor a different core, and syncing one
  /// onto a console running a different build is how a player loses a session
  /// they thought was safe. Opt-in, per docs/CONFIG.md.
  bool states = false;

  bool conflict_show = true;
};

/// `[downloads]`.
struct DownloadsConfig {
  bool enabled = true;
  bool verify_hash = true;
  bool resume = true;
};

/// Where one RomM platform lives on the SD card.
///
/// All three lists hold absolute paths from the SD root, normalised (see
/// `NormalizeSdPath`) and deduplicated in the order they were written. Any of
/// them may be empty, and an empty one is a decision rather than an oversight:
/// a platform with `roms` and no `saves` downloads fine and is skipped by the
/// save scanner, which is exactly what docs/CONFIG.md promises.
struct PlatformFolders {
  /// Download destinations. **The first entry is the write target**; the rest
  /// are only consulted when asking whether a rom is already on the card.
  std::vector<std::string> roms;

  /// Directories scanned for SRAM saves.
  std::vector<std::string> saves;

  /// Directories scanned for save states.
  std::vector<std::string> states;

  bool empty() const { return roms.empty() && saves.empty() && states.empty(); }
};

/// A rom, reduced to the two fields a destination is built from.
///
/// Named fields rather than two loose strings because the mistake this is
/// written against is a silent one: `platform_slug` and `platform_fs_slug` are
/// both on RomM's rom schema, both read `gba`, `nes`, `psx` on a conventional
/// library, and only the second one is the directory name `platforms` is keyed
/// by. A library whose PlayStation folder is called `playstation` downloads
/// nothing at all the day a caller passes the wrong one, and no fixture whose
/// slugs happen to agree would ever catch it.
struct RomFile {
  /// RomM's on-disk folder name for the platform. **Not `platform_slug`.**
  std::string_view platform_fs_slug;

  /// The rom's name on the *server's* filesystem: `240pee.nes`, or
  /// `Synthetic Two Disc Game` for a multi-file rom, where it names a
  /// directory. Never trusted -- see `ValidRomFileName`.
  std::string_view fs_name;

  /// Both fields are views, which makes this an argument and never a record:
  /// it must not outlive the parsed body they point into. A caller that keeps
  /// a rom keeps the rom's own strings and builds one of these at the call.
};

/// Where a rom is written, or why it is not written anywhere.
///
/// The two halves are exclusive: a resolved `path` carries no `reason`, and an
/// empty one always carries one. There is no third state and no guessed folder
/// -- an unmapped platform is a skip with a sentence, per docs/CONFIG.md.
struct RomDestination {
  /// Absolute SD path: the platform's first `roms` entry joined with the rom's
  /// `fs_name`. Empty when there is no destination.
  std::string path;

  /// Why there is none, in a sentence for the log and the overlay. Empty when
  /// `path` is set.
  ///
  /// **Never quotes `fs_name`.** That string comes off someone else's
  /// filesystem, and the reasons it can be refused -- a control character, 800
  /// characters of it -- are exactly the reasons not to paste it into a log
  /// line or an overlay row. The caller knows the rom id and the name it
  /// asked about; this says what was wrong with it.
  std::string reason;

  bool ok() const { return !path.empty(); }
};

/// The whole file, merged over the built-in defaults.
struct Config {
  ServerConfig server;
  SyncConfig sync;
  DownloadsConfig downloads;

  /// Keyed by RomM `platform_fs_slug` -- the on-disk folder name under RomM's
  /// `library/roms/`, so it is case-sensitive and not lowercased on the way in.
  /// A platform with no entry here has no mapping and is skipped entirely.
  std::map<std::string, PlatformFolders, std::less<>> platforms;

  /// There is a server to talk to. False on a first boot, and after an edit
  /// that made `server.url` unusable.
  bool configured() const { return !server.url.empty(); }

  /// The mapping for `slug`, or nullptr when there is none -- which is the
  /// signal to skip the platform, not a reason to guess a folder.
  const PlatformFolders* Platform(std::string_view slug) const;

  /// Where a download for `slug` is written: the first `roms` entry, or empty
  /// when the platform is unmapped or maps no rom folder.
  std::string RomTarget(std::string_view slug) const;

  /// The absolute path one rom downloads to: `RomTarget(rom.platform_fs_slug)`
  /// joined with the rom's `fs_name`, or an empty path and the reason there is
  /// none.
  ///
  /// This is the only supported way to turn a rom into a destination. The
  /// joining is not the interesting part -- the refusals are: an unmapped
  /// platform, a `fs_name` `ValidRomFileName` will not have, and a joined path
  /// past `kMaxPathLength`, which is refused rather than truncated because a
  /// truncated path is a perfectly real file in the mapped folder holding
  /// another rom's bytes.
  RomDestination DestinationFor(const RomFile& rom) const;

  /// Every path the rom could already occupy, in the order the map lists them,
  /// the write target first.
  ///
  /// "Is it already on the card?" reads *all* of a platform's `roms` entries,
  /// while only the first is written to (docs/CONFIG.md): the later entries are
  /// exactly the folders where someone already keeps that platform's roms, and
  /// a check that skipped them re-downloads a rom the card has.
  ///
  /// Empty when the platform is unmapped, maps no roms folder, or the name is
  /// one `ValidRomFileName` refuses. It is **not** a way to find the write
  /// target: a folder whose joined path is too long drops out of this list, so
  /// when the *first* folder is the one that overflows, `front()` here is the
  /// second folder while `DestinationFor` refuses outright. `DestinationFor`
  /// is the only answer to "where does this get written".
  std::vector<std::string> ExistingRomPaths(const RomFile& rom) const;

  /// Every save directory across every platform, deduplicated, in first-seen
  /// order.
  ///
  /// This exists because RetroArch keeps one flat `saves/` for every system, so
  /// the same directory is legitimately listed under a dozen platforms and a
  /// scanner walking the map naively would read it a dozen times and report a
  /// dozen copies of each file. Deduping is the caller's job exactly once, so
  /// it is here rather than in each caller (docs/CONFIG.md#validation-rules).
  std::vector<std::string> SaveScanDirs() const;

  /// The same for save states. Answers regardless of `sync.states`: whether to
  /// scan them is the engine's decision, not the map's.
  std::vector<std::string> StateScanDirs() const;
};

/// How bad a `Diagnostic` is.
///
/// Three levels rather than two because "we used what you wrote and cannot
/// check it" is a different thing from "we threw what you wrote away", and an
/// overlay that shows them the same way trains a user to ignore both.
enum class Severity {
  /// Taken as written. Worth reading once -- an unrecognised platform slug, a
  /// console with no `config.ini` yet -- and never a reason to look for a bug.
  kNotice,

  /// The line did not take effect as written: it was ignored, or replaced by
  /// its default -- or it took effect and carries a risk worth naming, as
  /// plain `http://` does. The rest of the file is in force and the client
  /// runs.
  kWarning,

  /// The client cannot do its job as configured: no usable server, or the file
  /// itself could not be read. The `Config` is still populated with defaults --
  /// this is a report, not an exception.
  kError,
};

/// Stable, log-friendly name. Never null.
const char* ToString(Severity severity);

/// One complaint about the file.
///
/// The line number is the point: "invalid boolean" sends a user hunting through
/// a file they cannot see; "line 9 [sync] states: expected true or false" is a
/// fix. `Describe()` renders exactly that.
struct Diagnostic {
  Severity severity = Severity::kWarning;

  /// 1-based line in the file. `0` when the complaint is about the file as a
  /// whole rather than a line in it.
  int line = 0;

  /// Section the line was in, as written minus the brackets (`sync`,
  /// `platform.snes`). Empty above the first section header.
  std::string section;

  /// Key the line set. Empty for a complaint about a section header or the
  /// file.
  std::string key;

  /// What was wrong, in a sentence a user can act on.
  ///
  /// **Never quotes the value of `server.url`.** A URL is the one field here
  /// that can carry a credential -- someone will write
  /// `https://me:hunter2@romm.lan` -- and a diagnostic goes to a log and to the
  /// overlay. Everything else is a folder path or a boolean and is quoted
  /// freely, because naming the offending text is most of the value.
  std::string message;

  /// "line 9 [sync] states: expected true or false".
  std::string Describe() const;
};

/// A parsed configuration and everything wrong with it.
///
/// `value` is always usable, whatever `diagnostics` says. See the header note.
struct LoadResult {
  Config value;

  /// In file order, then whole-file complaints. Bounded: a file of nothing but
  /// bad lines produces at most `kMaxDiagnostics` of these plus a final one
  /// saying the rest were dropped, because a sysmodule that answers a corrupt
  /// file with fifty thousand strings has turned a typo into an outage.
  std::vector<Diagnostic> diagnostics;

  /// No `kError`. Warnings are compatible with a working client.
  bool ok() const;

  /// One line per diagnostic, for a log. Empty when there are none.
  std::string DescribeDiagnostics() const;
};

/// How many diagnostics are kept before the rest are summarised.
inline constexpr std::size_t kMaxDiagnostics = 64;

/// The mapping the client ships with: Tico for roms, RetroArch and Tico for
/// saves and states, for the systems the Switch actually has an emulator for.
///
/// `config.ini` overrides entries in this map; it does not have to restate it.
/// Platforms deliberately absent (ps2, ps3, ps4, wii, ngc, 3ds) are absent
/// because nothing on the console runs them -- mapping one is a supported
/// override, not a defect in this list.
///
/// The keys are a *guess* at the user's RomM folder names, since a
/// `platform_fs_slug` is whatever they called the directory. They are the
/// conventional ones; a library that uses other names needs overrides.
const std::map<std::string, PlatformFolders, std::less<>>& DefaultPlatforms();

/// The configuration of a console with no `config.ini` at all.
Config Defaults();

/// Parse the contents of a `config.ini`.
///
/// Pure: no filesystem, no clock, no network. Every rule is here rather than in
/// `LoadConfig` so the whole grammar is testable against a string literal.
LoadResult ParseConfig(std::string_view text);

/// Read `path` and parse it.
///
/// A file that is not there is not a failure -- it is a console that has not
/// been configured yet -- so it yields the defaults and a single `kNotice`
/// saying so. A file that exists and will not open, or is larger than
/// `kMaxConfigBytes`, is a `kError`: something is there and the user's settings
/// are not being honoured, which they have to be told rather than left to infer
/// from a client that quietly does something else.
///
/// A *missing* file falls back to `io::PreviousPathFor(path)` first, the same
/// recovery `token_store` and `device_identity` make and for the same reason:
/// `io::WriteAtomically` moves the record already in place aside before renaming
/// the new one on, so the one moment `config.ini` legitimately does not exist is
/// the moment the user's settings are sitting intact under `config.ini.old`.
/// Only a missing file takes it -- a file that exists and will not open is a bad
/// moment, not evidence that the previous record is the current one.
LoadResult LoadConfig(const std::string& path);

/// Normalise one SD path from the folder map, or fail.
///
/// Absolute from the SD root, `/` separated, no trailing slash (except the root
/// itself), no repeated slashes, no `.` or `..` segment, no backslash -- FAT32
/// forbids one in a name, so it is a Windows habit rather than a path -- no
/// control character or NUL, and no longer than `kMaxPathLength`.
///
/// `..` is refused rather than resolved. These paths name folders a human
/// picked; a `..` in one is a mistake, and resolving it silently would turn a
/// mistake into a folder that exists and gets written to.
///
/// Returns false and sets `why` to a user-facing reason on refusal; `out` is
/// left untouched.
bool NormalizeSdPath(std::string_view raw, std::string* out, std::string* why);

/// True when `fs_name` is usable as one leaf inside a mapped folder.
///
/// A `fs_name` is a name on the *server's* filesystem, chosen by whoever filled
/// that library, and nothing has checked it before it reaches a path this
/// client writes to -- `NormalizeSdPath` validates the directories a human
/// configured and nothing validates this. So: not empty, no path separator
/// (`/` or `\`), not `.` or `..`, no control character or NUL, none of the
/// characters FAT32 and exFAT reserve (`? * : " < > |`), and no longer than
/// `kMaxPathLength`.
///
/// The reserved set is refused rather than rewritten. `?` is conventional in a
/// No-Intro name and perfectly legal on the Linux filesystem RomM scanned, so
/// the card is where it stops being storable -- and a rom whose name this
/// client quietly changed would be looked for, on the next tick, under a name
/// nothing ever wrote, and downloaded again forever.
///
/// It is a *leaf*, so a separator is refused rather than normalised away. A
/// `fs_name` with one in it is not a deeper folder anybody asked for; it is a
/// server naming a path inside this console's SD card, and `../../atmosphere`
/// under a mapped folder is how a download becomes a system file. The only
/// safe answer is to refuse that rom by name.
///
/// Sets `why` to a user-facing reason on refusal. It never quotes `fs_name`,
/// for the reason `RomDestination::reason` gives.
bool ValidRomFileName(std::string_view fs_name, std::string* why);

/// Normalise the `[server] url`, or fail.
///
/// Requires `http://` or `https://` and a real host -- `https://:8080` and
/// `https://host:` are refused, since both are `configured()` and reach nothing,
/// which is precisely the "quietly does something else" failure this module is
/// written against. A bracketed IPv6 literal (`http://[::1]:8080`) is a host.
/// Drops a trailing
/// slash; keeps a path prefix, because RomM behind a reverse proxy at `/romm`
/// is a normal deployment. Refuses a query or a fragment (this is an origin,
/// not a link) and refuses `user:password@` outright -- RomM authenticates with
/// a bearer token, so credentials in the URL are never used, and carrying them
/// only leaks them into every log line that names the server.
///
/// `why` never quotes the input, for that same reason. Plain `http://` is
/// accepted; the warning about it is raised by the caller that has somewhere to
/// put it.
bool NormalizeServerUrl(std::string_view raw, std::string* out, std::string* why);

/// True when `text` is one of the accepted spellings of a boolean: `true`,
/// `false`, `yes`, `no`, `on`, `off`, `1`, `0`, in any case.
bool ParseBool(std::string_view text, bool* out);

}  // namespace rommsync::config
