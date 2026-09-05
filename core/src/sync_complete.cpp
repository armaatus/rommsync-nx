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
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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

Encoded EncodeCompleteRequest(const CompletionCounts& counts) {
  Encoded encoded;
  if (counts.operations_completed < 0) {
    encoded.error = Fail("operations_completed", "is negative");
    return encoded;
  }
  if (counts.operations_failed < 0) {
    encoded.error = Fail("operations_failed", "is negative");
    return encoded;
  }
  encoded.body = "{\"operations_completed\":" + std::to_string(counts.operations_completed) +
                 ",\"operations_failed\":" + std::to_string(counts.operations_failed) +
                 ",\"play_sessions\":[]}";
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
  // key at all, so only a non-null object is worth a word.
  const json::Value* ingest = document.value.Find("play_session_ingest");
  completion.play_session_ingest = ingest != nullptr && !ingest->is_null();
  if (completion.play_session_ingest) {
    completion.warnings.push_back(
        "the server answered a play_session_ingest for a completion that sent no play sessions; "
        "play sessions are M6's and nothing here reads it");
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
  const Encoded encoded = EncodeCompleteRequest(counts);
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
