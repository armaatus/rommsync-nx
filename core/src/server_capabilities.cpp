// Read `server_capabilities.hpp` first -- the rules are there.
//
// What is only here is the walk: how a heartbeat body is crossed to reach one
// member without building the rest of it.
#include "rommsync/server_capabilities.hpp"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace rommsync::server {
namespace {

constexpr std::string_view kHeartbeatPath = "/api/heartbeat";

/// The largest component `ParseVersion` takes. A release number past this is a
/// server answering something that is not a release.
constexpr int kMaxComponent = 100000;

json::Error Fail(std::size_t offset, std::string message) {
  json::Error error;
  error.offset = offset;
  error.message = std::move(message);
  return error;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/// Walks a document without keeping any of it.
///
/// **A validator that builds nothing, not a substring scan.** It checks the
/// grammar of every value it crosses -- members, commas, colons, the bracket
/// that closes each container, the spelling of each literal -- so a `"}"`
/// inside a platform name cannot end the walk early, a key that only *looks*
/// like `"SYSTEM"` inside another member's value is never mistaken for it, and
/// a truncated or garbled body is refused rather than half-read
/// (core/AGENTS.md). It holds a bitstack rather than recursing, so a hostile
/// body costs no stack and no heap. What it leaves to `json::Parse` is the
/// detail it does not need: escape sequences, number ranges, duplicate keys --
/// which the one member this client keeps is held to anyway.
class Walker {
 public:
  explicit Walker(std::string_view text) : text_(text) {}

  std::size_t at() const { return at_; }
  bool done() const { return at_ >= text_.size(); }

  void SkipSpace() {
    while (at_ < text_.size() && IsSpace(text_[at_])) {
      ++at_;
    }
  }

  bool Take(char c) {
    SkipSpace();
    if (at_ < text_.size() && text_[at_] == c) {
      ++at_;
      return true;
    }
    return false;
  }

  /// A string at the cursor, quotes included, as the raw bytes between them.
  /// Escapes are stepped over, not decoded: a key spelled with one is not a
  /// key this client is looking for.
  bool String(std::string_view* raw) {
    SkipSpace();
    if (at_ >= text_.size() || text_[at_] != '"') {
      return false;
    }
    const std::size_t start = ++at_;
    while (at_ < text_.size()) {
      const char c = text_[at_];
      if (c == '\\') {
        at_ += 2;
        continue;
      }
      if (c == '"') {
        *raw = text_.substr(start, at_ - start);
        ++at_;
        return true;
      }
      ++at_;
    }
    return false;
  }

  /// Step over one value, of any type. False when it is not well-formed, when
  /// the text ends inside it, or when it nests past `json::kMaxDepth`.
  ///
  /// The containers it is inside are a bitstack -- one bit a level, set for an
  /// object -- which is all a walk that keeps nothing needs to know about them.
  bool Value() {
    static_assert(json::kMaxDepth <= 64, "the bitstack holds one bit a level");
    std::uint64_t objects = 0;
    int depth = 0;
    for (;;) {
      // A value starts here.
      SkipSpace();
      if (at_ >= text_.size()) {
        return false;
      }
      const char c = text_[at_];
      if (c == '{' || c == '[') {
        if (depth >= json::kMaxDepth) {
          return false;
        }
        ++at_;
        objects = (objects << 1) | (c == '{' ? 1u : 0u);
        ++depth;
        if (Take(c == '{' ? '}' : ']')) {
          objects >>= 1;
          --depth;
        } else {
          if (c == '{' && !MemberName()) {
            return false;
          }
          continue;
        }
      } else if (c == '"') {
        std::string_view ignored;
        if (!String(&ignored)) {
          return false;
        }
      } else if (!Scalar()) {
        return false;
      }

      // A value ended. Close every container it finished, then either the
      // whole value is done or a comma starts the next one.
      for (;;) {
        if (depth == 0) {
          return true;
        }
        const bool object = (objects & 1u) == 1u;
        if (Take(',')) {
          if (object && !MemberName()) {
            return false;
          }
          break;
        }
        if (!Take(object ? '}' : ']')) {
          return false;
        }
        objects >>= 1;
        --depth;
      }
    }
  }

  /// `"name":`, inside an object.
  bool MemberName() {
    std::string_view ignored;
    return String(&ignored) && Take(':');
  }

  /// A literal or a number at the cursor. Spelled out rather than run to the
  /// next delimiter, so `fals` is refused here as `json::Parse` would refuse
  /// it -- a body with one broken field is a body this client does not trust
  /// the rest of either. A number is held to RFC 8259's grammar; nothing here
  /// reads its value, so its range is not checked.
  bool Scalar() {
    for (std::string_view literal : {"true", "false", "null"}) {
      if (text_.substr(at_, literal.size()) == literal) {
        at_ += literal.size();
        return Delimited();
      }
    }
    // -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
    Accept('-');
    if (Accept('0')) {
      // A leading zero is the whole integer part: `01` is not a number.
    } else if (Digits() == 0) {
      return false;
    }
    if (Accept('.') && Digits() == 0) {
      return false;
    }
    if (Accept('e') || Accept('E')) {
      if (!Accept('+')) {
        Accept('-');
      }
      if (Digits() == 0) {
        return false;
      }
    }
    return Delimited();
  }

  bool Accept(char c) {
    if (at_ < text_.size() && text_[at_] == c) {
      ++at_;
      return true;
    }
    return false;
  }

  std::size_t Digits() {
    const std::size_t start = at_;
    while (at_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[at_])) != 0) {
      ++at_;
    }
    return at_ - start;
  }

  /// A scalar has to end where a value can: `truex` is not `true`.
  bool Delimited() const {
    return at_ >= text_.size() || text_[at_] == ',' || text_[at_] == '}' || text_[at_] == ']' ||
           IsSpace(text_[at_]);
  }

  std::string_view Slice(std::size_t from) const { return text_.substr(from, at_ - from); }

 private:
  std::string_view text_;
  std::size_t at_ = 0;
};

/// The text of top-level member `key` of the object `body`, or nullopt when
/// there is none. `*error` is set, and the answer nullopt, when the body is not
/// an object this walk can cross.
std::optional<std::string_view> TopLevelMember(std::string_view body, std::string_view key,
                                               json::Error* error) {
  Walker walk(body);
  if (!walk.Take('{')) {
    *error = Fail(walk.at(), "the heartbeat is not a JSON object");
    return std::nullopt;
  }
  std::optional<std::string_view> found;
  if (!walk.Take('}')) {
    for (;;) {
      std::string_view name;
      if (!walk.String(&name)) {
        *error = Fail(walk.at(), "expected a member name");
        return std::nullopt;
      }
      if (!walk.Take(':')) {
        *error = Fail(walk.at(), "expected ':' after a member name");
        return std::nullopt;
      }
      walk.SkipSpace();
      const std::size_t start = walk.at();
      if (!walk.Value()) {
        *error = Fail(walk.at(), "a member's value ends early or does not balance");
        return std::nullopt;
      }
      if (name == key) {
        if (found.has_value()) {
          // `json::Parse` refuses a duplicate key for the same reason: which
          // of the two is meant is not this client's to guess.
          *error = Fail(start, "the member appears twice");
          error->field = std::string(key);
          return std::nullopt;
        }
        found = walk.Slice(start);
      }
      if (walk.Take(',')) {
        continue;
      }
      if (walk.Take('}')) {
        break;
      }
      *error = Fail(walk.at(), "expected ',' or '}' between members");
      return std::nullopt;
    }
  }
  walk.SkipSpace();
  if (!walk.done()) {
    // A truncated body followed by a second one must not read as the first --
    // the rule `json::Parse` keeps for whole documents.
    *error = Fail(walk.at(), "content after the heartbeat object");
    return std::nullopt;
  }
  return found;
}

/// One component of a version: digits only, and a number a release could be.
bool Component(std::string_view text, int* out) {
  if (text.empty() || text.size() > 6) {
    return false;
  }
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size() || value > kMaxComponent) {
    return false;
  }
  *out = value;
  return true;
}

bool TransientStatus(int status) { return status >= 500 || status == 408 || status == 429; }

}  // namespace

std::optional<Version> ParseVersion(std::string_view text) {
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }
  // The pre-release and the build metadata, in that order of precedence:
  // `5.4.0-beta.1+abc` is 5.4.0 either way.
  text = text.substr(0, text.find_first_of("-+"));

  Version version;
  int* const parts[] = {&version.major, &version.minor, &version.patch};
  std::size_t count = 0;
  while (true) {
    const std::size_t dot = text.find('.');
    if (count == 3 || !Component(text.substr(0, dot), parts[count])) {
      return std::nullopt;
    }
    ++count;
    if (dot == std::string_view::npos) {
      break;
    }
    text.remove_prefix(dot + 1);
  }
  // `5` alone names no release; `5.4` is 5.4.0, which is how release notes
  // and people both spell one.
  if (count < 2) {
    return std::nullopt;
  }
  return version;
}

std::string ToString(const Version& version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
         std::to_string(version.patch);
}

namespace {

/// The one place a release becomes a set of flags. `parsed` empty is the
/// unknown version, which reads as the newest -- see the header.
ServerCapabilities Infer(std::string text, const std::optional<Version>& parsed,
                         const Version& floor) {
  ServerCapabilities caps;
  caps.version = std::move(text);
  caps.known = parsed.has_value();
  const Version version = parsed.value_or(Version{kMaxComponent, 0, 0});
  caps.supported = version >= floor;
  caps.rom_ids = version >= kRelease53;
  caps.slot_cap = version >= kRelease53;
  caps.delete_action = version >= kRelease54;
  caps.emulators_filter = version >= kRelease54;
  caps.restore_unlisted = version >= kRelease54;
  caps.hash_baselines = version >= kRelease54;
  caps.device_capabilities = version >= kRelease54;
  return caps;
}

}  // namespace

ServerCapabilities CapabilitiesFor(std::string_view version_text, const Version& floor) {
  return Infer(std::string(version_text.substr(0, kMaxVersionBytes)), ParseVersion(version_text),
               floor);
}

ServerCapabilities CapabilitiesFor(const Version& version) {
  return Infer(ToString(version), version, kMinimumVersion);
}

auth::Parsed<std::string> ParseHeartbeatVersion(std::string_view body) {
  auth::Parsed<std::string> out;
  const std::optional<std::string_view> system = TopLevelMember(body, "SYSTEM", &out.error);
  if (!out.error.ok() || !system.has_value()) {
    return out;
  }
  if (system->size() > kMaxSystemBytes) {
    out.error = Fail(0, "SYSTEM is " + std::to_string(system->size()) +
                            " bytes, past the " + std::to_string(kMaxSystemBytes) +
                            " this client parses");
    out.error.field = "SYSTEM";
    return out;
  }
  const json::ParseResult parsed = json::Parse(*system);
  if (!parsed.ok()) {
    out.error = parsed.error;
    out.error.field = "SYSTEM";
    return out;
  }
  if (!parsed.value.is_object()) {
    out.error = Fail(0, std::string("expected an object, got ") +
                            json::ToString(parsed.value.type()));
    out.error.field = "SYSTEM";
    return out;
  }
  const json::Value* version = parsed.value.Find("VERSION");
  if (version == nullptr || version->is_null()) {
    return out;
  }
  if (!version->is_string()) {
    out.error = Fail(0, std::string("expected a string, got ") + json::ToString(version->type()));
    out.error.field = "SYSTEM.VERSION";
    return out;
  }
  out.value = version->string();
  return out;
}

auth::Parsed<bool> ParseHeartbeatInstall(std::string_view body) {
  auth::Parsed<bool> out;
  const std::optional<std::string_view> install =
      TopLevelMember(body, "DEVICE_INSTALL", &out.error);
  if (!out.error.ok() || !install.has_value()) {
    return out;
  }
  // The member is itself an object, so the same walk crosses it to `ENABLED`
  // -- past the excluded-platform list, keeping none of it.
  const std::optional<std::string_view> enabled = TopLevelMember(*install, "ENABLED", &out.error);
  if (!out.error.ok()) {
    out.error.field = "DEVICE_INSTALL" + (out.error.field.empty() ? "" : "." + out.error.field);
    return out;
  }
  if (!enabled.has_value() || *enabled == "null") {
    return out;
  }
  if (*enabled != "true" && *enabled != "false") {
    out.error = Fail(0, "expected a boolean");
    out.error.field = "DEVICE_INSTALL.ENABLED";
    return out;
  }
  out.value = *enabled == "true";
  return out;
}

bool Probe::transient() const {
  return (transport != http::Error::kNone && transport != http::Error::kInvalidRequest) ||
         TransientStatus(status);
}

Probe ReadServer(http::HttpClient& client, std::string_view base_url,
                 const sync::CallPolicy& policy, const Version& floor) {
  Probe probe;
  if (policy.cancel != nullptr && policy.cancel->canceled()) {
    probe.transport = http::Error::kCanceled;
    probe.message = "the heartbeat was not asked: the caller cancelled first";
    return probe;
  }

  http::Request request;
  request.url = http::JoinUrl(base_url, kHeartbeatPath);
  request.headers.push_back({"Accept", "application/json"});
  request.timeout = policy.timeout;
  request.cancel = policy.cancel;

  const int attempts = policy.max_attempts > 0 ? policy.max_attempts : 1;
  std::chrono::milliseconds backoff =
      policy.backoff > policy.max_backoff ? policy.max_backoff : policy.backoff;

  for (int attempt = 1;; ++attempt) {
    probe = Probe{};
    probe.attempts = attempt;
    const http::Result sent = client.Send(request);
    if (!sent.ok()) {
      probe.transport = sent.error;
      probe.message = std::string("the server's version could not be read: ") +
                      http::ToString(sent.error) + " (" + sent.message + ")";
    } else if (!sent.successful()) {
      probe.status = sent.response.status;
      probe.message =
          "the heartbeat was refused with status " + std::to_string(sent.response.status);
    } else {
      const auth::Parsed<std::string> version = ParseHeartbeatVersion(sent.response.body);
      if (version.ok()) {
        probe.capabilities = CapabilitiesFor(version.value, floor);
        // Never a reason to refuse the server: a switch that will not read
        // is a switch that is off (`install_enabled`).
        const auth::Parsed<bool> install = ParseHeartbeatInstall(sent.response.body);
        probe.capabilities.install_enabled = install.ok() && install.value;
        return probe;
      }
      probe.shape = version.error;
      probe.message = "the answer is not a RomM heartbeat: " + version.error.Describe();
    }

    if (!probe.transient() || probe.transport == http::Error::kCanceled ||
        attempt >= attempts) {
      return probe;
    }
    if (policy.cancel != nullptr && policy.cancel->canceled()) {
      return probe;
    }
    if (policy.wait != nullptr) {
      policy.wait(backoff);
    } else {
      std::this_thread::sleep_for(backoff);
    }
    backoff = backoff * 2 > policy.max_backoff ? policy.max_backoff : backoff * 2;
  }
}

}  // namespace rommsync::server
