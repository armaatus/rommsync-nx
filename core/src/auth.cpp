#include "rommsync/auth.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "rommsync/json.hpp"

namespace rommsync::auth {
namespace {

std::chrono::seconds Clamp(std::int64_t seconds, std::chrono::seconds low,
                           std::chrono::seconds high) {
  if (seconds < low.count()) {
    return low;
  }
  if (seconds > high.count()) {
    return high;
  }
  return std::chrono::seconds{seconds};
}

/// `server_url` without its trailing slashes, so joining a path that starts
/// with one cannot produce `//pair/device` -- which RomM's router does not
/// match and which a user reading it off the overlay would retype wrongly.
std::string_view Origin(std::string_view server_url) {
  while (!server_url.empty() && server_url.back() == '/') {
    server_url.remove_suffix(1);
  }
  return server_url;
}

/// A path that already carries a scheme is a URL, not a path to join onto.
/// 5.2.0 never sends one; a RomM that moved to RFC 8628's `verification_uri`
/// would, and prefixing an origin onto it yields a link nobody can follow.
bool IsAbsolute(const std::string& path) {
  const std::size_t scheme = path.find("://");
  return scheme != std::string::npos && scheme > 0 && path.find('/') > scheme;
}

std::string Join(std::string_view server_url, const std::string& path) {
  if (IsAbsolute(path)) {
    return path;
  }
  std::string url(Origin(server_url));
  if (!path.empty() && path.front() != '/') {
    url.push_back('/');
  }
  url.append(path);
  return url;
}

}  // namespace

std::chrono::seconds DeviceInitResponse::lifetime() const {
  return Clamp(expires_in, kMinPairingLifetime, kMaxPairingLifetime);
}

std::chrono::seconds DeviceInitResponse::poll_interval() const {
  // Only ever slower than asked, never faster: RomM restarts its pacing window
  // on every poll it answers `slow_down`, so a loop that undercuts `interval`
  // earns `slow_down` forever and the code expires under it. The ceiling is the
  // code's own lifetime, past which there is nothing left to poll for.
  const std::chrono::seconds asked = Clamp(interval, kMinPollInterval, kMaxPairingLifetime);
  return asked < lifetime() ? asked : lifetime();
}

std::string DeviceInitResponse::VerificationUrl(std::string_view server_url) const {
  return Join(server_url, verification_path);
}

std::string DeviceInitResponse::VerificationUrlComplete(std::string_view server_url) const {
  return Join(server_url, verification_path_complete);
}

bool DeviceTokenResponse::HasScope(std::string_view scope) const {
  return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

Parsed<DeviceInitResponse> ParseDeviceInitResponse(std::string_view body) {
  Parsed<DeviceInitResponse> parsed;
  json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    parsed.error = std::move(document.error);
    return parsed;
  }

  DeviceInitResponse response;
  json::Reader reader(document.value, "device init response");
  reader.Required("device_code", &response.device_code);
  reader.Required("user_code", &response.user_code);
  reader.Required("verification_path", &response.verification_path);
  reader.Required("verification_path_complete", &response.verification_path_complete);
  reader.Required("expires_in", &response.expires_in);
  reader.Required("interval", &response.interval);
  if (!reader.ok()) {
    parsed.error = reader.error();
    return parsed;
  }

  // Negative durations are not a shape problem, so the reader lets them past;
  // they are still nonsense, and `poll_interval()` would silently clamp them
  // into something that looks deliberate.
  if (response.expires_in <= 0) {
    parsed.error.field = "expires_in";
    parsed.error.message = "is not a positive number of seconds";
    return parsed;
  }
  if (response.interval < 0) {
    parsed.error.field = "interval";
    parsed.error.message = "is negative";
    return parsed;
  }

  parsed.value = std::move(response);
  return parsed;
}

Parsed<DeviceTokenResponse> ParseDeviceTokenResponse(std::string_view body) {
  Parsed<DeviceTokenResponse> parsed;
  json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    parsed.error = std::move(document.error);
    return parsed;
  }

  DeviceTokenResponse response;
  json::Reader reader(document.value, "device token response");
  reader.Required("access_token", &response.access_token);
  reader.Required("device_id", &response.device_id);
  reader.Required("scopes", &response.scopes);
  // Present-but-null, every time, on 5.2.0. Required rather than optional: a
  // response that stopped carrying the key at all would be a different
  // contract, and we want to hear about it here rather than assume "no expiry".
  reader.RequiredNullable("expires_at", &response.expires_at);
  if (!reader.ok()) {
    parsed.error = reader.error();
    return parsed;
  }

  parsed.value = std::move(response);
  return parsed;
}

const char* ToString(TokenPoll poll) {
  switch (poll) {
    case TokenPoll::kGranted:
      return "granted";
    case TokenPoll::kAuthorizationPending:
      return "authorization_pending";
    case TokenPoll::kSlowDown:
      return "slow_down";
    case TokenPoll::kRateLimited:
      return "rate_limited";
    case TokenPoll::kServerError:
      return "server_error";
    case TokenPoll::kAccessDenied:
      return "access_denied";
    case TokenPoll::kExpiredToken:
      return "expired_token";
    case TokenPoll::kUnrecognized:
      return "unrecognized";
  }
  return "unrecognized";
}

bool ShouldKeepPolling(TokenPoll poll) {
  return poll == TokenPoll::kAuthorizationPending || poll == TokenPoll::kSlowDown ||
         poll == TokenPoll::kRateLimited || poll == TokenPoll::kServerError;
}

TokenPoll ClassifyTokenPoll(int status, std::string_view body) {
  if (status == 200) {
    return TokenPoll::kGranted;
  }
  // Prose, not a code: "Too many polling attempts. Try again later.". The
  // status is the contract here, so the body is not read.
  if (status == 429) {
    return TokenPoll::kRateLimited;
  }
  // A gateway restarting, or RomM itself, says nothing about the device_code:
  // it is still good for the rest of its `expires_in`. Abandoning the pairing
  // screen over a 502 throws away minutes of a perfectly live code.
  if (status >= 500 && status < 600) {
    return TokenPoll::kServerError;
  }
  if (status != 400) {
    return TokenPoll::kUnrecognized;
  }

  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    return TokenPoll::kUnrecognized;
  }
  const json::Value* detail = document.value.Find("detail");
  if (detail == nullptr || !detail->is_string()) {
    return TokenPoll::kUnrecognized;
  }
  const std::string& reason = detail->string();
  if (reason == "authorization_pending") {
    return TokenPoll::kAuthorizationPending;
  }
  if (reason == "slow_down") {
    return TokenPoll::kSlowDown;
  }
  if (reason == "access_denied") {
    return TokenPoll::kAccessDenied;
  }
  if (reason == "expired_token") {
    return TokenPoll::kExpiredToken;
  }
  return TokenPoll::kUnrecognized;
}

}  // namespace rommsync::auth
