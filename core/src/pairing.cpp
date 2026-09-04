#include "rommsync/pairing.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/core.hpp"
#include "rommsync/http.hpp"
#include "rommsync/json.hpp"

namespace rommsync::auth {
namespace {

using namespace std::chrono_literals;

constexpr const char* kInitPath = "/api/auth/device/init";
constexpr const char* kTokenPath = "/api/auth/device/token";

/// Every `PairingState`, so the IPC decoder and `ToString` cannot drift.
constexpr PairingState kAllStates[] = {
    PairingState::kIdle,   PairingState::kPending, PairingState::kApproved,
    PairingState::kDenied, PairingState::kExpired, PairingState::kFailed,
};

std::string InitBody(const PairingConfig& config) {
  std::string body("{\"client_device_identifier\":");
  body += json::Quote(config.client_device_identifier);
  body += ",\"name\":";
  body += json::Quote(config.device_name);
  body += ",\"client\":";
  body += json::Quote(kClientName);
  body += ",\"platform\":";
  body += json::Quote(kClientPlatform);
  // Optional in the snapshot, and worth sending: when a console starts
  // misbehaving, RomM's device list is the one place that can say which build
  // of this client it is.
  body += ",\"client_version\":";
  body += json::Quote(version());
  body += ",\"requested_scopes\":";
  body += json::QuoteArray(config.requested_scopes);
  body += "}";
  return body;
}

std::string TokenBody(const std::string& device_code) {
  return "{\"device_code\":" + json::Quote(device_code) + "}";
}

http::Request JsonPost(const std::string& url, std::string body,
                       std::chrono::milliseconds timeout) {
  http::Request request;
  request.method = http::Method::kPost;
  request.url = url;
  request.headers.push_back({"Content-Type", "application/json"});
  request.headers.push_back({"Accept", "application/json"});
  request.body = std::move(body);
  request.timeout = timeout;
  return request;
}

/// A field that may legitimately be blank -- `user_code` before a code exists,
/// `message` when nothing went wrong -- so `json::Reader::Required`, which
/// refuses an empty string on purpose, is the wrong tool for it. Absent and
/// `""` mean the same thing; a non-string does not.
bool ReadText(const json::Value& object, std::string_view key, std::string* out,
              json::Error* error) {
  const json::Value* found = object.Find(key);
  if (found == nullptr || found->is_null()) {
    out->clear();
    return true;
  }
  if (!found->is_string()) {
    error->field = std::string(key);
    error->message = std::string("expected a string, got ") + json::ToString(found->type());
    return false;
  }
  *out = found->string();
  return true;
}

bool ReadCount(const json::Value& object, std::string_view key, std::int64_t* out,
               json::Error* error) {
  const json::Value* found = object.Find(key);
  if (found == nullptr) {
    error->field = std::string(key);
    error->message = "is missing";
    return false;
  }
  if (!found->is_integer()) {
    error->field = std::string(key);
    error->message = std::string("expected an integer, got ") + json::ToString(found->type());
    return false;
  }
  *out = found->integer();
  return true;
}

}  // namespace

const char* ToString(PairingState state) {
  switch (state) {
    case PairingState::kIdle:
      return "idle";
    case PairingState::kPending:
      return "pending";
    case PairingState::kApproved:
      return "approved";
    case PairingState::kDenied:
      return "denied";
    case PairingState::kExpired:
      return "expired";
    case PairingState::kFailed:
      return "failed";
  }
  return "idle";
}

bool IsTerminal(PairingState state) {
  return state == PairingState::kApproved || state == PairingState::kDenied ||
         state == PairingState::kExpired || state == PairingState::kFailed;
}

std::string SerializePairingStatus(const PairingStatus& status) {
  std::string out("{\"state\":");
  out += json::Quote(ToString(status.state));
  out += ",\"user_code\":";
  out += json::Quote(status.user_code);
  out += ",\"verification_url\":";
  out += json::Quote(status.verification_url);
  out += ",\"verification_url_complete\":";
  out += json::Quote(status.verification_url_complete);
  out += ",\"expires_in\":";
  out += std::to_string(status.expires_in.count());
  out += ",\"polls\":";
  out += std::to_string(status.polls);
  out += ",\"message\":";
  out += json::Quote(status.message);
  out += "}";
  return out;
}

Parsed<PairingStatus> ParsePairingStatus(std::string_view text) {
  Parsed<PairingStatus> parsed;
  const json::ParseResult document = json::Parse(text);
  if (!document.ok()) {
    parsed.error = document.error;
    return parsed;
  }
  if (!document.value.is_object()) {
    parsed.error.field = "";
    parsed.error.message = "pair state: expected an object";
    return parsed;
  }

  PairingStatus status;
  std::string state;
  if (!ReadText(document.value, "state", &state, &parsed.error)) {
    return parsed;
  }
  bool known = false;
  for (const PairingState candidate : kAllStates) {
    if (state == ToString(candidate)) {
      status.state = candidate;
      known = true;
      break;
    }
  }
  if (!known) {
    parsed.error.field = "state";
    // The value is one of six names this file wrote, so quoting it leaks
    // nothing -- and a decoder that only said "bad state" would be useless
    // against an overlay built from a different version of this header.
    parsed.error.message = "is not a pairing state: " + state;
    return parsed;
  }

  std::int64_t expires_in = 0;
  std::int64_t polls = 0;
  if (!ReadText(document.value, "user_code", &status.user_code, &parsed.error) ||
      !ReadText(document.value, "verification_url", &status.verification_url, &parsed.error) ||
      !ReadText(document.value, "verification_url_complete", &status.verification_url_complete,
                &parsed.error) ||
      !ReadText(document.value, "message", &status.message, &parsed.error) ||
      !ReadCount(document.value, "expires_in", &expires_in, &parsed.error) ||
      !ReadCount(document.value, "polls", &polls, &parsed.error)) {
    return parsed;
  }
  status.expires_in = std::chrono::seconds{expires_in};
  status.polls = static_cast<int>(polls);

  parsed.value = std::move(status);
  return parsed;
}

PairingSession::PairingSession(http::HttpClient& client, PairingConfig config, Clock clock)
    : client_(client), config_(std::move(config)), clock_(std::move(clock)) {}

PairingSession::TimePoint PairingSession::Now() const {
  return clock_ ? clock_() : std::chrono::steady_clock::now();
}

std::string PairingSession::ApiUrl(std::string_view path) const {
  std::string_view origin = config_.server_url;
  while (!origin.empty() && origin.back() == '/') {
    origin.remove_suffix(1);
  }
  return std::string(origin) + std::string(path);
}

PairingState PairingSession::Fail(std::string message) {
  status_.state = PairingState::kFailed;
  status_.message = std::move(message);
  return status_.state;
}

void PairingSession::ScheduleIn(std::chrono::seconds delay) {
  const std::chrono::seconds interval = init_.poll_interval();
  next_poll_ = Now() + (delay < interval ? interval : delay);
}

void PairingSession::BackOff() {
  const std::chrono::seconds interval = init_.poll_interval();
  std::chrono::seconds delay = interval;
  for (int step = 1; step < transient_failures_ && delay < config_.max_poll_backoff; ++step) {
    delay *= 2;
  }
  ScheduleIn(delay > config_.max_poll_backoff ? config_.max_poll_backoff : delay);
}

PairingState PairingSession::Begin() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = PairingStatus{};
    token_.reset();
    init_ = DeviceInitResponse{};
    transient_failures_ = 0;
    rejected_polls_ = 0;

    // Caught here rather than as RomM's 422: the server's answer would name a
    // field of a request body, which is a long way from "nobody has configured
    // a server URL yet".
    if (config_.server_url.empty()) {
      return Fail("no server URL is configured");
    }
    if (config_.client_device_identifier.empty()) {
      return Fail("no client device identifier was derived");
    }
    if (config_.requested_scopes.empty()) {
      return Fail("no scopes were requested");
    }
  }

  const http::Result result =
      client_.Send(JsonPost(ApiUrl(kInitPath), InitBody(config_), config_.request_timeout));

  std::lock_guard<std::mutex> lock(mutex_);
  if (!result.ok()) {
    return Fail(std::string("device init did not complete: ") + http::ToString(result.error));
  }
  // 5.2.0 answers 201. Any 2xx is accepted because the body is what decides:
  // `ParseDeviceInitResponse` refuses anything that is not the real shape, so
  // being strict about the status here would only reject a future RomM for a
  // difference that does not matter.
  // RomM allows ten of these a minute per IP. A console pairs once, so it is
  // not a limit anyone reaches by hand -- but it is transient, and saying so is
  // the difference between "wait a minute and press Pair again" and a user
  // reinstalling the sysmodule.
  if (result.response.status == 429) {
    return Fail("the server is rate limiting pairing requests (HTTP 429); try again in a minute");
  }
  if (!result.successful()) {
    return Fail("device init was refused: HTTP " + std::to_string(result.response.status));
  }
  Parsed<DeviceInitResponse> parsed = ParseDeviceInitResponse(result.response.body);
  if (!parsed.ok()) {
    return Fail("device init response: " + parsed.error.Describe());
  }

  init_ = std::move(parsed.value);
  const TimePoint now = Now();
  deadline_ = now + init_.lifetime();
  // The first poll may go out immediately: RomM's pacing window opens on the
  // first poll it *answers*, not on init, so waiting one interval here would
  // just be five seconds of a pairing screen doing nothing.
  next_poll_ = now;

  status_.state = PairingState::kPending;
  status_.user_code = init_.user_code;
  status_.verification_url = init_.VerificationUrl(config_.server_url);
  status_.verification_url_complete = init_.VerificationUrlComplete(config_.server_url);
  status_.expires_in = init_.lifetime();
  return status_.state;
}

PairingState PairingSession::Poll() {
  std::string device_code;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != PairingState::kPending) {
      return status_.state;
    }
    if (Now() >= deadline_) {
      status_.state = PairingState::kExpired;
      status_.expires_in = 0s;
      status_.message = "the code expired before anyone approved it";
      return status_.state;
    }
    if (Now() < next_poll_) {
      return status_.state;
    }
    device_code = init_.device_code;
  }

  // Sent outside the lock: `status()` is what the overlay calls while it
  // redraws, and it must not wait out a poll that is stalling.
  const http::Result result =
      client_.Send(JsonPost(ApiUrl(kTokenPath), TokenBody(device_code), config_.request_timeout));

  std::lock_guard<std::mutex> lock(mutex_);
  ++status_.polls;

  if (!result.ok()) {
    // No response at all -- offline, a stall the timeout caught, a connection
    // dropped mid-body. None of it says anything about the device_code, which
    // still has most of its ten minutes, so this backs off rather than
    // abandoning the pairing screen.
    ++transient_failures_;
    BackOff();
    status_.message =
        std::string("poll did not complete: ") + http::ToString(result.error) + ", retrying";
    return status_.state;
  }

  const TokenPoll poll = ClassifyTokenPoll(result.response.status, result.response.body);
  switch (poll) {
    case TokenPoll::kGranted: {
      Parsed<DeviceTokenResponse> granted = ParseDeviceTokenResponse(result.response.body);
      if (!granted.ok()) {
        // A 200 that cannot be read is not an approval, and polling again would
        // make it unreadable *and* unrecoverable: the code was just redeemed,
        // so the next poll answers expired_token and the real reason is lost.
        return Fail("device token response: " + granted.error.Describe());
      }
      token_ = std::move(granted.value);
      transient_failures_ = 0;
      rejected_polls_ = 0;
      status_.state = PairingState::kApproved;
      status_.message.clear();
      return status_.state;
    }

    case TokenPoll::kAuthorizationPending:
      transient_failures_ = 0;
      rejected_polls_ = 0;
      status_.message.clear();
      ScheduleIn(init_.poll_interval());
      return status_.state;

    case TokenPoll::kSlowDown:
      // We already wait `interval`, so being told to slow down means the
      // server's window is wider than the number it sent. Treating it as a
      // transient failure backs off instead of arguing -- and RomM restarts
      // that window on the poll it answered, so anything shorter would earn
      // another one.
      ++transient_failures_;
      BackOff();
      status_.message = "the server asked us to poll more slowly";
      return status_.state;

    case TokenPoll::kRateLimited:
      ++transient_failures_;
      BackOff();
      status_.message = "the server is rate limiting us; backing off";
      return status_.state;

    case TokenPoll::kServerError:
      ++transient_failures_;
      BackOff();
      status_.message = "the server is unwell (HTTP " +
                        std::to_string(result.response.status) + "); retrying";
      return status_.state;

    case TokenPoll::kAccessDenied:
      status_.state = PairingState::kDenied;
      status_.message = "the code was refused in the web interface";
      return status_.state;

    case TokenPoll::kExpiredToken:
      status_.state = PairingState::kExpired;
      status_.expires_in = 0s;
      // "Already redeemed" answers identically, which is why nothing re-polls
      // to confirm a token it has already been given (docs/AUTH.md).
      status_.message = "the code expired or had already been used";
      return status_.state;

    case TokenPoll::kUnrecognized:
      break;
  }

  // A 401 or 403 cannot be a verdict on the device_code: this endpoint takes no
  // credentials, so RomM has nothing to reject. It is something in front of it,
  // and that is worth a few retries and then a diagnosis -- see
  // `PairingConfig::max_rejected_polls`.
  const int status_code = result.response.status;
  if (status_code == 401 || status_code == 403) {
    ++rejected_polls_;
    if (rejected_polls_ < config_.max_rejected_polls) {
      ++transient_failures_;
      BackOff();
      status_.message =
          "the poll was rejected with HTTP " + std::to_string(status_code) + "; retrying";
      return status_.state;
    }
    return Fail("the server kept rejecting the poll with HTTP " + std::to_string(status_code) +
                " -- something in front of RomM is asking for credentials");
  }
  return Fail("the server answered something this client does not understand: HTTP " +
              std::to_string(status_code));
}

PairingStatus PairingSession::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  PairingStatus copy = status_;
  if (copy.state == PairingState::kPending) {
    const TimePoint now = Now();
    copy.expires_in = now >= deadline_
                          ? 0s
                          : std::chrono::duration_cast<std::chrono::seconds>(deadline_ - now);
  } else {
    copy.expires_in = 0s;
  }
  return copy;
}

const DeviceTokenResponse* PairingSession::token() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_.state == PairingState::kApproved && token_.has_value() ? &*token_ : nullptr;
}

std::chrono::steady_clock::time_point PairingSession::next_poll_at() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_poll_;
}

}  // namespace rommsync::auth
