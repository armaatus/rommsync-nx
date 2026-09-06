// The device-code pairing flow, end to end against the real docker RomM.
//
// One scenario per CTest entry (`pair.happy`, `pair.denied`, ...), selected by
// argv[1], so a red run names the behaviour that broke. Every code here is a
// real one RomM issued, every approval goes through RomM's own
// `/api/auth/device/approve`, and every denial through `/api/auth/device/deny`
// -- the grant assumes a human at a browser, and that endpoint is what lets a
// test be the human (docs/TESTING.md).
//
// The failure scenarios are the point. A pairing screen that works when the
// server is healthy and wedges on the first 503 is a console the user has to
// reboot, so `retry`, `stall`, `drop` and `unauthorized` all assert on what the
// state machine does *next* rather than on what it returned.
//
// Timing is real, deliberately: RomM paces the token endpoint per device_code
// and answers `slow_down` to a client that undercuts the interval it asked for,
// so a test that faked the clock around the poll loop would prove nothing about
// the one rule this flow has to obey. Only `expired` uses an injected clock,
// because ten minutes is not a thing to wait for and the deadline it tests is
// entirely ours.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "rig.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/json.hpp"
#include "rommsync/overlay_pairing_view.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/token_store.hpp"

namespace {

namespace auth = rommsync::auth;
namespace http = rommsync::http;
namespace json = rommsync::json;
namespace overlay = rommsync::overlay;

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/// What the client asks for in production, not a convenient subset.
///
/// `auth.scopes` already checks this list against the document; running the real
/// pairing with it is the other half — proof that a live RomM 5.2.0 accepts
/// every one of them. A scope the server does not know is the kind of thing that
/// otherwise surfaces on a console. The approved set is still worth reading
/// back: RomM returns what the user approved, sorted, not what was asked for.
const std::vector<std::string> kScopes = auth::MinimumScopes();

/// How long a scenario may spend waiting for a state machine to settle. Every
/// scenario here should finish in well under this; it exists so a wedged one
/// fails with its own message rather than as a CTest timeout.
constexpr auto kBudget = 90s;

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

/// Be the human at the browser.
///
/// `/api/auth/device/approve` and `/api/auth/device/deny` are ordinary
/// authenticated endpoints, and they take HTTP Basic -- which is why these need
/// no session cookie and no CSRF dance. That is the whole trick that makes the
/// grant testable unattended (docs/TESTING.md).
http::Result AsTheUser(http::HttpClient& client, const std::string& base, const std::string& path,
                       const std::string& body) {
  http::Request request;
  request.method = http::Method::kPost;
  request.url = base + path;
  request.headers.push_back({"Content-Type", "application/json"});
  request.headers.push_back(
      {"Authorization",
       "Basic " + Base64(std::string(rig::kUser) + ":" + rig::kPassword)});
  request.body = body;
  return client.Send(request);
}

http::Result Approve(http::HttpClient& client, const std::string& base,
                     const std::string& user_code) {
  return AsTheUser(client, base, "/api/auth/device/approve",
                   "{\"user_code\":" + json::Quote(user_code) +
                       ",\"approved_scopes\":" + json::QuoteArray(kScopes) + "}");
}

http::Result Deny(http::HttpClient& client, const std::string& base,
                  const std::string& user_code) {
  return AsTheUser(client, base, "/api/auth/device/deny",
                   "{\"user_code\":" + json::Quote(user_code) + "}");
}

auth::PairingConfig Config(const std::string& base) {
  auth::PairingConfig config;
  config.server_url = base;
  // `LoadOrCreateDeviceIdentity` derives this on a console; a fixed literal
  // here, so two runs do not fight over one device registration and so a failure
  // is never about which identifier the scenario happened to mint.
  config.client_device_identifier = "rommsync-nx-test";
  config.device_name = "rommsync-nx test";
  config.requested_scopes = kScopes;
  // Shorter than production so a backed-off retry does not dominate the run.
  // The floor is still the server's interval, which is what actually matters.
  config.max_poll_backoff = 5s;
  return config;
}

/// Drive the session the way the sysmodule will: ask it to poll far more often
/// than it should, and let it decide when a request is actually due. A session
/// that ignored `interval` would earn `slow_down` from a real RomM here.
auth::PairingState RunUntilTerminal(
    auth::PairingSession& session, rig::Checks& checks,
    const std::function<void(const auth::PairingStatus&)>& each = {}) {
  const Clock::time_point deadline = Clock::now() + kBudget;
  while (Clock::now() < deadline) {
    const auth::PairingState state = session.Poll();
    if (each) {
      each(session.status());
    }
    if (auth::IsTerminal(state)) {
      return state;
    }
    std::this_thread::sleep_for(100ms);
  }
  checks.Expect(false, "the pairing did not settle within the budget");
  return session.status().state;
}

/// `rig::Checks::ExpectOk` only says the exchange completed -- a 422 from
/// `/approve` or a 400 from `/__fault` passes it. That matters here more than
/// anywhere else in the suite: a scenario whose approval was rejected does not
/// fail on the approval, it polls a code nobody approved for the full budget and
/// reports "the pairing did not settle", which names the wrong thing. So every
/// call that sets a scenario up goes through this instead.
void ExpectSucceeded(rig::Checks& checks, const http::Result& result, const std::string& what) {
  checks.ExpectOk(result, what);
  if (result.ok() && !result.successful()) {
    checks.Expect(false, what + " -- HTTP " + std::to_string(result.response.status) + ": " +
                             result.response.body.substr(0, 200));
  }
}

/// What the fault proxy currently has armed, or an empty string.
///
/// A scenario that arms a fault and then does not get it is indistinguishable
/// from one whose code behaved differently, so the ones that depend on a fault
/// still being live check rather than assume.
std::string ArmedFault(http::HttpClient& client, const std::string& base) {
  http::Request request;
  request.url = base + "/__fault";
  const http::Result result = client.Send(request);
  return result.successful() ? result.response.body : std::string();
}

/// The pairing screen, drawn the way the overlay draws it: the status is
/// serialised, parsed back and rendered, because the overlay never sees a status
/// that did not cross IPC first (M4-5, #27).
overlay::PairingView RenderOverWire(rig::Checks& checks, const auth::PairingStatus& status) {
  const auth::Parsed<auth::PairingStatus> parsed =
      auth::ParsePairingStatus(auth::SerializePairingStatus(status));
  checks.Expect(parsed.ok(), "the status survives the wire: " + parsed.error.Describe());
  if (!parsed.ok()) {
    return overlay::PairingView{};
  }
  return overlay::RenderPairing(parsed.value);
}

/// Every string that screen would draw, joined. What a "the screen never shows
/// X" assertion is made against.
std::string DrawnText(const overlay::PairingView& view) {
  return view.headline + "\n" + view.hint + "\n" + view.code + "\n" + view.url + "\n" +
         view.qr_payload + "\n" + view.countdown + "\n" + view.detail;
}

void ExpectState(rig::Checks& checks, auth::PairingState got, auth::PairingState want,
                 const std::string& what) {
  checks.ExpectEq(std::string(auth::ToString(got)), std::string(auth::ToString(want)), what);
}

/// A live code, ready to be approved or denied.
///
/// RomM allows ten device inits a minute per IP, and the whole suite is one IP:
/// nine scenarios opening a code each will trip a limit that a console -- which
/// pairs once, ever -- never comes near. So a rate-limited init is waited out
/// rather than failed. The wait is bounded, so an init that is broken for any
/// other reason still fails here rather than hanging.
std::string BeginAndShow(auth::PairingSession& session, rig::Checks& checks,
                         const std::string& base) {
  const Clock::time_point give_up = Clock::now() + kBudget;
  auth::PairingState state = session.Begin();
  while (state == auth::PairingState::kFailed &&
         session.status().message.find("429") != std::string::npos &&
         Clock::now() < give_up) {
    // Two seconds, not five: the limit clears on a rolling minute, so a coarse
    // sleep overshoots it and is what makes these scenarios' wall clock vary by
    // half a minute between runs.
    std::this_thread::sleep_for(2s);
    state = session.Begin();
  }
  ExpectState(checks, state, auth::PairingState::kPending,
              "init leaves a live code (" + session.status().message + ")");
  const auth::PairingStatus status = session.status();
  checks.ExpectEq(status.user_code.size(), std::size_t{8}, "the user code is 8 characters");
  checks.ExpectEq(status.verification_url, base + "/pair/device",
                  "the verification path is joined onto the configured origin");
  checks.ExpectEq(status.verification_url_complete,
                  base + "/pair/device?user_code=" + status.user_code,
                  "and the complete one carries the code");
  checks.Expect(status.expires_in > 0s, "the code has time left on it");
  checks.ExpectEq(status.polls, 0, "nothing has been polled yet");
  return status.user_code;
}

// --- scenarios ---------------------------------------------------------------

/// init -> approve -> token -> persisted. The acceptance criterion the rest of
/// the milestone hangs off.
int Happy(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  auth::PairingSession session(client, Config(base));
  const std::string user_code = BeginAndShow(session, checks, base);

  ExpectSucceeded(checks, Approve(client, base, user_code), "approving the code");
  ExpectState(checks, RunUntilTerminal(session, checks), auth::PairingState::kApproved,
              "an approved code yields a token");

  const auth::DeviceTokenResponse* granted = session.token();
  if (granted == nullptr) {
    checks.Expect(false, "an approved pairing carries a token");
    return checks.failures();
  }
  checks.Expect(granted->access_token.rfind("rmm_", 0) == 0, "the token is a RomM client token");
  checks.Expect(!granted->device_id.empty(), "the pairing names a device");
  for (const std::string& scope : kScopes) {
    checks.Expect(granted->HasScope(scope), "the approved scopes include " + scope);
  }

  // "Stop polling the moment a token arrives" is a rule, not a nicety: the poll
  // that redeemed the code consumed it, so one more would answer expired_token
  // and turn a completed pairing into a failed one.
  const int polls = session.status().polls;
  for (int again = 0; again < 3; ++again) {
    session.Poll();
  }
  checks.ExpectEq(session.status().polls, polls, "an approved pairing stops polling");
  ExpectState(checks, session.status().state, auth::PairingState::kApproved, "and stays approved");

  const std::string path = rig::ScratchDir() + "/pair-happy-token.dat";
  std::filesystem::remove(path);
  const auth::StoredToken record = auth::StoredTokenFrom(base, *granted);
  const auth::StoreResult saved = auth::SaveToken(path, record);
  checks.Expect(saved.ok(), "the token is persisted: " + saved.message);
  checks.Expect(!std::filesystem::exists(path + ".tmp"), "and no partial file is left behind");

  const auth::LoadedToken reloaded = auth::LoadToken(path);
  checks.Expect(reloaded.ok(), "the persisted token reads back: " + reloaded.message);
  checks.ExpectEq(reloaded.value.access_token, granted->access_token, "with the same token");
  checks.ExpectEq(reloaded.value.device_id, granted->device_id, "and the same device");
  checks.ExpectEq(reloaded.value.server_url, base, "and the server it was issued by");

  // What crosses IPC to the overlay: the code and the URL, and nothing that
  // would let a UI leak a credential into a log or a screenshot.
  const std::string payload = auth::SerializePairingStatus(session.status());
  checks.Expect(payload.find(granted->access_token) == std::string::npos,
                "the IPC payload does not carry the token");
  const json::ParseResult document = json::Parse(payload);
  checks.Expect(document.ok(), "the IPC payload is JSON: " + document.error.Describe());
  for (const json::Member& member : document.value.members()) {
    checks.Expect(member.key != "device_code" && member.key != "access_token",
                  "the IPC payload has no secret field: " + member.key);
  }

  // ...and the same assertion one layer further out, over a real token. The
  // payload carrying no secret only matters if what is *drawn* from it carries
  // none either, and a screen is where a credential ends up in a screenshot.
  const std::string drawn = DrawnText(RenderOverWire(checks, session.status()));
  checks.Expect(drawn.find(granted->access_token) == std::string::npos,
                "the pairing screen does not draw the token");
  checks.Expect(drawn.find("rmm_") == std::string::npos,
                "...nor anything shaped like one");
  return checks.failures();
}

/// Approval that lands after several polls -- the normal case, since a human has
/// to find the page and type the code.
int MidPoll(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  auth::PairingSession session(client, Config(base));
  const std::string user_code = BeginAndShow(session, checks, base);

  const Clock::time_point started = Clock::now();
  bool approved = false;
  const auth::PairingState state =
      RunUntilTerminal(session, checks, [&](const auth::PairingStatus& status) {
        if (!approved && status.polls >= 2) {
          approved = true;
          ExpectSucceeded(checks, Approve(client, base, user_code), "approving mid-poll");
        }
      });
  ExpectState(checks, state, auth::PairingState::kApproved, "an approval mid-poll is picked up");
  checks.Expect(approved, "the approval actually happened during polling");

  // Three polls -- pending, pending, granted -- spread over two intervals. The
  // elapsed time is the assertion that bites: a loop that ignored `interval`
  // would get here in under a second, and against a slower approval it would
  // also start collecting `slow_down`s, which show up as extra polls.
  const auth::PairingStatus final_status = session.status();
  checks.ExpectEq(final_status.polls, 3, "it took exactly three polls");
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started);
  checks.Expect(elapsed >= 10s, "and it waited the interval between them, not less");
  return checks.failures();
}

/// A human refusing the code in the web UI, and the re-pair that follows.
int Denied(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  auth::PairingSession session(client, Config(base));
  const std::string user_code = BeginAndShow(session, checks, base);

  ExpectSucceeded(checks, Deny(client, base, user_code), "denying the code");
  ExpectState(checks, RunUntilTerminal(session, checks), auth::PairingState::kDenied,
              "a denied code ends as denied, not as expired");
  checks.Expect(session.token() == nullptr, "a denied pairing carries no token");
  checks.Expect(!session.status().message.empty(), "and says what happened");

  // "Re-pair" in the overlay: the same session starts over and gets a new code.
  const std::string second = BeginAndShow(session, checks, base);
  checks.Expect(second != user_code, "re-pairing asks for a fresh code");
  checks.Expect(session.token() == nullptr, "and drops whatever the last attempt held");
  return checks.failures();
}

/// Re-pair, end to end, against a real RomM: the same console, not a second one.
///
/// This is the acceptance criterion M1-5 exists for, and it is not checkable
/// without a server — only RomM can say whether the device it just paired is the
/// device it paired before. The chain under test is the whole one: derive an
/// identifier, persist it, pair, throw the token away the way "Re-pair" does,
/// and pair again. If the identifier had been kept inside `token.dat`, or the
/// second `LoadOrCreateDeviceIdentity` had preferred its seed, RomM would answer
/// with a different `device_id` here and every save on the console would start
/// again with an empty sync history.
int RePair(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;

  const std::string directory = rig::ScratchDir() + "/pair-repair";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::filesystem::create_directories(directory, ignored);
  const std::string identity_path = directory + "/" + auth::kDeviceIdentityFileName;
  const std::string token_path = directory + "/token.dat";

  // Synthetic, and per scenario so this run does not adopt another scenario's
  // device. On a console this is the serial.
  auth::IdentitySeed seed;
  seed.stable = "pair-repair-scenario";

  const auth::IdentityResult first = auth::LoadOrCreateDeviceIdentity(identity_path, seed);
  checks.Expect(first.ok(), "an identifier is derived and persisted: " + first.message);
  checks.Expect(first.created, "on the first boot it is minted");
  checks.Expect(auth::IsDeviceIdentifier(first.value.client_device_identifier),
                "and it is the documented shape");

  auth::PairingConfig config = Config(base);
  config.client_device_identifier = first.value.client_device_identifier;

  auth::PairingSession session(client, config);
  const std::string first_code = BeginAndShow(session, checks, base);
  ExpectSucceeded(checks, Approve(client, base, first_code), "approving the first code");
  ExpectState(checks, RunUntilTerminal(session, checks), auth::PairingState::kApproved,
              "the first pairing completes");
  const auth::DeviceTokenResponse* first_grant = session.token();
  if (first_grant == nullptr) {
    checks.Expect(false, "the first pairing carries a token");
    return checks.failures();
  }
  const std::string device_id = first_grant->device_id;
  const std::string first_token = first_grant->access_token;
  checks.Expect(auth::SaveToken(token_path, auth::StoredTokenFrom(base, *first_grant)).ok(),
                "and it is persisted");

  // "Re-pair": the credentials go, the identifier stays.
  checks.Expect(auth::DiscardToken(token_path), "re-pair discards the token");
  checks.Expect(!auth::LoadToken(token_path).ok(), "the console reads itself as unpaired");
  checks.Expect(std::filesystem::exists(identity_path),
                "and the identifier is still there to pair with");

  const auth::IdentityResult second = auth::LoadOrCreateDeviceIdentity(identity_path, seed);
  checks.Expect(!second.created, "the re-pair reads the identifier rather than minting one");
  checks.ExpectEq(second.value.client_device_identifier, first.value.client_device_identifier,
                  "so the same value goes to the server");

  auth::PairingConfig again_config = Config(base);
  again_config.client_device_identifier = second.value.client_device_identifier;
  auth::PairingSession again(client, again_config);
  const std::string second_code = BeginAndShow(again, checks, base);
  checks.Expect(second_code != first_code, "re-pairing asks for a fresh code");
  ExpectSucceeded(checks, Approve(client, base, second_code), "approving the second code");
  ExpectState(checks, RunUntilTerminal(again, checks), auth::PairingState::kApproved,
              "the second pairing completes");
  const auth::DeviceTokenResponse* second_grant = again.token();
  if (second_grant == nullptr) {
    checks.Expect(false, "the second pairing carries a token");
    return checks.failures();
  }

  // The whole point.
  checks.ExpectEq(second_grant->device_id, device_id,
                  "RomM recognises the console rather than registering a second device");
  checks.Expect(second_grant->access_token != first_token,
                "and it is genuinely a new token, not the discarded one coming back");
  return checks.failures();
}

/// The two ways a code dies of old age: our own deadline, and the server's.
int Expired(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;

  // Ours. Ten minutes is not a thing to wait for, and the deadline is entirely
  // client-side, so this is the one place an injected clock is honest.
  Clock::time_point now = Clock::now();
  auth::PairingSession session(client, Config(base), [&now] { return now; });
  BeginAndShow(session, checks, base);
  const std::chrono::seconds lifetime = session.status().expires_in;
  checks.Expect(lifetime > 0s, "the code reported a lifetime");

  now += lifetime + 1s;
  ExpectState(checks, session.Poll(), auth::PairingState::kExpired,
              "a code past its lifetime expires");
  checks.ExpectEq(session.status().polls, 0,
                  "without spending a poll on a code that cannot be alive");
  checks.ExpectEq(session.status().expires_in.count(), std::int64_t{0}, "and reports no time left");
  checks.Expect(session.token() == nullptr, "an expired pairing carries no token");

  // Theirs. A redeemed code answers `expired_token`, indistinguishably from one
  // that never existed -- which is why nothing re-polls to confirm a token
  // (docs/AUTH.md). Driven raw rather than through a session, because a session
  // is exactly the thing that refuses to make this call.
  http::Request init;
  init.method = http::Method::kPost;
  init.url = base + "/api/auth/device/init";
  init.headers.push_back({"Content-Type", "application/json"});
  init.body =
      R"({"client_device_identifier":"rommsync-nx-test-redeem","name":"rommsync-nx test",)"
      R"("client":"rommsync-nx","platform":"switch","requested_scopes":)" +
      json::QuoteArray(kScopes) + "}";
  // Same ten-a-minute limit as BeginAndShow waits out, for the same reason.
  http::Result opened = client.Send(init);
  const Clock::time_point give_up = Clock::now() + kBudget;
  while (opened.response.status == 429 && Clock::now() < give_up) {
    std::this_thread::sleep_for(2s);
    opened = client.Send(init);
  }
  checks.ExpectOk(opened, "opening a code to redeem twice");
  checks.ExpectEq(opened.response.status, 201, "RomM answers 201 Created, not 200");
  const auth::Parsed<auth::DeviceInitResponse> parsed =
      auth::ParseDeviceInitResponse(opened.response.body);
  if (!parsed.ok()) {
    checks.Expect(false, "the init response parses: " + parsed.error.Describe());
    return checks.failures();
  }
  ExpectSucceeded(checks, Approve(client, base, parsed.value.user_code), "approving it");

  http::Request poll;
  poll.method = http::Method::kPost;
  poll.url = base + "/api/auth/device/token";
  poll.headers.push_back({"Content-Type", "application/json"});
  poll.body = "{\"device_code\":" + json::Quote(parsed.value.device_code) + "}";

  const http::Result granted = client.Send(poll);
  checks.ExpectOk(granted, "redeeming it");
  checks.ExpectEq(std::string(auth::ToString(
                      auth::ClassifyTokenPoll(granted.response.status, granted.response.body))),
                  std::string("granted"), "the first redemption is granted");

  const http::Result twice = client.Send(poll);
  checks.ExpectOk(twice, "polling the same code again");
  checks.ExpectEq(std::string(auth::ToString(
                      auth::ClassifyTokenPoll(twice.response.status, twice.response.body))),
                  std::string("expired_token"), "a spent code reads as expired, not as granted");
  return checks.failures();
}

/// A 5xx mid-poll is a gateway having a bad minute, not a verdict on a code
/// that still has nine of its ten minutes left.
int Retries(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  auth::PairingSession session(client, Config(base));
  const std::string user_code = BeginAndShow(session, checks, base);
  ExpectSucceeded(checks, Approve(client, base, user_code), "approving before the trouble starts");

  ExpectSucceeded(checks, rig::ArmFault(client, base,
                                R"({"mode":"status","status":503,)"
                                R"("path":"/api/auth/device/token","count":2})"),
                  "arming two 503s on the token endpoint");

  bool saw_server_error = false;
  const auth::PairingState state =
      RunUntilTerminal(session, checks, [&](const auth::PairingStatus& status) {
        if (status.state == auth::PairingState::kPending &&
            status.message.find("unwell") != std::string::npos) {
          saw_server_error = true;
        }
      });
  ExpectState(checks, state, auth::PairingState::kApproved,
              "the pairing survives the server being unwell");
  checks.Expect(saw_server_error, "and the failure was visible on the pairing screen while it did");
  checks.ExpectEq(session.status().polls, 3, "two failed polls and the one that worked");
  return checks.failures();
}

/// A 401 cannot be about the device_code -- the token endpoint takes no
/// credentials -- so it is retried a few times and then diagnosed, rather than
/// silently burning the code's whole lifetime.
int Unauthorized(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  auth::PairingConfig config = Config(base);
  config.max_rejected_polls = 2;
  auth::PairingSession session(client, config);
  const std::string user_code = BeginAndShow(session, checks, base);
  ExpectSucceeded(checks, Approve(client, base, user_code), "approving, so only the 401s can stop this");

  ExpectSucceeded(checks, rig::ArmFault(client, base,
                                R"({"mode":"status","status":401,)"
                                R"("path":"/api/auth/device/token","count":10})"),
                  "arming a proxy that always answers 401");

  std::string armed_after_first;
  const auth::PairingState state =
      RunUntilTerminal(session, checks, [&](const auth::PairingStatus& status) {
        if (armed_after_first.empty() && status.polls == 1) {
          armed_after_first = ArmedFault(client, base);
        }
      });
  rig::DisarmFault(client, base);

  // Without this, a fault that vanished between the two polls reads as "the
  // state machine gave up on a 401 it should have failed on" -- the same
  // assertion failing for the opposite reason.
  checks.Expect(armed_after_first.find("401") != std::string::npos,
                "the 401 was still armed for the second poll, not just the first (" +
                    armed_after_first + ")");
  ExpectState(checks, state, auth::PairingState::kFailed, "a persistent 401 is a named failure");
  checks.ExpectEq(session.status().polls, 2, "after the retries the budget allowed");
  const std::string message = session.status().message;
  checks.Expect(message.find("401") != std::string::npos, "the message names the status");
  return checks.failures();
}

/// One armed status on the token endpoint, for the scenarios that stage a
/// sequence of them.
http::Result ArmStatus(http::HttpClient& client, const std::string& base, int status) {
  return rig::ArmFault(client, base,
                       "{\"mode\":\"status\",\"status\":" + std::to_string(status) +
                           ",\"path\":\"/api/auth/device/token\",\"count\":1}");
}

/// The 401 budget counts *consecutive* rejections, so anything else arriving in
/// between clears it.
///
/// A gateway that answers 401 once every few minutes is having a bad minute,
/// not demanding credentials; giving up on the second one an hour apart would
/// abandon a code with eight minutes left and report "the server kept rejecting
/// the poll", which is not what happened.
int RejectionStreak(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  auth::PairingConfig config = Config(base);
  // Two consecutive rejections would end the attempt -- so if the count is not
  // cleared in between, the second 401 below is fatal.
  config.max_rejected_polls = 2;
  auth::PairingSession session(client, config);
  const std::string user_code = BeginAndShow(session, checks, base);

  // The proxy holds one armed scenario at a time, so the sequence 401, 503, 401
  // is staged a step at a time as each poll consumes the last one.
  ExpectSucceeded(checks, ArmStatus(client, base, 401), "arming the first 401");
  int staged = 0;
  const auth::PairingState state =
      RunUntilTerminal(session, checks, [&](const auth::PairingStatus& status) {
        if (status.polls == staged) {
          return;
        }
        staged = status.polls;
        if (staged == 1) {
          ExpectSucceeded(checks, ArmStatus(client, base, 503), "arming the 503 in between");
        } else if (staged == 2) {
          ExpectSucceeded(checks, ArmStatus(client, base, 401), "arming the second 401");
        } else if (staged == 3) {
          ExpectSucceeded(checks, Approve(client, base, user_code), "approving after the streak");
        }
      });
  rig::DisarmFault(client, base);

  ExpectState(checks, state, auth::PairingState::kApproved,
              "two 401s with a 503 between them are not a streak");
  checks.ExpectEq(session.status().polls, 4, "401, 503, 401, granted");
  return checks.failures();
}

/// A poll that hangs, and one whose connection dies mid-body. Neither says
/// anything about the code, and neither may wedge the state machine.
///
/// The approval deliberately lands after the *recovery* poll, not just after the
/// failed one. A lost response is harmless while the answer it lost was
/// `authorization_pending`; once a code is approved, the poll that reaches RomM
/// redeems it whether or not the answer gets home. `drop` is the mode that does
/// that here -- the request lands and the answer does not -- so approving before
/// the recovery poll races it. That failure has its own ending; see `LostGrant`.
///
/// A stalled request no longer contributes to that race: since #109 the proxy
/// holds it and closes it unanswered rather than replaying it upstream once the
/// client has given up, so `pair.stall` cannot redeem a code behind the
/// scenario's back. `pair.drop` still can, which is why the ordering stays.
int SurvivesTransport(http::HttpClient& client, const std::string& base, const std::string& fault,
                      const std::string& what) {
  rig::Checks checks;
  auth::PairingConfig config = Config(base);
  // Well under the fault's duration, so the stall is caught by our own timeout
  // rather than by CTest's.
  config.request_timeout = 1500ms;
  auth::PairingSession session(client, config);
  const std::string user_code = BeginAndShow(session, checks, base);
  ExpectSucceeded(checks, rig::ArmFault(client, base, fault), "arming " + what);

  bool saw_failure = false;
  bool approved = false;
  const auth::PairingState state =
      RunUntilTerminal(session, checks, [&](const auth::PairingStatus& status) {
        if (status.state == auth::PairingState::kPending &&
            status.message.find("did not complete") != std::string::npos) {
          saw_failure = true;
        }
        if (!approved && status.polls >= 2) {
          approved = true;
          ExpectSucceeded(checks, Approve(client, base, user_code),
                          "approving once the flow has recovered");
        }
      });
  rig::DisarmFault(client, base);

  ExpectState(checks, state, auth::PairingState::kApproved, "the pairing survives " + what);
  checks.Expect(saw_failure, "the failed poll was reported rather than swallowed");
  checks.ExpectEq(session.status().polls, 3,
                  "the failed poll, the one that recovered, and the one that was granted");
  return checks.failures();
}

/// The one transport failure that cannot be retried away: the poll that redeems
/// the code arrives, and its answer does not get home.
///
/// RomM consumes an approved code on the poll it *receives*, so the token was
/// issued into a response nobody read, and every later poll answers
/// `expired_token` -- indistinguishable from a code that never existed. There
/// is nothing to recover, and the only correct behaviour is to end, distinctly
/// and quickly, on the state that tells the overlay to offer a fresh code.
int LostGrant(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  auth::PairingSession session(client, Config(base));
  const std::string user_code = BeginAndShow(session, checks, base);
  ExpectSucceeded(checks, Approve(client, base, user_code), "approving the code");
  ExpectSucceeded(checks, rig::ArmFault(client, base,
                                R"({"mode":"drop","bytes":8,)"
                                R"("path":"/api/auth/device/token","count":1})"),
                  "arming a connection that dies on the poll that redeems it");

  const auth::PairingState state = RunUntilTerminal(session, checks);
  rig::DisarmFault(client, base);

  ExpectState(checks, state, auth::PairingState::kExpired,
              "a grant lost in transit ends the attempt rather than looping on a spent code");
  checks.Expect(session.token() == nullptr, "and carries no token");
  checks.ExpectEq(session.status().polls, 2, "after the lost poll and the one that found it spent");
  return checks.failures();
}

/// `status()` answers while a request is in flight, and says something more
/// useful than "not started" while it does.
///
/// This is the one claim in `pairing.hpp` that needs two threads to check: the
/// overlay redraws on its own thread while the sysmodule's auth thread is
/// blocked in `Begin()`, and if `status()` waited on the mutex the request holds
/// -- or reported `kIdle` -- the pairing screen would be blank or wrong for as
/// long as an unreachable server takes to time out.
int Starting(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  auth::PairingConfig config = Config(base);
  config.request_timeout = 2000ms;
  auth::PairingSession session(client, config);

  ExpectSucceeded(checks,
                  rig::ArmFault(client, base,
                                R"({"mode":"stall","seconds":6,)"
                                R"("path":"/api/auth/device/init","count":1})"),
                  "arming an init that hangs");

  std::thread owner([&session] { session.Begin(); });

  bool saw_starting = false;
  const Clock::time_point give_up = Clock::now() + 10s;
  while (Clock::now() < give_up) {
    const Clock::time_point asked = Clock::now();
    const auth::PairingStatus status = session.status();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - asked);
    checks.Expect(took < 500ms, "status() answers without waiting out the request in flight");
    if (status.state == auth::PairingState::kStarting) {
      saw_starting = true;
      break;
    }
    if (auth::IsTerminal(status.state)) {
      break;
    }
    std::this_thread::sleep_for(50ms);
  }
  owner.join();
  rig::DisarmFault(client, base);

  checks.Expect(saw_starting,
                "the overlay sees 'starting' rather than 'idle' while the init is in flight");
  ExpectState(checks, session.status().state, auth::PairingState::kFailed,
              "and an init that never answers ends as a named failure");
  return checks.failures();
}

/// The IPC payload the overlay's pairing screen (M4-5) will decode.
int Payload(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  auth::PairingSession session(client, Config(base));
  BeginAndShow(session, checks, base);

  const auth::PairingStatus status = session.status();
  const auth::Parsed<auth::PairingStatus> back =
      auth::ParsePairingStatus(auth::SerializePairingStatus(status));
  checks.Expect(back.ok(), "a status round trips: " + back.error.Describe());
  ExpectState(checks, back.value.state, status.state, "state survives");
  checks.ExpectEq(back.value.user_code, status.user_code, "user_code survives");
  checks.ExpectEq(back.value.verification_url, status.verification_url, "the url survives");
  checks.ExpectEq(back.value.verification_url_complete, status.verification_url_complete,
                  "the complete url survives");
  checks.ExpectEq(back.value.expires_in.count(), status.expires_in.count(), "expires_in survives");
  checks.ExpectEq(back.value.polls, status.polls, "the poll count survives");

  // An idle status is mostly empty strings, which is exactly the shape a strict
  // reader refuses if nobody thought about it -- and the overlay asks for the
  // state before anything has started.
  const auth::Parsed<auth::PairingStatus> idle =
      auth::ParsePairingStatus(auth::SerializePairingStatus(auth::PairingStatus{}));
  checks.Expect(idle.ok(), "an idle status round trips too: " + idle.error.Describe());
  ExpectState(checks, idle.value.state, auth::PairingState::kIdle, "and stays idle");

  const auth::Parsed<auth::PairingStatus> nonsense =
      auth::ParsePairingStatus(R"({"state":"sideways","user_code":"","verification_url":"",)"
                               R"("verification_url_complete":"","expires_in":0,"polls":0,)"
                               R"("message":""})");
  checks.Expect(!nonsense.ok(), "a state this build does not know is refused");

  const auth::Parsed<auth::PairingStatus> negative =
      auth::ParsePairingStatus(R"({"state":"pending","user_code":"ABCD2345",)"
                               R"("verification_url":"","verification_url_complete":"",)"
                               R"("expires_in":-1,"polls":0,"message":""})");
  checks.Expect(!negative.ok(), "a countdown that runs backwards is refused");

  const auth::Parsed<auth::PairingStatus> missing =
      auth::ParsePairingStatus(R"({"state":"pending","user_code":"ABCD2345",)"
                               R"("verification_url":"","verification_url_complete":"",)"
                               R"("polls":0,"message":""})");
  checks.Expect(!missing.ok(), "a payload missing a count is refused rather than defaulted");

  // Begin() -> status() -> serialize -> parse -> view, against a live RomM: the
  // whole chain the overlay's pairing screen sits at the end of. What it has to
  // yield is a code and an address a human could actually type (M4-5, #27).
  const overlay::PairingView view = RenderOverWire(checks, status);
  checks.ExpectEq(view.code, status.user_code, "the screen shows the code RomM issued");
  checks.ExpectEq(view.code.size(), std::size_t{8}, "...all eight characters of it");
  checks.Expect(view.code.find('-') == std::string::npos, "...with nothing inserted into it");
  checks.ExpectEq(view.url, base + "/pair/device", "and an absolute address to type it into");
  checks.Expect(view.url.rfind("http", 0) == 0, "...which is a URL rather than a path");
  checks.Expect(view.url.find("//pair") == std::string::npos,
                "...and was not joined onto the origin a second time");
  checks.Expect(!view.countdown.empty(), "the code counts down");
  checks.Expect(!view.start && !view.start_over,
                "and a live code offers neither Pair nor Start over");
  checks.ExpectEq(view.qr_payload, status.verification_url_complete,
                  "the QR payload is the address with the code on it");
  return checks.failures();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: test_pairing <scenario>\n";
    return 2;
  }
  const std::string scenario = argv[1];
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

  int failures = 0;
  if (scenario == "happy") {
    failures = Happy(*client, base);
  } else if (scenario == "mid_poll") {
    failures = MidPoll(*client, base);
  } else if (scenario == "repair") {
    failures = RePair(*client, base);
  } else if (scenario == "denied") {
    failures = Denied(*client, base);
  } else if (scenario == "expired") {
    failures = Expired(*client, base);
  } else if (scenario == "retry") {
    failures = Retries(*client, base);
  } else if (scenario == "unauthorized") {
    failures = Unauthorized(*client, base);
  } else if (scenario == "stall") {
    failures = SurvivesTransport(*client, base,
                                 R"({"mode":"stall","seconds":4,)"
                                 R"("path":"/api/auth/device/token","count":1})",
                                 "a stalled poll");
  } else if (scenario == "drop") {
    failures = SurvivesTransport(*client, base,
                                 R"({"mode":"drop","bytes":8,)"
                                 R"("path":"/api/auth/device/token","count":1})",
                                 "a dropped poll");
  } else if (scenario == "rejection_streak") {
    failures = RejectionStreak(*client, base);
  } else if (scenario == "starting") {
    failures = Starting(*client, base);
  } else if (scenario == "lost_grant") {
    failures = LostGrant(*client, base);
  } else if (scenario == "payload") {
    failures = Payload(*client, base);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  rig::DisarmFault(*client, base);
  if (failures == 0) {
    std::cout << "pair." << scenario << " ok against " << base << "\n";
  }
  return failures == 0 ? 0 : 1;
}
