// Save states, which the server does not arbitrate.
//
// M2's other files are one shape: the client reports what it has, RomM decides
// what should happen to each save, and `sync_execute.hpp` obeys. **None of that
// exists for a state.** `SyncNegotiatePayload` carries only `saves`, `StateSchema`
// has no `content_hash`, no `origin_device_id` and no `device_syncs`, and there
// is no `/api/states/{id}/downloaded` -- so there is no negotiation, no per-device
// history and no digest (docs/API_CONTRACT.md#save-states). Hard rule 3, "the
// server is the source of truth", has nothing to be the source of truth *with*.
//
// What replaces it is a policy the client can defend on its own, and it is the
// conservative one, because a state is a snapshot of an emulator core's memory
// and losing one loses a session:
//
//   - **Overwrite only on an unambiguous baseline match.** A local file is
//     replaced only when `state.db` says which server row it is paired with and
//     that row has not moved since -- and even then the previous bytes go under
//     `.backup/` first, through the same two `io` primitives and in the same
//     order as a save (`sync::BackUpFirst`, `io::CommitStaged`).
//   - **Keep both on anything else.** Two copies this client cannot tell apart
//     stay where they are: the local file is not touched and the server row is
//     not written. That costs disk and a warning; the alternative costs a
//     session.
//   - **Never delete a state**, locally or on the server. `POST /api/states/delete`
//     is not called from here at all.
//
// Two facts about 5.2.0 shape all of it, both verified against a live server and
// neither one true of `/api/saves`:
//
//   - **`POST /api/states` is an upsert keyed on `(rom_id, file_name)` alone.**
//     A second POST with a name the rom already has replaces that row's bytes in
//     place, keeps its id, and *moves* its `emulator` to whatever the new request
//     said. It does not duplicate the way `POST /api/saves` does. So a POST is an
//     overwrite of somebody's state, and this client only ever issues one for a
//     name the rom does not have or for the row its own baseline names.
//   - **RomM does not rename a state on ingest.** A save sent as `probe.srm` is
//     stored as `probe [2026-09-04_11-12-27].srm`; a state sent as
//     `probe (USA).state` is stored as `probe (USA).state`. The server's name and
//     the client's are the same string, which is what makes `(rom_id, file_name)`
//     usable as a pairing key on both sides.
//
// **A downloaded state cannot be verified the way a save is.** RomM computes no
// digest for one, so the only check available is `file_size_bytes` --
// `http::DownloadTarget::expected_size`, which catches the clean short body a
// dropped connection leaves and nothing else. A length match is not an integrity
// check, and this file says so in a warning on every download rather than
// letting a caller read one as though it were the save path's MD5.
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
#include "rommsync/rom_index.hpp"
#include "rommsync/save_scan.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/sync_execute.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::sync {

/// One row of `GET /api/states?rom_id=`, reduced to what arbitration uses.
///
/// `StateSchema` has seventeen fields; these five are the ones a decision is
/// made on. Everything else -- `file_path`, `download_path`, `screenshot` -- is
/// either derivable or a path off the server's disk that this client must never
/// join into one of its own.
struct ServerState {
  std::int64_t id = 0;
  std::int64_t rom_id = 0;

  /// The name the state was uploaded under, unchanged. The pairing key with
  /// `rom_id`; see the header note on ingest.
  std::string file_name;

  /// `null` on the wire when the uploader named none; empty here.
  std::string emulator;

  std::int64_t file_size_bytes = 0;
  Timestamp updated_at{};
};

/// Parse a `StateSchema[]` body.
///
/// Strict in the same way `ParseNegotiateResponse` is: a row missing a field
/// this reads, or carrying one of the wrong type, is a named error and no list
/// -- not a row with a zero in it that later reads as "the server has nothing".
auth::Parsed<std::vector<ServerState>> ParseStateList(std::string_view body);

/// The same for the single `StateSchema` `POST /api/states` answers with.
auth::Parsed<ServerState> ParseState(std::string_view body);

/// What one state was found to need, before anything was done about it.
///
/// Exposed because it is the decision worth testing directly: everything below
/// it is transport, and everything above it is the scan.
enum class StateAction {
  /// Both sides are where the last sync left them.
  kNoOp,

  /// The server has no row with this name for this rom, or has the row this
  /// client's baseline names and nothing else has touched it. Safe to write.
  kUpload,

  /// The baseline names a row, the local file is unchanged, and the row has
  /// moved. The one case a local state may be replaced.
  kDownload,

  /// The server has a row this client has no history for. Placing it is a new
  /// local file, so nothing is overwritten.
  kPlace,

  /// Two copies this client cannot tell apart. Nothing is transferred and
  /// nothing is written; both survive.
  kKeepBoth,
};

/// Stable, log-friendly name. Never null.
const char* ToString(StateAction action);

/// What one state operation did.
enum class StateOutcome {
  kNoOp,
  kUploaded,
  kDownloaded,

  /// `kKeepBoth`, carried out: nothing moved, on purpose. Counted apart from
  /// `completed` because it is work the user may want to finish by hand.
  kKeptBoth,

  kFailed,
  kCanceled,
};

/// Stable, log-friendly name. Never null.
const char* ToString(StateOutcome outcome);

/// Why a state operation did not do what it set out to.
///
/// Split on the same principle as `OperationError`, and for the same reason: a
/// tick has no user to ask, so the remedy has to be in the name.
enum class StateError {
  kNone,

  /// The path is not one this card can resolve, or its bytes could not be read.
  kUnreadableCard,

  /// The previous bytes could not be preserved. **Nothing was overwritten.**
  kBackupFailed,

  /// The exchange never completed: offline, stalled, reset, or a body that
  /// ended early.
  kTransferFailed,

  /// The server answered a status this operation could not proceed from.
  kRefused,

  /// 401 -- the token has been revoked. Stops the rest of the states, for
  /// `OperationError::kUnauthorized`'s reasons.
  kUnauthorized,

  /// 403 -- a scope this pairing was not granted. `/api/states` needs
  /// `assets.read` and `assets.write` (docs/API_CONTRACT.md#scopes-to-request).
  kForbidden,

  /// The downloaded bytes are not the length the server said they were. They
  /// were discarded and the local file was not touched. **This is the only
  /// check a state download gets** -- see the header note.
  kUnverified,

  /// The verified bytes could not be moved onto the state.
  kCommitFailed,

  /// A server state with no local file, and nothing said where to put one.
  /// `StateSyncOptions::place`'s job, and never guessed at.
  kNoPlacement,

  /// The caller's `http::CancelToken` fired.
  kCanceled,
};

/// Stable, log-friendly name. Never null.
const char* ToString(StateError error);

/// What this error says about the credentials, for `auth::Gate`.
auth::Answer AnswerOf(StateError error);

/// One state, and what became of it.
struct StateOperationResult {
  StateAction action = StateAction::kNoOp;
  std::int64_t rom_id = 0;

  /// The pairing key with `rom_id`.
  std::string file_name;

  StateOutcome outcome = StateOutcome::kNoOp;
  StateError error = StateError::kNone;

  /// For the log and for the overlay. Names the rom and the state; never a
  /// token, and never the file's contents.
  std::string message;

  /// The local file this operation read or wrote, SD-root absolute. Empty when
  /// there was none.
  std::string sd_path;

  /// The backup it left behind, SD-root absolute, or empty when it wrote none.
  std::string backup_sd_path;

  /// The server row afterwards.
  std::optional<std::int64_t> state_id;

  /// That row in full, for an upload: the timestamp and size the *next* tick
  /// compares against. Default-constructed unless `outcome` is `kUploaded`.
  ServerState server;
};

/// What the states half of a tick did.
struct StateSyncReport {
  /// **False when `sync.states` is off.** Nothing under `StateScanDirs()` was
  /// read and no request to `/api/states` was made -- which is what "off by
  /// default means silent" is, expressed as something a caller can assert.
  bool ran = false;

  /// The scan this run performed, skips and all. Empty when `ran` is false.
  scan::ScanResult scan;

  std::vector<StateOperationResult> operations;

  /// Operations that did what they set out to, `no_op` included.
  int completed = 0;
  int failed = 0;

  /// Operations resolved keep-both: nothing was transferred and both copies
  /// survive. Neither completed nor failed -- it is the policy working.
  int kept_both = 0;

  /// The `CancelToken` fired: this operation, and every one after it, was not
  /// attempted.
  bool canceled = false;

  /// The server stopped accepting the token part way through.
  bool unauthorized = false;

  /// State rows dropped from the baseline to keep it inside
  /// `state::kMaxRecords`. States are trimmed before saves ever are; see
  /// state_db.hpp.
  std::size_t rows_dropped = 0;

  /// One line per operation that failed or was kept-both, plus the sentence
  /// every download owes: a state is checked against its length and nothing
  /// more. `core/` has no logger, so these are handed up.
  std::vector<std::string> warnings;
};

/// How hard one states run tries, and what it is allowed to touch.
struct StateSyncOptions {
  /// Where backups go, SD-root absolute. Shared with saves on purpose: one
  /// directory is what M7-1 has to show, and `sync::StateBackupDiscriminator`
  /// is what keeps the names apart inside it.
  std::string backup_dir = kBackupDir;

  /// Ceiling on one API call -- the listing, the upload, the row read.
  std::chrono::milliseconds timeout = http::kDefaultTimeout;

  /// What a *content* transfer is judged by instead. A state is tens of
  /// megabytes, and capping the whole exchange is how a legitimately slow
  /// transfer becomes a failed one.
  std::chrono::milliseconds stall_timeout = http::kDefaultStallTimeout;

  /// The clock a backup name is stamped from. Injected for the test that proves
  /// two states backed up in the same second do not collide.
  std::function<Timestamp()> now;

  /// Where a server state the client has no local file for should be written,
  /// SD-root absolute. Returning an empty string -- or leaving this null --
  /// fails that one with `kNoPlacement` rather than guessing.
  ///
  /// Injected for `ExecuteOptions::place`'s reasons: the directory depends on
  /// the rom's platform and on which emulator the state belongs to, and neither
  /// is something this file can work out from the row alone.
  std::function<std::string(const ServerState&)> place;

  /// Optional, not owned; must outlive the call. Checked between states and
  /// passed to every request, so a shutdown ends the run at a boundary rather
  /// than mid-write.
  const http::CancelToken* cancel = nullptr;
};

/// The whole states half of one tick: scan, arbitrate, transfer, record.
///
/// **`config.sync.states` gates all of it.** With the toggle off this returns a
/// report with `ran` false having listed no directory and sent no request; there
/// is deliberately no second entry point that skips the check.
///
/// `baseline` is read for what the last run left and updated in place with what
/// this one did -- state rows only, never a save row. Persisting it is the
/// caller's, with `state::SaveBaseline` or as part of `FinishTick`: this
/// function performs no write to `state.db`, so a states run and a saves run
/// commit one baseline between them rather than racing to write two.
StateSyncReport SyncStates(http::HttpClient& client, fs::FileSystem& files,
                           const auth::StoredToken& token, const config::Config& config,
                           const roms::RomIndex& index, state::Baseline* baseline,
                           const StateSyncOptions& options = {});

}  // namespace rommsync::sync
