#include "rommsync/download.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/config.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/json.hpp"
#include "rommsync/rom_index.hpp"

namespace rommsync::download {
namespace {

std::string Describe(const std::string& path, std::string_view what) {
  return path + ": " + std::string(what);
}

void Add(std::vector<std::string>* diagnostics, std::string message) {
  if (diagnostics->size() < kMaxDiagnostics) {
    diagnostics->push_back(std::move(message));
  }
}

/// A message cut to `kMaxMessageChars`, on a byte boundary.
///
/// Bytes rather than characters, and no ellipsis: the bound exists so the file
/// stays inside `kMaxQueueBytes`, and a UTF-8 sequence split across the cut is
/// still a bounded string. Every message this module writes is ASCII from its
/// own source; the cut is for the ones `http::Result::message` brings in from a
/// backend.
std::string Clip(std::string message) {
  if (message.size() > kMaxMessageChars) {
    message.resize(kMaxMessageChars);
  }
  return message;
}

/// The bar an entry is held to in both directions.
///
/// The same bar on the way in and on the way out, `state::Usable`'s reasoning:
/// an entry this refuses to write is one `ParseQueue` would have thrown the
/// whole file away over, so writing it would cost the *next* boot its queue.
bool Usable(const QueueEntry& entry, std::string* why) {
  if (entry.rom_id <= 0) {
    *why = "rom_id must be a positive RomM rom id";
    return false;
  }
  if (entry.size_bytes < 0) {
    *why = "size_bytes is negative";
    return false;
  }
  if (entry.bytes_done < 0) {
    *why = "bytes_done is negative";
    return false;
  }
  if (entry.attempts < 0) {
    *why = "attempts is negative";
    return false;
  }
  if (entry.queued_at < 0) {
    *why = "queued_at is before the epoch";
    return false;
  }
  if (entry.message.size() > kMaxMessageChars) {
    *why = "message is longer than " + std::to_string(kMaxMessageChars) + " bytes";
    return false;
  }
  // Both are joined into a path, or came out of one, so the bound `config`
  // enforces on a destination is the bound an entry may carry.
  if (entry.fs_name.size() > config::kMaxPathLength ||
      entry.destination.size() > config::kMaxPathLength ||
      entry.platform_fs_slug.size() > config::kMaxPathLength) {
    *why = "a path field is longer than " + std::to_string(config::kMaxPathLength) + " bytes";
    return false;
  }
  return true;
}

std::string SerializeEntry(const QueueEntry& entry) {
  std::string out("{\"rom_id\":");
  out += std::to_string(entry.rom_id);
  out += ",\"platform_fs_slug\":";
  out += json::Quote(entry.platform_fs_slug);
  out += ",\"fs_name\":";
  out += json::Quote(entry.fs_name);
  out += ",\"size_bytes\":";
  out += std::to_string(entry.size_bytes);
  // `null` rather than an omitted key, `state_db`'s reasoning: "the library has
  // no hash for this rom" is an answer, and a reader that could not tell it from
  // a missing field would have to guess which one it was looking at.
  out += ",\"sha1_hash\":";
  out += entry.sha1_hash.empty() ? std::string("null") : json::Quote(entry.sha1_hash);
  out += ",\"destination\":";
  out += json::Quote(entry.destination);
  out += ",\"state\":";
  out += json::Quote(ToString(entry.state));
  out += ",\"bytes_done\":";
  out += std::to_string(entry.bytes_done);
  out += ",\"attempts\":";
  out += std::to_string(entry.attempts);
  out += ",\"message\":";
  out += json::Quote(entry.message);
  out += ",\"queued_at\":";
  out += std::to_string(entry.queued_at);
  out += "}";
  return out;
}

/// A `string | null` field read straight off the object.
///
/// `json::Reader::RequiredNullable` refuses an empty string, which is right for
/// a `device_id` and wrong here: it would make an entry whose `platform_fs_slug`
/// has not been resolved yet -- every entry between `Enqueue` and the worker's
/// first look at it -- a file the reader discards.
bool ReadString(const json::Value& object, std::string_view key, std::string* out,
                std::string* why) {
  const json::Value* member = object.Find(key);
  if (member == nullptr) {
    *why = "field " + std::string(key) + ": missing";
    return false;
  }
  if (member->is_null()) {
    out->clear();
    return true;
  }
  if (!member->is_string()) {
    *why = "field " + std::string(key) + ": expected a string or null";
    return false;
  }
  if (member->string().find('\0') != std::string::npos) {
    // json.hpp's reason: an escaped NUL is legal JSON, and every C API
    // downstream -- a path handed to `fopen`, a URL -- stops at it, so the value
    // that gets used would not be the value that was checked.
    *why = "field " + std::string(key) + ": carries a NUL";
    return false;
  }
  *out = member->string();
  return true;
}

bool ReadInteger(const json::Value& object, std::string_view key, std::int64_t* out,
                 std::string* why) {
  const json::Value* member = object.Find(key);
  if (member == nullptr) {
    *why = "field " + std::string(key) + ": missing";
    return false;
  }
  if (!member->is_integer()) {
    *why = "field " + std::string(key) + ": expected a whole number";
    return false;
  }
  *out = member->integer();
  return true;
}

bool ParseEntry(const json::Value& object, QueueEntry* out, std::string* why) {
  if (!object.is_object()) {
    *why = "a queue entry is not an object";
    return false;
  }

  QueueEntry entry;
  std::int64_t attempts = 0;
  std::string state;
  const bool read = ReadInteger(object, "rom_id", &entry.rom_id, why) &&
                    ReadString(object, "platform_fs_slug", &entry.platform_fs_slug, why) &&
                    ReadString(object, "fs_name", &entry.fs_name, why) &&
                    ReadInteger(object, "size_bytes", &entry.size_bytes, why) &&
                    ReadString(object, "sha1_hash", &entry.sha1_hash, why) &&
                    ReadString(object, "destination", &entry.destination, why) &&
                    ReadString(object, "state", &state, why) &&
                    ReadInteger(object, "bytes_done", &entry.bytes_done, why) &&
                    ReadInteger(object, "attempts", &attempts, why) &&
                    ReadString(object, "message", &entry.message, why) &&
                    ReadInteger(object, "queued_at", &entry.queued_at, why);
  if (!read) {
    return false;
  }
  if (!ParseQueueState(state, &entry.state)) {
    // Named rather than defaulted to `kQueued`: a state this build does not know
    // came from a different release, and reading it as "waiting for the worker"
    // would re-download a rom that release had already finished.
    *why = "field state: not a state this build knows";
    return false;
  }
  if (attempts < 0 || attempts > kMaxAttemptsRecorded) {
    *why = "field attempts: outside the range an entry can claim";
    return false;
  }
  entry.attempts = static_cast<int>(attempts);
  if (!Usable(entry, why)) {
    return false;
  }
  *out = std::move(entry);
  return true;
}

/// Read at most `kMaxQueueBytes` of `path`. The bound and its reasoning are
/// `io::ReadBounded`'s, shared with `config.ini` rather than restated here.
io::BoundedRead ReadBounded(const std::string& path, std::string* out) {
  return io::ReadBounded(path, kMaxQueueBytes, out);
}

/// The size of `path` in bytes, or `0` when it is not there.
///
/// `<cstdio>` rather than `<filesystem>`, which `core/` may not include
/// (core/AGENTS.md). It is used for one thing: how many bytes of a rom are
/// already on the card, `.part` included, which is what `bytes_done` means.
std::int64_t FileSizeBytes(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return 0;
  }
  std::int64_t size = 0;
  if (std::fseek(file, 0, SEEK_END) == 0) {
    const long told = std::ftell(file);
    size = told > 0 ? static_cast<std::int64_t>(told) : 0;
  }
  std::fclose(file);
  return size;
}

/// Bytes on the card for `destination`: the finished file if it is there, the
/// `.part` otherwise. `http::DownloadTarget` renames one onto the other, so at
/// most one of the two is the answer at any moment.
std::int64_t BytesOnCard(const std::string& destination) {
  const std::int64_t whole = FileSizeBytes(destination);
  return whole > 0 ? whole : FileSizeBytes(destination + ".part");
}

/// Whole Unix seconds now. Not injected: `queued_at` is stamped once, is never
/// compared against anything, and exists so a queue screen can say how long a
/// rom has been waiting.
std::int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// --- states -------------------------------------------------------------------

const char* ToString(QueueState state) {
  switch (state) {
    case QueueState::kQueued:
      return "queued";
    case QueueState::kActive:
      return "active";
    case QueueState::kVerifying:
      return "verifying";
    case QueueState::kDone:
      return "done";
    case QueueState::kFailed:
      return "failed";
    case QueueState::kSkipped:
      return "skipped";
  }
  return "queued";
}

bool ParseQueueState(std::string_view text, QueueState* out) {
  static constexpr QueueState kStates[] = {QueueState::kQueued, QueueState::kActive,
                                           QueueState::kVerifying, QueueState::kDone,
                                           QueueState::kFailed,  QueueState::kSkipped};
  for (const QueueState state : kStates) {
    if (text == ToString(state)) {
      *out = state;
      return true;
    }
  }
  return false;
}

bool Terminal(QueueState state) {
  return state == QueueState::kDone || state == QueueState::kFailed ||
         state == QueueState::kSkipped;
}

// --- the queue ----------------------------------------------------------------

std::vector<QueueEntry>::iterator Queue::FindLocked(std::int64_t rom_id) {
  return std::find_if(entries_.begin(), entries_.end(),
                      [rom_id](const QueueEntry& entry) { return entry.rom_id == rom_id; });
}

ipc::Error Queue::Enqueue(std::int64_t rom_id, std::int32_t* position) {
  if (rom_id <= 0) {
    // There is no such rom, and saying so here means every caller is held to it
    // -- `EnqueueRom` checks the library, and a `sysmodule` engine that skipped
    // that check would otherwise write a `"rom_id":0` entry the reader discards.
    return ipc::Error::kUnknownRom;
  }
  std::lock_guard<std::mutex> held(mutex_);

  const auto existing = FindLocked(rom_id);
  if (existing != entries_.end()) {
    if (!Terminal(existing->state)) {
      return ipc::Error::kDuplicate;
    }
    // "Try again" -- see the header. Erased and re-added rather than reset in
    // place, so a rom re-queued after a failure goes behind the entries that
    // were already waiting rather than ahead of them.
    entries_.erase(existing);
  } else if (entries_.size() >= kMaxQueueEntries) {
    return ipc::Error::kQueueFull;
  }

  QueueEntry entry;
  entry.rom_id = rom_id;
  entry.queued_at = NowSeconds();
  entries_.push_back(std::move(entry));

  std::int32_t pending = 0;
  for (const QueueEntry& row : entries_) {
    pending += Terminal(row.state) ? 0 : 1;
  }
  *position = pending;
  return ipc::Error::kOk;
}

ipc::Error Queue::Remove(std::int64_t rom_id) {
  std::lock_guard<std::mutex> held(mutex_);
  const auto found = FindLocked(rom_id);
  if (found == entries_.end()) {
    return ipc::Error::kNotQueued;
  }
  entries_.erase(found);
  return ipc::Error::kOk;
}

void Queue::Clear() {
  std::lock_guard<std::mutex> held(mutex_);
  entries_.clear();
}

std::vector<QueueEntry> Queue::Snapshot() const {
  std::lock_guard<std::mutex> held(mutex_);
  return entries_;
}

QueueEntry Queue::Find(std::int64_t rom_id) const {
  std::lock_guard<std::mutex> held(mutex_);
  for (const QueueEntry& entry : entries_) {
    if (entry.rom_id == rom_id) {
      return entry;
    }
  }
  return {};
}

void Queue::Reset(std::vector<QueueEntry> entries) {
  if (entries.size() > kMaxQueueEntries) {
    entries.resize(kMaxQueueEntries);
  }
  std::lock_guard<std::mutex> held(mutex_);
  entries_ = std::move(entries);
}

std::size_t Queue::pending() const {
  std::lock_guard<std::mutex> held(mutex_);
  std::size_t count = 0;
  for (const QueueEntry& entry : entries_) {
    count += Terminal(entry.state) ? 0 : 1;
  }
  return count;
}

std::size_t Queue::size() const {
  std::lock_guard<std::mutex> held(mutex_);
  return entries_.size();
}

ipc::DownloadSnapshot Queue::CurrentDownload() const {
  std::lock_guard<std::mutex> held(mutex_);
  for (const QueueEntry& entry : entries_) {
    if (entry.state != QueueState::kActive && entry.state != QueueState::kVerifying) {
      continue;
    }
    ipc::DownloadSnapshot snapshot;
    snapshot.state = entry.state == QueueState::kVerifying ? ipc::DownloadState::kVerifying
                                                           : ipc::DownloadState::kDownloading;
    snapshot.rom_id = entry.rom_id;
    snapshot.fs_name = entry.fs_name;
    snapshot.bytes_done = entry.bytes_done;
    snapshot.bytes_total = entry.size_bytes;
    return snapshot;
  }
  // A queue with something waiting and nothing moving is `kQueued`, not
  // `kIdle`: the status screen has to tell "nothing to do" from "about to
  // start", and `kIdle` on a queue three deep reads as a worker that stopped.
  //
  // `ipc::DownloadState::kFailed` is deliberately not produced here. It is the
  // *current* entry's state, and a failed entry is not current -- what the
  // status screen does with a queue whose last entry failed is #22's, and it
  // has the whole snapshot to decide from.
  for (const QueueEntry& entry : entries_) {
    if (entry.state == QueueState::kQueued) {
      ipc::DownloadSnapshot snapshot;
      snapshot.state = ipc::DownloadState::kQueued;
      snapshot.rom_id = entry.rom_id;
      snapshot.fs_name = entry.fs_name;
      snapshot.bytes_done = entry.bytes_done;
      snapshot.bytes_total = entry.size_bytes;
      return snapshot;
    }
  }
  return {};
}

QueueEntry Queue::NextPending() const {
  std::lock_guard<std::mutex> held(mutex_);
  for (const QueueEntry& entry : entries_) {
    if (!Terminal(entry.state)) {
      return entry;
    }
  }
  return {};
}

bool Queue::Update(const QueueEntry& entry) {
  std::lock_guard<std::mutex> held(mutex_);
  const auto found = FindLocked(entry.rom_id);
  if (found == entries_.end()) {
    return false;
  }
  *found = entry;
  return true;
}

ipc::Error EnqueueRom(Queue& queue, const roms::RomIndex& library, std::int64_t rom_id,
                      std::int32_t* position) {
  const roms::Rom* rom = library.ById(rom_id);
  if (rom == nullptr) {
    return ipc::Error::kUnknownRom;
  }
  if (rom->has_multiple_files) {
    return ipc::Error::kMultiFile;
  }
  return queue.Enqueue(rom_id, position);
}

// --- the file -----------------------------------------------------------------

std::string LoadedQueue::DescribeDiagnostics() const {
  std::string out;
  for (const std::string& diagnostic : diagnostics) {
    out += diagnostic;
    out += "\n";
  }
  return out;
}

const char* ToString(StoreError error) {
  switch (error) {
    case StoreError::kNone:
      return "none";
    case StoreError::kTooManyEntries:
      return "too_many_entries";
    case StoreError::kTooLarge:
      return "too_large";
    case StoreError::kOpenFailed:
      return "open_failed";
    case StoreError::kWriteFailed:
      return "write_failed";
    case StoreError::kCommitFailed:
      return "commit_failed";
  }
  return "none";
}

std::string SerializeQueue(const std::vector<QueueEntry>& entries) {
  std::string out("{\"format\":");
  out += json::Quote(kFormatMagic);
  out += ",\"version\":";
  out += std::to_string(kFormatVersion);
  out += ",\"entries\":[";
  for (std::size_t at = 0; at < entries.size(); ++at) {
    if (at != 0) {
      out += ",";
    }
    out += SerializeEntry(entries[at]);
  }
  out += "]}\n";
  return out;
}

LoadedQueue ParseQueue(std::string_view text) {
  LoadedQueue loaded;

  const json::ParseResult document = json::Parse(text);
  if (!document.ok()) {
    // Naming the complaint rather than quoting the text: the bytes may be
    // anything a corrupt card region holds.
    Add(&loaded.diagnostics, std::string(kQueueFileName) + " is not readable JSON (" +
                                 document.error.Describe() + "); the queue is discarded");
    return loaded;
  }

  std::string format;
  std::int64_t version = 0;
  json::Reader reader(document.value, "download queue");
  reader.Required("format", &format);
  reader.Required("version", &version);
  if (!reader.ok()) {
    Add(&loaded.diagnostics, std::string(kQueueFileName) + ": " + reader.error().Describe() +
                                 "; the queue is discarded");
    return loaded;
  }
  if (format != kFormatMagic || version != kFormatVersion) {
    Add(&loaded.diagnostics, std::string(kQueueFileName) + " is not a \"" + kFormatMagic +
                                 "\" version " + std::to_string(kFormatVersion) +
                                 " file; the queue is discarded");
    return loaded;
  }

  const json::Value* entries = document.value.Find("entries");
  if (entries == nullptr || !entries->is_array()) {
    Add(&loaded.diagnostics,
        std::string(kQueueFileName) + ": field entries: expected an array; the queue is discarded");
    return loaded;
  }
  if (entries->elements().size() > kMaxQueueEntries) {
    Add(&loaded.diagnostics, std::string(kQueueFileName) + " holds more than the " +
                                 std::to_string(kMaxQueueEntries) +
                                 " entries a queue may have; it is discarded");
    return loaded;
  }

  std::vector<QueueEntry> rows;
  std::vector<std::string> complaints;
  for (std::size_t at = 0; at < entries->elements().size(); ++at) {
    QueueEntry entry;
    std::string why;
    if (!ParseEntry(entries->elements()[at], &entry, &why)) {
      Add(&complaints,
          std::string(kQueueFileName) + " entry " + std::to_string(at + 1) + ": " + why);
      continue;
    }
    const bool duplicate = std::any_of(
        rows.begin(), rows.end(),
        [&entry](const QueueEntry& row) { return row.rom_id == entry.rom_id; });
    if (duplicate) {
      Add(&complaints, std::string(kQueueFileName) + " entry " + std::to_string(at + 1) +
                           ": a second entry for the same rom");
      continue;
    }
    rows.push_back(std::move(entry));
  }

  if (!complaints.empty()) {
    // All of it, not the entries that parsed -- see the header. A truncation
    // leaves a prefix that is individually well-formed and collectively a lie,
    // and half a queue is a state no caller can reason about.
    loaded.diagnostics = std::move(complaints);
    // Unconditionally, not through `Add`: the per-entry complaints are capped,
    // and a file bad enough to hit the cap is exactly the one whose reader most
    // needs to be told the *whole* queue went.
    loaded.diagnostics.push_back(std::string(kQueueFileName) +
                                 " is not intact; the whole queue is discarded");
    return loaded;
  }

  loaded.entries = std::move(rows);
  return loaded;
}

LoadedQueue LoadQueue(const std::string& path) {
  std::string contents;
  const io::BoundedRead outcome = ReadBounded(path, &contents);
  if (outcome == io::BoundedRead::kOk) {
    return ParseQueue(contents);
  }

  if (outcome == io::BoundedRead::kMissing) {
    // The one moment `queue.json` legitimately does not exist is the window
    // `io::WriteAtomically` opens between its two renames, and the previous
    // queue is sitting under `.old`. Only a *missing* file takes this branch.
    const std::string previous_path = io::PreviousPathFor(path);
    std::string previous;
    if (ReadBounded(previous_path, &previous) == io::BoundedRead::kOk) {
      LoadedQueue recovered = ParseQueue(previous);
      // Whatever came back, this branch answers -- `state_db`'s reasoning:
      // falling through would report "there is no queue yet", the message a
      // brand-new card produces, and would drop `recovered`'s diagnostics on the
      // floor. "A commit was interrupted *and* what it left behind is unusable"
      // is the one state worth seeing in a log, and it is the state that would
      // otherwise have been silent.
      std::vector<std::string> diagnostics{Describe(
          path, recovered.entries.empty()
                    ? "is missing and " + previous_path +
                          " holds no usable queue either -- a write of it was interrupted"
                    : "is missing; the queue was recovered from " + previous_path +
                          " after an interrupted write")};
      for (std::string& diagnostic : recovered.diagnostics) {
        Add(&diagnostics, std::move(diagnostic));
      }
      recovered.diagnostics = std::move(diagnostics);
      return recovered;
    }
  }

  LoadedQueue loaded;
  switch (outcome) {
    case io::BoundedRead::kOk:
      break;
    case io::BoundedRead::kMissing:
      Add(&loaded.diagnostics, Describe(path, "does not exist yet; the queue is empty"));
      break;
    case io::BoundedRead::kUnreadable:
      Add(&loaded.diagnostics,
          Describe(path, "could not be read; the queue is empty for this boot"));
      break;
    case io::BoundedRead::kTooLarge:
      Add(&loaded.diagnostics,
          Describe(path, "is larger than the " + std::to_string(kMaxQueueBytes) +
                             " bytes a queue may take; it is discarded"));
      break;
  }
  return loaded;
}

StoreResult SaveQueue(const std::string& path, const Queue& queue) {
  StoreResult result;
  const std::vector<QueueEntry> entries = queue.Snapshot();

  if (entries.size() > kMaxQueueEntries) {
    result.error = StoreError::kTooManyEntries;
    result.message =
        Describe(path, "holds " + std::to_string(entries.size()) + " entries; " +
                           std::to_string(kMaxQueueEntries) + " is the most a queue may have");
    return result;
  }
  for (const QueueEntry& entry : entries) {
    std::string why;
    if (!Usable(entry, &why)) {
      // A refusal rather than a skip -- see the header. It names the rom id and
      // not the rom, because the name is the field most likely to be the problem
      // and the one that must not reach a log (config.hpp).
      result.error = StoreError::kTooLarge;
      result.message = Describe(path, "the entry for rom " + std::to_string(entry.rom_id) +
                                          " cannot be written: " + why);
      return result;
    }
  }

  const std::string contents = SerializeQueue(entries);
  if (contents.size() > kMaxQueueBytes) {
    result.error = StoreError::kTooLarge;
    result.message =
        Describe(path, "would be " + std::to_string(contents.size()) + " bytes; " +
                           std::to_string(kMaxQueueBytes) + " is the most a queue may take");
    return result;
  }

  const io::WriteResult written = io::WriteAtomically(path, contents);
  switch (written.error) {
    case io::WriteError::kNone:
      break;
    case io::WriteError::kOpenFailed:
      result.error = StoreError::kOpenFailed;
      break;
    case io::WriteError::kWriteFailed:
      result.error = StoreError::kWriteFailed;
      break;
    case io::WriteError::kCommitFailed:
      result.error = StoreError::kCommitFailed;
      break;
  }
  result.message = written.message;
  return result;
}

// --- the endpoints ------------------------------------------------------------

std::string ContentUrl(std::string_view base_url, std::int64_t rom_id, std::string_view fs_name) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out(base_url);
  out += "/api/roms/";
  out += std::to_string(rom_id);
  out += "/content/";
  for (const char character : fs_name) {
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

json::Error ParseRomDetail(std::string_view body, RomDetail* out) {
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    return document.error;
  }

  RomDetail detail;
  json::Reader reader(document.value, "rom detail");
  reader.Required("id", &detail.id);
  reader.Required("fs_name", &detail.fs_name);
  reader.Required("platform_fs_slug", &detail.platform_fs_slug);
  reader.Required("fs_size_bytes", &detail.size_bytes);
  reader.Required("has_multiple_files", &detail.has_multiple_files);
  reader.Required("missing_from_fs", &detail.missing_from_fs);
  if (!reader.ok()) {
    return reader.error();
  }

  // `sha1_hash` is `string | null` and the only field here allowed to have no
  // value: an unscanned library leaves it null, and M3-3 (#20) is what decides
  // what a download with nothing to check against is worth. A *missing* key is
  // still a shape error -- a body without it is not the schema this was written
  // against.
  std::optional<std::string> sha1;
  json::Reader hashes(document.value, "rom detail");
  if (!hashes.RequiredNullable("sha1_hash", &sha1)) {
    return hashes.error();
  }
  if (sha1.has_value()) {
    detail.sha1_hash = *sha1;
  }

  if (detail.id <= 0) {
    json::Error error;
    error.field = "id";
    error.message = "field id: expected a positive rom id";
    return error;
  }
  if (detail.size_bytes < 0) {
    json::Error error;
    error.field = "fs_size_bytes";
    error.message = "field fs_size_bytes: expected a size that is not negative";
    return error;
  }

  *out = std::move(detail);
  return {};
}

// --- the worker ---------------------------------------------------------------

const char* ToString(DrainOutcome outcome) {
  switch (outcome) {
    case DrainOutcome::kIdle:
      return "idle";
    case DrainOutcome::kDisabled:
      return "disabled";
    case DrainOutcome::kCompleted:
      return "completed";
    case DrainOutcome::kRetryable:
      return "retryable";
    case DrainOutcome::kCanceled:
      return "canceled";
    case DrainOutcome::kUnauthorized:
      return "unauthorized";
    case DrainOutcome::kStoreFailed:
      return "store_failed";
  }
  return "idle";
}

namespace {

/// What one exchange with RomM means for the entry that made it.
enum class Verdict {
  kOk,
  kRetry,         ///< nothing about this says the request was wrong; try it again
  kFatal,         ///< this rom will not download as asked, however often it is tried
  kUnauthorized,  ///< the token, not the rom. Every other entry would fail the same way.
  kCanceled,
};

/// Classify a completed exchange. `why` gets a sentence for
/// `QueueEntry::message`, and never quotes the rom.
Verdict Judge(const http::Result& result, std::string* why) {
  if (!result.ok()) {
    switch (result.error) {
      case http::Error::kCanceled:
        *why = "the download was stopped";
        return Verdict::kCanceled;
      case http::Error::kInvalidRequest:
      case http::Error::kWriteFailed:
        // Neither gets better by trying again: one is this client building a
        // request wrong, the other is the card refusing the write -- a mapped
        // folder that does not exist is the ordinary way to reach it.
        *why = std::string("the transfer could not be made: ") + http::ToString(result.error) +
               " (" + result.message + ")";
        return Verdict::kFatal;
      default:
        *why = std::string("the transfer did not complete: ") + http::ToString(result.error) +
               " (" + result.message + ")";
        return Verdict::kRetry;
    }
  }

  const int status = result.response.status;
  if (status >= 200 && status < 300) {
    return Verdict::kOk;
  }
  if (status == 401 || status == 403) {
    // Not this entry's problem, and a retried 401 is a 401 retried forever
    // (sync.hpp). The drain stops rather than spending the whole queue on it.
    *why = status == 401 ? "the server no longer accepts this console's token"
                         : "this console's token was not granted what a download needs";
    return Verdict::kUnauthorized;
  }
  if (status == 404) {
    *why = "the server has no such rom any more";
    return Verdict::kFatal;
  }
  if (status == 408 || status == 429 || status >= 500) {
    // A proxy and a rate limiter answer with the first two; a 5xx is the server
    // having a bad minute. All three are worth another request.
    *why = "the server answered " + std::to_string(status);
    return Verdict::kRetry;
  }
  *why = "the server refused the request with " + std::to_string(status);
  return Verdict::kFatal;
}

http::Request Authed(const WorkerOptions& options, std::string url) {
  http::Request request;
  request.method = http::Method::kGet;
  request.url = std::move(url);
  request.headers.push_back({"Authorization", "Bearer " + options.bearer_token});
  request.cancel = options.cancel;
  return request;
}

/// What a write of the queue did.
enum class Written {
  kOk,
  kGone,    ///< there is no such entry any more -- `Remove` ran while the worker was on it
  kFailed,  ///< `queue.json` could not be written
};

/// The state one entry's turn left the drain in.
struct Step {
  DrainOutcome outcome = DrainOutcome::kCompleted;
  StoreResult store;
  std::string message;

  /// Keep going round the loop. False for everything that ends the drain.
  bool advance() const { return outcome == DrainOutcome::kCompleted; }
};

/// One drain, and the bookkeeping its two request loops share.
class Drainer {
 public:
  Drainer(http::HttpClient& client, fs::FileSystem& filesystem, const config::Config& config,
          Queue& queue, const WorkerOptions& options)
      : client_(client),
        filesystem_(filesystem),
        config_(config),
        queue_(queue),
        options_(options),
        attempts_(options.max_attempts > 0 ? options.max_attempts : 1),
        backoff_(options.backoff) {}

  DrainResult Run() {
    DrainResult result;
    if (!config_.downloads.enabled) {
      // Not even opened. Switching downloads back on resumes exactly what was
      // there, which is what "idles, it does not drop the queue" means.
      result.outcome = DrainOutcome::kDisabled;
      result.message = "[downloads] enabled is false; the queue is untouched";
      return result;
    }

    bool worked = false;
    for (;;) {
      if (Canceled()) {
        result.outcome = DrainOutcome::kCanceled;
        result.message = "the drain was stopped";
        break;
      }
      const QueueEntry entry = queue_.NextPending();
      if (entry.rom_id == 0) {
        result.outcome = worked ? DrainOutcome::kCompleted : DrainOutcome::kIdle;
        break;
      }
      worked = true;
      const Step step = One(entry);
      if (!step.advance()) {
        result.outcome = step.outcome;
        result.store = step.store;
        result.message = step.message;
        break;
      }
    }

    result.downloaded = downloaded_;
    result.skipped = skipped_;
    result.failed = failed_;
    result.attempts = requests_;
    result.waited = waited_;
    return result;
  }

 private:
  bool Canceled() const { return options_.cancel != nullptr && options_.cancel->canceled(); }

  /// Wait out one backoff, doubling it for the next.
  ///
  /// `sync::Negotiate`'s scheme, clamped before its first use for the same
  /// reason: a caller whose `backoff` already exceeds `max_backoff` must not
  /// wait longer than the ceiling the header calls binding, exactly once.
  void Wait() {
    if (backoff_ > options_.max_backoff) {
      backoff_ = options_.max_backoff;
    }
    if (options_.wait != nullptr) {
      options_.wait(backoff_);
    } else {
      std::this_thread::sleep_for(backoff_);
    }
    waited_ += backoff_;
    backoff_ = backoff_ * 2 > options_.max_backoff ? options_.max_backoff : backoff_ * 2;
  }

  /// Write the queue down. **Every state transition goes through this**, so the
  /// file and memory never disagree by more than one `rename` -- which is what
  /// makes a power cut resumable rather than a restart.
  Written Persist(const QueueEntry& entry, Step* step) {
    if (!queue_.Update(entry)) {
      return Written::kGone;
    }
    const StoreResult stored = SaveQueue(options_.queue_path, queue_);
    if (stored.ok()) {
      return Written::kOk;
    }
    step->outcome = DrainOutcome::kStoreFailed;
    step->store = stored;
    step->message = stored.message;
    return Written::kFailed;
  }

  /// Finish `entry` in a terminal state, counting it.
  Step Settle(QueueEntry entry, QueueState state, std::string why) {
    Step step;
    entry.state = state;
    entry.message = Clip(std::move(why));
    switch (state) {
      case QueueState::kDone:
        ++downloaded_;
        break;
      case QueueState::kSkipped:
        ++skipped_;
        break;
      default:
        ++failed_;
        break;
    }
    Persist(entry, &step);
    return step;
  }

  /// Leave `entry` queued after a failure that says nothing about the rom, and
  /// end the drain with `outcome`.
  Step Requeue(QueueEntry entry, std::string why, DrainOutcome outcome) {
    Step step;
    entry.state = QueueState::kQueued;
    entry.message = Clip(std::move(why));
    if (!destination_.empty()) {
      // What a resume will start from. Counting the `.part` rather than this
      // request's bytes is the difference between a bar that carries on and one
      // that restarts at zero (#22).
      entry.bytes_done = BytesOnCard(destination_);
    }
    if (Persist(entry, &step) != Written::kFailed) {
      step.outcome = outcome;
      step.message = entry.message;
    }
    return step;
  }

  Step One(const QueueEntry& queued) {
    QueueEntry entry = queued;
    // Nothing carries over from the last entry: `Requeue` asks the card how many
    // bytes a *resolved* destination holds, and a stale one would answer about
    // another rom's file.
    destination_.clear();

    // 1. Who is this rom? `Enqueue` recorded an id and nothing else, because no
    // IPC command may block on the network (ipc.hpp).
    RomDetail detail;
    std::string why;
    switch (FetchDetail(&entry, &detail, &why)) {
      case Verdict::kOk:
        break;
      case Verdict::kRetry:
        return Requeue(std::move(entry), std::move(why), DrainOutcome::kRetryable);
      case Verdict::kCanceled:
        return Requeue(std::move(entry), std::move(why), DrainOutcome::kCanceled);
      case Verdict::kUnauthorized:
        return Requeue(std::move(entry), std::move(why), DrainOutcome::kUnauthorized);
      case Verdict::kFatal:
        return Settle(std::move(entry), QueueState::kFailed, std::move(why));
    }

    entry.platform_fs_slug = detail.platform_fs_slug;
    entry.fs_name = detail.fs_name;
    entry.size_bytes = detail.size_bytes;
    entry.sha1_hash = detail.sha1_hash;

    // 2. The two answers that are not a download. Both are refusals with a
    // sentence rather than a transfer that fails later: a disc set served over
    // `content` is a zip with no length at all (docs/API_CONTRACT.md), and a rom
    // missing from the server's own filesystem 404s part way in.
    if (detail.has_multiple_files) {
      return Settle(std::move(entry), QueueState::kSkipped,
                    "this rom is a disc set; rommsync v1 downloads single-file roms only");
    }
    if (detail.missing_from_fs) {
      return Settle(std::move(entry), QueueState::kFailed,
                    "the server's library no longer holds this rom's file");
    }

    // 3. Where does it go? `DestinationFor` refuses rather than repairs, and a
    // refused entry is a skip carrying its reason -- never a guessed folder
    // (config.hpp). That reason never quotes `fs_name`, which is why it can go
    // into the queue file as it stands.
    const config::RomDestination target =
        config_.DestinationFor({detail.platform_fs_slug, detail.fs_name});
    if (!target.ok()) {
      return Settle(std::move(entry), QueueState::kSkipped, target.reason);
    }
    entry.destination = target.path;

    destination_ = filesystem_.Resolve(target.path);
    if (destination_.empty()) {
      // A refusal, not a failure to find (file_system.hpp): falling back to the
      // SD path would resolve against the process's working directory.
      return Settle(std::move(entry), QueueState::kFailed,
                    "the mapped folder is not a path on this card");
    }

    // 4. The bytes.
    return Transfer(std::move(entry));
  }

  Verdict FetchDetail(QueueEntry* entry, RomDetail* out, std::string* why) {
    http::Request request =
        Authed(options_, options_.base_url + "/api/roms/" + std::to_string(entry->rom_id));
    request.headers.push_back({"Accept", "application/json"});
    request.timeout = options_.detail_timeout;

    for (int attempt = 1;; ++attempt) {
      const http::Result result = client_.Send(request);
      ++requests_;
      Verdict verdict = Judge(result, why);
      if (verdict == Verdict::kOk) {
        const json::Error shape = ParseRomDetail(result.response.body, out);
        if (shape.ok()) {
          return Verdict::kOk;
        }
        // A 2xx that is not a rom. Not retried: the same server answers the same
        // shape, and this is the failure that must never be papered over --
        // "this server is not the RomM this build was written against"
        // (rom_index.hpp).
        *why = "the rom could not be read: " + shape.Describe();
        verdict = Verdict::kFatal;
      }
      if (verdict != Verdict::kCanceled) {
        ++entry->attempts;
      }
      if (verdict != Verdict::kRetry || attempt >= attempts_ || Canceled()) {
        return verdict;
      }
      Wait();
    }
  }

  Step Transfer(QueueEntry entry) {
    http::Request request =
        Authed(options_, ContentUrl(options_.base_url, entry.rom_id, entry.fs_name));
    // No total ceiling. A 120 MiB rom on hotel Wi-Fi is slow rather than dead,
    // and `stall_timeout` is the timeout that tells those two apart (http.hpp).
    request.timeout = std::chrono::milliseconds{0};
    request.stall_timeout = options_.stall_timeout;

    http::DownloadTarget target;
    target.path = destination_;
    target.resume = config_.downloads.resume;
    // Always, even though nothing here checks a hash yet: `truncate` sends no
    // `Content-Length` at all, so the caller's own expected size is the only
    // thing that catches a body ending early (fault_proxy.py).
    target.expected_size = static_cast<std::uint64_t>(entry.size_bytes);

    for (int attempt = 1;; ++attempt) {
      entry.state = QueueState::kActive;
      entry.bytes_done = BytesOnCard(destination_);
      entry.message.clear();
      Step step;
      if (Persist(entry, &step) != Written::kOk) {
        // Either the entry was removed while the worker was on it, or the queue
        // could not be written. Neither is a reason to start moving bytes.
        return step;
      }

      const http::Result result = client_.Download(request, target);
      ++requests_;
      std::string why;
      const Verdict verdict = Judge(result, &why);
      if (verdict == Verdict::kOk) {
        entry.bytes_done = BytesOnCard(destination_);
        // M3-3 (#20) puts `kVerifying` and the SHA-1 between here and `kDone`.
        return Settle(std::move(entry), QueueState::kDone, "downloaded");
      }
      if (verdict != Verdict::kCanceled) {
        ++entry.attempts;
      }
      switch (verdict) {
        case Verdict::kFatal:
          return Settle(std::move(entry), QueueState::kFailed, std::move(why));
        case Verdict::kUnauthorized:
          return Requeue(std::move(entry), std::move(why), DrainOutcome::kUnauthorized);
        case Verdict::kCanceled:
          return Requeue(std::move(entry), std::move(why), DrainOutcome::kCanceled);
        default:
          break;
      }
      if (attempt >= attempts_ || Canceled()) {
        return Requeue(std::move(entry), std::move(why), DrainOutcome::kRetryable);
      }
      Wait();
    }
  }

  http::HttpClient& client_;
  fs::FileSystem& filesystem_;
  const config::Config& config_;
  Queue& queue_;
  const WorkerOptions& options_;
  const int attempts_;

  /// The real path the entry in hand is written to, set by `One` and cleared by
  /// it. Empty until a destination has resolved, which is what `Requeue` checks
  /// before asking the card how many bytes of it are already there.
  std::string destination_;

  std::chrono::milliseconds backoff_;
  std::chrono::milliseconds waited_{0};
  int requests_ = 0;
  int downloaded_ = 0;
  int skipped_ = 0;
  int failed_ = 0;
};

}  // namespace

DrainResult Drain(http::HttpClient& client, fs::FileSystem& filesystem,
                  const config::Config& config, Queue& queue, const WorkerOptions& options) {
  Drainer drainer(client, filesystem, config, queue, options);
  return drainer.Run();
}

}  // namespace rommsync::download
