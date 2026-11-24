// Stage 3: nifm, DNS and TLS through the sysmodule's own `ssl` backend.
//
// Every request goes through `rommsync::sysmodule::MakeHorizonHttpClient` --
// sysmodule/source/http, compiled in whole -- so a row that passes here is the
// transport the sysmodule will use, under the timeouts it sets.
#include <netdb.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "rommsync/clock_check.hpp"
#include "selftest.hpp"
#include "ssl_http_client.hpp"

namespace selftest {
namespace {

using Clock = std::chrono::steady_clock;
using rommsync::http::Error;
using rommsync::http::Request;
using rommsync::http::Result;

long long MillisSince(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

/// `https://host[:port]/...` -> `host`.
std::string HostOf(const std::string& url) {
  std::size_t start = url.find("://");
  start = start == std::string::npos ? 0 : start + 3;
  std::size_t end = url.find_first_of(":/", start);
  if (end == std::string::npos) end = url.size();
  return url.substr(start, end - start);
}

Request Get(const std::string& url) {
  Request request;
  request.url = url;
  return request;
}

}  // namespace

std::optional<rommsync::sync::Timestamp> NetworkClock() {
  u64 seconds = 0;
  if (R_FAILED(timeGetCurrentTime(TimeType_NetworkSystemClock, &seconds))) return std::nullopt;
  return rommsync::sync::Timestamp{std::chrono::seconds(seconds)};
}

void RunNetwork(Run& run) {
  Report& report = *run.report;
  rommsync::http::HttpClient& http = *run.http;
  const std::string heartbeat = run.config.url + "/api/heartbeat";

  // nifm, the way the engine asks it before every tick.
  NifmInternetConnectionType type = {};
  u32 strength = 0;
  NifmInternetConnectionStatus status = {};
  const ::Result nifm = nifmGetInternetConnectionStatus(&type, &strength, &status);
  using rommsync::sysmodule::NifmVerdict;
  const bool online = rommsync::sysmodule::NetworkReady() &&
                      rommsync::sysmodule::ConsoleNetwork() == NifmVerdict::kOnline;
  char text[160];
  std::snprintf(text, sizeof(text), "%s: rc=0x%08x, type %d, strength %u, status %d",
                online ? "online" : "offline", nifm, static_cast<int>(type), strength,
                static_cast<int>(status));
  report.Row("net.nifm", online ? Outcome::kPass : Outcome::kFail, text);

  // DNS, through sfdnsres -- the call M11-6 (#290) bounds with an abort.
  const std::string host = HostOf(run.config.url);
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  Clock::time_point start = Clock::now();
  const int resolved = getaddrinfo(host.c_str(), nullptr, &hints, &found);
  const long long dns_ms = MillisSince(start);
  if (found != nullptr) freeaddrinfo(found);
  report.Row("net.dns", resolved == 0 ? Outcome::kPass : Outcome::kFail,
             host + (resolved == 0 ? " resolved" : " did not resolve: " + Number(resolved)) +
                 " in " + Number(dns_ms) + " ms");

  // A verified request: PeerCa | HostName | DateCheck, against the CertStore
  // and `ca_file` -- the options the sysmodule sets.
  start = Clock::now();
  const Result first = http.Send(Get(heartbeat));
  run.server_date = rommsync::sync::ServerDate(first.response.headers);
  report.Row("net.tls", first.successful() ? Outcome::kPass : Outcome::kFail,
             Describe(first) + " from " + heartbeat + " in " + Number(MillisSince(start)) +
                 " ms" + (run.config.ca_file.empty() ? ", CertStore" : ", CertStore + ca_file"));

  // The console's clock against the server's (clock_check.hpp): the check the
  // engine makes before every plan, with the references it has.
  rommsync::sync::ClockReading reading;
  reading.local = std::chrono::system_clock::now();
  reading.network = NetworkClock();
  reading.server = run.server_date;
  const rommsync::sync::ClockCheck clock = rommsync::sync::CheckClock(reading);
  report.Row("platform.clock",
             run.server_date && clock.trusted() ? Outcome::kPass : Outcome::kFail,
             run.server_date ? clock.Describe() : std::string("no Date header to check against"),
             Number(clock.skew_seconds));

  if (run.config.tls13_url.empty()) {
    report.Row("net.tls13_only", Outcome::kSkip, "no tls13_url in selftest.ini");
  } else {
    const Result tls13 = http.Send(Get(run.config.tls13_url));
    report.Row("net.tls13_only", tls13.successful() ? Outcome::kPass : Outcome::kFail,
               Describe(tls13) + " from " + run.config.tls13_url);
  }

  // Handshakes that fail, more of them than bsd has sockets to lose, and then
  // one that must still work. If the `ssl` service did not take the
  // descriptor on ENOENT (ssl_http_client.cpp), each of these leaked a socket
  // and the last request has none left to open.
  constexpr int kFailedHandshakes = 64;
  int refused = 0;
  std::string odd;
  const HeapCount heap_before = HeapNow();
  for (int i = 0; i < kFailedHandshakes; ++i) {
    const Result bad = http.Send(Get(run.config.bad_tls_url));
    if (bad.error == Error::kTls) {
      ++refused;
    } else if (odd.empty()) {
      odd = "; attempt " + Number(i + 1) + " was " + Describe(bad);
    }
  }
  const Result after = http.Send(Get(heartbeat));
  const long long heap_growth = static_cast<long long>(HeapNow().outstanding_bytes) -
                                static_cast<long long>(heap_before.outstanding_bytes);
  report.Row("net.handshake_leak",
             refused == kFailedHandshakes && after.successful() ? Outcome::kPass : Outcome::kFail,
             Number(refused) + "/" + Number(kFailedHandshakes) +
                 " handshakes refused as TLS failures, then " + Describe(after) +
                 "; heap moved " + Number(heap_growth) + " bytes" + odd);

  // Timeouts honoured: a connect into nothing, and -- when one is named -- a
  // listener that accepts and never answers, which is the read the
  // sslConnectionPoll/SetIoTimeout questions are about.
  constexpr auto kConnect = std::chrono::milliseconds(3'000);
  constexpr long long kSlackMs = 2'000;
  Request blackhole = Get("https://" + run.config.blackhole_host + "/");
  blackhole.connect_timeout = kConnect;
  start = Clock::now();
  const Result nowhere = http.Send(blackhole);
  const long long nowhere_ms = MillisSince(start);
  bool timely = !nowhere.ok() && nowhere_ms <= kConnect.count() + kSlackMs;
  std::string detail = "connect to " + run.config.blackhole_host + ": " + Describe(nowhere) +
                       " after " + Number(nowhere_ms) + " ms (limit " +
                       Number(kConnect.count()) + ")";
  if (!run.config.stall_url.empty()) {
    constexpr auto kStall = std::chrono::milliseconds(3'000);
    Request stalled = Get(run.config.stall_url);
    stalled.stall_timeout = kStall;
    stalled.timeout = std::chrono::milliseconds(30'000);
    start = Clock::now();
    const Result stall = http.Send(stalled);
    const long long stall_ms = MillisSince(start);
    const bool stall_timely = stall.error == Error::kTimeout &&
                              stall_ms <= kStall.count() + kConnect.count() + kSlackMs;
    timely = timely && stall_timely;
    detail += "; stalled read: " + Describe(stall) + " after " + Number(stall_ms) + " ms";
  } else {
    detail += "; no stall_url, so the read timeout was not asked";
  }
  report.Row("net.timeout", timely ? Outcome::kPass : Outcome::kFail, detail);

  // Fifty in a row: every one answers, and the heap is where it was after the
  // first five -- a socket, a session or a buffer per request would show here.
  constexpr int kRequests = 50;
  int answered = 0;
  std::size_t settled = 0;
  for (int i = 0; i < kRequests; ++i) {
    answered += http.Send(Get(heartbeat)).successful() ? 1 : 0;
    if (i == 4) settled = HeapNow().outstanding_bytes;
  }
  const long long drift =
      static_cast<long long>(HeapNow().outstanding_bytes) - static_cast<long long>(settled);
  constexpr long long kDriftLimit = 8 * 1024;
  report.Row("net.connect_50",
             answered == kRequests && drift <= kDriftLimit ? Outcome::kPass : Outcome::kFail,
             Number(answered) + "/" + Number(kRequests) + " answered; heap moved " +
                 Number(drift) + " bytes after the fifth (limit " + Number(kDriftLimit) + ")");
}

}  // namespace selftest
