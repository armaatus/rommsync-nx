// The auth structs against the REAL captured payloads.
//
// The bodies here are not typed out from the docs: they are read from
// server/contract/captures/, the unedited responses a live RomM 5.2.0 sent
// (issue M0-4). contract.captures keeps those files honest against a running
// server; this keeps the structs honest against those files. Between the two,
// a RomM that renames a field turns something red instead of being discovered
// on a console.
//
// No network here, so this never skips.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/auth.hpp"

namespace auth = rommsync::auth;
namespace json = rommsync::json;

namespace {

std::string ReadCapture(checks::Checks& c, const std::string& name) {
  const std::string path = std::string(ROMMSYNC_CAPTURES_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  c.Expect(!body.empty(), "capture " + name + " is readable at " + path);
  return body;
}

/// Every key the struct claims to know about. A capture that grew a field is
/// drift the structs have to answer for -- silently ignoring it is how a
/// client ends up not knowing about the one field that started mattering.
void OnlyKnownFields(checks::Checks& c, const std::string& body, const char* what,
                     const std::vector<std::string>& known) {
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    c.Expect(false, std::string(what) + " parses: " + document.error.Describe());
    return;
  }
  for (const json::Member& member : document.value.members()) {
    const bool recognized =
        std::find(known.begin(), known.end(), member.key) != known.end();
    c.Expect(recognized, std::string(what) + " has an unknown field: " + member.key);
  }
  for (const std::string& key : known) {
    c.Expect(document.value.Find(key) != nullptr,
             std::string(what) + " no longer carries " + key);
  }
}

void InitCapture(checks::Checks& c) {
  const std::string body = ReadCapture(c, "auth-device-init.json");
  OnlyKnownFields(c, body, "the init capture",
                  {"device_code", "user_code", "verification_path",
                   "verification_path_complete", "expires_in", "interval"});

  const auth::Parsed<auth::DeviceInitResponse> parsed = auth::ParseDeviceInitResponse(body);
  if (!parsed.ok()) {
    c.Expect(false, "the init capture parses: " + parsed.error.Describe());
    return;
  }
  const auth::DeviceInitResponse& init = parsed.value;

  // The codes are the two values the capture redacts; the rest is verbatim.
  c.ExpectEq(init.device_code, std::string("<device_code>"), "device_code");
  c.ExpectEq(init.user_code, std::string("<user_code>"), "user_code");
  c.ExpectEq(init.verification_path, std::string("/pair/device"), "verification_path");
  c.ExpectEq(init.verification_path_complete,
             std::string("/pair/device?user_code=<user_code>"),
             "verification_path_complete");
  c.ExpectEq(init.expires_in, std::int64_t{600}, "expires_in");
  c.ExpectEq(init.interval, std::int64_t{5}, "interval");

  // RomM sends a path because it is origin-agnostic; joining it is the
  // client's job, and doubling the slash gives a URL that does not route.
  c.ExpectEq(init.VerificationUrl("http://romm.lan:8080"),
             std::string("http://romm.lan:8080/pair/device"), "joined verification URL");
  c.ExpectEq(init.VerificationUrl("http://romm.lan:8080/"),
             std::string("http://romm.lan:8080/pair/device"), "a trailing slash is dropped");
  c.ExpectEq(init.VerificationUrlComplete("http://romm.lan:8080///"),
             std::string("http://romm.lan:8080/pair/device?user_code=<user_code>"),
             "...however many there are");
  c.Expect(init.poll_interval() == std::chrono::seconds{5}, "poll_interval");
  c.Expect(init.lifetime() == std::chrono::seconds{600}, "lifetime");
}

void TokenCapture(checks::Checks& c) {
  const std::string body = ReadCapture(c, "auth-device-token.json");
  OnlyKnownFields(c, body, "the token capture",
                  {"access_token", "device_id", "scopes", "expires_at"});

  const auth::Parsed<auth::DeviceTokenResponse> parsed = auth::ParseDeviceTokenResponse(body);
  if (!parsed.ok()) {
    c.Expect(false, "the token capture parses: " + parsed.error.Describe());
    return;
  }
  const auth::DeviceTokenResponse& token = parsed.value;

  c.ExpectEq(token.access_token, std::string("<access_token>"), "access_token");

  // `device_id` and the scope list are fixture values from the run that
  // produced the capture, not contract constants (captures/README.md). What is
  // a contract is their shape: a UUID, and a list of scope strings the client
  // reads back to see what it was actually granted.
  c.ExpectEq(token.device_id.size(), std::size_t{36}, "device_id is a UUID");
  c.Expect(token.device_id.find('-') != std::string::npos, "...with its dashes");
  c.Expect(!token.scopes.empty(), "the approved scopes arrive");
  c.Expect(token.HasScope("roms.read"), "an approved scope is found");
  c.Expect(!token.HasScope("me.write"), "a scope that was never requested is not");

  // The one nullable field, and null on every 5.2.0 response: this token does
  // not expire on its own, which is never an error.
  c.Expect(!token.expires_at.has_value(), "expires_at is null");
}

/// The pending poll, from the capture rather than from a literal: this is the
/// body the pairing screen sees on every tick, and the shape the OpenAPI
/// snapshot does not declare.
void PendingCapture(checks::Checks& c) {
  const std::string body = ReadCapture(c, "auth-device-token-pending.json");
  OnlyKnownFields(c, body, "the pending-poll capture", {"detail"});
  c.ExpectEq(std::string(auth::ToString(auth::ClassifyTokenPoll(400, body))),
             std::string("authorization_pending"), "the captured 400 means keep polling");
  c.Expect(auth::ShouldKeepPolling(auth::ClassifyTokenPoll(400, body)),
           "...and is retried rather than abandoned");

  // The same body is not a token: nothing may read a poll that failed as one.
  c.Expect(!auth::ParseDeviceTokenResponse(body).ok(),
           "a pending body is not a token response");
}

/// A response that has lost a field, or grown the wrong type in one, must come
/// back as a named error -- never as a struct whose missing half reads as "".
void PartialsAreRejected(checks::Checks& c) {
  struct Case {
    const char* body;
    const char* field;  // the field the error must name; "" for a syntax error
    const char* what;
  };

  const Case kInit[] = {
      {R"({"user_code":"A","verification_path":"/p","verification_path_complete":"/p?c=A","expires_in":600,"interval":5})",
       "device_code", "init without device_code"},
      {R"({"device_code":"d","verification_path":"/p","verification_path_complete":"/p?c=A","expires_in":600,"interval":5})",
       "user_code", "init without user_code"},
      {R"({"device_code":"d","user_code":"A","verification_path_complete":"/p?c=A","expires_in":600,"interval":5})",
       "verification_path", "init without verification_path"},
      {R"({"device_code":"d","user_code":"A","verification_path":"/p","expires_in":600,"interval":5})",
       "verification_path_complete", "init without verification_path_complete"},
      {R"({"device_code":"d","user_code":"A","verification_path":"/p","verification_path_complete":"/p?c=A","interval":5})",
       "expires_in", "init without expires_in"},
      {R"({"device_code":"d","user_code":"A","verification_path":"/p","verification_path_complete":"/p?c=A","expires_in":600})",
       "interval", "init without interval"},
      {R"({"device_code":"","user_code":"A","verification_path":"/p","verification_path_complete":"/p?c=A","expires_in":600,"interval":5})",
       "device_code", "init with a blank device_code"},
      {R"({"device_code":null,"user_code":"A","verification_path":"/p","verification_path_complete":"/p?c=A","expires_in":600,"interval":5})",
       "device_code", "init with a null device_code"},
      {R"({"device_code":"d","user_code":"A","verification_path":"/p","verification_path_complete":"/p?c=A","expires_in":"600","interval":5})",
       "expires_in", "init with expires_in as a string"},
      {R"({"device_code":"d","user_code":"A","verification_path":"/p","verification_path_complete":"/p?c=A","expires_in":0,"interval":5})",
       "expires_in", "init with a code that is already dead"},
      {R"({"device_code":"d","user_code":"A","verification_path":"/p","verification_path_complete":"/p?c=A","expires_in":600,"interval":-1})",
       "interval", "init with a negative interval"},
      {R"({"device_code":"d","user_code":"A","verification_path":"/p","verification_path_complete":"/p?c=A","expires_in":600,"interval":5)",
       "", "an init body cut short"},
      {"[]", "", "an init body that is an array"},
      {"<html>502 Bad Gateway</html>", "", "an init body that is a proxy error page"},
  };
  for (const Case& bad : kInit) {
    const auth::Parsed<auth::DeviceInitResponse> parsed = auth::ParseDeviceInitResponse(bad.body);
    c.Expect(!parsed.ok(), std::string("refuses ") + bad.what);
    if (parsed.ok()) {
      continue;
    }
    c.ExpectEq(parsed.error.field, std::string(bad.field),
               std::string("names the field for ") + bad.what);
    c.Expect(parsed.value.device_code.empty() && parsed.value.expires_in == 0,
             std::string("leaves the struct default-constructed for ") + bad.what);
  }

  const Case kToken[] = {
      {R"({"device_id":"d","scopes":[],"expires_at":null})", "access_token",
       "a token response without access_token"},
      {R"({"access_token":"t","scopes":[],"expires_at":null})", "device_id",
       "a token response without device_id"},
      {R"({"access_token":"t","device_id":"d","expires_at":null})", "scopes",
       "a token response without scopes"},
      {R"({"access_token":"t","device_id":"d","scopes":[]})", "expires_at",
       "a token response without expires_at"},
      {R"({"access_token":"","device_id":"d","scopes":[],"expires_at":null})", "access_token",
       "a blank access_token"},
      {R"({"access_token":"t","device_id":null,"scopes":[],"expires_at":null})", "device_id",
       "a null device_id"},
      {R"({"access_token":"t","device_id":"d","scopes":"roms.read","expires_at":null})",
       "scopes", "scopes as a bare string"},
      {R"({"access_token":"t","device_id":"d","scopes":["roms.read",7],"expires_at":null})",
       "scopes", "a number inside scopes"},
      {R"({"access_token":"t","device_id":"d","scopes":[],"expires_at":600})", "expires_at",
       "expires_at as a number"},
      {R"({"access_token":"t","device_id":"d","scopes":[],"expires_at":nul})", "",
       "a token body truncated mid-literal"},
      {"", "", "an empty token body"},
  };
  for (const Case& bad : kToken) {
    const auth::Parsed<auth::DeviceTokenResponse> parsed =
        auth::ParseDeviceTokenResponse(bad.body);
    c.Expect(!parsed.ok(), std::string("refuses ") + bad.what);
    if (parsed.ok()) {
      continue;
    }
    c.ExpectEq(parsed.error.field, std::string(bad.field),
               std::string("names the field for ") + bad.what);
    c.Expect(parsed.value.access_token.empty() && parsed.value.scopes.empty(),
             std::string("leaves the struct default-constructed for ") + bad.what);
  }

  // An approval that granted nothing is a real answer and has to arrive as
  // one: the caller disables the features whose scope is missing, which it
  // cannot do if the response was rejected.
  const auth::Parsed<auth::DeviceTokenResponse> no_scopes = auth::ParseDeviceTokenResponse(
      R"({"access_token":"t","device_id":"d","scopes":[],"expires_at":null})");
  c.Expect(no_scopes.ok(), "an empty scope list is accepted");
  c.Expect(no_scopes.value.scopes.empty(), "...and is empty");

  // 5.2.0 always sends null, but the field is declared `string | null` and the
  // pending endpoint already emits this shape. If RomM starts setting it, it
  // must arrive rather than being refused.
  const auth::Parsed<auth::DeviceTokenResponse> expiring = auth::ParseDeviceTokenResponse(
      R"({"access_token":"t","device_id":"d","scopes":["me.read"],)"
      R"("expires_at":"2026-09-04T13:04:00.528870+00:00"})");
  c.Expect(expiring.ok(), "an expires_at that is set parses: " + expiring.error.Describe());
  c.Expect(expiring.value.expires_at.has_value(), "...and arrives");
  c.ExpectEq(expiring.value.expires_at.value_or(""),
             std::string("2026-09-04T13:04:00.528870+00:00"), "expires_at");
}

/// The poll error shapes, which the OpenAPI snapshot does not declare at all.
/// Every body below was observed against a live 5.2.0 -- see docs/AUTH.md.
void PollStates(checks::Checks& c) {
  struct Case {
    int status;
    const char* body;
    auth::TokenPoll expected;
    bool keep_polling;
  };
  const Case kCases[] = {
      {200, R"({"access_token":"t","device_id":"d","scopes":[],"expires_at":null})",
       auth::TokenPoll::kGranted, false},
      {400, R"({"detail":"authorization_pending"})", auth::TokenPoll::kAuthorizationPending, true},
      {400, R"({"detail":"slow_down"})", auth::TokenPoll::kSlowDown, true},
      {400, R"({"detail":"access_denied"})", auth::TokenPoll::kAccessDenied, false},
      {400, R"({"detail":"expired_token"})", auth::TokenPoll::kExpiredToken, false},
      {429, R"({"detail":"Too many polling attempts. Try again later."})",
       auth::TokenPoll::kRateLimited, true},
      // A 400 whose detail is a shape this code has never seen. Stopping is
      // the safe answer: the alternative is polling a dead code until the
      // rate limiter answers instead.
      {400, R"({"detail":"something_new"})", auth::TokenPoll::kUnrecognized, false},
      {400, R"({"detail":{"code":"authorization_pending"}})", auth::TokenPoll::kUnrecognized,
       false},
      {400, R"({"error":"authorization_pending"})", auth::TokenPoll::kUnrecognized, false},
      {400, "<html>bad gateway</html>", auth::TokenPoll::kUnrecognized, false},
      {422, R"({"detail":[{"type":"missing","loc":["body","device_code"]}]})",
       auth::TokenPoll::kUnrecognized, false},
      {500, "", auth::TokenPoll::kUnrecognized, false},
      // A 401 does not mean "pending": nothing may read a status other than
      // 400 as one of the device-grant reasons.
      {401, R"({"detail":"authorization_pending"})", auth::TokenPoll::kUnrecognized, false},
  };
  for (const Case& poll : kCases) {
    const auth::TokenPoll got = auth::ClassifyTokenPoll(poll.status, poll.body);
    c.ExpectEq(std::string(auth::ToString(got)), std::string(auth::ToString(poll.expected)),
               "classifying " + std::to_string(poll.status));
    c.ExpectEq(auth::ShouldKeepPolling(got), poll.keep_polling,
               std::string("retryability of ") + auth::ToString(poll.expected));
  }
}

/// The interval and lifetime a poll loop is handed are clamped, because both
/// arrive over the network: a zero interval would spin the sysmodule against
/// RomM's rate limiter, and a huge one would leave the pairing screen looking
/// hung past the point the code had died anyway.
void DurationsAreClamped(checks::Checks& c) {
  auth::DeviceInitResponse init;
  init.interval = 0;
  init.expires_in = 0;
  c.Expect(init.poll_interval() == auth::kMinPollInterval, "a zero interval is floored");
  c.Expect(init.lifetime() == auth::kMinPollInterval, "a zero lifetime is floored");

  init.interval = 100'000;
  init.expires_in = 100'000;
  c.Expect(init.poll_interval() == auth::kMaxPollInterval, "a huge interval is capped");
  c.Expect(init.lifetime() == auth::kMaxPairingLifetime, "a huge lifetime is capped");
}

}  // namespace

int main() {
  checks::Checks c;
  InitCapture(c);
  TokenCapture(c);
  PendingCapture(c);
  PartialsAreRejected(c);
  PollStates(c);
  DurationsAreClamped(c);
  return c.failures() == 0 ? 0 : 1;
}
