// Read `remote_install.hpp` first -- the exchanges and why there are three.
#include "rommsync/remote_install.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rommsync::install {
namespace {

constexpr std::string_view kDevicesPath = "/api/devices/";

json::Error Fail(std::string field, std::string message) {
  json::Error error;
  error.field = std::move(field);
  error.message = std::move(message);
  return error;
}

/// One exchange, retried with backoff while the failure is transient. `what`
/// names it for the log, never with the token. The body of the last 2xx is left
/// in `*body`.
Exchange Send(http::HttpClient& client, http::Request request, const auth::StoredToken& token,
              const sync::CallPolicy& policy, const std::string& what, std::string* body) {
  Exchange exchange;
  if (!http::IsUrlPathSegment(token.device_id)) {
    exchange.transport = http::Error::kInvalidRequest;
    exchange.message = what + " was not sent: the paired device id cannot be a URL path";
    return exchange;
  }
  if (policy.cancel != nullptr && policy.cancel->canceled()) {
    exchange.transport = http::Error::kCanceled;
    exchange.message = what + " was not sent: the caller cancelled first";
    return exchange;
  }
  request.headers.push_back({"Accept", "application/json"});
  request.headers.push_back({"Authorization", "Bearer " + token.access_token});
  if (!request.body.empty()) {
    request.headers.push_back({"Content-Type", "application/json"});
  }
  request.timeout = policy.timeout;
  request.cancel = policy.cancel;

  const int attempts = policy.max_attempts > 0 ? policy.max_attempts : 1;
  retry::Backoff backoff = sync::BackoffFor(policy);
  for (int attempt = 1;; ++attempt) {
    exchange = Exchange{};
    exchange.attempts = attempt;
    http::Result sent = client.Send(request);
    if (!sent.ok()) {
      exchange.transport = sent.error;
      exchange.message =
          what + " did not complete: " + http::ToString(sent.error) + " (" + sent.message + ")";
    } else if (!sent.successful()) {
      exchange.status = sent.response.status;
      exchange.message = what + " was refused with status " + std::to_string(exchange.status);
    } else {
      *body = std::move(sent.response.body);
      return exchange;
    }
    if (!exchange.transient() || exchange.transport == http::Error::kCanceled ||
        attempt >= attempts || (policy.cancel != nullptr && policy.cancel->canceled())) {
      return exchange;
    }
    sync::WaitToRetry(policy, &backoff);
  }
}

std::string DeviceUrl(std::string_view base_url, const auth::StoredToken& token) {
  return http::JoinUrl(base_url, std::string(kDevicesPath) + token.device_id);
}

/// The longest prefix of `text` of at most `limit` bytes that does not end
/// inside a UTF-8 sequence.
std::string_view CutUtf8(std::string_view text, std::size_t limit) {
  if (text.size() <= limit) {
    return text;
  }
  std::size_t end = limit;
  // Back off the continuation bytes of a sequence the limit splits, then the
  // lead byte that opened it.
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
    --end;
  }
  return text.substr(0, end);
}

}  // namespace

auth::Parsed<ClaimPage> ParseClaim(std::string_view body, std::size_t max) {
  auth::Parsed<ClaimPage> out;
  ClaimPage page;
  const json::ParseResult document =
      json::ParseEach(body, "", [&page, max](const json::Value& item, std::size_t index) {
        const std::string where = "[" + std::to_string(index) + "].";
        json::Reader reader(item, "install request");
        Request request;
        reader.Required("id", &request.id);
        reader.Required("rom_id", &request.rom_id);
        if (!reader.ok()) {
          json::Error error = reader.error();
          error.field = where + error.field;
          return error;
        }
        if (request.id.size() > kMaxRequestIdBytes || !http::IsUrlPathSegment(request.id)) {
          return Fail(where + "id", "is not an id this client can put in a URL");
        }
        if (request.rom_id <= 0) {
          return Fail(where + "rom_id", "must be a positive rom id");
        }
        if (page.requests.size() < max) {
          page.requests.push_back(std::move(request));
        } else {
          ++page.dropped;
        }
        return json::Error{};
      });
  if (!document.ok()) {
    out.error = document.error;
    return out;
  }
  if (!document.value.is_array()) {
    out.error = Fail("", std::string("expected an array of install requests, got ") +
                             json::ToString(document.value.type()));
    return out;
  }
  out.value = std::move(page);
  return out;
}

const char* ToString(Outcome outcome) {
  switch (outcome) {
    case Outcome::kDone:
      return "done";
    case Outcome::kAlreadyInstalled:
      return "already_installed";
    case Outcome::kFailed:
      return "failed";
  }
  return "failed";
}

std::optional<Verdict> VerdictFor(const download::QueueEntry& entry) {
  if (entry.rom_id == 0) {
    return Verdict{Outcome::kFailed,
                   "removed from the console's download queue before it finished"};
  }
  switch (entry.state) {
    case download::QueueState::kQueued:
    case download::QueueState::kActive:
    case download::QueueState::kVerifying:
      return std::nullopt;
    case download::QueueState::kDone:
      return Verdict{entry.message == download::kAlreadyOnCardMessage ? Outcome::kAlreadyInstalled
                                                                      : Outcome::kDone,
                     {}};
    case download::QueueState::kFailed:
    case download::QueueState::kSkipped:
      return Verdict{Outcome::kFailed, entry.message.empty()
                                           ? std::string("the console could not download it")
                                           : entry.message};
  }
  return std::nullopt;
}

std::string CapabilitiesBody(bool remote_install) {
  return std::string("{\"capabilities\":{") + json::Quote(kCapability) + ":" +
         (remote_install ? "true" : "false") + "}}";
}

std::string ReportBody(const Verdict& verdict) {
  std::string body = std::string("{\"status\":") + json::Quote(ToString(verdict.outcome));
  if (!verdict.reason.empty()) {
    body += ",\"reason\":" + json::Quote(CutUtf8(verdict.reason, kMaxReasonBytes));
  }
  return body + "}";
}

bool Exchange::transient() const {
  // The client's one rule, and the 409 these three endpoints alone answer for a
  // race (the header). Nowhere else is a 409 RomM losing one: a heartbeat that
  // answers it is not RomM, so `server::Probe` does not retry it (#251).
  return (transport != http::Error::kNone && transport != http::Error::kInvalidRequest) ||
         http::TransientStatus(status) || status == 409;
}

Exchange Register(http::HttpClient& client, std::string_view base_url,
                  const auth::StoredToken& token, bool remote_install,
                  const sync::CallPolicy& policy) {
  http::Request request;
  request.method = http::Method::kPut;
  request.url = DeviceUrl(base_url, token);
  request.body = CapabilitiesBody(remote_install);
  std::string ignored;
  return Send(client, std::move(request), token, policy, "PUT /api/devices/{id} (capabilities)",
              &ignored);
}

Claimed Claim(http::HttpClient& client, std::string_view base_url, const auth::StoredToken& token,
              const sync::CallPolicy& policy, std::size_t max) {
  http::Request request;
  request.method = http::Method::kPost;
  request.url = DeviceUrl(base_url, token) + "/installs/claim";
  Claimed claimed;
  std::string body;
  claimed.exchange =
      Send(client, std::move(request), token, policy, "POST /api/devices/{id}/installs/claim",
           &body);
  if (!claimed.exchange.ok()) {
    return claimed;
  }
  auth::Parsed<ClaimPage> parsed = ParseClaim(body, max);
  if (!parsed.ok()) {
    claimed.exchange.shape = parsed.error;
    claimed.exchange.message = "the claim's answer is not a list of install requests: " +
                               parsed.error.Describe();
    return claimed;
  }
  claimed.page = std::move(parsed.value);
  return claimed;
}

Exchange Report(http::HttpClient& client, std::string_view base_url,
                const auth::StoredToken& token, std::string_view request_id,
                const Verdict& verdict, const sync::CallPolicy& policy) {
  if (!http::IsUrlPathSegment(request_id)) {
    Exchange refused;
    refused.transport = http::Error::kInvalidRequest;
    refused.message = "the report was not sent: the request id cannot be a URL path";
    return refused;
  }
  http::Request request;
  request.method = http::Method::kPut;
  request.url = DeviceUrl(base_url, token) + "/installs/" + std::string(request_id);
  request.body = ReportBody(verdict);
  std::string ignored;
  return Send(client, std::move(request), token, policy,
              std::string("PUT /api/devices/{id}/installs/{request} (") +
                  ToString(verdict.outcome) + ")",
              &ignored);
}

}  // namespace rommsync::install
