// One HTTPS GET over the Horizon `ssl` system service, instrumented.
//
// This is the M0-1 question in code: can a process with a sysmodule-sized heap
// do TLS by borrowing the OS's TLS stack instead of carrying its own? The shape
// of the answer is here -- `ssl` never gives us a socket, it *takes* one, so the
// sequence is bsd first (socket, connect), then ssl (context, connection,
// SetSocketDescriptor, DoHandshake, Read/Write). What that costs is measured at
// each step rather than estimated, because the estimate is the thing under test.
//
// Nothing in this file is the eventual sysmodule backend. It is the experiment
// that says whether writing one is worth doing; the real `HttpClient` backend is
// M8 work and goes behind core/include/rommsync/http.hpp like every other
// transport (docs/DEVELOPMENT.md#tls-in-a-sysmodule).

#include <arpa/inet.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "probe.hpp"

namespace tlsprobe {
namespace {

u64 NowMs() { return armTicksToNs(armGetSystemTick()) / 1'000'000ULL; }

/// Everything that has to be undone, undone in the right order even when a
/// stage fails. `ssl` is picky here: the sockfd handed back by
/// SetSocketDescriptor must be closed *before* the connection object is, and
/// the connection before the context.
struct Session {
  bool socket_up = false;
  bool ssl_up = false;
  bool context_open = false;
  bool connection_open = false;
  int sockfd = -1;      ///< ours, until ssl takes it
  int ssl_sockfd = -1;  ///< what ssl handed back, ours to close

  ~Session() {
    if (ssl_sockfd >= 0) close(ssl_sockfd);
    if (connection_open) sslConnectionClose(&connection);
    if (context_open) sslContextClose(&context);
    if (ssl_up) sslExit();
    // Only close our own fd if ssl never took it: after
    // SetSocketDescriptor succeeds, ssl owns that descriptor.
    if (sockfd >= 0) close(sockfd);
    if (socket_up) socketExit();
  }

  SslContext context{};
  SslConnection connection{};
};

void Fail(Report& report, const char* where, Result rc) {
  report.ok = false;
  report.failed_at = where;
  report.rc = rc;
}

/// Read a PEM into a heap buffer. Deliberately on the heap and deliberately
/// freed straight after the import: a CA bundle is a real cost a sysmodule
/// would pay, and one it does not have to keep paying.
bool ImportCa(const char* path, SslContext* context, Report& report) {
  FILE* file = fopen(path, "rb");
  if (file == nullptr) {
    Fail(report, "fopen(ca_pem)", 0);
    return false;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  // A PEM this big is a mistake, not a CA bundle, and ImportServerPki takes a
  // u32 size.
  if (size <= 0 || size > 256 * 1024) {
    fclose(file);
    Fail(report, "ca_pem size", 0);
    return false;
  }
  char* buffer = new (std::nothrow) char[static_cast<size_t>(size)];
  if (buffer == nullptr) {
    fclose(file);
    Fail(report, "ca_pem alloc", 0);
    return false;
  }
  const size_t got = fread(buffer, 1, static_cast<size_t>(size), file);
  fclose(file);

  Result rc = 0;
  if (got != static_cast<size_t>(size)) {
    Fail(report, "fread(ca_pem)", 0);
  } else {
    rc = sslContextImportServerPki(context, buffer, static_cast<u32>(size),
                                   SslCertificateFormat_Pem, nullptr);
    if (R_FAILED(rc)) Fail(report, "sslContextImportServerPki", rc);
  }
  delete[] buffer;
  return R_SUCCEEDED(rc) && report.failed_at[0] == '\0';
}

/// connect() with a deadline. The blocking connect() a probe would otherwise
/// write can hang for the stack's own timeout, and "never block boot" is a hard
/// rule for the process this experiment is standing in for (CLAUDE.md).
bool ConnectWithTimeout(int fd, const sockaddr_in& addr, u32 timeout_ms, Report& report) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    Fail(report, "fcntl(O_NONBLOCK)", 0);
    return false;
  }

  int rc = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  if (rc < 0 && errno != EINPROGRESS) {
    Fail(report, "connect", 0);
    return false;
  }

  if (rc < 0) {
    pollfd pfd{fd, POLLOUT, 0};
    const int ready = poll(&pfd, 1, static_cast<int>(timeout_ms));
    if (ready == 0) {
      Fail(report, "connect timed out", 0);
      return false;
    }
    if (ready < 0) {
      Fail(report, "poll(connect)", 0);
      return false;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
      Fail(report, "connect refused", 0);
      return false;
    }
  }

  // Back to blocking: the ssl service reads and writes this descriptor itself,
  // and SslIoMode_Blocking is what the sysmodule backend would use.
  if (fcntl(fd, F_SETFL, flags) < 0) {
    Fail(report, "fcntl(restore)", 0);
    return false;
  }
  return true;
}

/// Resolve `host`, which is usually already an address on the test rig.
bool Resolve(const Config& config, sockaddr_in& out, Report& report) {
  std::memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  out.sin_port = htons(config.port);

  if (inet_pton(AF_INET, config.host, &out.sin_addr) == 1) return true;

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(config.host, nullptr, &hints, &result) != 0 || result == nullptr) {
    Fail(report, "getaddrinfo", 0);
    return false;
  }
  out.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
  freeaddrinfo(result);
  return true;
}

}  // namespace

const char* StageName(Stage stage) {
  switch (stage) {
    case Stage::kBaseline: return "baseline";
    case Stage::kSocket: return "socketInitialize";
    case Stage::kSslContext: return "ssl context";
    case Stage::kConnected: return "tcp connected";
    case Stage::kHandshake: return "tls handshake";
    case Stage::kPeak: return "peak while reading";
    case Stage::kTeardown: return "after teardown";
    case Stage::kCount: break;
  }
  return "?";
}

HeapSample SampleHeap() {
  HeapSample sample;
  const struct mallinfo info = mallinfo();
  sample.malloc_in_use = static_cast<size_t>(info.uordblks);
  u64 used = 0;
  if (R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) {
    sample.used_memory = used;
  }
  return sample;
}

// File-local: RunProbe() below is the entry point, and it needs this to have
// returned -- destructors and all -- before it can measure teardown.
static void RunProbeInner(const Config& config, Report& report) {
  auto record = [&report](Stage stage) {
    report.stage[static_cast<size_t>(stage)] = SampleHeap();
  };

  report.expected_bsd_tmem = ExpectedBsdTransferMemory(config.socket);
  record(Stage::kBaseline);

  const u64 started_ms = NowMs();
  Session session;

  const SocketInitConfig socket_config = {
      .tcp_tx_buf_size = config.socket.tcp_tx_buf_size,
      .tcp_rx_buf_size = config.socket.tcp_rx_buf_size,
      .tcp_tx_buf_max_size = config.socket.tcp_tx_buf_max_size,
      .tcp_rx_buf_max_size = config.socket.tcp_rx_buf_max_size,
      .udp_tx_buf_size = config.socket.udp_tx_buf_size,
      .udp_rx_buf_size = config.socket.udp_rx_buf_size,
      .sb_efficiency = config.socket.sb_efficiency,
      .num_bsd_sessions = config.socket.num_bsd_sessions,
      .bsd_service_type = BsdServiceType_User,
  };

  Result rc = socketInitialize(&socket_config);
  if (R_FAILED(rc)) {
    Fail(report, "socketInitialize", rc);
    return;
  }
  session.socket_up = true;
  record(Stage::kSocket);

  // Which address the console answered on. Cheap, and the first thing anyone
  // debugging a failed run wants to know. A failure here is not a failure of
  // the experiment: nifm is diagnostics, the transport is bsd + ssl.
  if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
    u32 address = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&address))) {
      in_addr self{};
      self.s_addr = address;
      std::snprintf(report.ip, sizeof(report.ip), "%s", inet_ntoa(self));
    }
    nifmExit();
  }

  rc = sslInitialize(1);
  if (R_FAILED(rc)) {
    Fail(report, "sslInitialize", rc);
    return;
  }
  session.ssl_up = true;

  // SslVersion_Auto is TLS 1.0-1.2; 1.3 needs [11.0.0+] and an explicit bit.
  // Auto is what a sysmodule should ask for -- it is the widest set the
  // installed firmware can honour without a version gate of our own.
  rc = sslCreateContext(&session.context, SslVersion_Auto);
  if (R_FAILED(rc)) {
    Fail(report, "sslCreateContext", rc);
    return;
  }
  session.context_open = true;

  if (config.ca_path[0] != '\0') {
    if (!ImportCa(config.ca_path, &session.context, report)) return;
    report.ca_imported = true;
  }
  record(Stage::kSslContext);

  sockaddr_in addr{};
  if (!Resolve(config, addr, report)) return;

  session.sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (session.sockfd < 0) {
    Fail(report, "socket", 0);
    return;
  }

  // The only stall protection this path has. SslIoMode_Blocking's own timeout
  // is five minutes (libnx ssl.h), which is not a timeout a sync tick can wait
  // out -- see the note in docs/DEVELOPMENT.md#tls-in-a-sysmodule.
  timeval stall{};
  stall.tv_sec = static_cast<time_t>(config.stall_timeout_ms / 1000);
  stall.tv_usec = static_cast<suseconds_t>((config.stall_timeout_ms % 1000) * 1000);
  setsockopt(session.sockfd, SOL_SOCKET, SO_RCVTIMEO, &stall, sizeof(stall));
  setsockopt(session.sockfd, SOL_SOCKET, SO_SNDTIMEO, &stall, sizeof(stall));

  const u64 connect_started_ms = NowMs();
  if (!ConnectWithTimeout(session.sockfd, addr, config.connect_timeout_ms, report)) return;
  report.connect_ms = NowMs() - connect_started_ms;

  rc = sslContextCreateConnection(&session.context, &session.connection);
  if (R_FAILED(rc)) {
    Fail(report, "sslContextCreateConnection", rc);
    return;
  }
  session.connection_open = true;

  // The wrapper, not sslConnectionSetSocketDescriptor: libnx keeps its own fd
  // table over the bsd service's, and the raw cmd wants the bsd-side one.
  // errno == ENOENT means "no descriptor came back", which the header says to
  // ignore rather than treat as a failure.
  const int returned = socketSslConnectionSetSocketDescriptor(&session.connection,
                                                              session.sockfd);
  if (returned < 0 && errno != ENOENT) {
    Fail(report, "socketSslConnectionSetSocketDescriptor", 0);
    return;
  }
  session.sockfd = -1;  // ssl owns it now
  session.ssl_sockfd = returned;
  record(Stage::kConnected);

  const char* sni = config.sni[0] != '\0' ? config.sni : config.host;
  // strlen + 1: libnx documents this parameter as a *buffer* size, and where it
  // means a NUL-excluding length it says so (compare sslContextAddPolicyOid,
  // "excluding NUL-terminator ... should be actual_bufsize-1"). The wrapper
  // passes the buffer straight to the service as an IPC in-buffer of exactly
  // this many bytes, so a length would hand the service an unterminated
  // hostname -- and this name is what SslVerifyOption_HostName is checked
  // against.
  rc = sslConnectionSetHostName(&session.connection, sni,
                                static_cast<u32>(std::strlen(sni) + 1));
  if (R_FAILED(rc)) {
    Fail(report, "sslConnectionSetHostName", rc);
    return;
  }

  if (config.verify) {
    rc = sslConnectionSetVerifyOption(&session.connection,
                                      SslVerifyOption_PeerCa | SslVerifyOption_HostName |
                                          SslVerifyOption_DateCheck);
  } else {
    // [5.0.0+] refuses to clear PeerCa|HostName unless this is set first. This
    // is the "deliberately self-signed home server" case docs/SECURITY.md
    // allows the user to opt into, and nothing else.
    rc = sslConnectionSetOption(&session.connection, SslOptionType_SkipDefaultVerify, true);
    if (R_SUCCEEDED(rc)) rc = sslConnectionSetVerifyOption(&session.connection, 0);
  }
  if (R_FAILED(rc)) {
    Fail(report, "sslConnectionSetVerifyOption", rc);
    return;
  }

  rc = sslConnectionSetIoMode(&session.connection, SslIoMode_Blocking);
  if (R_FAILED(rc)) {
    Fail(report, "sslConnectionSetIoMode", rc);
    return;
  }

  const u64 handshake_started_ms = NowMs();
  rc = sslConnectionDoHandshake(&session.connection, nullptr, nullptr, nullptr, 0);
  if (R_FAILED(rc)) {
    Fail(report, "sslConnectionDoHandshake", rc);
    // Why it was refused, if the service will say. The cmd clears the stored
    // value as it reads it, so this is the only chance to ask.
    const Result verify_rc = sslConnectionGetVerifyCertError(&session.connection);
    if (R_FAILED(verify_rc)) report.verify_cert_rc = verify_rc;
    return;
  }
  report.handshake_ms = NowMs() - handshake_started_ms;
  record(Stage::kHandshake);

  SslCipherInfo cipher{};
  if (R_SUCCEEDED(sslConnectionGetCipherInfo(&session.connection, &cipher))) {
    std::snprintf(report.cipher, sizeof(report.cipher), "%.*s",
                  static_cast<int>(sizeof(cipher.cipher)), cipher.cipher);
    std::snprintf(report.protocol_version, sizeof(report.protocol_version), "%.*s",
                  static_cast<int>(sizeof(cipher.protocol_version)), cipher.protocol_version);
  }

  // `Connection: close` so the body ends at EOF: this probe is measuring the
  // TLS path, not implementing HTTP, and a keep-alive response would need
  // chunked decoding to know where it stopped.
  char request[512];
  const int request_len =
      std::snprintf(request, sizeof(request),
                    "GET %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "User-Agent: rommsync-tlsprobe\r\n"
                    "Accept: */*\r\n"
                    "Connection: close\r\n\r\n",
                    config.path, sni);
  if (request_len <= 0 || request_len >= static_cast<int>(sizeof(request))) {
    Fail(report, "request too long", 0);
    return;
  }

  u32 written_total = 0;
  while (written_total < static_cast<u32>(request_len)) {
    u32 written = 0;
    rc = sslConnectionWrite(&session.connection, request + written_total,
                            static_cast<u32>(request_len) - written_total, &written);
    if (R_FAILED(rc) || written == 0) {
      Fail(report, "sslConnectionWrite", rc);
      return;
    }
    written_total += written;
  }

  // The one in-flight buffer the heap budget is really about. On the heap on
  // purpose: a sysmodule's download buffer is heap too, and a .bss array here
  // would hide the cost this whole target exists to show.
  const u32 buffer_size = config.read_buf_size != 0 ? config.read_buf_size : 4096;
  char* buffer = new (std::nothrow) char[buffer_size];
  if (buffer == nullptr) {
    Fail(report, "read buffer alloc", 0);
    return;
  }

  HeapSample peak = SampleHeap();
  bool headers_done = false;
  bool status_done = false;  // the response line has ended; stop copying it
  size_t status_len = 0;
  char status_line[64] = {};
  u32 crlf_state = 0;  // how much of "\r\n\r\n" has matched so far

  for (;;) {
    u32 got = 0;
    rc = sslConnectionRead(&session.connection, buffer, buffer_size, &got);
    if (R_FAILED(rc)) {
      // A server closing after `Connection: close` is the expected end, and
      // some firmware reports that as a failed Read rather than a zero one.
      report.read_end_rc = rc;
      break;
    }
    if (got == 0) break;

    const HeapSample now = SampleHeap();
    if (now.malloc_in_use > peak.malloc_in_use) peak = now;

    for (u32 i = 0; i < got; ++i) {
      const char c = buffer[i];
      if (headers_done) {
        ++report.body_bytes;
        continue;
      }
      ++report.header_bytes;
      // The response line only, not the first 64 bytes of headers: without the
      // stop, a short status line runs straight on into `Server: nginx` and the
      // recorded line is a splice of two.
      if (!status_done) {
        if (c == '\r' || c == '\n') {
          status_done = true;
        } else if (status_len + 1 < sizeof(status_line)) {
          status_line[status_len++] = c;
        }
      }
      // "\r\n\r\n" ends the headers. Tracked as a state machine because it can
      // straddle two reads.
      if ((crlf_state == 0 && c == '\r') || (crlf_state == 2 && c == '\r')) {
        ++crlf_state;
      } else if ((crlf_state == 1 || crlf_state == 3) && c == '\n') {
        ++crlf_state;
        if (crlf_state == 4) headers_done = true;
      } else {
        crlf_state = 0;
      }
    }
  }

  delete[] buffer;
  report.stage[static_cast<size_t>(Stage::kPeak)] = peak;

  // "HTTP/1.1 200 OK" -> 200.
  if (std::strncmp(status_line, "HTTP/", 5) == 0) {
    const char* space = std::strchr(status_line, ' ');
    if (space != nullptr) report.status = std::atoi(space + 1);
  }

  report.total_ms = NowMs() - started_ms;
  report.ok = report.status != 0;
  if (!report.ok) Fail(report, "no HTTP status in the response", report.read_end_rc);
}

void RunProbe(const Config& config, Report& report) {
  // The inner scope is what makes the teardown number mean anything: `Session`
  // closes the connection, the context, ssl and the socket driver in its
  // destructor, so the sample has to be taken after that scope ends. A teardown
  // figure that has not returned to baseline is a leak a resident sysmodule
  // would pay for on every sync tick, which is the failure this stage exists to
  // catch.
  { RunProbeInner(config, report); }
  report.stage[static_cast<size_t>(Stage::kTeardown)] = SampleHeap();
}

}  // namespace tlsprobe
