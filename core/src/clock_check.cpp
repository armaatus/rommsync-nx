#include "rommsync/clock_check.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace rommsync::sync {
namespace {

std::int64_t SecondsBetween(Timestamp local, Timestamp reference) {
  return UnixSeconds(local) - UnixSeconds(reference);
}

std::int64_t Magnitude(std::int64_t value) { return value < 0 ? -value : value; }

/// "3 days" / "5 hours" / "40 minutes": the unit a user reads a clock error in.
std::string Span(std::int64_t seconds) {
  const std::int64_t magnitude = Magnitude(seconds);
  const auto unit = [](std::int64_t count, const char* name) {
    return std::to_string(count) + " " + name + (count == 1 ? "" : "s");
  };
  if (magnitude >= 86400) {
    return unit(magnitude / 86400, "day");
  }
  if (magnitude >= 3600) {
    return unit(magnitude / 3600, "hour");
  }
  return unit(magnitude / 60, "minute");
}

/// The `count` digits at `text[at]` as a number, or -1.
int Digits(std::string_view text, std::size_t at, std::size_t count) {
  if (at + count > text.size()) {
    return -1;
  }
  int value = 0;
  for (std::size_t i = at; i < at + count; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return -1;
    }
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

}  // namespace

const char* ToString(ClockVerdict verdict) {
  switch (verdict) {
    case ClockVerdict::kTrusted:
      return "trusted";
    case ClockVerdict::kBeforeFloor:
      return "before_floor";
    case ClockVerdict::kBehind:
      return "behind";
    case ClockVerdict::kAhead:
      return "ahead";
  }
  return "trusted";
}

const char* ToString(ClockReference reference) {
  switch (reference) {
    case ClockReference::kNone:
      return "none";
    case ClockReference::kNetwork:
      return "network clock";
    case ClockReference::kServer:
      return "server";
  }
  return "none";
}

std::string ClockCheck::Describe() const {
  const std::string against = reference == ClockReference::kServer
                                  ? "the server's clock"
                                  : std::string("the ") + ToString(reference);
  switch (verdict) {
    case ClockVerdict::kTrusted:
      return reference == ClockReference::kNone
                 ? "the console clock is past the floor; there was nothing to compare it with"
                 : "the console clock agrees with " + against;
    case ClockVerdict::kBeforeFloor:
      return "the console clock is not set: it reads a date before 2026" +
             (reference == ClockReference::kNone
                  ? std::string()
                  : ", " + Span(skew_seconds) + " behind " + against);
    case ClockVerdict::kBehind:
      return "the console clock is " + Span(skew_seconds) + " behind " + against;
    case ClockVerdict::kAhead:
      return "the console clock is " + Span(skew_seconds) + " ahead of " + against;
  }
  return {};
}

ClockCheck CheckClock(const ClockReading& reading) {
  const std::int64_t limit = std::chrono::seconds{kMaxClockSkew}.count();
  ClockCheck check;
  const bool before_floor = UnixSeconds(reading.local) < kClockFloorSeconds;

  // The network clock first: it is the console's own, kept by nifm, so it
  // disagrees only when the user's clock does. The server's is the one the
  // arbitration compares with, and it decides when the network clock is absent
  // or agrees.
  const std::array<std::pair<ClockReference, const std::optional<Timestamp>*>, 2> references = {{
      {ClockReference::kNetwork, &reading.network},
      {ClockReference::kServer, &reading.server},
  }};
  for (const auto& [which, instant] : references) {
    if (!instant->has_value()) {
      continue;
    }
    const std::int64_t skew = SecondsBetween(reading.local, **instant);
    if (check.reference == ClockReference::kNone) {
      // Kept even when inside the limit, so a trusted check names what it was
      // compared with.
      check.reference = which;
      check.skew_seconds = skew;
    }
    if (Magnitude(skew) > limit) {
      check.reference = which;
      check.skew_seconds = skew;
      check.verdict = skew > 0 ? ClockVerdict::kAhead : ClockVerdict::kBehind;
      break;
    }
  }
  if (before_floor) {
    check.verdict = ClockVerdict::kBeforeFloor;
  }
  return check;
}

std::optional<Timestamp> ParseHttpDate(std::string_view text) {
  // `Sun, 06 Nov 1994 08:49:37 GMT`: fixed width, 29 characters.
  if (text.size() != 29 || text[3] != ',' || text[4] != ' ' || text[7] != ' ' ||
      text[11] != ' ' || text[16] != ' ' || text[19] != ':' || text[22] != ':' ||
      text.substr(25) != " GMT") {
    return std::nullopt;
  }
  static constexpr std::array<std::string_view, 7> kDays = {"Mon", "Tue", "Wed", "Thu",
                                                            "Fri", "Sat", "Sun"};
  bool day_known = false;
  for (const std::string_view day : kDays) {
    day_known = day_known || text.substr(0, 3) == day;
  }
  static constexpr std::array<std::string_view, 12> kMonths = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month = 0;
  for (std::size_t i = 0; i < kMonths.size(); ++i) {
    if (text.substr(8, 3) == kMonths[i]) {
      month = static_cast<int>(i) + 1;
    }
  }
  const int day = Digits(text, 5, 2);
  const int year = Digits(text, 12, 4);
  if (!day_known || month == 0 || day < 0 || year < 0 || Digits(text, 17, 2) < 0 ||
      Digits(text, 20, 2) < 0 || Digits(text, 23, 2) < 0) {
    return std::nullopt;
  }
  // Handed to the RFC 3339 reader rather than converted a second way: it
  // already refuses a day the month does not have and an hour past 23, and two
  // calendars in one client are two places for a leap year to go wrong.
  std::string rfc3339 = std::string(text.substr(12, 4)) + "-" + (month < 10 ? "0" : "") +
                        std::to_string(month) + "-" + std::string(text.substr(5, 2)) + "T" +
                        std::string(text.substr(17, 8)) + "Z";
  return ParseTimestamp(rfc3339);
}

std::optional<Timestamp> ServerDate(const http::Headers& headers) {
  const std::string* date = http::FindHeader(headers, "date");
  return date == nullptr ? std::nullopt : ParseHttpDate(*date);
}

bool DecidedByClock(const SyncOperation& operation) {
  return operation.action == Action::kUpload &&
         operation.reason == Reason::kClientNewerNoHistory;
}

bool TlsClockFault(http::Error transport, std::string_view detail, const ClockCheck& clock) {
  if (transport != http::Error::kTls) {
    return false;
  }
  // OpenSSL's wording, which libcurl passes through ("certificate has expired",
  // "certificate is not yet valid"). Horizon's ssl service reports a result
  // code rather than a sentence, so on the console it is the clock check that
  // answers.
  return !clock.trusted() || detail.find("expired") != std::string_view::npos ||
         detail.find("not yet valid") != std::string_view::npos;
}

}  // namespace rommsync::sync
