// The negotiate call and the plan it answers with -- the other half of
// sync.hpp, kept out of sync.cpp because the two halves share nothing but the
// endpoint: one builds a body and refuses what it cannot say, the other makes a
// request and refuses what it cannot read.
#include <chrono>
#include <cstddef>
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

constexpr const char* kNegotiatePath = "/api/sync/negotiate";

/// What RomM answers a 400 with when the user has turned sync off for the
/// device. The status alone cannot say so -- a 422 shape error is also a 4xx
/// with a `detail` -- and the remedy is a switch in RomM's own UI, not a retry
/// and not a re-pair, so it is worth separating on the string
/// (docs/API_CONTRACT.md#device-registration).
constexpr const char* kSyncDisabledDetail = "Sync is disabled for this device";

/// And what it answers a 404 with when the device is gone -- `Device with ID
/// {device_id} not found`.
///
/// Gated on the body for the same reason the 400 is, and the cost of not gating
/// is higher here: a `server_url` that points at something which is not this
/// RomM -- a reverse proxy that does not route `/api/`, a path prefix the user
/// left out -- answers 404 too, and FastAPI's own is `{"detail":"Not Found"}`.
/// Reading that as "your device was deleted" sends the user to a pairing screen
/// for a problem re-pairing cannot fix, and discards a working token on the way.
/// A 404 that does not carry this falls through to `kRejected`, which is the
/// safer direction to be wrong in: it neither retries forever nor re-pairs.
constexpr const char* kNoSuchDeviceDetail = "Device with ID";

/// One mapping between an enum and the string RomM writes, in one place, so the
/// classifier and the speller cannot drift apart.
struct ReasonSpelling {
  Reason reason;
  const char* slug;
  const char* text;
};

constexpr ReasonSpelling kReasons[] = {
    {Reason::kClientOnly, "client_only", "Save exists on client but not on server"},
    {Reason::kClientNewerNoHistory, "client_newer_no_history",
     "Client save is newer (no sync history)"},
    {Reason::kClientNewer, "client_newer", "Client save is newer than last sync"},
    {Reason::kServerOnly, "server_only", "Save exists on server but not on client"},
    {Reason::kServerNewerNoHistory, "server_newer_no_history",
     "Server save is newer (no sync history)"},
    {Reason::kServerNewer, "server_newer", "Server save is newer than last sync"},
    {Reason::kServerChangedClientMissing, "server_changed_client_missing",
     "Server save updated since last sync, not present on client"},
    {Reason::kBothChanged, "both_changed", "Both sides changed since last sync"},
    {Reason::kSameTimestampDifferentContent, "same_timestamp_different_content",
     "Same timestamp but different content"},
    {Reason::kContentIdentical, "content_identical", "Content is identical"},
    {Reason::kNoChanges, "no_changes", "No changes since last sync"},
    {Reason::kAppearIdentical, "appear_identical", "Saves appear identical"},
    {Reason::kUntracked, "untracked", "Save is untracked on this device"},
};

const ReasonSpelling* SpellingOf(Reason reason) {
  for (const ReasonSpelling& spelling : kReasons) {
    if (spelling.reason == reason) {
      return &spelling;
    }
  }
  return nullptr;
}

json::Error Fail(std::string_view field, std::string message) {
  json::Error error;
  error.field = std::string(field);
  error.message = std::move(message);
  return error;
}

Negotiation Refuse(NegotiateError error, std::string message) {
  Negotiation refused;
  refused.error = error;
  refused.message = std::move(message);
  return refused;
}

/// Read one operation, appending anything the client did not fully understand
/// to `warnings`. Returns an error only for a field that is missing or the
/// wrong type -- an unknown *value* is a server that moved, not a broken body.
json::Error ReadOperation(const json::Value& value, std::size_t index, SyncOperation* out,
                          std::vector<std::string>* warnings) {
  const std::string where = "operations[" + std::to_string(index) + "]";

  json::Reader reader(value, "sync operation");
  reader.Required("action", &out->action_text);
  reader.Required("rom_id", &out->rom_id);
  reader.RequiredNullable("save_id", &out->save_id);
  reader.Required("file_name", &out->file_name);
  reader.RequiredNullable("slot", &out->slot);
  reader.RequiredNullable("emulator", &out->emulator);
  reader.Required("reason", &out->reason_text);
  reader.RequiredNullable("server_updated_at", &out->server_updated_at);
  reader.RequiredNullable("server_content_hash", &out->server_content_hash);
  if (!reader.ok()) {
    json::Error error = reader.error();
    // An element that is not an object at all fails in the reader's constructor,
    // which names the context rather than a field -- so there is nothing to put
    // a dot in front of, and `operations[0].` would name nothing.
    error.field = error.field.empty() ? where : where + "." + error.field;
    return error;
  }

  // RomM's ids are positive. A `0` would be an operation naming no rom, which
  // nothing downstream can match to a local file, and which would otherwise
  // reach M2-5 as a plausible-looking entry.
  if (out->rom_id <= 0) {
    return Fail(where + ".rom_id", "is not a positive rom id");
  }
  if (out->save_id.has_value() && *out->save_id <= 0) {
    return Fail(where + ".save_id", "is present but not a positive save id");
  }

  out->action = ClassifyAction(out->action_text, &out->known_action);
  if (!out->known_action) {
    warnings->push_back(where + ": unknown action \"" + out->action_text +
                        "\"; treated as no_op -- this server is newer than this client");
  }
  out->reason = ClassifyReason(out->reason_text);
  if (out->reason == Reason::kUnrecognized) {
    warnings->push_back(where + ": unknown reason \"" + out->reason_text +
                        "\"; the action was still obeyed");
  }
  if (!IsSingleFileName(out->file_name)) {
    warnings->push_back(where + ": the server's file_name \"" + out->file_name +
                        "\" is not a single path component; it must not be joined into a "
                        "local path");
  }
  // The same shape `Validate` holds an outgoing `content_hash` to, applied to the
  // one coming back -- reported rather than refused, because this digest is
  // whatever some *other* client uploaded and RomM stores what it is sent. A
  // SHA1 or an uppercase digest here compares equal to nothing, so the save it
  // belongs to negotiates as changed on every tick, forever, with no other
  // symptom. That is the failure the outgoing check exists to prevent, arriving
  // from the other direction (docs/API_CONTRACT.md).
  if (out->server_content_hash.has_value() && !IsContentHash(*out->server_content_hash)) {
    warnings->push_back(where + ": the server's content_hash is " +
                        std::to_string(out->server_content_hash->size()) +
                        " characters and not lowercase hex; saves are compared on MD5, so this "
                        "one will match nothing");
  }
  return {};
}

/// Everything that can go wrong between sending the request and holding a 2xx
/// body, classified once.
///
/// Empty when the exchange produced a body worth parsing.
std::optional<Negotiation> Refused(const http::Result& result) {
  if (result.error == http::Error::kCanceled) {
    // Kept apart from `kUnreachable`, which is what a caller would otherwise
    // read it as and retry. A cancelled call is one nobody is waiting for.
    return Refuse(NegotiateError::kCanceled,
                  "the negotiation was stopped: " +
                      (result.message.empty() ? std::string("the caller cancelled")
                                              : result.message));
  }
  if (!result.ok()) {
    return Refuse(NegotiateError::kUnreachable,
                  std::string("the negotiation did not complete: ") +
                      http::ToString(result.error) +
                      (result.message.empty() ? "" : " (" + result.message + ")"));
  }

  const int status = result.response.status;
  if (status == 401) {
    // Nothing to refresh: `expires_at` is null on every 5.2.0 token, so this is
    // the token having been revoked (docs/AUTH.md#re-pairing--revocation).
    return Refuse(NegotiateError::kUnauthorized,
                  "the negotiation was rejected: HTTP 401; the token has been revoked");
  }
  if (status == 403) {
    // Deliberately not folded into the 401. RomM approves what the *user*
    // ticked, which need not be what was requested, so a 403 is a scope missing
    // from a pairing that is otherwise working -- and the client is meant to
    // have read `scopes` back off the token rather than meet it here
    // (docs/AUTH.md#scopes-to-request). Reporting it as a revocation sends the
    // user looking for something that did not happen.
    return Refuse(NegotiateError::kForbidden,
                  "the negotiation was rejected: HTTP 403; this pairing was not granted the "
                  "scopes sync needs");
  }
  if (status == 404 && result.response.body.find(kNoSuchDeviceDetail) != std::string::npos) {
    return Refuse(NegotiateError::kNoSuchDevice,
                  "the negotiation was answered 404; this device was deleted in RomM");
  }
  if (status == 400 && result.response.body.find(kSyncDisabledDetail) != std::string::npos) {
    return Refuse(NegotiateError::kSyncDisabled,
                  "sync is turned off for this device in RomM; turn it back on there");
  }
  // The same three-way split `auth::Refused` makes, and for the same reason: a
  // 429 or a 408 is the server declining *now*, and letting either fall through
  // to a non-retryable error would wedge a tick over a rate limiter.
  if (status >= 500 || status == 429 || status == 408) {
    return Refuse(NegotiateError::kServerError,
                  "the negotiation: HTTP " + std::to_string(status) +
                      (status == 429 ? "; the server is rate limiting" : ""));
  }
  if (!result.successful()) {
    return Refuse(NegotiateError::kRejected,
                  "the negotiation was refused: HTTP " + std::to_string(status));
  }
  return std::nullopt;
}

}  // namespace

const char* ToString(Action action) {
  switch (action) {
    case Action::kUpload:
      return "upload";
    case Action::kDownload:
      return "download";
    case Action::kConflict:
      return "conflict";
    case Action::kNoOp:
      return "no_op";
  }
  return "no_op";
}

Action ClassifyAction(std::string_view action, bool* recognized) {
  if (recognized != nullptr) {
    *recognized = true;
  }
  if (action == "upload") {
    return Action::kUpload;
  }
  if (action == "download") {
    return Action::kDownload;
  }
  if (action == "conflict") {
    return Action::kConflict;
  }
  if (action == "no_op") {
    return Action::kNoOp;
  }
  if (recognized != nullptr) {
    *recognized = false;
  }
  // The safe default, and the only one: every other action can overwrite a save.
  return Action::kNoOp;
}

const char* ToString(Reason reason) {
  const ReasonSpelling* spelling = SpellingOf(reason);
  return spelling != nullptr ? spelling->slug : "unrecognized";
}

const char* ReasonText(Reason reason) {
  const ReasonSpelling* spelling = SpellingOf(reason);
  return spelling != nullptr ? spelling->text : "";
}

Reason ClassifyReason(std::string_view reason) {
  for (const ReasonSpelling& spelling : kReasons) {
    if (reason == spelling.text) {
      return spelling.reason;
    }
  }
  return Reason::kUnrecognized;
}

const char* ToString(NegotiateError error) {
  switch (error) {
    case NegotiateError::kNone:
      return "none";
    case NegotiateError::kUnusablePayload:
      return "unusable_payload";
    case NegotiateError::kNotRegistered:
      return "not_registered";
    case NegotiateError::kUnauthorized:
      return "unauthorized";
    case NegotiateError::kForbidden:
      return "forbidden";
    case NegotiateError::kNoSuchDevice:
      return "no_such_device";
    case NegotiateError::kSyncDisabled:
      return "sync_disabled";
    case NegotiateError::kRejected:
      return "rejected";
    case NegotiateError::kCanceled:
      return "canceled";
    case NegotiateError::kUnreachable:
      return "unreachable";
    case NegotiateError::kServerError:
      return "server_error";
    case NegotiateError::kMalformed:
      return "malformed";
  }
  return "none";
}

bool ShouldRetry(NegotiateError error) {
  return error == NegotiateError::kUnreachable || error == NegotiateError::kServerError;
}

bool NeedsPairing(NegotiateError error) {
  return error == NegotiateError::kNotRegistered || error == NegotiateError::kUnauthorized ||
         error == NegotiateError::kForbidden || error == NegotiateError::kNoSuchDevice;
}

auth::Answer AnswerOf(NegotiateError error) {
  switch (error) {
    case NegotiateError::kUnauthorized:
      return auth::Answer::kRejected;
    case NegotiateError::kForbidden:
      return auth::Answer::kForbidden;
    // Accepted only where the answer is proof RomM read the token: a plan, and
    // the two refusals gated on RomM's own `detail` text.
    case NegotiateError::kNone:
    case NegotiateError::kNoSuchDevice:
    case NegotiateError::kSyncDisabled:
      return auth::Answer::kAccepted;
    // The rest say nothing. `kRejected` is a bare 4xx, which anything in front
    // of RomM can answer, so it does not clear a count either.
    case NegotiateError::kRejected:
    case NegotiateError::kUnusablePayload:
    case NegotiateError::kNotRegistered:
    case NegotiateError::kCanceled:
    case NegotiateError::kUnreachable:
    case NegotiateError::kServerError:
    case NegotiateError::kMalformed:
      break;
  }
  return auth::Answer::kSilent;
}

auth::Parsed<SyncPlan> ParseNegotiateResponse(std::string_view body) {
  auth::Parsed<SyncPlan> parsed;
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    // A truncated body lands here, and it must: half a plan that parsed would be
    // a plan with saves missing from it, which looks exactly like a device that
    // is already in sync.
    parsed.error = document.error;
    return parsed;
  }

  SyncPlan plan;
  json::Reader reader(document.value, "negotiate response");
  reader.Required("session_id", &plan.session_id);
  reader.Required("total_upload", &plan.total_upload);
  reader.Required("total_download", &plan.total_download);
  reader.Required("total_conflict", &plan.total_conflict);
  reader.Required("total_no_op", &plan.total_no_op);
  if (!reader.ok()) {
    parsed.error = reader.error();
    return parsed;
  }
  if (plan.session_id <= 0) {
    parsed.error = Fail("session_id", "is not a positive session id");
    return parsed;
  }

  const json::Value* operations = document.value.Find("operations");
  if (operations == nullptr) {
    parsed.error = Fail("operations", "is missing");
    return parsed;
  }
  if (!operations->is_array()) {
    // An empty array is a fully-synced device and is fine; anything that is not
    // an array is a shape that moved.
    parsed.error =
        Fail("operations", std::string("expected an array, got ") + json::ToString(operations->type()));
    return parsed;
  }

  plan.operations.reserve(operations->size());
  for (std::size_t index = 0; index < operations->elements().size(); ++index) {
    SyncOperation operation;
    const json::Error error =
        ReadOperation(operations->elements()[index], index, &operation, &plan.warnings);
    if (!error.ok()) {
      parsed.error = error;
      return parsed;
    }
    plan.operations.push_back(std::move(operation));
  }

  parsed.value = std::move(plan);
  return parsed;
}

Negotiation Negotiate(http::HttpClient& client, const auth::StoredToken& token,
                      const std::vector<ClientSaveState>& saves,
                      const NegotiateOptions& options) {
  if (token.server_url.empty() || token.access_token.empty()) {
    return Refuse(NegotiateError::kNotRegistered, "this console is not paired");
  }
  if (token.device_id.empty()) {
    // Sent explicitly rather than left to the token, even though the snapshot
    // marks it optional: a token that is not device-bound would negotiate as
    // nobody, and every save would come back as a first encounter.
    return Refuse(NegotiateError::kNotRegistered,
                  "the stored token names no device; there is nothing to negotiate for");
  }

  // Checked here as well as inside the encoder, only to say *which* save. The
  // encoder names `saves[3].updated_at`, and an index into a vector this call
  // built is not something anyone can go and look at; the rom and the slot are.
  // Neither is the save's own name, which is a game title a user chose and which
  // `json::Error` would not quote back anyway.
  for (std::size_t index = 0; index < saves.size(); ++index) {
    const json::Error invalid = Validate(saves[index]);
    if (invalid.ok()) {
      continue;
    }
    // The whole payload is refused, not just this entry, and that is M2-1's
    // decision rather than a shortcut here: a save dropped from the request has
    // no `updated_at` for the server to arbitrate on, so the plan can answer
    // `download` and overwrite the very file that could not be described. One
    // tick lost is the cheaper failure -- but it is lost on every tick until the
    // save is fixed, so the log line has to be enough to fix it.
    return Refuse(NegotiateError::kUnusablePayload,
                  "the negotiation body could not be built: rom " +
                      std::to_string(saves[index].rom_id) + ", slot " +
                      (saves[index].slot.has_value() ? *saves[index].slot : "<none>") + ": " +
                      invalid.Describe());
  }

  SyncNegotiatePayload payload;
  payload.device_id = token.device_id;
  payload.saves = saves;
  const Encoded encoded = EncodeNegotiateRequest(payload);
  if (!encoded.ok()) {
    // The payload's own fields -- a blank `device_id` -- which the loop above
    // does not cover. Nothing is sent either way.
    return Refuse(NegotiateError::kUnusablePayload,
                  "the negotiation body could not be built: " + encoded.error.Describe());
  }

  http::Request request;
  request.method = http::Method::kPost;
  request.url = http::JoinUrl(token.server_url, kNegotiatePath);
  request.headers.push_back({"Accept", "application/json"});
  request.headers.push_back({"Content-Type", "application/json"});
  request.headers.push_back({"Authorization", "Bearer " + token.access_token});
  request.body = encoded.body;
  request.timeout = options.timeout;
  request.cancel = options.cancel;

  if (options.cancel != nullptr && options.cancel->canceled()) {
    // Before the first attempt as well as between them: a tick that was
    // cancelled while it was still scanning must not spend a request finding
    // that out.
    return Refuse(NegotiateError::kCanceled,
                  "the negotiation was not attempted: the caller cancelled first");
  }

  const int attempts = options.max_attempts > 0 ? options.max_attempts : 1;
  // Clamped before the first use, not after the first doubling: a caller whose
  // `backoff` already exceeds `max_backoff` would otherwise wait longer than the
  // ceiling the header calls binding, exactly once.
  std::chrono::milliseconds backoff =
      options.backoff > options.max_backoff ? options.max_backoff : options.backoff;
  std::chrono::milliseconds waited{0};

  for (int attempt = 1;; ++attempt) {
    const http::Result result = client.Send(request);
    Negotiation outcome;
    if (const std::optional<Negotiation> refused = Refused(result)) {
      outcome = *refused;
    } else {
      auth::Parsed<SyncPlan> parsed = ParseNegotiateResponse(result.response.body);
      if (parsed.ok()) {
        outcome.plan = std::move(parsed.value);
      } else {
        outcome = Refuse(NegotiateError::kMalformed,
                         "the plan could not be read: " + parsed.error.Describe());
      }
    }
    outcome.attempts = attempt;
    outcome.waited = waited;

    // Only a failure that says nothing about the pairing or the payload earns
    // another request. A 401 retried is a 401 retried forever, and a body the
    // server refused is a body it will refuse again.
    if (outcome.ok() || !ShouldRetry(outcome.error) || attempt >= attempts) {
      return outcome;
    }
    if (options.cancel != nullptr && options.cancel->canceled()) {
      // The backoff is time a shutdown does not have, and a retryable failure is
      // still one to stop on once the caller has given up.
      outcome = Refuse(NegotiateError::kCanceled,
                       "the negotiation was not retried: the caller cancelled");
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
