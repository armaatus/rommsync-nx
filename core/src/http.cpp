// The portable half of the HttpClient surface: the bits every backend needs and
// none of them should reimplement. No transport lives here -- see host/ for the
// libcurl backend and (from M0-3) sysmodule/ for the Horizon `ssl` one.
#include "rommsync/http.hpp"

#include <string>
#include <string_view>

#include "rommsync/disk.hpp"

namespace rommsync::http {
namespace {

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (Lower(a[i]) != Lower(b[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

Error ErrorForWrite(int error_number) {
  switch (io::RefusalOf(error_number)) {
    case io::Refusal::kFull:
      return Error::kDiskFull;
    case io::Refusal::kFileTooLarge:
      return Error::kFileTooLarge;
    case io::Refusal::kRefused:
      break;
  }
  return Error::kWriteFailed;
}

bool AnsweredTooLarge(const Result& result) {
  return result.error == Error::kTooLarge && result.response.status >= 200 &&
         result.response.status < 300;
}

const char* ToString(Error error) {
  switch (error) {
    case Error::kNone:           return "ok";
    case Error::kInvalidRequest: return "invalid request";
    case Error::kUnresolvedHost: return "host could not be resolved";
    case Error::kConnectFailed:  return "connection failed";
    case Error::kTls:            return "TLS failure";
    case Error::kTimeout:        return "timed out";
    case Error::kCanceled:       return "canceled";
    case Error::kTruncated:      return "body ended early";
    case Error::kWriteFailed:    return "the card refused the write to the destination file";
    case Error::kDiskFull:       return "the card is full";
    case Error::kFileTooLarge:   return "the file is too large for the card's format";
    case Error::kTransport:      return "transport failure";
    case Error::kTooLarge:       return "the response is larger than this client holds";
  }
  return "unknown error";
}

std::string JoinUrl(std::string_view server_url, std::string_view path) {
  while (!server_url.empty() && server_url.back() == '/') {
    server_url.remove_suffix(1);
  }
  return std::string(server_url) + std::string(path);
}

const std::string* FindHeader(const Headers& headers, std::string_view name) {
  for (const Header& header : headers) {
    if (EqualsIgnoreCase(header.name, name)) {
      return &header.value;
    }
  }
  return nullptr;
}

std::string PartialPathFor(std::string_view path) { return std::string(path) + ".part"; }

std::string EncodeQueryValue(std::string_view value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (const char character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    const bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                            byte == '.' || byte == '~';
    if (unreserved) {
      out.push_back(character);
    } else {
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0F]);
    }
  }
  return out;
}

}  // namespace rommsync::http
