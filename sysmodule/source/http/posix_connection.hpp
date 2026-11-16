// A plain, unencrypted TCP `Connection`, and the deadline-bounded connect that
// makes one.
//
// It is here rather than inside `ssl_http_client.cpp` because it names no libnx
// type -- only POSIX sockets, which devkitA64's newlib and every host have --
// so `sysmodule/AGENTS.md`'s rule applies and the CMake build compiles it too.
// That is not a tidiness point: a home RomM reached over `http://` on the LAN is
// a configuration docs/SECURITY.md lets the user choose, so this is a path that
// *ships*, and having it here is what puts it under `wire.*` against the real
// docker RomM instead of leaving an untested twin of a tested class.
//
// The `ssl` path cannot be shared this way and is not: it lives in
// `ssl_http_client.cpp` with the rest of what no test can reach before M8-1.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "http_wire.hpp"

namespace rommsync::sysmodule {

/// A name to an IPv4 address, in network byte order, or false.
///
/// A seam because the console's lookup is not `getaddrinfo` alone: it is one
/// IPC to `sfdnsres` that nothing on the calling thread can interrupt, so the
/// console's resolver arms a `CancelToken::Abort` that cancels it from the
/// thread that fires `cancel` (`ssl_http_client.cpp`, M11-6, #290). Null means
/// `ResolveBlocking`.
using Resolve = bool (*)(const std::string& host, std::uint32_t* address,
                         const http::CancelToken* cancel);

/// `getaddrinfo`, with `cancel` read before and after it but not during: POSIX
/// has no way to interrupt one. What the host build and the tests use.
bool ResolveBlocking(const std::string& host, std::uint32_t* address,
                     const http::CancelToken* cancel);

/// Resolve `origin` and connect, or return -1 with `error` and `message` set.
///
/// The descriptor comes back in **blocking** mode: the deadline is honoured with
/// a non-blocking connect and a `poll`, and then the flags are put back, because
/// the `ssl` service reads and writes the descriptor itself and every wait above
/// it is bounded by the caller's own budget rather than by the socket.
///
/// `io_timeout` is set as `SO_RCVTIMEO`/`SO_SNDTIMEO`. For the plain path that
/// is belt and braces under the `poll` in `Read`/`Write`; for the `ssl` path it
/// is the only bound that reaches the service's own I/O, and whether the service
/// honours a socket option set before it takes the descriptor is one of the
/// things a run on hardware would tell us (docs/DEVELOPMENT.md).
///
/// `cancel` ends any phase of it within one slice -- `http::Error::kCanceled`
/// -- and `resolve` is how the name, if it is one, becomes an address.
int ConnectTo(const Origin& origin, std::chrono::milliseconds connect_timeout,
              std::chrono::milliseconds io_timeout, http::Error* error, std::string* message,
              const http::CancelToken* cancel = nullptr, Resolve resolve = nullptr);

/// Take ownership of `fd` and speak plain TCP over it. Null only when there was
/// no room for the object, in which case `fd` is closed.
std::unique_ptr<Connection> MakePlainConnection(int fd);

/// The `Connector` for `http://`: `ConnectTo` plus `MakePlainConnection`.
///
/// The console's `SslConnector` delegates to these two for a non-TLS origin
/// rather than carrying a second copy of them, and the host suite uses this
/// class directly -- so what `wire.*` drives is the shipping code, minus the
/// `ssl` layer it cannot reach.
///
/// `resolve` is for the tests that need a lookup to block (`sleep.*`); the
/// default is `ResolveBlocking`.
class PlainConnector final : public Connector {
 public:
  explicit PlainConnector(Resolve resolve = nullptr) : resolve_(resolve) {}

  std::unique_ptr<Connection> Open(const Origin& origin,
                                   std::chrono::milliseconds connect_timeout,
                                   const http::ClientOptions& options,
                                   const http::CancelToken* cancel, http::Error* error,
                                   std::string* message) override;

 private:
  Resolve resolve_;
};

}  // namespace rommsync::sysmodule
