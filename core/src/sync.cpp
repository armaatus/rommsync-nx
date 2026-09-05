#include "rommsync/sync.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace rommsync::sync {
namespace {

/// `count` decimal digits starting at `at`, or empty if they are not all
/// digits. **Consumes them**: `at` is advanced past them on success and left
/// where it was otherwise, so a caller reads the fields in order.
///
/// Written out rather than left to `sscanf`, which accepts leading spaces and a
/// sign and would read `2026-9-4T1:2:3` as a date -- a shape RomM never sends,
/// and one whose acceptance would let a body that is not a timestamp become an
/// instant a save is arbitrated on.
std::optional<int> TakeDigits(std::string_view text, std::size_t& at, std::size_t count) {
  if (at + count > text.size()) {
    return std::nullopt;
  }
  int value = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char character = text[at + index];
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    value = value * 10 + (character - '0');
  }
  at += count;
  return value;
}

/// Howard Hinnant's days_from_civil, the exact inverse of the civil_from_days
/// spelled out in `FormatTimestamp`. Both are here rather than in the C library
/// for the same reason: its UTC conversion is not available in the same shape on
/// Horizon and on the host, and its thread-safe spelling differs again.
std::int64_t DaysFromCivil(std::int64_t year, std::int64_t month, std::int64_t day) {
  const std::int64_t shifted = year - (month <= 2 ? 1 : 0);
  const std::int64_t era = (shifted >= 0 ? shifted : shifted - 399) / 400;
  const std::int64_t year_of_era = shifted - era * 400;                             // [0, 399]
  const std::int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const std::int64_t day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

/// The days in `month` of `year`, so 2026-02-30 is refused rather than rolled
/// forward into March. A date that rolls is a save dated a day it was not.
int DaysInMonth(int year, int month) {
  constexpr int kLengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
    return 29;
  }
  return kLengths[month - 1];
}

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

std::string_view ExtensionOf(std::string_view file_name) {
  const std::size_t dot = file_name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) {
    return {};
  }
  return file_name.substr(dot);
}

bool IsContentHash(std::string_view value) {
  return value.size() == kContentHashDigits && LowercaseHex(value);
}

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

std::optional<Timestamp> ParseTimestamp(std::string_view text) {
  std::size_t at = 0;
  const std::optional<int> year = TakeDigits(text, at, 4);
  if (!year.has_value() || at >= text.size() || text[at++] != '-') {
    return std::nullopt;
  }
  const std::optional<int> month = TakeDigits(text, at, 2);
  if (!month.has_value() || at >= text.size() || text[at++] != '-') {
    return std::nullopt;
  }
  const std::optional<int> day = TakeDigits(text, at, 2);
  if (!day.has_value() || at >= text.size() || (text[at] != 'T' && text[at] != ' ')) {
    return std::nullopt;
  }
  ++at;
  const std::optional<int> hour = TakeDigits(text, at, 2);
  if (!hour.has_value() || at >= text.size() || text[at++] != ':') {
    return std::nullopt;
  }
  const std::optional<int> minute = TakeDigits(text, at, 2);
  if (!minute.has_value() || at >= text.size() || text[at++] != ':') {
    return std::nullopt;
  }
  const std::optional<int> second = TakeDigits(text, at, 2);
  if (!second.has_value()) {
    return std::nullopt;
  }

  // The fraction is dropped rather than rounded, which is `FormatTimestamp`'s
  // rule read the other way: a copy stamped :27.9 is not newer than one stamped
  // :27, and rounding it up would hand it an arbitration it should have lost.
  if (at < text.size() && text[at] == '.') {
    ++at;
    const std::size_t digits = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      ++at;
    }
    if (at == digits) {
      return std::nullopt;  // a dot with no fraction after it
    }
  }

  // `Z`, `+00:00`, or nothing at all. RomM sends the second, this client writes
  // the first, and a naive datetime -- which pydantic can emit -- is UTC,
  // because that is what RomM stores.
  std::int64_t offset_seconds = 0;
  if (at < text.size() && (text[at] == 'Z' || text[at] == 'z')) {
    ++at;
  } else if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    const int sign = text[at] == '-' ? -1 : 1;
    ++at;
    const std::optional<int> offset_hours = TakeDigits(text, at, 2);
    if (!offset_hours.has_value() || at >= text.size() || text[at++] != ':') {
      return std::nullopt;
    }
    const std::optional<int> offset_minutes = TakeDigits(text, at, 2);
    if (!offset_minutes.has_value() || *offset_hours > 23 || *offset_minutes > 59) {
      return std::nullopt;
    }
    offset_seconds = sign * (*offset_hours * 3600 + *offset_minutes * 60);
  }
  if (at != text.size()) {
    return std::nullopt;  // trailing anything is a shape this is not reading
  }

  // A leap second is refused rather than rolled into the next minute, on the same
  // rule `DaysInMonth` applies to 2026-02-30: a value that rolls is an instant
  // the string did not name. RomM stores datetimes and never writes one.
  if (*month < 1 || *month > 12 || *day < 1 || *day > DaysInMonth(*year, *month) || *hour > 23 ||
      *minute > 59 || *second > 59) {
    return std::nullopt;
  }

  const std::int64_t seconds =
      DaysFromCivil(*year, *month, *day) * 86400 + *hour * 3600 + *minute * 60 + *second -
      offset_seconds;
  // The same window `Validate` holds a save's own mtime to. An instant outside
  // it is one no baseline row may store and no save may claim, so returning it
  // would only move the refusal somewhere with less to say about which save it
  // was.
  if (seconds < kMinTimestampSeconds || seconds > kMaxTimestampSeconds) {
    return std::nullopt;
  }
  return Timestamp{} + std::chrono::seconds{seconds};
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
