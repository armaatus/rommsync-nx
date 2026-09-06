// The HTTP half of the console's `http::HttpClient`, with no transport in it.
//
// `sysmodule/source/http/` is where the Horizon backend lives, because `core/`
// may not name a transport (hard rule 4). But almost none of what a backend has
// to get right is *transport*: it is HTTP/1.1 framing, `Range` resume, the
// `.part` file, the timeouts and the progress contract in
// `core/include/rommsync/http.hpp` -- and every one of those can be got wrong
// without a console. So the backend is split in two here:
//
//   * `WireHttpClient` -- this file. Speaks HTTP over an abstract `Connection`
//     and names no libnx type, so `sysmodule/AGENTS.md`'s rule applies: the host
//     CMake build compiles it too, and `wire.*` drives it against the real
//     docker RomM through the fault proxy, over the same eighteen scenarios
//     `http.*` holds libcurl's backend to (tests/test_http_native.cpp).
//   * `SslConnector` -- `ssl_http_client.hpp`. bsd sockets plus the Horizon
//     `ssl` service, which cannot be executed anywhere before the M8-1 gate
//     (#43) and is kept as small as it can be for exactly that reason.
//
// The split is the point. What ships is one client; what a test can reach is all
// of it except the socket underneath.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "rommsync/http.hpp"

namespace rommsync::sysmodule {

/// How one read or write ended.
enum class IoStatus {
  kOk,        ///< `transferred` bytes moved; may be fewer than asked for
  kTimedOut,  ///< the deadline passed with nothing moved
  kClosed,    ///< the peer closed cleanly; nothing more will arrive
  kFailed,    ///< the connection is broken
};

struct IoResult {
  IoStatus status = IoStatus::kFailed;
  std::size_t transferred = 0;
};

/// One open byte stream to an origin, already through TLS if it needed to be.
///
/// Every call carries its own deadline rather than the connection holding one:
/// the caller owns the total and stall budgets (`http::Request`), and it is the
/// caller that has to poll the `CancelToken` between them. A backend that
/// blocked for its own idea of a timeout would be the five-minute
/// `SslIoMode_Blocking` problem (docs/DEVELOPMENT.md#tls-in-a-sysmodule) with
/// extra steps.
class Connection {
 public:
  virtual ~Connection() = default;

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  /// Move up to `size` bytes out of `data`. `kOk` with `transferred == 0` is
  /// allowed and means "not yet"; the caller loops on its own deadline.
  virtual IoResult Write(const void* data, std::size_t size,
                         std::chrono::milliseconds timeout) = 0;

  /// Fill up to `size` bytes of `data`. `kClosed` is the end of the body for a
  /// response the server framed by closing.
  virtual IoResult Read(void* data, std::size_t size, std::chrono::milliseconds timeout) = 0;

 protected:
  Connection() = default;
};

/// Where a `Connection` comes from. One per transport: `SslConnector` on the
/// console, a plain TCP one in the host suite.
struct Origin {
  std::string host;  ///< a name or an address, as it appeared in the URL
  std::uint16_t port = 443;
  bool tls = true;
};

class Connector {
 public:
  virtual ~Connector() = default;

  Connector(const Connector&) = delete;
  Connector& operator=(const Connector&) = delete;

  /// Open a connection, or explain why not. `error` is one of
  /// `http::Error::{kUnresolvedHost, kConnectFailed, kTls, kTimeout,
  /// kTransport}` and `message` is for the log, never for a user.
  virtual std::unique_ptr<Connection> Open(const Origin& origin,
                                           std::chrono::milliseconds connect_timeout,
                                           const http::ClientOptions& options,
                                           http::Error* error, std::string* message) = 0;

 protected:
  Connector() = default;
};

/// The one in-flight buffer the heap budget in
/// docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision is about.
///
/// One per call, on the heap, and never one per rom: a download streams through
/// this and lands on the card. 16 KiB is a TLS record's worth, so a read rarely
/// stops in the middle of one, and it is small enough that two concurrent
/// transfers still fit the inner heap (`sysmodule/source/main.cpp`).
inline constexpr std::size_t kTransferBufferSize = 16 * 1024;

/// How much of a failed download's body is kept, matching the native backend.
/// A 404 from RomM is a short JSON document and worth surfacing; anything
/// longer is a page that must not reach a log line or the destination file.
inline constexpr std::size_t kErrorBodyCap = 64 * 1024;

/// An `http::HttpClient` that frames HTTP/1.1 over whatever `connector` opens.
///
/// Thread safe in the sense `http.hpp` requires: it holds no per-request state,
/// every exchange lives on the calling thread's stack, and a `Connection` is
/// opened and closed inside one call. Every request is `Connection: close` --
/// keep-alive would mean a connection pool with its own lifetime rules inside a
/// process that is trying to hold on to as little as it can.
std::unique_ptr<http::HttpClient> MakeWireHttpClient(Connector& connector,
                                                     const http::ClientOptions& options = {});

}  // namespace rommsync::sysmodule
