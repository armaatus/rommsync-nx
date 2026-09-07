// Proves the whole test rig works end to end: a real RomM in Docker, reached
// through the fault proxy, driven from a CTest-registered binary.
//
// It asserts three things:
//   1. RomM answers /api/heartbeat through the proxy   (the rig is up)
//   2. an armed fault turns that same call into a 401  (failures can be forced)
//   3. the fault auto-disarms after one use            (tests do not leak state)
//
// Point 2 is the important one. Without it "green" only ever means the happy
// path was tried, and the backup-before-overwrite guarantee in SYNC_PROTOCOL.md
// would never actually be exercised.
//
// This uses libcurl directly, because it is testing the RIG rather than our own
// code. Production HTTP goes through the HttpClient interface built in M0-2.

#include <curl/curl.h>
#include <unistd.h>  // getpid: the owner tag below is per process

#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

namespace {

constexpr int kSkip = 77;  // CTest SKIP_RETURN_CODE -- see tests/CMakeLists.txt

/// Whose fault this process arms and claims. One proxy serves every client that
/// dials this worktree, and an untagged scenario is claimed by whichever request
/// arrives next -- including one from a second `ctest` running against the same
/// rig, which is how this file used to fail with "expected the armed fault to
/// yield 401" having done nothing wrong (#118). tests/rig.hpp says the same
/// thing for the tests that speak through `HttpClient`; this one is raw libcurl
/// on purpose, so it repeats it rather than sharing it -- random suffix
/// included, because the proxy outlives this process and pids come round again.
const std::string& OwnerTag() {
  static const std::string tag = "rig-smoke-" +
                                 std::to_string(static_cast<long>(::getpid())) + "-" +
                                 std::to_string(std::random_device{}());
  return tag;
}

std::size_t Collect(char* data, std::size_t size, std::size_t nmemb, void* out) {
  static_cast<std::string*>(out)->append(data, size * nmemb);
  return size * nmemb;
}

struct Response {
  CURLcode code = CURLE_OK;
  long status = 0;
  std::string body;
};

Response Request(const std::string& url, const char* method = "GET",
                 const std::string& payload = {}) {
  Response response;
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    response.code = CURLE_FAILED_INIT;
    return response;
  }

  curl_slist* headers = curl_slist_append(nullptr, ("X-Fault-Owner: " + OwnerTag()).c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  if (!payload.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  }

  response.code = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  return response;
}

std::string BaseUrl() {
  if (const char* override_url = std::getenv("PROXY_BASE_URL")) {
    return override_url;
  }
  return ROMMSYNC_PROXY_BASE_URL;
}

}  // namespace

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  const std::string base = BaseUrl();
  const std::string heartbeat = base + "/api/heartbeat";
  const std::string control = base + "/__fault";
  int failures = 0;

  // 1. the rig is reachable ------------------------------------------------
  const Response live = Request(heartbeat);
  if (live.code != CURLE_OK) {
    std::cerr << "rig unreachable at " << base << ": " << curl_easy_strerror(live.code)
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    curl_global_cleanup();
    return kSkip;
  }
  if (live.status != 200) {
    std::cerr << "expected 200 from " << heartbeat << ", got " << live.status << "\n";
    ++failures;
  }

  // 2. a fault can be armed and it takes effect ----------------------------
  const Response armed =
      Request(control, "POST", R"({"mode":"status","status":401,"path":"/api/heartbeat"})");
  if (armed.status != 200) {
    std::cerr << "could not arm a fault: status " << armed.status << "\n";
    ++failures;
  }

  const Response injected = Request(heartbeat);
  if (injected.status != 401) {
    std::cerr << "expected the armed fault to yield 401, got " << injected.status << "\n";
    ++failures;
  }

  // 3. and it disarms itself, so scenarios do not leak between tests -------
  const Response after = Request(heartbeat);
  if (after.status != 200) {
    std::cerr << "fault did not auto-disarm; got " << after.status << " on the next call\n";
    ++failures;
  }

  Request(control, "DELETE");  // belt: leave the proxy clean regardless
  curl_global_cleanup();

  if (failures == 0) {
    std::cout << "rig ok: real RomM via fault proxy at " << base << "\n";
  }
  return failures == 0 ? 0 : 1;
}
