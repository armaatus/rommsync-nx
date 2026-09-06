// One tick, in order. Read `sync_tick.hpp` first -- the rules are there.
//
// What is only here is the joining: which stage's failure ends the tick and
// which does not, and the one place the cancel token is fanned out. Nothing in
// this file retries a call. Each stage already has `CallPolicy`, and widening
// what `ShouldRetry` picks is explicitly not this module's to do -- a 401, a 404
// and a refused body are all answers that do not change on a second attempt.
#include "rommsync/sync_tick.hpp"

#include <string>
#include <utility>
#include <vector>

#include "rommsync/auth_gate.hpp"
#include "rommsync/file_system.hpp"

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
    case NegotiateError::kUnusablePayload:
    case NegotiateError::kNotRegistered:
    case NegotiateError::kNoSuchDevice:
    case NegotiateError::kSyncDisabled:
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
    case TickOutcome::kUnauthorized:
      return "unauthorized";
    case TickOutcome::kCanceled:
      return "canceled";
    case TickOutcome::kRescanNeeded:
      return "rescan_needed";
  }
  return "unknown";
}

TickResult RunTick(http::HttpClient& client, fs::FileSystem& files,
                   const auth::StoredToken& token,
                   const std::vector<ClientSaveState>& reported,
                   const std::vector<SaveTarget>& targets, state::Baseline previous,
                   const TickOptions& options) {
  TickResult result;
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

  NegotiateOptions negotiate = options.negotiate;
  negotiate.cancel = options.cancel;
  result.negotiated = Negotiate(client, token, reported, negotiate);
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
  result.executed =
      ExecutePlan(client, files, token, result.negotiated.plan, targets, execute);

  // Always, including after a cancellation and after a 401. The baseline is
  // local and is the expensive thing to lose; the completion call is the cheap
  // one, and with the token plumbed a cancelled tick abandons it without a
  // request (sync_finish.hpp).
  FinishOptions finish = options.finish;
  finish.complete.cancel = options.cancel;
  result.finished = FinishTick(client, files, token, result.negotiated.plan, result.executed,
                               reported, std::move(previous), finish);

  result.session_gone = SessionGone(result.finished.reported.error);
  result.outcome = OutcomeAfter(result.executed, result.finished);
  result.answer = AnswerAfter(result.negotiated.error, result.executed, result.finished);
  return result;
}

}  // namespace rommsync::sync
