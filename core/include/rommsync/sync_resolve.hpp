// What a tick does about an upload RomM's slot guard refused (M10-6, #232).
//
// Every upload goes out with `overwrite=false` (sync_execute.hpp), so a slot
// that holds a save this device has not synced answers `409 Slot has a newer
// save since your last sync` instead of being superseded. On 5.4.0 that is not
// a race only: another device's upload is a new row and becomes the head, this
// device has no sync row against it, and negotiate then falls back to
// timestamps and plans an `upload` -- even for bytes this device itself sent
// earlier (#227, `sync-negotiate-other-device-version.json`). Executing that
// plan is the 409. Leaving it there would draw the same 409 every tick, forever;
// lifting the guard would silently supersede the other device's save, which is
// what the guard is for.
//
// So the tick resolves it, once, in three steps:
//
//   1. **Ask the server again**, scoped to the refused saves' roms. The plan the
//      upload came from may name a head the other device has since replaced, and
//      a download of that row would fetch -- and confirm -- a superseded save.
//      One second negotiation per tick, never more, and never a loop.
//   2. **Decide**, per save, with Grout's rule (#226 R3): bytes this device has
//      not changed since it last synced take the head as a download; bytes it
//      has changed, or cannot show are unchanged, are a conflict.
//      `ResolveNewerOnServer` is that rule, and it never answers `upload`.
//   3. **Execute** that with the same `ExecutePlan` every other operation goes
//      through -- stage, verify, back up, commit, confirm -- so a resolution owes
//      hard rule 2 exactly what a planned download does and gets nothing less.
//
// **The server stays the source of truth** (hard rule 3). The 409 *is* its
// verdict -- the slot holds a newer save -- and both answers put the server's
// head on the card. What the client decides is only whether to call that a
// conflict, which costs nothing but a row on the overlay's conflict screen (and,
// with #240, an archival copy of the local bytes on the server). A download that
// would replace progress never synced is recorded as one; that is the direction
// to err in.
//
// The resolution is **merged into the tick's own plan and report**, at the
// refused operation's index, rather than reported beside them. Everything
// downstream -- the baseline (`AdvanceBaseline`), the conflict history
// (`conflicts::RecordSaves`), the counts `complete` is told -- pairs a plan with
// its report by position, and every one of them then sees what actually
// happened to the save: a download of the head, with the head's digest.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/sync_execute.hpp"

namespace rommsync::sync {

/// RomM's own sentence for the 409, byte-identical on 5.2.0 and 5.4.0
/// (`saves-post-409-no-overwrite.json`). A resolution that changed the action
/// the server named carries it as `reason_text`, so the conflict screen shows
/// the server's words for why the save is there rather than an upload's.
inline constexpr const char* kSlotNewerDetail = "Slot has a newer save since your last sync";

/// What one save whose upload drew the 409 becomes, given `head` -- the
/// operation a second negotiation planned for its `(rom_id, slot)`.
///
/// `local` is what step 0 reported for it, and `last_synced` its `state.db` row;
/// either may be null. The local bytes are unchanged since the last sync when
/// `local->content_hash` equals `last_synced->server_content_hash`: the digest
/// the server held when the two sides last agreed. Not `content_hash`, which is
/// what the client last *reported* and is written for a planned no-op even when
/// the two sides differed.
///
///   - `no_op` stays a `no_op`: the server says the head already holds these
///     bytes, or the device untracked the save.
///   - `conflict` stays a `conflict`.
///   - `upload` or `download`: a `download` of the head when the bytes are
///     unchanged since the last sync, a `conflict` otherwise.
///
/// **Never `upload`.** And nullopt -- nothing to run, the 409 stands and the
/// next tick negotiates again -- for an action this build does not understand,
/// or a head the server names no `save_id` for: there is nothing to fetch, and
/// guessing is how a save is lost.
std::optional<SyncOperation> ResolveNewerOnServer(const SyncOperation& head,
                                                  const ClientSaveState* local,
                                                  const state::SaveRecord* last_synced);

/// The most refused uploads one tick resolves (#248).
///
/// Each costs, beside the run it is resolving, its save asked about again, the
/// head the server names, what that becomes and what running it did -- ~1.5 KiB
/// on aarch64 at the phase that holds most of them -- and a plan's every upload
/// can draw the 409. Unbounded, the second plan alone was up to a whole plan
/// beside the tick's own run, ~190 KiB at `kMaxPlanOperations` with what it
/// became, which `sysmodule/source/main.cpp` now counts at this bound
/// (`kTickResolveBytes`). **The rest are not lost**: they stay refused, as one
/// past RomM's rom bound already did, and the next tick negotiates them again.
/// A 409 is another device uploading to the same slot between this tick's
/// negotiate and its upload, seconds apart; more than one at once is not a thing
/// a console meets, and four is what the heap's margin could still pay for.
inline constexpr std::size_t kMaxResolvedUploads = 4;

/// How hard the resolution tries. `RunTick` fills it from `TickOptions`.
struct ResolveOptions {
  /// `server::ServerCapabilities::rom_ids`: whether the second negotiation may
  /// be scoped. True on every server a tick runs against (5.4.0 is the floor);
  /// unscoped, it is still correct -- only the refused saves' operations are
  /// run -- and merely a larger answer.
  bool rom_ids = true;

  /// `TickOptions::hold_resolution`: asked before the second negotiation, and
  /// true sends nothing and changes nothing (`ResolveReport::held`).
  std::function<bool()> hold;

  NegotiateOptions negotiate;
  ExecuteOptions execute;
  CompleteOptions complete;
};

/// What the resolution did, for the log and for a test.
struct ResolveReport {
  /// Operations in the plan whose upload drew the 409.
  std::size_t refused = 0;

  /// `ResolveOptions::hold` answered true: a game is running, and the refused
  /// uploads stay refused for the next tick. Nothing was sent.
  bool held = false;

  /// The second negotiation was sent. At most once per tick, and only when
  /// `refused` is not zero.
  bool renegotiated = false;

  /// Its outcome. A failure leaves every refused operation as it was: failed,
  /// `kNewerOnServer`, for the next tick.
  NegotiateError negotiate_error = NegotiateError::kNone;
  std::string negotiate_message;

  /// Refused operations replaced in the tick's plan and report by what the
  /// second plan resolved them to -- whether that then succeeded or not.
  std::size_t resolved = 0;

  /// The second session's accounting. Best-effort: the transfers are done and
  /// the baseline is the first session's to write, so a failure here costs a row
  /// in a history a user reads and nothing else (5.4.0 expires an unfinished
  /// session after 24 h).
  Completion second_session;
};

/// Resolve every operation in `*executed` that failed with `kNewerOnServer`,
/// in place: the operation in `*plan` and its result in `*executed`, at the same
/// index, become what the resolution ran and what became of it, and the
/// report's counts and warnings are made to agree.
///
/// Does nothing -- no request -- when there is no such operation, or when the
/// report was cancelled or stopped by a 401/403: the first ends the tick and the
/// second would only be met again.
///
/// `reported` and `targets` are the tick's step-0 output, `previous` the
/// baseline it started from. Only the refused saves are offered to the second
/// negotiation, scoped to their roms (`rom_ids`), and only their operations in
/// its plan are run: anything else it names is the next tick's.
ResolveReport ResolveRefusedUploads(http::HttpClient& client, fs::FileSystem& files,
                                    const auth::StoredToken& token,
                                    const std::vector<ClientSaveState>& reported,
                                    const std::vector<SaveTarget>& targets,
                                    const state::Baseline& previous, SyncPlan* plan,
                                    ExecutionReport* executed,
                                    const ResolveOptions& options = {});

}  // namespace rommsync::sync
