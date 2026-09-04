// Shared helpers for tests that talk to this worktree's RomM through the fault
// proxy. See docs/TESTING.md; the ports come from .env via tests/CMakeLists.txt.
//
// These tests assert on HTTP behaviour, not on payload shapes, and the few
// fields they read out of a RomM response are read with a substring scan that
// fails loudly rather than silently returning something plausible. Anything
// that cares about a *response shape* uses rommsync::json instead -- see
// tests/test_auth_shapes.cpp.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "checks.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/http.hpp"

namespace rig {

namespace http = rommsync::http;

/// CTest's SKIP_RETURN_CODE, matching tests/CMakeLists.txt and test_rig_smoke.
inline constexpr int kSkip = 77;

/// The fixture account these tests create and then log in as.
///
/// Not a secret: this RomM is a throwaway container bound to 127.0.0.1 that
/// holds no real data, and the same reasoning already applies to the database
/// password in server/testing/docker-compose.yml. Keeping the credentials
/// constant is what lets any worktree, old or new, provision itself.
inline constexpr const char* kUser = "rommsync";
inline constexpr const char* kPassword = "rommsync-test-only";
inline constexpr const char* kEmail = "rommsync@example.invalid";

inline std::string BaseUrl() {
  if (const char* override_url = std::getenv("PROXY_BASE_URL")) {
    return override_url;
  }
  return ROMMSYNC_PROXY_BASE_URL;
}

inline std::string ScratchDir() { return ROMMSYNC_TEST_SCRATCH; }

// --- assertions ---------------------------------------------------------------

/// The shared harness (tests/checks.hpp) plus the two HTTP-shaped assertions
/// only the rig tests need.
class Checks : public ::checks::Checks {
 public:
  void ExpectOk(const http::Result& result, std::string_view what) {
    if (!result.ok()) {
      Fail(std::string(what) + " -- " + http::ToString(result.error) + ": " + result.message);
    }
  }

  void ExpectError(const http::Result& result, http::Error expected, std::string_view what) {
    if (result.error != expected) {
      Fail(std::string(what) + " -- expected " + http::ToString(expected) + ", got " +
           http::ToString(result.error) + " (" + result.message + ")");
    }
  }
};

// --- json-ish scraping -------------------------------------------------------

/// Value of a `"key": "value"` pair. Returns an empty string when the key is
/// absent or its value is not a string (`null` included).
inline std::string JsonString(std::string_view body, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":";
  std::size_t at = body.find(needle);
  if (at == std::string_view::npos) {
    return {};
  }
  at += needle.size();
  while (at < body.size() && body[at] == ' ') {
    ++at;
  }
  if (at >= body.size() || body[at] != '"') {
    return {};
  }
  ++at;
  const std::size_t end = body.find('"', at);
  if (end == std::string_view::npos) {
    return {};
  }
  return std::string(body.substr(at, end - at));
}

/// Value of a `"key": 123` pair, as written. Empty when absent or not a number.
inline std::string JsonNumber(std::string_view body, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":";
  std::size_t at = body.find(needle);
  if (at == std::string_view::npos) {
    return {};
  }
  at += needle.size();
  while (at < body.size() && body[at] == ' ') {
    ++at;
  }
  std::size_t end = at;
  while (end < body.size() && body[end] >= '0' && body[end] <= '9') {
    ++end;
  }
  return std::string(body.substr(at, end - at));
}

// --- files -------------------------------------------------------------------

inline std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline bool WriteFile(const std::string& path, std::string_view content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return out.good();
}

// --- the rig ------------------------------------------------------------------

/// Arm a fault-proxy scenario. `spec` is the JSON body documented in
/// server/testing/fault_proxy.py.
inline http::Result ArmFault(http::HttpClient& client, const std::string& base,
                             const std::string& spec) {
  http::Request request;
  request.method = http::Method::kPost;
  request.url = base + "/__fault";
  request.headers.push_back({"Content-Type", "application/json"});
  request.body = spec;
  return client.Send(request);
}

inline void DisarmFault(http::HttpClient& client, const std::string& base) {
  http::Request request;
  request.method = http::Method::kDelete;
  request.url = base + "/__fault";
  client.Send(request);
}

/// True when this worktree's RomM answers through the proxy.
inline bool Reachable(http::HttpClient& client, const std::string& base) {
  http::Request request;
  request.url = base + "/api/heartbeat";
  request.timeout = std::chrono::milliseconds{10'000};
  const http::Result result = client.Send(request);
  return result.successful();
}

inline bool SameHeaderName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char lhs = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
    const char rhs = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
    if (lhs != rhs) {
      return false;
    }
  }
  return true;
}

/// Value of one cookie out of the response's `Set-Cookie` headers, or an empty
/// string. There can be several, so this scans them all rather than taking the
/// first the way FindHeader would.
inline std::string CookieValue(const http::Headers& headers, std::string_view name) {
  const std::string prefix = std::string(name) + "=";
  for (const http::Header& header : headers) {
    if (!SameHeaderName(header.name, "Set-Cookie") || header.value.rfind(prefix, 0) != 0) {
      continue;
    }
    const std::size_t end = header.value.find(';', prefix.size());
    return header.value.substr(prefix.size(),
                               end == std::string::npos ? std::string::npos : end - prefix.size());
  }
  return {};
}

/// Create the fixture admin account if this RomM does not have one yet.
///
/// A freshly created worktree gets an empty RomM, and every authenticated
/// scenario below needs an account. Doing it here rather than in a provisioning
/// script means a worktree that predates this test also just works, and it
/// exercises the cookie + CSRF header path the M1 auth work will need anyway.
inline void EnsureUser(http::HttpClient& client, const std::string& base) {
  http::Request probe;
  probe.url = base + "/api/heartbeat";
  const http::Result seen = client.Send(probe);
  const std::string csrf = CookieValue(seen.response.headers, "romm_csrftoken");

  http::Request create;
  create.method = http::Method::kPost;
  create.url = base + "/api/users";
  create.headers.push_back({"Content-Type", "application/json"});
  if (!csrf.empty()) {
    // RomM double-submits its CSRF token: the cookie it just set has to come
    // back both as a cookie and as a header.
    create.headers.push_back({"Cookie", "romm_csrftoken=" + csrf});
    create.headers.push_back({"x-csrftoken", csrf});
  }
  create.body = std::string("{\"username\":\"") + kUser + "\",\"password\":\"" + kPassword +
                "\",\"email\":\"" + kEmail + "\",\"role\":\"admin\"}";
  // 201 means we just created it; anything else means RomM already has an admin
  // and refused, which is the normal case on the second run. Login decides.
  client.Send(create);
}

/// Log in and return a bearer token, or an empty string with the reason on
/// stderr. Requests every scope the tests touch.
inline std::string Login(http::HttpClient& client, const std::string& base) {
  http::Request request;
  request.method = http::Method::kPost;
  request.url = base + "/api/token";
  request.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
  request.body = std::string("grant_type=password&username=") + kUser + "&password=" + kPassword +
                 "&scope=me.read+collections.read+collections.write";

  const http::Result result = client.Send(request);
  if (!result.successful()) {
    std::cerr << "  login failed: " << http::ToString(result.error) << " status "
              << result.response.status << " " << result.response.body << "\n";
    return {};
  }
  const std::string token = JsonString(result.response.body, "access_token");
  if (token.empty()) {
    std::cerr << "  login response had no access_token: " << result.response.body << "\n";
  }
  return token;
}

/// Path of RomM's own frontend bundle, e.g. `/assets/index-CF4gpQFQ.js`.
///
/// The streaming tests need a resource that is big enough to interrupt half way
/// through and that nginx serves with real `Range` support. The library's roms
/// would be the natural choice, and since M0-6 they are actually available:
/// server/testing/provision.py drives the socket.io scan, so a provisioned
/// worktree has a scanned library rather than an empty one. Switching these
/// tests onto a real rom is M0-5's call, not a rig helper's -- the bundle is a
/// genuine ~1.6 MB file served by the same nginx over the same proxy, which is
/// what these tests are actually about, and it needs no auth.
/// It is discovered rather than hardcoded so a RomM bump does not break them.
inline std::string DiscoverLargeAsset(http::HttpClient& client, const std::string& base) {
  http::Request request;
  request.url = base + "/";
  const http::Result result = client.Send(request);
  if (!result.successful()) {
    return {};
  }
  const std::string_view body = result.response.body;
  // Bounded to the one quoted attribute it starts in: RomM's HTML also links an
  // `index-<hash>.css`, and scanning past the closing quote for the next ".js"
  // would return a "path" made of intervening markup.
  for (std::size_t at = body.find("/assets/index-"); at != std::string_view::npos;
       at = body.find("/assets/index-", at + 1)) {
    const std::size_t end = body.find('"', at);
    if (end == std::string_view::npos) {
      break;
    }
    const std::string_view candidate = body.substr(at, end - at);
    if (candidate.size() > 3 && candidate.substr(candidate.size() - 3) == ".js") {
      return std::string(candidate);
    }
  }
  return {};
}

// --- a deterministic PNG, so no binary fixture has to live in the repo --------

namespace detail {

inline std::uint32_t Crc32(const std::string& data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char byte : data) {
    crc ^= static_cast<unsigned char>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

inline void AppendBigEndian(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

inline void AppendChunk(std::string& out, const char (&type)[5], const std::string& payload) {
  AppendBigEndian(out, static_cast<std::uint32_t>(payload.size()));
  const std::string typed = std::string(type) + payload;
  out += typed;
  AppendBigEndian(out, Crc32(typed));
}

/// zlib stream (RFC 1950) wrapping deflate stored blocks (RFC 1951 s3.2.4).
/// Storing rather than compressing keeps this to a few lines and avoids taking
/// a dependency on zlib for a test fixture.
inline std::string ZlibStored(const std::string& raw) {
  std::string out;
  out.push_back(0x78);  // CMF: deflate, 32K window
  out.push_back(0x01);  // FLG: no dictionary, check bits make 0x7801 % 31 == 0
  std::size_t at = 0;
  do {
    const std::size_t take = std::min<std::size_t>(raw.size() - at, 65535);
    const bool last = (at + take) >= raw.size();
    out.push_back(static_cast<char>(last ? 1 : 0));
    out.push_back(static_cast<char>(take & 0xFF));
    out.push_back(static_cast<char>((take >> 8) & 0xFF));
    out.push_back(static_cast<char>(~take & 0xFF));
    out.push_back(static_cast<char>((~take >> 8) & 0xFF));
    out.append(raw, at, take);
    at += take;
  } while (at < raw.size());

  std::uint32_t s1 = 1;
  std::uint32_t s2 = 0;
  for (const char byte : raw) {
    s1 = (s1 + static_cast<unsigned char>(byte)) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  AppendBigEndian(out, (s2 << 16) | s1);
  return out;
}

}  // namespace detail

/// Minimal PNG encoder: 24-bit RGB, zlib with stored (uncompressed) blocks.
///
/// The multipart test needs a real image, because the only way RomM proves it
/// received the file part is by processing it into a cover. Generating one keeps
/// a binary blob out of the tree and keeps the fixture deterministic.
inline std::string MakePng(int width, int height) {
  std::string raw;
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);  // filter type 0 (none) for this scanline
    for (int x = 0; x < width; ++x) {
      raw.push_back(static_cast<char>(x * 4));
      raw.push_back(static_cast<char>(y * 4));
      raw.push_back(static_cast<char>((x + y) * 2));
    }
  }

  std::string header;
  detail::AppendBigEndian(header, static_cast<std::uint32_t>(width));
  detail::AppendBigEndian(header, static_cast<std::uint32_t>(height));
  header.push_back(8);  // 8 bits per channel
  header.push_back(2);  // colour type 2: truecolour RGB
  header.append(3, '\0');  // deflate, adaptive filtering, no interlace

  std::string png("\x89PNG\r\n\x1a\n", 8);
  detail::AppendChunk(png, "IHDR", header);
  detail::AppendChunk(png, "IDAT", detail::ZlibStored(raw));
  detail::AppendChunk(png, "IEND", std::string());
  return png;
}

}  // namespace rig
