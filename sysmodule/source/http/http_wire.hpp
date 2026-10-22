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

/// The in-flight buffer of an API exchange: `Send`, its request body and its
/// response. One per call, on the heap.
///
/// Small because nothing it carries is large enough for the chunk to matter --
/// the biggest response is a few hundred KiB of JSON, a few dozen reads -- and
/// because it is the one that can be in flight *beside* a download: the pairing
/// thread's polls run while the worker streams a rom, so this is what the second
/// concurrent request costs the inner heap (`sysmodule/source/main.cpp`).
inline constexpr std::size_t kTransferBufferSize = 16 * 1024;

/// The in-flight buffer of a `Download`, which is the one place the chunk size
/// is a cost that scales with the file (M9-10, #201).
///
/// On the console every `Connection::Read` is an `sslConnectionRead`, and every
/// one of those is an IPC round trip to the `ssl` sysmodule. At 16 KiB that is
/// 65,536 round trips per GiB of rom; NPShopSwitch, a shipping boot2 sysmodule,
/// measured the same choice holding its HTTPS installs under 1 MB/s
/// (nps_http_ssl.cpp). The old justification -- "a TLS record's worth" -- was
/// about TLS framing, and framing is not the cost: the IPC is per call, and only
/// the call count scales with a rom.
///
/// **64 KiB, because that is the receive window.** A read can only return what
/// the socket has buffered, and `SocketBudget::tcp_rx_buf_max_size` caps that at
/// 64 KiB (`ssl_http_client.hpp`), so a larger buffer is heap the read has
/// nothing to fill from. That is 16,384 round trips per GiB, a quarter of
/// before -- **at best**: it assumes one `sslConnectionRead` hands back more
/// than one TLS record, which NPShopSwitch's speed-up implies and nothing off a
/// console can show. `tlsprobe` reports the largest read it saw, and #44 asks
/// for it. The window and this move together, and `main.cpp` holds them to it.
///
/// One download is in flight at a time -- the worker drains the queue one rom
/// after another (`download::Drain`) -- and a redirect frees one hop's buffer
/// before the next hop allocates its own, so this costs the heap once.
inline constexpr std::size_t kDownloadBufferSize = 64 * 1024;

/// The largest `Content-Length` an API body is given room for up front (M9-19,
/// #217).
///
/// `Send` holds its body in a `std::string`, and appended a read at a time that
/// string doubles: a body ends up holding as much as twice its length, and three
/// times while the last reallocation has both buffers live. The heap table in
/// `main.cpp` budgets every buffered response at its text, once, so the body is
/// reserved to the length the server declared before the first byte lands.
///
/// Up to here and no further, because the number is the server's: a
/// `Content-Length` of a gigabyte on a ten-byte body would otherwise be an
/// allocation this process makes on a stranger's say-so, and on a
/// `-fno-exceptions` build a failed one is `std::terminate`. 256 KiB is above
/// every response the table budgets -- `main.cpp` asserts it -- so a body past
/// it is one the heap was never sized for, and growing it the old way is no
/// worse than it was.
inline constexpr std::size_t kMaxReservedBodyBytes = 256 * 1024;

/// The largest API body taken **without** a `Content-Length` -- chunked, or
/// ended by the close -- and past it the call fails (M9-20, #246).
///
/// Such a body has nothing to reserve, so it grows, and until #246 nothing
/// stopped it. RomM behind its own nginx always declares a length (measured on
/// 5.2.0), but a reverse proxy in front of it may not, and every buffered
/// response is counted in `main.cpp`'s heap table at its text, once. So an
/// undeclared body grows in powers of two up to this and no further: it holds at
/// most this, and at most half again while the last step copies -- 192 KiB,
/// which `main.cpp` asserts fits the row a response is counted under. A power of
/// two so the last step lands on it exactly.
///
/// What it costs is a library past ~60 roms a page behind a proxy that strips
/// `Content-Length`: a 64-row page of RomM's ~2.1 KiB rows is ~135 KiB, so the
/// index fetch fails, by name, rather than growing into a heap with no room.
inline constexpr std::size_t kMaxUndeclaredBodyBytes = 128 * 1024;
static_assert((kMaxUndeclaredBodyBytes & (kMaxUndeclaredBodyBytes - 1)) == 0,
              "grown in powers of two, the cap has to be one for the last step to land on it");

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
