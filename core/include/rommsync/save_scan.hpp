// Step 0 of docs/SYNC_PROTOCOL.md: the SD card, as a list of saves -- and, when
// the user has opted in, save states -- that have a rom id.
//
// The scanner walks the folders `config::Config::SaveScanDirs()` hands it,
// strips each file's extension, asks `roms::RomIndex` which rom that name is,
// and emits what `sync::ClientSaveState` needs. Everything else in M2 is a
// no-op without it -- and a *wrong* answer here is not a failed tick, it is one
// game's save uploaded over another game's, so the two rules below are the
// point of the module rather than details of it:
//
//   - **An ambiguity is skipped, never guessed.** A base name that exists on
//     two platforms, found in a directory that implies neither -- RetroArch's
//     one flat `saves/` -- is reported and left alone.
//   - **Every emitted record is `sync::Validate`-clean.** A file whose mtime
//     the card could not give up, or whose name the server would read as a
//     path, is a skip with a reason rather than an entry that turns into a 422
//     -- or worse, into a save the server accepts and arbitrates as something
//     the client did not mean.
//
// **A state is scanned by the same walk and paired on a different key.** RomM
// keys a save `(rom_id, slot)` and a state `(rom_id, file_name)` -- verified
// against a live 5.2.0, docs/API_CONTRACT.md#save-states -- so `ScanStates`
// emits `StateFile` rather than `SaveFile` and its duplicate rule is about the
// name rather than the slot. Everything else about the walk is shared, because
// everything else about it is the same decision: an ambiguous name is skipped,
// a folder listed under a dozen platforms is walked once, and a file whose
// mtime the card would not give up costs one file rather than the tick.
//
// The digest is M2-3's arithmetic and this module's responsibility. Every save
// is handed to `state::ContentHashFor`, which reuses the baseline's digest when
// the file's mtime *and* size still match and reads the file otherwise -- so an
// unchanged card costs no reads and a save is never reported without a hash it
// could have had. Null is a documented value ("cannot compare content", fall
// back to timestamps), but it is not a shortcut: a save reported without a
// digest is planned as an upload the server already has, on this tick and every
// tick after it. The only ones that carry a null here are the ones whose bytes
// genuinely could not be read, and they are counted and named.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/config.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::scan {

/// Which of a platform's folder lists a scan is walking.
enum class Folders {
  kSaves,
  kStates,
};

/// One local save file, matched.
struct SaveFile {
  std::int64_t rom_id = 0;

  /// Where it is, SD-root absolute. The scan's own output: negotiate never
  /// sees it, and every later step -- back up, overwrite, upload -- needs it,
  /// because the server's `file_name` is not this one (SYNC_PROTOCOL.md step 2).
  std::string sd_path;

  /// The file's own name, `Game (USA).srm`. Never a path.
  std::string file_name;

  /// The pairing key with `rom_id`, derived and stable: `retroarch-srm`.
  ///
  /// Derived rather than chosen because it has to be the *same* string on the
  /// next tick and on the next boot -- a slot that moves makes the same file a
  /// new save every time. It carries the emulator as well as the save's
  /// extension because RomM pairs on `(rom_id, slot)` alone: a rom whose
  /// RetroArch `.srm` and Tico `.srm` both mapped to `srm` would have the two
  /// files overwrite each other through the server, forever.
  std::string slot;

  /// `retroarch` or `tico`, when the directory said so; empty when it did not,
  /// which is sent as `null`.
  std::string emulator;

  /// The rom's platform, for callers that lay files out by it.
  std::string platform_fs_slug;

  /// The save's **MD5**, 32 lowercase hex digits, from `state::ContentHashFor`
  /// -- computed or reused from the baseline, the caller cannot tell and does
  /// not need to. Empty only when the file's bytes could not be read, which is
  /// reported in `ScanResult::unhashed`.
  std::string content_hash;

  std::int64_t size_bytes = 0;

  /// mtime, whole seconds since the Unix epoch, UTC.
  std::int64_t modified_unix = 0;

  /// The negotiate entry for this file, carrying `content_hash` above.
  ///
  /// The argument overrides it, for a caller that hashed the file itself or
  /// re-read it after writing. Passing an empty string is the same as passing
  /// nothing: "" and `null` are different values to the server and only one of
  /// them is a value, which is why `sync::Validate` refuses the former.
  sync::ClientSaveState ToClientSaveState(
      std::optional<std::string> content_hash = std::nullopt) const;
};

/// One local save **state** file, matched.
///
/// Deliberately not a `SaveFile` with the slot left empty. The two are paired on
/// different keys, and a struct that could be read as either is a struct a
/// caller eventually pairs on the wrong one -- which for a state is another
/// console's session overwritten.
struct StateFile {
  std::int64_t rom_id = 0;

  /// Where it is, SD-root absolute.
  std::string sd_path;

  /// The file's own name, `Game (USA).state`. Never a path -- and this is the
  /// pairing key with `rom_id`, on both sides: RomM stores a state under the
  /// name it was sent, without the ingest tag it stamps on a save.
  std::string file_name;

  /// `retroarch` or `tico`, when the directory said so; empty when it did not.
  /// Sent as the `emulator` query parameter and used to place a download, but
  /// never part of the key -- see `SkipReason::kDuplicateName`.
  std::string emulator;

  /// The rom's platform, for callers that lay files out by it.
  std::string platform_fs_slug;

  /// The state's **MD5**, from `state::ContentHashForState` -- computed or
  /// reused from the baseline. Empty only when the bytes could not be read.
  ///
  /// Local only. RomM computes no digest for a state, so this is never compared
  /// against a server value; it is how *this* client tells a state it has
  /// already uploaded from one that has changed since.
  std::string content_hash;

  std::int64_t size_bytes = 0;

  /// mtime, whole seconds since the Unix epoch, UTC.
  std::int64_t modified_unix = 0;
};

/// Why a file in a save directory did not become a `SaveFile`.
enum class SkipReason {
  kUnmatched,       ///< no rom carries the name; usually not a save at all
  kAmbiguous,       ///< several do, and the path implied no platform
  kDuplicateSlot,   ///< an earlier file already claimed this `(rom_id, slot)`
  kUnusable,        ///< `sync::Validate` would refuse it -- an unset mtime, say
  kDirectoryFailed, ///< a mapped folder could not be read; its files are simply absent
  kTooManySaves,    ///< the scan hit `kMaxSaves` and stopped
  kUnhashed,        ///< reported, but with no digest: its bytes could not be read

  /// A state whose `(rom_id, file_name)` an earlier state already claimed.
  ///
  /// `kDuplicateSlot`'s reasoning on the key RomM actually enforces for a
  /// state: two local files landing on the same pair would overwrite each other
  /// *through RomM* -- its `POST /api/states` replaces the row with that name in
  /// place rather than making a second one -- on alternating ticks, forever. The
  /// realistic pair is one game's `Game.state` under RetroArch and under Tico,
  /// which is why the emulator is deliberately not part of the key: the server
  /// ignores it, so a client that keyed on it would be arranging exactly that
  /// alternation.
  kDuplicateName,

  kTooManyStates,   ///< the state scan hit `kMaxStates` and stopped
};

/// Stable, log-friendly name. Never null.
const char* ToString(SkipReason reason);

/// One file the scan did not emit, and why.
struct Skip {
  SkipReason reason = SkipReason::kUnmatched;

  /// The file, or -- for `kDirectoryFailed` -- the directory.
  std::string sd_path;

  /// What was wrong, in a sentence a user can act on.
  std::string message;

  /// "ambiguous /retroarch/saves/Game.srm: \"Game\" matches 2 roms (gb, gba)".
  std::string Describe() const;
};

/// How many saves one tick will report, and how many skips it will spell out.
///
/// Both bounds are the `config::kMaxDiagnostics` reasoning: a flat `saves/`
/// holding four thousand files nobody has roms for must cost one log line and a
/// count, not four thousand strings on a sysmodule heap. The *count* is always
/// exact -- only the spelling out is bounded.
/// `kMaxSaves` is `state::kMaxRecords` and not a round number of its own.
///
/// `state.db` holds one row per reported save and its writer *refuses* a
/// baseline with more rows than that (`state_db.hpp` sizes it against the
/// sysmodule's ~390 KiB, and says the two constants and `kInnerHeapSize` move
/// together). A scanner that emitted more would produce a tick whose baseline
/// can never be written: the write fails, nothing is stored, and the whole
/// library is re-hashed on the next tick and every tick after it -- silently,
/// which is the failure that refusal exists to prevent. Stopping here costs the
/// saves past the bound and says so; the alternative costs all of them, forever,
/// and says nothing.
inline constexpr std::size_t kMaxSaves = state::kMaxRecords;
inline constexpr std::size_t kMaxSkipsReported = 64;

/// How many states one tick will report.
///
/// A share of `state::kMaxRecords` rather than a bound of its own, because the
/// baseline holds saves and states in one file and it is the *file* the writer
/// refuses over. A quarter, because states are opt-in and a console that has
/// turned them on still holds far more saves than states -- and because the
/// alternative, halving the save budget for a feature that is off by default,
/// would cost every console something to buy one console this.
///
/// It is a scan bound, not the whole story: `sync::SyncStates` trims its own
/// rows before a save's when the two together would still exceed the file's
/// bound. A save is what hard rule 2 protects.
inline constexpr std::size_t kMaxStates = state::kMaxRecords / 4;

struct ScanResult {
  /// Filled by `ScanSaves`; empty from `ScanStates`.
  std::vector<SaveFile> saves;

  /// Filled by `ScanStates`; empty from `ScanSaves`. One result type for both
  /// walks, so the skip and diagnostic machinery below is shared rather than
  /// spelled a second way.
  std::vector<StateFile> states;

  /// The first `kMaxSkipsReported` skips, in scan order.
  std::vector<Skip> skipped;

  /// Every skip, including the ones not spelled out above.
  std::size_t skipped_total = 0;

  /// Files seen in the mapped directories, matched or not.
  std::size_t files_seen = 0;

  /// The rom index did not hold the whole library (`roms::RomIndex::truncated`).
  ///
  /// Carried because it changes what an unmatched file *means*: with a short
  /// index, "no rom named X" may simply be a rom that was never read, and a
  /// caller that reported those as files-with-no-rom would send a user hunting
  /// for a library problem that is really a client bound.
  bool index_truncated = false;

  /// Saves reported with no digest because their bytes could not be read, in
  /// scan order and bounded like `skipped`.
  ///
  /// Not skips: they are still negotiated, on timestamps, which is the right
  /// call for a save the card would not open this once. But it is the state
  /// where the server plans an upload for bytes it may already have, so it is
  /// counted and named rather than silently normal.
  std::vector<Skip> unhashed;

  /// Every unhashed save, including the ones not spelled out above.
  std::size_t unhashed_total = 0;

  /// One line per reported skip, plus a final line when there were more. Empty
  /// when the scan skipped nothing.
  ///
  /// Includes the unhashed saves: they are not skips, but a caller that logs
  /// this and nothing else would otherwise never see them, and "reported with
  /// no digest" is the state where the server uploads bytes it already has.
  std::string DescribeSkipped() const;
};

/// Walk `config`'s save directories and match what is in them.
///
/// Directories come from `config::Config::SaveScanDirs()`, so one physical
/// folder listed under a dozen platforms is walked **once** and yields one
/// record per file. Order is the config's first-seen order, and each directory's
/// entries are sorted by name, so two runs over an unchanged card produce the
/// same list -- which is what makes the duplicate-slot rule below deterministic
/// rather than a race with `readdir`.
/// `baseline` is `state.db` as the last tick left it, and it is required rather
/// than defaulted: an empty one is a perfectly good baseline that hashes
/// everything, and a caller that *forgot* the argument would otherwise get a
/// scan that quietly reported no digests at all.
ScanResult ScanSaves(const config::Config& config, const roms::RomIndex& index,
                     fs::FileSystem& files, const state::Baseline& baseline);

/// The same walk over `config::Config::StateScanDirs()`, emitting states.
///
/// **It does not consult `sync.states`.** `StateScanDirs()` answers regardless
/// of the toggle because whether to scan is the engine's decision rather than
/// the map's (config.hpp), and the engine makes it in `sync::SyncStates` -- one
/// place, where the same `if` also decides whether `/api/states` is called at
/// all. A second copy of that check here would be a second thing to get wrong,
/// and the failure mode of getting it wrong is silent: a console reading state
/// directories the user never opted into.
ScanResult ScanStates(const config::Config& config, const roms::RomIndex& index,
                      fs::FileSystem& files, const state::Baseline& baseline);

// --- the pieces, exposed because they are the parts worth testing directly ----

/// `Game (USA).srm` -> `Game (USA)`. A name with no extension is its own base,
/// and a leading dot is not an extension -- `.DS_Store` is a whole name.
///
/// One extension, not every one: `Game.state.auto` reduces to `Game.state`,
/// which matches no rom and is reported. That is the honest answer -- stripping
/// until something matches is how `Game.nes.srm` becomes a match for a rom
/// called `Game.nes` that the user does not have.
std::string BaseName(std::string_view file_name);

/// The emulator a mapped directory belongs to: `retroarch`, `tico`, or empty.
///
/// Any segment of the path, not just the first, so `/emulators/retroarch/saves`
/// is recognised as well as the default `/retroarch/saves`. The comparison is
/// case-insensitive because FAT32 is.
std::string EmulatorFor(std::string_view sd_dir);

/// The slot for a file from `emulator`'s directory. See `SaveFile::slot`.
std::string SlotFor(std::string_view emulator, std::string_view file_name);

/// Which platform each save directory implies, keyed by directory.
///
/// A directory listed under exactly one platform implies it; one listed under
/// several -- RetroArch's flat `saves/` -- implies nothing and maps to an empty
/// string, which is what makes its files match across the whole library and
/// makes a two-platform name an ambiguity rather than a coin toss.
std::map<std::string, std::string, std::less<>> PlatformHints(
    const config::Config& config, Folders which = Folders::kSaves);

}  // namespace rommsync::scan
