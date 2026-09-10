// What the sysmodule's boot path does off a console (M9-1, #195).
//
// **Name resolution**, driven through `posix_connection.cpp` -- the connector
// the console itself uses for an `http://` origin, not a twin of it
// (tests/tcp_connector.hpp). A `server.url` naming a host takes the
// `getaddrinfo` branch, and that branch is the one an `smExit()` in `__appInit`
// breaks on Horizon: libnx re-opens `sfdnsres` off the `sm` session on every
// call. Nothing off a console has an `sm` session to close, so what runs here is
// the resolution, and `boot.dns` is what holds the session open above it -- the
// two together are the decision #195 asked for, rather than a comment.
//
// One scenario per CTest entry, selected by argv[1], so a failure names the
// behaviour.
#include <netdb.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <string>

#include "checks.hpp"
#include "http/posix_connection.hpp"
#include "loopback_server.hpp"

namespace {

namespace http = rommsync::http;
using rommsync::sysmodule::ConnectTo;
using rommsync::sysmodule::Origin;

constexpr int kSkip = 77;

/// A name RFC 6761 reserves for "this never resolves". A resolver that answers
/// it anyway -- a captive portal, an ISP that monetises NXDOMAIN -- makes the
/// `unresolved` scenario a test of that resolver rather than of this code, so it
/// skips rather than failing.
constexpr const char* kNeverResolves = "rommsync-nx-no-such-host.invalid";

bool ResolvesHere(const char* host) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* resolved = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &resolved) != 0 || resolved == nullptr) return false;
  freeaddrinfo(resolved);
  return true;
}

/// `ConnectTo` reaches a loopback server addressed **by name**.
///
/// The point is the branch, not the connection: `inet_pton` answers a bare IPv4
/// literal without any name service at all, so `127.0.0.1` would prove nothing
/// about the path every `romm.local`, NAS name and DDNS name takes.
int Resolve(checks::Checks& checks) {
  rig::LoopbackServer server;
  if (!server.Start(1, [](int fd, std::size_t, const std::string&) {
        rig::WriteAll(fd, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
      })) {
    std::cerr << "  FAIL: could not start the loopback server\n";
    return 1;
  }

  Origin origin;
  origin.host = "localhost";
  origin.port = server.port();
  origin.tls = false;

  http::Error error = http::Error::kNone;
  std::string message;
  const int fd = ConnectTo(origin, std::chrono::milliseconds(2000),
                           std::chrono::milliseconds(2000), &error, &message);
  checks.Expect(fd >= 0, "connect to `localhost` -- " + message);
  if (fd >= 0) ::close(fd);
  server.Stop();
  return checks.failures();
}

/// ...and says so, rather than reporting a connect failure, when the name is the
/// thing that is wrong. `kUnresolvedHost` is what the overlay turns into "check
/// the address" rather than "check the server".
int Unresolved(checks::Checks& checks) {
  if (ResolvesHere(kNeverResolves)) {
    std::cerr << "SKIP: this resolver answers " << kNeverResolves << "\n";
    return kSkip;
  }
  Origin origin;
  origin.host = kNeverResolves;
  origin.port = 80;
  origin.tls = false;

  http::Error error = http::Error::kNone;
  std::string message;
  const int fd = ConnectTo(origin, std::chrono::milliseconds(2000),
                           std::chrono::milliseconds(2000), &error, &message);
  checks.Expect(fd < 0, "a name that does not resolve must not produce a descriptor");
  if (fd >= 0) ::close(fd);
  checks.Expect(error == http::Error::kUnresolvedHost,
                "an unresolvable host is kUnresolvedHost, not a connect failure");
  return checks.failures();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "";
  checks::Checks checks;

  if (scenario == "resolve") return Resolve(checks) == 0 ? 0 : 1;
  if (scenario == "unresolved") {
    const int rc = Unresolved(checks);
    return rc == kSkip ? kSkip : (rc == 0 ? 0 : 1);
  }

  std::cerr << "unknown scenario: " << scenario << "\n";
  return 2;
}
