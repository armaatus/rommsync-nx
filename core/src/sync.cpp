#include "rommsync/sync.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>

namespace rommsync::sync {
namespace {

json::Error Fail(std::string_view field, std::string message) {
  json::Error error;
  error.field = std::string(field);
  error.message = std::move(message);
  return error;
}

/// True for a string that survives every hop between here and RomM's database.
///
/// A NUL is refused for the reason `json::Reader::Required` refuses one: it is
/// legal JSON and it truncates every C API downstream, so the value that gets
/// used would not be the value that was checked. The other control characters
/// go with it -- `json::Quote` would escape them faithfully, but a save file
/// whose name contains a newline is a name that got built wrong, not one worth
/// carrying to a server that will store it as a path component.
bool Printable(std::string_view value) {
  for (const char character : value) {
    if (static_cast<unsigned char>(character) < 0x20 || character == 0x7F) {
      return false;
    }
  }
  return true;
}

bool LowercaseHex(std::string_view value) {
  for (const char character : value) {
    const bool digit = character >= '0' && character <= '9';
    const bool letter = character >= 'a' && character <= 'f';
    if (!digit && !letter) {
      return false;
    }
  }
  return true;
}

/// A `T | null` field: absent is fine, present and unusable is not.
json::Error ValidateNullable(const std::optional<std::string>& value, std::string_view field) {
  if (!value.has_value()) {
    return {};
  }
  if (value->empty()) {
    return Fail(field, "present but empty; send null rather than a blank value");
  }
  if (!Printable(*value)) {
    return Fail(field, "contains a control character");
  }
  return {};
}

void AppendMember(std::string& out, std::string_view key, const std::optional<std::string>& value) {
  out += ",\"";
  out += key;
  out += "\":";
  out += value.has_value() ? json::Quote(*value) : "null";
}

}  // namespace

bool IsSingleFileName(std::string_view value) {
  // `saves-post.json` shows where these land:
  // `users/<user>/saves/<platform>/<rom>/<emulator>/<file_name>`. Both the
  // emulator and the file name are pasted into that path, so a `/` in either is
  // a client asking the server to write somewhere else, and `.` or `..` is the
  // same request without a separator in it. A directory scan is the intended
  // producer of these values (SYNC_PROTOCOL.md step 0) and `readdir` hands out
  // `.` and `..` for free, so this is a mistake to make by accident, not only in
  // anger.
  return value.find('/') == std::string_view::npos && value != "." && value != "..";
}

std::int64_t UnixSeconds(Timestamp when) {
  return std::chrono::floor<std::chrono::seconds>(when.time_since_epoch()).count();
}

std::string FormatTimestamp(Timestamp when) {
  const std::int64_t seconds = UnixSeconds(when);
  if (seconds < kMinTimestampSeconds || seconds > kMaxTimestampSeconds) {
    return {};
  }

  // Howard Hinnant's civil_from_days, which is exact for every day in range and
  // needs neither a time zone database nor `gmtime_r` -- the C library's
  // conversion is not available in the same shape on Horizon and on the host,
  // and its thread-safe spelling differs again.
  std::int64_t days = seconds / 86400;
  std::int64_t rest = seconds % 86400;
  if (rest < 0) {  // unreachable while the floor is 1, and wrong to leave to luck
    rest += 86400;
    --days;
  }

  days += 719468;  // shift the epoch to 0000-03-01, where the leap cycle starts
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::int64_t day_of_era = days - era * 146097;                             // [0, 146096]
  const std::int64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;  // [0, 399]
  const std::int64_t day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);  // [0, 365]
  const std::int64_t march_month = (5 * day_of_year + 2) / 153;               // [0, 11], 0 = March
  const std::int64_t day = day_of_year - (153 * march_month + 2) / 5 + 1;
  const std::int64_t month = march_month + (march_month < 10 ? 3 : -9);
  const std::int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);

  char text[32];
  const int written = std::snprintf(
      text, sizeof(text), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
      static_cast<long long>(year), static_cast<long long>(month), static_cast<long long>(day),
      static_cast<long long>(rest / 3600), static_cast<long long>((rest / 60) % 60),
      static_cast<long long>(rest % 60));
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(text)) {
    return {};
  }
  return std::string(text, static_cast<std::size_t>(written));
}

json::Error Validate(const ClientSaveState& save) {
  if (save.rom_id <= 0) {
    return Fail("rom_id", "must be a positive RomM rom id; this save matched no rom");
  }

  if (save.file_name.empty()) {
    return Fail("file_name", "missing");
  }
  if (!Printable(save.file_name)) {
    return Fail("file_name", "contains a control character");
  }
  if (!IsSingleFileName(save.file_name)) {
    return Fail("file_name", "is a path, not a file name; the server joins it into one");
  }

  if (const json::Error error = ValidateNullable(save.slot, "slot"); !error.ok()) {
    return error;
  }
  if (const json::Error error = ValidateNullable(save.emulator, "emulator"); !error.ok()) {
    return error;
  }
  // The emulator is a directory in the save's stored path, not just a label.
  if (save.emulator.has_value() && !IsSingleFileName(*save.emulator)) {
    return Fail("emulator", "is a path segment on the server; it may not name a directory");
  }
  if (const json::Error error = ValidateNullable(save.content_hash, "content_hash"); !error.ok()) {
    return error;
  }

  if (save.content_hash.has_value()) {
    const std::string& hash = *save.content_hash;
    if (hash.size() != kContentHashDigits) {
      // The 40-character case is called out by name because it is the one a
      // reader will otherwise spend an afternoon on: the rom schema's hash is a
      // SHA1, saves are compared on MD5, and the server stores whatever it is
      // sent -- so a SHA1 here is accepted and then matches nothing, on every
      // tick, for the life of the save.
      return Fail("content_hash", hash.size() == 40
                                      ? "is 40 digits, which is a SHA1; saves are compared on MD5"
                                      : "must be " + std::to_string(kContentHashDigits) +
                                            " hex digits (MD5), got " +
                                            std::to_string(hash.size()));
    }
    if (!LowercaseHex(hash)) {
      return Fail("content_hash", "must be lowercase hex; the server compares the digest it "
                                  "stored, and stores it lowercase");
    }
  }

  const std::int64_t updated_at = UnixSeconds(save.updated_at);
  if (updated_at < kMinTimestampSeconds) {
    return Fail("updated_at", "is at or before the Unix epoch, which is an unset clock rather "
                              "than an mtime");
  }
  if (updated_at > kMaxTimestampSeconds) {
    return Fail("updated_at", "is past 9999-12-31, which no timestamp RomM can read");
  }

  if (save.file_size_bytes < 0) {
    return Fail("file_size_bytes", "is negative");
  }
  return {};
}

Encoded EncodeNegotiateRequest(const SyncNegotiatePayload& payload) {
  Encoded encoded;
  // A blank device_id is the one that has to be caught here rather than by the
  // server: it is what a token store that lost its device leaves behind, and
  // RomM would read the whole negotiation as belonging to no device -- every
  // save with no sync history, every plan a first encounter.
  if (const json::Error error = ValidateNullable(payload.device_id, "device_id"); !error.ok()) {
    encoded.error = error;
    return encoded;
  }

  std::string body("{\"device_id\":");
  body += payload.device_id.has_value() ? json::Quote(*payload.device_id) : "null";
  body += ",\"saves\":[";

  for (std::size_t index = 0; index < payload.saves.size(); ++index) {
    const ClientSaveState& save = payload.saves[index];
    if (const json::Error error = Validate(save); !error.ok()) {
      encoded.error = error;
      encoded.error.field = "saves[" + std::to_string(index) + "]." + error.field;
      return encoded;
    }

    if (index != 0) {
      body += ',';
    }
    // Field order follows the snapshot, which is also the order
    // server/probe_contract.py sent to a live 5.2.0. Nothing depends on it --
    // it is what makes a captured body and an encoded one diffable by eye.
    body += "{\"rom_id\":";
    body += std::to_string(save.rom_id);
    body += ",\"file_name\":";
    body += json::Quote(save.file_name);
    AppendMember(body, "slot", save.slot);
    AppendMember(body, "emulator", save.emulator);
    AppendMember(body, "content_hash", save.content_hash);
    body += ",\"updated_at\":";
    body += json::Quote(FormatTimestamp(save.updated_at));
    body += ",\"file_size_bytes\":";
    body += std::to_string(save.file_size_bytes);
    body += '}';
  }

  body += "]}";
  encoded.body = std::move(body);
  return encoded;
}

}  // namespace rommsync::sync
