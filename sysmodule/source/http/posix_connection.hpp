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

#include <algorithm>
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

/// How long any one wait inside `Connector::Open` blocks before the cancel
/// token is read again (M11-6, #290). A tenth of a second, so a cancelled open
/// is back in one slice and the quiesce budget is spent on the rest of the
/// tick's way out rather than on a connect that was never going to finish.
inline constexpr std::chrono::milliseconds kOpenSlice{100};

/// One try at a phase of an open that can be waited on: finished, worth
/// waiting another slice for, or over.
enum class Step { kDone, kPending, kFailed };

/// How a sliced phase ended.
enum class Sliced { kDone, kFailed, kCanceled, kTimedOut };

/// Call `step(slice)` until it is done or failed, `cancel` fires, or `timeout`
/// passes. `step` itself blocks for at most `slice`.
///
/// The one loop the connect and the console's handshake both run, so neither
/// can forget the token: before #290 each was one call bounded only by its own
/// timeout, ten seconds and more, and a sleep could not end it.
template <typename StepFn>
Sliced RunInSlices(StepFn step, const http::CancelToken* cancel,
                   std::chrono::milliseconds timeout) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (cancel != nullptr && cancel->canceled()) return Sliced::kCanceled;
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now >= deadline) return Sliced::kTimedOut;
    const std::chrono::milliseconds slice =
        std::min(kOpenSlice, std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
    switch (step(slice)) {
      case Step::kDone:
        return Sliced::kDone;
      case Step::kFailed:
        return Sliced::kFailed;
      case Step::kPending:
        break;
    }
  }
}

/// Resolve `origin` and connect, or return -1 with `error` and `message` set.
///
/// The descriptor comes back in **blocking** mode: the deadline is honoured with
/// a non-blocking connect and a `poll` in `kOpenSlice`s, and then the flags are
/// put back, because the `ssl` service reads and writes the descriptor itself
/// and every wait above it is bounded by the caller's own budget rather than by
/// the socket.
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
