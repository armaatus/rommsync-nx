#include "rommsync/ipc.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/config.hpp"
#include "rommsync/json.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::ipc {
namespace {

// --- bounds -------------------------------------------------------------------
//
// Every number on this wire is bounded, and the bounds are here rather than at
// each call site so a field cannot quietly acquire a wider one. They are not
// defensive padding: a decoder that shrugged at a negative `bytes_done` or a
// `queue_depth` of 2^40 would hand the overlay a number to render and a `int32_t`
// to overflow, and the writer of the payload is on the other side of an SD card
// from the reader -- a different release of a different binary.

/// Sync counts, and anything else counting operations in one tick.
constexpr std::int64_t kMaxCount = 1'000'000'000;

/// Queue depth and a queue position.
constexpr std::int64_t kMaxQueueDepth = 1'000'000;

/// A RomM row id. Ids are positive; `0` is the absence of one.
constexpr std::int64_t kMaxId = 1'000'000'000'000'000;

/// A file size. 1 PiB -- a bound on a size, not a plausible rom.
constexpr std::int64_t kMaxBytes = std::int64_t{1} << 50;

/// A page size a client may ask for, before `ServiceCore` clamps it.
constexpr std::int64_t kMaxRequestedPageSize = 1'000'000;

// --- reading ------------------------------------------------------------------
//
// The house rule, one level up from `json::Reader`: every field this build knows
// is required and is held to its type, and the first thing wrong with a payload
// is what comes back. Nothing defaults.
//
// That makes adding a field to a payload an incompatible change, which is
// deliberate -- `kVersion` is the thing that makes it safe, and a decoder that
// tolerated a missing field would be one that cannot tell an old sysmodule from
// a broken one.
//
// Each reader is a no-op once the error is set, so a caller can read every field
// and check once at the end.

bool Fail(json::Error* error, std::string_view key, std::string message) {
  if (error->ok()) {
    error->field = std::string(key);
    error->message = std::move(message);
  }
  return false;
}

const json::Value* Member(const json::Value& object, std::string_view key, json::Error* error) {
  if (!error->ok()) {
    return nullptr;
  }
  const json::Value* found = object.Find(key);
  if (found == nullptr) {
    Fail(error, key, "is missing");
    return nullptr;
  }
  return found;
}

/// A string field. May be empty -- a `user_code` before a code exists, a
/// `message` when nothing went wrong -- but never absent, of another type, or
/// carrying a NUL: `json::Reader::Required` refuses that last one because every
/// C API downstream stops at it, so the value that gets used would not be the
/// value that was checked.
bool ReadText(const json::Value& object, std::string_view key, std::string* out,
              json::Error* error) {
  const json::Value* found = Member(object, key, error);
  if (found == nullptr) {
    return false;
  }
  if (!found->is_string()) {
    return Fail(error, key,
                std::string("expected a string, got ") + json::ToString(found->type()));
  }
  if (found->string().find('\0') != std::string::npos) {
    return Fail(error, key, "carries a NUL");
  }
  *out = found->string();
  return true;
}

bool ReadNonEmptyText(const json::Value& object, std::string_view key, std::string* out,
                      json::Error* error) {
  if (!ReadText(object, key, out, error)) {
    return false;
  }
  if (out->empty()) {
    return Fail(error, key, "is empty");
  }
  return true;
}

bool ReadInteger(const json::Value& object, std::string_view key, std::int64_t* out,
                 std::int64_t low, std::int64_t high, json::Error* error) {
  const json::Value* found = Member(object, key, error);
  if (found == nullptr) {
    return false;
  }
  if (!found->is_integer()) {
    return Fail(error, key,
                std::string("expected an integer, got ") + json::ToString(found->type()));
  }
  if (found->integer() < low || found->integer() > high) {
    return Fail(error, key, "is out of range");
  }
  *out = found->integer();
  return true;
}

bool ReadBool(const json::Value& object, std::string_view key, bool* out, json::Error* error) {
  const json::Value* found = Member(object, key, error);
  if (found == nullptr) {
    return false;
  }
  if (!found->is_bool()) {
    return Fail(error, key,
                std::string("expected a boolean, got ") + json::ToString(found->type()));
  }
  *out = found->boolean();
  return true;
}

const json::Value* ReadObject(const json::Value& object, std::string_view key,
                              json::Error* error) {
  const json::Value* found = Member(object, key, error);
  if (found == nullptr) {
    return nullptr;
  }
  if (!found->is_object()) {
    Fail(error, key, std::string("expected an object, got ") + json::ToString(found->type()));
    return nullptr;
  }
  return found;
}

const json::Value* ReadArray(const json::Value& object, std::string_view key,
                             std::size_t max_size, json::Error* error) {
  const json::Value* found = Member(object, key, error);
  if (found == nullptr) {
    return nullptr;
  }
  if (!found->is_array()) {
    Fail(error, key, std::string("expected an array, got ") + json::ToString(found->type()));
    return nullptr;
  }
  if (found->size() > max_size) {
    Fail(error, key, "holds more entries than this build accepts");
    return nullptr;
  }
  return found;
}

/// An enum written as its `ToString` spelling.
///
/// The value is quoted back on a mismatch, which `json::Error` refuses to do for
/// a value off the network. The reason that rule exists does not apply here: the
/// writer is this file's own encoder in another process, so the string is one of
/// ours -- and against an overlay built from a different release, "is not a sync
/// outcome: throttled" is the whole diagnosis.
template <typename E, std::size_t N>
bool ReadEnum(const json::Value& object, std::string_view key, const E (&values)[N],
              const char* (*to_string)(E), std::string_view what, E* out, json::Error* error) {
  std::string text;
  if (!ReadText(object, key, &text, error)) {
    return false;
  }
  for (const E candidate : values) {
    if (text == to_string(candidate)) {
      *out = candidate;
      return true;
    }
  }
  return Fail(error, key, "is not " + std::string(what) + ": " + text);
}

// --- the enum tables ----------------------------------------------------------
//
// One array per enum, so a `ToString` that grew a case and a decoder that did
// not cannot drift apart -- `ipc.roundtrip` walks these.

constexpr AuthState kAllAuthStates[] = {
    AuthState::kNeverPaired,
    AuthState::kUnauthenticated,
    AuthState::kPaired,
};

constexpr SyncResult kAllSyncResults[] = {
    SyncResult::kNever,
    SyncResult::kOk,
    SyncResult::kPartial,
    SyncResult::kFailed,
};

constexpr DownloadState kAllDownloadStates[] = {
    DownloadState::kIdle, DownloadState::kQueued, DownloadState::kDownloading,
    DownloadState::kVerifying, DownloadState::kFailed,
};

constexpr SyncOutcome kAllSyncOutcomes[] = {
    SyncOutcome::kAccepted,      SyncOutcome::kAlreadyRunning, SyncOutcome::kNotConfigured,
    SyncOutcome::kUnauthenticated, SyncOutcome::kDisabled,
};

constexpr ListKind kAllListKinds[] = {
    ListKind::kPlatforms,
    ListKind::kRoms,
    ListKind::kQueue,
};

constexpr config::Severity kAllSeverities[] = {
    config::Severity::kNotice,
    config::Severity::kWarning,
    config::Severity::kError,
};

// --- decoding scaffolding -----------------------------------------------------

/// Parse `text` as an object and hand it to `read`.
///
/// The size check is first and applies to every payload in both directions: a
/// buffer longer than `kMaxPayloadBytes` is refused before it is parsed, so a
/// hostile or broken peer cannot make this side spend a parse proportional to
/// what it chose to send.
template <typename T, typename Read>
Decoded<T> DecodeObject(std::string_view text, std::string_view context, Read read) {
  Decoded<T> decoded;
  if (text.size() > kMaxPayloadBytes) {
    decoded.error.message = std::string(context) + ": payload is longer than the IPC cap";
    return decoded;
  }
  const json::ParseResult document = json::Parse(text);
  if (!document.ok()) {
    decoded.error = document.error;
    return decoded;
  }
  if (!document.value.is_object()) {
    decoded.error.message = std::string(context) + ": expected an object, got " +
                            json::ToString(document.value.type());
    return decoded;
  }
  read(document.value, &decoded.value, &decoded.error);
  if (!decoded.error.ok()) {
    decoded.value = T{};
  }
  return decoded;
}

// --- encoding scaffolding -----------------------------------------------------

void AppendKey(std::string* out, std::string_view key, bool first = false) {
  if (!first) {
    *out += ',';
  }
  *out += json::Quote(key);
  *out += ':';
}

void AppendBool(std::string* out, std::string_view key, bool value, bool first = false) {
  AppendKey(out, key, first);
  *out += value ? "true" : "false";
}

void AppendInteger(std::string* out, std::string_view key, std::int64_t value,
                   bool first = false) {
  AppendKey(out, key, first);
  *out += std::to_string(value);
}

void AppendText(std::string* out, std::string_view key, std::string_view value,
                bool first = false) {
  AppendKey(out, key, first);
  *out += json::Quote(value);
}

void AppendPathArray(std::string* out, std::string_view key,
                     const std::vector<std::string>& paths) {
  AppendKey(out, key);
  *out += json::QuoteArray(paths);
}

std::string EncodeDiagnosticArray(const std::vector<config::Diagnostic>& diagnostics) {
  std::string out("[");
  bool first = true;
  for (const config::Diagnostic& diagnostic : diagnostics) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += '{';
    AppendText(&out, "severity", config::ToString(diagnostic.severity), /*first=*/true);
    AppendInteger(&out, "line", diagnostic.line);
    AppendText(&out, "section", diagnostic.section);
    AppendText(&out, "key", diagnostic.key);
    AppendText(&out, "message", diagnostic.message);
    out += '}';
  }
  out += ']';
  return out;
}

bool ReadDiagnosticArray(const json::Value& object, std::vector<config::Diagnostic>* out,
                         json::Error* error) {
  const json::Value* array = ReadArray(object, "diagnostics", config::kMaxDiagnostics + 1, error);
  if (array == nullptr) {
    return false;
  }
  for (const json::Value& element : array->elements()) {
    if (!element.is_object()) {
      return Fail(error, "diagnostics", "holds something that is not an object");
    }
    config::Diagnostic diagnostic;
    std::int64_t line = 0;
    if (!ReadEnum(element, "severity", kAllSeverities, config::ToString, "a severity",
                  &diagnostic.severity, error) ||
        // A diagnostic's line is 1-based, or 0 for a complaint about the file as
        // a whole. `config::kMaxConfigBytes` bounds how many lines there can be.
        !ReadInteger(element, "line", &line, 0,
                     static_cast<std::int64_t>(config::kMaxConfigBytes), error) ||
        !ReadText(element, "section", &diagnostic.section, error) ||
        !ReadText(element, "key", &diagnostic.key, error) ||
        !ReadText(element, "message", &diagnostic.message, error)) {
      return false;
    }
    diagnostic.line = static_cast<int>(line);
    out->push_back(std::move(diagnostic));
  }
  return true;
}

/// A folder-map list: absolute SD paths, bounded like `config::kMaxPathsPerKey`.
bool ReadPathArray(const json::Value& object, std::string_view key,
                   std::vector<std::string>* out, json::Error* error) {
  const json::Value* array = ReadArray(object, key, config::kMaxPathsPerKey, error);
  if (array == nullptr) {
    return false;
  }
  for (const json::Value& element : array->elements()) {
    if (!element.is_string()) {
      return Fail(error, key, "holds something that is not a path");
    }
    if (element.string().size() > config::kMaxPathLength) {
      return Fail(error, key, "holds a path longer than the console can open");
    }
    if (element.string().find('\0') != std::string::npos) {
      return Fail(error, key, "holds a path carrying a NUL");
    }
    out->push_back(element.string());
  }
  return true;
}

/// The `[platform.*]` map, which is the one part of a config that grows with
/// what a user wrote. Encoded separately so `EncodeConfigView` can leave it out
/// and say so rather than refuse to answer -- see `ConfigView::platforms_truncated`.
std::string EncodePlatformMap(const config::Config& config) {
  std::string out("{");
  bool first = true;
  for (const auto& [slug, folders] : config.platforms) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += json::Quote(slug);
    out += ":{";
    AppendKey(&out, "roms", /*first=*/true);
    out += json::QuoteArray(folders.roms);
    AppendPathArray(&out, "saves", folders.saves);
    AppendPathArray(&out, "states", folders.states);
    out += '}';
  }
  out += '}';
  return out;
}

bool ReadPlatformMap(const json::Value& object, config::Config* config, json::Error* error) {
  const json::Value* map = ReadObject(object, "platforms", error);
  if (map == nullptr) {
    return false;
  }
  if (map->size() > config::kMaxPlatformSections) {
    return Fail(error, "platforms", "holds more platforms than this build accepts");
  }
  for (const json::Member& member : map->members()) {
    if (!member.value.is_object()) {
      return Fail(error, "platforms", "holds something that is not a folder map");
    }
    config::PlatformFolders folders;
    if (!ReadPathArray(member.value, "roms", &folders.roms, error) ||
        !ReadPathArray(member.value, "saves", &folders.saves, error) ||
        !ReadPathArray(member.value, "states", &folders.states, error)) {
      return false;
    }
    config->platforms.emplace(member.key, std::move(folders));
  }
  return true;
}

/// Cut `text` to `kMaxDiagnosticTextBytes`, visibly and on a character boundary.
///
/// A diagnostic quotes what the user wrote -- a folder path, a platform slug --
/// and those are UTF-8. Cutting at a byte offset lands inside a multi-byte
/// sequence about half the time, and `json::Quote` passes bytes at or above 0x80
/// through unchanged, so the truncation would travel as malformed UTF-8 for the
/// overlay's font renderer to deal with. Backing up over the continuation bytes
/// (10xxxxxx) costs at most three of them.
///
/// The ellipsis is three ASCII dots rather than U+2026 for the same reason: it
/// is one byte per character whatever the encoding around it.
void Shorten(std::string* text) {
  if (text->size() <= kMaxDiagnosticTextBytes) {
    return;
  }
  std::size_t cut = kMaxDiagnosticTextBytes;
  while (cut > 0 && (static_cast<unsigned char>((*text)[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  text->resize(cut);
  *text += "...";
}

}  // namespace

// --- trimming -----------------------------------------------------------------

std::vector<config::Diagnostic> TrimDiagnostics(
    const std::vector<config::Diagnostic>& diagnostics) {
  std::vector<config::Diagnostic> out;
  const std::size_t keep = diagnostics.size() > kMaxDiagnosticsInPayload
                               ? kMaxDiagnosticsInPayload - 1
                               : diagnostics.size();
  out.reserve(keep + 1);
  for (std::size_t at = 0; at < keep; ++at) {
    config::Diagnostic diagnostic = diagnostics[at];
    Shorten(&diagnostic.section);
    Shorten(&diagnostic.key);
    Shorten(&diagnostic.message);
    out.push_back(std::move(diagnostic));
  }
  if (keep < diagnostics.size()) {
    // Said rather than dropped: a settings screen that stops at eight
    // complaints, with nothing to say there were more, reads as a file with
    // eight things wrong with it.
    config::Diagnostic more;
    more.severity = config::Severity::kNotice;
    more.message = std::to_string(diagnostics.size() - keep) +
                   " more diagnostics did not fit this payload; the full list is in the log";
    out.push_back(std::move(more));
  }
  return out;
}

// --- the command table --------------------------------------------------------

const Command kAllCommands[14] = {
    Command::kGetInterfaceVersion, Command::kGetStatus,  Command::kGetConfig,
    Command::kSetConfig,           Command::kSetEnabled, Command::kSyncNow,
    Command::kStartPair,           Command::kGetPairState, Command::kUnpair,
    Command::kEnqueue,             Command::kDequeue,    Command::kListBegin,
    Command::kListNext,            Command::kListEnd,
};

const char* ToString(Command command) {
  switch (command) {
    case Command::kGetInterfaceVersion:
      return "GetInterfaceVersion";
    case Command::kGetStatus:
      return "GetStatus";
    case Command::kGetConfig:
      return "GetConfig";
    case Command::kSetConfig:
      return "SetConfig";
    case Command::kSetEnabled:
      return "SetEnabled";
    case Command::kSyncNow:
      return "SyncNow";
    case Command::kStartPair:
      return "StartPair";
    case Command::kGetPairState:
      return "GetPairState";
    case Command::kUnpair:
      return "Unpair";
    case Command::kEnqueue:
      return "Enqueue";
    case Command::kDequeue:
      return "Dequeue";
    case Command::kListBegin:
      return "ListBegin";
    case Command::kListNext:
      return "ListNext";
    case Command::kListEnd:
      return "ListEnd";
  }
  return "Unknown";
}

bool IsCommand(std::uint32_t id, Command* out) {
  for (const Command candidate : kAllCommands) {
    if (static_cast<std::uint32_t>(candidate) == id) {
      if (out != nullptr) {
        *out = candidate;
      }
      return true;
    }
  }
  return false;
}

const char* ToString(Error error) {
  switch (error) {
    case Error::kOk:
      return "ok";
    case Error::kInvalid:
      return "invalid";
    case Error::kWriteFailed:
      return "write_failed";
    case Error::kNotConfigured:
      return "not_configured";
    case Error::kUnknownRom:
      return "unknown_rom";
    case Error::kQueueFull:
      return "queue_full";
    case Error::kDuplicate:
      return "duplicate";
    case Error::kMultiFile:
      return "multi_file";
    case Error::kNotQueued:
      return "not_queued";
    case Error::kBadCursor:
      return "bad_cursor";
    case Error::kOffline:
      return "offline";
    case Error::kUnknownCommand:
      return "unknown_command";
    case Error::kMalformedRequest:
      return "malformed_request";
    case Error::kTooLarge:
      return "too_large";
    case Error::kInternal:
      return "internal";
  }
  return "internal";
}

const char* ToString(AuthState state) {
  switch (state) {
    case AuthState::kNeverPaired:
      return "never_paired";
    case AuthState::kUnauthenticated:
      return "unauthenticated";
    case AuthState::kPaired:
      return "paired";
  }
  return "never_paired";
}

const char* ToString(SyncResult result) {
  switch (result) {
    case SyncResult::kNever:
      return "never";
    case SyncResult::kOk:
      return "ok";
    case SyncResult::kPartial:
      return "partial";
    case SyncResult::kFailed:
      return "failed";
  }
  return "never";
}

const char* ToString(DownloadState state) {
  switch (state) {
    case DownloadState::kIdle:
      return "idle";
    case DownloadState::kQueued:
      return "queued";
    case DownloadState::kDownloading:
      return "downloading";
    case DownloadState::kVerifying:
      return "verifying";
    case DownloadState::kFailed:
      return "failed";
  }
  return "idle";
}

const char* ToString(SyncOutcome outcome) {
  switch (outcome) {
    case SyncOutcome::kAccepted:
      return "accepted";
    case SyncOutcome::kAlreadyRunning:
      return "already_running";
    case SyncOutcome::kNotConfigured:
      return "not_configured";
    case SyncOutcome::kUnauthenticated:
      return "unauthenticated";
    case SyncOutcome::kDisabled:
      return "disabled";
  }
  return "accepted";
}

const char* ToString(ListKind kind) {
  switch (kind) {
    case ListKind::kPlatforms:
      return "platforms";
    case ListKind::kRoms:
      return "roms";
    case ListKind::kQueue:
      return "queue";
  }
  return "platforms";
}

// --- list values --------------------------------------------------------------

ListValue ListValue::Text(std::string value) {
  ListValue out;
  out.type = Type::kString;
  out.text = std::move(value);
  return out;
}

ListValue ListValue::Integer(std::int64_t value) {
  ListValue out;
  out.type = Type::kInteger;
  out.number = value;
  return out;
}

ListValue ListValue::Flag(bool value) {
  ListValue out;
  out.type = Type::kBool;
  out.flag = value;
  return out;
}

bool ListValue::operator==(const ListValue& other) const {
  if (type != other.type) {
    return false;
  }
  switch (type) {
    case Type::kString:
      return text == other.text;
    case Type::kInteger:
      return number == other.number;
    case Type::kBool:
      return flag == other.flag;
  }
  return false;
}

const ListValue* ListItem::Find(std::string_view key) const {
  for (const ListField& field : fields) {
    if (field.key == key) {
      return &field.value;
    }
  }
  return nullptr;
}

// --- codecs -------------------------------------------------------------------

bool Fits(std::string_view payload) { return payload.size() <= kMaxPayloadBytes; }

std::string EncodeInterfaceVersion(std::uint32_t interface_version) {
  // Frozen. Every build there will ever be answers command 0 with exactly this,
  // because it is the one payload an overlay decodes before it knows whether it
  // can decode anything else. `ipc.version` fails if these bytes change.
  return "{\"interface\":" + std::to_string(interface_version) + "}";
}

Decoded<std::uint32_t> DecodeInterfaceVersion(std::string_view text) {
  return DecodeObject<std::uint32_t>(
      text, "interface version",
      [](const json::Value& object, std::uint32_t* out, json::Error* error) {
        std::int64_t value = 0;
        if (ReadInteger(object, "interface", &value, 0, 0xFFFFFFFF, error)) {
          *out = static_cast<std::uint32_t>(value);
        }
      });
}

std::string EncodeStatus(const Status& status) {
  std::string out("{");
  AppendInteger(&out, "interface", status.interface, /*first=*/true);
  AppendText(&out, "build", status.build);
  AppendBool(&out, "enabled", status.enabled);
  AppendText(&out, "auth", ToString(status.auth));
  AppendBool(&out, "configured", status.configured);
  AppendBool(&out, "online", status.online);
  AppendInteger(&out, "last_sync_at", status.last_sync_at);
  AppendText(&out, "last_sync_result", ToString(status.last_sync_result));
  AppendInteger(&out, "uploaded", status.uploaded);
  AppendInteger(&out, "downloaded", status.downloaded);
  AppendInteger(&out, "conflicts", status.conflicts);
  AppendInteger(&out, "failed", status.failed);
  AppendInteger(&out, "queue_depth", status.queue_depth);
  AppendKey(&out, "download");
  out += '{';
  AppendText(&out, "state", ToString(status.download.state), /*first=*/true);
  AppendInteger(&out, "rom_id", status.download.rom_id);
  AppendText(&out, "fs_name", status.download.fs_name);
  AppendInteger(&out, "bytes_done", status.download.bytes_done);
  AppendInteger(&out, "bytes_total", status.download.bytes_total);
  out += "}}";
  return out;
}

Decoded<Status> DecodeStatus(std::string_view text) {
  return DecodeObject<Status>(text, "status", [](const json::Value& object, Status* out,
                                                 json::Error* error) {
    std::int64_t interface_version = 0;
    if (!ReadInteger(object, "interface", &interface_version, 0, 0xFFFFFFFF, error) ||
        !ReadText(object, "build", &out->build, error) ||
        !ReadBool(object, "enabled", &out->enabled, error) ||
        !ReadEnum(object, "auth", kAllAuthStates, ToString, "an auth state", &out->auth, error) ||
        !ReadBool(object, "configured", &out->configured, error) ||
        !ReadBool(object, "online", &out->online, error) ||
        !ReadInteger(object, "last_sync_at", &out->last_sync_at, 0,
                     sync::kMaxTimestampSeconds, error) ||
        !ReadEnum(object, "last_sync_result", kAllSyncResults, ToString, "a sync result",
                  &out->last_sync_result, error) ||
        !ReadInteger(object, "uploaded", &out->uploaded, 0, kMaxCount, error) ||
        !ReadInteger(object, "downloaded", &out->downloaded, 0, kMaxCount, error) ||
        !ReadInteger(object, "conflicts", &out->conflicts, 0, kMaxCount, error) ||
        !ReadInteger(object, "failed", &out->failed, 0, kMaxCount, error) ||
        !ReadInteger(object, "queue_depth", &out->queue_depth, 0, kMaxQueueDepth, error)) {
      return;
    }
    out->interface = static_cast<std::uint32_t>(interface_version);

    const json::Value* download = ReadObject(object, "download", error);
    if (download == nullptr) {
      return;
    }
    if (!ReadEnum(*download, "state", kAllDownloadStates, ToString, "a download state",
                  &out->download.state, error) ||
        !ReadInteger(*download, "rom_id", &out->download.rom_id, 0, kMaxId, error) ||
        !ReadText(*download, "fs_name", &out->download.fs_name, error) ||
        !ReadInteger(*download, "bytes_done", &out->download.bytes_done, 0, kMaxBytes, error) ||
        !ReadInteger(*download, "bytes_total", &out->download.bytes_total, 0, kMaxBytes,
                     error)) {
      return;
    }
  });
}

std::string EncodeConfigView(const ConfigView& view) {
  std::string out("{");
  AppendKey(&out, "server", /*first=*/true);
  out += '{';
  AppendText(&out, "url", view.config.server.url, /*first=*/true);
  out += '}';

  AppendKey(&out, "sync");
  out += '{';
  AppendBool(&out, "enabled", view.config.sync.enabled, /*first=*/true);
  AppendInteger(&out, "interval_min", view.config.sync.interval_min);
  AppendBool(&out, "on_boot", view.config.sync.on_boot);
  AppendBool(&out, "saves", view.config.sync.saves);
  AppendBool(&out, "states", view.config.sync.states);
  AppendBool(&out, "conflict_show", view.config.sync.conflict_show);
  out += '}';

  AppendKey(&out, "downloads");
  out += '{';
  AppendBool(&out, "enabled", view.config.downloads.enabled, /*first=*/true);
  AppendBool(&out, "verify_hash", view.config.downloads.verify_hash);
  AppendBool(&out, "resume", view.config.downloads.resume);
  out += '}';

  AppendKey(&out, "platforms");
  out += view.platforms_truncated ? "{}" : EncodePlatformMap(view.config);
  AppendBool(&out, "platforms_truncated", view.platforms_truncated);

  AppendKey(&out, "diagnostics");
  out += EncodeDiagnosticArray(view.diagnostics);
  out += '}';
  return out;
}

Decoded<ConfigView> DecodeConfigView(std::string_view text) {
  return DecodeObject<ConfigView>(
      text, "config", [](const json::Value& object, ConfigView* out, json::Error* error) {
        const json::Value* server = ReadObject(object, "server", error);
        if (server == nullptr || !ReadText(*server, "url", &out->config.server.url, error)) {
          return;
        }

        const json::Value* sync_section = ReadObject(object, "sync", error);
        if (sync_section == nullptr) {
          return;
        }
        std::int64_t interval = 0;
        if (!ReadBool(*sync_section, "enabled", &out->config.sync.enabled, error) ||
            !ReadInteger(*sync_section, "interval_min", &interval, config::kMinIntervalMinutes,
                         config::kMaxIntervalMinutes, error) ||
            !ReadBool(*sync_section, "on_boot", &out->config.sync.on_boot, error) ||
            !ReadBool(*sync_section, "saves", &out->config.sync.saves, error) ||
            !ReadBool(*sync_section, "states", &out->config.sync.states, error) ||
            !ReadBool(*sync_section, "conflict_show", &out->config.sync.conflict_show, error)) {
          return;
        }
        out->config.sync.interval_min = static_cast<int>(interval);

        const json::Value* downloads = ReadObject(object, "downloads", error);
        if (downloads == nullptr ||
            !ReadBool(*downloads, "enabled", &out->config.downloads.enabled, error) ||
            !ReadBool(*downloads, "verify_hash", &out->config.downloads.verify_hash, error) ||
            !ReadBool(*downloads, "resume", &out->config.downloads.resume, error)) {
          return;
        }

        if (!ReadPlatformMap(object, &out->config, error) ||
            !ReadBool(object, "platforms_truncated", &out->platforms_truncated, error) ||
            !ReadDiagnosticArray(object, &out->diagnostics, error)) {
          return;
        }
      });
}

std::string EncodeDiagnostics(const std::vector<config::Diagnostic>& diagnostics) {
  std::string out("{");
  AppendKey(&out, "diagnostics", /*first=*/true);
  out += EncodeDiagnosticArray(diagnostics);
  out += '}';
  return out;
}

Decoded<std::vector<config::Diagnostic>> DecodeDiagnostics(std::string_view text) {
  return DecodeObject<std::vector<config::Diagnostic>>(
      text, "diagnostics",
      [](const json::Value& object, std::vector<config::Diagnostic>* out, json::Error* error) {
        ReadDiagnosticArray(object, out, error);
      });
}

std::string EncodeConfigEdit(const ConfigEdit& edit) {
  std::string out("{");
  AppendKey(&out, "assignments", /*first=*/true);
  out += '[';
  bool first = true;
  for (const ConfigAssignment& assignment : edit.assignments) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += '{';
    AppendText(&out, "section", assignment.section, /*first=*/true);
    AppendText(&out, "key", assignment.key);
    AppendText(&out, "value", assignment.value);
    AppendBool(&out, "remove", assignment.remove);
    out += '}';
  }
  out += "]}";
  return out;
}

Decoded<ConfigEdit> DecodeConfigEdit(std::string_view text) {
  return DecodeObject<ConfigEdit>(
      text, "config edit", [](const json::Value& object, ConfigEdit* out, json::Error* error) {
        const json::Value* array = ReadArray(object, "assignments", kMaxAssignments, error);
        if (array == nullptr) {
          return;
        }
        for (const json::Value& element : array->elements()) {
          if (!element.is_object()) {
            Fail(error, "assignments", "holds something that is not an assignment");
            return;
          }
          ConfigAssignment assignment;
          if (!ReadNonEmptyText(element, "section", &assignment.section, error) ||
              !ReadNonEmptyText(element, "key", &assignment.key, error) ||
              !ReadText(element, "value", &assignment.value, error) ||
              !ReadBool(element, "remove", &assignment.remove, error)) {
            return;
          }
          out->assignments.push_back(std::move(assignment));
        }
      });
}

std::string EncodeEnabled(bool enabled) {
  std::string out("{");
  AppendBool(&out, "enabled", enabled, /*first=*/true);
  out += '}';
  return out;
}

Decoded<bool> DecodeEnabled(std::string_view text) {
  return DecodeObject<bool>(text, "enabled",
                            [](const json::Value& object, bool* out, json::Error* error) {
                              ReadBool(object, "enabled", out, error);
                            });
}

std::string EncodeSyncOutcome(SyncOutcome outcome) {
  std::string out("{");
  AppendText(&out, "outcome", ToString(outcome), /*first=*/true);
  out += '}';
  return out;
}

Decoded<SyncOutcome> DecodeSyncOutcome(std::string_view text) {
  return DecodeObject<SyncOutcome>(
      text, "sync outcome", [](const json::Value& object, SyncOutcome* out, json::Error* error) {
        ReadEnum(object, "outcome", kAllSyncOutcomes, ToString, "a sync outcome", out, error);
      });
}

std::string EncodeRomId(std::int64_t rom_id) {
  std::string out("{");
  AppendInteger(&out, "rom_id", rom_id, /*first=*/true);
  out += '}';
  return out;
}

Decoded<std::int64_t> DecodeRomId(std::string_view text) {
  return DecodeObject<std::int64_t>(
      text, "rom id", [](const json::Value& object, std::int64_t* out, json::Error* error) {
        // A rom id is positive: RomM's ids are, and a `0` here is a caller that
        // never filled the field in -- which `Enqueue` must not answer
        // `kUnknownRom` to, because that reads as "the library moved".
        ReadInteger(object, "rom_id", out, 1, kMaxId, error);
      });
}

std::string EncodeQueuePosition(std::int32_t position) {
  std::string out("{");
  AppendInteger(&out, "position", position, /*first=*/true);
  out += '}';
  return out;
}

Decoded<std::int32_t> DecodeQueuePosition(std::string_view text) {
  return DecodeObject<std::int32_t>(
      text, "queue position", [](const json::Value& object, std::int32_t* out,
                                 json::Error* error) {
        std::int64_t value = 0;
        if (ReadInteger(object, "position", &value, 1, kMaxQueueDepth, error)) {
          *out = static_cast<std::int32_t>(value);
        }
      });
}

std::string EncodeListRequest(const ListRequest& request) {
  std::string out("{");
  AppendText(&out, "kind", ToString(request.kind), /*first=*/true);
  AppendInteger(&out, "platform_id", request.platform_id);
  AppendText(&out, "search", request.search);
  AppendInteger(&out, "page_size", request.page_size);
  out += '}';
  return out;
}

Decoded<ListRequest> DecodeListRequest(std::string_view text) {
  return DecodeObject<ListRequest>(
      text, "list request", [](const json::Value& object, ListRequest* out, json::Error* error) {
        std::int64_t page_size = 0;
        if (!ReadEnum(object, "kind", kAllListKinds, ToString, "a list kind", &out->kind,
                      error) ||
            !ReadInteger(object, "platform_id", &out->platform_id, 0, kMaxId, error) ||
            !ReadText(object, "search", &out->search, error) ||
            // Clamped by `ServiceCore::ListBegin`, not here: what the client
            // wanted is a legitimate thing to say, and a page size of zero is
            // the one value that cannot mean anything.
            !ReadInteger(object, "page_size", &page_size, 1, kMaxRequestedPageSize, error)) {
          return;
        }
        out->page_size = static_cast<std::int32_t>(page_size);
      });
}

std::string EncodeCursor(Cursor cursor) {
  std::string out("{");
  AppendInteger(&out, "cursor", cursor, /*first=*/true);
  out += '}';
  return out;
}

Decoded<Cursor> DecodeCursor(std::string_view text) {
  return DecodeObject<Cursor>(text, "cursor",
                              [](const json::Value& object, Cursor* out, json::Error* error) {
                                std::int64_t value = 0;
                                // `0` is never a live cursor, so it is refused
                                // here rather than reaching the engine as an
                                // address that happens to miss.
                                if (ReadInteger(object, "cursor", &value, 1, 0xFFFFFFFF, error)) {
                                  *out = static_cast<Cursor>(value);
                                }
                              });
}

std::string EncodeListPage(const ListPage& page) {
  std::string out("{");
  AppendKey(&out, "items", /*first=*/true);
  out += '[';
  bool first_item = true;
  for (const ListItem& item : page.items) {
    if (!first_item) {
      out += ',';
    }
    first_item = false;
    out += '{';
    bool first_field = true;
    for (const ListField& field : item.fields) {
      AppendKey(&out, field.key, first_field);
      first_field = false;
      switch (field.value.type) {
        case ListValue::Type::kString:
          out += json::Quote(field.value.text);
          break;
        case ListValue::Type::kInteger:
          out += std::to_string(field.value.number);
          break;
        case ListValue::Type::kBool:
          out += field.value.flag ? "true" : "false";
          break;
      }
    }
    out += '}';
  }
  out += ']';
  AppendBool(&out, "has_more", page.has_more);
  AppendBool(&out, "pending", page.pending);
  out += '}';
  return out;
}

Decoded<ListPage> DecodeListPage(std::string_view text) {
  return DecodeObject<ListPage>(
      text, "list page", [](const json::Value& object, ListPage* out, json::Error* error) {
        const json::Value* items =
            ReadArray(object, "items", static_cast<std::size_t>(kMaxPageSize), error);
        if (items == nullptr) {
          return;
        }
        for (const json::Value& element : items->elements()) {
          if (!element.is_object()) {
            Fail(error, "items", "holds something that is not an item");
            return;
          }
          if (element.size() > kMaxItemFields) {
            Fail(error, "items", "holds an item with more fields than this build accepts");
            return;
          }
          ListItem item;
          for (const json::Member& member : element.members()) {
            ListField field;
            field.key = member.key;
            // A flat object of scalars, and nothing else: no nesting, no arrays,
            // no nulls. That is what keeps a page's size predictable and this
            // decoder unable to recurse (ipc.hpp, `ListValue`).
            if (member.value.is_string()) {
              if (member.value.string().find('\0') != std::string::npos) {
                Fail(error, "items", "holds a field carrying a NUL");
                return;
              }
              field.value = ListValue::Text(member.value.string());
            } else if (member.value.is_integer()) {
              field.value = ListValue::Integer(member.value.integer());
            } else if (member.value.is_bool()) {
              field.value = ListValue::Flag(member.value.boolean());
            } else {
              Fail(error, "items",
                   std::string("holds a field that is not a scalar: ") +
                       json::ToString(member.value.type()));
              return;
            }
            item.fields.push_back(std::move(field));
          }
          out->items.push_back(std::move(item));
        }
        if (!ReadBool(object, "has_more", &out->has_more, error) ||
            !ReadBool(object, "pending", &out->pending, error)) {
          return;
        }
      });
}

bool AppendIfItFits(ListPage* page, ListItem item) {
  if (page->items.size() >= static_cast<std::size_t>(kMaxPageSize)) {
    return false;
  }
  page->items.push_back(std::move(item));
  if (Fits(EncodeListPage(*page))) {
    return true;
  }
  page->items.pop_back();
  return false;
}

std::string EncodeEmpty() { return "{}"; }

json::Error DecodeEmpty(std::string_view text) {
  json::Error error;
  if (text.size() > kMaxPayloadBytes) {
    error.message = "empty payload: longer than the IPC cap";
    return error;
  }
  const json::ParseResult document = json::Parse(text);
  if (!document.ok()) {
    return document.error;
  }
  if (!document.value.is_object()) {
    error.message = std::string("empty payload: expected an object, got ") +
                    json::ToString(document.value.type());
  }
  return error;
}

}  // namespace rommsync::ipc
