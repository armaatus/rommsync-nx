// One tick, in order. Read `sync_tick.hpp` first -- the rules are there.
//
// What is only here is the joining: which stage's failure ends the tick and
// which does not, and the one place the cancel token is fanned out. Nothing in
// this file retries a call. Each stage already has `CallPolicy`, and widening
// what `ShouldRetry` picks is explicitly not this module's to do -- a 401, a 404
// and a refused body are all answers that do not change on a second attempt.
#include "rommsync/sync_tick.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
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
    // Unsplittable here: only an unscoped negotiation reaches this, and it has
    // no rom ids to halve (`RunTick` splits a scoped one before it gets here).
    case NegotiateError::kPlanTooLarge:
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

/// How far from clean an outcome is, for a tick that ran several plans: the
/// furthest is the tick's. Only the outcomes a plan's run can end in are ranked.
int Severity(TickOutcome outcome) {
  switch (outcome) {
    case TickOutcome::kCompleted:
      return 0;
    case TickOutcome::kPartial:
      return 1;
    case TickOutcome::kUnreported:
      return 2;
    case TickOutcome::kUnauthorized:
      return 3;
    case TickOutcome::kCanceled:
      return 4;
    // No plan's run ends in these; enumerated rather than defaulted, so a new
    // outcome is ranked on purpose.
    case TickOutcome::kOffline:
    case TickOutcome::kRefused:
    case TickOutcome::kServerDisabled:
    case TickOutcome::kDisabled:
    case TickOutcome::kRescanNeeded:
    case TickOutcome::kUnsupported:
      break;
  }
  return 1;
}

TickOutcome Worse(TickOutcome left, TickOutcome right) {
  return Severity(right) > Severity(left) ? right : left;
}

/// `chunk` cut in two by its rom ids, each save going with its rom -- the rule
/// `ChunkScope` keeps (rom_scope.hpp) -- and a save whose rom is in neither half
/// with the first, where `ChunkScope` put it.
std::pair<ScopeChunk, ScopeChunk> Halve(const ScopeChunk& chunk,
                                        const std::vector<ClientSaveState>& offered) {
  ScopeChunk lower;
  ScopeChunk upper;
  lower.restore = upper.restore = chunk.restore;
  const std::size_t middle = chunk.rom_ids.size() / 2;
  lower.rom_ids.assign(chunk.rom_ids.begin(), chunk.rom_ids.begin() + middle);
  upper.rom_ids.assign(chunk.rom_ids.begin() + middle, chunk.rom_ids.end());
  for (const std::size_t index : chunk.saves) {
    const std::int64_t rom = offered[index].rom_id;
    const bool in_upper = std::binary_search(upper.rom_ids.begin(), upper.rom_ids.end(), rom);
    (in_upper ? upper : lower).saves.push_back(index);
  }
  return {std::move(lower), std::move(upper)};
}

/// What one plan's run did, before it is folded into the tick.
struct PlanRun {
  ExecutionReport executed;
  ResolveReport resolved;
  TickCompletion finished;
  TickOutcome outcome = TickOutcome::kCompleted;
  auth::Answer answer = auth::Answer::kSilent;
};

/// Execute, resolve and finish one plan, starting from `*baseline` and leaving
/// in it what was written. `first` is the run that carries the play sessions.
PlanRun CarryOut(http::HttpClient& client, fs::FileSystem& files, const auth::StoredToken& token,
                 const std::vector<ClientSaveState>& reported,
                 const std::vector<ClientSaveState>& offered,
                 const std::vector<SaveTarget>& targets, Negotiation* negotiated,
                 state::Baseline* baseline, const TickOptions& options,
                 const NegotiateOptions& negotiate, bool first) {
  PlanRun run;
  ExecuteOptions execute = options.execute;
  execute.cancel = options.cancel;
  execute.send_content_hash = options.server.hash_baselines;
  // A `delete` (#233) is obeyed only where the heartbeat said the server means
  // one, and only on the bytes this tick reported: the server ruled on those.
  execute.delete_action = options.server.delete_action;
  execute.reported = &offered;
  run.executed = ExecutePlan(client, files, token, negotiated->plan, targets, execute);

  // Before the baseline is written, because the baseline is written from the
  // plan and the report this changes: an upload the slot guard refused becomes
  // a download of the server's head, or a conflict, at the same index -- never
  // another upload, and never more than one second negotiation (#232).
  ResolveOptions resolve;
  resolve.rom_ids = options.server.rom_ids;
  resolve.hold = options.hold;
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
  run.resolved = ResolveRefusedUploads(client, files, token, offered, targets, *baseline,
                                       &negotiated->plan, &run.executed, resolve);

  // Always, including after a cancellation and after a 401. The baseline is
  // local and is the expensive thing to lose; the completion call is the cheap
  // one, and with the token plumbed a cancelled tick abandons it without a
  // request (sync_finish.hpp). Each run completes its own session, so the
  // whole report is its count.
  FinishOptions finish = options.finish;
  finish.complete.cancel = options.cancel;
  if (!first) {
    finish.play_sessions.clear();
  }
  // `reported`, not `offered`: nothing was planned for a withheld save, so its
  // marked row stands as it is -- and it is on the card, so a baseline past its
  // bound does not trim it as stale (#233).
  run.finished = FinishTick(client, files, token, negotiated->plan, run.executed, reported,
                            std::move(*baseline), finish, baseline);

  run.outcome = OutcomeAfter(run.executed, run.finished);
  run.answer = AnswerAfter(negotiated->error, run.executed, run.finished);
  return run;
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
  // caller knows them (#241) -- and each chunk's plan carried out on its own,
  // one after another, a chunk too large for one plan split in half until it
  // fits or is one rom (#246; sync_tick.hpp, `TickOptions::rom_scope`).
  const bool scoped = options.rom_scope.has_value() && options.server.rom_ids;
  std::deque<ScopeChunk> pending;
  if (scoped) {
    const std::vector<ScopeChunk> chunks =
        ChunkScope(*offered, *options.rom_scope,
                   options.server.restore_unlisted ? options.restore_roms
                                                   : std::vector<std::int64_t>{});
    pending.assign(chunks.begin(), chunks.end());
  } else {
    pending.emplace_back();
  }

  state::Baseline baseline = std::move(previous);
  TickOutcome outcome = TickOutcome::kCompleted;
  auth::Answer answer = auth::Answer::kSilent;
  int attempts = 0;
  std::chrono::milliseconds waited{0};
  bool backup_made = false;
  bool left_out = false;
  const auto unasked = [&result](const ScopeChunk& chunk) {
    result.unasked_roms.insert(result.unasked_roms.end(), chunk.rom_ids.begin(),
                               chunk.rom_ids.end());
  };
  const auto stop = [&]() {
    for (const ScopeChunk& rest : pending) {
      unasked(rest);
    }
    pending.clear();
  };

  while (!pending.empty()) {
    if (result.totals.plans > 0) {
      // A shutdown, or a game launched while the last plan ran (#242): the
      // chunks left are the next tick's, unasked.
      const bool canceled = Canceled(options.cancel);
      result.held = !canceled && options.hold != nullptr && options.hold();
      if (canceled || result.held) {
        outcome = Worse(outcome, canceled ? TickOutcome::kCanceled : TickOutcome::kPartial);
        stop();
        break;
      }
    }
    ScopeChunk chunk = std::move(pending.front());
    pending.pop_front();
    if (result.totals.plans > 0) {
      // The last plan and what it did were handed to `on_plan` and nothing else
      // reads them, so they go before the next chunk is asked about: kept, they
      // were a second plan and a run's results beside this one arriving, where
      // `kHeapTickSaves` counts one (#248).
      std::vector<SyncOperation>().swap(result.negotiated.plan.operations);
      std::vector<OperationResult>().swap(result.executed.operations);
      std::vector<std::string>().swap(result.executed.warnings);
    }

    Negotiation one;
    if (!scoped) {
      one = Negotiate(client, token, *offered, negotiate);
    } else {
      NegotiateOptions asked = negotiate;
      asked.restore_unlisted = chunk.restore;
      // The whole vector when this chunk carries every save, which is nearly
      // every card: the tick that carries it is the heap's peak, and a copy of
      // it buys nothing. Asked of each chunk, not once: a split's halves carry
      // part each, and RomM plans every rom a save is sent for, so a half sent
      // the rest would plan them all again. (`ChunkScope` and `Halve` give each
      // save to one chunk, so a chunk of `offered`'s size holds all of it.)
      const bool whole = chunk.saves.size() == offered->size();
      std::vector<ClientSaveState> some;
      if (!whole) {
        some.reserve(chunk.saves.size());
        for (const std::size_t index : chunk.saves) {
          some.push_back((*offered)[index]);
        }
      }
      one = NegotiateRoms(client, token, whole ? *offered : some, chunk.rom_ids, asked);
    }
    attempts += one.attempts;
    waited += one.waited;

    if (scoped && one.error == NegotiateError::kPlanTooLarge) {
      if (chunk.rom_ids.size() > 1) {
        // Both halves before the chunks after them, so the order the tick works
        // in is still the scope's.
        std::pair<ScopeChunk, ScopeChunk> halves = Halve(chunk, *offered);
        pending.push_front(std::move(halves.second));
        pending.push_front(std::move(halves.first));
        continue;
      }
      // One rom whose own saves are more than a plan holds: left out of this
      // tick, its saves unoffered and untouched, and named.
      left_out = true;
      result.oversized_roms.insert(result.oversized_roms.end(), chunk.rom_ids.begin(),
                                   chunk.rom_ids.end());
      unasked(chunk);
      continue;
    }

    if (!one.ok()) {
      if (result.totals.plans == 0) {
        // The offline case, and the one the acceptance is sharpest about:
        // nothing after this point runs, so no save is touched, no backup is
        // written and `state.db` is not rewritten.
        unasked(chunk);
        stop();
        one.attempts = attempts;
        one.waited = waited;
        result.outcome = OutcomeOf(one.error);
        result.answer = AnswerOf(one.error);
        result.negotiated = std::move(one);
        std::sort(result.oversized_roms.begin(), result.oversized_roms.end());
        std::sort(result.unasked_roms.begin(), result.unasked_roms.end());
        return result;
      }
      // After a plan ran, what it did stands; the rest waits for the next tick.
      const TickOutcome failed = OutcomeOf(one.error);
      outcome = Worse(outcome, failed == TickOutcome::kUnauthorized || failed == TickOutcome::kCanceled
                                   ? failed
                                   : TickOutcome::kPartial);
      if (const auth::Answer said = AnswerOf(one.error); said != auth::Answer::kSilent) {
        answer = said;
      }
      unasked(chunk);
      stop();
      result.negotiated = std::move(one);
      result.executed = ExecutionReport{};
      break;
    }

    // Only now: nothing needs `.backup/` until an operation does, and a tick that
    // never reached the server must not have written anything to the card.
    if (!backup_made && options.create_backup_dir) {
      backup_made = true;
      const fs::MakeDirResult made = files.CreateDirectory(options.execute.backup_dir);
      if (!made.ok() && result.recovered.warnings.size() < kMaxRecoveryWarnings) {
        // Not fatal. Every operation that would overwrite a save fails with
        // `kBackupFailed` further down -- no backup, no overwrite -- and an
        // upload or a no-op does not need the directory at all, so a tick that
        // can still do half its work does it. Bounded by the same cap the
        // sweep's own lines are, because the list is the same list.
        result.recovered.warnings.push_back("the backup directory " +
                                            options.execute.backup_dir +
                                            " could not be created: " + made.message);
      }
    }

    const bool first = result.totals.plans == 0;
    PlanRun run = CarryOut(client, files, token, reported, *offered, targets, &one, &baseline,
                           options, negotiate, first);
    ++result.totals.plans;
    result.totals.completed += run.executed.completed;
    result.totals.failed += run.executed.failed;
    result.totals.stored = result.totals.stored && run.finished.stored.ok();
    if (options.on_plan) {
      options.on_plan(one.plan, run.executed);
    }
    outcome = Worse(outcome, run.outcome);
    if (run.answer != auth::Answer::kSilent) {
      answer = run.answer;
    }
    if (first) {
      result.session_gone = SessionGone(run.finished.reported.error);
      result.finished = std::move(run.finished);
    } else {
      result.other_sessions.push_back(std::move(run.finished.reported));
      result.finished.warnings.insert(result.finished.warnings.end(),
                                      run.finished.warnings.begin(),
                                      run.finished.warnings.end());
    }
    // The last plan is kept for the caller; any before it is freed here, before
    // the next chunk is asked (`TickOptions::on_plan`).
    result.negotiated = std::move(one);
    result.executed = std::move(run.executed);
    Accumulate(&result.resolved, std::move(run.resolved));

    if (run.outcome == TickOutcome::kUnauthorized || run.outcome == TickOutcome::kCanceled) {
      // The next chunk would meet the same refusal, or the same shutdown.
      stop();
    }
  }

  // Every chunk a rom too large for a plan is a tick in which nothing ran and
  // nothing was written -- `kPartial` below, never `kCompleted`.
  result.negotiated.attempts = attempts;
  result.negotiated.waited = waited;
  if (left_out) {
    outcome = Worse(outcome, TickOutcome::kPartial);
  }
  std::sort(result.oversized_roms.begin(), result.oversized_roms.end());
  std::sort(result.unasked_roms.begin(), result.unasked_roms.end());
  result.outcome = outcome;
  result.answer = answer;
  return result;
}

}  // namespace rommsync::sync
