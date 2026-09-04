#include "rommsync/device_registration.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/http.hpp"
#include "rommsync/json.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::auth {
namespace {

constexpr const char* kDevicesPath = "/api/devices";

std::string ApiUrl(std::string_view server_url, std::string_view path) {
  while (!server_url.empty() && server_url.back() == '/') {
    server_url.remove_suffix(1);
  }
  return std::string(server_url) + std::string(path);
}

/// The RFC 3986 unreserved set.
///
/// A device id is a value read back off the SD card and then pasted into a URL
/// *path*, which is the one place a stray character stops being data: a `?` or a
/// `#` truncates the path, a `/` calls a different endpoint, and a `%` invites a
/// second round of decoding. RomM sends a UUID, which is entirely inside this
/// set, so nothing legitimate is refused -- this is a check on what a corrupted
/// or hand-edited `token.dat` can make this client request, not on the id's
/// format, which is the server's to change.
bool UrlPathSafe(std::string_view value) {
  for (const char byte : value) {
    const bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                            byte == '_' || byte == '~';
    if (!unreserved) {
      return false;
    }
  }
  return !value.empty();
}

http::Request AuthedGet(const std::string& url, const std::string& access_token,
                        std::chrono::milliseconds timeout) {
  http::Request request;
  request.method = http::Method::kGet;
  request.url = url;
  request.headers.push_back({"Accept", "application/json"});
  request.headers.push_back({"Authorization", "Bearer " + access_token});
  request.timeout = timeout;
  return request;
}

Registration Fail(RegistrationError error, std::string message) {
  Registration failed;
  failed.error = error;
  failed.message = std::move(message);
  return failed;
}

/// Everything that can go wrong between sending a request and holding a 2xx
/// body, classified once so the two calls below cannot disagree about what a
/// 401 or a 503 means.
///
/// Empty when the exchange produced a body worth parsing.
std::optional<Registration> Refused(const http::Result& result, std::string_view what) {
  if (!result.ok()) {
    return Fail(RegistrationError::kUnreachable,
                std::string(what) + " did not complete: " + http::ToString(result.error));
  }
  const int status = result.response.status;
  if (status == 401 || status == 403) {
    // `expires_at` is null on 5.2.0, so a token does not go stale on its own:
    // a 401 is a token that was revoked, and retrying it is retrying forever
    // (docs/AUTH.md#re-pairing--revocation).
    return Fail(RegistrationError::kUnauthorized,
                std::string(what) + " was rejected: HTTP " + std::to_string(status) +
                    "; the token has been revoked");
  }
  if (status >= 500) {
    return Fail(RegistrationError::kServerError,
                std::string(what) + ": HTTP " + std::to_string(status));
  }
  if (!result.successful()) {
    return Fail(RegistrationError::kMalformed,
                std::string(what) + " was refused: HTTP " + std::to_string(status));
  }
  return std::nullopt;
}

/// `string | null`, flattened: `null` and absent-from-our-struct are the same
/// thing to every caller, and an empty string is refused by the reader anyway.
std::string Flatten(const std::optional<std::string>& value) { return value.value_or(std::string()); }

Parsed<DeviceRecord> ReadDevice(const json::Value& value) {
  Parsed<DeviceRecord> parsed;
  DeviceRecord device;
  std::optional<std::string> name;
  std::optional<std::string> platform;
  std::optional<std::string> client;
  std::optional<std::string> identifier;

  json::Reader reader(value, "device record");
  reader.Required("id", &device.id);
  reader.RequiredNullable("name", &name);
  reader.RequiredNullable("platform", &platform);
  reader.RequiredNullable("client", &client);
  reader.RequiredNullable("client_device_identifier", &identifier);
  reader.Required("sync_enabled", &device.sync_enabled);
  if (!reader.ok()) {
    parsed.error = reader.error();
    return parsed;
  }

  device.name = Flatten(name);
  device.platform = Flatten(platform);
  device.client = Flatten(client);
  device.client_device_identifier = Flatten(identifier);
  parsed.value = std::move(device);
  return parsed;
}

}  // namespace

Parsed<DeviceRecord> ParseDeviceRecord(std::string_view body) {
  Parsed<DeviceRecord> parsed;
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    parsed.error = document.error;
    return parsed;
  }
  return ReadDevice(document.value);
}

Parsed<std::vector<DeviceRecord>> ParseDeviceList(std::string_view body) {
  Parsed<std::vector<DeviceRecord>> parsed;
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    parsed.error = document.error;
    return parsed;
  }
  if (!document.value.is_array()) {
    parsed.error.field = "device list";
    parsed.error.message =
        std::string("expected an array, got ") + json::ToString(document.value.type());
    return parsed;
  }

  std::vector<DeviceRecord> devices;
  devices.reserve(document.value.elements().size());
  for (const json::Value& element : document.value.elements()) {
    // Every row, not just the ones this console might be. A row that will not
    // parse is `DeviceSchema` having changed, and skipping it quietly would
    // hide exactly the drift this parse exists to catch -- while leaving the
    // search below to report "no such device" for a console that is right there.
    Parsed<DeviceRecord> device = ReadDevice(element);
    if (!device.ok()) {
      parsed.error = device.error;
      return parsed;
    }
    devices.push_back(std::move(device.value));
  }
  parsed.value = std::move(devices);
  return parsed;
}

const DeviceRecord* FindByIdentifier(const std::vector<DeviceRecord>& devices,
                                     std::string_view identifier) {
  if (identifier.empty()) {
    // Every device RomM did not pair carries `null` here, so an empty needle
    // would match the first browser session in the list.
    return nullptr;
  }
  const DeviceRecord* found = nullptr;
  for (const DeviceRecord& device : devices) {
    if (device.client_device_identifier != identifier) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &device;
  }
  return found;
}

const char* ToString(RegistrationState state) {
  switch (state) {
    case RegistrationState::kUnpaired:
      return "unpaired";
    case RegistrationState::kUnregistered:
      return "unregistered";
    case RegistrationState::kRegistered:
      return "registered";
  }
  return "unpaired";
}

RegistrationState StateOf(const StoredToken& token) {
  if (token.access_token.empty() || token.server_url.empty()) {
    return RegistrationState::kUnpaired;
  }
  return token.device_id.empty() ? RegistrationState::kUnregistered
                                 : RegistrationState::kRegistered;
}

const char* ToString(RegistrationError error) {
  switch (error) {
    case RegistrationError::kNone:
      return "none";
    case RegistrationError::kNotRegistered:
      return "not_registered";
    case RegistrationError::kUnauthorized:
      return "unauthorized";
    case RegistrationError::kNoSuchDevice:
      return "no_such_device";
    case RegistrationError::kAmbiguous:
      return "ambiguous";
    case RegistrationError::kSyncDisabled:
      return "sync_disabled";
    case RegistrationError::kUnreachable:
      return "unreachable";
    case RegistrationError::kServerError:
      return "server_error";
    case RegistrationError::kMalformed:
      return "malformed";
  }
  return "none";
}

bool ShouldRetry(RegistrationError error) {
  return error == RegistrationError::kUnreachable || error == RegistrationError::kServerError;
}

bool NeedsPairing(RegistrationError error) {
  return error == RegistrationError::kNotRegistered ||
         error == RegistrationError::kUnauthorized || error == RegistrationError::kNoSuchDevice;
}

namespace {

/// The last gate both calls share: a device that is there, and that RomM will
/// actually sync.
Registration Accept(DeviceRecord device) {
  if (!device.sync_enabled) {
    return Fail(RegistrationError::kSyncDisabled,
                "sync is turned off for this device in RomM; turn it back on there");
  }
  Registration registration;
  registration.device = std::move(device);
  return registration;
}

}  // namespace

Registration ConfirmRegistration(http::HttpClient& client, const StoredToken& token,
                                 std::chrono::milliseconds timeout) {
  if (StateOf(token) == RegistrationState::kUnpaired) {
    return Fail(RegistrationError::kNotRegistered, "this console is not paired");
  }
  if (token.device_id.empty()) {
    return Fail(RegistrationError::kNotRegistered,
                "the stored token names no device; this console is paired but not registered");
  }
  if (!UrlPathSafe(token.device_id)) {
    return Fail(RegistrationError::kMalformed,
                "the stored device id is not a value that can be put in a URL");
  }

  const http::Result result =
      client.Send(AuthedGet(ApiUrl(token.server_url, std::string(kDevicesPath) + "/" +
                                                         token.device_id),
                            token.access_token, timeout));
  // Handled here rather than in `Refused`, because a 404 means two different
  // things at the two paths this module calls: on one device it is the device
  // being gone, which is a state to report; on the list it would be the endpoint
  // being gone, which is not a statement about any device at all.
  if (result.ok() && result.response.status == 404) {
    return Fail(RegistrationError::kNoSuchDevice,
                "the device lookup: the server has no such device (HTTP 404)");
  }
  if (const std::optional<Registration> refused = Refused(result, "the device lookup")) {
    return *refused;
  }

  Parsed<DeviceRecord> parsed = ParseDeviceRecord(result.response.body);
  if (!parsed.ok()) {
    return Fail(RegistrationError::kMalformed, "device response: " + parsed.error.Describe());
  }
  if (parsed.value.id != token.device_id) {
    // A server that answered about a different device than was asked for is one
    // whose answer says nothing about the cached id. Accepting it would cache
    // whatever came back and scope every sync call by it.
    return Fail(RegistrationError::kMalformed,
                "the server answered about a different device than the one asked about");
  }
  return Accept(std::move(parsed.value));
}

Registration FindRegistration(http::HttpClient& client, const StoredToken& token,
                              std::string_view client_device_identifier,
                              std::chrono::milliseconds timeout) {
  if (StateOf(token) == RegistrationState::kUnpaired) {
    return Fail(RegistrationError::kNotRegistered, "this console is not paired");
  }
  if (client_device_identifier.empty()) {
    return Fail(RegistrationError::kNotRegistered,
                "no client device identifier was derived, so no device can be found");
  }

  const http::Result result = client.Send(
      AuthedGet(ApiUrl(token.server_url, kDevicesPath), token.access_token, timeout));
  if (const std::optional<Registration> refused = Refused(result, "the device list")) {
    return *refused;
  }

  Parsed<std::vector<DeviceRecord>> parsed = ParseDeviceList(result.response.body);
  if (!parsed.ok()) {
    return Fail(RegistrationError::kMalformed, "device list: " + parsed.error.Describe());
  }

  const DeviceRecord* found = FindByIdentifier(parsed.value, client_device_identifier);
  if (found != nullptr) {
    return Accept(*found);
  }

  // `FindByIdentifier` answers nullptr to two very different situations, and
  // they get different errors because they have different remedies: nothing
  // carries the identifier (pair again), or several rows do (a state RomM
  // allows, which only the user can tidy up -- pairing again would just find
  // the same two).
  std::size_t matches = 0;
  for (const DeviceRecord& device : parsed.value) {
    if (device.client_device_identifier == client_device_identifier) {
      ++matches;
    }
  }
  if (matches > 1) {
    return Fail(RegistrationError::kAmbiguous,
                "this server has " + std::to_string(matches) +
                    " devices for this console; remove the extra ones in RomM");
  }
  return Fail(RegistrationError::kNoSuchDevice,
              "no device on this server belongs to this console");
}

Registration ResolveRegistration(http::HttpClient& client, const StoredToken& token,
                                 std::string_view client_device_identifier,
                                 std::chrono::milliseconds timeout) {
  Registration confirmed = ConfirmRegistration(client, token, timeout);
  if (confirmed.ok() || (confirmed.error != RegistrationError::kNotRegistered &&
                         confirmed.error != RegistrationError::kNoSuchDevice)) {
    return confirmed;
  }
  // Only these two. A transient failure falling through would be answered by a
  // listing that says the device is fine, which is the opposite of what just
  // happened; a 401 falling through would spend a second doomed request to move
  // the failure from the device to the device list.
  //
  // What the search returns is what gets reported either way: it is the call
  // that looked at every device rather than at one, so its answer is the better
  // description of the same failure.
  return FindRegistration(client, token, client_device_identifier, timeout);
}

StoreResult CacheDeviceId(const std::string& path, StoredToken& token,
                          const Registration& registration) {
  if (!registration.ok()) {
    return {StoreError::kUnusableToken,
            path + ": refusing to cache a device id that was never confirmed"};
  }
  if (token.device_id == registration.device.id) {
    // Every boot after the first. Rewriting the record would spend an SD write,
    // and a commit window in which `token.dat` is briefly absent, to store the
    // value that is already in it.
    return {};
  }
  token.device_id = registration.device.id;
  return SaveToken(path, token);
}

}  // namespace rommsync::auth
