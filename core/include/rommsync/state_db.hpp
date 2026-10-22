// `sdmc:/config/rommsync/state.db` -- what the last sync left behind.
//
// One row per `(rom_id, slot)`: the digest and mtime the client last reported,
// and the ones the server last held. Two things read it. The scanner asks "is
// this file still the one I hashed?" and skips the hashing when the mtime and
// size both match, which is what keeps a tick cheap on a large library. M2-8's
// conflict detection asks what each side looked like at the last sync, which is
// how "both sides changed" is told from "only one did"
// (docs/SYNC_PROTOCOL.md#change-detection-between-ticks).
//
// **It is an optimisation, never a gate.** A `state.db` that is missing,
// truncated or garbage yields an empty baseline and a diagnostic, and the tick
// proceeds by hashing everything -- the server arbitrates either way, so a lost
// baseline costs time and never correctness. That is why nothing here refuses:
// the one thing it must not do is stop a sync.
//
// **Skipping the hash is not skipping the report.** A file whose mtime and size
// still match its row is not re-read, and the *stored* `content_hash` still goes
// into the payload. A save reported without one is a save the server compares on
// timestamps alone, which plans an upload for bytes it already has -- forever
// (sync.hpp).
//
// ## The format
//
// docs/ARCHITECTURE.md used to leave this as "small SQLite or a flat file". It
// is a flat, line-oriented file, because `core/` may include only standard and
// `rommsync/` headers (core/AGENTS.md) so SQLite cannot be linked into the
// portable engine at all, and because a sysmodule heap has no room for one.
//
//     rommsync-state 3
//     {"rom_id":12,"slot":"autosave","content_hash":"...","mtime":1757000000,...}
//     {"rom_id":12,"slot":null,...}
//     {"kind":"state","rom_id":12,"file_name":"Game (USA).state",...}
//
// A version line, then one JSON object per row. JSON per line rather than a
// separator-delimited record because a save's `slot` and `file_name` are user
// data that can hold any byte a filesystem allows, and `json::Quote` already
// escapes them exactly once -- inventing a second escaping scheme here is how a
// slot with a tab in it becomes two columns. The rows are read with
// `json::Reader`, the same strict reader every server response goes through, so
// a field of the wrong type is a named diagnostic rather than a default value
// that silently means "unchanged".
//
// ## Two kinds of row, and why the version moved
//
// Version 1 held saves only, keyed `(rom_id, slot)`. M2-8 added save states,
// which RomM keys `(rom_id, file_name)` and gives no slot at all, so a state
// cannot be a `SaveRecord`: synthesising a slot for one would collide with a
// real save whose slot happened to match, and the collision would be a state
// overwriting a save through the baseline. So a row is now **discriminated**.
// `"kind":"state"` is a state; an absent `kind` is a save, which keeps a save
// row byte-identical to the v1 row it replaced and keeps the per-row cost the
// heap arithmetic below was sized against.
//
// A version 1 file is not migrated. `ParseBaseline` discards a file whose
// version line it does not recognise, which is a re-hash and never a refusal --
// one extra tick of hashing, once, on the boot after the upgrade. That is the
// designed cost of the version line, not a migration anybody has to write.
//
// ## Version 3: the shared slot (#236)
//
// The rows did not change shape; what a save's slot *means* did. Version 2
// derived it from the folder's source and the extension (`tico-srm`); version 3
// syncs every save in the configured shared slot (`autosave`, `autosave-rtc`).
// Version 2 is every card rc1 wrote, so it is **read, not discarded**: discarding
// it would lose its states' rows -- which no rescan rebuilds, and without which a
// state is kept on both sides forever -- and every save's digest. Its states and
// archival rows are in force as they were; its slotted rows are held apart in
// `LoadedBaseline::legacy_saves` and moved by `AdoptLegacySaves` to the slot
// each file is synced in now. A newer file than this build's is discarded.
//
// Timestamps are whole Unix seconds, not RFC 3339. The comparison the server
// makes is at second granularity anyway, and a number cannot be spelled two
// ways -- which the string can: this client writes `Z` and RomM writes `+00:00`
// (`sync::FormatTimestamp` and `sync::ParseTimestamp` read and write both, and
// the row stores the result rather than the spelling).
//
// The write is `io::WriteAtomically` and the read recovers from
// `io::PreviousPathFor`, the same commit and the same recovery `token.dat`,
// `device.dat` and `config.ini` use -- see atomic_file.hpp for why the commit is
// two renames.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/file_system.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::state {

/// The file the baseline lives in, relative to `sdmc:/config/rommsync/`.
inline constexpr const char* kStateFileName = "state.db";

/// The first line of a well-formed file, `<magic> <version>`.
inline constexpr const char* kFormatMagic = "rommsync-state";
inline constexpr int kFormatVersion = 3;

/// The version before #236 moved every save to the shared slot: the same rows,
/// with slots derived as `<source>-<extension>`. Read, and migrated by
/// `AdoptLegacySaves` -- see `LoadedBaseline::legacy_saves`.
inline constexpr int kSourceSlotVersion = 2;

/// The largest `state.db` that will be read, and the most rows it may hold.
///
/// The same reasoning as `config::kMaxConfigBytes`: a FAT32 card that gets
/// yanked mid-write leaves a directory entry that can claim any size at all, so
/// a corrupt one has to be a named refusal rather than a `bad_alloc`.
///
/// **These are sized against the sysmodule's heap, not against a round number.**
/// A bound larger than the heap is not a bound -- it is a `bad_alloc` with a
/// constant next to it. The arithmetic, from
/// [docs/DEVELOPMENT.md](../../../docs/DEVELOPMENT.md#tls-in-a-sysmodule):
/// `kInnerHeapSize` is `0x180000` (1.5 MiB) since M9-20 (#246) -- M1-7 (#126) made
/// it a number somebody derived rather than devkitPro's template default,
/// #207 re-derived it once the two thread stacks and the open-directory cache
/// had terms, #201 added the download's 64 KiB read chunk, and #246 the rows a
/// tick holds for every save -- and the trimmed socket transfer memory takes
/// 116 KiB of it. Loading a baseline costs the file's text and then the parsed
/// `Baseline`, whose per-row `std::string`s each exceed the 15-character
/// small-string buffer and land on the heap, so the map costs more than the text
/// it came from. At ~214 bytes a row, `kMaxRecords` rows is ~27 KiB of text and
/// ~33 KiB parsed, under the 32 KiB each the heap table gives them.
///
/// **128, and 32 KiB, since M9-20 (#246).** They were 512 and 128 KiB, sized
/// against the baseline alone -- and a save costs a tick far more than its row
/// here: the scan's record, what is reported and targeted, the plan's operation
/// and what it did, ~1.7 KiB a save on aarch64 at the tick's worst moment, all
/// held beside the baseline. At 512 that was more heap than the table had, so
/// `kMaxRecords` is what the heap holds, and the trade -- ~140 KiB of resident
/// `.bss` for each further 64 saves -- is `sysmodule/source/main.cpp`'s to make
/// again, beside the rest of the table, rather than this constant's alone.
///
/// `core.state_db` asserts the two agree -- a full baseline must serialize to
/// less than the byte bound, or the writer would produce a file the reader
/// discards. And it is the *writer* that enforces both, so exceeding them is a
/// named refusal once rather than a silent re-hash on every boot.
///
/// **`kMaxRecords` is the budget for saves *and* states together**, because it
/// bounds the file rather than either kind. `scan::kMaxStates` is the share the
/// state scan may take of it and is a quarter, on the grounds that states are
/// opt-in and a console holds far fewer of them; the states half of a tick trims
/// its own rows before a save's rather than letting the two compete
/// (`sync::SyncStates`). A save is what hard rule 2 protects, so a save row is
/// never dropped to make room for a state.
///
/// A state row is the longer of the two -- it carries a file name and an
/// emulator where a save carries a slot -- so the byte bound is the tighter of
/// the pair for a mixed file, and `core.state_db` asserts the worst mix the scan
/// bounds can produce rather than only a file of saves.
///
/// **A card whose saves alone reach `kMaxRecords` records no states at all.**
/// `scan::kMaxSaves` is the whole of this bound, so there is no room left beside
/// them; `sync::SyncStates` drops every state row rather than write a file
/// `ParseBaseline` would discard whole, and each state is then kept on both
/// sides forever instead of syncing. It settles rather than churning -- the next
/// tick finds no row and answers keep-both, which sends nothing -- and the run
/// says so in a sentence naming the remedy. This is the card #11 describes:
/// `kInnerHeapSize`, `scan::kMaxSaves` and `kMaxRecords` move together.
///
/// A card with more saves than this needs the sysmodule heap raised first.
/// These two constants and `kInnerHeapSize` move together.
inline constexpr std::size_t kMaxRecords = 128;
inline constexpr std::size_t kMaxStateBytes = 32 * 1024;

// A plan names one operation per save, and a save this file cannot record is a
// save no tick can finish: the two bounds are one number (M9-20, #246).
static_assert(sync::kMaxPlanOperations == kMaxRecords,
              "a negotiate plan may carry exactly as many operations as state.db records");

/// How many diagnostics are kept before the rest are dropped. `config`'s
/// reasoning: a corrupt file must not answer with fifty thousand strings.
inline constexpr std::size_t kMaxDiagnostics = 32;

/// What one save looked like at the end of the last sync.
struct SaveRecord {
  /// The rom this save belongs to. Positive, like `ClientSaveState::rom_id`.
  std::int64_t rom_id = 0;

  /// The pairing key with `rom_id`. `null` is archival/manual-upload, and is a
  /// different row from any slotted one -- exactly as the server pairs them
  /// (sync.hpp). An empty string is neither and is refused.
  std::optional<std::string> slot;

  /// The MD5 the client last reported: 32 lowercase hex digits
  /// (`sync::kContentHashDigits`). Never empty in a stored row -- a row with no
  /// digest is a row that saves no hashing, so it is not worth writing.
  std::string content_hash;

  /// The local file's mtime when that digest was taken, truncated to whole
  /// seconds. Compared against the file's current mtime, at the same
  /// truncation, to decide whether the digest still describes it.
  sync::Timestamp mtime{};

  /// Its size then, in bytes. Checked *with* the mtime rather than instead of
  /// it: a save rewritten within the same second by an emulator that restores
  /// mtimes is caught by the size, and a same-size edit is caught by the mtime.
  std::int64_t file_size_bytes = 0;

  /// What the server last said about its own copy. Absent until a sync has
  /// completed for this save, which is the "no sync history" branch M2-8
  /// arbitrates differently (docs/SYNC_PROTOCOL.md).
  ///
  /// **After an upload, `server_updated_at` is absent and
  /// `server_content_hash` is not.** The digest is known -- the server stored
  /// the bytes this client just sent it -- but the row's new timestamp is the
  /// server's own clock, and no response the client reads carries it back
  /// (`sync::OperationResult` keeps the save id and nothing else). It is left
  /// absent rather than guessed from the local mtime: an absent one costs the
  /// next arbitration a fallback, a wrong one costs it the answer. Closing that
  /// means carrying the upload response's `updated_at` up out of
  /// `sync::ExecutePlan`, which is worth doing when something actually reads
  /// this field.
  std::optional<sync::Timestamp> server_updated_at;
  std::optional<std::string> server_content_hash;

  /// RomM deleted this save and `[sync] on_server_delete = keep` left it on the
  /// card (#233). While the file still holds `content_hash`, `sync::RunTick`
  /// does not offer it to the server: RomM's own memory of the deletion is
  /// bounded and does not cover the same bytes rewritten later, so the client
  /// keeps the promise itself. New bytes are new progress and are offered.
  ///
  /// Written as `"deleted_on_server":true` only when set, so every other row is
  /// byte-identical to the ones before it, and read as false when absent.
  bool deleted_on_server = false;
};

/// What one save **state** looked like at the end of the last sync.
///
/// A separate struct rather than a `SaveRecord` with two of its fields left
/// empty, because the two are keyed on different things and mixing them is the
/// mistake the version bump exists to prevent: a state has no slot, and the
/// pairing key RomM actually enforces for one is `(rom_id, file_name)` --
/// verified against a live 5.2.0, docs/API_CONTRACT.md#save-states.
///
/// **A state row is only ever written after a transfer that landed**, so every
/// server field is known and none of them is optional. There is no "the server
/// has never told us about this state" row: with nothing on the server there is
/// nothing to pair against, and the arbitration wants that to be the absence of
/// a row rather than a row with holes in it.
struct StateRecord {
  /// The rom this state belongs to. Positive, like `SaveRecord::rom_id`.
  std::int64_t rom_id = 0;

  /// The pairing key with `rom_id`: the file's own name, `Game (USA).state`.
  ///
  /// The *server's* name and the client's are the same string, which is the one
  /// way states are simpler than saves: RomM renames a save on ingest and does
  /// **not** rename a state (docs/API_CONTRACT.md#save-states). Never a path --
  /// `sync::IsSingleFileName` is the bar.
  std::string file_name;

  /// `retroarch` or `tico`, when the directory said so; empty when it did not.
  ///
  /// Metadata, not part of the key, because RomM's own upsert ignores it: a
  /// second upload of the same `(rom_id, file_name)` under another emulator
  /// **moves** the existing row rather than making a second one. Stored so a
  /// download can be placed back in the directory it came from.
  std::string emulator;

  /// The MD5 the client last computed for the local file: 32 lowercase hex
  /// digits. Local only -- RomM computes no digest for a state, which is why a
  /// downloaded one can be checked against its length and nothing more.
  std::string content_hash;

  /// The local file's mtime when that digest was taken, whole seconds.
  sync::Timestamp mtime{};

  /// Its size then, in bytes. Checked *with* the mtime, for `SaveRecord`'s
  /// reasons.
  std::int64_t file_size_bytes = 0;

  /// The `id` of the server row this local file is paired with. Positive.
  std::int64_t server_state_id = 0;

  /// The server row's `updated_at` at the last sync. This is the whole of the
  /// client's conflict detection for states: with no server arbitration and no
  /// digest, "has the server's copy moved since we agreed?" is this field
  /// compared against the row `GET /api/states` hands back today.
  sync::Timestamp server_updated_at{};

  /// ...and its `file_size_bytes` then, checked alongside for the reason the
  /// local pair is checked together: a same-second rewrite moves one and not the
  /// other.
  std::int64_t server_file_size_bytes = 0;
};

/// The rows, keyed the way the server pairs each kind.
///
/// Saves are keyed `(rom_id, slot)` and `std::optional`'s ordering puts the
/// null-slot row first; states are keyed `(rom_id, file_name)`. Two maps rather
/// than one of a variant, because the two keys are different types and a single
/// map would have to flatten them into a string -- which is the collision the
/// version bump exists to prevent. Both come out in a stable order, so a
/// baseline rewritten with nothing changed produces byte-identical contents.
class Baseline {
 public:
  using Key = std::pair<std::int64_t, std::optional<std::string>>;
  using StateKey = std::pair<std::int64_t, std::string>;

  /// The row for this save, or nullptr when there is none -- which is the
  /// signal to hash the file, not a reason to guess.
  const SaveRecord* Find(std::int64_t rom_id, const std::optional<std::string>& slot) const;

  /// The same for a state, keyed as RomM keys one.
  const StateRecord* FindState(std::int64_t rom_id, std::string_view file_name) const;

  /// Insert or replace the row for `record`'s key.
  void Set(SaveRecord record);
  void SetState(StateRecord record);

  /// Drop the row for a save that is no longer on the card. Returns whether
  /// there was one.
  bool Erase(std::int64_t rom_id, const std::optional<std::string>& slot);
  bool EraseState(std::int64_t rom_id, std::string_view file_name);

  const std::map<Key, SaveRecord>& rows() const { return rows_; }
  const std::map<StateKey, StateRecord>& state_rows() const { return states_; }

  /// Every row of either kind -- what `kMaxRecords` bounds, because it bounds
  /// the file.
  std::size_t size() const { return rows_.size() + states_.size(); }
  bool empty() const { return rows_.empty() && states_.empty(); }

 private:
  std::map<Key, SaveRecord> rows_;
  std::map<StateKey, StateRecord> states_;
};

/// A baseline and everything wrong with the file it came from.
///
/// `value` is always usable: empty is a perfectly good baseline, and it is what
/// every failure here produces. See the header note -- nothing in this module
/// refuses.
struct LoadedBaseline {
  Baseline value;

  /// In the order they were found, bounded by `kMaxDiagnostics`. A first boot
  /// produces exactly one, saying there is no file yet.
  std::vector<std::string> diagnostics;

  /// The version the file was written in: `kFormatVersion`, or
  /// `kSourceSlotVersion` for a file from before #236. Zero when nothing was
  /// read -- no file, or one discarded.
  int version = 0;

  /// A `kSourceSlotVersion` file's slotted save rows, **not** in `value`.
  ///
  /// Their slots are `<source>-<extension>` (`tico-srm`), which no save is
  /// synced in any more, so a row left under one would describe nothing and
  /// cost a row of `kMaxRecords` until it was trimmed. Held apart rather than
  /// discarded, because each is still the digest of a file that is on the card:
  /// `AdoptLegacySaves` moves it to the slot that file is now synced in, and the
  /// first tick after the upgrade hashes nothing it already knew. A caller that
  /// never adopts them loses only that -- every save is hashed once.
  ///
  /// The v2 file's archival (null-slot) rows and its states are in `value` as
  /// they were: neither key moved.
  std::vector<SaveRecord> legacy_saves;

  /// One line per diagnostic, for a log. Empty when there are none.
  std::string DescribeDiagnostics() const;
};

/// The whole file, version line included, ending in a newline.
std::string SerializeBaseline(const Baseline& baseline);

/// Parse the contents of a `state.db`.
///
/// Pure: no filesystem, no clock. **A malformed file yields an empty baseline,
/// not the rows that happened to parse.** A truncation leaves a prefix that is
/// individually well-formed and collectively a lie -- rows written before the
/// interruption, none of the ones after -- and "some rows survived" is a state
/// no caller can reason about. Since dropping the lot costs only a tick of
/// hashing, that is the cheaper mistake to make.
LoadedBaseline ParseBaseline(std::string_view text);

/// Move `loaded->legacy_saves` into `loaded->value`, each under the slot
/// `slot_for` names for its old one -- `scan::MigratedSlot` under the
/// configured slot (#236). Returns how many rows landed.
///
/// **Only the local half moves**: the digest, mtime and size of the file, which
/// are what spare the scan a re-hash. The server half (`server_updated_at`,
/// `server_content_hash`) described the *old* slot's row, and this device has
/// never synced the new one -- carried across, the old digest would let #232's
/// 409 rule read "unchanged since the last sync" against a slot whose head is
/// some other device's progress, and download over a newer local save without a
/// conflict. Left absent, the first meeting is what it is: no sync history.
///
/// **`deleted_on_server` moves with the row** (#233). It records that RomM
/// deleted this save and the card kept it, which is about the save and not the
/// slot: dropped, the next tick would offer the deleted bytes to the shared slot
/// and resurrect there what the user deleted.
///
/// A row whose new key is already taken -- RetroArch's and Tico's `.srm` of one
/// rom, both now `autosave` -- is dropped, the first in key order kept, which is
/// RetroArch's: the copy the scan also reads first. So is one `slot_for` has no
/// slot for. Either costs a re-hash and nothing else.
std::size_t AdoptLegacySaves(
    LoadedBaseline* loaded,
    const std::function<std::optional<std::string>(std::string_view old_slot)>& slot_for);

/// Read `path` and parse it.
///
/// A *missing* file falls back to `io::PreviousPathFor(path)` first, the same
/// recovery `token_store`, `device_identity` and `config` make: the one moment
/// `state.db` legitimately does not exist is the window between
/// `io::WriteAtomically`'s two renames, and the previous baseline is sitting
/// intact under `state.db.old`. A file that exists and will not open does not
/// take it -- that is a bad moment, not a commit window -- and yields an empty
/// baseline, which is safe here in a way it would not be for `device.dat`.
LoadedBaseline LoadBaseline(const std::string& path);

/// Why writing the baseline did not work. Mirrors `auth::StoreError`.
///
/// There is deliberately no "a row was bad" member. A single unusable row is
/// **skipped**, not fatal -- see `SaveBaseline`.
enum class StoreError {
  kNone,
  kTooManyRecords,  ///< more rows than `kMaxRecords`; the reader would discard the file
  kTooLarge,        ///< the serialized file exceeds `kMaxStateBytes`, same reason
  kOpenFailed,      ///< the temp file could not be created -- usually a missing directory
  kWriteFailed,     ///< the bytes did not all reach the disk; the destination is untouched
  kCommitFailed,    ///< the rename onto `path` failed; see atomic_file.hpp
};

/// Stable, log-friendly name. Never null.
const char* ToString(StoreError error);

struct StoreResult {
  StoreError error = StoreError::kNone;

  /// For logs. Names the path and what went wrong.
  std::string message;

  /// How many rows reached the file. Less than `baseline.size()` when rows were
  /// skipped; `0` on any error, because nothing was written.
  std::size_t rows_written = 0;

  /// One line per row that was left out, bounded by `kMaxDiagnostics`. Empty on
  /// the ordinary path. **Not an error** -- see `SaveBaseline`.
  std::vector<std::string> skipped;

  bool ok() const { return error == StoreError::kNone; }
};

/// Write `baseline` to `path`, atomically.
///
/// **A row that could not be read back is skipped, and the rest are written.**
/// The bar is `kMaxRecords`-style validity: a positive `rom_id`, a slot that is
/// either absent or non-empty, digests that are 32 lowercase hex, timestamps a
/// save can actually claim. The reason it is a skip and not a refusal is the
/// module's own framing -- an optimisation, never a gate. A console whose RTC
/// was never set stamps a save with the epoch, which is outside the range;
/// refusing the whole file over it would freeze the baseline at its last good
/// version and re-hash the entire library on every tick from then on, forever,
/// with one log line to explain it. Leaving that one save out costs that one
/// save a re-hash. The skipped rows are named in `skipped`.
///
/// The file-level bounds are refusals, because there is nothing partial to fall
/// back to: more than `kMaxRecords` rows, or more than `kMaxStateBytes` of text,
/// is a file `ParseBaseline` would discard whole. Writing one anyway is the
/// silent version of the same failure -- every later boot finds a `state.db`,
/// throws all of it away, and re-hashes. Better to fail once, loudly, here.
///
/// Writing the baseline at the end of a tick is `sync::FinishTick`'s
/// (sync_finish.hpp), which also decides *which* rows a tick may move; this owns
/// the format.
StoreResult SaveBaseline(const std::string& path, const Baseline& baseline);

/// Why a file could not be hashed.
enum class HashError {
  kNone,
  kUnreadable,  ///< it is not there, or the bytes could not be got out of it
};

/// Stable, log-friendly name. Never null.
const char* ToString(HashError error);

/// A save's `content_hash`, and whether the file had to be read for it.
struct HashOutcome {
  /// 32 lowercase hex digits, empty on failure. Goes into
  /// `ClientSaveState::content_hash` whether it was computed or reused.
  std::string content_hash;

  HashError error = HashError::kNone;

  /// The digest came from the baseline and the file was never opened. What
  /// "skip re-hashing an unchanged file" actually means, and the only way a
  /// caller can tell -- the digest itself is identical either way.
  bool reused = false;

  /// For logs. Names the path and what went wrong.
  std::string message;

  bool ok() const { return error == HashError::kNone; }
};

/// Stream `path` through MD5.
///
/// Chunked rather than read whole: a save state is tens of megabytes and the
/// sysmodule heap is small (core/AGENTS.md). The chunk is 4 KiB and stays that
/// way -- `sys-rommsync.json` gives the main thread a 16 KiB stack, so a buffer
/// sized for a desktop is a stack overflow on the console.
///
/// Unlike `LoadBaseline` this has no size bound -- the file being hashed is the
/// user's save, and refusing to hash a large one would report it with no digest,
/// which is the failure the digest exists to prevent.
HashOutcome HashFile(const std::string& path);

/// The `content_hash` for one scanned save: the stored one when the file is
/// unchanged, a fresh `HashFile` otherwise.
///
/// Unchanged means the baseline has a row for `(rom_id, slot)` and both its
/// `mtime` -- at whole-second granularity, which is all the file holds -- and
/// its `file_size_bytes` match what the scanner just stat'd. Both, because
/// either one alone misses a real edit: an emulator that restores mtimes, and a
/// same-size overwrite within the same second.
HashOutcome ContentHashFor(const Baseline& baseline, std::int64_t rom_id,
                           const std::optional<std::string>& slot, const std::string& path,
                           sync::Timestamp mtime, std::int64_t file_size_bytes);

/// The same for a save state, keyed `(rom_id, file_name)`.
///
/// A state is the file this reuse was written for: tens of megabytes, hashed
/// 4 KiB at a time, and unchanged between most ticks. The rule is `ContentHashFor`'s
/// unchanged -- both the mtime and the size have to still match the row.
HashOutcome ContentHashForState(const Baseline& baseline, std::int64_t rom_id,
                                std::string_view file_name, const std::string& path,
                                sync::Timestamp mtime, std::int64_t file_size_bytes);

/// What a file on the card looks like *right now*: the three fields a baseline
/// row is built from.
struct FileFacts {
  std::string content_hash;
  sync::Timestamp mtime{};
  std::int64_t file_size_bytes = 0;
};

/// Re-stat and re-hash a file whose bytes the engine has just replaced.
///
/// Everything the tick reported about such a file describes bytes that are no
/// longer there, so all three fields come from the card. A row built from the
/// reported ones would claim a digest for bytes nothing ever hashed -- and
/// worse, would claim it against an mtime and a size that no longer match, which
/// is a row that lies and then never gets used.
///
/// False with the reason in `why` when the file is gone, its folder could not be
/// read, its mtime is not one this client can claim, or its bytes would not
/// hash. A caller's answer to that is to *erase* the row rather than keep one
/// describing bytes that are gone.
///
/// `directories` is carried across calls so several files in one folder cost one
/// listing (fs::Directories).
bool ReadBackFile(fs::FileSystem& files, fs::Directories& directories, const std::string& sd_path,
                  FileFacts* out, std::string* why);

}  // namespace rommsync::state
