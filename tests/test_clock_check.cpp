// Whether the console's clock is believed (M11-11, #295), with a fake clock.
//
// `sync::CheckClock` reads no clock -- every instant is an argument -- so a
// console a year in the past, a week in the future, or an hour off can be put
// in front of it directly. What a failed check does to a tick is `tick.clock`'s
// (test_sync_tick.cpp); this is the decision itself, and the `Date` header it
// reads. No network, so this never skips.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include "checks.hpp"
#include "rommsync/clock_check.hpp"
#include "rommsync/http.hpp"
#include "rommsync/sync.hpp"

namespace {

namespace sync = rommsync::sync;
namespace http = rommsync::http;
using std::chrono::hours;
using std::chrono::seconds;

/// 2026-11-13T12:00:00Z: the day this was written, a known-good instant.
sync::Timestamp Today() {
  return sync::Timestamp{} + seconds{1'794'571'200};
}

void WithinThresholdIsTrusted(checks::Checks& c) {
  // An hour off either reference -- a time zone mistaken for UTC on the way --
  // is inside the day the check allows.
  sync::ClockReading reading;
  reading.local = Today() + hours{1};
  reading.network = Today();
  reading.server = Today() - hours{2};
  const sync::ClockCheck check = sync::CheckClock(reading);
  c.Expect(check.trusted(), "an hour off both references is trusted: " + check.Describe());

  // 23 hours ahead of the server is still inside it.
  reading.network.reset();
  reading.server = Today();
  reading.local = Today() + hours{23};
  c.Expect(sync::CheckClock(reading).trusted(), "23 hours ahead of the server is trusted");

  // No reference at all, and a clock past the floor: an unknown is not a
  // finding, so nothing is held.
  sync::ClockReading alone;
  alone.local = Today();
  const sync::ClockCheck unknown = sync::CheckClock(alone);
  c.Expect(unknown.trusted(), "a clock past the floor with no reference is trusted");
  c.Expect(unknown.reference == sync::ClockReference::kNone, "...against nothing");
}

void FutureConsoleIsAhead(checks::Checks& c) {
  // A console set a week ahead: every save it writes would win a first sync.
  sync::ClockReading reading;
  reading.local = Today() + hours{24 * 7};
  reading.server = Today();
  const sync::ClockCheck check = sync::CheckClock(reading);
  c.Expect(check.verdict == sync::ClockVerdict::kAhead,
           std::string("a week ahead of the server is ahead: ") + sync::ToString(check.verdict));
  c.Expect(check.reference == sync::ClockReference::kServer, "...of the server");
  c.ExpectEq(check.skew_seconds, std::int64_t{7 * 24 * 3600}, "by a week, in seconds");
  c.Expect(!check.trusted(), "and not trusted");
  const std::string said = check.Describe();
  c.Expect(said.find("ahead") != std::string::npos && said.find("7 days") != std::string::npos &&
               said.find("server") != std::string::npos,
           "the sentence says which way, how far and against what: " + said);

  // Just past the threshold is past it.
  reading.local = Today() + hours{24} + seconds{1};
  c.Expect(sync::CheckClock(reading).verdict == sync::ClockVerdict::kAhead,
           "a day and a second ahead is ahead");

  // The network clock is asked before the server, and decides when it
  // disagrees.
  reading.local = Today() + hours{48};
  reading.network = Today();
  reading.server = Today() + hours{48};
  const sync::ClockCheck network = sync::CheckClock(reading);
  c.Expect(network.verdict == sync::ClockVerdict::kAhead &&
               network.reference == sync::ClockReference::kNetwork,
           std::string("two days ahead of the network clock is ahead of it: ") +
               sync::ToString(network.reference));

  // And the server decides when only it disagrees: RomM compares against its
  // own stamps, so a skew against it is the skew the arbitration sees.
  reading.network = Today() + hours{48};
  reading.server = Today();
  const sync::ClockCheck server = sync::CheckClock(reading);
  c.Expect(server.verdict == sync::ClockVerdict::kAhead &&
               server.reference == sync::ClockReference::kServer,
           "agreeing with the network clock does not excuse disagreeing with the server");
}

void PastConsoleIsBehindOrBeforeTheFloor(checks::Checks& c) {
  // An RTC reset by a drained battery: 2000-01-01, decades before the floor,
  // and no reference to hand -- the console is offline, which is when it
  // happens.
  sync::ClockReading reset;
  reset.local = sync::Timestamp{} + seconds{946'684'800};
  const sync::ClockCheck floor = sync::CheckClock(reset);
  c.Expect(floor.verdict == sync::ClockVerdict::kBeforeFloor,
           std::string("a clock in 2000 is before the floor: ") + sync::ToString(floor.verdict));
  c.Expect(!floor.trusted(), "and is not trusted");
  c.Expect(!floor.Describe().empty(), "and says so: " + floor.Describe());

  // The floor's own second is past it; the one before is not.
  sync::ClockReading edge;
  edge.local = sync::Timestamp{} + seconds{sync::kClockFloorSeconds};
  c.Expect(sync::CheckClock(edge).trusted(), "the floor itself is a trusted instant");
  edge.local -= seconds{1};
  c.Expect(sync::CheckClock(edge).verdict == sync::ClockVerdict::kBeforeFloor,
           "a second before it is not");

  // Past the floor and still months behind the server: behind.
  sync::ClockReading behind;
  behind.local = Today() - hours{24 * 90};
  behind.server = Today();
  const sync::ClockCheck late = sync::CheckClock(behind);
  c.Expect(late.verdict == sync::ClockVerdict::kBehind,
           std::string("ninety days behind the server is behind: ") + sync::ToString(late.verdict));
  c.ExpectEq(late.skew_seconds, std::int64_t{-90 * 24 * 3600}, "by ninety days, negative");
  c.Expect(late.Describe().find("behind") != std::string::npos, "and says so: " + late.Describe());

  // A floor verdict still measures against a reference it has.
  reset.network = Today();
  const sync::ClockCheck measured = sync::CheckClock(reset);
  c.Expect(measured.verdict == sync::ClockVerdict::kBeforeFloor &&
               measured.reference == sync::ClockReference::kNetwork &&
               measured.skew_seconds < 0,
           "a reset clock with a network clock to hand names how far behind it is");
}

void ReadsTheDateHeader(checks::Checks& c) {
  // RFC 9110's own example.
  const std::optional<sync::Timestamp> example = sync::ParseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT");
  c.Expect(example.has_value(), "the RFC's IMF-fixdate is read");
  if (example.has_value()) {
    c.ExpectEq(sync::UnixSeconds(*example), std::int64_t{784'111'777}, "as the instant it names");
  }
  const std::optional<sync::Timestamp> today = sync::ParseHttpDate("Fri, 13 Nov 2026 12:00:00 GMT");
  c.Expect(today.has_value() && *today == Today(), "and today's, as uvicorn spells it");

  // Shapes that are not it: the obsolete forms, a missing zone, nonsense.
  c.Expect(!sync::ParseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT").has_value(),
           "RFC 850's form is not read");
  c.Expect(!sync::ParseHttpDate("Sun Nov  6 08:49:37 1994").has_value(),
           "asctime's form is not read");
  c.Expect(!sync::ParseHttpDate("Sun, 06 Nov 1994 08:49:37").has_value(), "a missing GMT is not");
  c.Expect(!sync::ParseHttpDate("Sun, 06 Foo 1994 08:49:37 GMT").has_value(),
           "an unknown month is not");
  c.Expect(!sync::ParseHttpDate("Sun, 31 Feb 1994 08:49:37 GMT").has_value(),
           "a day the month does not have is not");
  c.Expect(!sync::ParseHttpDate("").has_value(), "nor is nothing");

  http::Headers headers = {{"content-type", "application/json"},
                           {"date", "Fri, 13 Nov 2026 12:00:00 GMT"}};
  const std::optional<sync::Timestamp> found = sync::ServerDate(headers);
  c.Expect(found.has_value() && *found == Today(), "ServerDate finds a lower-case date header");
  c.Expect(!sync::ServerDate(http::Headers{}).has_value(), "and nothing in no headers");
}

void HoldsOnlyTheTimestampUpload(checks::Checks& c) {
  sync::SyncOperation operation;
  operation.action = sync::Action::kUpload;
  operation.reason = sync::Reason::kClientNewerNoHistory;
  c.Expect(sync::DecidedByClock(operation), "an upload with no sync history is the clock's");
  operation.reason = sync::Reason::kClientOnly;
  c.Expect(!sync::DecidedByClock(operation), "an upload into an empty slot is not");
  operation.reason = sync::Reason::kClientNewer;
  c.Expect(!sync::DecidedByClock(operation), "an upload past a baseline is not");
  operation.action = sync::Action::kDownload;
  operation.reason = sync::Reason::kServerNewerNoHistory;
  c.Expect(!sync::DecidedByClock(operation), "a download is never held: it backs up first");
}

void NamesATlsFailureTheClockCaused(checks::Checks& c) {
  sync::ClockReading reset;
  reset.local = sync::Timestamp{} + seconds{946'684'800};
  const sync::ClockCheck wrong = sync::CheckClock(reset);
  sync::ClockReading fine;
  fine.local = Today();
  const sync::ClockCheck right = sync::CheckClock(fine);

  c.Expect(sync::TlsClockFault(http::Error::kTls, "handshake failed", wrong),
           "a TLS failure on a reset clock is the clock's");
  c.Expect(!sync::TlsClockFault(http::Error::kTls, "tlsv1 alert protocol version", right),
           "one on a good clock, for another reason, is not");
  c.Expect(sync::TlsClockFault(http::Error::kTls,
                               "SSL certificate problem: certificate has expired", right),
           "a backend that says the certificate expired is the clock's");
  c.Expect(sync::TlsClockFault(http::Error::kTls,
                               "SSL certificate problem: certificate is not yet valid", right),
           "and so is one not yet valid");
  c.Expect(!sync::TlsClockFault(http::Error::kConnectFailed, "", wrong),
           "a failure that is not TLS is never the clock's");
}

}  // namespace

int main() {
  checks::Checks c;
  WithinThresholdIsTrusted(c);
  FutureConsoleIsAhead(c);
  PastConsoleIsBehindOrBeforeTheFloor(c);
  ReadsTheDateHeader(c);
  HoldsOnlyTheTimestampUpload(c);
  NamesATlsFailureTheClockCaused(c);
  if (c.failures() != 0) {
    std::cerr << c.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << "sync clock: past, future and within the threshold agree\n";
  return 0;
}
