// The device-code pairing flow, driven one step at a time.
//
// `auth.hpp` says what RomM's answers *mean*; this runs the conversation:
// `POST /api/auth/device/init`, then `POST /api/auth/device/token` at the
// interval the server asked for, until a human approves the code in a browser,
// refuses it, or lets it expire.
//
// It is a state machine and not a loop, for two reasons. The overlay asks for
// the pairing state whenever it redraws (`GetPairState`,
// docs/DEVELOPMENT.md#ipc) and must never wait on a socket to get an answer;
// and a sysmodule may not park a thread in a 600-second poll loop when the rule
// is that nothing blocks boot. So `Poll()` performs at most one request, returns
// immediately when the next one is not due yet, and the caller owns the
// scheduling -- which is also what makes every branch here testable in seconds
// against a real RomM.
//
// Nothing that leaves this class carries a secret. `PairingStatus` is what the
// overlay renders and what crosses IPC: it holds the `user_code` a human is
// meant to read off the screen and the URL to type, and never the `device_code`
// or the access token.
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/http.hpp"

namespace rommsync::auth {

/// What RomM is told it is talking to. Fixed by the protocol, not configurable:
/// `platform` is what puts "switch" next to the device in RomM's own UI.
inline constexpr const char* kClientName = "rommsync-nx";
inline constexpr const char* kClientPlatform = "switch";

/// The scopes this client asks for, and the complete list of them.
///
/// Least privilege, and pinned to docs/API_CONTRACT.md#scopes-to-request by
/// `auth.scopes` so the code and the document cannot drift. Every `.write` here
/// is one the client actually performs: `roms.user.write` and `assets.write`
/// are how a save gets uploaded, `devices.write` is how the console registers
/// itself (M1-3). `me.write` is in the document and deliberately *not* here --
/// it exists only for recording play sessions, which this client does not do,
/// and a scope that is granted and never used is blast radius bought for
/// nothing (docs/SECURITY.md).
///
/// RomM may approve a subset of these, which is why the granted set is read
/// back off the token response rather than assumed.
std::vector<std::string> MinimumScopes();

/// Where a pairing attempt has got to.
///
/// The three terminal failures are deliberately distinct. A human who refused
/// the code, a code that ran out of time, and a server that answered something
/// this client cannot act on need three different sentences on the overlay --
/// "you denied this on the website", "the code expired, here is a new one",
/// "your server rejected the pairing" -- and only the last is worth a bug
/// report. Collapsing them into one "failed" is how a support thread starts.
///
/// `kStarting` is the other one worth having. `Begin()`'s request can take as
/// long as `request_timeout`, and reporting `kIdle` for those thirty seconds
/// tells the user that pressing Pair did nothing. The owning thread never
/// observes it -- only the overlay, asking while the init is in flight, which
/// is exactly who needs to be able to say "contacting the server".
enum class PairingState {
  kIdle,      ///< nothing has been started, or the last attempt was discarded
  kStarting,  ///< asking the server for a code; there is nothing to show yet
  kPending,   ///< a code is live; show it and keep polling
  kApproved,  ///< a token was granted; `PairingSession::token()` has it
  kDenied,    ///< a human refused the code in the web UI. Start over.
  kExpired,   ///< the code ran out before anyone approved it. Start over.
  kFailed,    ///< the exchange could not be completed; `message` says why
};

/// Stable, log-friendly name. Never null.
const char* ToString(PairingState state);

/// Whether this state will ever change on its own. Terminal states are what a
/// caller's loop stops on; only `Begin()` moves out of one. `kStarting` is not
/// one of them.
bool IsTerminal(PairingState state);

/// Everything a pairing attempt needs to know about the console it runs on.
struct PairingConfig {
  /// The RomM origin the user configured, e.g. `http://romm.lan:8080`. Trailing
  /// slashes are tolerated.
  std::string server_url;

  /// Stable per console, so re-pairing is recognised as the same device rather
  /// than accumulating a new one every time, and never a value that identifies
  /// the *user* or the hardware. `LoadOrCreateDeviceIdentity` in
  /// device_identity.hpp is what produces one.
  std::string client_device_identifier;

  /// What the user will see in RomM's device list.
  std::string device_name = "rommsync-nx on Switch";

  /// Least privilege by default, and the default is the whole answer: see
  /// `MinimumScopes`. A caller that narrows this further is fine; one that
  /// widens it is asking for a token that can do more than this client ever
  /// does. RomM may approve a subset, which is why
  /// `DeviceTokenResponse::scopes` is read back.
  std::vector<std::string> requested_scopes = MinimumScopes();

  /// Ceiling on one init or one poll. A poll that hangs must not hold the
  /// pairing screen: it fails, gets counted, and the next one is scheduled.
  std::chrono::milliseconds request_timeout = http::kDefaultTimeout;

  /// How far backoff may stretch a retry after a transient failure. Never
  /// shorter than the server's `interval`, which remains the floor.
  std::chrono::seconds max_poll_backoff{60};

  /// How many consecutive polls may be rejected with a 401/403 before the
  /// attempt is given up on.
  ///
  /// The token endpoint takes no credentials, so RomM itself has no way to
  /// answer 401 there -- one comes from something in *front* of it, an
  /// authenticating reverse proxy or a gateway having a bad minute. A blip
  /// deserves a retry; a proxy that will answer 401 every time must not be
  /// allowed to burn the code's whole 600 seconds and then report "expired",
  /// which is the one diagnosis guaranteed to send the user looking in the
  /// wrong place. Hence a small budget rather than either extreme.
  int max_rejected_polls = 3;
};

/// The pairing state as the overlay sees it, and as it crosses IPC.
///
/// Carries no secret: not the `device_code`, which is what trades for a token,
/// and not the token itself. Whatever renders this can be as careless as a UI
/// usually is.
struct PairingStatus {
  PairingState state = PairingState::kIdle;

  /// The 8 characters the human types. Shown exactly as it arrived -- RomM's
  /// alphabet already excludes the confusable letters (docs/AUTH.md).
  std::string user_code;

  /// Absolute, ready to display: the configured origin joined with the path
  /// RomM returned.
  std::string verification_url;

  /// The same with `?user_code=` already on it, for a QR code.
  std::string verification_url_complete;

  /// How long the code still has. Counts down; zero once it is gone.
  std::chrono::seconds expires_in{0};

  /// Polls actually sent. A pairing that completes in three has honoured the
  /// interval; one that took thirty was hammering.
  int polls = 0;

  /// Why, for the states that need a why. Log-safe and user-safe: a status
  /// code and a reason, never a response body and never a credential.
  std::string message;
};

/// The status as JSON, for the IPC payload behind `GetPairState`.
///
/// The transport is M5-2's; the *payload* is this milestone's, because the
/// pairing screen (M4-5) cannot be written against a shape that does not exist
/// yet. Keeping the encoder and decoder next to each other -- and testing that
/// a round trip is lossless and that the output contains no secret -- is what
/// stops the overlay and the sysmodule from disagreeing later.
std::string SerializePairingStatus(const PairingStatus& status);
Parsed<PairingStatus> ParsePairingStatus(std::string_view text);

/// One pairing attempt, from `init` to a token.
///
/// Threading: `Begin()`, `Poll()` and `token()` belong to whichever thread owns
/// the pairing -- the sysmodule's auth thread. `status()` is safe from any
/// thread and never blocks on the network, because that is the call the overlay
/// makes while a request is in flight.
class PairingSession {
 public:
  /// Injectable so expiry and backoff are testable without waiting out a
  /// ten-minute code. Monotonic on purpose: a user setting the console clock
  /// mid-pairing must not expire a live code or resurrect a dead one.
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  /// `client` must outlive the session. A null `clock` means `steady_clock`.
  PairingSession(http::HttpClient& client, PairingConfig config, Clock clock = nullptr);

  PairingSession(const PairingSession&) = delete;
  PairingSession& operator=(const PairingSession&) = delete;

  /// Ask RomM for a code. One request; moves `kIdle` -> `kPending`, or to
  /// `kFailed` with a reason.
  ///
  /// Safe to call from any state: it discards whatever the last attempt left
  /// behind, which is what "Re-pair" in the overlay does.
  PairingState Begin();

  /// Drive the attempt forward by at most one request.
  ///
  /// Returns immediately -- and sends nothing -- when the state is terminal or
  /// the next poll is not due. Call it as often as you like; the interval RomM
  /// asked for is enforced here, not by the caller, because polling faster than
  /// `interval` earns `slow_down` on every subsequent poll and wedges the
  /// pairing until the code expires (docs/AUTH.md).
  PairingState Poll();

  /// A consistent snapshot, with `expires_in` counted down to now.
  PairingStatus status() const;

  /// The granted token, or nullptr unless the state is `kApproved`. Valid until
  /// the next `Begin()`.
  const DeviceTokenResponse* token() const;

  /// When `Poll()` will next do something. Meaningful only while `kPending`.
  std::chrono::steady_clock::time_point next_poll_at() const;

 private:
  using TimePoint = std::chrono::steady_clock::time_point;

  TimePoint Now() const;
  std::string ApiUrl(std::string_view path) const;
  PairingState Fail(std::string message);

  /// Schedule the next poll `delay` from now, never sooner than the server's
  /// interval.
  void ScheduleIn(std::chrono::seconds delay);

  /// Back off after a transient failure: the interval doubled once per
  /// consecutive failure, capped by `max_poll_backoff`.
  void BackOff();

  http::HttpClient& client_;
  const PairingConfig config_;
  const Clock clock_;

  mutable std::mutex mutex_;
  PairingStatus status_;
  DeviceInitResponse init_;
  std::optional<DeviceTokenResponse> token_;
  TimePoint deadline_{};
  TimePoint next_poll_{};
  int transient_failures_ = 0;
  int rejected_polls_ = 0;
};

}  // namespace rommsync::auth
