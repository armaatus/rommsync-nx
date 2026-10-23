// M10-2 (#228): which RomM this console is talking to, and what it can do.
//
// `versions` and `heartbeat` need no server: the version rule and the shallow
// read of a heartbeat body, against literal bodies taken from the fixture's
// 5.2.0 and 5.4.0. `live` asks the fixture itself -- whichever version
// `ROMM_VERSION` started -- and `faults` drives the retry through the proxy.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "rig.hpp"
#include "rommsync/json.hpp"
#include "rommsync/server_capabilities.hpp"

namespace {

using namespace rommsync;

/// The whole 5.2.0 heartbeat the fixture answers, verbatim (1,166 bytes on the
/// fixture of #227): the shape `ParseHeartbeatVersion` walks past.
constexpr const char* kHeartbeat520 =
    R"({"SYSTEM":{"VERSION":"5.2.0","SHOW_SETUP_WIZARD":false},)"
    R"("METADATA_SOURCES":{"ANY_SOURCE_ENABLED":true,"IGDB_API_ENABLED":false,)"
    R"("SS_API_ENABLED":false,"SS_DEV_CREDENTIALS_SET":true,"MOBY_API_ENABLED":false,)"
    R"("STEAMGRIDDB_API_ENABLED":false,"RA_API_ENABLED":false,"LAUNCHBOX_API_ENABLED":false,)"
    R"("HASHEOUS_API_ENABLED":false,"PLAYMATCH_API_ENABLED":false,"TGDB_API_ENABLED":false,)"
    R"("FLASHPOINT_API_ENABLED":false,"HLTB_API_ENABLED":false,"LIBRETRO_API_ENABLED":true},)"
    R"("FILESYSTEM":{"FS_PLATFORMS":["gba","nes","psx","gb"]},)"
    R"("EMULATION":{"DISABLE_EMULATOR_JS":false,"DISABLE_RUFFLE_RS":false},)"
    R"("FRONTEND":{"DISABLE_USERPASS_LOGIN":false,"DISABLE_LOGS_VIEWER":false,)"
    R"("YOUTUBE_BASE_URL":"https://www.youtube.com"},)"
    R"("OIDC":{"ENABLED":false,"AUTOLOGIN":false,"PROVIDER":"","RP_INITIATED_LOGOUT":false},)"
    R"("TASKS":{"ENABLE_SCHEDULED_RESCAN":false,"SCHEDULED_RESCAN_CRON":"0 3 * * *"}})";

bool AllFlags(const server::ServerCapabilities& caps) {
  return caps.rom_ids && caps.slot_cap && caps.delete_action && caps.emulators_filter &&
         caps.restore_unlisted && caps.hash_baselines && caps.device_capabilities;
}

bool NoFlags(const server::ServerCapabilities& caps) {
  return !caps.rom_ids && !caps.slot_cap && !caps.delete_action && !caps.emulators_filter &&
         !caps.restore_unlisted && !caps.hash_baselines && !caps.device_capabilities;
}

void Versions(rig::Checks& checks) {
  struct Case {
    const char* text;
    bool parses;
    server::Version expected;
  };
  const std::vector<Case> cases = {
      {"5.4.0", true, {5, 4, 0}},
      {"5.4.0-beta.1", true, {5, 4, 0}},
      {"5.4.0+git.abc", true, {5, 4, 0}},
      {"v5.4.1", true, {5, 4, 1}},
      {"5.4", true, {5, 4, 0}},
      {"5.10.2", true, {5, 10, 2}},
      {"", false, {}},
      {"<version>", false, {}},
      {"development", false, {}},
      {"5", false, {}},
      {"5.x.0", false, {}},
      {"5.4.0.1", false, {}},
      {"5..0", false, {}},
      {"99999999999.0.0", false, {}},
  };
  for (const Case& c : cases) {
    const std::optional<server::Version> parsed = server::ParseVersion(c.text);
    checks.Expect(parsed.has_value() == c.parses,
                  std::string("ParseVersion(\"") + c.text + "\") " +
                      (c.parses ? "parses" : "is refused"));
    if (parsed.has_value() && c.parses) {
      checks.Expect(*parsed == c.expected, std::string("ParseVersion(\"") + c.text +
                                               "\") is " + server::ToString(c.expected) +
                                               ", got " + server::ToString(*parsed));
    }
  }
  checks.ExpectEq(server::ToString(server::kMinimumVersion), std::string("5.4.0"),
                  "the floor is 5.4.0 (D1, #226)");

  // The acceptance's unit half: 5.2.0 and every 5.3.x are refused.
  for (const char* old : {"5.2.0", "5.2.9", "5.3.0", "5.3.1", "5.3.9-rc1", "4.8.1"}) {
    const server::ServerCapabilities caps = server::CapabilitiesFor(old);
    checks.Expect(caps.known, std::string(old) + " is a version this client can read");
    checks.Expect(!caps.supported, std::string(old) + " is below the floor and refused");
    checks.ExpectEq(caps.version, std::string(old), "and the version is kept as spelled");
  }
  // What 5.3 brought is still said, refused or not: the struct describes the
  // server, and the refusal is a separate fact about this build.
  const server::ServerCapabilities five_three = server::CapabilitiesFor("5.3.1");
  checks.Expect(five_three.rom_ids && five_three.slot_cap, "5.3 has rom_ids and the slot cap");
  checks.Expect(!five_three.delete_action && !five_three.hash_baselines &&
                    !five_three.emulators_filter,
                "and none of what 5.4 added");
  checks.Expect(NoFlags(server::CapabilitiesFor("5.2.0")), "5.2.0 has none of the flags");

  for (const char* current : {"5.4.0", "5.4.0-beta.1", "5.4.3", "5.5.0", "6.0.0"}) {
    const server::ServerCapabilities caps = server::CapabilitiesFor(current);
    checks.Expect(caps.known && caps.supported, std::string(current) + " is supported");
    checks.Expect(AllFlags(caps), std::string(current) + " has the full capability set");
  }

  // Missing or unparseable reads as the newest -- provision.py's rule.
  for (const char* unknown : {"", "<version>", "development"}) {
    const server::ServerCapabilities caps = server::CapabilitiesFor(unknown);
    checks.Expect(!caps.known, std::string("\"") + unknown + "\" is not a known version");
    checks.Expect(caps.supported && AllFlags(caps),
                  std::string("\"") + unknown + "\" reads as the newest server");
  }

  const std::string long_text(200, '5');
  checks.Expect(server::CapabilitiesFor(long_text).version.size() <= server::kMaxVersionBytes,
                "a version string the server made up is cut, not carried whole");
}

void Heartbeat(rig::Checks& checks) {
  const auth::Parsed<std::string> old = server::ParseHeartbeatVersion(kHeartbeat520);
  checks.Expect(old.ok(), "the fixture's whole 5.2.0 heartbeat reads: " + old.error.Describe());
  checks.ExpectEq(old.value, std::string("5.2.0"), "and says 5.2.0");

  // server/contract/captures/5.4.0/heartbeat-system.json, with GIT_BRANCH.
  const auth::Parsed<std::string> current = server::ParseHeartbeatVersion(
      R"({ "SYSTEM" : { "VERSION": "5.4.0", "GIT_BRANCH": null, "SHOW_SETUP_WIZARD": false } })");
  checks.Expect(current.ok() && current.value == "5.4.0", "the 5.4.0 capture says 5.4.0");

  // SYSTEM anywhere among the members, not only first.
  const auth::Parsed<std::string> later = server::ParseHeartbeatVersion(
      R"({"FILESYSTEM":{"FS_PLATFORMS":["a\"}",{"x":[1,2,{"y":"]"}]}]},)"
      R"("SYSTEM":{"VERSION":"5.4.0-beta.1"}})");
  const auth::Parsed<std::string> numbers = server::ParseHeartbeatVersion(
      R"({"A":-1.5e+3,"B":[0,12,3.25,1E9],"C":-0,"SYSTEM":{"VERSION":"5.4.0"}})");
  checks.Expect(numbers.ok() && numbers.value == "5.4.0",
                "well-formed numbers elsewhere are walked past: " + numbers.error.Describe());
  checks.Expect(later.ok() && later.value == "5.4.0-beta.1",
                "SYSTEM after a member whose strings hold brackets and escaped quotes: " +
                    later.error.Describe());

  // Missing fields are tolerated, and are not errors.
  for (const char* missing : {R"({})", R"({"OIDC":{"ENABLED":false}})", R"({"SYSTEM":{}})",
                              R"({"SYSTEM":{"VERSION":null}})"}) {
    const auth::Parsed<std::string> read = server::ParseHeartbeatVersion(missing);
    checks.Expect(read.ok() && read.value.empty(),
                  std::string("no version is an empty answer, not an error: ") + missing);
  }

  // What is not a heartbeat is a named error rather than a guess.
  for (const char* broken :
       {"", "[]", "\"5.4.0\"", "<html>nginx</html>", R"({"SYSTEM":{"VERSION":"5.4.0"})",
        R"({"SYSTEM":{"VERSION":"5.4.0"}} trailing)", R"({"SYSTEM":"5.4.0"})",
        R"({"SYSTEM":{"VERSION":5.4}})", R"({"SYSTEM":{"VERSION":"5.4.0",}})",
        R"({"OIDC":{"ENABLED":fals},"SYSTEM":{"VERSION":"5.4.0"}})",
        R"({"OIDC":"unterminated,"SYSTEM":{"VERSION":"5.4.0"}})", R"({"SYSTEM" {}})",
        // Numbers are held to their grammar, not to their characters.
        R"({"N":1-2,"SYSTEM":{"VERSION":"5.4.0"}})", R"({"N":-,"SYSTEM":{"VERSION":"5.4.0"}})",
        R"({"N":[e],"SYSTEM":{"VERSION":"5.4.0"}})", R"({"N":01,"SYSTEM":{"VERSION":"5.4.0"}})",
        R"({"N":1.,"SYSTEM":{"VERSION":"5.4.0"}})",
        // A second SYSTEM is refused, as json::Parse refuses a duplicate key:
        // which one is meant is not this client's to guess.
        R"({"SYSTEM":{"VERSION":"5.2.0"},"SYSTEM":{"VERSION":"5.4.0"}})"}) {
    const auth::Parsed<std::string> read = server::ParseHeartbeatVersion(broken);
    checks.Expect(!read.ok(), std::string("refused: ") + broken);
  }

  // The bound: a SYSTEM member past kMaxSystemBytes is not parsed at all.
  std::string huge = R"({"SYSTEM":{"VERSION":"5.4.0","PAD":")";
  huge.append(server::kMaxSystemBytes, 'x');
  huge += "\"}}";
  checks.Expect(!server::ParseHeartbeatVersion(huge).ok(),
                "a SYSTEM member larger than kMaxSystemBytes is refused, not parsed");

  // ...and the rest of the body is not what the bound is on: a heartbeat with a
  // large library's worth of platform folders still reads.
  std::string wide = R"({"FILESYSTEM":{"FS_PLATFORMS":[)";
  for (int i = 0; i < 4000; ++i) {
    wide += (i == 0 ? "\"" : ",\"") + std::string("platform-") + std::to_string(i) + "\"";
  }
  wide += R"(]},"SYSTEM":{"VERSION":"5.4.0"}})";
  const auth::Parsed<std::string> big = server::ParseHeartbeatVersion(wide);
  checks.Expect(big.ok() && big.value == "5.4.0",
                "a " + std::to_string(wide.size()) + "-byte heartbeat still reads");
}

/// #245: `DEVICE_INSTALL.ENABLED`, the switch that makes RomM's web UI offer
/// "Install on device". Read the way `SYSTEM` is, and walked rather than
/// parsed, so the excluded-platform list beside it costs nothing however long
/// an admin makes it.
void HeartbeatInstall(rig::Checks& checks) {
  const auth::Parsed<bool> old = server::ParseHeartbeatInstall(kHeartbeat520);
  checks.Expect(old.ok() && !old.value,
                "a 5.2.0 heartbeat has no DEVICE_INSTALL: off, not an error");

  // The member as the 5.4.0 fixture serves it, verified live (#228's note on #245).
  const auth::Parsed<bool> current = server::ParseHeartbeatInstall(
      R"({"SYSTEM":{"VERSION":"5.4.0"},"DEVICE_INSTALL":{"ENABLED": true,)"
      R"("EXCLUDED_PLATFORM_SLUGS":["win","win3x","win9x","windows-apps"]},"TASKS":{}})");
  checks.Expect(current.ok() && current.value,
                "the 5.4.0 shape reads as enabled: " + current.error.Describe());
  const auth::Parsed<bool> off = server::ParseHeartbeatInstall(
      R"({"DEVICE_INSTALL":{"EXCLUDED_PLATFORM_SLUGS":["a\"}"],"ENABLED":false}})");
  checks.Expect(off.ok() && !off.value,
                "ENABLED false, after a list whose string holds a quote and a brace");
  for (const char* absent :
       {R"({"DEVICE_INSTALL":{}})", R"({"DEVICE_INSTALL":{"ENABLED":null}})"}) {
    const auth::Parsed<bool> read = server::ParseHeartbeatInstall(absent);
    checks.Expect(read.ok() && !read.value, std::string("no ENABLED is off: ") + absent);
  }

  // No bound refuses a long list: the walk keeps nothing of it.
  std::string wide = R"({"DEVICE_INSTALL":{"EXCLUDED_PLATFORM_SLUGS":[)";
  for (int i = 0; i < 8000; ++i) {
    wide += (i == 0 ? "\"" : ",\"") + std::string("platform-") + std::to_string(i) + "\"";
  }
  wide += R"(],"ENABLED":true},"SYSTEM":{"VERSION":"5.4.0"}})";
  const auth::Parsed<bool> big = server::ParseHeartbeatInstall(wide);
  checks.Expect(big.ok() && big.value, "a " + std::to_string(wide.size()) +
                                           "-byte DEVICE_INSTALL still reads as enabled");

  for (const char* broken :
       {"", "[]", R"({"DEVICE_INSTALL":true})", R"({"DEVICE_INSTALL":{"ENABLED":"yes"}})",
        R"({"DEVICE_INSTALL":{"ENABLED":1}})", R"({"DEVICE_INSTALL":{"ENABLED":tru}})",
        R"({"DEVICE_INSTALL":{"ENABLED":true,"ENABLED":false}})",
        R"({"DEVICE_INSTALL":{"ENABLED":true},"DEVICE_INSTALL":{"ENABLED":true}})",
        R"({"DEVICE_INSTALL":{"ENABLED":true})"}) {
    checks.Expect(!server::ParseHeartbeatInstall(broken).ok(), std::string("refused: ") + broken);
  }

  // The switch is configuration, not a release feature, so a version alone
  // never claims it.
  checks.Expect(!server::CapabilitiesFor(server::kMinimumVersion).install_enabled,
                "CapabilitiesFor(version) leaves install_enabled off");
}

server::Version FixtureVersion(http::HttpClient& client, const std::string& base) {
  // An independent read of the same fact: the whole body through the strict
  // parser, which is what the shallow read must agree with.
  http::Request request;
  request.url = base + "/api/heartbeat";
  const http::Result sent = client.Send(request);
  const json::ParseResult parsed = json::Parse(sent.response.body);
  const json::Value* system = parsed.value.Find("SYSTEM");
  const json::Value* version = system == nullptr ? nullptr : system->Find("VERSION");
  return server::ParseVersion(version == nullptr ? "" : version->string())
      .value_or(server::Version{});
}

void Live(rig::Checks& checks, http::HttpClient& client, const std::string& base) {
  const server::Version fixture = FixtureVersion(client, base);
  const server::Probe probe = server::ReadServer(client, base);
  checks.Expect(probe.ok(), "the fixture answers its heartbeat: " + probe.message);
  checks.ExpectEq(probe.attempts, 1, "in one request");
  checks.Expect(probe.capabilities.known, "with a version this client reads");
  checks.Expect(server::ParseVersion(probe.capabilities.version) == fixture,
                "the shallow read agrees with the whole-document parse: " +
                    probe.capabilities.version);
  if (fixture >= server::kMinimumVersion) {
    checks.Expect(probe.capabilities.supported && AllFlags(probe.capabilities),
                  "the " + server::ToString(fixture) + " fixture yields the full capability set");
  } else {
    checks.Expect(!probe.capabilities.supported,
                  "the " + server::ToString(fixture) + " fixture is below the floor and refused");
  }
  if (fixture >= server::kRelease54) {
    // The fixture runs with RomM's default `DEVICE_INSTALL_ENABLED`, which is on.
    checks.Expect(probe.capabilities.install_enabled,
                  "and its heartbeat says Install on device is enabled (#245)");
  }
  std::cout << "  fixture RomM " << probe.capabilities.version
            << (probe.capabilities.supported ? " (supported)" : " (refused)") << "\n";
}

void Faults(rig::Checks& checks, http::HttpClient& client, const std::string& base) {
  std::vector<std::chrono::milliseconds> waits;
  sync::CallPolicy policy;
  policy.wait = [&waits](std::chrono::milliseconds delay) { waits.push_back(delay); };
  // The jitter rolled at its top, so the one delay is a number: the default
  // second and a quarter of it on top (#251).
  policy.random = [] { return 1.0; };

  rig::ArmFault(client, base, R"({"mode":"status","status":503,"path":"/api/heartbeat"})");
  const server::Probe retried = server::ReadServer(client, base, policy);
  checks.Expect(retried.ok(), "a 503 is retried and the second answer is used: " +
                                  retried.message);
  checks.ExpectEq(retried.attempts, 2, "two requests");
  checks.ExpectEq(waits.size(), std::size_t{1}, "with one backoff between them");
  checks.ExpectEq(waits.empty() ? std::int64_t{0} : waits[0].count(), std::int64_t{1'250},
                  "of the curve every retry loop shares, with its jitter on top");

  waits.clear();
  rig::ArmFault(client, base,
                R"({"mode":"status","status":404,"path":"/api/heartbeat","count":5})");
  const server::Probe missing = server::ReadServer(client, base, policy);
  rig::DisarmFault(client, base);
  checks.Expect(!missing.ok() && missing.status == 404, "a 404 is reported as a 404");
  checks.Expect(!missing.transient(), "and is not transient: it is not a RomM heartbeat");
  checks.ExpectEq(missing.attempts, 1, "so it is not retried");

  waits.clear();
  rig::ArmFault(client, base,
                R"({"mode":"drop","bytes":10,"path":"/api/heartbeat","count":5})");
  const server::Probe dropped = server::ReadServer(client, base, policy);
  rig::DisarmFault(client, base);
  checks.Expect(dropped.transport != http::Error::kNone, "a reset is a transport failure");
  checks.Expect(dropped.transient(), "and transient");
  checks.ExpectEq(dropped.attempts, policy.max_attempts, "retried to the budget");
  checks.ExpectEq(waits.size(), static_cast<std::size_t>(policy.max_attempts - 1),
                  "backing off between each");

  http::CancelToken cancel;
  cancel.Cancel();
  sync::CallPolicy canceled = policy;
  canceled.cancel = &cancel;
  const server::Probe stopped = server::ReadServer(client, base, canceled);
  checks.Expect(stopped.transport == http::Error::kCanceled && stopped.attempts == 0,
                "a fired token costs no request");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "versions";
  rig::Checks checks;
  if (scenario == "versions" || scenario == "heartbeat") {
    if (scenario == "versions") {
      Versions(checks);
    } else {
      Heartbeat(checks);
      HeartbeatInstall(checks);
    }
  } else {
    const std::string base = rig::BaseUrl();
    const std::unique_ptr<http::HttpClient> client = rig::MakeClient();
    if (!rig::Reachable(*client, base)) {
      std::cerr << "rig unreachable at " << base
                << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
      return rig::kSkip;
    }
    rig::DisarmFault(*client, base);
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
    std::cout << "server." << scenario << " ok\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
