// M10-19 (#245): "Install on device" from RomM's web UI.
//
//   parse  -- no server: the claim's answer, the verdict a queue row gives, and
//             the two bodies this client sends
//   live   -- the three exchanges against the fixture, on a device of the test's
//             own: register, claim (twice, to see it is idempotent), report, and
//             the 404 for a request already ended
//   faults -- the claim's retry, through the proxy
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "harness.hpp"
#include "rig.hpp"
#include "rommsync/download.hpp"
#include "rommsync/http.hpp"
#include "rommsync/json.hpp"
#include "rommsync/remote_install.hpp"

namespace {

using namespace rommsync;

/// A claim's answer as the 5.4.0 fixture gave it, verbatim: two requests, the
/// second for a disc set with two file ids.
constexpr const char* kClaimed =
    R"([{"id":"5c80f3a5-3740-41ab-a994-aec881962d56","user_id":1,)"
    R"("device_id":"317dd82a-6f5c-4738-a52d-6ff5de34dfd8","rom_id":5,"file_ids":[5],)"
    R"("status":"taken","reason":null,"created_at":"2026-10-22T06:27:22.607668+00:00",)"
    R"("updated_at":"2026-10-22T06:27:22.645448+00:00"},)"
    R"({"id":"c2058390-e1d8-4489-863f-25bbf1101768","user_id":1,)"
    R"("device_id":"317dd82a-6f5c-4738-a52d-6ff5de34dfd8","rom_id":7,"file_ids":[7,8],)"
    R"("status":"taken","reason":null,"created_at":"2026-10-22T06:27:22.634966+00:00",)"
    R"("updated_at":"2026-10-22T06:27:22.645448+00:00"}])";

download::QueueEntry Row(std::int64_t rom_id, download::QueueState state,
                         std::string message = {}) {
  download::QueueEntry entry;
  entry.rom_id = rom_id;
  entry.state = state;
  entry.message = std::move(message);
  return entry;
}

void Parse(rig::Checks& checks) {
  const auth::Parsed<install::ClaimPage> claimed = install::ParseClaim(kClaimed);
  checks.Expect(claimed.ok(), "the fixture's claim reads: " + claimed.error.Describe());
  checks.ExpectEq(claimed.value.requests.size(), std::size_t{2}, "two requests");
  if (claimed.value.requests.size() == 2) {
    checks.ExpectEq(claimed.value.requests[0].id,
                    std::string("5c80f3a5-3740-41ab-a994-aec881962d56"), "oldest first, by id");
    checks.ExpectEq(claimed.value.requests[0].rom_id, std::int64_t{5}, "with its rom");
    checks.ExpectEq(claimed.value.requests[1].rom_id, std::int64_t{7}, "and the second's");
  }
  checks.ExpectEq(claimed.value.dropped, std::size_t{0}, "none dropped");

  const auth::Parsed<install::ClaimPage> none = install::ParseClaim("[]");
  checks.Expect(none.ok() && none.value.requests.empty(), "an empty claim is no requests");

  // Past the bound, the rest stay taken on the server and are counted.
  const auth::Parsed<install::ClaimPage> one = install::ParseClaim(kClaimed, 1);
  checks.Expect(one.ok() && one.value.requests.size() == 1 && one.value.dropped == 1,
                "a claim past `max` keeps the oldest and counts the rest");

  for (const char* broken :
       {"", "{}", R"({"items":[]})", "[1]", R"([{"rom_id":5}])", R"([{"id":"a"}])",
        R"([{"id":"","rom_id":5}])", R"([{"id":"a","rom_id":0}])",
        R"([{"id":"a","rom_id":"5"}])", R"([{"id":"a/../b","rom_id":5}])",
        R"([{"id":"a?b","rom_id":5}])", R"([{"id":"ok","rom_id":5},{"id":"x"}])",
        R"([{"id":"a","rom_id":5}] trailing)"}) {
    checks.Expect(!install::ParseClaim(broken).ok(), std::string("refused: ") + broken);
  }
  const std::string long_id(install::kMaxRequestIdBytes + 1, 'a');
  checks.Expect(!install::ParseClaim(R"([{"id":")" + long_id + R"(","rom_id":5}])").ok(),
                "an id past kMaxRequestIdBytes is refused, not pasted into a URL");
  // The one rule a request id and a device id are both held to before a path.
  checks.Expect(http::IsUrlPathSegment("3f2a-9c_1.x~"), "the unreserved set is a path segment");
  for (const char* unsafe : {"", "a/b", "a?b", "a#b", "a%2Fb", "a b", ".. /"}) {
    checks.Expect(!http::IsUrlPathSegment(unsafe), std::string("not a path segment: ") + unsafe);
  }

  // The verdict is the queue row's terminal state.
  using download::QueueState;
  for (QueueState pending : {QueueState::kQueued, QueueState::kActive, QueueState::kVerifying}) {
    checks.Expect(!install::VerdictFor(Row(5, pending)).has_value(),
                  std::string("nothing to report while the row is ") + download::ToString(pending));
  }
  const std::optional<install::Verdict> done =
      install::VerdictFor(Row(5, QueueState::kDone, "verified against the server's sha1"));
  checks.Expect(done && done->outcome == install::Outcome::kDone && done->reason.empty(),
                "a row downloaded is done");
  const std::optional<install::Verdict> there =
      install::VerdictFor(Row(5, QueueState::kDone, download::kAlreadyOnCardMessage));
  checks.Expect(there && there->outcome == install::Outcome::kAlreadyInstalled,
                "a row found already on the card is already_installed");
  const std::optional<install::Verdict> skipped =
      install::VerdictFor(Row(7, QueueState::kSkipped, "a multi-disc set is not downloaded"));
  checks.Expect(skipped && skipped->outcome == install::Outcome::kFailed &&
                    skipped->reason == "a multi-disc set is not downloaded",
                "a skipped row is failed, with the row's reason");
  const std::optional<install::Verdict> failed =
      install::VerdictFor(Row(5, QueueState::kFailed));
  checks.Expect(failed && failed->outcome == install::Outcome::kFailed && !failed->reason.empty(),
                "a failed row with no message still says something");
  const std::optional<install::Verdict> removed = install::VerdictFor(download::QueueEntry{});
  checks.Expect(removed && removed->outcome == install::Outcome::kFailed &&
                    removed->reason.find("removed") != std::string::npos,
                "no row at all is a failure that says it was removed on the console: " +
                    (removed ? removed->reason : std::string("nothing")));

  checks.ExpectEq(std::string(install::ToString(install::Outcome::kDone)), std::string("done"),
                  "done");
  checks.ExpectEq(std::string(install::ToString(install::Outcome::kAlreadyInstalled)),
                  std::string("already_installed"), "already_installed");
  checks.ExpectEq(std::string(install::ToString(install::Outcome::kFailed)),
                  std::string("failed"), "failed");

  checks.ExpectEq(install::CapabilitiesBody(true),
                  std::string(R"({"capabilities":{"remote_install":true}})"), "registering");
  checks.ExpectEq(install::CapabilitiesBody(false),
                  std::string(R"({"capabilities":{"remote_install":false}})"), "and withdrawing");
  checks.ExpectEq(install::ReportBody({install::Outcome::kDone, {}}),
                  std::string(R"({"status":"done"})"), "a report with no reason has none");
  checks.ExpectEq(install::ReportBody({install::Outcome::kFailed, "the card is \"full\""}),
                  std::string(R"({"status":"failed","reason":"the card is \"full\""})"),
                  "a reason is quoted, not concatenated");
  // Cut on a UTF-8 boundary: 499 bytes of ASCII and a two-byte character
  // straddling the bound lose the whole character.
  const std::string straddling = std::string(install::kMaxReasonBytes - 1, 'x') + "\xC3\xA9";
  const json::ParseResult cut =
      json::Parse(install::ReportBody({install::Outcome::kFailed, straddling}));
  const json::Value* reason = cut.ok() ? cut.value.Find("reason") : nullptr;
  checks.Expect(reason != nullptr && reason->string() == straddling.substr(0, 499),
                "a reason is cut to kMaxReasonBytes, never through a character");
}

http::Result QueueFromTheBrowser(http::HttpClient& client, const std::string& base,
                                 const std::string& device_id, std::int64_t rom_id) {
  return rig::AsTheUser(client, base, "/api/devices/" + device_id + "/installs",
                        "{\"rom_id\":" + std::to_string(rom_id) + "}");
}

void Live(rig::Checks& checks, http::HttpClient& client, const std::string& base) {
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    checks.Expect(false, "the fixture is provisioned");
    return;
  }
  harness::Rom rom;
  checks.Expect(harness::FindRom(client, base, fixture, "nova.nes", &rom), "the library has nova");
  const auth::StoredToken token = harness::PairDevice(checks, client, base, "install-live");
  if (token.device_id.empty() || rom.id == 0) {
    return;
  }
  // A re-run adopts its row, so whatever an earlier one left taken is ended
  // first, and its capability withdrawn.
  for (const install::Request& left : install::Claim(client, base, token).page.requests) {
    install::Report(client, base, token, left.id, {install::Outcome::kFailed, "an earlier run"});
  }
  const install::Exchange withdrawn = install::Register(client, base, token, false);
  checks.Expect(withdrawn.ok(), "a device can say it takes no remote installs: " +
                                    withdrawn.message);
  checks.ExpectEq(QueueFromTheBrowser(client, base, token.device_id, rom.id).response.status, 400,
                  "and then the web UI cannot queue one for it");

  const install::Exchange registered = install::Register(client, base, token, true);
  checks.Expect(registered.ok(), "registering remote_install: " + registered.message);
  checks.ExpectEq(registered.attempts, 1, "in one request");
  http::Request mine;
  mine.url = base + "/api/devices/" + token.device_id;
  mine.headers.push_back({"Authorization", "Bearer " + token.access_token});
  const json::ParseResult device = json::Parse(client.Send(mine).response.body);
  const json::Value* caps = device.ok() ? device.value.Find("capabilities") : nullptr;
  const json::Value* remote = caps == nullptr ? nullptr : caps->Find(install::kCapability);
  checks.Expect(remote != nullptr && remote->boolean(),
                "RomM records the capability on the paired device itself");

  const http::Result queued = QueueFromTheBrowser(client, base, token.device_id, rom.id);
  checks.ExpectEq(queued.response.status, 201, "so the web UI queues nova for it");
  const std::string request_id = rig::JsonString(queued.response.body, "id");

  const install::Claimed first = install::Claim(client, base, token);
  checks.Expect(first.exchange.ok(), "the device claims: " + first.exchange.message);
  checks.ExpectEq(first.page.requests.size(), std::size_t{1}, "one request");
  if (first.page.requests.size() == 1) {
    checks.ExpectEq(first.page.requests[0].id, request_id, "the one the browser queued");
    checks.ExpectEq(first.page.requests[0].rom_id, rom.id, "for nova");
  }
  const install::Claimed again = install::Claim(client, base, token);
  checks.Expect(again.exchange.ok() && again.page.requests.size() == 1,
                "a second claim hands back the same taken request: nothing to persist");

  // Another device's token: RomM answers as though the device were not there.
  auth::StoredToken elsewhere = token;
  elsewhere.device_id = fixture.device_id;
  const install::Claimed foreign = install::Claim(client, base, elsewhere);
  checks.ExpectEq(foreign.exchange.status, 404, "a token bound to another device claims nothing");

  // Newer than this, so a notice an earlier run left cannot pass for this one.
  const std::int64_t before_report = rig::LatestNotificationId(client, base);
  const install::Exchange reported =
      install::Report(client, base, token, request_id, {install::Outcome::kDone, {}});
  checks.Expect(reported.ok(), "the device reports it done: " + reported.message);
  checks.Expect(install::Claim(client, base, token).page.requests.empty(),
                "which ends it: the next claim is empty");
  const install::Exchange twice =
      install::Report(client, base, token, request_id, {install::Outcome::kDone, {}});
  checks.Expect(!twice.ok() && twice.ended() && !twice.transient(),
                "a second report is a 404 that settles it, not a retry: HTTP " +
                    std::to_string(twice.status));
  checks.ExpectEq(rig::InstallNoticesSince(client, base, before_report,
                                           "rommsync-nx install-live", "done"),
                  std::size_t{1}, "RomM tells the user the install on this device is done");

  // A failure, with a reason past the server's bound: cut, not refused.
  const http::Result requeued = QueueFromTheBrowser(client, base, token.device_id, rom.id);
  const std::string second = rig::JsonString(requeued.response.body, "id");
  install::Claim(client, base, token);
  const install::Exchange failed = install::Report(
      client, base, token, second,
      {install::Outcome::kFailed, std::string(install::kMaxReasonBytes + 100, 'r')});
  checks.Expect(failed.ok(), "a failure with a long reason is accepted: " + failed.message);

  install::Register(client, base, token, false);
  harness::DeleteDevice(client, base, fixture, token.device_id);
}

void Faults(rig::Checks& checks, http::HttpClient& client, const std::string& base) {
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    checks.Expect(false, "the fixture is provisioned");
    return;
  }
  const auth::StoredToken token = harness::PairDevice(checks, client, base, "install-faults");
  if (token.device_id.empty()) {
    return;
  }
  // A request to claim, so the answer is long enough to be cut or dropped mid-way.
  harness::Rom rom;
  checks.Expect(harness::FindRom(client, base, fixture, "nova.nes", &rom), "the library has nova");
  checks.Expect(install::Register(client, base, token, true).ok(), "the device registers");
  checks.Expect(QueueFromTheBrowser(client, base, token.device_id, rom.id).successful(),
                "and the web UI queues nova for it");

  const std::string path = "/api/devices/" + token.device_id + "/installs/claim";
  std::vector<std::chrono::milliseconds> waits;
  sync::CallPolicy policy;
  policy.wait = [&waits](std::chrono::milliseconds delay) { waits.push_back(delay); };

  rig::ArmFault(client, base, R"({"mode":"status","status":503,"path":")" + path + "\"}");
  const install::Claimed retried = install::Claim(client, base, token, policy);
  checks.Expect(retried.exchange.ok(), "a 503 is retried: " + retried.exchange.message);
  checks.ExpectEq(retried.exchange.attempts, 2, "two requests");
  checks.ExpectEq(waits.size(), std::size_t{1}, "with one backoff between them");

  waits.clear();
  rig::ArmFault(client, base,
                R"({"mode":"status","status":401,"count":5,"path":")" + path + "\"}");
  const install::Claimed refused = install::Claim(client, base, token, policy);
  rig::DisarmFault(client, base);
  checks.ExpectEq(refused.exchange.status, 401, "a 401 is reported as one");
  checks.Expect(!refused.exchange.transient() && refused.exchange.attempts == 1,
                "and not retried");

  waits.clear();
  rig::ArmFault(client, base,
                R"({"mode":"truncate","bytes":40,"count":5,"path":")" + path + "\"}");
  const install::Claimed cut = install::Claim(client, base, token, policy);
  rig::DisarmFault(client, base);
  checks.Expect(!cut.exchange.ok() && cut.page.requests.empty(),
                "a truncated answer yields no requests: " + cut.exchange.message);

  waits.clear();
  rig::ArmFault(client, base, R"({"mode":"drop","bytes":10,"count":5,"path":")" + path + "\"}");
  const install::Claimed dropped = install::Claim(client, base, token, policy);
  rig::DisarmFault(client, base);
  checks.Expect(dropped.exchange.transport != http::Error::kNone && dropped.exchange.transient(),
                "a reset is a transient transport failure");
  checks.ExpectEq(dropped.exchange.attempts, policy.max_attempts, "retried to the budget");

  http::CancelToken cancel;
  cancel.Cancel();
  sync::CallPolicy canceled = policy;
  canceled.cancel = &cancel;
  const install::Claimed stopped = install::Claim(client, base, token, canceled);
  checks.Expect(stopped.exchange.transport == http::Error::kCanceled &&
                    stopped.exchange.attempts == 0,
                "a fired token costs no request");
  checks.ExpectEq(install::Claim(client, base, token).page.requests.size(), std::size_t{1},
                  "and none of it lost the request: the next clean claim still holds it");
  harness::DeleteDevice(client, base, fixture, token.device_id);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "parse";
  rig::Checks checks;
  if (scenario == "parse") {
    Parse(checks);
  } else {
    const std::string base = rig::BaseUrl();
    const std::unique_ptr<http::HttpClient> client = rig::MakeClient();
    if (!rig::Reachable(*client, base)) {
      std::cerr << "rig unreachable at " << base
                << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
      return rig::kSkip;
    }
    if (scenario == "live") {
      Live(checks, *client, base);
    } else if (scenario == "faults") {
      Faults(checks, *client, base);
    } else {
      std::cerr << "unknown scenario " << scenario << "\n";
      return 2;
    }
  }
  if (checks.failures() == 0) {
    std::cout << "install." << scenario << " ok\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
