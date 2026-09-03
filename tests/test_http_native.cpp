// The native HttpClient backend, exercised against the real docker RomM.
//
// One scenario per CTest entry (`http.get`, `http.drop`, ...), selected by
// argv[1], so a failure names the behaviour that broke rather than "the http
// tests". Every scenario runs through the fault proxy, and the ones that need a
// failure the server will not produce on demand arm it first -- there is no
// mock anywhere in here (docs/TESTING.md).
//
// The interesting half is the failure scenarios. A downloader that works on the
// happy path and quietly leaves a half-written rom behind when the connection
// dies is worse than one that does not work at all, so `drop`, `truncate`,
// `cancel` and `stall` all assert on what is NOT on disk afterwards.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include "rig.hpp"

namespace {

namespace http = rommsync::http;

using Clock = std::chrono::steady_clock;

std::string Scratch(const std::string& name) {
  return rig::ScratchDir() + "/" + name;
}

/// Remove a destination and any partial file left from an earlier run, so a
/// scenario's assertions are about what this run did.
void Clear(const std::string& path) {
  std::filesystem::remove(path);
  std::filesystem::remove(path + ".part");
}

bool Exists(const std::string& path) { return std::filesystem::exists(path); }

std::uint64_t SizeOf(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

long ElapsedMs(Clock::time_point since) {
  return static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

// --- scenarios ---------------------------------------------------------------

// A plain GET of a genuine RomM response, and the accessors around it.
int Get(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;

  http::Request request;
  request.url = base + "/api/heartbeat";
  const http::Result result = client.Send(request);

  checks.ExpectOk(result, "GET /api/heartbeat");
  checks.ExpectEq(result.response.status, 200, "status");
  checks.Expect(result.successful(), "successful()");
  checks.Expect(result.response.body.find("\"VERSION\"") != std::string::npos,
                "body carries RomM's version block");

  // Header lookup is case-insensitive because the wire is.
  const std::string* type = http::FindHeader(result.response.headers, "CONTENT-TYPE");
  checks.Expect(type != nullptr && type->find("json") != std::string::npos,
                "Content-Type found regardless of case");
  checks.ExpectEq(result.response.bytes_received, result.response.body.size(),
                  "bytes_received matches the body");
  checks.ExpectEq(result.response.declared_size, result.response.body.size(),
                  "declared_size matches Content-Length");
  return checks.failures();
}

// An HTTP error status is a delivered response, not a transport error -- and an
// armed fault disarms itself, so scenarios do not leak into each other.
int Status(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;

  http::Request unauthenticated;
  unauthenticated.url = base + "/api/collections";
  const http::Result denied = client.Send(unauthenticated);
  checks.ExpectOk(denied, "an unauthenticated GET still completes");
  checks.ExpectEq(denied.response.status, 401, "status");
  checks.Expect(!denied.successful(), "401 is not successful()");

  rig::ArmFault(client, base, R"({"mode":"status","status":503,"path":"/api/heartbeat"})");

  http::Request request;
  request.url = base + "/api/heartbeat";
  const http::Result faulted = client.Send(request);
  checks.ExpectOk(faulted, "a 503 is a response, not a failure");
  checks.ExpectEq(faulted.response.status, 503, "injected status");

  const http::Result after = client.Send(request);
  checks.ExpectEq(after.response.status, 200, "the fault disarmed itself");

  rig::DisarmFault(client, base);
  return checks.failures();
}

// A JSON POST against the endpoint the device-code flow (M1) actually starts on.
int PostJson(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;

  http::Request request;
  request.method = http::Method::kPost;
  request.url = base + "/api/auth/device/init";
  request.headers.push_back({"Content-Type", "application/json"});
  request.body =
      R"({"client_device_identifier":"rommsync-nx-http-test","name":"http test",)"
      R"("client":"rommsync-nx","platform":"switch","client_version":"0.0.0",)"
      R"("requested_scopes":["me.read"]})";

  const http::Result result = client.Send(request);
  checks.ExpectOk(result, "POST /api/auth/device/init");
  checks.ExpectEq(result.response.status, 201, "status");
  checks.Expect(!rig::JsonString(result.response.body, "device_code").empty(),
                "RomM returned a device_code");
  // Nothing approves the code, so it expires on its own and no state leaks.
  return checks.failures();
}

// A form-urlencoded POST: RomM's token endpoint, which is also how every
// authenticated scenario below gets its bearer token.
int PostForm(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  rig::EnsureUser(client, base);

  const std::string token = rig::Login(client, base);
  checks.Expect(!token.empty(), "POST /api/token returned an access_token");
  return checks.failures();
}

// multipart/form-data with a file part streamed from disk. RomM only proves it
// received the bytes by turning them into a cover, so that is what we assert.
int Multipart(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  rig::EnsureUser(client, base);
  const std::string token = rig::Login(client, base);
  if (token.empty()) {
    return 1;
  }

  const std::string artwork = Scratch("multipart-artwork.png");
  if (!rig::WriteFile(artwork, rig::MakePng(64, 64))) {
    std::cerr << "  could not write " << artwork << "\n";
    return 1;
  }

  http::Request request;
  request.method = http::Method::kPost;
  request.url = base + "/api/collections";
  request.headers.push_back({"Authorization", "Bearer " + token});
  request.form.push_back({"name", "m0-2-http-multipart", "", "", ""});
  request.form.push_back({"description", "created by tests/test_http_native.cpp", "", "", ""});
  request.form.push_back({"artwork", "", artwork, "cover.png", "image/png"});

  const http::Result result = client.Send(request);
  checks.ExpectOk(result, "POST /api/collections (multipart)");
  checks.ExpectEq(result.response.status, 200, "status");
  checks.ExpectEq(rig::JsonString(result.response.body, "name"), std::string("m0-2-http-multipart"),
                  "the text parts arrived");
  checks.Expect(!rig::JsonString(result.response.body, "path_cover_small").empty(),
                "the file part arrived: RomM built a cover from it");

  const std::string id = rig::JsonNumber(result.response.body, "id");
  if (!id.empty()) {
    http::Request cleanup;
    cleanup.method = http::Method::kDelete;
    cleanup.url = base + "/api/collections/" + id;
    cleanup.headers.push_back({"Authorization", "Bearer " + token});
    const http::Result removed = client.Send(cleanup);
    checks.Expect(removed.successful(), "the collection was cleaned up again");
  }
  return checks.failures();
}

// Streaming a whole resource to disk, without ever holding it in memory.
int Download(http::HttpClient& client, const std::string& base, const std::string& asset) {
  rig::Checks checks;

  http::Request reference_request;
  reference_request.url = base + asset;
  const http::Result reference = client.Send(reference_request);
  checks.Expect(reference.successful(), "fetched the reference copy");

  const std::string path = Scratch("download.bin");
  Clear(path);

  http::Request request;
  request.url = base + asset;
  request.timeout = std::chrono::milliseconds{0};  // bounded by stall_timeout
  const http::Result result = client.Download(request, {path, false, 0});

  checks.ExpectOk(result, "download");
  checks.ExpectEq(result.response.status, 200, "status");
  checks.Expect(result.response.body.empty(), "the body went to disk, not into memory");
  checks.ExpectEq(SizeOf(path), reference.response.body.size(), "file size");
  checks.ExpectEq(rig::ReadFile(path), reference.response.body, "file contents");
  checks.Expect(!Exists(path + ".part"), "the partial file was renamed away");
  return checks.failures();
}

// Range: fetch a slice and prove it is the right slice.
int Range(http::HttpClient& client, const std::string& base, const std::string& asset) {
  rig::Checks checks;
  constexpr std::uint64_t kOffset = 1'000'000;

  http::Request reference_request;
  reference_request.url = base + asset;
  const http::Result reference = client.Send(reference_request);
  checks.Expect(reference.successful() && reference.response.body.size() > kOffset,
                "the reference copy is bigger than the offset");
  if (checks.failures() != 0) {
    return checks.failures();
  }

  const std::string path = Scratch("range.bin");
  Clear(path);

  http::Request request;
  request.url = base + asset;
  request.range_start = kOffset;
  const http::Result result = client.Download(request, {path, false, 0});

  checks.ExpectOk(result, "ranged download");
  checks.ExpectEq(result.response.status, 206, "the server honoured Range");
  checks.Expect(http::FindHeader(result.response.headers, "Content-Range") != nullptr,
                "Content-Range came back");
  checks.ExpectEq(result.response.declared_size, reference.response.body.size(),
                  "declared_size is the whole resource, from Content-Range");
  checks.ExpectEq(rig::ReadFile(path), reference.response.body.substr(kOffset),
                  "the file holds exactly the tail");
  return checks.failures();
}

// A TCP reset mid-body. This is the acceptance criterion that matters: it has to
// surface as an error, and it must not leave a truncated file at the
// destination where a complete one is expected.
int Drop(http::HttpClient& client, const std::string& base, const std::string& asset) {
  rig::Checks checks;
  constexpr std::uint64_t kCutAt = 100'000;

  const std::string path = Scratch("drop.bin");
  Clear(path);

  rig::ArmFault(client, base,
                R"({"mode":"drop","bytes":)" + std::to_string(kCutAt) + R"(,"path":")" + asset +
                    R"("})");

  http::Request request;
  request.url = base + asset;
  request.timeout = std::chrono::milliseconds{0};
  const http::Result result = client.Download(request, {path, false, 0});

  checks.ExpectError(result, http::Error::kTruncated, "a reset mid-body is an error");
  checks.Expect(!Exists(path), "no truncated file at the destination");
  // How much of the cut-off body survives the reset is up to the kernel, so the
  // assertion is that whatever arrived was kept and accounted for -- not an
  // exact count, which would flake.
  const std::uint64_t kept = SizeOf(path + ".part");
  checks.Expect(kept > 0 && kept <= kCutAt, "the bytes that did arrive are kept for a resume");
  checks.ExpectEq(result.response.bytes_received, kept, "and bytes_received matches them");

  rig::DisarmFault(client, base);
  return checks.failures();
}

// The other half of the same story: pick the interrupted download back up.
int Resume(http::HttpClient& client, const std::string& base, const std::string& asset) {
  rig::Checks checks;
  constexpr std::uint64_t kCutAt = 100'000;

  http::Request reference_request;
  reference_request.url = base + asset;
  const http::Result reference = client.Send(reference_request);
  checks.Expect(reference.successful(), "fetched the reference copy");

  const std::string path = Scratch("resume.bin");
  Clear(path);

  rig::ArmFault(client, base,
                R"({"mode":"drop","bytes":)" + std::to_string(kCutAt) + R"(,"path":")" + asset +
                    R"("})");

  http::Request request;
  request.url = base + asset;
  request.timeout = std::chrono::milliseconds{0};
  const http::Result interrupted = client.Download(request, {path, false, 0});
  checks.ExpectError(interrupted, http::Error::kTruncated, "the first attempt was cut short");

  // A reset does not deliver a predictable number of bytes, so the resume is
  // measured against what actually survived rather than against `bytes`.
  const std::uint64_t got_first = SizeOf(path + ".part");
  checks.Expect(got_first > 0 && got_first <= kCutAt, "the first attempt left a partial file");

  const http::Result resumed = client.Download(request, {path, true, 0});
  checks.ExpectOk(resumed, "the resumed attempt");
  checks.ExpectEq(resumed.response.status, 206, "the resume was a Range request");
  checks.ExpectEq(resumed.response.bytes_received, reference.response.body.size() - got_first,
                  "only the missing bytes were fetched");
  checks.ExpectEq(rig::ReadFile(path), reference.response.body,
                  "the two halves make the original file");
  checks.Expect(!Exists(path + ".part"), "the partial file was renamed away");

  rig::DisarmFault(client, base);
  return checks.failures();
}

// A server may ignore Range and send the whole resource anyway. Appending then
// would splice the file into itself, so the partial bytes have to be dropped.
// RomM's JSON endpoints do exactly this, which makes it testable for real.
int ResumeWithoutRangeSupport(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;

  http::Request reference_request;
  reference_request.url = base + "/api/heartbeat";
  const http::Result reference = client.Send(reference_request);
  checks.Expect(reference.successful(), "fetched the reference copy");

  const std::string path = Scratch("resume-no-range.bin");
  Clear(path);
  checks.Expect(rig::WriteFile(path + ".part", std::string(64, 'x')),
                "planted a partial file to resume from");

  http::Request request;
  request.url = base + "/api/heartbeat";
  const http::Result result = client.Download(request, {path, true, 0});

  checks.ExpectOk(result, "download");
  checks.ExpectEq(result.response.status, 200, "the server ignored Range");
  checks.ExpectEq(rig::ReadFile(path), reference.response.body,
                  "the stale partial bytes were discarded, not prepended");
  return checks.failures();
}

// A clean close mid-body is invisible to the transport: the server never said
// how long the body would be, so nothing about the exchange looks wrong. Only
// the size the caller already knows can catch it -- and it must, because this is
// the shape a silently truncated rom would take.
int Truncate(http::HttpClient& client, const std::string& base, const std::string& asset) {
  rig::Checks checks;
  constexpr std::uint64_t kCutAt = 100'000;

  http::Request reference_request;
  reference_request.url = base + asset;
  const http::Result reference = client.Send(reference_request);
  checks.Expect(reference.successful(), "fetched the reference copy");
  const std::uint64_t full_size = reference.response.body.size();

  const std::string blind = Scratch("truncate-blind.bin");
  Clear(blind);
  rig::ArmFault(client, base,
                R"({"mode":"truncate","bytes":)" + std::to_string(kCutAt) + R"(,"path":")" + asset +
                    R"("})");

  http::Request request;
  request.url = base + asset;
  request.timeout = std::chrono::milliseconds{0};
  const http::Result unchecked = client.Download(request, {blind, false, 0});
  checks.ExpectOk(unchecked, "a clean short close completes at the transport level");
  checks.ExpectEq(unchecked.response.declared_size, std::uint64_t{0},
                  "and the server declared no length to compare against");
  checks.ExpectEq(SizeOf(blind), kCutAt, "so the short body is all we got");

  const std::string guarded = Scratch("truncate-guarded.bin");
  Clear(guarded);
  rig::ArmFault(client, base,
                R"({"mode":"truncate","bytes":)" + std::to_string(kCutAt) + R"(,"path":")" + asset +
                    R"("})");
  const http::Result checked = client.Download(request, {guarded, false, full_size});
  checks.ExpectError(checked, http::Error::kTruncated,
                     "an expected_size turns the same body into an error");
  checks.Expect(!Exists(guarded), "and nothing lands at the destination");

  rig::DisarmFault(client, base);
  return checks.failures();
}

// A stalled server must not hang a sync tick. Both timeouts are exercised: the
// overall ceiling an API call gets, and the no-progress ceiling a download gets
// instead so a legitimately slow transfer is not killed for being slow.
int Stall(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  constexpr int kStallSeconds = 8;

  rig::ArmFault(client, base, R"({"mode":"stall","seconds":)" + std::to_string(kStallSeconds) +
                                  R"(,"path":"/api/heartbeat"})");
  http::Request bounded;
  bounded.url = base + "/api/heartbeat";
  bounded.timeout = std::chrono::milliseconds{1'500};
  Clock::time_point started = Clock::now();
  const http::Result timed_out = client.Send(bounded);
  checks.ExpectError(timed_out, http::Error::kTimeout, "the total timeout fired");
  checks.Expect(ElapsedMs(started) < kStallSeconds * 1000,
                "and it fired before the server would have answered");

  rig::ArmFault(client, base, R"({"mode":"stall","seconds":)" + std::to_string(kStallSeconds) +
                                  R"(,"path":"/api/heartbeat"})");
  http::Request unbounded;
  unbounded.url = base + "/api/heartbeat";
  unbounded.timeout = std::chrono::milliseconds{0};
  unbounded.stall_timeout = std::chrono::milliseconds{2'000};
  started = Clock::now();
  const http::Result stalled = client.Send(unbounded);
  checks.ExpectError(stalled, http::Error::kTimeout, "the stall timeout fired");
  checks.Expect(ElapsedMs(started) < kStallSeconds * 1000,
                "and it fired before the server would have answered");

  rig::DisarmFault(client, base);
  return checks.failures();
}

// The overlay's "stop" must not wait for a download to finish. Cancellation is
// cooperative and comes from another thread, so this is where a data race would
// show up too.
int Cancel(http::HttpClient& client, const std::string& base, const std::string& asset) {
  rig::Checks checks;
  constexpr int kStallSeconds = 8;

  const std::string path = Scratch("cancel.bin");
  Clear(path);

  rig::ArmFault(client, base, R"({"mode":"stall","seconds":)" + std::to_string(kStallSeconds) +
                                  R"(,"path":")" + asset + R"("})");

  http::CancelToken token;
  http::Request request;
  request.url = base + asset;
  request.timeout = std::chrono::milliseconds{0};
  request.cancel = &token;

  const Clock::time_point started = Clock::now();
  http::Result result;
  std::thread worker([&] { result = client.Download(request, {path, false, 0}); });
  std::this_thread::sleep_for(std::chrono::milliseconds{500});
  token.Cancel();
  worker.join();

  checks.ExpectError(result, http::Error::kCanceled, "the download was canceled");
  checks.Expect(ElapsedMs(started) < kStallSeconds * 1000, "and it stopped promptly");
  checks.Expect(!Exists(path), "a canceled download leaves nothing at the destination");

  rig::DisarmFault(client, base);
  return checks.failures();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: test_http_native <scenario>\n";
    return 2;
  }
  const std::string scenario = argv[1];
  const std::string base = rig::BaseUrl();

  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);
  if (error) {
    std::cerr << "could not create " << rig::ScratchDir() << ": " << error.message() << "\n";
    return 2;
  }

  const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  // Whatever an earlier run left armed would damage this one's first request.
  rig::DisarmFault(*client, base);

  // Only the streaming scenarios need it, and discovering it costs a request.
  std::string asset;
  if (scenario == "download" || scenario == "range" || scenario == "drop" ||
      scenario == "resume" || scenario == "truncate" || scenario == "cancel") {
    asset = rig::DiscoverLargeAsset(*client, base);
    if (asset.empty()) {
      std::cerr << "could not find a large asset to stream from " << base << "\n";
      return 1;
    }
  }

  int failures = 0;
  if (scenario == "get") {
    failures = Get(*client, base);
  } else if (scenario == "status") {
    failures = Status(*client, base);
  } else if (scenario == "post_json") {
    failures = PostJson(*client, base);
  } else if (scenario == "post_form") {
    failures = PostForm(*client, base);
  } else if (scenario == "multipart") {
    failures = Multipart(*client, base);
  } else if (scenario == "download") {
    failures = Download(*client, base, asset);
  } else if (scenario == "range") {
    failures = Range(*client, base, asset);
  } else if (scenario == "drop") {
    failures = Drop(*client, base, asset);
  } else if (scenario == "resume") {
    failures = Resume(*client, base, asset);
  } else if (scenario == "resume_no_range") {
    failures = ResumeWithoutRangeSupport(*client, base);
  } else if (scenario == "truncate") {
    failures = Truncate(*client, base, asset);
  } else if (scenario == "stall") {
    failures = Stall(*client, base);
  } else if (scenario == "cancel") {
    failures = Cancel(*client, base, asset);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (failures == 0) {
    std::cout << "http." << scenario << " ok against " << base << "\n";
  }
  return failures == 0 ? 0 : 1;
}
