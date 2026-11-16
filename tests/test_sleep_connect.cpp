// A console put to sleep while a request is still opening its connection
// (M11-6, #290).
//
// `SdEngine::Quiesce` waits `kQuiesceBudget` for the worker to come back and
// then acknowledges PSC whatever happened, because a module that never answers
// is a console that freezes (#208). What that wait could not reach was
// `Connector::Open`: the name lookup, the connect and the TLS handshake looked
// at no cancel token, so a sleep that landed in one of them was acknowledged
// three seconds later with a bsd, `sfdnsres` or `ssl` call still in flight --
// the "services in use across SleepReady" crash #208 is about.
//
// One scenario per phase, each through the shipping `http_wire.cpp` and, for
// the two phases a laptop can reach, the shipping `posix_connection.cpp`:
//
//   dns       -- the lookup blocks the way the console's `sfdnsres` request does,
//                until something cancels it from another thread
//   connect   -- a real connect to a listener whose backlog is full, so the SYN
//                is never answered
//   handshake -- a real connection to a server that never answers the hello,
//                waited on in the same slices `SslConnector` waits its handshake
//
// In each, the sleep is delivered through `power::Watcher` from
// `power_fake.hpp`, as PSC delivers it, and what is asserted is the issue's
// goal: the acknowledgement goes out well inside the budget, with no open in
// flight, and without the `power.sleep` line that says it went out early.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.hpp"
#include "harness.hpp"
#include "http/http_wire.hpp"
#include "http/posix_connection.hpp"
#include "power.hpp"
#include "power_fake.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/log.hpp"
#include "rommsync/token_store.hpp"

namespace auth = rommsync::auth;
namespace http = rommsync::http;
namespace power = rommsync::sysmodule::power;
namespace rlog = rommsync::log;
namespace sysmodule = rommsync::sysmodule;

namespace {

/// Comfortably inside `kQuiesceBudget`, and far from it: a cancelled phase
/// ends within one slice, and a phase that ignores the cancel runs the whole
/// three seconds out.
constexpr std::chrono::milliseconds kWellInside{1'500};

/// Where an open is, as the test watches it: entered, and how many are still
/// in flight. The test sleeps the console only once one is inside its phase.
class Phase {
 public:
  void Enter() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++inside_;
    ++entered_;
    changed_.notify_all();
  }

  void Leave() {
    std::lock_guard<std::mutex> lock(mutex_);
    --inside_;
    changed_.notify_all();
  }

  bool AwaitEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds{20}, [this] { return entered_ > 0; });
  }

  int inside() {
    std::lock_guard<std::mutex> lock(mutex_);
    return inside_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  int inside_ = 0;
  int entered_ = 0;
};

/// Any `Connector`, with `Phase` kept around its `Open`.
class Watched final : public sysmodule::Connector {
 public:
  Watched(sysmodule::Connector& inner, Phase& phase) : inner_(inner), phase_(phase) {}

  std::unique_ptr<sysmodule::Connection> Open(const sysmodule::Origin& origin,
                                              std::chrono::milliseconds connect_timeout,
                                              const http::ClientOptions& options,
                                              const http::CancelToken* cancel, http::Error* error,
                                              std::string* message) override {
    phase_.Enter();
    std::unique_ptr<sysmodule::Connection> opened =
        inner_.Open(origin, connect_timeout, options, cancel, error, message);
    phase_.Leave();
    return opened;
  }

 private:
  sysmodule::Connector& inner_;
  Phase& phase_;
};

// --- dns ---------------------------------------------------------------------

/// The console's lookup, minus `sfdnsres`: it blocks until it is cancelled from
/// another thread, and only through the token's `Abort` -- there is nothing to
/// poll inside one IPC. Ten seconds is `http::kDefaultConnectTimeout`, the
/// longest a request would have waited.
struct Lookup {
  std::mutex mutex;
  std::condition_variable aborted_cv;
  bool aborted = false;
};
Lookup g_lookup;

void AbortLookup(void*) {
  std::lock_guard<std::mutex> lock(g_lookup.mutex);
  g_lookup.aborted = true;
  g_lookup.aborted_cv.notify_all();
}

bool BlockingLookup(const std::string&, std::uint32_t*, const http::CancelToken* cancel) {
  const http::CancelToken::Abort abort(cancel, AbortLookup, nullptr);
  std::unique_lock<std::mutex> lock(g_lookup.mutex);
  if (cancel != nullptr && cancel->canceled()) return false;
  g_lookup.aborted_cv.wait_for(lock, http::kDefaultConnectTimeout,
                               [] { return g_lookup.aborted; });
  return false;
}

// --- connect -----------------------------------------------------------------

/// A loopback listener nobody accepts from, and the connections that filled
/// its backlog. Past the backlog the kernel drops a SYN rather than answering
/// it, so a further connect neither completes nor fails: it waits, exactly as
/// one to a server behind a dead link does.
class FullBacklog {
 public:
  ~FullBacklog() {
    for (const int fd : clients_) close(fd);
    if (listener_ >= 0) close(listener_);
  }

  bool Fill() {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    if (listener_ < 0 || bind(listener_, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
        listen(listener_, 0) != 0 ||
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      return false;
    }
    port_ = ntohs(address.sin_port);
    for (int attempt = 0; attempt < 512; ++attempt) {
      const int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return false;
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
      const int rc = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
      if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        return false;
      }
      pollfd descriptor{fd, POLLOUT, 0};
      if (rc != 0 && poll(&descriptor, 1, 300) == 0) {
        // This one hangs, so the next one will too.
        close(fd);
        return true;
      }
      clients_.push_back(fd);
    }
    return false;
  }

  std::uint16_t port() const { return port_; }

 private:
  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::vector<int> clients_;
};

// --- handshake ---------------------------------------------------------------

/// A loopback listener that takes the connection and never says a word, so a
/// client waiting for the server's hello waits for ever.
class Silent {
 public:
  ~Silent() {
    if (listener_ >= 0) close(listener_);
  }

  bool Listen() {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    if (listener_ < 0 || bind(listener_, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
        listen(listener_, 8) != 0 ||
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      return false;
    }
    port_ = ntohs(address.sin_port);
    return true;
  }

  std::uint16_t port() const { return port_; }

 private:
  int listener_ = -1;
  std::uint16_t port_ = 0;
};

/// `SslConnector::Open`'s shape with the `ssl` service taken out: the shipping
/// `ConnectTo`, then a wait for the server's first bytes standing in for
/// `sslConnectionDoHandshake`.
class Handshaking final : public sysmodule::Connector {
 public:
  std::unique_ptr<sysmodule::Connection> Open(const sysmodule::Origin& origin,
                                              std::chrono::milliseconds connect_timeout,
                                              const http::ClientOptions&,
                                              const http::CancelToken* cancel, http::Error* error,
                                              std::string* message) override {
    const int fd =
        sysmodule::ConnectTo(origin, connect_timeout, connect_timeout, error, message, cancel);
    if (fd < 0) return nullptr;
    // `SslConnector`'s handshake loop: one non-blocking try, then a wait for
    // the server of at most a slice, in `RunInSlices` -- with a poll of the
    // socket standing in for `sslConnectionPoll`, and a server that never
    // answers standing in for one that is slow to.
    const sysmodule::Sliced shook = sysmodule::RunInSlices(
        [fd](std::chrono::milliseconds slice) {
          pollfd descriptor{fd, POLLIN, 0};
          return poll(&descriptor, 1, static_cast<int>(slice.count())) > 0
                     ? sysmodule::Step::kFailed
                     : sysmodule::Step::kPending;
        },
        cancel, connect_timeout);
    close(fd);
    *error = shook == sysmodule::Sliced::kCanceled ? http::Error::kCanceled : http::Error::kTls;
    *message = "the TLS handshake did not finish";
    return nullptr;
  }
};

// --- the console ---------------------------------------------------------------

/// An engine paired with `url`, ticking through `connector`, put to sleep once
/// an open is inside its phase.
void SleepsDuringOpen(checks::Checks& c, const char* name, const std::string& url,
                      sysmodule::Connector& inner) {
  rlog::Reset();
  Phase phase;
  Watched connector(inner, phase);
  const std::unique_ptr<http::HttpClient> client = sysmodule::MakeWireHttpClient(connector);

  harness::Sandbox sandbox(c, name);
  c.Expect(sandbox.Write("/config/rommsync/config.ini",
                         "[server]\nurl = " + url +
                             "\n\n[sync]\nenabled = true\ninterval_min = 0\non_boot = false\n"),
           "a configured card");
  const std::string directory = sandbox.Host(harness::kConfigDir) + "/";
  auth::StoredToken token;
  token.server_url = url;
  token.access_token = "not-a-real-token";
  token.device_id = std::string("console-") + name;
  c.Expect(auth::SaveToken(directory + auth::kTokenFileName, token).ok(), "and a paired one");

  const std::unique_ptr<rommsync::fs::FileSystem> card =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  sysmodule::SdEngine engine;
  engine.Load(directory);
  engine.UseServer(client.get(), token.access_token);
  engine.UseCard(card.get());
  engine.StartWorker();
  c.Expect(engine.RequestSync(), "a tick is asked for");
  c.Expect(phase.AwaitEntered(), "and its first request is opening a connection");
  // Into the blocking call itself, not merely past `Enter`.
  std::this_thread::sleep_for(std::chrono::milliseconds{300});

  power_fake::Scripted psc;
  power_fake::Running watching(psc, engine);
  const std::chrono::steady_clock::time_point asked = std::chrono::steady_clock::now();
  psc.Deliver(power::State::kSleepReady);
  const bool acked = psc.AwaitAcks(1, std::chrono::seconds{20});
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - asked);
  const int still_inside = phase.inside();

  c.Expect(acked, "the sleep is acknowledged");
  c.Expect(took < kWellInside, std::string("well inside the ") +
                                   std::to_string(sysmodule::kQuiesceBudget.count()) +
                                   "ms budget, because the open ended at the cancel: took " +
                                   std::to_string(took.count()) + "ms");
  c.ExpectEq(still_inside, 0,
             "and no open is in flight when it goes out -- a bsd, sfdnsres or ssl call "
             "across SleepReady is the crash #208 is about");

  std::vector<rlog::Line> tail;
  rlog::Tail(rlog::kTailLines, &tail);
  std::string rendered;
  bool late = false;
  for (const rlog::Line& line : tail) {
    rendered += "\n    " + line.text;
    late = late || line.text.find(" power.sleep") != std::string::npos;
  }
  c.Expect(!late, "nor is it the acknowledgement that went out early:" + rendered);
}

void Dns(checks::Checks& c) {
  sysmodule::PlainConnector plain(BlockingLookup);
  // `.invalid` never resolves anywhere (RFC 6761), so nothing here can reach a
  // real name service even if the seam were bypassed.
  SleepsDuringOpen(c, "sleep-dns", "http://romm.invalid", plain);
}

void Connect(checks::Checks& c) {
  FullBacklog backlog;
  if (!backlog.Fill()) {
    c.Expect(false, "a listener whose backlog leaves a connect hanging");
    return;
  }
  sysmodule::PlainConnector plain;
  SleepsDuringOpen(c, "sleep-connect", "http://127.0.0.1:" + std::to_string(backlog.port()),
                   plain);
}

void Handshake(checks::Checks& c) {
  Silent server;
  if (!server.Listen()) {
    c.Expect(false, "a listener to handshake with");
    return;
  }
  Handshaking connector;
  SleepsDuringOpen(c, "sleep-handshake", "http://127.0.0.1:" + std::to_string(server.port()),
                   connector);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "dns";
  checks::Checks checks;
  if (scenario == "dns") {
    Dns(checks);
  } else if (scenario == "connect") {
    Connect(checks);
  } else if (scenario == "handshake") {
    Handshake(checks);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() != 0) {
    std::cerr << scenario << ": " << checks.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << scenario << ": ok\n";
  return 0;
}
