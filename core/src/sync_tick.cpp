// One tick, in order. Read `sync_tick.hpp` first -- the rules are there.
//
// What is only here is the joining: which stage's failure ends the tick and
// which does not, and the one place the cancel token is fanned out. Nothing in
// this file retries a call. Each stage already has `CallPolicy`, and widening
// what `ShouldRetry` picks is explicitly not this module's to do -- a 401, a 404
// and a refused body are all answers that do not change on a second attempt.
#include "rommsync/sync_tick.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "rommsync/auth_gate.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/rom_scope.hpp"

namespace rommsync::sync {
namespace {

bool Canceled(const http::CancelToken* cancel) {
  return cancel != nullptr && cancel->canceled();
}

/// What a negotiation that produced no plan means for the schedule.
TickOutcome OutcomeOf(NegotiateError error) {
  switch (error) {
    case NegotiateError::kNone:
      return TickOutcome::kCompleted;
    case NegotiateError::kCanceled:
      return TickOutcome::kCanceled;
    case NegotiateError::kUnauthorized:
    case NegotiateError::kForbidden:
      return TickOutcome::kUnauthorized;
    case NegotiateError::kUnreachable:
    case NegotiateError::kServerError:
      // The two `ShouldRetry` picks, and the only two that say nothing about
      // this client: the link, or RomM having a bad minute.
      return TickOutcome::kOffline;
    case NegotiateError::kSyncDisabled:
      // The user's switch in RomM, and its own outcome because it is not a
      // failure: the schedule keeps the interval, and the screen names it.
      return TickOutcome::kServerDisabled;
    case NegotiateError::kUnusablePayload:
    case NegotiateError::kNotRegistered:
    case NegotiateError::kNoSuchDevice:
    case NegotiateError::kRejected:
    case NegotiateError::kMalformed:
      // Answers that do not get better by asking again. A malformed body is in
      // here rather than with the transport failures on purpose: a truncated
      // plan is a plan with a save missing from it, and the cheap mistake is to
      // treat that as a server the client should stop hammering.
      return TickOutcome::kRefused;
  }
  return TickOutcome::kRefused;
}

/// Whether the completion said the session is not this tick's to close.
bool SessionGone(CompleteError error) {
  return error == CompleteError::kAlreadyCompleted || error == CompleteError::kSuperseded ||
         error == CompleteError::kNoSuchSession;
}

/// How a tick that got as far as a plan ended.
///
/// The order of the questions is the point. A cancellation and a 401 are the two
/// things that are not about one operation, so they are read first; after that
/// the accounting decides between "reported" and "not reported", and the counts
/// decide between "completed" and "partial".
TickOutcome OutcomeAfter(const ExecutionReport& executed, const TickCompletion& finished) {
  if (executed.canceled || finished.reported.error == CompleteError::kCanceled) {
    return TickOutcome::kCanceled;
  }
  if (executed.unauthorized || finished.reported.error == CompleteError::kUnauthorized ||
      finished.reported.error == CompleteError::kForbidden) {
    return TickOutcome::kUnauthorized;
  }
  if (!finished.stored.ok()) {
    // Asked **before** the accounting, because the two fail together -- a card
    // having a bad minute drops the link as readily as it refuses a write -- and
    // `kUnreported` promises the baseline is safe. Nothing is wrong with the
    // saves here; the next tick re-hashes the library, and calling that
    // "completed" or "reported" would hide it.
    return TickOutcome::kPartial;
  }
  if (!finished.reported.ok() && !SessionGone(finished.reported.error)) {
    // The transfers landed and the accounting did not, and the baseline is on
    // the card, so this costs a row in a history a user reads.
    return TickOutcome::kUnreported;
  }
  const bool all_done = executed.failed == 0 && executed.not_understood == 0;
  return all_done ? TickOutcome::kCompleted : TickOutcome::kPartial;
}

/// The credentials answer this tick earned, from whichever stage spoke last
/// about them.
///
/// **Last word, not worst word.** A 401 at one operation followed by a
/// completion the same token was accepted for is a proxy having a bad minute,
/// not a revocation -- `harness.expired` shows a live server doing exactly that
/// -- and `auth::Gate` counts *consecutive* rejections, so handing it the
/// rejection when the very next call proved the token works is how a console
/// talks itself out of a pairing that was never gone. A real revocation refuses
/// the completion too, and then this is `kRejected`.
///
/// `kSilent` falls through rather than winning: an offline completion is no
/// evidence either way, so the operation that did meet a 401 is what the gate
/// hears. And an invented `kAccepted` would clear a count that should have
/// stood, which is why nothing here manufactures one (auth_gate.hpp).
auth::Answer AnswerAfter(NegotiateError negotiated, const ExecutionReport& executed,
                         const TickCompletion& finished) {
  const auth::Answer reported = AnswerOf(finished.reported.error);
  if (reported != auth::Answer::kSilent) {
    return reported;
  }
  if (!executed.operations.empty()) {
    const auth::Answer last = AnswerOf(executed.operations.back().error);
    if (last != auth::Answer::kSilent) {
      return last;
    }
  }
  // The negotiation, last, and it is never nothing: reaching here means it
  // produced a plan, which is `kAccepted` -- the server had to read the token to
  // answer one. Leaving it out was a real hole: a tick that negotiated fine and
  // then lost the link at a download *and* at `complete` would report `kSilent`,
  // so `auth::Gate`'s consecutive-rejection count would survive a 200 that
  // should have cleared it, and three such ticks around three transient 401s
  // would drop a pairing the server never revoked.
  return AnswerOf(negotiated);
}

/// One session's share of a plan several negotiations made: its id, and the
/// operations `[begin, end)` it planned.
struct SessionShare {
  std::int64_t id = 0;
  std::size_t begin = 0;
  std::size_t end = 0;
};

/// Negotiate `offered` over `options.rom_scope`, one chunk at a time, into one
/// plan (rom_scope.hpp). The first failure is the answer, with the attempts and
/// the backoff of every chunk before it.
Negotiation NegotiateScope(http::HttpClient& client, const auth::StoredToken& token,
                           const std::vector<ClientSaveState>& offered,
                           const TickOptions& options, const NegotiateOptions& negotiate,
                           std::vector<SessionShare>* shares) {
  const std::vector<ScopeChunk> chunks =
      ChunkScope(offered, *options.rom_scope,
                 options.server.restore_unlisted ? options.restore_roms
                                                 : std::vector<std::int64_t>{});
  Negotiation merged;
  for (const ScopeChunk& chunk : chunks) {
    NegotiateOptions asked = negotiate;
    asked.restore_unlisted = chunk.restore;
    // The whole vector when there is one chunk, which is nearly every card:
    // the tick that carries it is the heap's peak, and a copy of it buys nothing.
    std::vector<ClientSaveState> some;
    if (chunks.size() > 1) {
      some.reserve(chunk.saves.size());
      for (const std::size_t index : chunk.saves) {
        some.push_back(offered[index]);
      }
    }
    Negotiation one =
        NegotiateRoms(client, token, chunks.size() > 1 ? some : offered, chunk.rom_ids, asked);
    one.attempts += merged.attempts;
    one.waited += merged.waited;
    if (!one.ok()) {
      return one;
    }
    const std::size_t begin = merged.plan.operations.size();
    const std::int64_t session = one.plan.session_id;
    if (shares->empty()) {
      merged = std::move(one);
    } else {
      SyncPlan& plan = merged.plan;
      for (SyncOperation& operation : one.plan.operations) {
        plan.operations.push_back(std::move(operation));
      }
      plan.total_upload += one.plan.total_upload;
      plan.total_download += one.plan.total_download;
      plan.total_conflict += one.plan.total_conflict;
      plan.total_no_op += one.plan.total_no_op;
      plan.total_delete += one.plan.total_delete;
      for (std::string& warning : one.plan.warnings) {
        plan.warnings.push_back(std::move(warning));
      }
      merged.attempts = one.attempts;
      merged.waited = one.waited;
    }
    shares->push_back({session, begin, merged.plan.operations.size()});
  }
  return merged;
}

}  // namespace

const char* ToString(TickOutcome outcome) {
  switch (outcome) {
    case TickOutcome::kCompleted:
      return "completed";
    case TickOutcome::kPartial:
      return "partial";
    case TickOutcome::kUnreported:
      return "unreported";
    case TickOutcome::kOffline:
      return "offline";
    case TickOutcome::kRefused:
      return "refused";
    case TickOutcome::kServerDisabled:
      return "server_disabled";
    case TickOutcome::kUnauthorized:
      return "unauthorized";
    case TickOutcome::kCanceled:
      return "canceled";
    case TickOutcome::kDisabled:
      return "disabled";
    case TickOutcome::kRescanNeeded:
      return "rescan_needed";
    case TickOutcome::kUnsupported:
      return "unsupported";
  }
  return "unknown";
}

TickResult RunTick(http::HttpClient& client, fs::FileSystem& files,
                   const auth::StoredToken& token,
                   const std::vector<ClientSaveState>& reported,
                   const std::vector<SaveTarget>& targets, state::Baseline previous,
                   const TickOptions& options) {
  TickResult result;
  if (!options.enabled) {
    // Before the cancel check and before the sweep: a console whose owner has
    // switched sync off has not asked for litter to be tidied either, and the
    // criterion this serves is that a disabled engine costs nothing at all --
    // no file opened, no request sent (#33). `download::Drain` refuses in the
    // same place for the same reason.
    result.outcome = TickOutcome::kDisabled;
    return result;
  }
  if (Canceled(options.cancel)) {
    result.outcome = TickOutcome::kCanceled;
    return result;
  }

  // Before anything, and deliberately before the network: this only removes
  // litter and puts back a save an interrupted commit parked, so it is right to
  // do even on a tick that turns out to be offline. It writes no backup, no
  // save and no `state.db`.
  result.recovered = RecoverStaging(files, options.recover_dirs);

  if (result.recovered.saves_restored > 0) {
    // A save the scan could not have seen is back on the card, so `reported` is
    // already out of date -- and negotiating with it would report that save as
    // absent, which RomM answers by planning a download over it. See
    // `TickOutcome::kRescanNeeded`. Nothing has been written but the restore
    // itself, and the caller scans again.
    result.outcome = TickOutcome::kRescanNeeded;
    return result;
  }

  if (Canceled(options.cancel)) {
    result.outcome = TickOutcome::kCanceled;
    return result;
  }

  if (!options.server.supported) {
    // After the sweep, which is local and only ever puts a save back, and
    // before the one request that would offer the card's saves to a server too
    // old to arbitrate them (`TickOutcome::kUnsupported`).
    result.outcome = TickOutcome::kUnsupported;
    return result;
  }

  // A save RomM deleted and `[sync] on_server_delete = keep` left is not
  // offered while it still holds the bytes that were deleted (#233). RomM's
  // memory of a deletion is bounded, and it reads the same bytes rewritten with
  // a later mtime as progress -- an emulator that writes its save on closing
  // would upload it straight back. New bytes are offered. Copied only when one
  // is withheld: the tick that carries this vector is the heap's peak.
  std::vector<ClientSaveState> without_kept;
  const std::vector<ClientSaveState>* offered = &reported;
  const auto withheld = [&previous](const ClientSaveState& save) {
    const state::SaveRecord* row = previous.Find(save.rom_id, save.slot);
    return row != nullptr && row->deleted_on_server && save.content_hash.has_value() &&
           *save.content_hash == row->content_hash;
  };
  if (std::any_of(reported.begin(), reported.end(), withheld)) {
    for (const ClientSaveState& save : reported) {
      if (!withheld(save)) {
        without_kept.push_back(save);
      }
    }
    offered = &without_kept;
  }

  NegotiateOptions negotiate = options.negotiate;
  negotiate.cancel = options.cancel;
  // Scoped to the roms on the card, and chunked to RomM's bound, wherever the
  // caller knows them (#241).
  std::vector<SessionShare> shares;
  result.negotiated = options.rom_scope.has_value() && options.server.rom_ids
                          ? NegotiateScope(client, token, *offered, options, negotiate, &shares)
                          : Negotiate(client, token, *offered, negotiate);
  if (!result.negotiated.ok()) {
    // The offline case, and the one the acceptance is sharpest about: nothing
    // after this point runs, so no save is touched, no backup is written and
    // `state.db` is not rewritten.
    result.outcome = OutcomeOf(result.negotiated.error);
    result.answer = AnswerOf(result.negotiated.error);
    return result;
  }

  // Only now: nothing needs `.backup/` until an operation does, and a tick that
  // never reached the server must not have written anything to the card.
  if (options.create_backup_dir) {
    const fs::MakeDirResult made = files.CreateDirectory(options.execute.backup_dir);
    if (!made.ok() && result.recovered.warnings.size() < kMaxRecoveryWarnings) {
      // Not fatal. Every operation that would overwrite a save fails with
      // `kBackupFailed` further down -- no backup, no overwrite -- and an upload
      // or a no-op does not need the directory at all, so a tick that can still
      // do half its work does it. Bounded by the same cap the sweep's own lines
      // are, because the list is the same list.
      result.recovered.warnings.push_back("the backup directory " + options.execute.backup_dir +
                                          " could not be created: " + made.message);
    }
  }

  ExecuteOptions execute = options.execute;
  execute.cancel = options.cancel;
  execute.send_content_hash = options.server.hash_baselines;
  // A `delete` (#233) is obeyed only where the heartbeat said the server means
  // one, and only on the bytes this tick reported: the server ruled on those.
  execute.delete_action = options.server.delete_action;
  execute.reported = offered;
  result.executed =
      ExecutePlan(client, files, token, result.negotiated.plan, targets, execute);

  // Before the baseline is written, because the baseline is written from the
  // plan and the report this changes: an upload the slot guard refused becomes
  // a download of the server's head, or a conflict, at the same index -- never
  // another upload, and never more than one second negotiation (#232).
  ResolveOptions resolve;
  resolve.rom_ids = options.server.rom_ids;
  resolve.hold = options.hold_resolution;
  resolve.negotiate = negotiate;
  resolve.execute = execute;
  // Never a removal inside the resolution (#233): it only ever runs the heads
  // `ResolveNewerOnServer` turned into a download or a conflict, and a delete
  // that slipped past it is still not obeyed here.
  resolve.execute.delete_action = false;
  resolve.complete = options.finish.complete;
  resolve.complete.cancel = options.cancel;
  // `offered` again: a save withheld as deleted on the server (#233) was never
  // uploaded, so it cannot have drawn a 409, and is not asked about here either.
  result.resolved = ResolveRefusedUploads(client, files, token, *offered, targets, previous,
                                          &result.negotiated.plan, &result.executed, resolve);

  // Always, including after a cancellation and after a 401. The baseline is
  // local and is the expensive thing to lose; the completion call is the cheap
  // one, and with the token plumbed a cancelled tick abandons it without a
  // request (sync_finish.hpp).
  FinishOptions finish = options.finish;
  finish.complete.cancel = options.cancel;
  if (shares.size() > 1) {
    // Each session is told what it planned and saw done (#241). The resolution
    // above replaced operations in place, so the shares still line up.
    finish.counts = CountsFor(result.executed, shares.front().begin, shares.front().end);
  }
  // `offered`, not `reported`: a withheld save is one this tick did not
  // mention, so its marked row stands as it is.
  result.finished = FinishTick(client, files, token, result.negotiated.plan, result.executed,
                               *offered, std::move(previous), finish);
  for (std::size_t index = 1; index < shares.size(); ++index) {
    result.other_sessions.push_back(CompleteSession(
        client, token, shares[index].id,
        CountsFor(result.executed, shares[index].begin, shares[index].end), {}, finish.complete));
  }

  result.session_gone = SessionGone(result.finished.reported.error);
  result.outcome = OutcomeAfter(result.executed, result.finished);
  result.answer = AnswerAfter(result.negotiated.error, result.executed, result.finished);
  return result;
}

}  // namespace rommsync::sync
