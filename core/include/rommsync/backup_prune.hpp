// Keeping `.backup/` bounded without ever deleting the only copy of anything
// (M11-8, #292).
//
// Hard rule 2 writes a backup before every overwrite, and until this nothing
// removed one. Two devices alternating on one game make a backup per download,
// per tick, forever; when the card fills, `sync::BackUpFirst` fails and every
// later download is `kBackupFailed` -- safe, and wedged until the user deletes
// files by hand. This is the retention rule that stops that.
//
// ## The rule
//
// A backup is **retained** while it is one of the newest `keep` of its group --
// one `(rom_id, discriminator)`, the save's slot or the state's name, which is
// exactly what `sync::BackupNameFor` separates -- or younger than `keep_days`.
// Everything else is a candidate, oldest first. Then, while the directory is
// still over `max_bytes`, the retained ones are candidates too, oldest first
// across the whole tree -- **except the newest of each group**, which is the
// one the Conflicts screen is likeliest to be asked to restore.
//
// ## A candidate is removed only when its bytes are somewhere else
//
// Age and count decide what *may* go. Whether it *does* is a digest check, and
// "unsure" always answers keep:
//
//   - **On the server.** The MD5 of the backup is the `content_hash` of a row
//     RomM holds for the same rom (`GET /api/saves?rom_id=`). A slotted upload
//     is a new row, not a replacement (docs/SYNC_PROTOCOL.md), so the bytes a
//     save download replaced are usually still there as an older version -- and
//     the server is the source of truth (hard rule 3). A rom whose listing could
//     not be read proves nothing.
//   - **In a newer backup of the same group that stays.** Two identical copies
//     are one copy too many. Removal is oldest first, so the newest of a run of
//     identical backups is the last to go, and it goes only on the server's
//     word -- which then covers the whole run.
//
// **The live save is deliberately not a second copy.** It is the one file the
// game writes next, so bytes that are "also in the live save" at prune time can
// be gone a minute later with nothing left anywhere.
//
// ## A state's digest is this client's, not RomM's (#330)
//
// RomM computes no digest for a state (state_sync.hpp), so for a state "on the
// server" is a claim this client proves itself. `state.db` already holds it: a
// state row is written only after a transfer that landed, and pairs the MD5 of
// the bytes that crossed -- the scan's digest of an upload, the read-back of a
// download -- with the server row's id, `updated_at` and size at that moment.
//
// **Only a row this tick's states half confirmed is a proof** (`ProvenStates`):
// one it found unmoved on both sides, or one a download just wrote. An upload's
// row carries the *scan's* digest, and a state rewritten at the same size
// between the scan and the upload's read went up as other bytes than the row
// names; the next tick sees the file moved and uploads again, so the row is
// trusted from the tick that finds it settled, never the one that wrote it.
// That also holds the rows to this server: the states half just checked them
// against its listing.
//
// And then only while a fresh `GET /api/states?rom_id=` still lists that row
// unmoved -- `ServerUnmoved`, the test a download is held to -- does the row's
// MD5 stand in for a save's `content_hash` (`StateDigestsOnServer`). A row that
// moved, vanished, or cannot be listed proves nothing, and the backup stays.
// The proof is exactly as strong as that test, which is the states half's own:
// a second console's rewrite in the same second at the same size is invisible
// to both, and a download is checked against its length only.
//
// What that does **not** do is bound a state traded between consoles. `POST
// /api/states` replaces its row in place, so the bytes a state download backed
// up are by construction the ones the server no longer holds: that backup is
// the only copy, and stays. The rule reaches a state backup whose bytes the
// server holds again or still -- the live state a restore from the Conflicts
// screen set aside, a copy of another state of the same game -- and nothing
// else, because nothing else is provably anywhere.
//
// ## When it runs, and what a power cut does to it
//
// The caller runs it at the end of a tick that succeeded, never during an
// operation: `Prune` is the only thing here that removes a backup, and nothing
// else is writing to `.backup/` while it runs (the engine holds its save-write
// lock). Each removal is one `remove()` of a file whose bytes were proven to be
// elsewhere *before* the call, so a cut at any point leaves a subset of the
// removals done and nothing else; a removal that does not survive the cut
// brings back a backup, which is harmless. `prune.crash` replays it.
//
// Hard rule 4: no libnx header. The server is asked through an injected
// callback, so the rule is testable without one.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/state_sync.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/sync_execute.hpp"

namespace rommsync::backup {

/// How many lines one prune may hand up, `sync::kMaxRecoveryWarnings`'s bound
/// and reason.
inline constexpr std::size_t kMaxPruneWarnings = 16;

/// `[backup]`, in the units the prune works in. Defaults are config.hpp's.
struct Retention {
  /// The newest this many of each group are retained. At least one: the
  /// newest of a group is never a count candidate.
  int keep = config::kDefaultBackupKeep;

  /// Anything younger than this many days is retained. Zero retains nothing by
  /// age.
  int keep_days = config::kDefaultBackupKeepDays;

  /// The directory's target size. Zero is no cap.
  std::int64_t max_bytes = std::int64_t{config::kDefaultBackupMaxMib} * 1024 * 1024;

  /// The state budget (#340): the bytes state backups may hold before the
  /// oldest beyond `keep_states` of each state go with no other copy shown.
  /// Zero is off. See the header.
  std::int64_t max_state_bytes = std::int64_t{config::kDefaultBackupMaxStateMib} * 1024 * 1024;

  /// The newest this many of each state the budget never removes. At least one.
  int keep_states = config::kDefaultBackupKeepStates;
};

/// `[backup]` as a `Retention`.
Retention RetentionFor(const config::BackupConfig& backup);

/// A backup's name, read back: `sync::BackupNameFor`'s
/// `<rom_id>-<discriminator>-<unix seconds>[-<uniquifier>]<ext>`.
struct BackupName {
  std::int64_t rom_id = 0;
  std::string discriminator;
  std::int64_t unix_seconds = 0;
  int uniquifier = 0;
};

/// Nullopt for anything `BackupNameFor` would not have written -- a `.tmp` an
/// interrupted copy left, or a file a person put there -- which a prune
/// therefore never touches.
///
/// A discriminator may itself end in `-<digits>` (`slot-5`), so a trailing
/// `-<n>` is read as the uniquifier only when it is short enough to be one
/// (`BackUpFirst` walks below 1000) and follows something long enough to be a
/// timestamp. Reading one the other way puts a backup in the wrong group, which
/// costs a retention count, never a copy: removal is still the digest check's.
std::optional<BackupName> ParseBackupName(std::string_view name);

/// The MD5s RomM holds for one rom's saves, or nullopt when that could not be
/// learnt. Nullopt keeps every candidate of the rom that is not a duplicate.
using ServerDigests = std::function<std::optional<std::vector<std::string>>(std::int64_t rom_id)>;

/// Whether a backup -- its parsed name, and the extension it carries -- is a
/// save state's, which the state budget may remove (#340). False is "a save's,
/// or not known to be a state's", which it never touches.
using IsStateBackup = std::function<bool(const BackupName& name, std::string_view extension)>;

/// The state backups `baseline` names: those under a state row's rom, spelled
/// with `sync::StateBackupDiscriminator` of its name and carrying its name's
/// extension -- exactly what a state download and a state restore write -- and
/// under no save row's discriminator of the same rom. See the header for why
/// the name alone is not enough.
IsStateBackup StateBackupsOf(const state::Baseline& baseline);

struct PruneOptions {
  std::string backup_dir = sync::kBackupDir;
  Retention retention;

  /// The clock "younger than `keep_days`" is measured from. Null is
  /// `std::chrono::system_clock::now`.
  std::function<sync::Timestamp()> now;

  /// Asked once per rom that has a candidate, and only then. Null knows
  /// nothing: only duplicates go.
  ServerDigests server_digests;

  /// The Unix second the console began syncing with the server
  /// `server_digests` asks (`state::Baseline::bound_since`, #294). A backup
  /// named before it was taken against another server -- or one nothing on the
  /// card can name -- under rom ids that mean nothing to this one, so this
  /// server's digests are never its proof: it goes only as a duplicate, and
  /// never asks the server about its rom. Zero trusts the server with every
  /// backup.
  std::int64_t server_since = 0;

  /// Which backups the state budget counts and may remove. Null is none: the
  /// budget never runs, whatever `retention` says. A backup named before
  /// `server_since` is never one -- its rom id is another server's, and its
  /// name proves nothing against this console's `state.db`.
  IsStateBackup is_state_backup;
};

struct PruneReport {
  /// Backups found -- names `ParseBackupName` reads -- and their bytes.
  std::size_t backups = 0;
  std::int64_t bytes_before = 0;

  /// Removed, and what that left.
  std::size_t pruned = 0;
  std::int64_t bytes_after = 0;

  /// Candidates kept because no other copy of their bytes could be shown: on
  /// the server under no rom row this client could read, and in no newer
  /// backup. Includes one that could not be hashed or removed.
  std::size_t kept_unique = 0;

  /// The listing stopped at `fs::kMaxDirectoryEntries`. The last rom it
  /// reached may be cut, so its backups are left out entirely, and the cap is
  /// measured on what was listed.
  bool truncated = false;

  /// The directory could not be read, or a file could not be hashed or
  /// removed. Never fatal: what could not be proven stays.
  std::vector<std::string> warnings;

  /// Still over `max_bytes` after everything provable went.
  bool over_cap = false;

  /// Of `pruned`, state backups the state budget removed with no other copy of
  /// their bytes shown: the one removal that is not a proof (#340).
  std::size_t budget_pruned = 0;

  /// What state backups held after the prune, and whether that is still over
  /// the budget -- the newest `keep_states` of each are never removed.
  std::int64_t state_bytes_after = 0;
  bool over_state_budget = false;
};

/// Apply the retention rule to `options.backup_dir`. See the header.
PruneReport Prune(fs::FileSystem& files, const PruneOptions& options);

/// `backup.pruned`'s line: counts and bytes, never a path.
std::string Describe(const PruneReport& report);

/// `backup.pruned`'s second line, for a prune the state budget removed sole
/// copies in, and empty for one it did not: the one removal a reader of the
/// log has to be able to tell from a proven one (#340).
std::string DescribeStateBudget(const PruneReport& report);

/// `GET /api/saves?rom_id=<rom_id>`, reduced to its rows' `content_hash`es.
///
/// Retried with backoff on a transient failure; nullopt for anything that is
/// not a clean, bounded, parseable 2xx -- an unread listing proves nothing.
std::optional<std::vector<std::string>> ListServerSaveDigests(http::HttpClient& client,
                                                              const auth::StoredToken& token,
                                                              std::int64_t rom_id,
                                                              const sync::CallPolicy& policy = {});

/// The MD5s of `rom_id`'s states that `listed` -- `GET /api/states?rom_id=`'s
/// rows -- proves the server still holds: each state row of `baseline` for the
/// rom whose server row `listed` carries unmoved (`sync::ServerUnmoved`). See
/// the header.
///
/// A row whose local size is not the server's is no proof either: the bytes
/// that crossed are not the bytes it hashed -- a state written between the
/// scan and the upload's read.
std::vector<std::string> StateDigestsOnServer(const state::Baseline& baseline, std::int64_t rom_id,
                                              const std::vector<sync::ServerState>& listed);

/// `GET /api/states?rom_id=<rom_id>`, reduced by `StateDigestsOnServer`.
///
/// Not sent at all, and an empty list, when `baseline` has no state row for
/// the rom: there is nothing it could prove. Otherwise `ListServerSaveDigests`'
/// retry and refusals, at `sync::kMaxStateListBytes`.
std::optional<std::vector<std::string>> ListServerStateDigests(http::HttpClient& client,
                                                               const auth::StoredToken& token,
                                                               std::int64_t rom_id,
                                                               const state::Baseline& baseline,
                                                               const sync::CallPolicy& policy = {});

/// The state rows of `baseline` that `report` -- the states half of the tick
/// that wrote it -- confirmed: unmoved on both sides (`kNoOp`), or just written
/// by a download or a placement (`kDownloaded`). Nothing when it did not run.
/// See the header for why an upload's row waits a tick.
state::Baseline ProvenStates(const state::Baseline& baseline, const sync::StateSyncReport& report);

/// What the engine hands `PruneOptions::server_digests`: a rom's save digests
/// and, against `proven_states` when it is not null, its states'. Each listing
/// proves what it lists, so one that answered is used though the other did
/// not; nullopt only when neither did.
std::optional<std::vector<std::string>> ListProvenServerDigests(
    http::HttpClient& client, const auth::StoredToken& token, std::int64_t rom_id,
    const state::Baseline* proven_states, const sync::CallPolicy& policy = {});

}  // namespace rommsync::backup
