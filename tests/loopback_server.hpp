// A loopback TCP server the HttpClient scenarios can be held to directly.
//
// **This is not a mock RomM, and the distinction is the one docs/TESTING.md
// draws.** Every scenario that can be run against the real docker RomM is, and
// the ones that need a failure a healthy RomM will not produce on demand use the
// fault proxy. This is the third case: two behaviours that are about the bytes
// this client puts on the wire, which no server can be asked to report --
//
//   * where a redirect sends the caller's `Authorization` header, which needs a
//     *second origin* to observe, and RomM is one origin;
//   * whether a multipart body ends inside its own `Content-Length`, which needs
//     a reader that counts bytes rather than one that answers 200.
//
// It speaks the smallest HTTP that lets those be asserted and nothing more: it
// never parses a body it was not told the length of, never keeps a connection
// alive, and answers exactly what the scenario hands it.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

namespace rig {

/// Read up to and including the blank line that ends a request head. Returns
/// what came before it, without the terminator; empty when the peer closed
/// first.
inline std::string ReadRequestHead(int fd) {
  std::string head;
  char byte = 0;
  while (head.find("\r\n\r\n") == std::string::npos) {
    const ssize_t got = ::recv(fd, &byte, 1, 0);
    if (got <= 0) return head;
    head.push_back(byte);
    if (head.size() > 64 * 1024) break;
  }
  const std::size_t end = head.find("\r\n\r\n");
  return end == std::string::npos ? head : head.substr(0, end);
}

/// `Content-Length` out of a request head, or 0.
inline std::uint64_t ContentLengthOf(const std::string& head) {
  std::string lowered;
  lowered.reserve(head.size());
  for (const char c : head) {
    lowered.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
  }
  const std::size_t at = lowered.find("content-length:");
  if (at == std::string::npos) return 0;
  std::uint64_t value = 0;
  for (std::size_t i = at + std::strlen("content-length:"); i < head.size(); ++i) {
    const char c = head[i];
    if (c == ' ' || c == '\t') continue;
    if (c < '0' || c > '9') break;
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return value;
}

/// Read exactly `count` bytes, `chunk` at a time with `pause` between reads.
///
/// The pause is what makes a request body outlive the moment it was framed: a
/// client that has written its whole body into the socket buffer before the
/// server reads any of it gives a test no window to change anything in.
inline std::string ReadExactly(int fd, std::uint64_t count, std::size_t chunk,
                               std::chrono::milliseconds pause) {
  std::string body;
  char buffer[8192];
  while (body.size() < count) {
    const std::size_t want =
        std::min<std::size_t>({chunk, sizeof(buffer), static_cast<std::size_t>(count) - body.size()});
    const ssize_t got = ::recv(fd, buffer, want, 0);
    if (got <= 0) break;
    body.append(buffer, static_cast<std::size_t>(got));
    if (pause.count() > 0) std::this_thread::sleep_for(pause);
  }
  return body;
}

inline void WriteAll(int fd, const std::string& text) {
  std::size_t at = 0;
  while (at < text.size()) {
    const ssize_t sent = ::send(fd, text.data() + at, text.size() - at, 0);
    if (sent <= 0) return;
    at += static_cast<std::size_t>(sent);
  }
}

/// Serves a fixed number of connections on 127.0.0.1, one at a time, on a
/// thread of its own. Bound to port 0, so three worktrees running this at once
/// never collide.
class LoopbackServer {
 public:
  /// What one accepted connection does. `index` counts from zero, so a handler
  /// can answer the first request differently from the second -- which is how a
  /// redirect back to the same origin is served, since every request this client
  /// makes is `Connection: close`.
  using Handler = std::function<void(int fd, std::size_t index, const std::string& head)>;

  ~LoopbackServer() { Stop(); }

  bool Start(std::size_t connections, Handler handler) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) return false;
    const int reuse = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener_, 4) != 0) {
      Stop();
      return false;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      Stop();
      return false;
    }
    port_ = ntohs(address.sin_port);

    worker_ = std::thread([this, connections, handler] {
      for (std::size_t index = 0; index < connections; ++index) {
        const int fd = ::accept(listener_, nullptr, nullptr);
        if (fd < 0) return;
        handler(fd, index, ReadRequestHead(fd));
        ::close(fd);
      }
    });
    return true;
  }

  std::uint16_t port() const { return port_; }

  std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }

  void Stop() {
    if (listener_ >= 0) {
      ::shutdown(listener_, SHUT_RDWR);
      ::close(listener_);
      listener_ = -1;
    }
    if (worker_.joinable()) worker_.join();
  }

 private:
  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::thread worker_;
};

}  // namespace rig
