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

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "http_wire.hpp"
#include "nifm_verdict.hpp"
#include "posix_connection.hpp"
#include "rommsync/config.hpp"
#include "rommsync/log.hpp"

namespace rommsync::sysmodule {
namespace {

/// Process-wide, because `socketInitialize` and `sslInitialize` are: both take
/// an `sm` session, and a sysmodule has one only inside `__appInit`
/// (`ipc/server.hpp` says the same thing about its port).
///
/// Atomic since #333: a transport that did not come up in `__appInit` is
/// brought up later by the worker, while the IPC and pairing threads read
/// these to decide whether to connect. Only `__appInit`, the worker and
/// `__appExit` write them, never two at once -- the worker is joined before
/// `main` returns -- so a flag is the whole of the guard: each is set only
/// after its service is up.
std::atomic<bool> g_socket_up{false};
std::atomic<bool> g_ssl_up{false};
std::atomic<bool> g_nifm_up{false};
// The worker writes this one while the IPC and pairing threads connect.
std::atomic<bool> g_romm_trusted{false};

/// `[server] ca_file` as the engine last handed it over (`TrustServerCa`,
/// #288). Process-wide for the reason the flags above are: there is one
/// `SslContext`, made by the one client `main.cpp` builds. Its own lock, not
/// the connector's, so the IPC thread recording a path never waits behind a
/// request importing one.
std::mutex g_trusted_ca_mutex;
std::string g_trusted_ca;

std::string TrustedCa() {
  std::lock_guard<std::mutex> lock(g_trusted_ca_mutex);
  return g_trusted_ca;
}

/// `ssl`'s ResultIoWouldBlock (module 123, description 204): a non-blocking
/// `DoHandshake` that has sent what it can and is waiting on the server
/// (switchbrew, Error codes). libnx names no constant for it.
constexpr Result kSslResultWouldBlock = MAKERESULT(123, 204);

void CancelLookup(void* handle) {
  resolverCancel(static_cast<u32>(reinterpret_cast<std::uintptr_t>(handle)));
}

/// `getaddrinfo` with a way out (M11-6, #290). On the console a lookup is one
/// IPC to `sfdnsres` that returns when the resolver does, which no slice on
/// this thread can shorten -- so the handle `sfdnsres` cancels by is taken
/// first and handed to the token, and a sleep that fires the token cancels the
/// lookup from the quiescing thread. `ResolveBlocking` reads the token before
/// it asks, which covers a cancel that landed before the hook was armed.
///
/// **What no console has confirmed** (#43's list): that a handle taken before
/// the call cancels the call, which is what libnx documents
/// (`resolverGetCancelHandle`, "the next resolver command on the current
/// thread"), and that a cancel landing between the check and the IPC is not
/// lost. A zero handle -- the request for one failed -- arms nothing, and the
/// lookup is as cancellable as it was before #290: not at all.
bool ResolveCancellable(const std::string& host, std::uint32_t* address,
                        const http::CancelToken* cancel) {
  const u32 handle = resolverGetCancelHandle();
  const http::CancelToken::Abort abort(
      handle != 0 ? cancel : nullptr, CancelLookup,
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle)));
  return ResolveBlocking(host, address, cancel);
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
                                   const http::ClientOptions& options,
                                   const http::CancelToken* cancel, http::Error* error,
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
    // nifm's "no internet" is let past only once RomM has answered the engine's
    // heartbeat through it (#328, `TrustRommPastNifm`): a console that blocks
    // Nintendo reaches RomM on its LAN, but a captive portal would answer a
    // drain or a list page too, and be handed the token for it.
    const NifmVerdict nifm = ConsoleNetwork();
    if (nifm == NifmVerdict::kOffline ||
        (nifm != NifmVerdict::kOnline && !g_romm_trusted.load())) {
      *error = http::Error::kConnectFailed;
      *message = nifm == NifmVerdict::kOffline ? "the console has no network connection"
                                               : "the console has no internet connection";
      return nullptr;
    }

    // The handshake's own bound, and the socket's. Deliberately the connect
    // budget: a handshake that has not finished in as long as a connection took
    // to make is one that is not going to.
    const int fd = ConnectTo(origin, connect_timeout, connect_timeout, error, message, cancel,
                             ResolveCancellable);
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
    return OpenTls(fd, origin, connect_timeout, options, cancel, error, message);
  }

 private:
  /// The `ssl` half. `fd` is still ours until
  /// `socketSslConnectionSetSocketDescriptor` succeeds; after that the service
  /// owns it and the caller must not close it.
  std::unique_ptr<Connection> OpenTls(int fd, const Origin& origin,
                                      std::chrono::milliseconds handshake_timeout,
                                      const http::ClientOptions& options,
                                      const http::CancelToken* cancel, http::Error* error,
                                      std::string* message) {
    // One context for the process, so a CA import is paid once rather than per
    // request -- and guarded, because a context is one `ssl` session and two
    // threads opening a connection at the same time would be two commands on it.
    // **Only the shared `SslContext` needs the lock**, and it is released
    // before the handshake. Holding it across `DoHandshake` would mean a
    // download's handshake to an unresponsive server blocking a sync tick's
    // `Send` inside `Connector::Open` -- waiting on a lock, where neither the
    // `CancelToken` nor the `Budget` is read -- for the whole of another
    // request's handshake. A shared client has to be safe for exactly that
    // (`http.hpp`), and a sleep has to be able to end it (#290).
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
    // does not, a failed handshake leaks a socket: a slot in bsd's table and
    // newlib's, not a kernel handle, so it is bsd's limit it runs into and not
    // the npdm's `handle_table_size` (`handle_budget.hpp`, #210). It is on #43's
    // gate list, because it is answerable on a console and nowhere else; closing
    // `fd` here on a guess would be a double close of a descriptor another
    // process is reading.
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

    // **The handshake non-blocking, in slices** (M11-6, #290). One blocking
    // `DoHandshake` waits for `SO_RCVTIMEO` -- the connect budget, ten seconds
    // -- or the service's own I/O ceiling, and nothing a sleep does can end it:
    // the descriptor is the service's now, so there is no socket left here to
    // shut down under it. Non-blocking, each try returns `ResultIoWouldBlock`
    // until the server has answered, and the wait between tries is a
    // `sslConnectionPoll` of one slice, with the token read before each.
    //
    // Read only, not Write: past the ClientHello a handshake waits on the
    // server, and a socket is nearly always writable, so polling for it would
    // spin the tries with no wait between them. A handshake that does wait to
    // write loses at most a slice per wait.
    rc = sslConnectionSetIoMode(&connection, SslIoMode_NonBlocking);
    if (R_FAILED(rc)) return fail(http::Error::kTls, "sslConnectionSetIoMode failed");
    const Sliced shook = RunInSlices(
        [&connection, &rc](std::chrono::milliseconds slice) {
          rc = sslConnectionDoHandshake(&connection, nullptr, nullptr, nullptr, 0);
          if (R_SUCCEEDED(rc)) return Step::kDone;
          if (rc != kSslResultWouldBlock) return Step::kFailed;
          // A poll that fails is the end, not another try: it fails at once,
          // so retrying would spin `DoHandshake` with no wait until the timeout.
          u32 signalled = 0;
          const Result polled =
              sslConnectionPoll(&connection, SslPollEvent_Read | SslPollEvent_Except, &signalled,
                                static_cast<u32>(slice.count()));
          return R_FAILED(polled) ? Step::kFailed : Step::kPending;
        },
        cancel, handshake_timeout);
    if (shook == Sliced::kCanceled) {
      return fail(http::Error::kCanceled, "canceled during the TLS handshake");
    }
    if (shook == Sliced::kTimedOut) {
      // `kTls`, as a handshake that outlived the socket's timeout always was:
      // what a caller makes of the error is not this change's to move.
      return fail(http::Error::kTls, "the TLS handshake did not finish in time");
    }
    if (shook == Sliced::kFailed) {
      // Why it was refused, if the service will say. The cmd clears the stored
      // value as it reads it, so this is the only chance to ask -- and "the
      // handshake failed" and "the certificate was rejected, for this reason"
      // are the two findings a self-signed home server is told apart by.
      const Result verify = sslConnectionGetVerifyCertError(&connection);
      return fail(http::Error::kTls, R_FAILED(verify) ? "the server's certificate was refused"
                                                      : "the TLS handshake failed");
    }

    // Then blocking, with the wait bounded from above: every read and write
    // goes through `sslConnectionPoll` with the caller's slice first
    // (`SslStream`), and on [16.0.0+] the service's own ceiling comes down from
    // five minutes to something a sync tick can wait out.
    rc = sslConnectionSetIoMode(&connection, SslIoMode_Blocking);
    if (R_FAILED(rc)) return fail(http::Error::kTls, "sslConnectionSetIoMode failed");
    if (hosversionAtLeast(16, 0, 0)) {
      sslConnectionSetIoTimeout(&connection, kIoTimeoutMs);
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
    // The client's own option first, then what `config.ini` names. A path that
    // changes is imported on the next request; one that is cleared or replaced
    // stays imported until the process restarts, since a context cannot forget
    // a CA and connections may still be open on this one.
    const std::string wanted =
        options.ca_bundle_path.empty() ? TrustedCa() : options.ca_bundle_path;
    if (wanted.empty() || imported_ca_ == wanted) return true;
    if (!ImportCa(wanted)) {
      *error = http::Error::kTls;
      *message = "could not import the CA file " + wanted;
      return false;
    }
    imported_ca_ = wanted;
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
    // `ImportServerPki` takes a u32 size. The bound `LoadConfig` checks the
    // file against, so the settings screen and this agree.
    if (size <= 0 || static_cast<unsigned long>(size) > config::kMaxCaFileBytes) {
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
  //
  // nifm is diagnostics rather than transport, and a failure here is not one:
  // the console can still reach a server, it just cannot be asked first whether
  // it is worth trying (`ConsoleNetwork`).
  //
  // **It is first, and that is not the arbitrary order it looks like.** It is
  // the only half whose failure is not returned, and since M9-1 (#195) it is
  // also the only `nifmInitialize` in this build -- `main.cpp` had a second one,
  // whose `NifmServiceType` libnx silently ignored because the call refcounts.
  // Bringing it up after `socketInitialize` would mean a console whose transfer
  // memory did not fit lost `ConsoleNetwork()`'s real answer as well as its
  // transport, and a connection probe is exactly what such a console still wants
  // to be able to give.
  if (!g_nifm_up) {
    g_nifm_up = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  }

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

bool SocketsReady() { return g_socket_up; }

NifmVerdict ConsoleNetwork() {
  if (!g_nifm_up) return NifmVerdict::kOnline;
  NifmInternetConnectionType type{};
  u32 strength = 0;
  NifmInternetConnectionStatus status{};
  const Result rc = nifmGetInternetConnectionStatus(&type, &strength, &status);
  // **0xd46ed is offline** (M11-5, #289): libnx documents this call failing
  // with it in airplane mode and with no known network in range, so reading a
  // failure as "nifm was busy" skipped the boot wait and the connector's
  // pre-check on exactly the consoles they exist for. Anything else short of
  // `Connected` may be a console that blocks Nintendo and reaches RomM fine
  // (#328, `nifm_verdict.hpp`).
  const NifmVerdict verdict =
      ClassifyNifm(R_FAILED(rc) ? rc : 0, status == NifmInternetConnectionStatus_Connected);
  if (verdict != NifmVerdict::kUnexpected) return verdict;
  // Said once per change of code rather than once per two-second probe: the
  // worker asks every poll while a tick is held.
  static std::atomic<Result> last_said{0};
  if (last_said.exchange(rc) != rc) {
    // `std::to_chars` rather than `snprintf`, which would link newlib's whole
    // `_svfprintf_r` into the sysmodule (`switch.builds`).
    char hex[16];
    const std::to_chars_result end =
        std::to_chars(hex, hex + sizeof(hex), static_cast<std::uint32_t>(rc), 16);
    log::Warn(log::Event::kNetOffline, "nifm answered 0x" + std::string(hex, end.ptr) +
                                           "; read as no internet connection");
  }
  return verdict;
}

void TrustRommPastNifm(bool trusted) { g_romm_trusted.store(trusted); }

void TrustServerCa(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_trusted_ca_mutex);
  g_trusted_ca = path;
}

std::unique_ptr<http::HttpClient> MakeHorizonHttpClient(const http::ClientOptions& options) {
  return std::unique_ptr<http::HttpClient>(new HorizonHttpClient(options));
}

}  // namespace rommsync::sysmodule
