// The device-code pairing responses, as RomM 5.2.0 actually sends them.
//
// Every field below was read off a live 5.2.0 and is committed under
// server/contract/captures/ (issue M0-4); nothing here is inferred from the
// OAuth device-grant spec, which RomM follows only loosely. docs/AUTH.md quotes
// the payloads and lists where the OpenAPI snapshot and the running server
// disagree.
//
// A response that does not carry every field, with the right type, is rejected
// with a named error. There is no partial parse: a `DeviceTokenResponse` whose
// `device_id` silently defaulted to "" pairs against nothing and then 401s on
// every sync tick, hours later and far from the cause.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/json.hpp"

namespace rommsync::auth {

/// `POST /api/auth/device/init` -> **201**, six fields, none nullable.
struct DeviceInitResponse {
  /// 64 lowercase hex characters on 5.2.0. Secret: it is what the poll trades
  /// for a token, so it never goes in a log.
  std::string device_code;

  /// 8 characters from `ABCDEFGHJKMNPQRSTUVWXYZ23456789`, no dash. Show it
  /// exactly as it arrives: a human retyping `ABCD-1234` off the overlay would
  /// not match `ABCD1234`. The alphabet already excludes `I`, `L`, `O`, `0` and
  /// `1`, so nothing downstream should be "correcting" a confusable character.
  std::string user_code;

  /// A *path*, not the absolute `verification_uri` the OAuth spec names: RomM
  /// is origin-agnostic and leaves the join to the client. `/pair/device`.
  std::string verification_path;

  /// The same path with `?user_code=` appended, for a QR code.
  std::string verification_path_complete;

  /// Seconds the `device_code` stays usable. 600 on 5.2.0.
  std::int64_t expires_in = 0;

  /// Seconds RomM asks the client to wait between polls. 5 on 5.2.0.
  std::int64_t interval = 0;

  /// `interval` clamped into a range a poll loop survives. The server's value
  /// is a hint that arrives over the network: a 0 would spin the sysmodule
  /// against RomM's rate limiter, and a very large one would leave a pairing
  /// screen looking hung until the code expired anyway.
  std::chrono::seconds poll_interval() const;

  /// How long polling may go on before the code is dead. Clamped the same way
  /// and for the same reason.
  std::chrono::seconds lifetime() const;

  /// The absolute URL to put in front of the user, `server_url` being the
  /// origin they configured (`http://romm.lan:8080`). Trailing slashes on
  /// `server_url` are dropped so the join never doubles one.
  std::string VerificationUrl(std::string_view server_url) const;

  /// The same, with the user code already in the query string.
  std::string VerificationUrlComplete(std::string_view server_url) const;
};

/// Bounds for `poll_interval()` / `lifetime()`.
inline constexpr std::chrono::seconds kMinPollInterval{1};
inline constexpr std::chrono::seconds kMaxPollInterval{60};
inline constexpr std::chrono::seconds kMaxPairingLifetime{3600};

/// `POST /api/auth/device/token` -> **200**, once the code has been approved.
///
/// Four fields. There is no `token_type`, no `refresh_token` and no
/// `expires_in` -- there is nothing to refresh, so a 401 means revoked
/// (docs/AUTH.md).
struct DeviceTokenResponse {
  /// `rmm_` + 64 lowercase hex characters on 5.2.0. Secret.
  std::string access_token;

  /// The device RomM paired. Already enough to negotiate with -- registering
  /// another one gives every save an empty sync history.
  std::string device_id;

  /// What the user **approved**, which need not be what was requested, and
  /// comes back sorted rather than in the requested order. May legitimately be
  /// empty, so it is read as an array, not as proof of anything.
  std::vector<std::string> scopes;

  /// Declared `string | null` and observed `null` on every 5.2.0 response:
  /// this token does not expire on its own. Empty means "no expiry", which is
  /// never an error. When RomM does start setting it, it is an ISO 8601
  /// timestamp with an offset (`2026-09-04T13:04:00.528870+00:00`), the shape
  /// `GET /api/auth/device/pending/{user_code}` already returns.
  std::optional<std::string> expires_at;

  bool HasScope(std::string_view scope) const;
};

/// A parsed value, or the reason there isn't one. `value` is left
/// default-constructed on failure and must not be used -- check `ok()`.
template <typename T>
struct Parsed {
  T value{};
  json::Error error;
  bool ok() const { return error.ok(); }
};

/// Parse a 201 body from `POST /api/auth/device/init`.
Parsed<DeviceInitResponse> ParseDeviceInitResponse(std::string_view body);

/// Parse a 200 body from `POST /api/auth/device/token`.
Parsed<DeviceTokenResponse> ParseDeviceTokenResponse(std::string_view body);

/// What a poll of `POST /api/auth/device/token` meant.
///
/// The status code alone cannot tell "keep polling" from "this pairing is
/// dead": RomM answers both with `400`, and only the `detail` string separates
/// them. None of it is in the OpenAPI snapshot, which declares `200` and `422`
/// and no error body at all -- see docs/AUTH.md. All five states below were
/// observed against a live 5.2.0.
enum class TokenPoll {
  kGranted,               ///< 200 -- read the body with ParseDeviceTokenResponse
  kAuthorizationPending,  ///< 400 `authorization_pending` -- nobody has approved yet
  kSlowDown,              ///< 400 `slow_down` -- polled inside `interval`, per device_code
  kRateLimited,           ///< 429 -- more than 60 polls in a minute, per IP
  kAccessDenied,          ///< 400 `access_denied` -- the human refused. Stop.
  kExpiredToken,          ///< 400 `expired_token` -- expired, unknown, or already spent. Stop.
  kUnrecognized,          ///< anything else, including a body that is not JSON
};

/// Stable, log-friendly name. Never null.
const char* ToString(TokenPoll poll);

/// Whether a poll in this state should be tried again rather than abandoned.
///
/// The three retryable states are the ones where the pairing is still alive and
/// the server is only asking for patience. `kUnrecognized` is not among them: a
/// status this code does not understand is not something to hammer.
bool ShouldKeepPolling(TokenPoll poll);

/// Classify one poll response. `body` is read only for a `400`, the one status
/// whose meaning lives in the payload.
TokenPoll ClassifyTokenPoll(int status, std::string_view body);

}  // namespace rommsync::auth
