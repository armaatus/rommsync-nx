// bsd + `ssl`, the Horizon transport under `http_wire.cpp`.
//
// The sequence is the one M0-1 found the API forces
// (docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision): `ssl` does not
// create sockets, it *takes* one. So bsd first -- socket, connect -- then
// `sslContextCreateConnection`, `socketSslConnectionSetSocketDescriptor` (the
// libnx wrapper, because the raw cmd wants the bsd-side descriptor), then
// `DoHandshake`. The descriptor the wrapper hands back is ours to `close()`,
// *before* `sslConnectionClose`.
//
// **None of this has been executed**, on hardware or in an emulator: hard rule 1
// puts a console behind the M8-1 gate (#43), and the Ryujinx rung needs keys
// this project may not have. What is checked here is that it compiles and links
// for aarch64 (`switch.builds`). Everything above it is checked for real by
// `wire.*` against the docker RomM.
#include "ssl_http_client.hpp"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "http_wire.hpp"
#include "posix_connection.hpp"

namespace rommsync::sysmodule {
namespace {

/// Process-wide, because `socketInitialize` and `sslInitialize` are: both take
/// an `sm` session, and a sysmodule has one only inside `__appInit`
/// (`ipc/server.hpp` says the same thing about its port).
bool g_socket_up = false;
bool g_ssl_up = false;
bool g_nifm_up = false;

/// Whether the console has an internet connection, asked before a connect
/// rather than after a ten-second timeout.
///
/// This is what "offline-safe" costs on a console (CLAUDE.md): a sync tick on a
/// Switch in a bag would otherwise spend `connect_timeout` per request finding
/// out what nifm can answer in one IPC call. A nifm that will not answer is not
/// treated as offline -- the transport is bsd and ssl, and refusing to try
/// because a diagnostic service was busy would be the worse mistake.
bool ConsoleIsOnline() {
  if (!g_nifm_up) return true;
  NifmInternetConnectionType type{};
  u32 strength = 0;
  NifmInternetConnectionStatus status{};
  if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status))) {
    return true;
  }
  return status == NifmInternetConnectionStatus_Connected;
}

/// One `SslConnection`, and the socket it was handed.
///
/// The teardown order is the API's, not a preference: the descriptor
/// `socketSslConnectionSetSocketDescriptor` returned is closed *before*
/// `sslConnectionClose`, and the connection before the context that made it.
class SslStream final : public Connection {
 public:
  SslStream(SslConnection connection, int ssl_sockfd)
      : connection_(connection), ssl_sockfd_(ssl_sockfd) {}

  ~SslStream() override {
    if (ssl_sockfd_ >= 0) close(ssl_sockfd_);
    sslConnectionClose(&connection_);
  }

  IoResult Write(const void* data, std::size_t size, std::chrono::milliseconds timeout) override {
    if (size == 0) return {IoStatus::kOk, 0};
    if (!Wait(SslPollEvent_Write, timeout)) return {IoStatus::kTimedOut, 0};
    u32 written = 0;
    const Result rc = sslConnectionWrite(&connection_, data, static_cast<u32>(size), &written);
    if (R_FAILED(rc)) return {IoStatus::kFailed, 0};
    return {IoStatus::kOk, written};
  }

  IoResult Read(void* data, std::size_t size, std::chrono::milliseconds timeout) override {
    if (size == 0) return {IoStatus::kOk, 0};
    if (!Wait(SslPollEvent_Read, timeout)) return {IoStatus::kTimedOut, 0};
    u32 got = 0;
    const Result rc = sslConnectionRead(&connection_, data, static_cast<u32>(size), &got);
    if (R_FAILED(rc)) {
      // A server closing after `Connection: close` is the expected end of a
      // close-framed body, and some firmware reports it as a failed Read rather
      // than a zero-length one -- M0-1 recorded that as `read_end_rc` rather
      // than a failure (tlsprobe/source/tls_get.cpp).
      //
      // Which of the two it was is not this layer's to decide, and that is the
      // safe direction: `http_wire.cpp` frames the body, so a `Content-Length`
      // still owed makes this `Error::kTruncated` and a close-framed body makes
      // it a clean end. Guessing "clean" here would be the one way a short rom
      // could look complete.
      return {IoStatus::kFailed, 0};
    }
    if (got == 0) return {IoStatus::kClosed, 0};
    return {IoStatus::kOk, got};
  }

 private:
  /// `sslConnectionPoll` rather than a blocking read, because
  /// `SslIoMode_Blocking`'s own timeout is **five minutes** (libnx `ssl.h`) and
  /// that is not a wait a sync tick -- or the overlay's "stop" -- can sit out.
  /// The caller's slice is what bounds it (`http_wire.cpp`'s `Budget`).
  bool Wait(u32 events, std::chrono::milliseconds timeout) {
    u32 signalled = 0;
    const u32 bounded = static_cast<u32>(timeout.count() < 0 ? 0 : timeout.count());
    const Result rc =
        sslConnectionPoll(&connection_, events | SslPollEvent_Except, &signalled, bounded);
    if (R_FAILED(rc)) {
      // The poll itself failed. Let the I/O call report what is wrong rather
      // than translating a failed diagnostic into a timeout.
      return true;
    }
    return (signalled & (events | SslPollEvent_Except)) != 0;
  }

  SslConnection connection_{};
  int ssl_sockfd_ = -1;
};

class SslConnector final : public Connector {
 public:
  ~SslConnector() override {
    if (context_open_) sslContextClose(&context_);
  }

  std::unique_ptr<Connection> Open(const Origin& origin,
                                   std::chrono::milliseconds connect_timeout,
                                   const http::ClientOptions& options, http::Error* error,
                                   std::string* message) override {
    *error = http::Error::kNone;
    if (!g_socket_up) {
      *error = http::Error::kConnectFailed;
      *message = "this build brought no socket driver up";
      return nullptr;
    }
    // Before the connect, not after it. A build whose `ssl` did not come up can
    // still open a TCP connection to an `https://` origin and get all the way to
    // the handshake before failing -- which would cost a caller a whole connect
    // timeout per request to learn something this process knew at boot, and
    // would report it as `kTls` where the header promises `kConnectFailed`.
    if (origin.tls && !g_ssl_up) {
      *error = http::Error::kConnectFailed;
      *message = "this build brought no ssl service up";
      return nullptr;
    }
    if (!ConsoleIsOnline()) {
      *error = http::Error::kConnectFailed;
      *message = "the console has no internet connection";
      return nullptr;
    }

    // The handshake's own bound, and the socket's. Deliberately the connect
    // budget: a handshake that has not finished in as long as a connection took
    // to make is one that is not going to.
    const int fd = ConnectTo(origin, connect_timeout, connect_timeout, error, message);
    if (fd < 0) return nullptr;

    if (!origin.tls) {
      std::unique_ptr<Connection> plain = MakePlainConnection(fd);
      if (plain == nullptr) {
        *error = http::Error::kTransport;
        *message = "no room for a connection";
      }
      return plain;
    }
    // `OpenTls` owns `fd` from here, including closing it -- once the service
    // has taken the descriptor it is the one `ssl` handed back that has to be
    // closed instead, and closing both would be a double close of a descriptor
    // another process is using.
    return OpenTls(fd, origin, options, error, message);
  }

 private:
  /// The `ssl` half. `fd` is still ours until
  /// `socketSslConnectionSetSocketDescriptor` succeeds; after that the service
  /// owns it and the caller must not close it.
  std::unique_ptr<Connection> OpenTls(int fd, const Origin& origin,
                                      const http::ClientOptions& options, http::Error* error,
                                      std::string* message) {
    // One context for the process, so a CA import is paid once rather than per
    // request -- and guarded, because a context is one `ssl` session and two
    // threads opening a connection at the same time would be two commands on it.
    // **Only the shared `SslContext` needs the lock**, and it is released
    // before the handshake. Holding it across `DoHandshake` would mean a
    // download's handshake to an unresponsive server blocking a sync tick's
    // `Send` inside `Connector::Open` -- where neither the `CancelToken` nor
    // the `Budget` is polled -- for the `ssl` service's own I/O ceiling. A
    // shared client has to be safe for exactly that (`http.hpp`).
    SslConnection connection{};
    {
      std::lock_guard<std::mutex> held(mutex_);
      if (!EnsureContext(options, error, message)) {
        close(fd);
        return nullptr;
      }
      const Result rc = sslContextCreateConnection(&context_, &connection);
      if (R_FAILED(rc)) {
        close(fd);
        *error = http::Error::kTls;
        *message = "sslContextCreateConnection failed";
        return nullptr;
      }
    }
    Result rc = 0;

    // Exactly one descriptor is ours to close from here, and which one it is
    // changes half way down: `fd` until the service takes it, and the one the
    // wrapper hands back afterwards.
    int owned = fd;
    auto fail = [&](http::Error kind, const char* why) -> std::unique_ptr<Connection> {
      if (owned >= 0) close(owned);
      sslConnectionClose(&connection);
      *error = kind;
      *message = why;
      return nullptr;
    };

    // The wrapper, not `sslConnectionSetSocketDescriptor`: libnx keeps its own
    // fd table over the bsd service's, and the raw cmd wants the bsd-side one.
    // `errno == ENOENT` means "no descriptor came back", which the header says
    // to ignore rather than treat as a failure.
    const int returned = socketSslConnectionSetSocketDescriptor(&connection, fd);
    if (returned < 0 && errno != ENOENT) {
      return fail(http::Error::kTls, "socketSslConnectionSetSocketDescriptor failed");
    }
    // `-1` means the service kept the descriptor and will close it with the
    // connection, so nothing here closes one. **Whether that is also true of the
    // `errno == ENOENT` case is not settled** -- libnx says to ignore that
    // errno, and says nothing about who owns the descriptor afterwards. If it
    // does not, a failed handshake leaks an fd against a `handle_table_size` of
    // 64. It is on #43's gate list, because it is answerable on a console and
    // nowhere else; closing `fd` here on a guess would be a double close of a
    // descriptor another process is reading.
    owned = returned;

    // strlen + 1: libnx documents this parameter as a *buffer* size, and this
    // name is what `SslVerifyOption_HostName` is checked against -- a length
    // would hand the service an unterminated hostname
    // (tlsprobe/source/tls_get.cpp says the same thing at more length).
    rc = sslConnectionSetHostName(&connection, origin.host.c_str(),
                                  static_cast<u32>(origin.host.size() + 1));
    if (R_FAILED(rc)) return fail(http::Error::kTls, "sslConnectionSetHostName failed");

    if (options.verify_peer) {
      rc = sslConnectionSetVerifyOption(
          &connection,
          SslVerifyOption_PeerCa | SslVerifyOption_HostName | SslVerifyOption_DateCheck);
    } else {
      // [5.0.0+] refuses to clear PeerCa|HostName unless this is set first.
      // This is the deliberately-self-signed home server docs/SECURITY.md lets
      // the user opt into, and nothing else.
      rc = sslConnectionSetOption(&connection, SslOptionType_SkipDefaultVerify, true);
      if (R_SUCCEEDED(rc)) rc = sslConnectionSetVerifyOption(&connection, 0);
    }
    if (R_FAILED(rc)) return fail(http::Error::kTls, "sslConnectionSetVerifyOption failed");

    // Blocking, with the wait bounded from above: every read and write goes
    // through `sslConnectionPoll` with the caller's slice first (`SslStream`),
    // and on [16.0.0+] the service's own ceiling comes down from five minutes
    // to something a sync tick can wait out.
    rc = sslConnectionSetIoMode(&connection, SslIoMode_Blocking);
    if (R_FAILED(rc)) return fail(http::Error::kTls, "sslConnectionSetIoMode failed");
    if (hosversionAtLeast(16, 0, 0)) {
      sslConnectionSetIoTimeout(&connection, kIoTimeoutMs);
    }

    rc = sslConnectionDoHandshake(&connection, nullptr, nullptr, nullptr, 0);
    if (R_FAILED(rc)) {
      // Why it was refused, if the service will say. The cmd clears the stored
      // value as it reads it, so this is the only chance to ask -- and "the
      // handshake failed" and "the certificate was rejected, for this reason"
      // are the two findings a self-signed home server is told apart by.
      const Result verify = sslConnectionGetVerifyCertError(&connection);
      return fail(http::Error::kTls, R_FAILED(verify) ? "the server's certificate was refused"
                                                      : "the TLS handshake failed");
    }

    std::unique_ptr<Connection> stream(new (std::nothrow) SslStream(connection, returned));
    if (stream == nullptr) return fail(http::Error::kTransport, "no room for a connection");
    return stream;
  }

  /// The one `SslContext`, made on first use so a console that never talks to a
  /// server never pays for one.
  bool EnsureContext(const http::ClientOptions& options, http::Error* error,
                     std::string* message) {
    if (!context_open_) {
      // `SslVersion_Auto` is TLS 1.0-1.2; 1.3 needs [11.0.0+] and its own bit.
      // Auto is the widest set the installed firmware can honour without a
      // version gate of ours (docs/DEVELOPMENT.md).
      if (R_FAILED(sslCreateContext(&context_, SslVersion_Auto))) {
        *error = http::Error::kTls;
        *message = "sslCreateContext failed";
        return false;
      }
      context_open_ = true;
    }
    if (options.ca_bundle_path.empty() || imported_ca_ == options.ca_bundle_path) return true;
    if (!ImportCa(options.ca_bundle_path)) {
      *error = http::Error::kTls;
      *message = "could not import " + options.ca_bundle_path;
      return false;
    }
    imported_ca_ = options.ca_bundle_path;
    return true;
  }

  /// A CA PEM for a self-signed home server. On the heap and freed straight
  /// after: it is a real cost this process pays, and not one it has to keep
  /// paying (docs/DEVELOPMENT.md).
  bool ImportCa(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    // A PEM this big is a mistake rather than a CA bundle, and
    // `ImportServerPki` takes a u32 size.
    if (size <= 0 || size > 256 * 1024) {
      std::fclose(file);
      return false;
    }
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[static_cast<std::size_t>(size)]);
    if (buffer == nullptr) {
      std::fclose(file);
      return false;
    }
    const std::size_t got = std::fread(buffer.get(), 1, static_cast<std::size_t>(size), file);
    std::fclose(file);
    if (got != static_cast<std::size_t>(size)) return false;
    return R_SUCCEEDED(sslContextImportServerPki(&context_, buffer.get(),
                                                 static_cast<u32>(size),
                                                 SslCertificateFormat_Pem, nullptr));
  }

  /// What [16.0.0+] is told the service's own I/O ceiling is. Longer than any
  /// slice `http_wire.cpp` asks for, because it is the backstop and not the
  /// timeout: the caller's stall budget is what actually ends a dead transfer.
  static constexpr u32 kIoTimeoutMs = 30'000;

  std::mutex mutex_;
  SslContext context_{};
  bool context_open_ = false;
  std::string imported_ca_;
};

/// The client `main.cpp` installs: the framing, the connector it borrows, and
/// the ownership of both in one object -- `MakeWireHttpClient` borrows the
/// connector, so something has to outlive it.
class HorizonHttpClient final : public http::HttpClient {
 public:
  explicit HorizonHttpClient(const http::ClientOptions& options)
      : client_(MakeWireHttpClient(connector_, options)) {}

  http::Result Send(const http::Request& request) override { return client_->Send(request); }

  http::Result Download(const http::Request& request,
                        const http::DownloadTarget& target) override {
    return client_->Download(request, target);
  }

 private:
  SslConnector connector_;
  std::unique_ptr<http::HttpClient> client_;
};

}  // namespace

Result NetworkInitialize(const SocketBudget& budget) {
  // Each half is brought up only if it is not already, so a second call after a
  // half-failed first one retries the half that failed rather than reporting
  // success because the other one is up.
  if (!g_socket_up) {
    const SocketInitConfig config = {
        .tcp_tx_buf_size = budget.tcp_tx_buf_size,
        .tcp_rx_buf_size = budget.tcp_rx_buf_size,
        .tcp_tx_buf_max_size = budget.tcp_tx_buf_max_size,
        .tcp_rx_buf_max_size = budget.tcp_rx_buf_max_size,
        .udp_tx_buf_size = budget.udp_tx_buf_size,
        .udp_rx_buf_size = budget.udp_rx_buf_size,
        .sb_efficiency = budget.sb_efficiency,
        .num_bsd_sessions = budget.num_bsd_sessions,
        .bsd_service_type = BsdServiceType_User,
    };
    const Result rc = socketInitialize(&config);
    if (R_FAILED(rc)) return rc;
    g_socket_up = true;
  }

  // Diagnostics rather than transport, and a failure here is not one: the
  // console can still reach a server, it just cannot be asked first whether it
  // is worth trying (`ConsoleIsOnline`).
  if (!g_nifm_up) {
    g_nifm_up = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  }

  if (!g_ssl_up) {
    const Result rc = sslInitialize(kSslSessions);
    if (R_FAILED(rc)) return rc;
    g_ssl_up = true;
  }
  return 0;
}

void NetworkExit() {
  if (g_ssl_up) {
    sslExit();
    g_ssl_up = false;
  }
  if (g_nifm_up) {
    nifmExit();
    g_nifm_up = false;
  }
  if (g_socket_up) {
    socketExit();
    g_socket_up = false;
  }
}

bool NetworkReady() { return g_socket_up && g_ssl_up; }

std::unique_ptr<http::HttpClient> MakeHorizonHttpClient(const http::ClientOptions& options) {
  return std::unique_ptr<http::HttpClient>(new HorizonHttpClient(options));
}

}  // namespace rommsync::sysmodule
