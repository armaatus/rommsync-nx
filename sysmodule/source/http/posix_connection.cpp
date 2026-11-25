// See posix_connection.hpp for why the plain path lives outside the `ssl` one.
#include "posix_connection.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>

#include "lookup_watch.hpp"

namespace rommsync::sysmodule {
namespace {

/// The request's cancel, passed on to the lookup's own token (`ConnectTo`).
void CancelFollower(void* follower) { static_cast<http::CancelToken*>(follower)->Cancel(); }

/// `connect()` with a deadline, and with the cancel token read every slice. A
/// blocking connect can hang for the stack's own timeout, and never blocking
/// boot is a hard rule (CLAUDE.md); a connect a sleep cannot end is a bsd call
/// still running when PSC is told this process is quiet (M11-6, #290).
Sliced ConnectWithTimeout(int fd, const sockaddr_in& address, std::chrono::milliseconds timeout,
                          const http::CancelToken* cancel) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return Sliced::kFailed;

  int rc = connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  if (rc < 0 && errno != EINPROGRESS) return Sliced::kFailed;
  if (rc < 0) {
    const Sliced waited = RunInSlices(
        [fd](std::chrono::milliseconds slice) {
          pollfd descriptor{fd, POLLOUT, 0};
          const int ready = poll(&descriptor, 1, static_cast<int>(slice.count()));
          if (ready == 0) return Step::kPending;
          if (ready < 0) return errno == EINTR ? Step::kPending : Step::kFailed;
          int pending = 0;
          socklen_t length = sizeof(pending);
          if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &pending, &length) < 0 || pending != 0) {
            return Step::kFailed;
          }
          return Step::kDone;
        },
        cancel, timeout);
    if (waited != Sliced::kDone) return waited;
  }
  return fcntl(fd, F_SETFL, flags) == 0 ? Sliced::kDone : Sliced::kFailed;
}

/// One open TCP socket. Every wait is the caller's slice, never the socket's own
/// idea of a timeout, so the `CancelToken` and the deadlines above stay the only
/// things that end a transfer (`http_wire.hpp`).
class PlainConnection final : public Connection {
 public:
  explicit PlainConnection(int fd) : fd_(fd) {}
  ~PlainConnection() override {
    if (fd_ >= 0) close(fd_);
  }

  IoResult Write(const void* data, std::size_t size, std::chrono::milliseconds timeout) override {
    if (!Wait(POLLOUT, timeout)) return {IoStatus::kTimedOut, 0};
    return Classify(send(fd_, data, size, 0));
  }

  IoResult Read(void* data, std::size_t size, std::chrono::milliseconds timeout) override {
    if (!Wait(POLLIN, timeout)) return {IoStatus::kTimedOut, 0};
    return Classify(recv(fd_, data, size, 0));
  }

 private:
  bool Wait(short events, std::chrono::milliseconds timeout) const {
    pollfd descriptor{fd_, events, 0};
    return poll(&descriptor, 1, static_cast<int>(timeout.count())) > 0;
  }

  /// `ECONNRESET` is what the fault proxy's `drop` mode arrives as, and it is a
  /// broken connection rather than a clean end -- the framing above has to be
  /// able to tell the two apart, so this never reports it as `kClosed`.
  static IoResult Classify(ssize_t moved) {
    if (moved > 0) return {IoStatus::kOk, static_cast<std::size_t>(moved)};
    if (moved == 0) return {IoStatus::kClosed, 0};
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return {IoStatus::kOk, 0};
    return {IoStatus::kFailed, 0};
  }

  int fd_ = -1;
};

}  // namespace

bool ResolveBlocking(const std::string& host, std::uint32_t* address,
                     const http::CancelToken* cancel) {
  if (cancel != nullptr && cancel->canceled()) return false;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* resolved = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &resolved) != 0 || resolved == nullptr) {
    return false;
  }
  *address = reinterpret_cast<sockaddr_in*>(resolved->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(resolved);
  return cancel == nullptr || !cancel->canceled();
}

int ConnectTo(const Origin& origin, std::chrono::milliseconds connect_timeout,
              std::chrono::milliseconds io_timeout, http::Error* error, std::string* message,
              const http::CancelToken* cancel, Resolve resolve) {
  sockaddr_in address{};
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(origin.port);

  // An address first, because on the test rig and on a LAN RomM it usually is
  // one -- and because that path needs no name service at all.
  if (inet_pton(AF_INET, origin.host.c_str(), &address.sin_addr) != 1) {
    std::uint32_t resolved = 0;
    const Resolve lookup = resolve != nullptr ? resolve : ResolveBlocking;
    // The lookup runs on a token of its own (#335), so its deadline can cancel
    // it without cancelling the request: `cancel` is a tick's, and a slow
    // resolver is a timed-out request, not a tick that was stopped. `cancel`
    // still reaches it, through `follow`, and the watch cancels it at the
    // connect timeout -- both through the `Abort` the resolver arms on it.
    http::CancelToken looking;
    const http::CancelToken::Abort follow(cancel, CancelFollower, &looking);
    if (cancel != nullptr && cancel->canceled()) looking.Cancel();
    const int watched = Lookups().Arm(&looking, LookupWatch::Clock::now() + connect_timeout);
    const bool found = lookup(origin.host, &resolved, &looking);
    const bool expired = Lookups().Disarm(watched);
    if (!found) {
      // A lookup the cancel cut short is not a name that does not exist: the
      // tick ends cancelled, not with a `net.offline` about a working link.
      // Nor is one the deadline cut short, which is a resolver that did not
      // answer in time.
      if (cancel != nullptr && cancel->canceled()) {
        *error = http::Error::kCanceled;
        *message = "canceled while resolving " + origin.host;
      } else if (expired) {
        *error = http::Error::kTimeout;
        *message = "timed out resolving " + origin.host + " after " +
                   std::to_string(connect_timeout.count()) + "ms";
      } else {
        *error = http::Error::kUnresolvedHost;
        *message = "could not resolve " + origin.host;
      }
      return -1;
    }
    address.sin_addr.s_addr = resolved;
  }

  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = http::Error::kConnectFailed;
    *message = "socket() failed";
    return -1;
  }

  timeval bound{};
  bound.tv_sec = static_cast<time_t>(io_timeout.count() / 1000);
  bound.tv_usec = static_cast<suseconds_t>((io_timeout.count() % 1000) * 1000);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &bound, sizeof(bound));

  const Sliced connected = ConnectWithTimeout(fd, address, connect_timeout, cancel);
  if (connected != Sliced::kDone) {
    close(fd);
    const bool canceled = connected == Sliced::kCanceled;
    *error = canceled ? http::Error::kCanceled : http::Error::kConnectFailed;
    *message = (canceled ? "canceled while connecting to " : "could not connect to ") +
               origin.host;
    return -1;
  }
  return fd;
}

std::unique_ptr<Connection> MakePlainConnection(int fd) {
  // Non-blocking, which `ConnectTo` deliberately does not leave it: the `ssl`
  // service reads and writes its descriptor itself and wants a blocking one,
  // while here `poll` is the wait and `send`/`recv` must not add a second one.
  // Otherwise a 16 KiB `send` on a congested link returns when the whole buffer
  // is queued or when `SO_SNDTIMEO` fires -- ten seconds past the 200 ms slice
  // the caller asked for, which is the cancel latency and the stall accounting
  // both gone.
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  std::unique_ptr<Connection> connection(new (std::nothrow) PlainConnection(fd));
  if (connection == nullptr) close(fd);
  return connection;
}

std::unique_ptr<Connection> PlainConnector::Open(const Origin& origin,
                                                 std::chrono::milliseconds connect_timeout,
                                                 const http::ClientOptions&,
                                                 const http::CancelToken* cancel,
                                                 http::Error* error, std::string* message) {
  *error = http::Error::kNone;
  if (origin.tls) {
    *error = http::Error::kTls;
    *message = "this connector speaks no TLS";
    return nullptr;
  }
  const int fd = ConnectTo(origin, connect_timeout, connect_timeout, error, message, cancel,
                           resolve_);
  if (fd < 0) return nullptr;
  std::unique_ptr<Connection> connection = MakePlainConnection(fd);
  if (connection == nullptr) {
    *error = http::Error::kTransport;
    *message = "no room for a connection";
  }
  return connection;
}

}  // namespace rommsync::sysmodule
