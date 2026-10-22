// Resolving an upload the slot guard refused. Read `sync_resolve.hpp` first --
// why there is a second negotiation, and why its answer is merged into the
// tick's own plan rather than reported beside it.
#include "rommsync/sync_resolve.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rommsync/sync_finish.hpp"

namespace rommsync::sync {
namespace {

const ClientSaveState* ReportedFor(const std::vector<ClientSaveState>& reported,
                                   std::int64_t rom_id, const std::optional<std::string>& slot) {
  for (const ClientSaveState& save : reported) {
    if (save.rom_id == rom_id && save.slot == slot) {
      return &save;
    }
  }
  return nullptr;
}

const SyncOperation* PlannedFor(const SyncPlan& plan, std::int64_t rom_id,
                                const std::optional<std::string>& slot) {
  for (const SyncOperation& operation : plan.operations) {
    if (operation.rom_id == rom_id && operation.slot == slot) {
      return &operation;
    }
  }
  return nullptr;
}

/// The counts `ExecutePlan` keeps, recomputed from the operations after the
/// merge replaced some of them. Enumerated for `ExecutePlan`'s reason: a
/// `default` would count whatever outcome is added next as work done.
void Recount(ExecutionReport* report) {
  report->completed = 0;
  report->failed = 0;
  report->not_understood = 0;
  for (const OperationResult& result : report->operations) {
    switch (result.outcome) {
      case OperationOutcome::kFailed:
        ++report->failed;
        break;
      case OperationOutcome::kNotUnderstood:
        ++report->not_understood;
        break;
      case OperationOutcome::kCanceled:
        break;
      case OperationOutcome::kNoOp:
      case OperationOutcome::kUploaded:
      case OperationOutcome::kDownloaded:
      case OperationOutcome::kKeptBoth:
      case OperationOutcome::kRemoved:
      case OperationOutcome::kKept:
        ++report->completed;
        break;
    }
  }
}

}  // namespace

std::optional<SyncOperation> ResolveNewerOnServer(const SyncOperation& head,
                                                  const ClientSaveState* local,
                                                  const state::SaveRecord* last_synced) {
  if (!head.known_action) {
    return std::nullopt;
  }
  if (head.action == Action::kNoOp) {
    return head;
  }
  if (head.action == Action::kDelete) {
    // The slot was emptied between the two negotiations (#233). A delete is
    // the main plan's to obey, on the next tick, against what that tick's scan
    // reports -- never this step's, which exists to take a server copy *in*.
    // It names no save row either, so the check below would refuse it anyway;
    // this says why rather than leaning on that.
    return std::nullopt;
  }
  if (!head.save_id.has_value()) {
    return std::nullopt;
  }

  const bool unchanged = local != nullptr && local->content_hash.has_value() &&
                         last_synced != nullptr &&
                         last_synced->server_content_hash.has_value() &&
                         IsContentHash(*local->content_hash) &&
                         *local->content_hash == *last_synced->server_content_hash;
  const Action action =
      head.action != Action::kConflict && unchanged ? Action::kDownload : Action::kConflict;

  SyncOperation resolved = head;
  if (resolved.action != action) {
    // The server's sentence for an action it did not plan would be a lie on the
    // conflict screen; the guard's own sentence is why the save is there.
    resolved.reason = Reason::kUnrecognized;
    resolved.reason_text = kSlotNewerDetail;
  }
  resolved.action = action;
  resolved.action_text = ToString(action);
  return resolved;
}

void Accumulate(ResolveReport* tick, ResolveReport run) { *tick = std::move(run); }

ResolveReport ResolveRefusedUploads(http::HttpClient& client, fs::FileSystem& files,
                                    const auth::StoredToken& token,
                                    const std::vector<ClientSaveState>& reported,
                                    const std::vector<SaveTarget>& targets,
                                    const state::Baseline& previous, SyncPlan* plan,
                                    ExecutionReport* executed, const ResolveOptions& options) {
  ResolveReport out;
  if (executed->canceled || executed->unauthorized) {
    // A shutdown ends the tick, and a refused token would only be refused again.
    return out;
  }

  std::vector<std::size_t> refused;
  std::vector<ClientSaveState> asked;
  std::vector<std::int64_t> rom_ids;
  for (std::size_t index = 0;
       index < executed->operations.size() && index < plan->operations.size(); ++index) {
    const OperationResult& result = executed->operations[index];
    if (result.error != OperationError::kNewerOnServer) {
      continue;
    }
    refused.push_back(index);
    const bool new_rom =
        std::find(rom_ids.begin(), rom_ids.end(), result.rom_id) == rom_ids.end();
    const ClientSaveState* local = ReportedFor(reported, result.rom_id, result.slot);
    const bool asked_already = ReportedFor(asked, result.rom_id, result.slot) != nullptr;
    if (local == nullptr || (new_rom && rom_ids.size() >= kMaxNegotiateRomIds) ||
        (!asked_already && asked.size() >= kMaxResolvedUploads)) {
      // Past RomM's bound on one scope, or past what one tick resolves
      // (`kMaxResolvedUploads`): not asked about this tick, so it stays refused
      // and the next tick asks.
      continue;
    }
    if (!asked_already) {
      asked.push_back(*local);
    }
    if (new_rom) {
      rom_ids.push_back(result.rom_id);
    }
  }
  out.refused = refused.size();
  if (asked.empty()) {
    // Nothing refused -- or nothing step 0 reported for what was, which cannot
    // be asked about faithfully and is left for the next tick.
    return out;
  }

  if (options.hold != nullptr && options.hold()) {
    // A game launched since the tick's own check (#242) has read the save this
    // would replace. Nothing is sent; the 409s stand for the next tick.
    out.held = true;
    return out;
  }

  // Once per tick, and only the refused saves, scoped to their roms: unscoped,
  // the server answers a narrow question with a download for every save in the
  // library this device has no history for.
  //
  // **Unfiltered** (#236). The guard refused the upload over the slot's newest
  // save from *any* emulator, and the question is what that save is. Filtered,
  // a head written by a lineage this console does not load would stay invisible:
  // the answer would be this console's own previous version, every later upload
  // would draw the same 409, and each would be "resolved" by putting that old
  // version back over the player's progress.
  NegotiateOptions unfiltered = options.negotiate;
  unfiltered.emulators.reset();
  // Of the answer, only the first head for each save asked about is held: the
  // rom scope answers for every save of those roms, up to a whole plan, beside
  // the tick's own run (#248). The rest is the next tick's anyway (below).
  std::vector<bool> headed(asked.size(), false);
  unfiltered.keep = [&asked, &headed](const SyncOperation& operation) {
    for (std::size_t at = 0; at < asked.size(); ++at) {
      if (!headed[at] && asked[at].rom_id == operation.rom_id &&
          asked[at].slot == operation.slot) {
        headed[at] = true;
        return true;
      }
    }
    return false;
  };
  Negotiation second = options.rom_ids
                           ? NegotiateRoms(client, token, asked, rom_ids, unfiltered)
                           : Negotiate(client, token, asked, unfiltered);
  out.renegotiated = true;
  out.negotiate_error = second.error;
  out.negotiate_message = second.message;
  if (!second.ok()) {
    executed->warnings.push_back(
        "the uploads the slot guard refused could not be resolved this tick: negotiate: " +
        std::string(ToString(second.error)) + "; " + second.message);
    return out;
  }

  SyncPlan run;
  run.session_id = second.plan.session_id;
  std::vector<std::size_t> run_at;
  for (const std::size_t index : refused) {
    const OperationResult& was = executed->operations[index];
    const SyncOperation* head = PlannedFor(second.plan, was.rom_id, was.slot);
    if (head == nullptr || ReportedFor(asked, was.rom_id, was.slot) == nullptr ||
        MatchTarget(targets, *head) == nullptr) {
      // Not asked about -- a sibling slot of an asked rom, which the rom scope
      // still answers for -- is the next tick's, as the header says. And a
      // resolution only ever replaces a file step 0 found: with no target,
      // `ExecutePlan` would *place* one, and a folder the scan left out (a Tico
      // account it could not decide, #231) is exactly where it must not.
      continue;
    }
    std::optional<SyncOperation> resolved = ResolveNewerOnServer(
        *head, ReportedFor(reported, was.rom_id, was.slot), previous.Find(was.rom_id, was.slot));
    if (!resolved.has_value()) {
      continue;
    }
    run.operations.push_back(std::move(*resolved));
    run_at.push_back(index);
  }
  // Anything else the second plan named is the next tick's, and the rest of it
  // is not worth holding while the transfers run.
  second.plan.operations.clear();
  second.plan.operations.shrink_to_fit();

  ExecutionReport ran = ExecutePlan(client, files, token, run, targets, options.execute);

  for (std::size_t at = 0; at < ran.operations.size() && at < run_at.size(); ++at) {
    const std::size_t index = run_at[at];
    if (ran.operations[at].outcome == OperationOutcome::kCanceled) {
      // Never attempted: the 409 is still what happened to this save.
      continue;
    }
    // The 409's own line goes: it described an operation this tick then did
    // something about, and the engine logs every warning here as a failed save.
    const auto stale = std::find(executed->warnings.begin(), executed->warnings.end(),
                                 executed->operations[index].message);
    if (stale != executed->warnings.end()) {
      executed->warnings.erase(stale);
    }
    plan->operations[index] = std::move(run.operations[at]);
    executed->operations[index] = std::move(ran.operations[at]);
    ++out.resolved;
  }
  executed->warnings.insert(executed->warnings.end(), ran.warnings.begin(), ran.warnings.end());
  executed->canceled = executed->canceled || ran.canceled;
  executed->unauthorized = executed->unauthorized || ran.unauthorized;
  Recount(executed);

  // The second session is closed with what it saw done. Best-effort: the
  // baseline and the first session's accounting are `FinishTick`'s, and this
  // one only costs a row in a history if it fails.
  out.second_session = CompleteSession(client, token, run.session_id, CountsFor(ran), {},
                                       options.complete);
  return out;
}

}  // namespace rommsync::sync
