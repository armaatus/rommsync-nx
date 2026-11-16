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
// be gone a minute later with nothing left anywhere. A state has no digest on
// the server at all (RomM computes none, state_sync.hpp) and a state upload
// replaces its row, so a state backup is pruned only as a duplicate: the bytes
// it holds are usually progress no other place has, and this module does not
// get to decide they are worthless.
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
};

/// Apply the retention rule to `options.backup_dir`. See the header.
PruneReport Prune(fs::FileSystem& files, const PruneOptions& options);

/// `backup.pruned`'s line: counts and bytes, never a path.
std::string Describe(const PruneReport& report);

/// `GET /api/saves?rom_id=<rom_id>`, reduced to its rows' `content_hash`es.
///
/// Retried with backoff on a transient failure; nullopt for anything that is
/// not a clean, bounded, parseable 2xx -- an unread listing proves nothing.
std::optional<std::vector<std::string>> ListServerSaveDigests(http::HttpClient& client,
                                                              const auth::StoredToken& token,
                                                              std::int64_t rom_id,
                                                              const sync::CallPolicy& policy = {});

}  // namespace rommsync::backup
