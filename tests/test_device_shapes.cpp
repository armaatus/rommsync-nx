// `DeviceSchema` against the real captured payload, and the states around it.
//
// The body here is not typed out from the docs: it is read from
// server/contract/captures/devices-get.json, the unedited response a live RomM
// 5.2.0 sent for the device its own pairing flow had just created.
// contract.captures keeps that file honest against a running server; this keeps
// `DeviceRecord` honest against the file.
//
// The rest is the classification the sysmodule acts on -- is this console
// registered, is a failure worth retrying, is it worth sending the user to the
// pairing screen -- which is decidable with no server at all. So this never
// skips.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "checks.hpp"
#include "rommsync/device_registration.hpp"
#include "rommsync/json.hpp"
#include "rommsync/token_store.hpp"

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

/// Every field `DeviceSchema` carries -- the six this client reads and the ten
/// it deliberately does not.
///
/// The unread ones are listed anyway, because the thing worth catching is a
/// *new* field: RomM growing one that matters to sync is exactly the drift that
/// otherwise surfaces on a console, and a check that only looked at the six
/// would never see it. A field that disappears is caught too, which is the same
/// warning from the other side.
const std::vector<std::string> kDeviceSchemaFields = {
    "id",        "user_id",     "name",        "platform",
    "client",    "client_version", "ip_address", "mac_address",
    "hostname",  "client_device_identifier",   "sync_mode", "sync_enabled",
    "sync_config", "last_seen",  "created_at", "updated_at",
};

void SchemaFields(checks::Checks& c, const std::string& body) {
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    c.Expect(false, "the device capture parses: " + document.error.Describe());
    return;
  }
  for (const json::Member& member : document.value.members()) {
    bool known = false;
    for (const std::string& field : kDeviceSchemaFields) {
      known = known || field == member.key;
    }
    c.Expect(known, "DeviceSchema grew a field this client has not looked at: " + member.key);
  }
  for (const std::string& field : kDeviceSchemaFields) {
    c.Expect(document.value.Find(field) != nullptr, "DeviceSchema no longer carries " + field);
  }
}

/// The capture, with one field replaced -- the only way to test a shape RomM
/// will not produce on demand without hand-writing a second copy of the body
/// that then drifts from the real one.
std::string WithField(const std::string& body, const std::string& key,
                      const std::string& replacement) {
  const std::string needle = "\"" + key + "\": ";
  const std::size_t at = body.find(needle);
  if (at == std::string::npos) {
    return body;
  }
  const std::size_t from = at + needle.size();
  std::size_t end = body.find_first_of(",\n", from);
  if (end == std::string::npos) {
    end = body.size();
  }
  return body.substr(0, from) + replacement + body.substr(end);
}

void Capture(checks::Checks& c) {
  const std::string body = ReadCapture(c, "devices-get.json");
  SchemaFields(c, body);

  const auth::Parsed<auth::DeviceRecord> parsed = auth::ParseDeviceRecord(body);
  if (!parsed.ok()) {
    c.Expect(false, "the device capture parses: " + parsed.error.Describe());
    return;
  }
  const auth::DeviceRecord& device = parsed.value;

  // The id is a fixture value from the run that produced the capture, not a
  // contract constant (captures/README.md). Its *shape* is: a UUID, and the one
  // value every sync call is scoped by.
  c.ExpectEq(device.id.size(), std::size_t{36}, "id is a UUID");
  c.Expect(device.id.find('-') != std::string::npos, "...with its dashes");
  c.ExpectEq(device.platform, std::string("switch"), "platform");
  c.ExpectEq(device.client_device_identifier, std::string("probe-contract-script"),
             "the identifier pairing sent comes back on the device");
  c.Expect(device.sync_enabled, "a freshly paired device has sync switched on");
  c.Expect(!device.name.empty(), "the device carries the name pairing gave it");

  // `null` is a value RomM sends for every one of these, and it means "nobody
  // set it" -- not a parse that lost something.
  const auth::Parsed<auth::DeviceRecord> unnamed =
      auth::ParseDeviceRecord(WithField(body, "name", "null"));
  c.Expect(unnamed.ok(), "a device with no name is a device: " + unnamed.error.Describe());
  c.Expect(unnamed.value.name.empty(), "...and its name reads back empty");

  // The one that matters most: a device RomM did not pair carries no
  // identifier, and that has to parse, because the *list* is full of them.
  const auth::Parsed<auth::DeviceRecord> anonymous =
      auth::ParseDeviceRecord(WithField(body, "client_device_identifier", "null"));
  c.Expect(anonymous.ok(),
           "a device with no client_device_identifier parses: " + anonymous.error.Describe());
  c.Expect(anonymous.value.client_device_identifier.empty(), "...as an empty identifier");

  // And the ones that must not parse. A `sync_enabled` this client read as
  // `true` because it was the string "true", or a `1`, would let it start a sync
  // RomM answers `400 "Sync is disabled for this device"`.
  for (const std::string& wrong : {std::string("\"true\""), std::string("1"),
                                   std::string("null")}) {
    const auth::Parsed<auth::DeviceRecord> refused =
        auth::ParseDeviceRecord(WithField(body, "sync_enabled", wrong));
    c.Expect(!refused.ok(), "sync_enabled as " + wrong + " is refused");
    c.ExpectEq(refused.error.field, std::string("sync_enabled"), "...and it is named");
  }
  for (const std::string& wrong : {std::string("null"), std::string("\"\""),
                                   std::string("7")}) {
    const auth::Parsed<auth::DeviceRecord> refused =
        auth::ParseDeviceRecord(WithField(body, "id", wrong));
    c.Expect(!refused.ok(), "an id of " + wrong + " is refused");
    c.ExpectEq(refused.error.field, std::string("id"), "...and it is named");
  }

  c.Expect(!auth::ParseDeviceRecord("").ok(), "an empty body is not a device");
  c.Expect(!auth::ParseDeviceRecord("[]").ok(), "an array is not a device");
  c.Expect(!auth::ParseDeviceRecord(body.substr(0, body.size() / 2)).ok(),
           "a truncated body is not a device");
}

void List(checks::Checks& c) {
  const std::string body = ReadCapture(c, "devices-get.json");

  const auth::Parsed<std::vector<auth::DeviceRecord>> empty = auth::ParseDeviceList("[]");
  c.Expect(empty.ok(), "an empty device list parses: " + empty.error.Describe());
  c.Expect(empty.value.empty(), "...as no devices");

  const std::string other = WithField(WithField(body, "id", "\"11111111-1111-1111-1111-111111111111\""),
                                      "client_device_identifier", "null");
  const auth::Parsed<std::vector<auth::DeviceRecord>> two =
      auth::ParseDeviceList("[" + body + "," + other + "]");
  if (!two.ok()) {
    c.Expect(false, "a two-device list parses: " + two.error.Describe());
    return;
  }
  c.ExpectEq(two.value.size(), std::size_t{2}, "both devices are read");

  const auth::DeviceRecord* found =
      auth::FindByIdentifier(two.value, "probe-contract-script");
  if (found == nullptr) {
    c.Expect(false, "the console's own device is found by its identifier");
    return;
  }
  c.ExpectEq(found->id, two.value[0].id, "...and it is the row that carries it");

  c.Expect(auth::FindByIdentifier(two.value, "nx-0123456789abcdef0123456789abcdef") == nullptr,
           "an identifier no device carries finds nothing");
  // Every device RomM did not pair carries `null` here, so a blank needle that
  // matched would hand this console someone's browser session.
  c.Expect(auth::FindByIdentifier(two.value, "") == nullptr,
           "an empty identifier matches no device, not the first anonymous one");

  // RomM has no uniqueness constraint on the identifier, so two rows carrying
  // one is a state the server can be in -- and picking one would send this
  // console's saves to whichever sorted first.
  const auth::Parsed<std::vector<auth::DeviceRecord>> twice =
      auth::ParseDeviceList("[" + body + "," +
                            WithField(body, "id", "\"22222222-2222-2222-2222-222222222222\"") +
                            "]");
  c.Expect(twice.ok(), "a list with two rows for one console parses: " + twice.error.Describe());
  c.Expect(auth::FindByIdentifier(twice.value, "probe-contract-script") == nullptr,
           "an ambiguous identifier resolves to nothing rather than to a guess");

  c.Expect(!auth::ParseDeviceList(body).ok(), "one device is not a device list");
  const auth::Parsed<std::vector<auth::DeviceRecord>> broken =
      auth::ParseDeviceList("[" + body + "," + WithField(body, "sync_enabled", "\"yes\"") + "]");
  c.Expect(!broken.ok(), "a list holding a row that will not parse is refused whole");
  c.ExpectEq(broken.error.field, std::string("sync_enabled"),
             "...naming the field, not the row's position");
}

auth::StoredToken Paired() {
  auth::StoredToken token;
  token.server_url = "http://romm.lan:8080";
  token.access_token = "rmm_" + std::string(64, 'a');
  token.device_id = "3e175584-2641-44bd-913b-e42d8fc64f85";
  token.scopes = {"me.read"};
  return token;
}

void States(checks::Checks& c) {
  const auth::StoredToken paired = Paired();
  c.ExpectEq(std::string(auth::ToString(auth::StateOf(paired))), std::string("registered"),
             "a token with a device is registered");

  auth::StoredToken half = paired;
  half.device_id.clear();
  c.ExpectEq(std::string(auth::ToString(auth::StateOf(half))), std::string("unregistered"),
             "a token with no device is paired but not registered");

  auth::StoredToken none = paired;
  none.access_token.clear();
  c.ExpectEq(std::string(auth::ToString(auth::StateOf(none))), std::string("unpaired"),
             "no token is unpaired");
  auth::StoredToken serverless = paired;
  serverless.server_url.clear();
  c.ExpectEq(std::string(auth::ToString(auth::StateOf(serverless))), std::string("unpaired"),
             "a token against no server is unpaired: there is nothing to ask");
  c.ExpectEq(std::string(auth::ToString(auth::StateOf(auth::StoredToken{}))),
             std::string("unpaired"), "and so is an empty record");

  // The two questions the overlay's two sentences hang off. Getting them the
  // wrong way round throws a working pairing away over a dropped connection, or
  // retries a revoked token until the user gives up.
  struct Case {
    auth::RegistrationError error;
    const char* name;
    bool retry;
    bool repair;
  };
  const Case kCases[] = {
      {auth::RegistrationError::kNone, "none", false, false},
      {auth::RegistrationError::kNotRegistered, "not_registered", false, true},
      {auth::RegistrationError::kUnauthorized, "unauthorized", false, true},
      {auth::RegistrationError::kNoSuchDevice, "no_such_device", false, true},
      {auth::RegistrationError::kAmbiguous, "ambiguous", false, false},
      {auth::RegistrationError::kSyncDisabled, "sync_disabled", false, false},
      {auth::RegistrationError::kUnreachable, "unreachable", true, false},
      {auth::RegistrationError::kServerError, "server_error", true, false},
      {auth::RegistrationError::kMalformed, "malformed", false, false},
  };
  for (const Case& one : kCases) {
    c.ExpectEq(std::string(auth::ToString(one.error)), std::string(one.name), "the name of an error");
    c.ExpectEq(auth::ShouldRetry(one.error), one.retry, std::string("retrying ") + one.name);
    c.ExpectEq(auth::NeedsPairing(one.error), one.repair,
               std::string("re-pairing over ") + one.name);
    c.Expect(!(auth::ShouldRetry(one.error) && auth::NeedsPairing(one.error)),
             std::string(one.name) + " is not both worth retrying and worth re-pairing over");
  }
}

void Caching(checks::Checks& c) {
  const std::string directory = std::string(ROMMSYNC_TEST_SCRATCH) + "/device-cache";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::filesystem::create_directories(directory, ignored);
  const std::string path = directory + "/token.dat";

  auth::Registration resolved;
  resolved.device.id = "3e175584-2641-44bd-913b-e42d8fc64f85";

  // The recovery case: a record that came back from pairing without a device.
  auth::StoredToken token = Paired();
  token.device_id.clear();
  c.Expect(auth::CacheDeviceId(path, token, resolved).ok(), "a confirmed device is cached");
  c.ExpectEq(token.device_id, resolved.device.id, "...into the record");
  const auth::LoadedToken reloaded = auth::LoadToken(path);
  c.Expect(reloaded.ok(), "and onto the disk: " + reloaded.message);
  c.ExpectEq(reloaded.value.device_id, resolved.device.id, "with the device on it");

  // Every boot after the first. Rewriting would cost an SD write and a commit
  // window for a value that did not change, so the proof is that the file is
  // untouched -- not that the call returned ok.
  std::filesystem::remove(path);
  c.Expect(auth::CacheDeviceId(path, token, resolved).ok(),
           "caching the id already in the record succeeds");
  c.Expect(!std::filesystem::exists(path), "...and writes nothing at all");

  // And the one that must not be cached. A device id that was never confirmed
  // is exactly the "a token with no device id, silently used for sync" this
  // module exists to prevent.
  auth::Registration failed;
  failed.error = auth::RegistrationError::kUnreachable;
  failed.device.id = "99999999-9999-9999-9999-999999999999";
  auth::StoredToken untouched = Paired();
  const auth::StoreResult refused = auth::CacheDeviceId(path, untouched, failed);
  c.Expect(!refused.ok(), "an unconfirmed device is not cached");
  c.ExpectEq(std::string(auth::ToString(refused.error)), std::string("unusable_token"),
             "...and it says why");
  c.ExpectEq(untouched.device_id, Paired().device_id, "the record keeps the id it had");
  c.Expect(!std::filesystem::exists(path), "and nothing was written");
  c.Expect(refused.message.find("99999999") == std::string::npos,
           "the refusal does not quote the id it refused");
}

}  // namespace

int main() {
  checks::Checks c;
  Capture(c);
  List(c);
  States(c);
  Caching(c);
  if (c.failures() == 0) {
    std::cout << "device shapes ok\n";
  }
  return c.failures() == 0 ? 0 : 1;
}
