// One sync tick, start to finish: what it cleans up before it begins, the three
// stages in the order they have to happen, and what is left on the card when
// something cuts it off half way.
//
// M2-4 negotiates, M2-5 executes and M2-6 completes, and each of those is safe
// on its own. This is the thing that runs them, and it exists because the
// dangerous state is not inside any one of them -- it is between them, and it is
// what a console that loses Wi-Fi, sleeps, or is switched off produces several
// times a day.
//
// **The rule the whole file serves**: the state left behind after any
// interruption is either the state before the tick, or a strictly completed
// subset of it. There is no third legal outcome (docs/SYNC_PROTOCOL.md's
// failure & safety rules).
//
// Three things here are not what an obvious reading produces:
//
//   - **A leftover `<save>.tmp` is removed, not committed.** `ExecutePlan`
//     stages a download at `io::TempPathFor(<save>)`, and `http::DownloadTarget`
//     renames its `.part` onto that path the instant the body ends -- which is
//     *before* the MD5 is checked and before the previous bytes are backed up.
//     So a `.tmp` a crash left behind is a complete body that may not be the
//     save the plan meant, with no backup beside it, and nothing at recovery
//     time can tell: the digest to check it against lived in a plan that is
//     gone. Committing it would overwrite a save with unverified bytes and no
//     backup, which is hard rule 2 broken in the one place it matters. It costs
//     one refetch to say no, and the next tick's negotiation is the arbiter.
//
//   - **A `<save>.old` is the save itself**, moved aside by `io::CommitStaged`,
//     and it is the one leftover that must not be deleted: when `<save>` is
//     missing it is the only copy. It is renamed back. When `<save>` *is* there
//     the commit finished -- only the tidy-up did not -- and the previous bytes
//     are already under `.backup/`, so it goes.
//
//   - **Nothing under `.backup/` is ever a leftover.** A backup is never
//     garbage: it is the copy M7-1 restores from, including the one written for
//     an overwrite that then failed. Only the `.tmp` of an interrupted
//     `io::CopyAtomically` is swept from there.
//
// This owns neither end of a tick. Step 0 -- scanning the card and matching
// saves to roms -- is M2-2's and arrives here as `reported` and `targets`; the
// schedule between ticks, the backoff and the clock are M7-2's, and so is firing
// the `http::CancelToken` this passes to all three stages.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/sync_execute.hpp"
#include "rommsync/sync_finish.hpp"

namespace rommsync::sync {

/// What a sweep found, and what it did about it.
///
/// Counts rather than paths: a card that comes back from a bad week can hold a
/// leftover per save, and `core/` has no logger to spend a line each on
/// (docs/ARCHITECTURE.md). `warnings` names the ones that could *not* be dealt
/// with, which is the short list worth reading.
struct RecoveryReport {
  /// `<save>.tmp.part` -- an interrupted body, removed.
  std::size_t partials_removed = 0;

  /// `<save>.tmp` -- a complete but unverified body, removed. See the header.
  std::size_t staged_removed = 0;

  /// `<save>.old` renamed back onto `<save>`, because `<save>` was missing.
  /// This is a save recovered, and the number worth being loudest about.
  std::size_t saves_restored = 0;

  /// `<save>.old` beside a `<save>` that is there: a commit that finished and
  /// did not tidy up. Removed.
  std::size_t previous_removed = 0;

  /// Leftovers the card would not let go of, and directories that would not
  /// list. Never fatal -- a tick runs anyway, and the worst a survivor costs is
  /// the space it takes and one more attempt next time.
  std::vector<std::string> warnings;

  std::size_t total() const {
    return partials_removed + staged_removed + saves_restored + previous_removed;
  }
};

/// Sweep `directories` for what an interrupted tick leaves beside a save.
///
/// The three suffixes are taken from `io::TempPathFor`, `io::PreviousPathFor`
/// and `http::PartialPathFor` rather than spelled again here: a second spelling
/// of `.tmp` inside `core/` is how the sweep and the writers stop agreeing.
///
/// **A bare `<name>.part` is left alone.** Only `<name>.tmp.part` is swept --
/// the partial of a *save* download, which is never resumed
/// (`DownloadTarget::resume` is false there, sync_execute.cpp). A `.part` beside
/// a rom is a M3-3 range-resume in progress and deleting it would throw away a
/// gigabyte of a transfer that was going to finish.
///
/// `directories` are SD-root paths and are listed, not walked: the save folders
/// from the config's map, plus `/config/rommsync` and its `.backup/`. Nothing
/// recurses, for the reason `fs::FileSystem` gives.
RecoveryReport RecoverStaging(fs::FileSystem& files,
                              const std::vector<std::string>& directories);

/// How a tick ended, as the thing that schedules the next one needs it.
///
/// Split on what a caller would *do*, the way every error enum here is: back
/// off, try again next schedule, ask `auth::Gate`, or stop.
enum class TickOutcome {
  /// Every operation the plan asked for happened, and the session was accounted
  /// for.
  kCompleted,

  /// It ran, and some operations did not do what the plan asked. The saves
  /// involved were left alone and the next negotiation plans them again. Not a
  /// reason to retry sooner: there is no retry inside a tick, deliberately
  /// (sync_execute.hpp).
  kPartial,

  /// The transfers landed and the accounting did not. The baseline is on the
  /// card -- `FinishTick` writes it first, on purpose -- so this costs a row in
  /// a history a user reads and nothing else.
  kUnreported,

  /// The negotiation never completed: offline, stalled, reset, or a 5xx.
  /// **Nothing was written.** The schedule backs off and tries again (M7-2).
  kOffline,

  /// The server answered, and the answer does not change on a second attempt: a
  /// device deleted in the web UI, sync switched off for it, a body it refused.
  /// Backing off harder than usual is the polite response; re-pairing is not.
  kRefused,

  /// The server stopped accepting the token, or refused a scope. `answer` is
  /// what `auth::Gate` needs; the tick did not decide anything about the
  /// pairing (M1-4).
  kUnauthorized,

  /// The caller's `http::CancelToken` fired. Not a failure of anything: the tick
  /// stopped at a boundary and what it had done stands.
  kCanceled,
};

/// Stable, log-friendly name. Never null.
const char* ToString(TickOutcome outcome);

/// What one tick is allowed to touch, and how hard each stage tries.
///
/// The per-call retry is not re-implemented here: `NegotiateOptions` and
/// `CompleteOptions` are `CallPolicy`, which already times out, retries and
/// backs off, and widening `ShouldRetry` is explicitly not this module's to do.
struct TickOptions {
  /// Swept by `RecoverStaging` before anything else happens. Empty means no
  /// sweep, which is only right for a caller that has just done one.
  std::vector<std::string> recover_dirs;

  /// Create `execute.backup_dir` on entry if it is not there.
  ///
  /// On a first boot it never is, and a missing one fails every download with
  /// `OperationError::kBackupFailed` -- correctly, because no backup means no
  /// overwrite. `fs::FileSystem::CreateDirectory` is the platform facility that
  /// closes that, and running it every tick costs one stat.
  ///
  /// A failure is a line in `TickResult::recovered.warnings` rather than the end
  /// of the tick: an upload and a no-op do not need the directory at all, and
  /// every operation that would overwrite a save fails with `kBackupFailed`
  /// further down anyway -- no backup, no overwrite. A tick that can still do
  /// half its work does it.
  bool create_backup_dir = true;

  NegotiateOptions negotiate;
  ExecuteOptions execute;
  FinishOptions finish;

  /// Optional, not owned; must outlive the call.
  ///
  /// **One token, three stages.** It is copied onto `negotiate.cancel`,
  /// `execute.cancel` and `finish.complete.cancel`, and checked between them, so
  /// a shutdown ends the tick at the next boundary rather than mid-write -- and
  /// without spending three timeouts plus two backoffs on the accounting call
  /// afterwards, on the link whose loss is usually why the shutdown happened.
  /// A `cancel` set on one of the three option blocks instead is overwritten by
  /// this one; setting it here is how a caller says it.
  const http::CancelToken* cancel = nullptr;
};

/// Everything one tick did, stage by stage.
///
/// Each stage's own report is kept rather than flattened: `negotiated.plan` is
/// what the server decided, `executed.operations` is what became of each save,
/// and `finished.stored` is whether the baseline reached the card. `outcome` is
/// the one-line answer for the scheduler and `answer` the one for `auth::Gate`.
struct TickResult {
  RecoveryReport recovered;
  Negotiation negotiated;
  ExecutionReport executed;
  TickCompletion finished;

  TickOutcome outcome = TickOutcome::kCompleted;

  /// What this tick learned about the credentials, for `auth::Gate::Observe`.
  /// `kSilent` when nothing it did says anything either way -- an offline tick
  /// is no evidence that the token still works (auth_gate.hpp).
  auth::Answer answer = auth::Answer::kSilent;

  /// The completion call was told the session is not this tick's to close:
  /// already completed, cancelled by a later negotiation, or unknown.
  ///
  /// **Never a reason to re-run the plan.** `kAlreadyCompleted` means the
  /// accounting succeeded -- a retry landing after the first attempt got through
  /// looks exactly like this -- and `kSuperseded` and `kNoSuchSession` mean the
  /// counts will never be recorded. In all three the transfers already happened
  /// and the baseline is on the card, so the answer is the next tick's own
  /// negotiation and not this one's second attempt.
  bool session_gone = false;

  /// True only for `kCompleted`. A caller that wants "the save data is safe"
  /// wants `finished.stored.ok()`.
  bool ok() const { return outcome == TickOutcome::kCompleted; }
};

/// Run one tick: recover, negotiate, execute, finish.
///
/// `reported` and `targets` are step 0's output (M2-2) and are paired by
/// `(rom_id, slot)` -- the first is what the server arbitrates on, the second is
/// where the files are. `previous` is the baseline `state::LoadBaseline` read,
/// taken by value so a caller can `std::move` it in.
///
/// **A failed negotiation returns before anything is written.** That is the
/// offline case and it is the common one: no request that follows would work, no
/// save is touched, no backup is written and `state.db` is not rewritten. The
/// tick is missed, which is harmless; a blocked boot is not (CLAUDE.md).
///
/// After the plan is in hand the tick always finishes: `FinishTick` persists the
/// baseline and *then* reports the session, in that order, whatever became of
/// the operations. A 401 part way through the plan does not skip it either --
/// the transfers that did happen are on the server, and the baseline is the
/// expensive thing to lose (sync_finish.hpp).
TickResult RunTick(http::HttpClient& client, fs::FileSystem& files,
                   const auth::StoredToken& token,
                   const std::vector<ClientSaveState>& reported,
                   const std::vector<SaveTarget>& targets, state::Baseline previous,
                   const TickOptions& options = {});

}  // namespace rommsync::sync
