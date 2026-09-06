// The complete call and the session it answers with -- step 3 of sync.hpp, kept
// out of sync_negotiate.cpp for the reason that file is kept out of sync.cpp:
// they share an idea and no code. This one builds two integers into a body and
// then reads a session strictly enough to believe the counts came back.
//
// What this call is *for* is worth stating, because it is not what the name
// suggests. Nothing is committed here. The uploads and downloads already landed
// on the server while the plan was executing, and RomM has already written the
// sync rows the next negotiation arbitrates against. This writes a history row
// a user reads and closes the session so the next negotiate does not have to
// cancel it. A `complete` that never lands costs a session RomM will show as
// CANCELLED; it costs no save and no arbitration.
//
// Which is exactly why sync_finish.hpp persists the baseline *before* calling
// this: the expensive thing to lose is the client's own record of what it
// hashed, and that is local.
//
// M7-4 added the second thing the body carries -- `play_sessions[]` -- and the
// answer it produces, `play_session_ingest`. Both live here rather than in
// play_sessions.hpp because they are fields of *this* call's request and reply;
// what fills the array and what becomes of the entries afterwards is that
// module's, and it includes this one rather than the other way round.
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/http.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::sync {
namespace {

/// The one status a completed session is supposed to carry, upper-case as RomM
/// spells it (docs/API_CONTRACT.md). Anything else is a warning, not an error:
/// the call was answered, and a session RomM had already cancelled is a thing
/// worth saying rather than a tick to fail.
constexpr const char* kCompletedStatus = "COMPLETED";

/// What RomM answers a 400 with when the session has already been accounted for
/// -- verified against the live 5.2.0. It is what a retry that worked looks
/// like: the first attempt reached the server, the connection died on the way
/// back, and the second is refused because the first succeeded.
constexpr const char* kAlreadyCompletedDetail = "Session is already COMPLETED";

/// And what it answers when another negotiation already took the device --
/// also verified against the live 5.2.0, and the *reachable* half of this 400:
/// every negotiate cancels the device's previous IN_PROGRESS session, so a tick
/// that gave up on one negotiation and made another lands here with the first
/// id (docs/API_CONTRACT.md).
constexpr const char* kSupersededDetail = "Session is already CANCELLED";

/// And what it answers a 404 with when the id names nothing -- `Sync session
/// with ID {id} not found`.
///
/// Gated on the body for the reason `kNoSuchDeviceDetail` is next door: a
/// `server_url` that points at something which is not this RomM answers 404 too,
/// and FastAPI's own is `{"detail":"Not Found"}`. An ungated one falls through
/// to `kRejected`, which is the safer direction to be wrong in.
constexpr const char* kNoSuchSessionDetail = "Sync session with ID";

json::Error Fail(std::string_view field, std::string message) {
  json::Error error;
  error.field = std::string(field);
  error.message = std::move(message);
  return error;
}

Completion Refuse(CompleteError error, std::string message) {
  Completion refused;
  refused.error = error;
  refused.message = std::move(message);
  return refused;
}

/// Everything that can go wrong between sending the request and holding a 2xx
/// body, classified once. Empty when the exchange produced a body worth parsing.
std::optional<Completion> Refused(const http::Result& result) {
  if (result.error == http::Error::kCanceled) {
    // Kept apart from `kUnreachable`, which is what it would otherwise be read
    // as: a caller that treats a shutdown as a transport failure retries it, and
    // the whole point of the token is that nothing else is attempted.
    return Refuse(CompleteError::kCanceled,
                  "the completion was stopped: " +
                      (result.message.empty() ? std::string("the caller cancelled")
                                              : result.message));
  }
  if (!result.ok()) {
    return Refuse(CompleteError::kUnreachable,
                  std::string("the session was not completed: ") + http::ToString(result.error) +
                      (result.message.empty() ? "" : " (" + result.message + ")"));
  }

  const int status = result.response.status;
  if (status == 401) {
    return Refuse(CompleteError::kUnauthorized,
                  "the completion was rejected: HTTP 401; the token has been revoked");
  }
  if (status == 403) {
    return Refuse(CompleteError::kForbidden,
                  "the completion was rejected: HTTP 403; this pairing was not granted the "
                  "scopes sync needs");
  }
  if (status == 404 && result.response.body.find(kNoSuchSessionDetail) != std::string::npos) {
    // Deliberately *not* "a session something else cancelled" -- that one still
    // exists and answers the 400 below. What reaches here is an id that was
    // never a session, which on a working pairing means the id came from
    // somewhere other than a negotiation.
    return Refuse(CompleteError::kNoSuchSession,
                  "the completion was answered 404; RomM has no session with that id at all");
  }
  if (status == 400 && result.response.body.find(kSupersededDetail) != std::string::npos) {
    // Not a refusal of this body either, and not the same news as the branch
    // below: this session was ended by another negotiation, so the counts it was
    // carrying were never recorded and cannot be. The next tick negotiates its
    // own session; there is nothing here to retry or to fix.
    return Refuse(CompleteError::kSuperseded,
                  "this sync session was cancelled by a later negotiation; its counts were not "
                  "recorded, and the next tick opens a session of its own");
  }
  if (status == 400 && result.response.body.find(kAlreadyCompletedDetail) != std::string::npos) {
    // Not a refusal of this body. The session has been accounted for -- almost
    // always by this client's own previous attempt, which reached RomM before
    // the exchange fell over.
    return Refuse(CompleteError::kAlreadyCompleted,
                  "this sync session was already completed; the counts it holds are the ones an "
                  "earlier attempt reported");
  }
  // The same three-way split `Refused` makes next door: a 429 or a 408 is the
  // server declining *now*, and letting either fall through to a non-retryable
  // error would give up on a tick over a rate limiter.
  if (status >= 500 || status == 429 || status == 408) {
    return Refuse(CompleteError::kServerError,
                  "the completion: HTTP " + std::to_string(status) +
                      (status == 429 ? "; the server is rate limiting" : ""));
  }
  if (!result.successful()) {
    return Refuse(CompleteError::kRejected,
                  "the completion was refused: HTTP " + std::to_string(status));
  }
  return std::nullopt;
}

/// Read the `session` object. Returns an error only for a field that is missing
/// or the wrong type -- an unexpected *value* goes to `warnings`.
json::Error ReadSession(const json::Value& value, SyncSession* out) {
  json::Reader reader(value, "sync session");
  reader.Required("id", &out->id);
  reader.Required("device_id", &out->device_id);
  reader.Required("user_id", &out->user_id);
  reader.Required("status", &out->status);
  reader.Required("initiated_at", &out->initiated_at);
  reader.RequiredNullable("completed_at", &out->completed_at);
  reader.Required("operations_planned", &out->operations_planned);
  reader.Required("operations_completed", &out->operations_completed);
  reader.Required("operations_failed", &out->operations_failed);
  reader.RequiredNullable("error_message", &out->error_message);
  reader.Required("created_at", &out->created_at);
  reader.Required("updated_at", &out->updated_at);
  if (!reader.ok()) {
    json::Error error = reader.error();
    error.field = error.field.empty() ? "session" : "session." + error.field;
    return error;
  }
  if (out->id <= 0) {
    return Fail("session.id", "is not a positive session id");
  }
  return {};
}

}  // namespace

const char* ToString(CompleteError error) {
  switch (error) {
    case CompleteError::kNone:
      return "none";
    case CompleteError::kNotRegistered:
      return "not_registered";
    case CompleteError::kNoSession:
      return "no_session";
    case CompleteError::kUnauthorized:
      return "unauthorized";
    case CompleteError::kForbidden:
      return "forbidden";
    case CompleteError::kNoSuchSession:
      return "no_such_session";
    case CompleteError::kAlreadyCompleted:
      return "already_completed";
    case CompleteError::kSuperseded:
      return "superseded";
    case CompleteError::kRejected:
      return "rejected";
    case CompleteError::kUnusablePayload:
      return "unusable_payload";
    case CompleteError::kCanceled:
      return "canceled";
    case CompleteError::kUnreachable:
      return "unreachable";
    case CompleteError::kServerError:
      return "server_error";
    case CompleteError::kMalformed:
      return "malformed";
  }
  return "none";
}

bool ShouldRetry(CompleteError error) {
  return error == CompleteError::kUnreachable || error == CompleteError::kServerError;
}

auth::Answer AnswerOf(CompleteError error) {
  switch (error) {
    case CompleteError::kUnauthorized:
      return auth::Answer::kRejected;
    case CompleteError::kForbidden:
      return auth::Answer::kForbidden;
    // Accepted only where the answer is proof RomM read the token: a session,
    // and the three refusals gated on RomM's own `detail` text (auth_gate.hpp).
    case CompleteError::kNone:
    case CompleteError::kNoSuchSession:
    case CompleteError::kAlreadyCompleted:
    case CompleteError::kSuperseded:
      return auth::Answer::kAccepted;
    // The rest say nothing, `kRejected` -- a bare 4xx -- included.
    case CompleteError::kRejected:
    case CompleteError::kNotRegistered:
    case CompleteError::kNoSession:
    case CompleteError::kUnusablePayload:
    case CompleteError::kCanceled:
    case CompleteError::kUnreachable:
    case CompleteError::kServerError:
    case CompleteError::kMalformed:
      break;
  }
  return auth::Answer::kSilent;
}

const char* ToString(IngestStatus status) {
  switch (status) {
    case IngestStatus::kCreated:
      return "created";
    case IngestStatus::kDuplicate:
      return "duplicate";
    case IngestStatus::kError:
      return "error";
  }
  return "error";
}

bool Ingested(IngestStatus status) {
  return status == IngestStatus::kCreated || status == IngestStatus::kDuplicate;
}

json::Error Validate(const PlaySession& session) {
  const std::int64_t start = UnixSeconds(session.start_time);
  const std::int64_t end = UnixSeconds(session.end_time);
  if (start < kMinTimestampSeconds || start > kMaxTimestampSeconds) {
    // The epoch is what an unset console clock produces, and a session stamped
    // with it is play time RomM would file under 1970 forever. Refused for
    // `ClientSaveState::updated_at`'s reason, and the same bound.
    return Fail("start_time", "is not an instant this client can spell");
  }
  if (end < kMinTimestampSeconds || end > kMaxTimestampSeconds) {
    return Fail("end_time", "is not an instant this client can spell");
  }
  if (end < start) {
    return Fail("end_time", "is before start_time");
  }
  if (session.duration_ms < 0) {
    return Fail("duration_ms", "is negative");
  }
  // A duration longer than the window it sits in is arithmetic that went wrong
  // upstream. It reaches a user as play time that never happened, in a total
  // nothing else contradicts, so it is refused here rather than clamped: a
  // clamp would hide the bug and still report a number nobody derived.
  const std::int64_t window_ms = (end - start) * 1000;
  if (session.duration_ms > window_ms) {
    return Fail("duration_ms", "is longer than the window between start_time and end_time");
  }
  if (session.rom_id.has_value() && *session.rom_id <= 0) {
    return Fail("rom_id", "is not a positive rom id");
  }
  if (session.save_slot.has_value() && session.save_slot->empty()) {
    // `""` and `null` are different values to the server and only one of them
    // is a value -- `Validate(ClientSaveState)`'s rule on `slot`.
    return Fail("save_slot", "is present and empty; send null instead");
  }
  return {};
}

Encoded EncodePlaySessions(const std::vector<PlaySession>& sessions) {
  Encoded encoded;
  std::string body("[");
  for (std::size_t index = 0; index < sessions.size(); ++index) {
    const PlaySession& session = sessions[index];
    if (const json::Error error = Validate(session); !error.ok()) {
      encoded.error = error;
      encoded.error.field = "play_sessions[" + std::to_string(index) + "]." + error.field;
      return encoded;
    }
    if (index != 0) {
      body += ',';
    }
    // Field order follows the snapshot, for `EncodeNegotiateRequest`'s reason:
    // nothing depends on it, and it makes an encoded body and a captured one
    // diffable by eye.
    body += "{\"rom_id\":";
    body += session.rom_id.has_value() ? std::to_string(*session.rom_id) : "null";
    body += ",\"save_slot\":";
    body += session.save_slot.has_value() ? json::Quote(*session.save_slot) : "null";
    body += ",\"start_time\":";
    body += json::Quote(FormatTimestamp(session.start_time));
    body += ",\"end_time\":";
    body += json::Quote(FormatTimestamp(session.end_time));
    body += ",\"duration_ms\":";
    body += std::to_string(session.duration_ms);
    body += '}';
  }
  body += ']';
  encoded.body = std::move(body);
  return encoded;
}

auth::Parsed<PlaySessionIngest> ParseIngestResponse(const json::Value& value,
                                                    std::string_view context) {
  auth::Parsed<PlaySessionIngest> parsed;
  PlaySessionIngest ingest;

  json::Reader reader(value, context);
  reader.Required("created_count", &ingest.created_count);
  reader.Required("skipped_count", &ingest.skipped_count);
  if (!reader.ok()) {
    parsed.error = reader.error();
    return parsed;
  }

  const json::Value* results = value.Find("results");
  if (results == nullptr || !results->is_array()) {
    parsed.error = Fail("results", "is missing or not an array");
    return parsed;
  }
  ingest.results.reserve(results->elements().size());
  for (std::size_t at = 0; at < results->elements().size(); ++at) {
    PlaySessionIngestResult result;
    std::string status;
    json::Reader row(results->elements()[at], "play session ingest result");
    row.Required("index", &result.index);
    row.Required("status", &status);
    row.RequiredNullable("id", &result.id);
    row.RequiredNullable("detail", &result.detail);
    if (!row.ok()) {
      parsed.error = row.error();
      parsed.error.field = "results[" + std::to_string(at) + "]." + parsed.error.field;
      return parsed;
    }
    if (status == "created") {
      result.status = IngestStatus::kCreated;
    } else if (status == "duplicate") {
      result.status = IngestStatus::kDuplicate;
    } else if (status == "error") {
      result.status = IngestStatus::kError;
    } else {
      // Not downgraded to `kError`, which is what a plan's unknown `action`
      // does, because the two defaults cost opposite things. An unknown action
      // must not overwrite a save, so the safe default is "do nothing"; an
      // unknown *status* would decide whether a recorded session may be
      // dropped, and the safe default there is to refuse the answer and keep
      // the session, which is one duplicate on the next flush.
      parsed.error = Fail("results[" + std::to_string(at) + "].status",
                          "is not created, duplicate or error");
      return parsed;
    }
    if (result.index < 0) {
      parsed.error = Fail("results[" + std::to_string(at) + "].index", "is negative");
      return parsed;
    }
    ingest.results.push_back(std::move(result));
  }

  parsed.value = std::move(ingest);
  return parsed;
}

Encoded EncodeCompleteRequest(const CompletionCounts& counts,
                              const std::vector<PlaySession>& play_sessions) {
  Encoded encoded;
  if (counts.operations_completed < 0) {
    encoded.error = Fail("operations_completed", "is negative");
    return encoded;
  }
  if (counts.operations_failed < 0) {
    encoded.error = Fail("operations_failed", "is negative");
    return encoded;
  }
  const Encoded sessions = EncodePlaySessions(play_sessions);
  if (!sessions.ok()) {
    encoded.error = sessions.error;
    return encoded;
  }
  encoded.body = "{\"operations_completed\":" + std::to_string(counts.operations_completed) +
                 ",\"operations_failed\":" + std::to_string(counts.operations_failed) +
                 ",\"play_sessions\":" + sessions.body + "}";
  return encoded;
}

auth::Parsed<SyncCompletion> ParseCompleteResponse(std::string_view body) {
  auth::Parsed<SyncCompletion> parsed;
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    // Where a truncated body lands, and it must: half a session that parsed
    // would be counts nobody can tell from the ones that were reported.
    parsed.error = document.error;
    return parsed;
  }

  const json::Value* session = document.value.Find("session");
  if (session == nullptr) {
    parsed.error = Fail("session", "is missing");
    return parsed;
  }

  SyncCompletion completion;
  if (const json::Error error = ReadSession(*session, &completion.session); !error.ok()) {
    parsed.error = error;
    return parsed;
  }

  // Present-and-null is the ordinary answer and the schema does not require the
  // key at all, so only a non-null object is read.
  //
  // **An ingest this build cannot read is a warning, never an error**, and that
  // is the whole feature's rule applied to the answer half: a play session may
  // not cost a sync tick. Failing here would make `CompleteError::kMalformed`
  // out of a completion the server performed, which `sync_tick.hpp` turns into a
  // failed `TickOutcome` and a scheduler backoff -- over play time, on a tick
  // whose saves are already synced.
  //
  // Leaving `play_session_ingest` absent is exactly the right fallback because
  // absent already means "the server said nothing about them": `play::Reconcile`
  // is never reached, nothing is released, and the sessions go out again on the
  // next tick to be answered `duplicate`. The cost of a RomM that changed this
  // shape is therefore one duplicate per tick, not a console that stops syncing.
  if (const json::Value* ingest = document.value.Find("play_session_ingest");
      ingest != nullptr && !ingest->is_null()) {
    auth::Parsed<PlaySessionIngest> parsed_ingest =
        ParseIngestResponse(*ingest, "play session ingest");
    if (parsed_ingest.ok()) {
      for (const PlaySessionIngestResult& result : parsed_ingest.value.results) {
        if (result.status == IngestStatus::kError) {
          // RomM's own sentence, carried rather than classified: nothing here
          // can act on it, and a caller that logs one list must still be able to
          // see that a session was refused rather than recorded.
          completion.warnings.push_back(
              "the server refused play session " + std::to_string(result.index) + ": " +
              (result.detail.has_value() ? *result.detail : std::string("no reason given")));
        }
      }
      completion.play_session_ingest = std::move(parsed_ingest.value);
    } else {
      // `play_session_ingest` is deliberately left absent, and the completion
      // carries on -- the two checks below still run, because a session that
      // came back CANCELLED is news whatever became of the play time.
      completion.warnings.push_back(
          "the server's play_session_ingest could not be read, so the play sessions this tick "
          "sent stay buffered and are sent again: " +
          parsed_ingest.error.Describe());
    }
  }

  if (completion.session.status != kCompletedStatus) {
    // The expected way to see this is a session another negotiation already
    // cancelled -- each negotiate cancels the device's previous IN_PROGRESS one
    // (docs/API_CONTRACT.md). The counts in it are then RomM's, not this tick's.
    completion.warnings.push_back("the session came back \"" + completion.session.status +
                                  "\" rather than " + kCompletedStatus +
                                  "; its counts are not this tick's");
  }
  if (completion.session.error_message.has_value()) {
    completion.warnings.push_back("the server attached an error_message to the session: " +
                                  *completion.session.error_message);
  }

  parsed.value = std::move(completion);
  return parsed;
}

Completion CompleteSession(http::HttpClient& client, const auth::StoredToken& token,
                           std::int64_t session_id, const CompletionCounts& counts,
                           const std::vector<PlaySession>& play_sessions,
                           const CompleteOptions& options) {
  if (token.server_url.empty() || token.access_token.empty()) {
    return Refuse(CompleteError::kNotRegistered, "this console is not paired");
  }
  if (session_id <= 0) {
    // A tick whose negotiation never produced a plan has nothing to complete,
    // and a `0` in the path would be a request for someone else's session.
    return Refuse(CompleteError::kNoSession,
                  "there is no sync session to complete; the negotiation produced no plan");
  }
  const Encoded encoded = EncodeCompleteRequest(counts, play_sessions);
  if (!encoded.ok()) {
    return Refuse(CompleteError::kUnusablePayload,
                  "the completion body could not be built: " + encoded.error.Describe());
  }

  http::Request request;
  request.method = http::Method::kPost;
  request.url = http::JoinUrl(token.server_url, "/api/sync/sessions/" +
                                                    std::to_string(session_id) + "/complete");
  request.headers.push_back({"Accept", "application/json"});
  request.headers.push_back({"Content-Type", "application/json"});
  request.headers.push_back({"Authorization", "Bearer " + token.access_token});
  request.body = encoded.body;
  request.timeout = options.timeout;
  request.cancel = options.cancel;

  if (options.cancel != nullptr && options.cancel->canceled()) {
    // Checked before the first attempt as well as between them. A tick cancelled
    // during execution reaches here with the token already fired, and spending a
    // request to find that out is the delay the token exists to avoid.
    return Refuse(CompleteError::kCanceled,
                  "the completion was not attempted: the caller cancelled first");
  }

  const int attempts = options.max_attempts > 0 ? options.max_attempts : 1;
  // Clamped before the first use rather than after the first doubling, exactly
  // as `Negotiate` does it: a caller whose `backoff` already exceeds
  // `max_backoff` would otherwise wait past the ceiling once.
  std::chrono::milliseconds backoff =
      options.backoff > options.max_backoff ? options.max_backoff : options.backoff;
  std::chrono::milliseconds waited{0};

  for (int attempt = 1;; ++attempt) {
    const http::Result result = client.Send(request);
    Completion outcome;
    if (const std::optional<Completion> refused = Refused(result)) {
      outcome = *refused;
    } else {
      auth::Parsed<SyncCompletion> parsed = ParseCompleteResponse(result.response.body);
      if (parsed.ok()) {
        outcome.value = std::move(parsed.value);
      } else {
        outcome = Refuse(CompleteError::kMalformed,
                         "the completed session could not be read: " + parsed.error.Describe());
      }
    }
    outcome.attempts = attempt;
    outcome.waited = waited;

    if (outcome.ok() || !ShouldRetry(outcome.error) || attempt >= attempts) {
      return outcome;
    }
    if (options.cancel != nullptr && options.cancel->canceled()) {
      // A retryable failure is still a failure to stop on once the caller has
      // given up: the backoff is time a shutdown does not have.
      outcome = Refuse(CompleteError::kCanceled,
                       "the completion was not retried: the caller cancelled");
      outcome.attempts = attempt;
      outcome.waited = waited;
      return outcome;
    }
    if (options.wait != nullptr) {
      options.wait(backoff);
    } else {
      std::this_thread::sleep_for(backoff);
    }
    waited += backoff;
    backoff = backoff * 2 > options.max_backoff ? options.max_backoff : backoff * 2;
  }
}

}  // namespace rommsync::sync
