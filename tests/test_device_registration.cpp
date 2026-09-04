// Registering the console, end to end against the real docker RomM.
//
// One scenario per CTest entry (`device.registered`, `device.deleted`, ...),
// selected by argv[1], so a red run names the behaviour that broke. Every device
// here is one RomM created, from a pairing this test drove through RomM's own
// `/api/auth/device/approve` -- the endpoint that lets a test be the human the
// grant assumes (docs/TESTING.md).
//
// Only a server can answer the question this milestone is about. "Pairing twice
// does not leave two devices behind" is not a claim about this client's code at
// all: it is a claim about what RomM does with a `client_device_identifier`, and
// the only way to check it is to pair twice and count the rows. `never_post` is
// the same argument in reverse -- it holds the finding that `POST /api/devices`
// creates a duplicate however it is called, so a future change back to it turns
// this red rather than accreting a device per boot on a console.
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "rig.hpp"
#include "rommsync/device_registration.hpp"
#include "rommsync/json.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/token_store.hpp"

namespace {

namespace auth = rommsync::auth;
namespace http = rommsync::http;
namespace json = rommsync::json;

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto kBudget = 90s;

const std::vector<std::string> kScopes = auth::MinimumScopes();

std::string Base64(std::string_view raw) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t at = 0; at < raw.size(); at += 3) {
    const std::size_t left = raw.size() - at;
    const unsigned a = static_cast<unsigned char>(raw[at]);
    const unsigned b = left > 1 ? static_cast<unsigned char>(raw[at + 1]) : 0u;
    const unsigned c = left > 2 ? static_cast<unsigned char>(raw[at + 2]) : 0u;
    const unsigned triple = (a << 16) | (b << 8) | c;
    out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    out.push_back(left > 1 ? kAlphabet[(triple >> 6) & 0x3F] : '=');
    out.push_back(left > 2 ? kAlphabet[triple & 0x3F] : '=');
  }
  return out;
}

/// Be the human at the browser, and be the owner of the device list.
///
/// `/api/auth/device/approve`, `GET /api/devices` and `DELETE /api/devices/{id}`
/// all take HTTP Basic, so a test can approve its own code and then inspect what
/// the approval did from the *server's* side -- which is the only vantage point
/// from which "one device, not two" is a checkable statement.
http::Result AsTheUser(http::HttpClient& client, const std::string& base, http::Method method,
                       const std::string& path, const std::string& body = {}) {
  http::Request request;
  request.method = method;
  request.url = base + path;
  request.headers.push_back(
      {"Authorization", "Basic " + Base64(std::string(rig::kUser) + ":" + rig::kPassword)});
  if (!body.empty()) {
    request.headers.push_back({"Content-Type", "application/json"});
    request.body = body;
  }
  return client.Send(request);
}

http::Result Approve(http::HttpClient& client, const std::string& base,
                     const std::string& user_code) {
  return AsTheUser(client, base, http::Method::kPost, "/api/auth/device/approve",
                   "{\"user_code\":" + json::Quote(user_code) +
                       ",\"approved_scopes\":" + json::QuoteArray(kScopes) + "}");
}

void ExpectSucceeded(rig::Checks& checks, const http::Result& result, const std::string& what) {
  checks.ExpectOk(result, what);
  if (result.ok() && !result.successful()) {
    checks.Expect(false, what + " -- HTTP " + std::to_string(result.response.status) + ": " +
                             result.response.body.substr(0, 200));
  }
}

void ExpectError(rig::Checks& checks, const auth::Registration& got,
                 auth::RegistrationError want, const std::string& what) {
  checks.ExpectEq(std::string(auth::ToString(got.error)), std::string(auth::ToString(want)),
                  what + " (" + got.message + ")");
}

// --- the server's view -------------------------------------------------------

/// Every device the fixture user owns, read as the user rather than as the
/// console: the console's own token can only ever confirm the device it holds,
/// and the duplicate this milestone exists to avoid is by definition one it does
/// not hold.
std::vector<auth::DeviceRecord> Devices(http::HttpClient& client, const std::string& base,
                                        rig::Checks& checks) {
  const http::Result result = AsTheUser(client, base, http::Method::kGet, "/api/devices");
  ExpectSucceeded(checks, result, "listing the devices");
  auth::Parsed<std::vector<auth::DeviceRecord>> parsed =
      auth::ParseDeviceList(result.response.body);
  if (!parsed.ok()) {
    checks.Expect(false, "the device list parses: " + parsed.error.Describe());
    return {};
  }
  return std::move(parsed.value);
}

std::size_t CountFor(const std::vector<auth::DeviceRecord>& devices,
                     const std::string& identifier) {
  std::size_t seen = 0;
  for (const auth::DeviceRecord& device : devices) {
    if (device.client_device_identifier == identifier) {
      ++seen;
    }
  }
  return seen;
}

void DeleteDevice(http::HttpClient& client, const std::string& base, const std::string& id) {
  AsTheUser(client, base, http::Method::kDelete, "/api/devices/" + id);
}

// --- pairing, as a means rather than as the thing under test ------------------

auth::PairingConfig Config(const std::string& base, const std::string& identifier) {
  auth::PairingConfig config;
  config.server_url = base;
  // Fixed per scenario, not random: RomM keys the device on it, so a stable
  // value means a re-run adopts the row the last run left rather than adding
  // one -- which is the same property the console depends on, exercised by the
  // test suite itself.
  config.client_device_identifier = identifier;
  config.device_name = "rommsync-nx " + identifier;
  config.requested_scopes = kScopes;
  config.max_poll_backoff = 5s;
  return config;
}

/// Pair, and hand back the token RomM granted.
///
/// The pairing itself is `pair.*`'s subject, not this file's. What is needed
/// here is a *real* token and a *real* device row, because the whole point is
/// that both came from the server.
auth::StoredToken Pair(http::HttpClient& client, const std::string& base, rig::Checks& checks,
                       const std::string& identifier) {
  auth::PairingSession session(client, Config(base, identifier));

  // RomM allows ten device inits a minute per IP, and the whole suite is one
  // IP. A console pairs once, ever, and never comes near that; a suite running
  // these back to back does, so a rate-limited init is waited out rather than
  // failed.
  const Clock::time_point give_up = Clock::now() + kBudget;
  auth::PairingState state = session.Begin();
  while (state == auth::PairingState::kFailed &&
         session.status().message.find("429") != std::string::npos && Clock::now() < give_up) {
    std::this_thread::sleep_for(2s);
    state = session.Begin();
  }
  if (state != auth::PairingState::kPending) {
    checks.Expect(false, "pairing started: " + session.status().message);
    return {};
  }

  ExpectSucceeded(checks, Approve(client, base, session.status().user_code), "approving the code");
  while (!auth::IsTerminal(state) && Clock::now() < give_up) {
    state = session.Poll();
    std::this_thread::sleep_for(100ms);
  }
  if (state != auth::PairingState::kApproved || session.token() == nullptr) {
    checks.Expect(false, "the pairing was approved: " + session.status().message);
    return {};
  }
  return auth::StoredTokenFrom(base, *session.token());
}

std::string Scratch(const std::string& scenario) {
  const std::string directory = rig::ScratchDir() + "/device-" + scenario;
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::filesystem::create_directories(directory, ignored);
  return directory + "/token.dat";
}

// --- scenarios ----------------------------------------------------------------

/// Fresh pairing registers a device and caches its id -- and the server agrees.
///
/// The acceptance criterion, checked from both ends: the id the token carried is
/// a device RomM will hand back, it is the row that names this console, and
/// there is exactly one of it.
int Registered(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  const std::string identifier = "nx-test-registered";
  const auth::StoredToken token = Pair(client, base, checks, identifier);
  if (token.device_id.empty()) {
    return checks.failures();
  }

  checks.ExpectEq(std::string(auth::ToString(auth::StateOf(token))), std::string("registered"),
                  "pairing leaves a token that names a device");

  const auth::Registration confirmed = auth::ConfirmRegistration(client, token);
  ExpectError(checks, confirmed, auth::RegistrationError::kNone, "the cached device confirms");
  checks.ExpectEq(confirmed.device.id, token.device_id, "and it is the id the token carried");
  checks.ExpectEq(confirmed.device.client_device_identifier, identifier,
                  "the row RomM created is the one that names this console");
  checks.ExpectEq(confirmed.device.platform, std::string(auth::kClientPlatform),
                  "and it is on the platform pairing declared");
  checks.Expect(confirmed.device.sync_enabled, "with sync switched on");

  // The whole registration question, asked of the server: one console, one row.
  const std::vector<auth::DeviceRecord> devices = Devices(client, base, checks);
  checks.ExpectEq(CountFor(devices, identifier), std::size_t{1},
                  "RomM holds exactly one device for this console");

  // Caching it is a no-op, because pairing already did. A console that rewrote
  // token.dat here would spend an SD write per boot for nothing.
  const std::string path = Scratch("registered");
  auth::StoredToken record = token;
  checks.Expect(auth::SaveToken(path, record).ok(), "the paired record persists");
  std::filesystem::remove(path);
  checks.Expect(auth::CacheDeviceId(path, record, confirmed).ok(), "caching the confirmed id");
  checks.Expect(!std::filesystem::exists(path),
                "...writes nothing, because the id did not change");
  return checks.failures();
}

/// Pairing a second time from the same identifier does not create a duplicate.
///
/// `pair.repair` already asserts that the second grant names the same device.
/// This asserts the thing that claim is *for*, and the one only the server can
/// answer: the device list did not grow.
int Repair(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  const std::string identifier = "nx-test-repair";

  const auth::StoredToken first = Pair(client, base, checks, identifier);
  if (first.device_id.empty()) {
    return checks.failures();
  }
  const std::size_t after_first = CountFor(Devices(client, base, checks), identifier);
  checks.ExpectEq(after_first, std::size_t{1}, "the first pairing leaves one device");

  const auth::StoredToken second = Pair(client, base, checks, identifier);
  if (second.device_id.empty()) {
    return checks.failures();
  }
  checks.ExpectEq(second.device_id, first.device_id,
                  "RomM recognises the console rather than registering a second device");
  checks.Expect(second.access_token != first.access_token, "and the token is genuinely new");
  checks.ExpectEq(CountFor(Devices(client, base, checks), identifier), std::size_t{1},
                  "and the device list did not grow");

  // Both tokens still confirm the same device: the first is not revoked by the
  // second, so a console that re-paired and then read back its old record does
  // not get a different device than the one it is about to sync with.
  const auth::Registration confirmed = auth::ConfirmRegistration(client, second);
  ExpectError(checks, confirmed, auth::RegistrationError::kNone, "the re-paired device confirms");
  checks.ExpectEq(confirmed.device.id, first.device_id, "as the device it always was");
  return checks.failures();
}

/// A token with no device id is "not fully paired", and recoverable.
///
/// The state RomM 5.2.0 never produces -- it always sends a `device_id` -- and
/// the one this client must not answer with a sync call anyway. The recovery is
/// a search, not a registration: the row is already there, and the identifier is
/// what points back at it.
int Recovers(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  const std::string identifier = "nx-test-recovers";
  const auth::StoredToken paired = Pair(client, base, checks, identifier);
  if (paired.device_id.empty()) {
    return checks.failures();
  }

  auth::StoredToken orphan = paired;
  orphan.device_id.clear();
  checks.ExpectEq(std::string(auth::ToString(auth::StateOf(orphan))), std::string("unregistered"),
                  "a token with no device is paired but not registered");

  const auth::Registration nothing = auth::ConfirmRegistration(client, orphan);
  ExpectError(checks, nothing, auth::RegistrationError::kNotRegistered,
              "there is nothing to confirm");
  checks.Expect(auth::NeedsPairing(nothing.error), "and it is not a state to sync from");
  checks.Expect(!auth::ShouldRetry(nothing.error), "nor one that retrying fixes");

  const auth::Registration found = auth::ResolveRegistration(client, orphan, identifier);
  ExpectError(checks, found, auth::RegistrationError::kNone, "the identifier finds the device");
  checks.ExpectEq(found.device.id, paired.device_id, "and it is the device pairing created");
  checks.ExpectEq(CountFor(Devices(client, base, checks), identifier), std::size_t{1},
                  "finding it created nothing");

  const std::string path = Scratch("recovers");
  checks.Expect(auth::SaveToken(path, paired).ok(), "a complete record persists");
  auth::StoredToken record = orphan;
  checks.Expect(auth::CacheDeviceId(path, record, found).ok(), "the recovered id is cached");
  checks.ExpectEq(record.device_id, paired.device_id, "into the record");
  const auth::LoadedToken reloaded = auth::LoadToken(path);
  checks.Expect(reloaded.ok(), "and onto the disk: " + reloaded.message);
  checks.ExpectEq(reloaded.value.device_id, paired.device_id, "with the device on it");

  // An identifier no device carries is not an invitation to make one.
  const auth::Registration missing =
      auth::ResolveRegistration(client, orphan, "nx-test-no-such-console");
  ExpectError(checks, missing, auth::RegistrationError::kNoSuchDevice,
              "an unknown identifier finds nothing");
  checks.Expect(auth::NeedsPairing(missing.error), "and the remedy is to pair");
  return checks.failures();
}

/// The device deleted in RomM's web UI while the token stays valid.
///
/// The cached id then names nothing, and a sync scoped by it answers 404 in the
/// middle of a negotiation. Asking at registration time turns that into a
/// sentence before the first save is touched.
int Deleted(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  const std::string identifier = "nx-test-deleted";
  const auth::StoredToken token = Pair(client, base, checks, identifier);
  if (token.device_id.empty()) {
    return checks.failures();
  }
  ExpectError(checks, auth::ConfirmRegistration(client, token), auth::RegistrationError::kNone,
              "the device confirms while it exists");

  DeleteDevice(client, base, token.device_id);
  checks.ExpectEq(CountFor(Devices(client, base, checks), identifier), std::size_t{0},
                  "the device is gone from the server");

  const auth::Registration gone = auth::ConfirmRegistration(client, token);
  ExpectError(checks, gone, auth::RegistrationError::kNoSuchDevice, "and the console notices");
  checks.Expect(auth::NeedsPairing(gone.error), "the remedy is to pair again");
  checks.Expect(!auth::ShouldRetry(gone.error), "not to keep asking");

  // The search agrees rather than inventing a replacement -- a client that
  // registered one here would hand the user a device with no sync history and
  // call it recovery.
  const auth::Registration resolved = auth::ResolveRegistration(client, token, identifier);
  ExpectError(checks, resolved, auth::RegistrationError::kNoSuchDevice,
              "and neither does the search");
  checks.ExpectEq(CountFor(Devices(client, base, checks), identifier), std::size_t{0},
                  "nothing was registered on the way past");
  return checks.failures();
}

/// A revoked token, and the fallback that must not happen.
///
/// `ResolveRegistration` falls back to a listing when the cached id names
/// nothing. A `401` is not that: the listing would fail the same way, and it is
/// the second failure that would get reported -- "no device on this server
/// belongs to this console", which sends the user to look at their device list
/// when what happened is that the token is dead.
int Revoked(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;

  auth::StoredToken bogus;
  bogus.server_url = base;
  bogus.access_token = "rmm_" + std::string(64, '0');
  bogus.device_id = "00000000-0000-0000-0000-000000000000";
  bogus.scopes = kScopes;

  const auth::Registration confirmed = auth::ConfirmRegistration(client, bogus);
  ExpectError(checks, confirmed, auth::RegistrationError::kUnauthorized,
              "a revoked token is rejected, not retried");
  checks.Expect(auth::NeedsPairing(confirmed.error), "the remedy is to pair again");
  checks.Expect(!auth::ShouldRetry(confirmed.error),
                "and a token that does not expire on its own will not start working");

  const auth::Registration resolved =
      auth::ResolveRegistration(client, bogus, "nx-test-revoked");
  ExpectError(checks, resolved, auth::RegistrationError::kUnauthorized,
              "and the diagnosis survives the resolve, rather than becoming a missing device");
  // The message is what says whether the doomed second request went out: a
  // resolve that fell through to a listing would report the listing's refusal,
  // not the lookup's.
  checks.Expect(resolved.message.find("lookup") != std::string::npos,
                "naming the call that actually failed first, not the one after it (" +
                    resolved.message + ")");
  return checks.failures();
}

/// Sync switched off for this device in RomM's own UI.
///
/// The device is there and the id is right; what is wrong is a setting, and
/// `POST /api/sync/negotiate` answers `400 "Sync is disabled for this device"`.
/// Neither waiting nor re-pairing fixes it, which is exactly why it is not
/// collapsed into either of them.
int SyncDisabled(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  const std::string identifier = "nx-test-sync-disabled";
  const auth::StoredToken token = Pair(client, base, checks, identifier);
  if (token.device_id.empty()) {
    return checks.failures();
  }

  ExpectSucceeded(checks,
                  AsTheUser(client, base, http::Method::kPut, "/api/devices/" + token.device_id,
                            "{\"sync_enabled\":false}"),
                  "turning sync off for the device");

  const auth::Registration off = auth::ConfirmRegistration(client, token);
  ExpectError(checks, off, auth::RegistrationError::kSyncDisabled, "the console reports it");
  checks.Expect(!auth::ShouldRetry(off.error), "waiting will not turn it back on");
  checks.Expect(!auth::NeedsPairing(off.error), "and neither will pairing again");
  checks.Expect(off.message.find("RomM") != std::string::npos,
                "the message says where to turn it back on");

  // The search is held to the same bar: a device the user has switched off is
  // not a device to sync with, however it was found.
  auth::StoredToken orphan = token;
  orphan.device_id.clear();
  ExpectError(checks, auth::ResolveRegistration(client, orphan, identifier),
              auth::RegistrationError::kSyncDisabled, "and so does the search that finds it");

  ExpectSucceeded(checks,
                  AsTheUser(client, base, http::Method::kPut, "/api/devices/" + token.device_id,
                            "{\"sync_enabled\":true}"),
                  "turning sync back on");
  ExpectError(checks, auth::ConfirmRegistration(client, token), auth::RegistrationError::kNone,
              "and the console goes green again");
  return checks.failures();
}

/// A server that is not there. The one failure that must not throw a pairing
/// away.
///
/// Every fault here damages a request that would otherwise have *succeeded* --
/// the device is real and the token is real, so what the client sees is a
/// healthy exchange going wrong, not a rejection wearing a transport error's
/// clothes. A bogus token would have made `drop` indistinguishable from the
/// 401 underneath it.
int Offline(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  const std::string identifier = "nx-test-offline";
  const auth::StoredToken token = Pair(client, base, checks, identifier);
  if (token.device_id.empty()) {
    return checks.failures();
  }

  const std::string path = Scratch("offline");
  checks.Expect(auth::SaveToken(path, token).ok(), "a paired record is on disk");
  const std::string before = rig::ReadFile(path);

  // A connection that dies mid-body. The proxy sends the real response's
  // headers, eight bytes of the real body, and then a TCP reset.
  ExpectSucceeded(
      checks,
      rig::ArmFault(client, base,
                    R"({"mode":"drop","bytes":8,"path":"/api/devices","count":1})"),
      "arming a lookup whose connection dies");
  const auth::Registration dropped = auth::ConfirmRegistration(client, token);
  rig::DisarmFault(client, base);
  ExpectError(checks, dropped, auth::RegistrationError::kUnreachable,
              "a dropped connection is not a verdict on the pairing");
  checks.Expect(auth::ShouldRetry(dropped.error), "it is worth trying again");
  checks.Expect(!auth::NeedsPairing(dropped.error), "and not worth a trip to the pairing screen");

  // A link that is up and going nowhere, which is what a console on sleeping
  // Wi-Fi looks like. The caller's timeout is what has to end it.
  ExpectSucceeded(
      checks,
      rig::ArmFault(client, base,
                    R"({"mode":"stall","seconds":6,"path":"/api/devices","count":1})"),
      "arming a lookup that hangs");
  const Clock::time_point started = Clock::now();
  const auth::Registration stalled = auth::ConfirmRegistration(client, token, 1500ms);
  rig::DisarmFault(client, base);
  ExpectError(checks, stalled, auth::RegistrationError::kUnreachable, "and neither is a stall");
  checks.Expect(Clock::now() - started < 5s,
                "which the caller's timeout ends, rather than the server's");

  // The one that inverts a diagnosis if `ResolveRegistration`'s fallback is
  // widened. The fault is spent after one request, so a resolve that fell
  // through to a listing would get a *healthy* answer and report that the
  // device is fine -- to a console that has just been unable to reach the
  // server at all.
  ExpectSucceeded(
      checks,
      rig::ArmFault(client, base,
                    R"({"mode":"stall","seconds":6,"path":"/api/devices","count":1})"),
      "arming a second lookup that hangs");
  const auth::Registration unresolved =
      auth::ResolveRegistration(client, token, identifier, 1500ms);
  rig::DisarmFault(client, base);
  ExpectError(checks, unresolved, auth::RegistrationError::kUnreachable,
              "an unreachable server stays unreachable through the resolve");

  ExpectSucceeded(
      checks,
      rig::ArmFault(client, base,
                    R"({"mode":"status","status":503,"path":"/api/devices","count":1})"),
      "arming a gateway having a bad minute");
  const auth::Registration unwell = auth::ConfirmRegistration(client, token);
  rig::DisarmFault(client, base);
  ExpectError(checks, unwell, auth::RegistrationError::kServerError, "nor is a 503");
  checks.Expect(auth::ShouldRetry(unwell.error), "which is also worth trying again");
  checks.Expect(!auth::NeedsPairing(unwell.error),
                "and a restarting container is not a reason to re-pair");

  // Nothing a failed confirm can do may cost the console its credentials.
  auth::StoredToken record = token;
  checks.Expect(!auth::CacheDeviceId(path, record, dropped).ok(),
                "an unconfirmed device is not cached");
  checks.ExpectEq(rig::ReadFile(path), before, "and token.dat is byte-for-byte untouched");

  // And the moment the faults are gone, so is the problem: none of this left
  // the console in a state it has to be re-paired out of.
  ExpectError(checks, auth::ConfirmRegistration(client, token), auth::RegistrationError::kNone,
              "the next attempt confirms as though nothing had happened");
  return checks.failures();
}

/// A 200 that is not the device that was asked about.
///
/// The cached id is the one thing this call is checking, so an answer *about
/// something else* says nothing about it. A client that took the body at face
/// value would cache whichever device came back and scope every sync call by
/// it -- and the first place that shows up is a save uploaded against a device
/// that never had it, which is a wrong sync history rather than a failure.
///
/// Neither shape here is one RomM produces; forcing them is what the fault
/// proxy is for (docs/TESTING.md).
int Impostor(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  const std::string identifier = "nx-test-impostor";
  const auth::StoredToken token = Pair(client, base, checks, identifier);
  if (token.device_id.empty()) {
    return checks.failures();
  }

  // A perfectly well-formed device, for a different id.
  const std::string elsewhere =
      R"({"id":"11111111-1111-1111-1111-111111111111","name":"someone else",)"
      R"("platform":"switch","client":"rommsync-nx",)"
      R"("client_device_identifier":null,"sync_enabled":true})";
  ExpectSucceeded(checks,
                  rig::ArmFault(client, base,
                                std::string(R"({"mode":"status","status":200,"body":)") +
                                    json::Quote(elsewhere) +
                                    R"(,"path":"/api/devices","count":1})"),
                  "arming an answer about a different device");
  const auth::Registration wrong = auth::ConfirmRegistration(client, token);
  rig::DisarmFault(client, base);
  ExpectError(checks, wrong, auth::RegistrationError::kMalformed,
              "an answer about another device confirms nothing");
  checks.Expect(!auth::ShouldRetry(wrong.error), "and is not something to hammer");
  checks.Expect(wrong.device.id.empty(), "the impostor is not handed back");

  // And a 200 that is not a device at all -- what a captive portal or an
  // authenticating proxy in front of RomM answers with.
  ExpectSucceeded(checks,
                  rig::ArmFault(client, base,
                                R"({"mode":"status","status":200,)"
                                R"("body":"<html>sign in</html>",)"
                                R"("path":"/api/devices","count":1})"),
                  "arming a 200 that is not JSON");
  const auth::Registration nonsense = auth::ConfirmRegistration(client, token);
  rig::DisarmFault(client, base);
  ExpectError(checks, nonsense, auth::RegistrationError::kMalformed,
              "a 200 that is not a device is not a device");

  // The list has its own version of this: RomM has no uniqueness constraint on
  // `client_device_identifier`, so two rows carrying one is a state it can be
  // in -- and one no amount of pairing gets a console out of, because pairing
  // again would find the same two. It cannot be produced by asking RomM for it,
  // which is exactly what the fault proxy is for.
  const std::string twin =
      R"({"id":"22222222-2222-2222-2222-222222222222","name":"a","platform":"switch",)"
      R"("client":"rommsync-nx","client_device_identifier":)" +
      json::Quote(identifier) + R"(,"sync_enabled":true})";
  const std::string other_twin =
      R"({"id":"33333333-3333-3333-3333-333333333333","name":"b","platform":"switch",)"
      R"("client":"rommsync-nx","client_device_identifier":)" +
      json::Quote(identifier) + R"(,"sync_enabled":true})";
  ExpectSucceeded(checks,
                  rig::ArmFault(client, base,
                                std::string(R"({"mode":"status","status":200,"body":)") +
                                    json::Quote("[" + twin + "," + other_twin + "]") +
                                    R"(,"path":"/api/devices","count":1})"),
                  "arming a list with two rows for one console");
  auth::StoredToken orphan = token;
  orphan.device_id.clear();
  const auth::Registration ambiguous =
      auth::FindRegistration(client, orphan, identifier);
  rig::DisarmFault(client, base);
  ExpectError(checks, ambiguous, auth::RegistrationError::kAmbiguous,
              "two devices for one console is reported, not guessed between");
  checks.Expect(!auth::NeedsPairing(ambiguous.error),
                "and pairing again would only find the same two");
  checks.Expect(ambiguous.message.find("2") != std::string::npos,
                "the message says how many (" + ambiguous.message + ")");

  ExpectError(checks, auth::ConfirmRegistration(client, token), auth::RegistrationError::kNone,
              "and the real device still confirms once the faults are gone");
  return checks.failures();
}

/// The finding this whole module is shaped around, held in place.
///
/// `POST /api/devices` matches an existing device on `hostname` or
/// `mac_address` and on nothing else. It does not match on
/// `client_device_identifier` -- not with `allow_existing`, and not when the
/// caller is the device-bound token of the very device it is asking about. So
/// it always creates a *second* device, which has no sync history, which makes
/// every save look like a first encounter.
///
/// This is why the client never calls it. Written down here as well as in
/// docs/API_CONTRACT.md, because a document cannot go red.
int NeverPost(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  const std::string identifier = "nx-test-never-post";
  const auth::StoredToken token = Pair(client, base, checks, identifier);
  if (token.device_id.empty()) {
    return checks.failures();
  }
  checks.ExpectEq(CountFor(Devices(client, base, checks), identifier), std::size_t{1},
                  "pairing left one device");

  http::Request request;
  request.method = http::Method::kPost;
  request.url = base + "/api/devices";
  request.headers.push_back({"Content-Type", "application/json"});
  request.headers.push_back({"Authorization", "Bearer " + token.access_token});
  request.body = std::string("{\"name\":\"rommsync-nx ") + identifier + "\",\"platform\":\"" +
                 auth::kClientPlatform + "\",\"client\":\"" + auth::kClientName +
                 "\",\"allow_existing\":true}";

  std::vector<std::string> created;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const http::Result result = client.Send(request);
    ExpectSucceeded(checks, result, "registering the device the long way");
    const std::string id = rig::JsonString(result.response.body, "device_id");
    checks.Expect(!id.empty(), "the response names a device (as `device_id`, not `id`)");
    checks.Expect(id != token.device_id,
                  "and it is NOT the device pairing created -- which is the whole finding");
    for (const std::string& seen : created) {
      checks.Expect(id != seen, "nor the one the previous call created");
    }
    if (!id.empty()) {
      created.push_back(id);
    }
  }

  const std::vector<auth::DeviceRecord> devices = Devices(client, base, checks);
  checks.ExpectEq(CountFor(devices, identifier), std::size_t{1},
                  "none of them carries the console's identifier, so none of them is findable");
  for (const std::string& id : created) {
    DeleteDevice(client, base, id);
  }
  checks.ExpectEq(created.size(), std::size_t{2},
                  "two calls, two devices: registration is not idempotent");

  // And the console is unharmed: it still confirms the device it was paired to.
  ExpectError(checks, auth::ConfirmRegistration(client, token), auth::RegistrationError::kNone,
              "the paired device is still the one the console holds");
  return checks.failures();
}

struct Scenario {
  const char* name;
  int (*run)(http::HttpClient&, const std::string&);
};

constexpr Scenario kScenarios[] = {
    {"registered", Registered},   {"repair", Repair},   {"recovers", Recovers},
    {"deleted", Deleted},         {"revoked", Revoked}, {"sync_disabled", SyncDisabled},
    {"offline", Offline},         {"impostor", Impostor},
    {"never_post", NeverPost},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: test_device_registration <scenario>\n";
    return 2;
  }
  const std::string wanted = argv[1];
  const std::string base = rig::BaseUrl();

  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);
  if (error) {
    std::cerr << "could not create " << rig::ScratchDir() << ": " << error.message() << "\n";
    return 2;
  }

  const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  // Approving needs the fixture admin, and a worktree older than the
  // provisioner may not have one.
  rig::EnsureUser(*client, base);
  // Whatever an earlier run left armed would damage this one's first request.
  rig::DisarmFault(*client, base);

  for (const Scenario& scenario : kScenarios) {
    if (wanted != scenario.name) {
      continue;
    }
    const int failures = scenario.run(*client, base);
    rig::DisarmFault(*client, base);
    if (failures == 0) {
      std::cout << "device." << scenario.name << " ok against " << base << "\n";
    }
    return failures == 0 ? 0 : 1;
  }
  std::cerr << "unknown scenario: " << wanted << "\n";
  return 2;
}
