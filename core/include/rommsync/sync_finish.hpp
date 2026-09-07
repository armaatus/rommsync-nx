// The end of one sync tick: the counts it reports, the baseline it commits, and
// the order the two happen in.
//
// `sync.hpp` opens a session and asks what should happen (step 1), and closes it
// again with `CompleteSession` (step 3). `sync_execute.hpp` does what the plan
// said (step 2). This is what turns the second into the third: an
// `ExecutionReport` into the two integers `complete` carries, and into the rows
// `state.db` keeps so the *next* tick knows which local files are worth
// re-hashing.
//
// Three things here are not what an obvious reading produces, and each one costs
// something real:
//
//   - **The baseline is persisted before the session is reported.** Complete is
//     accounting, not a commit point: the uploads and downloads already landed
//     on the server, and RomM already wrote the sync rows the next negotiation
//     arbitrates against. So a `complete` that fails must not take the baseline
//     down with it -- do that and the next tick re-uploads saves the server
//     already has, and RomM stamps each one as a new row. `FinishTick` performs
//     the two in that order and reports both, which is why it exists rather than
//     being left to a caller to remember.
//
//   - **Only an operation that did what the plan asked advances a row.** A
//     failed one keeps its previous row so the next tick retries it, and a save
//     whose hash could not be taken is not moved forward -- `state::SaveBaseline`
//     would refuse a row with an empty digest anyway. Whether the row it already
//     had survives that depends on whether the file did: an upload and a no-op
//     leave the card alone, so the stored row still describes it and is kept; a
//     download that could not be read back replaced the bytes, so the stored row
//     describes bytes that are gone and is **erased**.
//
//   - **A save whose bytes this tick replaced has to be read again.** After a
//     download the mtime, the size and the digest on the card are all different
//     from the ones the tick reported, and a row written from the reported ones
//     would claim a digest for bytes nothing ever hashed. The file is re-stat'd
//     through `fs::FileSystem::List` -- `core/` has no single-file stat -- and
//     re-hashed; a save that cannot be re-read has its row removed and is named
//     in `warnings`.
//
// `not_understood` is counted with `operations_failed`, which is the one
// accounting decision here worth stating twice: the server planned work that did
// not happen, and folding it into `completed` would tell RomM the client did
// something it did not (docs/SYNC_PROTOCOL.md step 3).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/sync_execute.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::sync {

/// Where the baseline lives, SD-root absolute -- `kBackupDir`'s neighbour, and
/// the same reasoning: docs/ARCHITECTURE.md puts the client's own state under
/// `/config/rommsync`, and a path spelled in each caller is a path two of them
/// eventually spell differently.
inline constexpr const char* kStateSdPath = "/config/rommsync/state.db";

/// The counts `report` earned.
///
/// `completed` is `operations_completed`, planned `no_op`s included.
/// `failed + not_understood` is `operations_failed`: an action this build did
/// not recognise is work the server planned and the client did not do, and it
/// has to be visible as such rather than vanishing between the two fields.
///
/// A cancelled tick is counted for what it actually did -- the operations after
/// the cancellation are in neither total, so the two add up to less than
/// `report.operations.size()`. That is honest: they were not attempted.
CompletionCounts CountsFor(const ExecutionReport& report);

/// The baseline a tick should store, and everything it could not put in it.
struct BaselineUpdate {
  /// `previous` with this tick's rows moved forward. Always usable: a tick that
  /// advanced nothing yields the previous baseline unchanged, which is exactly
  /// right -- nothing happened, so nothing about the last sync stopped being
  /// true.
  state::Baseline value;

  /// Rows this tick moved forward. Not the size of `value`, which also holds
  /// every row a failed or unattempted operation kept.
  std::size_t advanced = 0;

  /// Rows dropped to keep `value` inside `state::kMaxRecords` -- saves that were
  /// neither reported nor operated on this tick, so almost always saves that are
  /// no longer on the card. See `AdvanceBaseline`.
  std::size_t dropped = 0;

  /// One line per save that could not be advanced, bounded by
  /// `state::kMaxDiagnostics`. Never an error: every one of them costs a re-hash
  /// on the next tick and nothing else. Handed up rather than logged here, for
  /// `ExecutionReport::warnings`' reason; the caller writes them.
  std::vector<std::string> warnings;
};

/// Move `previous` forward by what this tick did.
///
/// `reported` is the `ClientSaveState` vector the tick negotiated with -- the
/// scan's own facts, which are still exactly right for every save this tick did
/// not rewrite. `plan` and `report` are paired by index, the order
/// `ExecutePlan` promises; an operation whose two halves disagree about
/// `(rom_id, slot)` is skipped and named rather than guessed at.
///
/// **A save with no operation is not advanced**, and does not need to be: a save
/// this client reports and is already in sync with comes back as an explicit
/// `no_op` / `Content is identical`, not as an absence (verified against the
/// live 5.2.0, docs/API_CONTRACT.md). What RomM leaves out of a plan is a save
/// the client did not mention, and there is nothing to record about one of
/// those.
///
/// An advancing row starts from the row that is already there rather than from
/// nothing, so a field the plan happened not to carry does not quietly erase
/// what the last sync knew. Which side may erase differs by direction, and that
/// is the point of doing it this way: an `upload` clears `server_updated_at`
/// because the stored one describes the copy it just replaced, a `download`
/// clears what the plan could not supply because the bytes on the card are
/// newer than the stored value describes, and a `no_op` moved nothing and so
/// keeps everything it does not restate.
///
/// **Rows are dropped only to fit, and only the ones this tick knows nothing
/// about.** A row whose save was neither reported nor planned is usually a save
/// that has been deleted from the card, but it is also what a mapped folder that
/// failed to list produces -- so they are kept until `value` would exceed
/// `state::kMaxRecords`, which is the point at which keeping them costs the
/// *whole* baseline (`SaveBaseline` refuses a file `ParseBaseline` would
/// discard whole). Dropping the stale ones first is the only order that loses
/// nothing this tick knows.
///
/// **It cannot invent room.** A tick that reports and plans more than
/// `kMaxRecords` saves by itself has nothing droppable left, and neither this
/// nor `SaveBaseline` can make that file readable -- that is the card #11 says
/// needs `kInnerHeapSize`, `scan::kMaxSaves` and `state::kMaxRecords` raised
/// together. The refusal is then `SaveBaseline`'s and is named in
/// `StoreResult::message`, which `FinishTick` hands back; the trim says so in
/// `warnings` on its way past. The byte bound is the writer's alone: a row's
/// serialized size depends on the slot the user's emulator chose, and this
/// function does not serialize.
/// `previous` is taken **by value** so a tick can `std::move` in the baseline it
/// loaded. Holding the old rows and the new ones at once costs twice
/// `state::kMaxStateBytes` worth of parsed rows -- more than that, since a row
/// costs more parsed than as text -- on the same tick that is already holding
/// the plan, the report, the reported saves and a directory listing, against a
/// 512 KiB inner heap (state_db.hpp, core/AGENTS.md).
BaselineUpdate AdvanceBaseline(state::Baseline previous, const SyncPlan& plan,
                               const ExecutionReport& report,
                               const std::vector<ClientSaveState>& reported,
                               fs::FileSystem& files);

/// What one tick's ending is allowed to touch, and how hard it tries.
struct FinishOptions {
  /// The baseline's path, SD-root absolute. Resolved through the `FileSystem`
  /// handed to `FinishTick`, so a test writes to its sandbox rather than to a
  /// card.
  std::string state_sd_path = kStateSdPath;

  /// The completion call's timeout, attempts and backoff.
  CompleteOptions complete;

  /// Play time to carry on the completion that is happening anyway (M7-4, #39).
  ///
  /// Empty on every tick that recorded none, which is most of them, and an
  /// empty vector is sent as `[]` rather than omitted (`EncodeCompleteRequest`).
  /// `play::Buffer::Pending` is what fills it and `play::Reconcile` is what
  /// reads the answer back out of `TickCompletion::reported`.
  ///
  /// **Nothing here may cost the tick.** A session the encoder refuses is
  /// dropped and the completion is sent without it, with a line in `warnings` --
  /// see `TickCompletion::play_sessions_sent`. Play time is the most droppable
  /// thing in the client; the session RomM is waiting to have closed is not.
  std::vector<PlaySession> play_sessions;
};

/// What the end of a tick did. Both halves are reported, because they fail
/// independently and mean different things.
struct TickCompletion {
  /// What `state::SaveBaseline` did. This happens **first**, whatever becomes of
  /// the report -- and `stored.skipped` is not an error: rows it could not read
  /// back were left out and the rest were written. Each of those is also
  /// appended to `warnings` below, so a caller that logs one list does not
  /// silently miss a save that lost its row -- up to the same
  /// `state::kMaxDiagnostics` cap every list here has, past which this field is
  /// the complete one.
  state::StoreResult stored;

  /// What `CompleteSession` answered.
  Completion reported;

  /// The counts that were sent, whether or not the call carrying them landed.
  CompletionCounts counts;

  /// How many of `FinishOptions::play_sessions` the body actually carried.
  ///
  /// Less than what was handed over means one of two things, and both are zero
  /// rather than a smaller number: the encoder refused a session, so the array
  /// went whole (`play::Reconcile` matches an answer by index, so a filtered one
  /// would reconcile against a list the server never saw); or `CompleteSession`
  /// refused before it built a request at all -- an unpaired token, no session
  /// id, a caller that had already cancelled. A `warnings` line names the first.
  ///
  /// **Zero is not "the server rejected them"**: it is "nothing was sent", and
  /// the sessions are still in the buffer. What RomM did with the ones that
  /// were sent is `reported.value.play_session_ingest`.
  std::size_t play_sessions_sent = 0;

  /// Rows `AdvanceBaseline` moved forward.
  std::size_t rows_advanced = 0;

  /// Rows it dropped to stay inside `state::kMaxRecords`. Reported as a count
  /// rather than left to `warnings`, which is bounded: a tick that pruned two
  /// hundred rows can say so in one number, where the lines naming them are the
  /// first `state::kMaxDiagnostics` of every warning this tick produced.
  std::size_t rows_dropped = 0;

  /// One line per save this tick could not record: `AdvanceBaseline`'s, and
  /// then the rows `state::SaveBaseline` left out. None of them is an error --
  /// each costs one save a re-hash on the next tick and nothing else.
  std::vector<std::string> warnings;

  /// Both halves worked. A caller that wants "the save data is safe" wants
  /// `stored.ok()`; this is "the tick ended cleanly".
  bool ok() const { return stored.ok() && reported.ok(); }
};

/// Persist the baseline, then report the session. In that order, always.
///
/// The order is the point (see the header note): the transfers are already done
/// on the server, so the expensive thing to lose is the client's own record of
/// what it hashed. A failed `complete` is a failed tick and a retry next
/// schedule; a lost baseline is the whole library re-hashed and re-negotiated,
/// silently, until something writes one again.
///
/// Nothing here is skipped *because* `report.canceled` is set. A shutdown that
/// got half a plan done still hashed what it hashed, and the counts it sends are
/// the operations it actually performed.
///
/// What stops the call is the token, not the flag: a caller that fired
/// `ExecuteOptions::cancel` and passes the same token on
/// `FinishOptions::complete` gets the completion abandoned without a request,
/// while the baseline -- which is local, and the expensive thing to lose -- is
/// written either way. That is the split on purpose. A caller that wants
/// neither can persist the baseline itself with `AdvanceBaseline` and
/// `state::SaveBaseline`, in that order.
TickCompletion FinishTick(http::HttpClient& client, fs::FileSystem& files,
                          const auth::StoredToken& token, const SyncPlan& plan,
                          const ExecutionReport& report,
                          const std::vector<ClientSaveState>& reported,
                          state::Baseline previous, const FinishOptions& options = {});

}  // namespace rommsync::sync
