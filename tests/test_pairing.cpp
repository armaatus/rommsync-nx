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
#include "rommsync/json.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/token_store.hpp"

namespace {

namespace auth = rommsync::auth;
namespace http = rommsync::http;
namespace json = rommsync::json;

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/// Least privilege, and enough of it that the approved set is worth reading
/// back: RomM returns what the user approved, sorted, not what was asked for.
const std::vector<std::string> kScopes = {"me.read", "roms.read", "assets.read"};

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
  // Stable per console in production (M1-5); per scenario here, so two runs do
  // not fight over one device registration.
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
    std::this_thread::sleep_for(5s);
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

  checks.ExpectOk(Approve(client, base, user_code), "approving the code");
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
          checks.ExpectOk(Approve(client, base, user_code), "approving mid-poll");
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

  checks.ExpectOk(Deny(client, base, user_code), "denying the code");
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
    std::this_thread::sleep_for(5s);
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
  checks.ExpectOk(Approve(client, base, parsed.value.user_code), "approving it");

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
  checks.ExpectOk(Approve(client, base, user_code), "approving before the trouble starts");

  checks.ExpectOk(rig::ArmFault(client, base,
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
  checks.ExpectOk(Approve(client, base, user_code), "approving, so only the 401s can stop this");

  checks.ExpectOk(rig::ArmFault(client, base,
                                R"({"mode":"status","status":401,)"
                                R"("path":"/api/auth/device/token","count":10})"),
                  "arming a proxy that always answers 401");

  const auth::PairingState state = RunUntilTerminal(session, checks);
  rig::DisarmFault(client, base);

  ExpectState(checks, state, auth::PairingState::kFailed, "a persistent 401 is a named failure");
  checks.ExpectEq(session.status().polls, 2, "after the retries the budget allowed");
  const std::string message = session.status().message;
  checks.Expect(message.find("401") != std::string::npos, "the message names the status");
  return checks.failures();
}

/// A poll that hangs, and one whose connection dies mid-body. Neither says
/// anything about the code, and neither may wedge the state machine.
///
/// The approval deliberately lands after the *recovery* poll, not just after the
/// failed one. A lost response is harmless while the answer it lost was
/// `authorization_pending`; once a code is approved, the poll that reaches RomM
/// redeems it whether or not the answer gets home -- and a stalled request is
/// still in flight upstream long after the client has given up on it, so
/// approving any earlier races that. That failure has its own ending; see
/// `LostGrant`.
int SurvivesTransport(http::HttpClient& client, const std::string& base, const std::string& fault,
                      const std::string& what) {
  rig::Checks checks;
  auth::PairingConfig config = Config(base);
  // Well under the fault's duration, so the stall is caught by our own timeout
  // rather than by CTest's.
  config.request_timeout = 1500ms;
  auth::PairingSession session(client, config);
  const std::string user_code = BeginAndShow(session, checks, base);
  checks.ExpectOk(rig::ArmFault(client, base, fault), "arming " + what);

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
          checks.ExpectOk(Approve(client, base, user_code),
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
  checks.ExpectOk(Approve(client, base, user_code), "approving the code");
  checks.ExpectOk(rig::ArmFault(client, base,
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
