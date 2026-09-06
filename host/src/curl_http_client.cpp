// libcurl implementation of rommsync::http::HttpClient.
//
// The only file in the tree that names a TLS/HTTP library. Everything above it
// sees rommsync/http.hpp, which is why the Horizon backend can replace this one
// without the sync engine noticing (docs/DEVELOPMENT.md#tls-in-a-sysmodule).
#include "rommsync/host/curl_http_client.hpp"

#include <curl/curl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/version.hpp"

#if LIBCURL_VERSION_NUM < 0x073800  // 7.56.0
#error "libcurl 7.56+ is required for the mime API used by the multipart path"
#endif

namespace rommsync::host {
namespace {

using http::DownloadTarget;
using http::Error;
using http::Headers;
using http::Method;
using http::Request;
using http::Response;
using http::Result;

/// How much of a failed download's body we keep. A 404 from RomM is a short
/// JSON document and worth surfacing; anything longer is a page we do not want
/// in a log line, and it must never reach the destination file.
constexpr std::size_t kErrorBodyCap = 64 * 1024;

void EnsureGlobalInit() {
  // curl_global_init is not thread safe and is required before the first easy
  // handle. Deliberately never paired with curl_global_cleanup: the client can
  // outlive any single call, and tearing global state down while another thread
  // holds a handle is worse than leaking it for the life of the process.
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string Trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

/// Total resource size out of a `Content-Range: bytes X-Y/Z` value. Zero when
/// the server sent `*` for the total, or the value is not one we understand.
std::uint64_t TotalFromContentRange(const std::string& value) {
  const std::size_t slash = value.rfind('/');
  if (slash == std::string::npos) {
    return 0;
  }
  std::uint64_t total = 0;
  for (std::size_t i = slash + 1; i < value.size(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') {
      return 0;
    }
    total = total * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return (slash + 1 < value.size()) ? total : 0;
}

/// Everything a transfer's callbacks need. One instance per request, on the
/// stack of the calling thread, so a shared client stays thread safe.
struct Transfer {
  const Request* request = nullptr;

  Headers headers;
  /// Filled from the status line as it arrives, because the body callback has
  /// to know whether what it is being handed is content or an error page.
  long status = 0;
  /// CURLOPT_RANGE borrows the caller's buffer, so it lives here -- per request,
  /// on the calling thread's stack -- rather than on the shared client.
  std::string range;

  std::string* body = nullptr;  ///< where Send() collects the body

  // Download-only state.
  std::FILE* file = nullptr;
  std::string error_body;      ///< a non-2xx body goes here, never to the file
  std::uint64_t resume_from = 0;
  /// There are bytes on disk from an earlier attempt and this response has not
  /// yet said whether they are still part of the file.
  bool restart_needed = false;
  bool write_failed = false;

  /// The caller's progress sink, or null. Borrowed from the `DownloadTarget`,
  /// which must outlive the call, and set only by `Download` -- so a `Send`
  /// cannot fire one no matter what a caller left on a reused target.
  const http::ProgressCallback* progress = nullptr;

  std::uint64_t received = 0;
};

std::size_t OnHeader(char* data, std::size_t size, std::size_t nmemb, void* context) {
  auto* transfer = static_cast<Transfer*>(context);
  const std::size_t length = size * nmemb;
  const std::string_view line(data, length);

  // A new status line starts a new header block: after a redirect (or a 100
  // Continue) only the last block describes the response the caller gets.
  if (line.rfind("HTTP/", 0) == 0) {
    transfer->headers.clear();
    // A redirect's body belongs to the redirect, not to the response the caller
    // ends up with, so it goes when its header block does.
    transfer->error_body.clear();
    transfer->status = 0;
    const std::size_t space = line.find(' ');
    if (space != std::string_view::npos) {
      for (std::size_t i = space + 1; i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i) {
        transfer->status = transfer->status * 10 + (line[i] - '0');
      }
    }
    return length;
  }
  const std::size_t colon = line.find(':');
  if (colon != std::string_view::npos) {
    transfer->headers.push_back({Trim(line.substr(0, colon)), Trim(line.substr(colon + 1))});
  }
  return length;
}

std::size_t OnBodyToString(char* data, std::size_t size, std::size_t nmemb, void* context) {
  auto* transfer = static_cast<Transfer*>(context);
  const std::size_t length = size * nmemb;
  transfer->body->append(data, length);
  transfer->received += length;
  return length;
}

std::size_t OnBodyToFile(char* data, std::size_t size, std::size_t nmemb, void* context) {
  auto* transfer = static_cast<Transfer*>(context);
  const std::size_t length = size * nmemb;

  // Only a successful response is content. An error body is a message about
  // why there is no content, and writing it to the part file would turn a 404
  // into a corrupt rom.
  if (transfer->status < 200 || transfer->status >= 300) {
    const std::size_t room = kErrorBodyCap - std::min(kErrorBodyCap, transfer->error_body.size());
    transfer->error_body.append(data, std::min(length, room));
    transfer->received += length;  // what arrived, even though only `room` was kept
    return length;
  }

  if (transfer->restart_needed) {
    transfer->restart_needed = false;
    if (transfer->status != 206) {
      // We resumed from a partial file but the server ignored the Range header
      // and started over. Appending would splice the resource into itself, so
      // throw the partial bytes away and take the whole thing.
      if (std::fflush(transfer->file) != 0 ||
          ftruncate(fileno(transfer->file), 0) != 0 ||
          std::fseek(transfer->file, 0, SEEK_SET) != 0) {
        transfer->write_failed = true;
        return 0;  // aborts the transfer with CURLE_WRITE_ERROR
      }
      transfer->resume_from = 0;
    }
  }

  if (std::fwrite(data, 1, length, transfer->file) != length) {
    transfer->write_failed = true;
    return 0;
  }
  transfer->received += length;
  return length;
}

int OnProgress(void* context, curl_off_t dltotal, curl_off_t, curl_off_t, curl_off_t) {
  const auto* transfer = static_cast<const Transfer*>(context);
  if (transfer->request->cancel != nullptr && transfer->request->cancel->canceled()) {
    return 1;  // aborts with CURLE_ABORTED_BY_CALLBACK
  }
  // Only a body-bearing response is progress. An error body is a message about
  // why there is no content -- `OnBodyToFile` counts its bytes but never writes
  // them -- and reporting them would push the caller's figure past what the file
  // holds.
  const bool content = transfer->status == 200 || transfer->status == 206;
  // ...and only once this response has said whether the bytes already on disk
  // are still part of it. Until the first body byte arrives, `resume_from` is a
  // prefix a 200 is about to throw away, so a figure built on it would claim a
  // file larger than the one that ends up there and then fall back. There is
  // nothing to lose by waiting: it is the first buffer of the transfer.
  if (transfer->progress != nullptr && content && !transfer->restart_needed) {
    // `resume_from + received`, not libcurl's `dlnow`: what the *file* holds,
    // which is what `ProgressCallback` promises and what a resumed download's
    // bar is drawn from. `resume_from` is this backend's own bookkeeping and is
    // zeroed the moment a 200 makes it start the file over, so the figure
    // follows the bytes rather than outliving them.
    //
    // `dltotal` is this response's body, so the whole file is `resume_from`
    // plus it: the resource on a resume, the slice on a caller's `range_start`.
    // Zero means the server declared nothing; negative never happens, but is
    // floored rather than cast, because a `curl_off_t` is signed.
    const std::uint64_t declared = dltotal > 0 ? static_cast<std::uint64_t>(dltotal) : 0;
    (*transfer->progress)(transfer->resume_from + transfer->received,
                          declared == 0 ? 0 : transfer->resume_from + declared);
  }
  return 0;
}

Error Classify(CURLcode code, const Transfer& transfer) {
  switch (code) {
    case CURLE_OK:
      return Error::kNone;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
      return Error::kInvalidRequest;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
      return Error::kUnresolvedHost;
    case CURLE_COULDNT_CONNECT:
      return Error::kConnectFailed;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_PEER_FAILED_VERIFICATION:
      return Error::kTls;
    case CURLE_OPERATION_TIMEDOUT:
      return Error::kTimeout;
    case CURLE_ABORTED_BY_CALLBACK:
      return Error::kCanceled;
    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
      // The connection died with bytes still owed. This is the shape a reset
      // mid-download takes, and the one the fault proxy's `drop` mode forces.
      return Error::kTruncated;
    case CURLE_WRITE_ERROR:
      // Our write callback is the only thing that can short-change libcurl, and
      // it only does so when the destination file refused the bytes.
      return transfer.write_failed ? Error::kWriteFailed : Error::kTransport;
    default:
      return Error::kTransport;
  }
}

/// Size of an existing file, or zero when it is not there.
///
/// Not ftell: its `long` is not a file offset, and a rom past 2 GiB would come
/// back negative, be read as "nothing to resume", and quietly restart a
/// multi-gigabyte download from zero.
std::uint64_t FileSize(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

/// Owns one easy handle and the lists hung off it, so every early return frees
/// them. libcurl requires the slists and mime part to outlive curl_easy_perform.
class Exchange {
 public:
  Exchange() : handle_(curl_easy_init()) {}

  ~Exchange() {
    if (mime_ != nullptr) {
      curl_mime_free(mime_);
    }
    if (header_list_ != nullptr) {
      curl_slist_free_all(header_list_);
    }
    if (handle_ != nullptr) {
      curl_easy_cleanup(handle_);
    }
  }

  Exchange(const Exchange&) = delete;
  Exchange& operator=(const Exchange&) = delete;

  CURL* handle() const { return handle_; }

  void AddHeader(const std::string& line) {
    header_list_ = curl_slist_append(header_list_, line.c_str());
  }

  void ApplyHeaders() {
    if (header_list_ != nullptr) {
      curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, header_list_);
    }
  }

  curl_mime* NewMime() {
    mime_ = curl_mime_init(handle_);
    return mime_;
  }

 private:
  CURL* handle_ = nullptr;
  curl_slist* header_list_ = nullptr;
  curl_mime* mime_ = nullptr;
};

class CurlHttpClient final : public http::HttpClient {
 public:
  explicit CurlHttpClient(http::ClientOptions options) : options_(std::move(options)) {
    if (options_.user_agent.empty()) {
      options_.user_agent = kUserAgent;
    }
    EnsureGlobalInit();
  }

  Result Send(const Request& request) override {
    Result result;
    Transfer transfer;
    transfer.request = &request;
    transfer.body = &result.response.body;

    Exchange exchange;
    if (!Prepare(exchange, request, transfer, result)) {
      return result;
    }
    curl_easy_setopt(exchange.handle(), CURLOPT_WRITEFUNCTION, OnBodyToString);
    curl_easy_setopt(exchange.handle(), CURLOPT_WRITEDATA, &transfer);

    Perform(exchange, transfer, result);
    return result;
  }

  Result Download(const Request& request, const DownloadTarget& target) override {
    Result result;
    if (target.path.empty()) {
      result.error = Error::kInvalidRequest;
      result.message = "download target path is empty";
      return result;
    }

    // The one spelling of the staging name, so `core/` can look at the same
    // file this writes without knowing the suffix (http.hpp).
    const std::string partial_path = http::PartialPathFor(target.path);

    // A caller-set range means "fetch this slice" and starts the file fresh; a
    // resume means "finish this file" and is the only case that keeps the bytes
    // already on disk.
    const std::uint64_t already_have =
        (target.resume && request.range_start == 0) ? FileSize(partial_path) : 0;
    const bool resuming = already_have > 0;

    Request effective = request;
    if (resuming) {
      effective.range_start = already_have;
    }

    Transfer transfer;
    transfer.request = &effective;
    transfer.resume_from = already_have;
    transfer.restart_needed = resuming;
    transfer.progress = target.progress ? &target.progress : nullptr;
    transfer.file = OpenPartial(partial_path, resuming);
    if (transfer.file == nullptr) {
      result.error = Error::kWriteFailed;
      result.message = "could not open " + partial_path + ": " + std::strerror(errno);
      return result;
    }

    Exchange exchange;
    if (!Prepare(exchange, effective, transfer, result)) {
      std::fclose(transfer.file);
      return result;
    }
    curl_easy_setopt(exchange.handle(), CURLOPT_WRITEFUNCTION, OnBodyToFile);
    curl_easy_setopt(exchange.handle(), CURLOPT_WRITEDATA, &transfer);

    Perform(exchange, transfer, result);

    // Still set means no body ever reached the write callback, so this response
    // never confirmed the bytes already on disk. They are not part of it, and
    // counting them would let an empty 200 promote a stale partial file.
    if (transfer.restart_needed) {
      transfer.resume_from = 0;
    }

    const bool flushed = std::fflush(transfer.file) == 0 && fsync(fileno(transfer.file)) == 0;
    const std::uint64_t written = transfer.resume_from + transfer.received;
    std::fclose(transfer.file);
    transfer.file = nullptr;

    result.response.body = std::move(transfer.error_body);
    if (!result.ok()) {
      return result;  // the partial file stays behind for a later resume
    }

    const int status = result.response.status;
    if (resuming && status == 416) {
      // Our offset is not valid for this resource any more, so the bytes we
      // were building on are worthless -- and keeping them would make every
      // future resume ask for the same impossible range. Start the next attempt
      // clean instead.
      std::remove(partial_path.c_str());
      result.error = Error::kTruncated;
      result.message = "the partial file no longer matches the resource; retry from the start";
      return result;
    }
    if (status != 200 && status != 206) {
      // Not a body-bearing response: a status the caller has to handle, with
      // nothing at the destination. Do not leave the empty part file we just
      // created lying next to it.
      if (!resuming) {
        std::remove(partial_path.c_str());
      }
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
    if (expected == 0 && !got_slice) {
      expected = result.response.declared_size;
    }
    if (resuming && expected == 0) {
      // Stitching two halves together is only safe if we can check the seam.
      result.error = Error::kTruncated;
      result.message = "resumed download cannot be verified: the server declared no size";
      return result;
    }
    if (expected != 0 && written != expected) {
      result.error = Error::kTruncated;
      result.message = "expected " + std::to_string(expected) + " bytes, wrote " +
                       std::to_string(written);
      return result;
    }

    if (std::rename(partial_path.c_str(), target.path.c_str()) != 0) {
      result.error = Error::kWriteFailed;
      result.message = "could not rename " + partial_path + ": " + std::strerror(errno);
    }
    return result;
  }

 private:
  static std::FILE* OpenPartial(const std::string& path, bool keep_existing) {
    if (!keep_existing) {
      return std::fopen(path.c_str(), "wb");
    }
    // "r+b" rather than "ab": O_APPEND would ignore the seek we need when the
    // server turns out to have ignored our Range header.
    std::FILE* file = std::fopen(path.c_str(), "r+b");
    if (file == nullptr) {
      return std::fopen(path.c_str(), "wb");
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
      std::fclose(file);
      return nullptr;
    }
    return file;
  }

  /// Everything both entry points configure. Returns false with `result` filled
  /// in when the request cannot be made at all.
  bool Prepare(Exchange& exchange, const Request& request, Transfer& transfer,
               Result& result) const {
    CURL* curl = exchange.handle();
    if (curl == nullptr) {
      result.error = Error::kTransport;
      result.message = "curl_easy_init failed";
      return false;
    }
    if (request.url.empty()) {
      result.error = Error::kInvalidRequest;
      result.message = "request url is empty";
      return false;
    }
    if (request.method != Method::kPost && request.method != Method::kPut &&
        (!request.form.empty() || !request.body.empty())) {
      // Silently dropping it is how a caller ends up debugging the server.
      result.error = Error::kInvalidRequest;
      result.message = "a request body is only valid on POST and PUT";
      return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, options_.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, options_.follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, static_cast<long>(options_.max_redirects));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, options_.verify_peer ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, options_.verify_peer ? 2L : 0L);
    if (!options_.ca_bundle_path.empty()) {
      curl_easy_setopt(curl, CURLOPT_CAINFO, options_.ca_bundle_path.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(request.connect_timeout.count()));
    if (request.timeout.count() > 0) {
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
    }
    if (request.stall_timeout.count() > 0) {
      // "No byte has moved for N seconds" -- the timeout that can bound a
      // multi-gigabyte download without capping how long it may legitimately
      // take. Sub-second stall timeouts round up to libcurl's 1s granularity.
      const long seconds = std::max<long>(1, static_cast<long>(request.stall_timeout.count() / 1000));
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, seconds);
    }

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, OnHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);

    switch (request.method) {
      case Method::kGet:
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        break;
      case Method::kHead:
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        break;
      case Method::kPost:
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        break;
      case Method::kPut:
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        break;
      case Method::kDelete:
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        break;
    }

    if (!request.form.empty()) {
      curl_mime* mime = exchange.NewMime();
      for (const http::FormPart& part : request.form) {
        curl_mimepart* slot = curl_mime_addpart(mime);
        curl_mime_name(slot, part.name.c_str());
        if (!part.file_path.empty()) {
          // Streams from disk. A save -- or a rom -- must never be read into
          // memory just to be posted.
          curl_mime_filedata(slot, part.file_path.c_str());
          if (!part.file_name.empty()) {
            curl_mime_filename(slot, part.file_name.c_str());
          }
        } else {
          curl_mime_data(slot, part.value.data(), part.value.size());
        }
        if (!part.content_type.empty()) {
          curl_mime_type(slot, part.content_type.c_str());
        }
      }
      curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    } else if (request.method == Method::kPost || request.method == Method::kPut) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(request.body.size()));
    }

    if (request.range_start > 0) {
      transfer.range = std::to_string(request.range_start) + "-";
      curl_easy_setopt(curl, CURLOPT_RANGE, transfer.range.c_str());
    }

    for (const http::Header& header : request.headers) {
      exchange.AddHeader(header.name + ": " + header.value);
    }
    // libcurl adds "Expect: 100-continue" to larger bodies. RomM answers it
    // fine, but the fault proxy is a plain BaseHTTPRequestHandler that does
    // not, and a one-second stall on every upload is not worth the round trip.
    exchange.AddHeader("Expect:");
    exchange.ApplyHeaders();
    return true;
  }

  void Perform(Exchange& exchange, Transfer& transfer, Result& result) const {
    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(exchange.handle(), CURLOPT_ERRORBUFFER, error_buffer);

    const CURLcode code = curl_easy_perform(exchange.handle());
    curl_easy_getinfo(exchange.handle(), CURLINFO_RESPONSE_CODE, &transfer.status);

    result.response.status = static_cast<int>(transfer.status);
    result.response.headers = std::move(transfer.headers);
    result.response.bytes_received = transfer.received;
    result.response.declared_size = DeclaredSize(result.response);

    result.error = Classify(code, transfer);
    if (!result.ok()) {
      result.message = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code);
    }
  }

  /// What the server said the whole resource weighs: the total from a
  /// `Content-Range` on a 206, otherwise `Content-Length`.
  static std::uint64_t DeclaredSize(const Response& response) {
    if (const std::string* range = http::FindHeader(response.headers, "Content-Range")) {
      if (const std::uint64_t total = TotalFromContentRange(*range); total != 0) {
        return total;
      }
    }
    if (const std::string* length = http::FindHeader(response.headers, "Content-Length")) {
      if (length->empty()) {
        return 0;
      }
      std::uint64_t value = 0;
      for (const char c : *length) {
        if (c < '0' || c > '9') {
          return 0;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
      }
      return value;
    }
    return 0;
  }

  http::ClientOptions options_;
};

}  // namespace

std::unique_ptr<http::HttpClient> MakeCurlHttpClient(const http::ClientOptions& options) {
  return std::make_unique<CurlHttpClient>(options);
}

}  // namespace rommsync::host
