// The one HTTP surface the engine is allowed to use.
//
// Auth, sync and downloads are written against `HttpClient` and nothing else,
// so the TLS/transport decision -- libcurl on a laptop, the Horizon `ssl`
// service in the sysmodule, something else if `ssl` proves impractical -- is a
// backend swap rather than a rewrite. That is what lets the whole engine be
// developed and proven against a real RomM on a laptop and in CI, with no
// console (docs/TESTING.md).
//
// Nothing here may name a concrete TLS or HTTP library, and nothing here may
// include a host-only or libnx header: this file is compiled for both targets.
// The .github/workflows/ci.yml `static` job enforces the first rule and the
// `switch-build` job the second.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rommsync::http {

enum class Method { kGet, kHead, kPost, kPut, kDelete };

/// Why a request never produced an HTTP response.
///
/// An HTTP error *status* is deliberately not in here. A 401 is a response the
/// server chose to send: `Result::ok()` stays true and the caller reads
/// `Response::status`. Only a failure to complete the exchange is an `Error`.
enum class Error {
  kNone,
  kInvalidRequest,   ///< malformed request; nothing was sent
  kUnresolvedHost,   ///< DNS said no
  kConnectFailed,    ///< could not reach the host
  kTls,              ///< handshake or certificate verification failed
  kTimeout,          ///< exceeded the total or stall timeout
  kCanceled,         ///< the caller's CancelToken fired
  kTruncated,        ///< the body ended before the length the server declared
  kWriteFailed,      ///< the destination file could not be written
  kTransport,        ///< anything else the backend could not classify
};

/// Stable, log-friendly name for an error. Never null.
const char* ToString(Error error);

struct Header {
  std::string name;
  std::string value;
};

using Headers = std::vector<Header>;

/// Case-insensitive header lookup, because header names are case-insensitive on
/// the wire and servers disagree about capitalisation. Returns nullptr when
/// absent; the pointer is valid until `headers` changes.
const std::string* FindHeader(const Headers& headers, std::string_view name);

/// One part of a `multipart/form-data` body.
///
/// A part with a `file_path` is streamed from disk rather than read into memory:
/// the sysmodule heap cannot hold a rom, and the same code path runs on both
/// targets, so the host backend must not get away with buffering either.
struct FormPart {
  std::string name;
  std::string value;         ///< the field value, when `file_path` is empty
  std::string file_path;     ///< stream this file as the part's content
  std::string file_name;     ///< reported filename; defaults to `file_path`'s leaf
  std::string content_type;  ///< optional; the server usually sniffs otherwise
};

/// Cooperative cancellation.
///
/// A download can take minutes, and the overlay's "stop" must not wait for it.
/// The backend polls this while transferring; a cancelled request returns
/// `Error::kCanceled` and leaves no file where a complete one is expected.
/// Safe to cancel from another thread.
class CancelToken {
 public:
  void Cancel() noexcept { canceled_.store(true, std::memory_order_relaxed); }
  bool canceled() const noexcept { return canceled_.load(std::memory_order_relaxed); }

 private:
  std::atomic<bool> canceled_{false};
};

/// Defaults chosen so a request on a sleeping Wi-Fi link fails in seconds rather
/// than hanging a sync tick. `Never block boot` is a hard rule (CLAUDE.md).
inline constexpr std::chrono::milliseconds kDefaultConnectTimeout{10'000};
inline constexpr std::chrono::milliseconds kDefaultTimeout{30'000};
inline constexpr std::chrono::milliseconds kDefaultStallTimeout{20'000};

struct Request {
  Method method = Method::kGet;
  std::string url;
  Headers headers;

  /// The raw request body -- JSON, form-urlencoded, anything. Ignored when
  /// `form` is non-empty. Only POST and PUT may carry one; a body on any other
  /// method is `Error::kInvalidRequest` rather than a silently dropped body.
  std::string body;

  /// When non-empty the body is built as `multipart/form-data` from these parts
  /// and `body` is ignored.
  std::vector<FormPart> form;

  /// Start the response at this byte offset (`Range: bytes=N-`). A server that
  /// honours it answers 206. One that ignores it answers 200 with the whole
  /// resource: `Download` then starts the file over, so the destination holds
  /// the entire resource rather than a splice, and `Response::status` is what
  /// tells the two apart.
  std::uint64_t range_start = 0;

  std::chrono::milliseconds connect_timeout = kDefaultConnectTimeout;

  /// Ceiling on the whole exchange. Right for API calls, wrong for a multi-GB
  /// rom -- set it to zero there and rely on `stall_timeout` instead.
  std::chrono::milliseconds timeout = kDefaultTimeout;

  /// Abort when no bytes have moved for this long. This is the timeout that
  /// matters for downloads: it distinguishes "slow" from "dead" without
  /// capping how long a legitimately large transfer may take.
  std::chrono::milliseconds stall_timeout = kDefaultStallTimeout;

  /// Optional, not owned; must outlive the call.
  const CancelToken* cancel = nullptr;
};

struct Response {
  int status = 0;
  Headers headers;

  /// The response body. Empty for a *successful* `Download` -- those bytes went
  /// to disk. A failed one keeps the server's explanation here instead, capped,
  /// because a 404's body says why there is no content and must never be
  /// mistaken for content.
  std::string body;

  /// Body bytes actually received. For a resumed download this counts only the
  /// bytes this call fetched, not the bytes already in the partial file, and it
  /// can exceed `body.size()` when a long error body was capped.
  std::uint64_t bytes_received = 0;

  /// What the server said the body would be: `Content-Length`, or the total
  /// from `Content-Range` on a 206. Zero means the server did not say.
  std::uint64_t declared_size = 0;
};

struct Result {
  Error error = Error::kNone;
  /// Backend detail for logs. Never contains credentials.
  std::string message;
  Response response;

  /// The exchange completed. Says nothing about the status code.
  bool ok() const { return error == Error::kNone; }

  /// The exchange completed *and* the server was happy.
  bool successful() const {
    return ok() && response.status >= 200 && response.status < 300;
  }
};

/// Where a streamed download lands.
///
/// Bytes are written to `<path>.part` and renamed onto `path` only once the
/// whole body has arrived. A failed transfer therefore never leaves a truncated
/// file where a complete one is expected -- the same reasoning as the
/// backup-before-overwrite rule in docs/SYNC_PROTOCOL.md, applied to downloads.
struct DownloadTarget {
  std::string path;

  /// Continue an interrupted download from the bytes already in `<path>.part`.
  bool resume = false;

  /// How many bytes `path` must hold to be complete: the resource size for a
  /// whole download, the slice length for a ranged one. When set, a body that
  /// ends early is `Error::kTruncated` even if the server declared no length --
  /// the only way to catch a server that closes cleanly mid-body. Zero means
  /// "trust whatever the server declares", which a `resume` cannot do: there is
  /// no way to check the seam between two halves without a total, so a resumed
  /// download against a server that declares nothing fails rather than guesses.
  std::uint64_t expected_size = 0;
};

/// Where a `Download` stages the bytes before it commits them: `<path>.part`.
///
/// Public because the staging file is not only the backend's business. A caller
/// that reports how far a download has got has to look at the same file the
/// backend is writing -- `download::QueueEntry::bytes_done` counts it, so a
/// resumed transfer's progress bar carries on instead of restarting at zero --
/// and a second spelling of the suffix inside `core/` would be a platform
/// detail smuggled past this interface (core/AGENTS.md). One name, named here.
std::string PartialPathFor(std::string_view path);

/// Backend-independent knobs. A backend ignores what it cannot honour.
struct ClientOptions {
  /// Sent on every request. Empty means the backend uses `rommsync::kUserAgent`.
  std::string user_agent;

  bool follow_redirects = true;
  int max_redirects = 5;

  /// Verify the server certificate. Only ever false for a deliberately
  /// self-signed home server, and only when the user opts in (docs/SECURITY.md).
  bool verify_peer = true;

  /// Optional CA bundle for such a server. Empty means the platform trust store.
  std::string ca_bundle_path;
};

/// The interface every backend implements and every caller depends on.
///
/// Implementations must be safe to call from several threads at once: the
/// download worker and the sync engine share one client.
class HttpClient {
 public:
  virtual ~HttpClient() = default;

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  /// Perform a request and buffer the response body in memory. For API calls;
  /// never for rom or save content.
  virtual Result Send(const Request& request) = 0;

  /// Perform a request and stream the response body to `target`, never holding
  /// more than a buffer's worth in memory.
  virtual Result Download(const Request& request, const DownloadTarget& target) = 0;

 protected:
  HttpClient() = default;
};

}  // namespace rommsync::http
