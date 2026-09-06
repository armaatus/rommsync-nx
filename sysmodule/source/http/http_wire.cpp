// HTTP/1.1 over an abstract `Connection`. See http_wire.hpp for why the console
// backend is split here rather than written as one file that no test can reach.
//
// Everything in this file is deliberately transport-blind and allocation-shy:
// it runs on the console's inner heap (`sysmodule/source/main.cpp`), so a rom
// streams through `kTransferBufferSize` and nothing that scales with a rom is
// ever built in memory -- not a download body, and not a multipart upload.
#include "http_wire.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/version.hpp"

namespace rommsync::sysmodule {
namespace {

using Clock = std::chrono::steady_clock;
using http::DownloadTarget;
using http::Error;
using http::Method;
using http::Request;
using http::Response;
using http::Result;

/// Longest status line + header block this client will read before giving up.
/// A server that has not finished its headers by here is not one we can talk
/// to, and the alternative is a `std::string` that grows until the heap ends.
constexpr std::size_t kMaxHeaderBytes = 32 * 1024;

/// How long a single read or write may block before the loop comes back up to
/// look at the `CancelToken` and the deadlines. The overlay's "stop" is what
/// this bounds: a download must not keep going for a whole stall timeout after
/// the user pressed it (http.hpp).
constexpr std::chrono::milliseconds kPollSlice{200};

const char* MethodName(Method method) {
  switch (method) {
    case Method::kGet:    return "GET";
    case Method::kHead:   return "HEAD";
    case Method::kPost:   return "POST";
    case Method::kPut:    return "PUT";
    case Method::kDelete: return "DELETE";
  }
  return "GET";
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (Lower(a[i]) != Lower(b[i])) return false;
  }
  return true;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

/// Decimal, and refused rather than clamped: a `Content-Length` this client
/// cannot read is a body it cannot frame, and guessing zero would turn a
/// truncated rom into a complete one.
bool ParseDecimal(std::string_view text, std::uint64_t* out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    if (value > (UINT64_MAX - static_cast<std::uint64_t>(c - '0')) / 10) return false;
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  *out = value;
  return true;
}

bool ParseHex(std::string_view text, std::uint64_t* out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (const char c : text) {
    int digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return false;
    if (value > (UINT64_MAX - static_cast<std::uint64_t>(digit)) / 16) return false;
    value = value * 16 + static_cast<std::uint64_t>(digit);
  }
  *out = value;
  return true;
}

/// Total resource size out of a `Content-Range: bytes X-Y/Z`. Zero when the
/// server sent `*`, or the value is not one we understand.
std::uint64_t TotalFromContentRange(const std::string& value) {
  const std::size_t slash = value.rfind('/');
  if (slash == std::string::npos || slash + 1 >= value.size()) return 0;
  std::uint64_t total = 0;
  return ParseDecimal(std::string_view(value).substr(slash + 1), &total) ? total : 0;
}

std::uint64_t FileSizeOf(const std::string& path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0 || info.st_size < 0) return 0;
  return static_cast<std::uint64_t>(info.st_size);
}

// --- the URL ---------------------------------------------------------------

struct Url {
  Origin origin;
  std::string target;  ///< path + query, what goes on the request line
};

/// `https://host:port/path?query`. Only http and https, because those are the
/// only two anything above this client asks for.
bool ParseUrl(std::string_view url, Url* out) {
  bool tls = true;
  if (url.rfind("https://", 0) == 0) {
    url.remove_prefix(8);
  } else if (url.rfind("http://", 0) == 0) {
    tls = false;
    url.remove_prefix(7);
  } else {
    return false;
  }

  const std::size_t slash = url.find('/');
  std::string_view authority = url.substr(0, slash);
  out->target = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
  if (out->target.empty()) out->target = "/";

  // Userinfo is not supported and is refused rather than ignored: silently
  // dropping credentials from a URL is how a request goes out unauthenticated.
  if (authority.find('@') != std::string_view::npos) return false;

  std::uint16_t port = tls ? 443 : 80;
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos) {
    std::uint64_t parsed = 0;
    if (!ParseDecimal(authority.substr(colon + 1), &parsed) || parsed == 0 || parsed > 65535) {
      return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    authority = authority.substr(0, colon);
  }
  if (authority.empty()) return false;

  out->origin.host.assign(authority);
  out->origin.port = port;
  out->origin.tls = tls;
  return true;
}

/// Whether two URLs are the same scheme, host and port. A URL this client
/// cannot parse is never the same origin as anything -- the safe answer, since
/// the only thing that turns on it is whether to keep sending a bearer token.
bool SameOrigin(const std::string& from, const std::string& to) {
  Url before;
  Url after;
  if (!ParseUrl(from, &before) || !ParseUrl(to, &after)) return false;
  return before.origin.tls == after.origin.tls && before.origin.port == after.origin.port &&
         EqualsIgnoreCase(before.origin.host, after.origin.host);
}

/// The headers that must not follow a redirect off this origin. `Cookie` is not
/// one this client sets, and is dropped anyway: a caller that set one meant it
/// for the server it addressed.
void DropCredentialHeaders(http::Headers* headers) {
  http::Headers kept;
  kept.reserve(headers->size());
  for (http::Header& header : *headers) {
    if (EqualsIgnoreCase(header.name, "Authorization") ||
        EqualsIgnoreCase(header.name, "Cookie") ||
        EqualsIgnoreCase(header.name, "Proxy-Authorization")) {
      continue;
    }
    kept.push_back(std::move(header));
  }
  *headers = std::move(kept);
}

/// `Location` resolved against the URL it came from. Absolute, absolute-path
/// and relative forms, which is every shape RomM and a reverse proxy in front
/// of it produce.
std::string ResolveLocation(const Url& parsed, std::string_view location) {
  if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
    return std::string(location);
  }
  const std::string root = std::string(parsed.origin.tls ? "https://" : "http://") +
                           parsed.origin.host + ":" + std::to_string(parsed.origin.port);
  if (!location.empty() && location.front() == '/') {
    return root + std::string(location);
  }
  // The query is not part of the path, and taking the directory of
  // `/api/roms?path=a/b` would resolve a relative `Location` against
  // `/api/roms?path=a/` -- a URL that was never on the server.
  const std::string_view path =
      std::string_view(parsed.target).substr(0, parsed.target.find('?'));
  const std::size_t slash = path.rfind('/');
  const std::string directory =
      slash == std::string_view::npos ? "/" : std::string(path.substr(0, slash + 1));
  return root + directory + std::string(location);
}

// --- the request body ------------------------------------------------------

/// One piece of the body: either literal text this client built, or a file on
/// the card that is streamed rather than read.
struct BodySegment {
  std::string literal;
  std::string file_path;
  std::uint64_t file_size = 0;
};

/// The request body, streamed. `size()` is the `Content-Length` -- known before
/// the first byte goes out, because a chunked upload is not something every
/// server behind a home RomM accepts, and because knowing it is what lets a
/// multi-gigabyte part be sent out of a 16 KiB buffer.
class BodyStream {
 public:
  explicit BodyStream(std::vector<BodySegment> segments) : segments_(std::move(segments)) {
    for (const BodySegment& segment : segments_) {
      total_ += segment.file_path.empty() ? segment.literal.size() : segment.file_size;
    }
  }

  ~BodyStream() {
    if (file_ != nullptr) std::fclose(file_);
  }

  BodyStream(const BodyStream&) = delete;
  BodyStream& operator=(const BodyStream&) = delete;

  std::uint64_t size() const { return total_; }

  /// Fill up to `size` bytes. `*produced == 0` means the body is finished.
  /// False is a file that stopped reading, which is `Error::kInvalidRequest`:
  /// the request was never sendable as described.
  bool Next(char* out, std::size_t size, std::size_t* produced) {
    *produced = 0;
    while (*produced == 0 && at_ < segments_.size()) {
      const BodySegment& segment = segments_[at_];
      if (segment.file_path.empty()) {
        const std::size_t left = segment.literal.size() - offset_;
        const std::size_t take = std::min(size, left);
        std::memcpy(out, segment.literal.data() + offset_, take);
        offset_ += take;
        *produced = take;
        if (offset_ == segment.literal.size()) Advance();
        if (take == 0) continue;
        return true;
      }
      if (file_ == nullptr) {
        file_ = std::fopen(segment.file_path.c_str(), "rb");
        if (file_ == nullptr) return false;
      }
      // Clamped to what `Content-Length` already promised, not to the buffer.
      // The file can grow between the `stat` in `BuildMultipart` and here --
      // that is the *save upload* path with a game still running -- and reading
      // past the declared length would push the multipart terminator beyond the
      // body the server is framing, which it reads either as a refused upload or
      // as the start of a second request.
      const std::uint64_t left = segment.file_size - offset_;
      const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size, left));
      const std::size_t got = want == 0 ? 0 : std::fread(out, 1, want, file_);
      if (got == 0) {
        // Short of what `size()` promised: the file changed under us, and the
        // server is already expecting that many bytes. Refusing here is the
        // only way not to leave the connection framed wrong.
        if (offset_ != segment.file_size) return false;
        Advance();
        continue;
      }
      offset_ += got;
      *produced = got;
      if (offset_ >= segment.file_size) Advance();
      return true;
    }
    return true;
  }

 private:
  void Advance() {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
    ++at_;
    offset_ = 0;
  }

  std::vector<BodySegment> segments_;
  std::size_t at_ = 0;
  std::uint64_t offset_ = 0;
  std::FILE* file_ = nullptr;
  std::uint64_t total_ = 0;
};

/// A boundary no body can contain by accident. Derived from the parts rather
/// than from randomness: this process has no entropy to spare on a delimiter,
/// and a counter that walks until nothing matches is exact rather than likely.
std::string ChooseBoundary(const std::vector<http::FormPart>& parts) {
  std::string boundary;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    boundary = "----rommsync-nx-boundary-" + std::to_string(attempt);
    bool clashes = false;
    for (const http::FormPart& part : parts) {
      // Only the values this client puts in the body verbatim can collide. A
      // file part's *contents* can too, in principle -- but reading a rom to
      // check would defeat the streaming this whole path exists for, and a file
      // that happens to contain this string is not something a counter fixes.
      clashes = clashes || part.name.find(boundary) != std::string::npos ||
                part.value.find(boundary) != std::string::npos ||
                part.file_name.find(boundary) != std::string::npos;
    }
    if (!clashes) return boundary;
  }
  return boundary;
}

/// A `Content-Disposition` parameter, with the two bytes that would break the
/// header removed rather than escaped.
///
/// It matters because not every one of these is this client's own text: a form
/// part's `file_name` is a rom's or a save's, which came off the card or off
/// another client. A quote would end the parameter early and a CRLF would end
/// the *header*, which is a request whose remainder the server reads as
/// something the caller never wrote. RFC 6266 escaping is not an option here --
/// the servers that accept a backslash-escaped quote and the ones that accept a
/// bare one are not the same set -- so the byte goes.
std::string SanitizeParameter(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c == '"' || c == '\r' || c == '\n' || c == '\\') continue;
    out.push_back(c);
  }
  return out;
}

std::string LeafOf(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// `multipart/form-data`, with file parts left on disk. Returns false when a
/// named file cannot be measured -- the length has to be known up front.
bool BuildMultipart(const std::vector<http::FormPart>& parts, const std::string& boundary,
                    std::vector<BodySegment>* segments) {
  for (const http::FormPart& part : parts) {
    std::string head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" +
                       SanitizeParameter(part.name) + "\"";
    if (!part.file_path.empty()) {
      const std::string name = part.file_name.empty() ? LeafOf(part.file_path) : part.file_name;
      head += "; filename=\"" + SanitizeParameter(name) + "\"";
    }
    head += "\r\n";
    if (!part.content_type.empty()) {
      head += "Content-Type: " + part.content_type + "\r\n";
    }
    head += "\r\n";
    segments->push_back({std::move(head), "", 0});

    if (part.file_path.empty()) {
      segments->push_back({part.value, "", 0});
    } else {
      struct stat info {};
      if (::stat(part.file_path.c_str(), &info) != 0 || info.st_size < 0) return false;
      segments->push_back({"", part.file_path, static_cast<std::uint64_t>(info.st_size)});
    }
    segments->push_back({"\r\n", "", 0});
  }
  segments->push_back({"--" + boundary + "--\r\n", "", 0});
  return true;
}

// --- deadlines -------------------------------------------------------------

/// The three budgets `http::Request` carries, plus the cancel token, in one
/// place -- because every read and every write in this file is bounded by
/// whichever of them expires first, and a path that forgot one is a sysmodule
/// that hangs.
class Budget {
 public:
  Budget(const Request& request, Clock::time_point started)
      : cancel_(request.cancel),
        total_(request.timeout),
        stall_(request.stall_timeout),
        started_(started),
        last_progress_(started) {}

  void Moved() { last_progress_ = Clock::now(); }

  /// How long the next I/O call may block, or an error if the budget is spent.
  /// `kNone` with `*slice` set is the only way on.
  Error Slice(std::chrono::milliseconds* slice) const {
    if (cancel_ != nullptr && cancel_->canceled()) return Error::kCanceled;
    const Clock::time_point now = Clock::now();
    std::chrono::milliseconds allowance = kPollSlice;
    if (total_.count() > 0) {
      const auto left = total_ - std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now - started_);
      if (left.count() <= 0) return Error::kTimeout;
      allowance = std::min(allowance, left);
    }
    if (stall_.count() > 0) {
      const auto left = stall_ - std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now - last_progress_);
      if (left.count() <= 0) return Error::kTimeout;
      allowance = std::min(allowance, left);
    }
    *slice = allowance;
    return Error::kNone;
  }

 private:
  const http::CancelToken* cancel_;
  std::chrono::milliseconds total_;
  std::chrono::milliseconds stall_;
  Clock::time_point started_;
  Clock::time_point last_progress_;
};

// --- reading ---------------------------------------------------------------

/// A buffered reader over a `Connection`, so the header parser can ask for a
/// line without a read syscall per byte and the body loop can still get the
/// bytes that arrived in the same packet.
class Reader {
 public:
  /// `buffer` is the exchange's one in-flight buffer, owned by the caller and
  /// on the heap: 16 KiB on a thread stack is most of the stack this process
  /// gives a thread (`sysmodule/sys-rommsync.json`).
  Reader(Connection& connection, Budget& budget, char* buffer, std::size_t size)
      : connection_(connection), budget_(budget), buffer_(buffer), size_(size) {}

  /// Whether the peer has closed and the buffer is empty.
  bool exhausted() const { return closed_ && at_ == filled_; }

  /// Refill once. `kNone` means either bytes arrived or the peer closed --
  /// `exhausted()` tells the two apart.
  Error Pump() {
    if (at_ < filled_) return Error::kNone;
    at_ = 0;
    filled_ = 0;
    for (;;) {
      std::chrono::milliseconds slice{0};
      if (const Error error = budget_.Slice(&slice); error != Error::kNone) return error;
      const IoResult io = connection_.Read(buffer_, size_, slice);
      switch (io.status) {
        case IoStatus::kOk:
          if (io.transferred == 0) continue;  // nothing yet; the budget bounds this
          filled_ = io.transferred;
          budget_.Moved();
          return Error::kNone;
        case IoStatus::kClosed:
          closed_ = true;
          return Error::kNone;
        case IoStatus::kTimedOut:
          continue;  // the slice expired, not the budget; Slice() owns the verdict
        case IoStatus::kFailed:
          closed_ = true;
          failed_ = true;
          return Error::kNone;  // whether it is truncation is the framing's call
      }
    }
  }

  /// Whether the connection broke rather than closed. A body framed by
  /// `Content-Length` treats both the same way -- short is short -- but a
  /// close-framed one does not, so the distinction has to survive.
  bool failed() const { return failed_; }

  /// One CRLF-terminated line, without the terminator. False when the peer
  /// closed first or `error` was set.
  bool ReadLine(std::string* line, std::size_t cap, Error* error) {
    line->clear();
    for (;;) {
      while (at_ < filled_) {
        const char c = buffer_[at_++];
        if (c == '\n') {
          if (!line->empty() && line->back() == '\r') line->pop_back();
          return true;
        }
        if (line->size() >= cap) {
          *error = Error::kTransport;
          return false;
        }
        line->push_back(c);
      }
      if (const Error pumped = Pump(); pumped != Error::kNone) {
        *error = pumped;
        return false;
      }
      if (exhausted()) return false;
    }
  }

  /// Whatever has arrived, without copying it: the body path writes straight
  /// out of this buffer, so a download costs one buffer rather than two.
  /// `*size == 0` with `*error == kNone` is the end of the stream.
  bool Peek(const char** data, std::size_t* size, Error* error) {
    if (at_ == filled_) {
      if (const Error pumped = Pump(); pumped != Error::kNone) {
        *error = pumped;
        *size = 0;
        return false;
      }
      if (exhausted()) {
        *size = 0;
        return true;
      }
    }
    *data = buffer_ + at_;
    *size = filled_ - at_;
    return true;
  }

  void Consume(std::size_t count) { at_ += std::min(count, filled_ - at_); }

 private:
  Connection& connection_;
  Budget& budget_;
  char* buffer_;
  std::size_t size_;
  std::size_t at_ = 0;
  std::size_t filled_ = 0;
  bool closed_ = false;
  bool failed_ = false;
};

/// Push `size` bytes out, however many `Write` calls that takes, bounded by the
/// budget. False leaves `result` explaining which half of the request stopped --
/// `what` is "head" or "body", which is the only thing that differs between the
/// two call sites.
bool WriteAll(Connection& connection, Budget& budget, const char* data, std::size_t size,
              const char* what, Result* result) {
  std::size_t at = 0;
  while (at < size) {
    std::chrono::milliseconds slice{0};
    if (const Error error = budget.Slice(&slice); error != Error::kNone) {
      result->error = error;
      result->message = std::string("while sending the request ") + what;
      return false;
    }
    const IoResult io = connection.Write(data + at, size - at, slice);
    if (io.status == IoStatus::kFailed || io.status == IoStatus::kClosed) {
      result->error = Error::kTransport;
      result->message = std::string("the connection closed while sending the request ") + what;
      return false;
    }
    if (io.transferred != 0) budget.Moved();
    at += io.transferred;
  }
  return true;
}

/// Where the body goes. Two implementations: a `std::string` for `Send`, and
/// the `.part` file for `Download`.
class BodyWriter {
 public:
  virtual ~BodyWriter() = default;

  /// The final response's status and headers are known. False aborts with
  /// `*error`.
  virtual bool Begin(const Response& head, Error* error) = 0;

  /// False aborts with `*error` -- `kWriteFailed` when the card refused it.
  virtual bool Write(const char* data, std::size_t size, Error* error) = 0;

 protected:
  BodyWriter() = default;
};

/// How the response body is delimited, decided once from the head.
enum class Framing { kNone, kLength, kChunked, kUntilClose };

}  // namespace

namespace {

class WireHttpClient final : public http::HttpClient {
 public:
  WireHttpClient(Connector& connector, http::ClientOptions options)
      : connector_(connector), options_(std::move(options)) {
    if (options_.user_agent.empty()) options_.user_agent = kUserAgent;
  }

  Result Send(const Request& request) override;
  Result Download(const Request& request, const DownloadTarget& target) override;

 private:
  /// One request/response exchange, redirects included. `writer` sees only the
  /// body of the response the caller ends up with.
  Result Exchange(const Request& request, BodyWriter* writer);

  /// Everything between opening a connection and the last body byte, for one
  /// hop. `redirect_to` comes back non-empty when the caller should follow.
  Result OneHop(const Request& request, const std::string& url, BodyWriter* writer,
                Budget& budget, bool allow_redirect, std::string* redirect_to);

  Connector& connector_;
  http::ClientOptions options_;
};

/// `Send`'s sink: the body in memory, for API calls only.
class StringWriter final : public BodyWriter {
 public:
  explicit StringWriter(std::string* body) : body_(body) {}

  bool Begin(const Response&, Error*) override { return true; }

  bool Write(const char* data, std::size_t size, Error*) override {
    body_->append(data, size);
    return true;
  }

 private:
  std::string* body_;
};

/// `Download`'s sink: `<path>.part`, and the resume bookkeeping around it.
///
/// The rules it enforces are `http.hpp`'s, and every one of them is a way a
/// download can silently corrupt a file rather than fail:
///   * an error body is a message about why there is no content and never
///     reaches the file;
///   * a `resume` the server answers 200 to is the whole resource, so the bytes
///     already on disk are discarded rather than prepended;
///   * `staged` follows the file down when that happens, instead of claiming
///     progress the card does not have.
class FileWriter final : public BodyWriter {
 public:
  FileWriter(std::string partial_path, bool resuming, const http::ProgressCallback* progress)
      : path_(std::move(partial_path)), staged_unconfirmed_(resuming), progress_(progress) {}

  ~FileWriter() override { Close(); }

  /// Open `<path>.part`. `resuming` keeps what is already in it and appends;
  /// otherwise the file starts empty.
  bool Open(bool resuming, std::uint64_t already_have) {
    resume_from_ = already_have;
    if (!resuming) {
      file_ = std::fopen(path_.c_str(), "wb");
      return file_ != nullptr;
    }
    // "r+b" rather than "ab": O_APPEND would ignore the seek needed when the
    // server turns out to have ignored the Range header.
    file_ = std::fopen(path_.c_str(), "r+b");
    if (file_ == nullptr) return false;
    if (std::fseek(file_, 0, SEEK_END) != 0) {
      Close();
      return false;
    }
    return true;
  }

  bool Begin(const Response& head, Error*) override {
    status_ = head.status;
    // What *this response* says its own body weighs, which is what `staged`'s
    // total is built from -- not `declared_size`, which is the whole resource
    // on a 206.
    body_length_ = 0;
    if (const std::string* length = http::FindHeader(head.headers, "Content-Length")) {
      if (!ParseDecimal(Trim(*length), &body_length_)) body_length_ = 0;
    }
    return true;
  }

  bool Write(const char* data, std::size_t size, Error* error) override {
    if (status_ < 200 || status_ >= 300) {
      const std::size_t room = kErrorBodyCap - std::min(kErrorBodyCap, error_body_.size());
      error_body_.append(data, std::min(size, room));
      received_ += size;  // what arrived, even though only `room` was kept
      return true;
    }
    if (staged_unconfirmed_) {
      staged_unconfirmed_ = false;
      // The server ignored `Range` and is sending the whole resource.
      // Appending would splice it into itself, so the bytes already on disk go.
      if (status_ != 206 && !Restart()) {
        *error = Error::kWriteFailed;
        return false;
      }
    }
    if (size != 0 && (file_ == nullptr || std::fwrite(data, 1, size, file_) != size)) {
      *error = Error::kWriteFailed;
      return false;
    }
    received_ += size;
    if (progress_ != nullptr) {
      const std::uint64_t total = body_length_ == 0 ? 0 : resume_from_ + body_length_;
      (*progress_)(resume_from_ + received_, total);
    }
    return true;
  }

  /// Flush and close, and make it durable through the same `io::FileSync` every
  /// atomic write in `core/` goes through -- `fsdevCommitDevice` on the console
  /// (`main.cpp`), rather than a second spelling of "make this durable".
  bool Finish() {
    // Still pending means no body byte ever reached this writer, so the
    // response never confirmed the bytes already on disk. Counting them would
    // let an empty 200 promote a stale partial file.
    if (staged_unconfirmed_) resume_from_ = 0;
    bool flushed = file_ == nullptr || std::fflush(file_) == 0;
    Close();
    if (flushed) {
      if (const io::FileSync sync = io::GetFileSync(); sync != nullptr) {
        flushed = sync(path_);
      }
    }
    return flushed;
  }

  std::uint64_t resume_from() const { return resume_from_; }
  std::uint64_t received() const { return received_; }
  std::string& error_body() { return error_body_; }

 private:
  /// Throw the staged bytes away and start the file over.
  ///
  /// Reopening with "wb" rather than `ftruncate`: devkitA64 compiles this target
  /// as `-std=c++20` rather than `gnu++20` (`switch.mk`), so newlib hides
  /// `ftruncate` and `fileno` behind `__STRICT_ANSI__` and neither is
  /// declared. `fopen(..., "wb")` is the truncation both targets do have.
  bool Restart() {
    Close();
    file_ = std::fopen(path_.c_str(), "wb");
    if (file_ == nullptr) return false;
    resume_from_ = 0;
    return true;
  }

  void Close() {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  std::string path_;
  std::FILE* file_ = nullptr;
  std::uint64_t resume_from_ = 0;
  /// True until a body byte of *this* response has confirmed that the bytes
  /// already in `<path>.part` are part of it. Both places it is read follow from
  /// that one meaning: the first byte to arrive decides whether to keep them
  /// (a 206) or throw them away (a 200 that ignored `Range`), and if no byte
  /// ever arrives they were never confirmed at all, so `Finish` discounts them.
  bool staged_unconfirmed_;
  const http::ProgressCallback* progress_;
  int status_ = 0;
  std::uint64_t body_length_ = 0;
  std::uint64_t received_ = 0;
  std::string error_body_;
};

}  // namespace

namespace {

/// The head of a response: everything before the body.
struct Head {
  int status = 0;
  http::Headers headers;
};

/// Read the status line and headers of one response. 1xx is informational and
/// is skipped rather than delivered -- a `100 Continue` is not the response the
/// caller asked for.
Error ReadHead(Reader& reader, Head* head) {
  for (;;) {
    // How much of the head is left to read. Not a `Budget` -- that is the
    // deadlines, and this is bytes.
    std::size_t room = kMaxHeaderBytes;
    std::string line;
    Error error = Error::kNone;
    if (!reader.ReadLine(&line, room, &error)) {
      if (error != Error::kNone) return error;
      // The peer closed before saying anything. `kTruncated` would claim a body
      // ended early; nothing was ever framed here.
      return Error::kTransport;
    }
    if (line.rfind("HTTP/", 0) != 0) return Error::kTransport;
    const std::size_t space = line.find(' ');
    if (space == std::string::npos) return Error::kTransport;
    std::uint64_t status = 0;
    const std::size_t end = line.find(' ', space + 1);
    if (!ParseDecimal(std::string_view(line).substr(
                          space + 1, (end == std::string::npos ? line.size() : end) - space - 1),
                      &status) ||
        status < 100 || status > 599) {
      return Error::kTransport;
    }
    head->status = static_cast<int>(status);
    head->headers.clear();
    room -= std::min(room, line.size());

    for (;;) {
      if (!reader.ReadLine(&line, room, &error)) {
        if (error != Error::kNone) return error;
        return Error::kTruncated;  // the header block never ended
      }
      if (room < line.size()) return Error::kTransport;
      room -= line.size();
      if (line.empty()) break;
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;  // a fold or a malformed line
      head->headers.push_back({std::string(Trim(std::string_view(line).substr(0, colon))),
                               std::string(Trim(std::string_view(line).substr(colon + 1)))});
    }
    if (head->status >= 200) return Error::kNone;
  }
}

/// What the server said the whole resource weighs: the total from a
/// `Content-Range` on a 206, otherwise `Content-Length`.
std::uint64_t DeclaredSize(const http::Headers& headers) {
  if (const std::string* range = http::FindHeader(headers, "Content-Range")) {
    if (const std::uint64_t total = TotalFromContentRange(*range); total != 0) return total;
  }
  if (const std::string* length = http::FindHeader(headers, "Content-Length")) {
    std::uint64_t value = 0;
    return ParseDecimal(Trim(*length), &value) ? value : 0;
  }
  return 0;
}

/// How the body is delimited (RFC 9112 s6.3), reduced to the cases a client
/// meets: no body at all, `Transfer-Encoding: chunked`, a `Content-Length`, or
/// the connection closing.
Framing FramingFor(Method method, const Head& head, std::uint64_t* length) {
  *length = 0;
  if (method == Method::kHead || head.status == 204 || head.status == 304 ||
      (head.status >= 100 && head.status < 200)) {
    return Framing::kNone;
  }
  if (const std::string* encoding = http::FindHeader(head.headers, "Transfer-Encoding")) {
    // `identity` is not chunked and is what the fault proxy's `truncate` mode
    // sends: a clean short close with nothing to compare against
    // (server/testing/fault_proxy.py).
    if (!EqualsIgnoreCase(Trim(*encoding), "identity")) return Framing::kChunked;
    return Framing::kUntilClose;
  }
  if (const std::string* declared = http::FindHeader(head.headers, "Content-Length")) {
    if (ParseDecimal(Trim(*declared), length)) return Framing::kLength;
  }
  return Framing::kUntilClose;
}

/// Read a chunked body into `writer`. The terminating zero-length chunk is what
/// says the body is complete; anything else ending the stream is `kTruncated`.
Error ReadChunked(Reader& reader, BodyWriter& writer, std::uint64_t* received) {
  for (;;) {
    std::string line;
    Error error = Error::kNone;
    if (!reader.ReadLine(&line, 64, &error)) {
      return error != Error::kNone ? error : Error::kTruncated;
    }
    // A chunk-size line may carry extensions after a ';'. We honour none of
    // them, but the size in front of one is still the size.
    const std::string_view size_text = Trim(std::string_view(line).substr(0, line.find(';')));
    std::uint64_t remaining = 0;
    if (!ParseHex(size_text, &remaining)) return Error::kTruncated;
    if (remaining == 0) {
      // Trailers, then a blank line. Read them off so the framing is complete
      // even though nothing here uses one.
      while (reader.ReadLine(&line, kMaxHeaderBytes, &error) && !line.empty()) {
      }
      return error;
    }
    while (remaining != 0) {
      const char* data = nullptr;
      std::size_t available = 0;
      if (!reader.Peek(&data, &available, &error)) return error;
      if (available == 0) return Error::kTruncated;
      const std::size_t take =
          static_cast<std::size_t>(std::min<std::uint64_t>(remaining, available));
      if (!writer.Write(data, take, &error)) return error;
      reader.Consume(take);
      *received += take;
      remaining -= take;
    }
    if (!reader.ReadLine(&line, 8, &error) || !line.empty()) {
      return error != Error::kNone ? error : Error::kTruncated;
    }
  }
}

Error ReadBody(Framing framing, std::uint64_t length, Reader& reader, BodyWriter& writer,
               std::uint64_t* received) {
  if (framing == Framing::kNone) return Error::kNone;
  if (framing == Framing::kChunked) return ReadChunked(reader, writer, received);

  std::uint64_t remaining = length;
  for (;;) {
    if (framing == Framing::kLength && remaining == 0) return Error::kNone;
    const char* data = nullptr;
    std::size_t available = 0;
    Error error = Error::kNone;
    if (!reader.Peek(&data, &available, &error)) return error;
    if (available == 0) {
      // The stream ended. A `Content-Length` still owed is a body that ended
      // early; a close-framed body ends exactly this way, unless the connection
      // broke rather than closed.
      if (framing == Framing::kLength) return Error::kTruncated;
      return reader.failed() ? Error::kTruncated : Error::kNone;
    }
    const std::size_t take =
        framing == Framing::kLength
            ? static_cast<std::size_t>(std::min<std::uint64_t>(remaining, available))
            : available;
    if (!writer.Write(data, take, &error)) return error;
    reader.Consume(take);
    *received += take;
    if (framing == Framing::kLength) remaining -= take;
  }
}

}  // namespace

Result WireHttpClient::OneHop(const Request& request, const std::string& url, BodyWriter* writer,
                              Budget& budget, bool allow_redirect, std::string* redirect_to) {
  Result result;
  Url parsed;
  if (!ParseUrl(url, &parsed)) {
    result.error = Error::kInvalidRequest;
    result.message = "not an http(s) url: " + url;
    return result;
  }

  // The body first, because a form part naming a file that is not there is a
  // request that was never sendable, and finding that out after a handshake
  // costs a connection for nothing.
  std::vector<BodySegment> segments;
  std::string content_type;
  if (!request.form.empty()) {
    const std::string boundary = ChooseBoundary(request.form);
    if (!BuildMultipart(request.form, boundary, &segments)) {
      result.error = Error::kInvalidRequest;
      result.message = "a form part names a file that cannot be read";
      return result;
    }
    content_type = "multipart/form-data; boundary=" + boundary;
  } else if (!request.body.empty()) {
    segments.push_back({request.body, "", 0});
  }
  BodyStream body(std::move(segments));
  const bool has_body = request.method == Method::kPost || request.method == Method::kPut;

  // The one in-flight buffer this exchange costs, on the heap rather than on
  // the thread's stack, and reused for the request body and then the response:
  // the two never overlap, and a second one would double what a download costs
  // the inner heap for nothing.
  const std::unique_ptr<char[]> scratch(new (std::nothrow) char[kTransferBufferSize]);
  if (scratch == nullptr) {
    result.error = Error::kTransport;
    result.message = "no room for a transfer buffer";
    return result;
  }

  Error open_error = Error::kNone;
  std::string open_message;
  std::unique_ptr<Connection> connection =
      connector_.Open(parsed.origin, request.connect_timeout, options_, &open_error, &open_message);
  if (connection == nullptr) {
    result.error = open_error == Error::kNone ? Error::kConnectFailed : open_error;
    result.message = std::move(open_message);
    return result;
  }
  // Opening the connection *is* progress, so the stall clock starts here rather
  // than at the start of the request. Otherwise a caller whose `stall_timeout`
  // is shorter than the handshake it just waited out would be told the transfer
  // stalled before a byte of it was ever asked for. The total timeout is
  // deliberately not reset: it is a ceiling on the whole exchange, redirects and
  // handshakes included.
  budget.Moved();

  std::string head;
  head.reserve(512);
  head += MethodName(request.method);
  head += ' ';
  head += parsed.target;
  head += " HTTP/1.1\r\nHost: ";
  head += parsed.origin.host;
  if (parsed.origin.port != (parsed.origin.tls ? 443 : 80)) {
    head += ':';
    head += std::to_string(parsed.origin.port);
  }
  // No keep-alive: a connection pool inside a resident process is lifetime
  // rules and idle sockets in exchange for a handshake, and the handshake is
  // what M0-1 measured as affordable (docs/DEVELOPMENT.md).
  head += "\r\nConnection: close\r\n";
  if (http::FindHeader(request.headers, "User-Agent") == nullptr) {
    head += "User-Agent: " + options_.user_agent + "\r\n";
  }
  if (http::FindHeader(request.headers, "Accept") == nullptr) {
    head += "Accept: */*\r\n";
  }
  if (request.range_start > 0) {
    head += "Range: bytes=" + std::to_string(request.range_start) + "-\r\n";
  }
  if (!content_type.empty()) {
    head += "Content-Type: " + content_type + "\r\n";
  }
  if (has_body) {
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  for (const http::Header& header : request.headers) {
    // The ones this client frames the exchange with are its own. A caller that
    // set `Content-Length` by hand would otherwise send two, which some servers
    // read as a request smuggling attempt and all of them read as malformed.
    if (EqualsIgnoreCase(header.name, "Host") || EqualsIgnoreCase(header.name, "Connection") ||
        EqualsIgnoreCase(header.name, "Content-Length") ||
        EqualsIgnoreCase(header.name, "Transfer-Encoding") ||
        EqualsIgnoreCase(header.name, "Range") ||
        (!content_type.empty() && EqualsIgnoreCase(header.name, "Content-Type"))) {
      continue;
    }
    head += header.name + ": " + header.value + "\r\n";
  }
  head += "\r\n";

  // --- write the request ---
  if (!WriteAll(*connection, budget, head.data(), head.size(), "head", &result)) {
    return result;
  }
  if (has_body) {
    char* const chunk = scratch.get();
    for (;;) {
      std::size_t produced = 0;
      if (!body.Next(chunk, kTransferBufferSize, &produced)) {
        result.error = Error::kInvalidRequest;
        result.message = "the request body could not be read from the card";
        return result;
      }
      if (produced == 0) break;
      if (!WriteAll(*connection, budget, chunk, produced, "body", &result)) return result;
    }
  }

  // --- read the response ---
  Reader reader(*connection, budget, scratch.get(), kTransferBufferSize);
  Head response_head;
  if (const Error error = ReadHead(reader, &response_head); error != Error::kNone) {
    result.error = error;
    if (error == Error::kTransport) result.message = "the server's response could not be read";
    return result;
  }
  result.response.status = response_head.status;
  result.response.headers = response_head.headers;
  result.response.declared_size = DeclaredSize(response_head.headers);

  const bool redirects = response_head.status == 301 || response_head.status == 302 ||
                         response_head.status == 303 || response_head.status == 307 ||
                         response_head.status == 308;
  if (allow_redirect && redirects) {
    if (const std::string* location = http::FindHeader(response_head.headers, "Location");
        location != nullptr && !location->empty()) {
      // The redirect's own body is not the caller's, and the connection is
      // closing anyway, so it is dropped with the connection rather than read.
      *redirect_to = ResolveLocation(parsed, *location);
      return result;
    }
  }

  if (!writer->Begin(result.response, &result.error)) {
    return result;
  }
  std::uint64_t length = 0;
  const Framing framing = FramingFor(request.method, response_head, &length);
  std::uint64_t received = 0;
  const Error error = ReadBody(framing, length, reader, *writer, &received);
  result.response.bytes_received = received;
  if (error != Error::kNone) {
    result.error = error;
    if (result.message.empty()) result.message = http::ToString(error);
  }
  return result;
}

Result WireHttpClient::Exchange(const Request& request, BodyWriter* writer) {
  Result result;
  if (request.url.empty()) {
    result.error = Error::kInvalidRequest;
    result.message = "request url is empty";
    return result;
  }
  if (request.method != Method::kPost && request.method != Method::kPut &&
      (!request.form.empty() || !request.body.empty())) {
    // Silently dropping it is how a caller ends up debugging the server.
    result.error = Error::kInvalidRequest;
    result.message = "a request body is only valid on POST and PUT";
    return result;
  }
  for (const http::Header& header : request.headers) {
    // Same reasoning as `SanitizeParameter`, one layer up: a header value
    // carrying a CRLF would end the request head and turn the rest of it into a
    // second request the server reads as this caller's. Checked here rather
    // than while the head is built, so a request that was never sendable does
    // not first cost a connection and a handshake.
    if (header.name.empty() || header.name.find_first_of("\r\n:") != std::string::npos ||
        header.value.find_first_of("\r\n") != std::string::npos) {
      result.error = Error::kInvalidRequest;
      result.message = "a request header is not a header line";
      return result;
    }
  }

  Budget budget(request, Clock::now());
  Request hop = request;
  std::string url = request.url;
  const int limit = options_.follow_redirects ? std::max(0, options_.max_redirects) : 0;

  for (int redirect = 0;; ++redirect) {
    std::string next;
    result = OneHop(hop, url, writer, budget, redirect < limit, &next);
    if (next.empty()) return result;

    // A 303 -- and, by every client's convention, a 301 or 302 on a POST --
    // continues as a GET with no body. 307 and 308 keep both, which is why a
    // form part has to still be on the card at that point.
    if (result.response.status == 303 ||
        ((result.response.status == 301 || result.response.status == 302) &&
         hop.method == Method::kPost)) {
      hop.method = Method::kGet;
      hop.body.clear();
      hop.form.clear();
    }
    // **Credentials do not cross an origin.** Every caller in `core/` sets an
    // `Authorization: Bearer` header, and a RomM behind a reverse proxy that
    // redirects a rom download to object storage or a CDN would otherwise hand
    // that host the user's RomM token. libcurl strips it on a cross-origin
    // redirect for the same reason, which is why neither `http.*` nor `wire.*`
    // would have shown the difference.
    if (!SameOrigin(url, next)) {
      DropCredentialHeaders(&hop.headers);
    }
    url = std::move(next);
  }
}

Result WireHttpClient::Send(const Request& request) {
  // Collected apart from the `Result` and moved in at the end: a redirect makes
  // several exchanges, and only the last one's body is the caller's.
  std::string body;
  StringWriter writer(&body);
  Result result = Exchange(request, &writer);
  result.response.body = std::move(body);
  return result;
}

Result WireHttpClient::Download(const Request& request, const DownloadTarget& target) {
  Result result;
  if (target.path.empty()) {
    result.error = Error::kInvalidRequest;
    result.message = "download target path is empty";
    return result;
  }

  // The one spelling of the staging name, so `core/` can look at the same file
  // this writes without knowing the suffix (http.hpp).
  const std::string partial_path = http::PartialPathFor(target.path);

  // A caller-set range means "fetch this slice" and starts the file fresh; a
  // resume means "finish this file" and is the only case that keeps the bytes
  // already on disk.
  const std::uint64_t already_have =
      (target.resume && request.range_start == 0) ? FileSizeOf(partial_path) : 0;
  const bool resuming = already_have > 0;

  Request effective = request;
  if (resuming) effective.range_start = already_have;

  FileWriter writer(partial_path, resuming, target.progress ? &target.progress : nullptr);
  if (!writer.Open(resuming, already_have)) {
    result.error = Error::kWriteFailed;
    result.message = "could not open " + partial_path;
    return result;
  }

  result = Exchange(effective, &writer);
  const bool flushed = writer.Finish();
  const std::uint64_t written = writer.resume_from() + writer.received();
  result.response.body = std::move(writer.error_body());
  if (!result.ok()) {
    return result;  // the partial file stays behind for a later resume
  }

  const int status = result.response.status;
  if (resuming && status == 416) {
    // Our offset is not valid for this resource any more, so the bytes we were
    // building on are worthless -- and keeping them would make every future
    // resume ask for the same impossible range.
    std::remove(partial_path.c_str());
    result.error = Error::kTruncated;
    result.message = "the partial file no longer matches the resource; retry from the start";
    return result;
  }
  if (status != 200 && status != 206) {
    // Not a body-bearing response: a status the caller has to handle, with
    // nothing at the destination. Do not leave the empty part file behind.
    if (!resuming) std::remove(partial_path.c_str());
    return result;
  }
  if (!flushed) {
    result.error = Error::kWriteFailed;
    result.message = "could not flush " + partial_path;
    return result;
  }

  // What the destination has to weigh to be complete. A caller-supplied size
  // wins: it is the only thing that can catch a server which ends the body
  // cleanly without ever declaring a length. Otherwise the server's own
  // declaration does, except for a slice the caller asked for -- there
  // `declared_size` is the whole resource, not the slice.
  const bool got_slice = request.range_start > 0 && status == 206;
  std::uint64_t expected = target.expected_size;
  if (expected == 0 && !got_slice) expected = result.response.declared_size;
  if (resuming && expected == 0) {
    // Stitching two halves together is only safe if the seam can be checked.
    result.error = Error::kTruncated;
    result.message = "resumed download cannot be verified: the server declared no size";
    return result;
  }
  if (expected != 0 && written != expected) {
    result.error = Error::kTruncated;
    result.message =
        "expected " + std::to_string(expected) + " bytes, wrote " + std::to_string(written);
    return result;
  }

  if (std::rename(partial_path.c_str(), target.path.c_str()) != 0) {
    result.error = Error::kWriteFailed;
    result.message = "could not rename " + partial_path;
  }
  return result;
}

std::unique_ptr<http::HttpClient> MakeWireHttpClient(Connector& connector,
                                                     const http::ClientOptions& options) {
  return std::unique_ptr<http::HttpClient>(new WireHttpClient(connector, options));
}

}  // namespace rommsync::sysmodule
