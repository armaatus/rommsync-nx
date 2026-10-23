// Step 2 of docs/SYNC_PROTOCOL.md: doing what the plan said.
//
// `sync.hpp` asks the server what should happen to each save and reads the
// answer strictly (M2-4). This executes it -- an upload, a download, a
// keep-both resolution, 5.4.0's `delete`, or nothing -- and it is the file that
// carries the project's one hard guarantee: **the bytes a save held before this
// code replaced or removed them are under `.backup/` first, or they were not
// replaced or removed.**
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
//   - **An upload goes out with `overwrite=false`, and a plan executed as
//     issued can be refused by the server that issued it.** RomM answers
//     `409 Slot has a newer save since your last sync` whenever the slot holds
//     a save this device has not synced -- another device's, written before the
//     plan (on 5.4.0 every other device's upload is a new head row, and
//     negotiate then plans `upload` / no sync history) or since. That 409 is
//     the protection a shared slot needs, so it is kept rather than lifted:
//     the upload is `OperationError::kNewerOnServer` here, and the tick
//     resolves it against the server's head (sync_resolve.hpp, #232). Only a
//     player's explicit "Keep local" sends `overwrite=true`
//     (`ExecuteOptions::keep_local`).
//   - **A download that is never confirmed did not happen, as far as
//     arbitration is concerned.** `POST /api/saves/{id}/downloaded` is what
//     writes this device's sync row; skip it and every later negotiation for
//     that save stays in the no-history branch.
//
// There is no retry inside a tick, deliberately, and for two reasons that have
// both been written down wrong at some point. **A re-posted upload does
// duplicate the save**, whatever `overwrite` says: RomM matches an existing
// slot row by a second-granularity datetime tag it computes at ingest, so a
// retry a second later gets a name that matches nothing and becomes a second
// row (docs/API_CONTRACT.md, verified against a live 5.2.0; the revision of this
// comment that said otherwise was reading a same-second run, which is why
// `execute.occupied` passed on a laptop and failed in CI -- issue #85). And the
// plan describes a state the server may have moved on from, so the arbiter of
// that is a fresh negotiation rather than this client -- which is what
// docs/SYNC_PROTOCOL.md's failure rules already say: count the operation
// failed, leave that save alone, let the next tick negotiate again. The one
// failure a tick does negotiate again for itself, once, is the slot guard's 409
// (sync_resolve.hpp) -- and what it runs then is never another upload.
// `sync_tick.hpp` owns the order of one tick and M7-2 the schedule between
// them.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/placement.hpp"
#include "rommsync/state_db.hpp"
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
/// The slots this client derives are already that shape (`autosave`,
/// `autosave-rtc`), but the one on an operation is whatever some other client
/// chose, and a `/` in it would put the backup somewhere other than `.backup/`. A
/// `null` slot -- an archival save -- spells itself `archival`, which only a
/// `[sync] slot = archival` would also produce; two such backups in one second
/// still never share a name, by `BackUpFirst`'s uniquifier.
///
/// `uniquifier` is appended from 1 upwards, for the caller that finds the name
/// already taken. `ExecutePlan` walks it rather than overwriting: a name that is
/// already there is a backup of *something*, and this module does not get to
/// decide it is worthless.
std::string BackupFileName(std::int64_t rom_id, const std::optional<std::string>& slot,
                           std::string_view file_name, std::int64_t unix_seconds,
                           int uniquifier = 0);

/// The same name built from a discriminator that is already a string.
///
/// The middle segment of a backup name exists to separate two files of one rom
/// backed up in the same second, and for a save that separator is its slot. A
/// **state has no slot**: RomM keys one `(rom_id, file_name)` and gives it no
/// slot at all, so passing `std::nullopt` above would spell `archival` for every
/// state of one rom and leave only the uniquifier -- `-1`, `-2` -- to tell them
/// apart, which makes the backups indistinguishable to whatever has to show them
/// to a user (M7-1). `StateBackupDiscriminator` supplies one built from the
/// state's own name instead, on exactly the reasoning that put the slot there.
std::string BackupNameFor(std::int64_t rom_id, std::string_view discriminator,
                          std::string_view file_name, std::int64_t unix_seconds,
                          int uniquifier = 0);

/// A save's discriminator: its sanitised slot, or `archival` for a null one.
/// Exposed alongside the state's so the two spellings of "what separates two
/// files of one rom" sit next to each other.
std::string SlotBackupDiscriminator(const std::optional<std::string>& slot);

/// `state-<name>`, reduced to `[A-Za-z0-9._-]` and bounded, for a state's
/// backup name.
///
/// The `state-` prefix is not a guarantee of uniqueness against a save's slot --
/// a slot on an operation is whatever some other client chose, so no literal is
/// impossible there -- it is a label that makes a `.backup/` listing readable.
/// The guarantee is `BackUpFirst`'s uniquifier walk, which never writes over a
/// name that is already taken.
std::string StateBackupDiscriminator(std::string_view file_name);


/// The name a conflict's losing local copy is uploaded under (#240):
/// `<stem> [switch-conflict <YYYY-MM-DD_HH-MM-SS>].<ext>`, from the client's own
/// `file_name` and the conflict's UTC second.
///
/// The second is in it because RomM tags no `null`-slot upload: it stores the
/// name as sent, and a second upload of an existing name *replaces that row's
/// bytes*. Without it a rom's next conflict would overwrite the copy the last
/// one kept -- the loss this upload exists to prevent.
std::string ArchiveFileName(std::string_view file_name, std::int64_t unix_seconds);

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

  /// The name an upload reports, and the source of the backup's extension. The
  /// engine passes `scan::SaveFile::server_name` -- `<rom fs_name_no_ext><ext>`,
  /// the stem RomM serves RetroArch the save under (#236) -- which is the
  /// client's to choose; never the server's tagged one.
  std::string file_name;
};

/// The pair RomM keys a save on, from anything that names one: a reported save,
/// a target, an operation or its result.
template <typename Save>
state::Baseline::Key KeyOf(const Save& save) {
  return {save.rom_id, save.slot};
}

/// Whether `save` is the one `key` names. A key is compared as itself, so a
/// list of keys (`ExecuteOptions::keep_local`) is searched like any other.
template <typename Save>
bool HasKey(const Save& save, const state::Baseline::Key& key) {
  return save.rom_id == key.first && save.slot == key.second;
}
inline bool HasKey(const state::Baseline::Key& entry, const state::Baseline::Key& key) {
  return entry == key;
}

/// The first of `saves` keyed `key`, or nullptr: **the** `(rom_id, slot)`
/// lookup, which every step of a tick that pairs one list with another goes
/// through (#259). A scan of at most `kMaxPlanOperations` entries, which builds
/// nothing on the heap.
template <typename Save>
const Save* FindByKey(const std::vector<Save>& saves, const state::Baseline::Key& key) {
  for (const Save& save : saves) {
    if (HasKey(save, key)) {
      return &save;
    }
  }
  return nullptr;
}

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

  /// A `delete` (#233), obeyed: the save's bytes are under `.backup/` and the
  /// file is off the card -- or it was already gone, and `backup_sd_path` is
  /// empty because there was nothing to protect.
  kRemoved,

  /// A `delete` under `[sync] on_server_delete = keep`: the file was left
  /// exactly as it was, and its baseline row is marked so the next tick does
  /// not offer those bytes again (`state::SaveRecord::deleted_on_server`).
  kKept,

  /// The server's copy is one this console will not take, for a reason only a
  /// file moving can change (#257): a pairing `scan::Interchangeable` refuses
  /// (`OperationError::kOtherEmulator`), or a first-time restore with nowhere to
  /// go -- no placement, or one onto a file already there
  /// (`OperationError::kNoLocalSave`). `error` says which. **Nothing was fetched
  /// and nothing local was touched.**
  ///
  /// Neither completed nor failed here, because neither is true: nothing went
  /// wrong, and asking again does not change it. Counted failed, it made every
  /// tick `kPartial` for as long as the two saves stayed where they were, which
  /// reads as "something is wrong" when nothing can be done from the console.
  /// RomM is still told it did not happen (`CountsFor`), as it is of an action
  /// this build does not know. Its line is said once (`Declines`).
  kDeclined,
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
  /// `ExecuteOptions::place`'s job -- and a placement that names a file already
  /// on the card has nowhere to put it either. Nothing was written.
  ///
  /// The download's two are `OperationOutcome::kDeclined` (#257): the plan asks
  /// for them every tick until a file moves or the placement's reason does. An
  /// upload or a `delete` with no local save is still a failure.
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

  /// The server answered a status this operation could not proceed from.
  kRefused,

  /// `409 Slot has a newer save since your last sync` on an upload: RomM's slot
  /// guard (#232). The slot holds a save this device has not synced -- another
  /// device's upload, or RetroArch's, since this device last synced it -- and
  /// uploading over it would supersede that save without anyone choosing to.
  /// **Nothing was uploaded and nothing local was touched.**
  ///
  /// Kept apart from `kRefused` because it is the one refusal with a remedy the
  /// tick applies itself: `RunTick` asks the server again which copy is the head
  /// and takes it -- a download, or a conflict -- and never re-posts the upload
  /// (sync_resolve.hpp). Here, inside one plan, it is only named.
  kNewerOnServer,

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

  /// The bytes are not the save the plan described: a download that is not the
  /// server's copy, or a save a `delete` names that changed after the scan
  /// reported it. Either way they were discarded and the local file was not
  /// touched.
  ///
  /// Also a save written *after its backup* (#263, `UnchangedSinceBackup`): the
  /// replacement or removal stood down, and the save and its backup are both
  /// left on the card.
  kUnverified,

  /// A `download` or `conflict` whose server copy was written by an emulator
  /// whose save format the local file's does not share (`scan::Interchangeable`,
  /// #238): a Beetle `.srm` for DuckStation's card, a `tico-*` save for a
  /// RetroArch folder, or an isolated format placed where nothing says who reads
  /// it. Refused before anything is fetched; **nothing local was touched**.
  ///
  /// RomM pairs on `(rom_id, slot)` whatever wrote the save, so this is the
  /// client's own guard behind the `emulators` filter rather than a server
  /// fault. The plan asks for it every tick until one of the two saves moves,
  /// so it is `OperationOutcome::kDeclined` rather than a failure (#257).
  kOtherEmulator,

  /// The verified bytes could not be moved onto the save, or a save a `delete`
  /// names could not be removed. `io::PreviousPathFor` and the backup are both
  /// worth looking at; the message says which.
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

/// Copy whatever is at `sd_path` into `backup_dir` under a name built from
/// `discriminator`, and say where it went. **Step 3 of the four**.
///
/// Shared by saves and states rather than spelled twice, because it is the step
/// docs/SYNC_PROTOCOL.md's hard rule is about: streamed with `io::CopyAtomically`
/// (the file may be tens of megabytes and the sysmodule heap is 1.5 MiB), and
/// stepping past a name that is already taken rather than over it -- a name that
/// is there is a backup of *something*, and this does not get to decide it is
/// worthless.
///
/// `OperationError::kNone` with `*backup_sd_path` left empty is success with
/// nothing to show for it: there is no file at `sd_path`, so the write that
/// follows overwrites nothing and there is nothing to protect. Every other
/// failure is a refusal to go on -- including a missing `backup_dir`, which
/// `core/` cannot create and which therefore stops the overwrite instead of
/// proceeding without a copy.
///
/// **A backup counts once it would survive a power cut, not once it is
/// written.** The copy's bytes are synced before its rename and its directory
/// after it (`io::CommitStaged`); a card that will not confirm that directory is
/// `kBackupFailed`, and the copy is removed. The overwrite that follows lands in
/// a different directory, and without this a power cut could keep the
/// overwrite and lose the backup's name -- the ordering property
/// docs/SYNC_PROTOCOL.md#backups states, and tests/crash_disk.hpp replays.
OperationError BackUpFirst(fs::FileSystem& files, const std::string& backup_dir,
                           const std::string& sd_path, std::int64_t rom_id,
                           std::string_view discriminator, std::string_view file_name,
                           std::int64_t unix_seconds, std::string* backup_sd_path,
                           std::string* message);

/// Whether the file at `sd_path` is still what `BackUpFirst` found there, asked
/// immediately before the call that replaces or removes it (#263).
///
/// A backup is a copy of the bytes a save held *when it was copied*. Anything
/// that writes the save after that -- a game launched mid-tick, which #242's
/// hold is asked too coarsely to stop -- is in no copy anywhere, and the commit
/// or removal that follows would take it. So the save is compared with its
/// backup by digest, and anything but a match stands the operation down with
/// both files left where they are. A digest because a size-and-mtime check
/// misses a same-size rewrite inside FAT32's two-second mtime.
///
/// Every overwrite asks it: `ExecutePlan`'s three actions, a state download
/// (`state_sync.cpp`) and a conflict restore (`conflicts::Restore`), the last
/// two since #270. Both files are streamed through `state::HashFile`, so a
/// state's tens of megabytes cost two reads of the card and no heap.
///
///   - `backup_sd_path` empty means `BackUpFirst` found nothing to protect, so
///     there must still be nothing -- a file that appeared since has no copy.
///     Only ENOENT or ENOTDIR is nothing, for `BackUpFirst`'s reason.
///   - A save that differs, or that cannot be read back, is `kUnverified`.
///   - A backup that cannot be read back is `kUnreadableCard`.
///
/// **It narrows the window to the instant between this read and the next call;
/// it does not close it.** Horizon offers no compare-and-swap on a file, and a
/// write landing in that instant is the residual risk
/// docs/SYNC_PROTOCOL.md#backups states.
OperationError UnchangedSinceBackup(fs::FileSystem& files, const std::string& sd_path,
                                    const std::string& backup_sd_path, std::string* message);

/// Whether a conflict's losing local copy is on the server too (#240).
///
/// Keep both writes the server's copy to the card and leaves the local bytes in
/// `.backup/` -- on one SD card, where no other device can recover them. So
/// before the server's copy lands, those bytes go up as an **archival** save:
/// `null` slot, the local emulator id, `ArchiveFileName`. RomM never pairs a
/// `null`-slot row in negotiate and never prunes one, so it cannot come back
/// into sync, and the server's copy still wins on the card (hard rule 3).
enum class ArchiveOutcome {
  /// Nothing to keep: not a conflict, or a conflict that replaced no local file.
  kNone,

  /// The local bytes are on the server as `OperationResult::archive_save_id`.
  kKept,

  /// The upload did not happen. The conflict was still resolved, and the local
  /// bytes are **only** under `.backup/` on this card; the overlay says so.
  kFailed,
};

/// Stable, log-friendly name. Never null.
const char* ToString(ArchiveOutcome outcome);

/// `OperationResult::warning` for an operation that wrote no line of its own.
inline constexpr std::int32_t kNoWarning = -1;

/// One operation, and what became of it.
struct OperationResult {
  Action action = Action::kNoOp;

  /// Where in `ExecutionReport::warnings` the line `ExecutePlan` wrote for this
  /// operation is -- a failure's, or an action not understood -- or
  /// `kNoWarning`. The line's identity, so the one thing that takes a line back
  /// out (a 409 a resolution then acted on, sync_resolve.hpp) does it by
  /// position rather than by matching a sentence that can be reworded (#259).
  ///
  /// 32 bits and here, beside `action`, because that is padding the struct
  /// already had: `sysmodule/source/main.cpp` pins its size, and the per-save
  /// heap figure measured on it.
  std::int32_t warning = kNoWarning;

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

  /// On a `conflict`: whether the bytes in `backup_sd_path` are on the server
  /// too, and as which row. `kNone` on every other action.
  ArchiveOutcome archive = ArchiveOutcome::kNone;
  std::optional<std::int64_t> archive_save_id;
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

  /// Operations declined (`OperationOutcome::kDeclined`, #257): a save this
  /// console will not take until a file moves. Not a failure here; RomM's count
  /// takes them with `failed` (`CountsFor`), for `not_understood`'s reason.
  int declined = 0;

  /// The `CancelToken` fired: this operation, and every one after it, was not
  /// attempted. Counted in none of the four totals above, so
  /// `completed + failed + not_understood + declined` is short of
  /// `operations.size()` exactly when this is set.
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
  /// Handed up rather than written down here, the same way `SyncPlan::warnings`
  /// are. There is a logger since M7-3 (`rommsync/log.hpp`) and this module
  /// still does not use it: what a line is worth saying about depends on the
  /// tick around it -- `SdEngine::RunOneTick` writes these under
  /// `log::Event::kSaveFailed`, and a unit test of one execution wants them as
  /// data.
  std::vector<std::string> warnings;

  /// One line per declined operation that is news (`Declines::Note`): the
  /// first time it is met, or met again after either copy changed. The line is
  /// `declined ` and the operation's message. Apart from
  /// `warnings` because a decline is not a failed save, and the engine writes
  /// those as one (#257). Nothing takes one of these back out, so they carry no
  /// index the way `OperationResult::warning` does.
  std::vector<std::string> declines;
};

/// The declines a console has already said, so each costs one line rather than
/// one every tick (#257).
///
/// Remembered per `(rom_id, slot)`, as a digest of everything that would make
/// the decline a different one: the server's save id, its timestamp and digest,
/// the emulator that wrote it, the digest step 0 reported for the local save,
/// and the operation's message -- which names the action, the local path, what
/// it is read as, and the placement's reason. So a new server save, the local
/// file moving or being played on, or the placement's answer changing is said
/// again, and the same decline met again is not. Played on matters: a card the
/// player keeps writing is an upload RomM's slot guard refuses and #232 resolves
/// into a conflict this console declines, so its progress is going nowhere --
/// said once per change rather than once.
///
/// **In memory, for the boot**: a reboot says each live decline once more, which
/// is cheaper than a row of `state.db` -- bounded in records and bytes, and for
/// saves on the card, which a placement's decline is not. Bounded at
/// `kMaxDeclines` and swept by `EndTick`, so what it holds is what is declined
/// now; one met past the bound is not remembered, and is said every tick.
///
/// Not thread-safe: the engine's tick is its one user, on the worker.
class Declines {
 public:
  /// 32 bytes on aarch64, which `kHeapDeclines` (`sysmodule/source/main.cpp`)
  /// counts at the bound. Hashes rather than strings: a slot is whatever some
  /// client chose, and a decline's message is a sentence.
  struct Entry {
    std::int64_t rom_id = 0;
    std::uint64_t slot = 0;
    std::uint64_t fingerprint = 0;
    /// Met since the last `EndTick`.
    bool met = false;
  };

  /// Whether `declined` -- the result of `operation` -- is to be said: the
  /// first time it is met, or the first time since either side changed.
  /// `local_digest` is what step 0 reported for the save, if anything. It is
  /// remembered either way, unless the bound is reached.
  bool Note(const SyncOperation& operation, const OperationResult& declined,
            const std::optional<std::string>& local_digest);

  /// `operation` did what the plan asked, so the next decline of its save,
  /// whatever it is, is news.
  void Forget(const SyncOperation& operation);

  /// After a tick that ran: what it did not meet again is no longer declined --
  /// the server's save went, or the rom left the card -- and is forgotten,
  /// except for a rom in `unasked_roms` (ascending), which no plan asked about.
  void EndTick(const std::vector<std::int64_t>& unasked_roms);

  std::size_t size() const { return entries_.size(); }

 private:
  std::vector<Entry> entries_;
};

/// How many declines `Declines` remembers. Ample for a console's real ones --
/// a few saves another emulator wrote, a file in the way -- and 1 KiB of a heap
/// with 7 left over (`kHeapDeclines`). A card that declines more, a Tico with
/// several accounts and none chosen say, says the ones past it every tick.
inline constexpr std::size_t kMaxDeclines = 32;

/// Set `report`'s four totals from its operations' outcomes. **The** mapping
/// from an outcome to `completed`, `failed`, `not_understood` or `declined` --
/// `ExecutePlan` counts with it as it goes, and a resolution that replaced some results
/// recounts with it (sync_resolve.hpp) -- so an outcome added later is counted
/// in one place or in none (#259). A cancelled operation counts nowhere.
void Recount(ExecutionReport* report);

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
  /// operation. A placement with no path -- or leaving this null -- declines
  /// that operation (`OperationOutcome::kDeclined`, #257) with `kNoLocalSave`
  /// and the placement's reason rather than guessing.
  ///
  /// It is injected because the answer is not in the plan and not on the card:
  /// the server's `file_name` is tagged and the folder depends on the emulator
  /// the platform is played in and how the card is laid out this tick -- which
  /// is `place::Placer` (#237). Guessing at it would write a file no emulator
  /// loads, under a name the next tick would upload back as a second save.
  ///
  /// **A placement only creates.** A file already at the path is one the scan
  /// did not pair with this `(rom_id, slot)` -- skipped, or another slot's -- and
  /// the operation is declined with `kNoLocalSave` without touching it. The
  /// folder is made when it is missing: Tico makes a system's folder the first
  /// time it saves one, which for a first-time restore is after.
  std::function<place::Placement(const SyncOperation&)> place;

  /// The emulator id a save written at `sd_path` -- the path `place` chose --
  /// will be read back as by the next tick's scan (`scan::EmulatorAt`), or "",
  /// for a placement that does not name one itself (`place::Placement::emulator`).
  ///
  /// A download is written only where its format is read (#238): a matched
  /// save's lineage is the one `reported` gave it, and a placed one's is the
  /// placement's, else this. With neither, a `tico-*` save is not placed, since
  /// nothing then says it would land where its own module reads; any other is
  /// placed as before. Checked before the placement's folder is made.
  /// `place::Placer` always names its emulator, so the engine leaves this null.
  std::function<std::string(const SyncOperation&, const std::string& sd_path)> reads_as;

  /// The saves whose upload may supersede a newer server copy: the
  /// `(rom_id, slot)` pairs a player chose "Keep local" for on the overlay's
  /// conflict screen, and **nothing else** (#232).
  ///
  /// Every other upload goes out with `overwrite=false` and meets RomM's slot
  /// guard, which is the protection a shared slot needs: a save another device
  /// wrote since this one last synced is answered with a 409
  /// (`OperationError::kNewerOnServer`) rather than silently replaced. The
  /// override is the player's to make, per save, knowing what it replaces --
  /// never the engine's. Filled by the engine from the overlay's Keep local
  /// (#249: `conflicts::KeepLocal`, then `ArmKeepLocal`), and spent by the plan
  /// that answers it (`SpendKeepLocal`), so it is one upload's and no more.
  /// Empty on every other tick: a conflict is resolved Keep server (with the
  /// local copy archived on the server) unless the player says otherwise.
  ///
  /// A Keep local upload that landed is rewritten in place a second later with
  /// the same bytes (`PUT /api/saves/{id}`, #272): RomM breaks a tie between two
  /// rows of one second by query order, and the other device's row could
  /// otherwise be the head the next negotiation names. That costs the upload
  /// ~1.1 s on the calling thread (`wait`). The rewrite is the one call here
  /// that is retried, with backoff: it is idempotent -- the same bytes onto the
  /// same row -- where a re-posted upload is a second row, and the choice it
  /// completes is spent by the time a next tick could try. One that still fails
  /// is a line in `ExecutionReport::warnings`, never a failed upload.
  std::vector<state::Baseline::Key> keep_local;

  /// How the executor waits: the second a Keep local's rewrite waits out, and
  /// the backoff between its attempts. Null sleeps, waking to check `cancel` --
  /// not "do not wait", `NegotiateOptions::wait`'s contract.
  std::function<void(std::chrono::milliseconds)> wait;

  /// Whether an upload and a `/downloaded` carry `content_hash`: the MD5 of the
  /// file as it is uploaded, and of the verified bytes a download put on the
  /// card (#234). RomM 5.4 keeps it as this device's baseline for the row, so a
  /// later mtime-only touch -- Tico's atomic rewrites -- reads as unchanged
  /// rather than as a conflict.
  ///
  /// `RunTick` sets it from `server::ServerCapabilities::hash_baselines`, which
  /// is true on every server a tick executes against (a server below the 5.4.0
  /// floor ends the tick `kUnsupported` first); it is a field so that the
  /// capability is asked rather than assumed, as every release-dependent
  /// behaviour is (server_capabilities.hpp). True by default for the same
  /// reason `TickOptions::server` defaults to the floor's set. The digest is always the bytes'
  /// own MD5, never a ZIP's per-entry hash: RomM holds this device's baseline
  /// only against what the same device reports later, so a function that
  /// differs from the server's costs a suppression and can never invent one.
  bool send_content_hash = true;

  /// What a `delete` does: `[sync] on_server_delete` (#233).
  config::OnServerDelete on_server_delete = config::OnServerDelete::kRemove;

  /// Whether this server's `delete` is one to obey --
  /// `server::ServerCapabilities::delete_action`, which `RunTick` copies in.
  /// False, the default, makes a `delete` `kNotUnderstood`, the way an action
  /// this build does not know is: the one action that removes a file is obeyed
  /// only where a caller has said the server means it.
  bool delete_action = false;

  /// What step 0 reported, the vector `Negotiate` sent. Not owned; `RunTick`
  /// sets it.
  ///
  /// A `delete` removes a save only when its backup holds the digest reported
  /// for that `(rom_id, slot)`: the server ruled on those bytes, and a save the
  /// player wrote to since is progress the ruling never saw. Null, or a save
  /// reported with no digest, is nothing to check against, and the delete is
  /// refused before anything is copied.
  ///
  /// It is also the emulator a matched download is held to (#238): a save
  /// reported under one id takes no copy whose format is another's
  /// (`scan::Interchangeable`). Null, a `tico-*` copy is written over nothing.
  const std::vector<ClientSaveState>* reported = nullptr;

  /// The declines already said (#257). Not owned; must outlive the call. A
  /// declined operation writes its line into `ExecutionReport::declines` only
  /// when `Declines::Note` says it is news, and one that did what the plan asked
  /// is forgotten. Null says every decline, every time.
  Declines* declines = nullptr;

  /// Optional, not owned; must outlive the call. Checked between operations and
  /// passed to every request, so a shutdown ends the tick at an operation
  /// boundary rather than mid-write.
  const http::CancelToken* cancel = nullptr;
};

/// How many Keep local choices wait for their upload at once (#249).
///
/// A choice is armed when the overlay's restore lands and spent by the next plan
/// that names its save, so the list is normally one long. It is bounded because
/// a save whose rom has left the card is never named again: the oldest choice is
/// let go first, which costs that save its override and nothing else -- its
/// next upload meets the slot guard like any other.
///
/// Four, because the sysmodule's heap pays for it four times over -- the list
/// and the three copies a tick holds (`kHeapKeepLocal`, sysmodule main.cpp) --
/// and because a choice asks for its tick at once, so more than one waits only
/// while sync is switched off or held.
inline constexpr std::size_t kMaxKeepLocal = 4;

/// Arm Keep local for `key`: once, however often it is chosen, and at most
/// `kMaxKeepLocal` of them, the oldest let go first.
void ArmKeepLocal(std::vector<state::Baseline::Key>* keep_local, const state::Baseline::Key& key);

/// Drop every Keep local `executed` answered, and keep the rest (#249).
///
/// **One upload, and only that one.** An upload of the key that went, or that
/// the server refused, has had its `overwrite=true`; so has a plan that asked
/// for anything but an upload of it -- a download, a conflict, a no-op -- which
/// is the server saying it wants no upload from this save, and a choice left
/// armed past that would let some later upload, days on, supersede a save
/// nobody chose to replace. An upload that never reached the server -- a
/// transfer that failed, a save the card could not read or no longer holds, a
/// cancellation -- keeps its key for the next plan, as
/// does a key no operation of `executed` names. A key that is not in `sent` --
/// the list the plan was executed with -- is kept whatever the plan did: it was
/// armed after the plan went out, so the plan's upload of it carried no
/// override.
void SpendKeepLocal(std::vector<state::Baseline::Key>* keep_local,
                    const std::vector<state::Baseline::Key>& sent,
                    const ExecutionReport& executed);

/// Execute `plan`, one operation at a time, in the order the server sent them.
///
/// `targets` is what the client has locally, matched on `(rom_id, slot)`.
/// `token` supplies the origin, the bearer token and the `device_id` every
/// upload and confirmation carries -- without that id nothing writes the sync
/// history the *next* negotiation arbitrates against.
///
/// One failed operation does not abandon the rest: each is isolated, counted,
/// and left for the next tick. Nothing here resolves a conflict the server did
/// not report, and the one thing that removes a save -- a 5.4.0 `delete` -- does
/// it only to a save in `targets`, only after `BackUpFirst` has put its bytes
/// on the card, and never to a path `place` invents.
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
