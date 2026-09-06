// Step 2 of docs/SYNC_PROTOCOL.md: doing what the plan said.
//
// `sync.hpp` asks the server what should happen to each save and reads the
// answer strictly (M2-4). This executes it -- an upload, a download, a
// keep-both resolution, or nothing -- and it is the file that carries the
// project's one hard guarantee: **the bytes a save held before this code
// replaced them are under `.backup/` first, or they were not replaced.**
//
// The plan arrives already typed. Nothing here re-parses JSON that
// `ParseNegotiateResponse` has already read, and nothing here second-guesses
// the server: the client obeys `action` and invents no resolution of its own
// (docs/SYNC_PROTOCOL.md#principle-the-server-is-the-source-of-truth).
//
// Four things about executing a plan are not what an obvious reading produces,
// and each one is a save:
//
//   - **The operation's `file_name` is the server's.** RomM renames a save on
//     ingest -- `probe.srm` is stored as `probe [2026-09-04_11-12-27].srm` --
//     and every later operation echoes that. Writing it to the SD produces a
//     file no emulator loads, so an operation is matched back to a local file
//     on `(rom_id, slot)` (`MatchTarget`) and the client's own path is kept.
//     Nothing in this header joins a server string into a path.
//   - **A download may not be pointed at the save.** `http::DownloadTarget`
//     renames its `.part` onto the destination the moment the body ends, which
//     is before anything has verified that the bytes are the save the plan
//     meant. So a download stages at `io::TempPathFor(<save>)`, is verified
//     against `server_content_hash`, and only then displaces the file -- after
//     the backup.
//   - **`overwrite=true` is not optional on an upload.** Without it RomM
//     answers `409 Slot has a newer save since your last sync` for exactly the
//     no-sync-history case the plan just told the client to upload, so a plan
//     executed as issued would be refused by the server that issued it.
//   - **A download that is never confirmed did not happen, as far as
//     arbitration is concerned.** `POST /api/saves/{id}/downloaded` is what
//     writes this device's sync row; skip it and every later negotiation for
//     that save stays in the no-history branch.
//
// There is no retry inside a tick, deliberately, and for two reasons that have
// both been written down wrong at some point. **A re-posted upload does
// duplicate the save**, `overwrite=true` included: RomM matches an existing
// slot row by a second-granularity datetime tag it computes at ingest, so a
// retry a second later gets a name that matches nothing and becomes a second
// row (docs/API_CONTRACT.md, verified against a live 5.2.0; the revision of this
// comment that said otherwise was reading a same-second run, which is why
// `execute.occupied` passed on a laptop and failed in CI -- issue #85). And the
// plan describes a state the server may have moved on from, so the arbiter of
// that is a fresh negotiation rather than this client -- which is what
// docs/SYNC_PROTOCOL.md's failure rules already say: count the operation
// failed, leave that save alone, let the next tick negotiate again.
// `sync_tick.hpp` owns the order of one tick and M7-2 the schedule between
// them.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::sync {

/// Where a save's previous bytes go, SD-root absolute
/// (docs/ARCHITECTURE.md puts the client's own state under `/config/rommsync`).
///
/// The directory has to exist already. `core/` cannot create one with only
/// standard headers -- that is the platform layer's job, the same rule
/// `io::WriteAtomically` states -- and a missing one fails the operation
/// *before* the save is touched, which is the right direction to fail in: no
/// backup, no overwrite.
inline constexpr const char* kBackupDir = "/config/rommsync/.backup";

/// The name of the backup for one save:
/// `<rom_id>-<slot>-<unix seconds>[-<uniquifier>].<ext>`.
///
/// **The slot is in it because M2-5 found the documented scheme unsafe.**
/// `<rom_id>-<ts>.<ext>` carries neither the slot nor the save's own name, so
/// two saves of one rom backed up in the same second -- one rom with two slots,
/// or a save and its state -- are the same file, and the second backup destroys
/// the first. `(rom_id, slot)` is exactly the pair RomM keys a save on, so it is
/// exactly the pair that separates two of them.
///
/// The slot is reduced to `[A-Za-z0-9._-]`, with every other byte becoming `_`.
/// The slots this client derives are already that shape (`retroarch-srm`), but
/// the one on an operation is whatever some other client chose, and a `/` in it
/// would put the backup somewhere other than `.backup/`. A `null` slot -- an
/// archival save -- spells itself `archival`, which is a slot no derived name
/// produces because `SlotFor` always carries an emulator and an extension.
///
/// `uniquifier` is appended from 1 upwards, for the caller that finds the name
/// already taken. `ExecutePlan` walks it rather than overwriting: a name that is
/// already there is a backup of *something*, and this module does not get to
/// decide it is worthless.
std::string BackupFileName(std::int64_t rom_id, const std::optional<std::string>& slot,
                           std::string_view file_name, std::int64_t unix_seconds,
                           int uniquifier = 0);

/// One local save an operation may act on.
///
/// Produced from a `scan::SaveFile` by the caller -- the scan is step 0 and
/// already holds all four fields. It is a separate struct so the executor
/// depends on the four things it needs rather than on the scanner.
struct SaveTarget {
  std::int64_t rom_id = 0;

  /// The pairing key with `rom_id`, as the server pairs them. An empty optional
  /// is an archival save and pairs only with another archival one.
  std::optional<std::string> slot;

  /// Where the save is, SD-root absolute (`/retroarch/saves/Game.srm`).
  /// `fs::FileSystem::Resolve` turns it into something that can be opened.
  std::string sd_path;

  /// The client's own name for it, `Game (USA).srm`. Used for the backup's
  /// extension and as the name an upload reports; never the server's.
  std::string file_name;
};

/// The target for `operation`, matched on `(rom_id, slot)`, or nullptr.
///
/// Never on `file_name`: that one is the server's (see the header note).
const SaveTarget* MatchTarget(const std::vector<SaveTarget>& targets,
                              const SyncOperation& operation);

/// What one operation did.
enum class OperationOutcome {
  /// The plan said nothing needed doing, and nothing was done.
  kNoOp,
  kUploaded,
  kDownloaded,

  /// A conflict, resolved keep-both: the local bytes are under `.backup/` and
  /// the server's copy is on the card.
  kKeptBoth,

  kFailed,

  /// The action was one this build does not recognise, so M2-4 delivered it as
  /// a `no_op` with `known_action` false. Not a completed no-op and not a
  /// failure: nothing was done, and the client does not know what should have
  /// been.
  kNotUnderstood,

  /// The caller's `http::CancelToken` fired. Also neither completed nor failed:
  /// the operation was stopped rather than attempted, and reporting a shutdown
  /// to `complete` as a failed operation would describe work that went wrong.
  kCanceled,
};

/// Stable, log-friendly name. Never null.
const char* ToString(OperationOutcome outcome);

/// Why an operation did not do what the plan asked.
///
/// Split this finely because the remedies differ and a sync tick has no user to
/// ask: `kBackupFailed` is a card or a missing directory and the save was *not*
/// touched, `kUnverified` is bytes that were not the save and were discarded,
/// and `kRefused` is the server declining something it planned.
enum class OperationError {
  kNone,

  /// An `upload` with no local file to send, or a `download` with nowhere to
  /// put one. The second is a save the client has never seen: placing it needs
  /// the rom's platform folder and a name the server cannot supply, which is
  /// `ExecuteOptions::place`'s job.
  kNoLocalSave,

  /// A `download` or `conflict` whose operation carries no `save_id`. The
  /// server has no copy, so there is nothing to fetch -- a plan contradicting
  /// itself rather than a failure of this client.
  kNoSaveId,

  /// The path is not one this card can resolve, or its bytes could not be read.
  kUnreadableCard,

  /// The previous bytes could not be preserved. **Nothing was overwritten** --
  /// that is what this error means, and it is the reason it is separate.
  kBackupFailed,

  /// The exchange never completed: offline, stalled, reset, or a body that
  /// ended early.
  kTransferFailed,

  /// The server answered a status this operation could not proceed from. A
  /// `409` on an upload is the one worth recognising: it means the request went
  /// out without `overwrite=true`.
  kRefused,

  /// 401 -- the server has stopped accepting this console's token.
  ///
  /// Not this operation's problem, and kept apart from `kRefused` because of
  /// what it does to the ones after it: every remaining operation would be
  /// refused the same way, so `ExecutePlan` **stops** rather than spending a
  /// whole plan's worth of requests on a token that is gone -- the call
  /// `download::Drain` already makes for its queue. `expires_at` is null on
  /// 5.2.0, so there is nothing to refresh and nothing that starts working again
  /// on its own (docs/AUTH.md#re-pairing--revocation).
  kUnauthorized,

  /// 403 -- the pairing is real and was not granted what this operation needs.
  ///
  /// The same stop, and deliberately not the same sentence: RomM approves what
  /// the *user* ticked, so this is a missing scope rather than a revocation
  /// (docs/AUTH.md#scopes).
  kForbidden,

  /// The downloaded bytes are not the save the plan described. They were
  /// discarded and the local file was not touched.
  kUnverified,

  /// The verified bytes could not be moved onto the save. `io::PreviousPathFor`
  /// and the backup are both worth looking at; the message says which.
  kCommitFailed,

  /// The bytes are on the card, and `POST /api/saves/{id}/downloaded` did not
  /// record that. The save is correct locally; the server still thinks this
  /// device has never seen it.
  kUnconfirmed,

  /// The caller's `http::CancelToken` fired. Nothing after this operation ran.
  kCanceled,
};

/// Stable, log-friendly name. Never null.
const char* ToString(OperationError error);

/// What this error says about the credentials, for `auth::Gate`. The same
/// question `sync::AnswerOf(NegotiateError)` answers, over one operation.
auth::Answer AnswerOf(OperationError error);

/// One operation, and what became of it.
struct OperationResult {
  Action action = Action::kNoOp;
  std::int64_t rom_id = 0;
  std::optional<std::string> slot;

  OperationOutcome outcome = OperationOutcome::kNoOp;
  OperationError error = OperationError::kNone;

  /// For the log and for the overlay. Names the rom, the slot and what
  /// happened; never a token, and never the save's contents.
  std::string message;

  /// The local save this operation read or wrote, SD-root absolute. Empty when
  /// there was none to name.
  std::string sd_path;

  /// The backup this operation left behind, SD-root absolute, or empty when it
  /// wrote none. On a `conflict` this is the copy the overlay surfaces (M7-1).
  std::string backup_sd_path;

  /// The server's save row afterwards: the one that was downloaded, or the new
  /// one an upload created. Empty when the operation never got that far.
  std::optional<std::int64_t> save_id;
};

/// What a whole plan did. The counts are what M2-6 reports to
/// `POST /api/sync/sessions/{id}/complete`.
struct ExecutionReport {
  /// One per operation in the plan, in the plan's order, up to the point a
  /// cancellation stopped it.
  std::vector<OperationResult> operations;

  /// Operations that did what the plan asked, `no_op` included -- RomM's own
  /// `operations_planned` counts only the ones that need work, so a plan of
  /// nothing but no-ops completing more than were planned is normal
  /// (docs/API_CONTRACT.md).
  int completed = 0;

  int failed = 0;

  /// Operations downgraded by the parse: an `action` this build does not know.
  /// Neither completed nor failed *here*, because they are a different thing to
  /// say -- but they are work the server planned and the client did not do, so
  /// a caller reporting to `complete` counts them with `failed` rather than
  /// letting them vanish (docs/SYNC_PROTOCOL.md step 3).
  int not_understood = 0;

  /// The `CancelToken` fired: this operation, and every one after it, was not
  /// attempted. Counted in none of the three totals above, so
  /// `completed + failed + not_understood` is short of `operations.size()`
  /// exactly when this is set.
  bool canceled = false;

  /// The server stopped accepting the token part way through, so the operations
  /// after that one were not attempted.
  ///
  /// **Unlike `canceled`, the operation that met it is counted `failed`**: it
  /// was attempted and it did not happen, and that is what keeps the
  /// `operations_failed` this tick reports to `complete` honest (M2-6). The ones
  /// after it are in no total, because they were never tried.
  ///
  /// A caller feeds `AnswerOf(OperationError)` for the last operation to
  /// `auth::Gate`; the flag is here so it does not have to search the vector for
  /// the reason the tick ended early.
  bool unauthorized = false;

  /// One line per operation that failed or was not understood, plus anything a
  /// completed one is worth saying out loud -- a download that could not be
  /// verified because the server holds no usable digest for it.
  ///
  /// `core/` has no logger (docs/ARCHITECTURE.md), so these are handed up the
  /// same way `SyncPlan::warnings` are.
  std::vector<std::string> warnings;
};

/// How hard one execution tries, and what it is allowed to touch.
struct ExecuteOptions {
  /// Where backups go, SD-root absolute. Overridable so a test can look at one
  /// somewhere other than the card's real config directory.
  std::string backup_dir = kBackupDir;

  /// Ceiling on one API call -- the upload, the preflight, the confirmation.
  std::chrono::milliseconds timeout = http::kDefaultTimeout;

  /// What a *content* transfer is judged by instead. A save is small, but a
  /// save state is not, and capping the whole exchange is how a legitimately
  /// slow transfer becomes a failed one (`http::Request::stall_timeout`).
  std::chrono::milliseconds stall_timeout = http::kDefaultStallTimeout;

  /// The clock the backup name is stamped from. Injected for the test that
  /// proves two saves backed up in the same second do not collide, which cannot
  /// be written against a clock that moves.
  ///
  /// Null means `std::chrono::system_clock::now`.
  std::function<Timestamp()> now;

  /// Where a save the client has no local file for should be written, given the
  /// operation. Returning an empty string -- or leaving this null -- fails that
  /// operation with `kNoLocalSave` rather than guessing.
  ///
  /// It is injected because the answer is not in the plan and not on the card:
  /// the server's `file_name` is tagged and the directory depends on the rom's
  /// platform, so placing a save needs the rom index and the platform folder map
  /// (M3-1). Guessing at it would write a file no emulator loads, under a name
  /// the next tick would upload back to the server as a second save.
  std::function<std::string(const SyncOperation&)> place;

  /// Optional, not owned; must outlive the call. Checked between operations and
  /// passed to every request, so a shutdown ends the tick at an operation
  /// boundary rather than mid-write.
  const http::CancelToken* cancel = nullptr;
};

/// Execute `plan`, one operation at a time, in the order the server sent them.
///
/// `targets` is what the client has locally, matched on `(rom_id, slot)`.
/// `token` supplies the origin, the bearer token and the `device_id` every
/// upload and confirmation carries -- without that id nothing writes the sync
/// history the *next* negotiation arbitrates against.
///
/// One failed operation does not abandon the rest: each is isolated, counted,
/// and left for the next tick. Nothing here deletes a save, and nothing here
/// resolves a conflict the server did not report.
///
/// The two exceptions are the two failures that are not about one operation: a
/// cancellation, and a 401 or 403 (`ExecutionReport::unauthorized`). Both stop
/// the plan where it stands, because everything after them would end the same
/// way -- and on a battery, twenty requests to prove that is nineteen too many.
ExecutionReport ExecutePlan(http::HttpClient& client, fs::FileSystem& files,
                            const auth::StoredToken& token, const SyncPlan& plan,
                            const std::vector<SaveTarget>& targets,
                            const ExecuteOptions& options = {});

}  // namespace rommsync::sync
