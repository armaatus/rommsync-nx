#include "rommsync/device_identity.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "rommsync/atomic_file.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sha256.hpp"

namespace rommsync::auth {
namespace {

/// Every source name, so `ToString` and the parser cannot drift apart.
constexpr IdentitySource kAllSources[] = {IdentitySource::kDerived, IdentitySource::kRandom};

/// Hash `material` under `kIdentitySalt` and wear it as an identifier.
///
/// The salt and the material are separated by a NUL, so no pair of inputs can
/// be concatenated into the same byte string: `stable` is not attacker-supplied
/// today, and a derivation whose safety depends on that staying true is one
/// nobody would notice going wrong.
std::string Wear(std::string_view material) {
  std::string input(kIdentitySalt);
  input.push_back('\0');
  input.append(material);
  return std::string(kDeviceIdentifierPrefix) +
         crypto::Sha256Hex(input, kDeviceIdentifierHexChars / 2);
}

}  // namespace

const char* ToString(IdentitySource source) {
  switch (source) {
    case IdentitySource::kDerived:
      return "derived";
    case IdentitySource::kRandom:
      return "random";
    case IdentitySource::kUnknown:
      return "unknown";
  }
  return "unknown";
}

const char* ToString(IdentityError error) {
  switch (error) {
    case IdentityError::kNone:
      return "none";
    case IdentityError::kNoSeed:
      return "no_seed";
    case IdentityError::kMalformed:
      return "malformed";
    case IdentityError::kPersistFailed:
      return "persist_failed";
    case IdentityError::kUnreadable:
      return "unreadable";
  }
  return "none";
}

bool IsDeviceIdentifier(std::string_view text) {
  const std::string_view prefix(kDeviceIdentifierPrefix);
  if (text.size() != kDeviceIdentifierLength || text.substr(0, prefix.size()) != prefix) {
    return false;
  }
  for (const char digit : text.substr(prefix.size())) {
    const bool hex = (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
    if (!hex) {
      return false;
    }
  }
  return true;
}

DerivedIdentity DeriveDeviceIdentity(const IdentitySeed& seed) {
  DerivedIdentity derived;
  if (!seed.stable.empty()) {
    derived.value.client_device_identifier = Wear(seed.stable);
    derived.value.source = IdentitySource::kDerived;
    return derived;
  }
  if (seed.entropy.size() < kMinimumEntropyBytes) {
    derived.error = IdentityError::kNoSeed;
    derived.message = "no stable console value, and " + std::to_string(seed.entropy.size()) +
                      " bytes of entropy is below the " +
                      std::to_string(kMinimumEntropyBytes) + " required to mint one";
    return derived;
  }
  // Hashed rather than used raw for the same reason the serial is: whatever the
  // platform handed over stays on the console, and the shape of the identifier
  // does not depend on where it came from.
  derived.value.client_device_identifier = Wear(seed.entropy);
  derived.value.source = IdentitySource::kRandom;
  return derived;
}

std::string SerializeDeviceIdentity(const DeviceIdentity& identity) {
  std::string out("{\"client_device_identifier\":");
  out += json::Quote(identity.client_device_identifier);
  out += ",\"source\":";
  out += json::Quote(ToString(identity.source));
  out += "}";
  return out;
}

DerivedIdentity ParseDeviceIdentity(std::string_view text) {
  DerivedIdentity parsed;
  json::ParseResult document = json::Parse(text);
  if (!document.ok()) {
    parsed.error = IdentityError::kMalformed;
    parsed.message = document.error.Describe();
    return parsed;
  }

  DeviceIdentity identity;
  json::Reader reader(document.value, "device identity record");
  reader.Required("client_device_identifier", &identity.client_device_identifier);
  if (!reader.ok()) {
    parsed.error = IdentityError::kMalformed;
    parsed.message = reader.error().Describe();
    return parsed;
  }
  if (!IsDeviceIdentifier(identity.client_device_identifier)) {
    // Named without quoting the value. It is not a secret, but a record that
    // failed this check is a record whose contents are unknown, and a log line
    // is the wrong place to find out what was in it.
    parsed.error = IdentityError::kMalformed;
    parsed.message = "field client_device_identifier: is not an identifier this client wrote";
    return parsed;
  }

  // Read last and never fatal: `source` is decorative, and a record from a
  // later build that named a source this one does not know still carries the
  // one thing that has to survive.
  std::string source;
  json::Reader source_reader(document.value, "device identity record");
  identity.source = IdentitySource::kUnknown;
  if (source_reader.Required("source", &source)) {
    for (const IdentitySource one : kAllSources) {
      if (source == ToString(one)) {
        identity.source = one;
        break;
      }
    }
  }

  parsed.value = std::move(identity);
  return parsed;
}

namespace {

DerivedIdentity ReadRecord(const std::string& path, io::ReadError* how) {
  const io::ReadResult read = io::ReadFile(path);
  *how = read.error;
  if (!read.ok()) {
    DerivedIdentity failed;
    failed.error = read.error == io::ReadError::kUnreadable ? IdentityError::kUnreadable
                                                           : IdentityError::kMalformed;
    failed.message = read.message;
    return failed;
  }
  return ParseDeviceIdentity(read.contents);
}

}  // namespace

IdentityResult LoadOrCreateDeviceIdentity(const std::string& path, const IdentitySeed& seed) {
  IdentityResult result;

  io::ReadError how = io::ReadError::kNone;
  DerivedIdentity existing = ReadRecord(path, &how);
  if (!existing.ok() && how != io::ReadError::kUnreadable) {
    // The one moment `path` does not exist is between `WriteAtomically`'s two
    // renames, and the previous record is under the `.old` name then. Minting
    // instead of looking there would hand RomM a second console.
    io::ReadError how_previous = io::ReadError::kNone;
    DerivedIdentity previous = ReadRecord(io::PreviousPathFor(path), &how_previous);
    if (previous.ok()) {
      // Put it back under its real name. Recovering the identifier and leaving
      // it where only the fallback finds it is one more interruption away from
      // losing it. The result is ignored on purpose: the identifier is in hand
      // either way, and a console that can pair should not be stopped from
      // pairing because the card refused a write it did not need.
      io::WriteAtomically(path, SerializeDeviceIdentity(previous.value));
      existing = std::move(previous);
      how = io::ReadError::kNone;
    }
  }

  if (existing.ok()) {
    result.value = std::move(existing.value);
    return result;
  }
  if (how == io::ReadError::kUnreadable) {
    // Deliberately not answered by minting. A card having a bad moment is not
    // evidence that this console has no identifier, and overwriting the one it
    // does have cannot be undone.
    result.error = IdentityError::kUnreadable;
    result.message = existing.message;
    return result;
  }

  DerivedIdentity fresh = DeriveDeviceIdentity(seed);
  if (!fresh.ok()) {
    result.error = fresh.error;
    result.message = fresh.message;
    return result;
  }

  const io::WriteResult written = io::WriteAtomically(path, SerializeDeviceIdentity(fresh.value));
  if (!written.ok()) {
    // Returned with no value on purpose: an identifier that is not on disk is a
    // different one next boot, and pairing with it registers a second device.
    result.error = IdentityError::kPersistFailed;
    result.message = written.message;
    return result;
  }

  result.value = std::move(fresh.value);
  result.created = true;
  return result;
}

bool ForgetDeviceIdentity(const std::string& path) { return io::Shred(path); }

}  // namespace rommsync::auth
