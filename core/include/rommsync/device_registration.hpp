// The device every sync call is scoped by, and how the console finds it again.
//
// **This client never calls `POST /api/devices`.** That is the surprise, so it
// is the first thing here. Pairing already registers the console: RomM creates
// the device row from the `client_device_identifier` in
// `POST /api/auth/device/init`, and `POST /api/auth/device/token` hands back its
// id. `POST /api/devices` does not find that row -- it matches on `hostname` or
// `mac_address` and on nothing else, so it creates a *second* device however it
// is called, with `allow_existing` and with a device-bound token for the very
// device it is being asked about. Verified against a live 5.2.0, six calls and
// five extra rows; the table is in docs/API_CONTRACT.md#device-registration.
//
// A second device is not a cosmetic duplicate. Sync history is per device, so a
// console that registered a fresh one has none, and the first negotiation of
// every tick reports every save as a first encounter (docs/SYNC_PROTOCOL.md).
//
// So "register" here means *resolve*: confirm the id pairing already cached, and
// when there is none to confirm, find the row the identifier names rather than
// making another one. Both are reads.
#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/http.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::auth {

/// `DeviceSchema`, the fields this client has a use for.
///
/// The rest of the schema -- `user_id`, `ip_address`, `mac_address`,
/// `sync_config`, the timestamps -- is RomM's bookkeeping and is deliberately
/// not read: a field this client cannot act on is one more shape a future RomM
/// can break the parse with, for nothing.
struct DeviceRecord {
  /// The id every sync call is scoped by. RomM calls the same value `id` here,
  /// `device_id` in the token response, and `device_id` again in
  /// `DeviceCreateResponse` -- one value, three names.
  std::string id;

  /// Declared `string | null` and read as such: empty means RomM sent `null`,
  /// which is a device nobody has named, not a parse that lost something.
  std::string name;
  std::string platform;
  std::string client;

  /// What ties this row to this console, and the only field that does. Empty on
  /// every device that did not come from the device-code flow -- including one
  /// a `POST /api/devices` created, which is exactly why that call cannot be
  /// used to find the console again.
  std::string client_device_identifier;

  /// The switch the user has in RomM's own device list. With it off,
  /// `POST /api/sync/negotiate` answers `400 "Sync is disabled for this
  /// device"` -- so this is read at registration time, where it can be reported
  /// as the setting it is, rather than met mid-sync as a bad request.
  bool sync_enabled = true;
};

/// Parse a 200 body from `GET /api/devices/{device_id}`.
Parsed<DeviceRecord> ParseDeviceRecord(std::string_view body);

/// Parse a 200 body from `GET /api/devices`: a JSON array, possibly empty.
Parsed<std::vector<DeviceRecord>> ParseDeviceList(std::string_view body);

/// The one device that names `identifier`, or nullptr.
///
/// Nullptr for a second match too. RomM has no uniqueness constraint on
/// `client_device_identifier`, so two rows carrying one identifier is a state
/// the server can be in -- and picking one of them arbitrarily would send this
/// console's saves to whichever row sorted first this boot. Ambiguous is a thing
/// to report, not to resolve by guessing.
const DeviceRecord* FindByIdentifier(const std::vector<DeviceRecord>& devices,
                                     std::string_view identifier);

/// How completely this console is paired, as the record on disk shows it.
///
/// The middle state is the one worth having a name for. A token with no device
/// is not an error and must not be raised as one at sync time: it is a pairing
/// that did not finish, and the remedy -- pair again -- is the same one an
/// unpaired console needs. What it must never be is a token that gets used
/// anyway, because every sync call is scoped by a device id and an empty one
/// scopes nothing.
enum class RegistrationState {
  kUnpaired,      ///< no token: nothing has been paired
  kUnregistered,  ///< a token and no device id: paired, not fully
  kRegistered,    ///< a token and a device id to scope sync by
};

/// Stable, log-friendly name. Never null.
const char* ToString(RegistrationState state);

/// What `token` amounts to. Reads the record only -- it asks the server nothing,
/// so it is what a caller checks before deciding whether there is anything to
/// ask about.
///
/// A `LoadToken` that failed is `kUnpaired`: "no file" and "a file that is not a
/// token record" are both "this console has no credentials".
RegistrationState StateOf(const StoredToken& token);

/// Why resolving the registration did not work.
enum class RegistrationError {
  kNone,
  kNotRegistered,  ///< the record carries no device id; there is nothing to confirm
  kUnauthorized,   ///< 401 -- the token was revoked. Re-pair.

  /// 403 -- the token is real and was not granted what this call needs.
  ///
  /// Kept apart from a revocation, which is the same split `sync::NegotiateError`
  /// makes and for the same reason: RomM approves what the *user* ticked, which
  /// need not be what was requested, so a 403 is a scope missing from an
  /// otherwise working pairing (docs/AUTH.md#scopes-to-request). Telling that
  /// user their token was revoked sends them looking for something that did not
  /// happen, and a client that discarded `token.dat` over it would re-pair
  /// straight back into the same partial grant.
  kForbidden,
  kNoSuchDevice,   ///< 404, or no row carries the identifier -- deleted in RomM's UI
  kAmbiguous,      ///< more than one device names this identifier; picking one would guess
  kSyncDisabled,   ///< the device is there and the user has turned sync off for it
  kUnreachable,    ///< the exchange did not complete -- offline, stalled, dropped
  /// The server would not deal with this request now, and may with the next:
  /// a 5xx, or the 429/408 a rate limiter or a reverse proxy answers with.
  kServerError,
  kMalformed,      ///< a 2xx that is not a device, or a cached id that cannot be a URL
};

/// Stable, log-friendly name. Never null.
const char* ToString(RegistrationError error);

/// Whether trying the same call again later could succeed.
///
/// Only the two failures that say nothing about the pairing: no response, and a
/// server having a bad minute -- which includes being rate limited, because a
/// remedy that is "wait" must not be reported as one that is not. `kMalformed`
/// is deliberately not among them, for the same reason
/// `TokenPoll::kUnrecognized` is not retryable -- an answer this client cannot
/// read is not something to hammer.
bool ShouldRetry(RegistrationError error);

/// Whether the remedy is to pair again.
///
/// The four failures that mean the credentials do not, as they stand, name a
/// device on that server.
///
/// `kForbidden` is in it and earns a different sentence: re-pairing is where the
/// user approves the scope that is missing, so the remedy really is to pair
/// again -- but "pair again and approve the scopes this needs" is not "your
/// pairing is gone", and only the second is true of a 401
/// (`auth::Describe(Block)` carries both). Distinct from `ShouldRetry` on purpose: the overlay's two sentences
/// are "your server is unreachable, this will retry" and "pair this console
/// again", and sending a user to the second over a dropped connection is how a
/// working pairing gets thrown away. An error that is in neither -- sync turned
/// off, a body that would not parse -- is one that neither waiting nor
/// re-pairing fixes, and saying so is the whole point of not collapsing them.
bool NeedsPairing(RegistrationError error);

/// What this error says about the credentials, for `auth::Gate`.
///
/// Not the same question as either of the two above: `kNoSuchDevice` and
/// `kSyncDisabled` are answers the server could only give *because* it took the
/// token, so they clear a rejection count rather than adding to one.
Answer AnswerOf(RegistrationError error);

/// A resolved registration, or the reason there isn't one. `device` is left
/// default-constructed on failure and must not be used -- check `ok()`.
struct Registration {
  DeviceRecord device{};
  RegistrationError error = RegistrationError::kNone;

  /// For logs and for the overlay. Names the status and the reason, never the
  /// token.
  std::string message;

  bool ok() const { return error == RegistrationError::kNone; }
};

/// Confirm the device id `token` already carries. One `GET /api/devices/{id}`.
///
/// This is the boot-time question: RomM still has this device, this token can
/// still read it, and sync is still on for it. One request, and three things
/// that would otherwise surface as a puzzling mid-sync failure -- a revoked
/// token, a device deleted in the web UI, sync switched off -- become a named
/// state before the first save is touched.
///
/// What it does **not** prove is that the device is this *console's*. The
/// `client_device_identifier` on the returned record is what says that, and it
/// is handed back rather than checked here, because a mismatch is not this
/// call's to act on: a device RomM created some other way legitimately carries
/// none, and failing a working pairing over a decorative field is the worse
/// error. Note that it would not catch the case it looks like it should either
/// -- a cloned SD card carries `device.dat` along with `token.dat`, so both
/// consoles present the same identifier and agree.
Registration ConfirmRegistration(http::HttpClient& client, const StoredToken& token,
                                 std::chrono::milliseconds timeout = http::kDefaultTimeout);

/// Find the device `client_device_identifier` names. One `GET /api/devices`.
///
/// The recovery path, for a token that has no device id to confirm. It is a
/// search and not a registration on purpose: the row is already there, because
/// pairing made it, and the identifier is the only field that points back at
/// this console. `LoadOrCreateDeviceIdentity` (device_identity.hpp) is what
/// produces the value to pass here; it is not in `token.dat`, because a re-pair
/// discards that file and has to keep the identifier.
Registration FindRegistration(http::HttpClient& client, const StoredToken& token,
                              std::string_view client_device_identifier,
                              std::chrono::milliseconds timeout = http::kDefaultTimeout);

/// Confirm, and fall back to a search when there is nothing to confirm.
///
/// The fallback is deliberately narrow: only `kNotRegistered` and
/// `kNoSuchDevice`, the two answers that mean "the cached id names no device".
/// Widening it inverts a diagnosis. A console whose server is unreachable would
/// spend a second timeout on a listing and then be told, by a listing that
/// finally went through, that its device is fine -- or on a stall, wait out two
/// timeouts instead of one before the overlay can say "offline". A `401` falls
/// outside it for a smaller reason: the token is revoked, so the listing is a
/// request that cannot succeed, and issuing it only moves the failure from the
/// call that named the device to the one that listed them all.
///
/// A successful result may carry a *different* id than `token` did. Persisting
/// it is `CacheDeviceId`.
Registration ResolveRegistration(http::HttpClient& client, const StoredToken& token,
                                 std::string_view client_device_identifier,
                                 std::chrono::milliseconds timeout = http::kDefaultTimeout);

/// Write `registration`'s device id into `token` and persist the record.
///
/// Does nothing, successfully, when `token` already carries that id -- which is
/// every boot after the first. A console that rewrote `token.dat` on each boot
/// would be spending an SD write, and a commit window in which the file is
/// briefly absent, on storing a value that did not change.
///
/// `token` is left untouched when the write fails, so the record and the disk
/// never disagree: a caller whose SD card was full retries with the same record
/// and actually writes, rather than short-circuiting on an id it only ever held
/// in memory.
///
/// A failed `registration` writes nothing and reports `kUnusableToken`: caching
/// an id that was never confirmed is exactly the "a token with no device id,
/// silently used for sync" this module exists to prevent.
StoreResult CacheDeviceId(const std::string& path, StoredToken& token,
                          const Registration& registration);

}  // namespace rommsync::auth
