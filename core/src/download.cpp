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
#include "rommsync/md5.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/sha1.hpp"

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
  // Both ends of the bound, because `ParseEntry` refuses the ceiling too: an
  // entry that crossed it in memory would serialise and commit fine and then
  // cost the *next* boot the whole file. `Drainer` saturates rather than
  // climbing past it, so reaching this is a bug rather than a long uptime.
  if (entry.attempts < 0 || entry.attempts > kMaxAttemptsRecorded) {
    *why = "attempts is outside the range an entry can claim";
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

bool ParseEntry(const json::Value& object, QueueEntry* out, std::string* why) {
  QueueEntry entry;
  std::int64_t attempts = 0;

  // The numbers go through `json::Reader`, the same strict reader every server
  // response uses -- it already refuses a fraction, an exponent and anything
  // outside an `int64_t`, and it says which field. Only the strings are
  // hand-read, for the reason `ReadString` gives. `Reader`'s constructor is also
  // what catches an entry that is not an object at all.
  json::Reader numbers(object, "queue entry");
  numbers.Required("rom_id", &entry.rom_id);
  numbers.Required("size_bytes", &entry.size_bytes);
  numbers.Required("bytes_done", &entry.bytes_done);
  numbers.Required("attempts", &attempts);
  numbers.Required("queued_at", &entry.queued_at);
  if (!numbers.ok()) {
    *why = numbers.error().Describe();
    return false;
  }

  std::string state;
  const bool read = ReadString(object, "platform_fs_slug", &entry.platform_fs_slug, why) &&
                    ReadString(object, "fs_name", &entry.fs_name, why) &&
                    ReadString(object, "sha1_hash", &entry.sha1_hash, why) &&
                    ReadString(object, "destination", &entry.destination, why) &&
                    ReadString(object, "state", &state, why) &&
                    ReadString(object, "message", &entry.message, why);
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
/// already on the card, which is what `bytes_done` means.
///
/// `std::ftell` answers a `long`, so this can only measure a file up to
/// `LONG_MAX`. Both targets are LP64 -- devkitA64 and the host rig alike -- so
/// that is 8 EiB and not a bound anything reaches; there is no standard 64-bit
/// seek `core/` is allowed to reach for, `fseeko` being POSIX. A platform where
/// `long` is 32 bits would report `0` for a rom over 2 GiB, which shows as a
/// progress bar that never moves rather than as a corrupt download.
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

/// Bytes of an *unfinished* transfer on the card: the in-flight file's size, or
/// zero when there is none.
///
/// `staging` is what the worker gave `http::DownloadTarget::path` --
/// `<destination>.tmp`, not the destination -- so the file this measures is
/// `<destination>.tmp.part`. Deliberately not "the destination if it is there":
/// a rom queued again over one already on the card would otherwise start at 100%
/// and stay there for the whole download (#22). The finished file is what the
/// transfer is about to replace, not progress towards it.
///
/// The in-flight name comes from `http::PartialPathFor` rather than from a
/// second spelling of `.part` here -- it is the backend's file, and a `core/`
/// copy of the suffix would be a platform detail past the interface
/// (core/AGENTS.md).
std::int64_t PartialBytes(const std::string& staging) {
  return FileSizeBytes(http::PartialPathFor(staging));
}

/// Remove the in-flight file under `staging`, if there is one.
///
/// Only ever called when the entry is finished with for good: a retry and a
/// cancel both *want* those bytes, and discarding them would turn a resumable
/// transfer into a restart. A `kFailed` entry wants nothing -- and a 120 MiB
/// orphan beside a rom that never arrived is bytes the card does not get back
/// until a human finds it, on a console with no file manager.
void DiscardPartial(const std::string& staging) {
  std::remove(http::PartialPathFor(staging).c_str());
}

/// ASCII lowercase, for comparing a digest this client computed against one the
/// server recorded.
///
/// The comparison is of hex *text*, and nothing guarantees which case a library
/// wrote: RomM 5.2.0 stores lowercase, and a client that only ever matched
/// lowercase would refuse every rom of a library that did not -- which looks
/// exactly like a corrupt download and is not one.
std::string LowerHex(std::string text) {
  for (char& character : text) {
    if (character >= 'A' && character <= 'F') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return text;
}

/// What checking a file against the rom's digests came to.
struct Verification {
  /// The file is not this rom. The one value that must never reach a
  /// destination, and the only one that fails an entry.
  bool mismatched = false;

  /// A digest was computed and it matched. False for both of the honest
  /// "nothing checked this" answers -- `verify_hash` off, and a library with no
  /// hash at all -- which are not failures and must not be reported as checks.
  bool verified = false;

  /// For `QueueEntry::message`, so it is held to that field's bar: a sentence, no
  /// `fs_name`, and no digest either. A digest in the queue file would put 40
  /// characters of noise in an overlay row and say nothing a user can act on.
  std::string message;
};

/// True when there is anything to check a downloaded file against.
///
/// The same question in three places -- whether a rom already on the card can be
/// recognised, whether a staged body left by a power cut can be salvaged, and
/// whether a mismatch means anything -- so it is asked once.
bool Checkable(const RomDetail& detail, bool verify_hash) {
  return verify_hash && (!detail.sha1_hash.empty() || !detail.md5_hash.empty());
}

/// Hash `path` and compare it with what RomM recorded for the rom.
///
/// SHA-1 when the library has one, MD5 when it has only that
/// (docs/API_CONTRACT.md#resume--integrity). Streamed, because this runs over a
/// rom: `crypto::Sha1FileHex` reads it in 4 KiB chunks and never holds it.
Verification Check(const std::string& path, const RomDetail& detail, bool verify_hash) {
  Verification outcome;
  if (!verify_hash) {
    outcome.message = "downloaded; [downloads] verify_hash is off, so nothing checked the bytes";
    return outcome;
  }

  const bool by_sha1 = !detail.sha1_hash.empty();
  const bool by_md5 = !by_sha1 && !detail.md5_hash.empty();
  if (!by_sha1 && !by_md5) {
    // An unscanned library leaves both null. Recorded rather than passed off as
    // a check: "we could not tell" and "we checked" are different things to say
    // to someone whose rom will not boot.
    outcome.message =
        "downloaded; the server's library records no hash for this rom, so nothing checked it";
    return outcome;
  }

  const std::string digest = by_sha1 ? crypto::Sha1FileHex(path) : crypto::Md5FileHex(path);
  if (digest.empty()) {
    // The bytes are on the card and cannot be read back. Treated as a mismatch
    // rather than as "not verified": a file that will not open is not one to
    // leave at a destination under a rom's name.
    outcome.mismatched = true;
    outcome.message = "the downloaded bytes could not be read back to check them";
    return outcome;
  }
  if (digest != LowerHex(by_sha1 ? detail.sha1_hash : detail.md5_hash)) {
    outcome.mismatched = true;
    outcome.message = by_sha1 ? "the downloaded bytes are not the SHA-1 the server recorded"
                              : "the downloaded bytes are not the MD5 the server recorded";
    return outcome;
  }

  outcome.verified = true;
  outcome.message = by_sha1 ? "downloaded and verified against the server's SHA-1"
                            : "downloaded and verified against the server's MD5";
  return outcome;
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
    // The file is full. A finished row is history the queue screen shows and
    // nothing prunes, so the oldest of those makes way; only work still to do
    // fills the queue in the sense `kQueueFull` names. See the header.
    const auto oldest = std::find_if(entries_.begin(), entries_.end(),
                                     [](const QueueEntry& row) { return Terminal(row.state); });
    if (oldest == entries_.end()) {
      return ipc::Error::kQueueFull;
    }
    entries_.erase(oldest);
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
  if (rom_id == last_finished_rom_id_) {
    last_finished_rom_id_ = 0;
  }
  if (rom_id == live_rom_id_) {
    live_rom_id_ = 0;
    live_bytes_per_second_ = 0;
  }
  return ipc::Error::kOk;
}

void Queue::Clear() {
  std::lock_guard<std::mutex> held(mutex_);
  entries_.clear();
  last_finished_rom_id_ = 0;
  live_rom_id_ = 0;
  live_bytes_per_second_ = 0;
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
  // These entries are not the ones the rate was measured over, nor the ones
  // anything finished -- this is a load, or the rollback of a write that failed.
  last_finished_rom_id_ = 0;
  live_rom_id_ = 0;
  live_bytes_per_second_ = 0;
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

DownloadStatus Queue::Status() const {
  const auto describe = [](DownloadStatus* status, const QueueEntry& entry,
                           ipc::DownloadState state) {
    status->state = state;
    status->rom_id = entry.rom_id;
    status->fs_name = entry.fs_name;
    status->platform_fs_slug = entry.platform_fs_slug;
    status->bytes_done = entry.bytes_done;
    status->bytes_total = entry.size_bytes;
    status->attempts = entry.attempts;
    status->message = entry.message;
  };

  DownloadStatus status;
  std::lock_guard<std::mutex> held(mutex_);
  status.queue_message = queue_message_;

  const QueueEntry* current = nullptr;
  const QueueEntry* waiting = nullptr;
  const QueueEntry* last_finished = nullptr;
  for (const QueueEntry& entry : entries_) {
    if (Terminal(entry.state)) {
      // Which row is the *last* one to have finished is not a question queue
      // order answers. A retryable failure leaves its entry where it is and
      // `Drain` carries on to the next rom, so a rom that was set aside and then
      // failed for good sits in front of one that finished cleanly before it.
      // `last_finished_rom_id_` is recorded as the transition happens instead.
      if (entry.rom_id == last_finished_rom_id_) {
        last_finished = &entry;
      }
      continue;
    }
    ++status.queue_depth;
    // What this entry still has to move. An unresolved one has `size_bytes ==
    // 0` and contributes nothing -- nothing has said how big it is yet -- and a
    // `bytes_done` past the size a server under-declared contributes nothing
    // either, rather than a negative that would eat another entry's bytes.
    if (entry.size_bytes > entry.bytes_done) {
      status.queue_bytes_remaining += entry.size_bytes - entry.bytes_done;
    }
    if (current == nullptr &&
        (entry.state == QueueState::kActive || entry.state == QueueState::kVerifying)) {
      current = &entry;
    }
    if (waiting == nullptr && entry.state == QueueState::kQueued) {
      waiting = &entry;
    }
  }

  if (current != nullptr) {
    describe(&status, *current,
             current->state == QueueState::kActive ? ipc::DownloadState::kDownloading
                                                   : ipc::DownloadState::kVerifying);
    // Only over bytes that are moving. A rate left standing on a `kVerifying`
    // entry would be a figure for a transfer that stopped, and one keyed on
    // another rom is a number drawn against the wrong bar.
    if (status.state == ipc::DownloadState::kDownloading && live_rom_id_ == current->rom_id) {
      status.bytes_per_second = live_bytes_per_second_;
    }
    return status;
  }
  if (waiting != nullptr) {
    // A queue with something waiting and nothing moving is `kQueued`, not
    // `kIdle`: the status screen has to tell "nothing to do" from "about to
    // start", and `kIdle` on a queue three deep reads as a worker that stopped.
    describe(&status, *waiting, ipc::DownloadState::kQueued);
    return status;
  }
  // Nothing left to do. `kFailed` is #22's call and this is where it is made:
  // the last entry to finish is what the screen is still about, and a queue
  // whose last act was a failure must not read "None" -- that is a user left
  // with a rom that never arrived and a screen saying nothing happened. A
  // `kDone` or `kSkipped` tail is `kIdle`: the work is over and it went as
  // asked, which is the sentence "None" already carries.
  if (last_finished != nullptr && last_finished->state == QueueState::kFailed) {
    describe(&status, *last_finished, ipc::DownloadState::kFailed);
  }
  // A boot that has finished nothing reports `kIdle` even over a `failed` row
  // the card kept, and that is the honest answer: this screen is about what the
  // engine is doing, and nothing has happened yet. The row itself is not lost --
  // it stays in `queue.json` for the queue screen (#31), which is where a user
  // reads why a rom never arrived.

  return status;
}

ipc::DownloadSnapshot Queue::CurrentDownload() const {
  const DownloadStatus status = Status();
  ipc::DownloadSnapshot snapshot;
  snapshot.state = status.state;
  snapshot.rom_id = status.rom_id;
  snapshot.fs_name = status.fs_name;
  snapshot.bytes_done = status.bytes_done;
  snapshot.bytes_total = status.bytes_total;
  return snapshot;
}

void Queue::ReportProgress(std::int64_t rom_id, std::int64_t bytes_done,
                           std::int64_t bytes_per_second) {
  std::lock_guard<std::mutex> held(mutex_);
  const auto found = FindLocked(rom_id);
  // Not `kActive` means the transfer this callback belongs to is over -- or the
  // user dequeued the rom under it. Neither is an error and neither may write:
  // progress onto an entry that has moved on to `kVerifying` would take the bar
  // back off 100%, and onto a row that is gone would resurrect it.
  if (found == entries_.end() || found->state != QueueState::kActive) {
    return;
  }
  found->bytes_done = bytes_done;
  live_rom_id_ = rom_id;
  live_bytes_per_second_ = bytes_per_second;
}

void Queue::set_queue_message(std::string message) {
  std::lock_guard<std::mutex> held(mutex_);
  queue_message_ = std::move(message);
}

QueueEntry Queue::NextPending(const std::vector<std::int64_t>& skip) const {
  std::lock_guard<std::mutex> held(mutex_);
  for (const QueueEntry& entry : entries_) {
    if (Terminal(entry.state)) {
      continue;
    }
    if (std::find(skip.begin(), skip.end(), entry.rom_id) == skip.end()) {
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
  if (Terminal(entry.state)) {
    // The moment a row finishes, which is the only reliable order there is --
    // see `Status`.
    last_finished_rom_id_ = entry.rom_id;
  }
  // A row is written back at state transitions, and every one of those ends the
  // window the rate was measured over -- the entry stops, or starts again from a
  // `.part` this transfer has not touched yet. Leaving the figure standing would
  // quote the *previous* attempt's rate over a second one that has not moved a
  // byte, which is the one thing `bytes_per_second` promises not to do.
  if (entry.rom_id == live_rom_id_) {
    live_rom_id_ = 0;
    live_bytes_per_second_ = 0;
  }
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
    case StoreError::kUnusableEntry:
      return "unusable_entry";
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
      // The test is whether `.old` *parsed*, not whether it held anything: a
      // drained queue is legitimately written as `entries: []`, and calling
      // that "no usable queue" would log the one state the module calls a
      // perfectly good queue as corruption.
      std::vector<std::string> diagnostics{Describe(
          path, recovered.diagnostics.empty()
                    ? "is missing; the queue was recovered from " + previous_path +
                          " after an interrupted write"
                    : "is missing and " + previous_path +
                          " holds no usable queue either -- a write of it was interrupted")};
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
      // The one outcome that is not a discard: the queue on the card is
      // probably intact and this boot simply cannot see it. See `trusted`.
      loaded.trusted = false;
      Add(&loaded.diagnostics,
          Describe(path, "could not be read; the queue is empty for this boot, and must not be "
                         "written over until it can be read again"));
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
      result.error = StoreError::kUnusableEntry;
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

  // The two digests are `string | null` and the only fields here allowed to have
  // no value: an unscanned library leaves both null, and a download with nothing
  // to check against is recorded as unverified rather than quietly passed
  // (download.hpp). A *missing* key is still a shape error -- a body without one
  // is not the schema this was written against, and reading that as "this rom
  // has no hash" would turn a client pointed at the wrong server into a library
  // of roms nothing checked.
  std::optional<std::string> sha1;
  std::optional<std::string> md5;
  json::Reader hashes(document.value, "rom detail");
  if (!hashes.RequiredNullable("sha1_hash", &sha1)) {
    return hashes.error();
  }
  if (!hashes.RequiredNullable("md5_hash", &md5)) {
    return hashes.error();
  }
  if (sha1.has_value()) {
    detail.sha1_hash = *sha1;
  }
  if (md5.has_value()) {
    detail.md5_hash = *md5;
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
    case DrainOutcome::kForbidden:
      return "forbidden";
    case DrainOutcome::kStoreFailed:
      return "store_failed";
  }
  return "idle";
}

auth::Answer AnswerOf(DrainOutcome outcome) {
  switch (outcome) {
    case DrainOutcome::kUnauthorized:
      return auth::Answer::kRejected;
    case DrainOutcome::kForbidden:
      return auth::Answer::kForbidden;
    // The one acceptance: every pending entry reached a terminal state, which
    // took a rom body out of RomM with this token (auth_gate.hpp states the
    // rule).
    case DrainOutcome::kCompleted:
      return auth::Answer::kAccepted;
    // The rest are not evidence either way -- a cancel can even come before the
    // first request.
    case DrainOutcome::kRetryable:
    case DrainOutcome::kCanceled:
    case DrainOutcome::kIdle:
    case DrainOutcome::kDisabled:
    case DrainOutcome::kStoreFailed:
      break;
  }
  return auth::Answer::kSilent;
}

namespace {
/// What one exchange with RomM means for the entry that made it.
enum class Verdict {
  kOk,
  kRetry,         ///< nothing about this says the request was wrong; try it again
  kFatal,         ///< this rom will not download as asked, however often it is tried
  kUnauthorized,  ///< the token, not the rom. Every other entry would fail the same way.
  kForbidden,     ///< the same, and not a revocation: a scope the pairing lacks.
  kCanceled,
};

/// What one exchange leaves on the entry, and what it leaves for the log.
///
/// Two strings rather than one because they go to different places and are held
/// to different bars. `entry` is copied into `QueueEntry::message`, which the
/// queue file carries and the overlay renders, and which **never quotes
/// `fs_name`** -- and `http::Result::message` does: a `kWriteFailed` from the
/// libcurl backend is `"could not open <destination>.part: ..."`, destination
/// and rom name included. `log` is the fuller line, for `DrainResult::message`.
struct Reason {
  std::string entry;
  std::string log;
};

/// Classify a completed exchange.
Verdict Judge(const http::Result& result, Reason* why) {
  const auto say = [&result, why](std::string sentence) {
    why->entry = std::move(sentence);
    why->log = why->entry;
    if (!result.message.empty()) {
      why->log += " (" + result.message + ")";
    }
  };

  if (!result.ok()) {
    switch (result.error) {
      case http::Error::kCanceled:
        say("the download was stopped");
        return Verdict::kCanceled;
      case http::Error::kInvalidRequest:
      case http::Error::kWriteFailed:
        // Neither gets better by trying again: one is this client building a
        // request wrong, the other is the card refusing the write -- a mapped
        // folder that does not exist is the ordinary way to reach it.
        say(std::string("the transfer could not be made: ") + http::ToString(result.error));
        return Verdict::kFatal;
      default:
        say(std::string("the transfer did not complete: ") + http::ToString(result.error));
        return Verdict::kRetry;
    }
  }

  const int status = result.response.status;
  if (status >= 200 && status < 300) {
    return Verdict::kOk;
  }
  if (status == 401) {
    // Not this entry's problem, and a retried 401 is a 401 retried forever
    // (sync.hpp). The drain stops rather than spending the whole queue on it.
    say("the server no longer accepts this console's token");
    return Verdict::kUnauthorized;
  }
  if (status == 403) {
    // The same stop, a different sentence and a different outcome: RomM approves
    // what the user ticked, so this is a scope missing from a pairing that
    // otherwise works and not a token that was revoked
    // (docs/AUTH.md#scopes).
    say("this console's token was not granted what a download needs");
    return Verdict::kForbidden;
  }
  if (status == 404) {
    say("the server has no such rom any more");
    return Verdict::kFatal;
  }
  if (status == 408 || status == 429 || status >= 500) {
    // A proxy and a rate limiter answer with the first two; a 5xx is the server
    // having a bad minute. All three are worth another request.
    say("the server answered " + std::to_string(status));
    return Verdict::kRetry;
  }
  say("the server refused the request with " + std::to_string(status));
  return Verdict::kFatal;
}

/// The drain outcome a verdict ends the entry's turn with. One mapping rather
/// than one per request loop, so the two cannot drift apart.
DrainOutcome OutcomeFor(Verdict verdict) {
  switch (verdict) {
    case Verdict::kRetry:
      return DrainOutcome::kRetryable;
    case Verdict::kUnauthorized:
      return DrainOutcome::kUnauthorized;
    case Verdict::kForbidden:
      return DrainOutcome::kForbidden;
    case Verdict::kCanceled:
      return DrainOutcome::kCanceled;
    case Verdict::kOk:
    case Verdict::kFatal:
      break;
  }
  return DrainOutcome::kCompleted;
}

http::Request Authed(const WorkerOptions& options, std::string url) {
  http::Request request;
  request.method = http::Method::kGet;
  request.url = std::move(url);
  request.headers.push_back({"Authorization", "Bearer " + options.bearer_token});
  request.cancel = options.cancel;
  return request;
}

/// Turns the backend's progress callbacks into a bounded stream of queue
/// updates, and measures a rate on the way.
///
/// One per transfer, on the worker's stack, borrowed by
/// `http::DownloadTarget::progress` for exactly that scope. Every method here
/// runs on the **transfer thread** (http.hpp), so it does the least a shared
/// mutex allows and allocates nothing.
///
/// It is not the place `bytes_done` becomes durable: `queue.json` is written at
/// state transitions only, because SD writes are not free.
///
/// It does no arithmetic on what the backend reports, either.
/// `http::ProgressCallback`'s `staged` is already the whole in-flight file, the
/// bytes an earlier attempt left in it included -- adding a starting point here
/// would be this side guessing about a file the backend is the one rewriting,
/// and it would be wrong in exactly the case the backend handles: a `resume` the
/// server answers 200 to, where the prefix is discarded and `staged` drops to
/// zero with it.
class Publisher {
 public:
  using Clock = std::chrono::steady_clock;

  Publisher(Queue& queue, std::int64_t rom_id, std::int64_t started_at,
            std::chrono::milliseconds interval)
      : queue_(queue),
        rom_id_(rom_id),
        interval_(interval),
        published_(started_at),
        since_(Clock::now()) {}

  void Report(std::uint64_t staged) {
    const Clock::time_point now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - since_);
    if (elapsed < interval_) {
      return;
    }
    const std::int64_t done = static_cast<std::int64_t>(staged);
    // Instantaneous, over the window that just closed. Smoothing is a decision
    // for whoever draws it: this is a label on a live transfer, and a constant
    // chosen here would be one every screen inherited. Nothing is quoted for a
    // window the file did not grow over, which is also what a discarded prefix
    // produces.
    std::int64_t rate = 0;
    if (elapsed.count() > 0 && done > published_) {
      rate = (done - published_) * 1000 / elapsed.count();
    }
    published_ = done;
    since_ = now;
    queue_.ReportProgress(rom_id_, done, rate);
  }

 private:
  Queue& queue_;
  const std::int64_t rom_id_;
  const std::chrono::milliseconds interval_;

  /// The last figure published, and when. Seeded with what the entry started
  /// this transfer holding, so the first window measures the bytes the transfer
  /// moved and not the `.part` it inherited.
  std::int64_t published_;
  Clock::time_point since_;
};

/// What a write of the queue did.
enum class Written {
  kOk,
  kGone,    ///< there is no such entry any more -- `Remove` ran while the worker was on it
  kFailed,  ///< `queue.json` could not be written
};

/// The state one entry's turn left the drain in.
///
/// `kCompleted` means "this entry is finished with, carry on"; every other value
/// is something the caller of `Drain` has to hear about.
struct Step {
  DrainOutcome outcome = DrainOutcome::kCompleted;
  StoreResult store;
  std::string message;
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
        budget_(options.max_attempts > 0 ? options.max_attempts : 1),
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
    DrainOutcome stopped = DrainOutcome::kCompleted;
    for (;;) {
      if (Canceled()) {
        stopped = DrainOutcome::kCanceled;
        result.message = "the drain was stopped";
        break;
      }
      const QueueEntry entry = queue_.NextPending(deferred_);
      if (entry.rom_id == 0) {
        break;
      }
      worked = true;
      const Step step = One(entry);
      if (step.outcome == DrainOutcome::kRetryable) {
        // Set aside, not dropped. One rom whose endpoint answers 500 every time
        // must not starve the rest of the queue, and it is still the user's
        // download -- so it stays `kQueued` and the next entry gets a turn.
        deferred_.push_back(entry.rom_id);
        deferred_message_ = step.message;
        continue;
      }
      if (step.outcome != DrainOutcome::kCompleted) {
        stopped = step.outcome;
        result.store = step.store;
        result.message = step.message;
        break;
      }
    }

    if (stopped != DrainOutcome::kCompleted) {
      result.outcome = stopped;
    } else if (!deferred_.empty()) {
      result.outcome = DrainOutcome::kRetryable;
      result.message = deferred_message_;
    } else {
      result.outcome = worked ? DrainOutcome::kCompleted : DrainOutcome::kIdle;
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

  /// One more failed request against the entry in hand, saturating rather than
  /// climbing past what the file may claim: an `attempts` over
  /// `kMaxAttemptsRecorded` writes fine and costs the *next* boot the whole
  /// queue, which is the silent loss the two bounds exist to prevent.
  static void CountAttempt(QueueEntry* entry) {
    if (entry->attempts < static_cast<int>(kMaxAttemptsRecorded)) {
      ++entry->attempts;
    }
  }

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

  /// Spend one of the entry's requests, and say whether it had one to spend.
  ///
  /// The budget is the *entry's*, shared by the detail call and the transfer:
  /// a budget each would let one entry issue twice `max_attempts` requests and
  /// wait twice the backoff in a single drain.
  bool Spend() { return ++spent_ < budget_; }

  /// Write the queue down. **Every state transition goes through this**, so the
  /// file and memory never disagree by more than one `rename` -- which is what
  /// makes a power cut resumable rather than a restart.
  Written Persist(const QueueEntry& entry, Step* step) {
    // Taken before the change, so a write that fails can be undone exactly --
    // `SdEngine::Commit`'s scheme, and here for two reasons beyond tidiness.
    // The invariant this module claims is that the file and memory never
    // disagree by more than one `rename`: memory left a transition ahead would
    // have an entry `kDone` here and `kActive` on the card, so the rom is not
    // retried this boot and *is* re-downloaded after a reboot. And a row the
    // writer refuses (`kUnusableEntry`) would otherwise stay in memory and make
    // every later write fail the same way, including one for an unrelated rom.
    std::vector<QueueEntry> before = queue_.Snapshot();
    if (!queue_.Update(entry)) {
      return Written::kGone;
    }
    const StoreResult stored = SaveQueue(options_.queue_path, queue_);
    if (stored.ok()) {
      return Written::kOk;
    }
    queue_.Reset(std::move(before));
    step->outcome = DrainOutcome::kStoreFailed;
    step->store = stored;
    step->message = stored.message;
    return Written::kFailed;
  }

  /// Finish `entry` in a terminal state.
  ///
  /// The count follows the write, not the intention: an entry the user removed
  /// mid-transfer has no row left to explain a `failed: 1` with, so it is not
  /// counted.
  Step Settle(QueueEntry entry, QueueState state, std::string why) {
    Step step;
    entry.state = state;
    entry.message = Clip(std::move(why));
    if (Persist(entry, &step) != Written::kOk) {
      return step;
    }
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
    return step;
  }

  /// Leave `entry` queued after a failure that says nothing about the rom, and
  /// end the entry's turn with `outcome`.
  Step Requeue(QueueEntry entry, const Reason& why, DrainOutcome outcome) {
    Step step;
    entry.state = QueueState::kQueued;
    entry.message = Clip(why.entry);
    if (!staging_.empty()) {
      // What a resume will start from. Counting the in-flight file rather than
      // this request's bytes is the difference between a bar that carries on
      // and one that restarts at zero (#22) -- and only when there is going to
      // be a resume, for the reason `Transfer` gives.
      entry.bytes_done = config_.downloads.resume ? PartialBytes(staging_) : 0;
    }
    const Written written = Persist(entry, &step);
    if (written == Written::kFailed) {
      return step;  // `kStoreFailed`, which ends the drain
    }
    if (written == Written::kGone && outcome == DrainOutcome::kRetryable) {
      // The user dequeued it while the transfer was in flight. There is no entry
      // to set aside and no row to explain a deferral with, so this turn is
      // simply over -- and the drain carries on to the next rom rather than
      // reporting a failure against an entry that does not exist. The outcomes
      // that are true of *every* entry still end it, below.
      return step;
    }
    step.outcome = outcome;
    step.message = why.log;
    return step;
  }

  Step One(const QueueEntry& queued) {
    QueueEntry entry = queued;
    // Nothing carries over from the last entry: `Requeue` asks the card how many
    // bytes a *resolved* destination holds, and a stale one would answer about
    // another rom's file. The budget is per entry for the same reason.
    destination_.clear();
    staging_.clear();
    spent_ = 0;
    backoff_ = options_.backoff;

    // 1. Who is this rom? `Enqueue` recorded an id and nothing else, because no
    // IPC command may block on the network (ipc.hpp).
    RomDetail detail;
    Reason why;
    const Verdict resolved = FetchDetail(&entry, &detail, &why);
    if (resolved == Verdict::kFatal) {
      return Settle(std::move(entry), QueueState::kFailed, why.entry);
    }
    if (resolved != Verdict::kOk) {
      return Requeue(std::move(entry), why, OutcomeFor(resolved));
    }

    if (detail.id != entry.rom_id) {
      // The id is read back and checked rather than ignored. RomM has one
      // download path where the id in the URL and the bytes served can disagree
      // (docs/API_CONTRACT.md#multi-file-roms), so a body describing a different
      // rom is worth refusing here rather than writing its bytes under this
      // entry's name.
      return Settle(std::move(entry), QueueState::kFailed,
                    "the server answered with a different rom than the one that was asked for");
    }

    // The strings on a `RomDetail` came off someone else's filesystem, and they
    // are about to become fields in a file this client has to be able to write
    // again. `Usable` is the bar `SaveQueue` holds them to, so it is asked here
    // -- on a copy -- rather than after: an entry recorded and *then* found
    // unwritable would make every later write of the queue fail the same way,
    // for every other rom, until the console rebooted.
    QueueEntry described = entry;
    described.platform_fs_slug = detail.platform_fs_slug;
    described.fs_name = detail.fs_name;
    described.size_bytes = detail.size_bytes;
    described.sha1_hash = detail.sha1_hash;
    std::string unwritable;
    if (!Usable(described, &unwritable)) {
      // `entry`, not `described`: the whole point is that the long fields do
      // not reach the file. The reason names the field and never its value, the
      // way `config::RomDestination::reason` does.
      return Settle(std::move(entry), QueueState::kSkipped,
                    "this rom cannot be recorded in the queue -- " + unwritable);
    }
    entry = std::move(described);

    // 2. The two answers that are not a download. Both are refusals with a
    // sentence rather than a transfer that fails later: a disc set served over
    // `content` is a zip with no length at all (docs/API_CONTRACT.md), and a rom
    // missing from the server's own filesystem 404s part way in.
    //
    // The disc-set test is `has_multiple_files` and nothing else. A rom whose
    // `fs_name` is a directory on the server is not by itself one -- a nested
    // single-file rom is a directory too, and downloads normally
    // (rom_index.hpp, `download.nested`).
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
    // Where the bytes land while nothing has checked them yet. The destination
    // is written by `Verify` and by nothing else (download.hpp).
    staging_ = io::TempPathFor(destination_);

    // 4. Is it already on the card? Every folder the platform maps, not only the
    // write target: the later `roms` entries are exactly the folders where
    // someone already keeps that platform's roms (config.hpp), and a check that
    // skipped them re-downloads a rom the card has.
    //
    // Answered with the digest or not at all. Without one there is no way to
    // tell "already there and correct" from "already there and truncated", and
    // skipping the second is how a user is left with a broken rom and a queue
    // that says done -- so a library with no hash, or `verify_hash = false`,
    // downloads it again rather than assuming.
    const std::optional<Step> present = AlreadyOnTheCard(&entry, detail);
    if (present.has_value()) {
      return *present;
    }

    // 5. The bytes.
    return Transfer(std::move(entry), detail);
  }

  /// Nothing when the rom is not already on the card, so the caller carries on.
  ///
  /// The size is checked first because it is free and a digest is not: hashing
  /// every candidate path would read a rom off the card per drain, and a file of
  /// the wrong length is already a no.
  std::optional<Step> AlreadyOnTheCard(QueueEntry* entry, const RomDetail& detail) {
    if (!Checkable(detail, config_.downloads.verify_hash) || detail.size_bytes <= 0) {
      return std::nullopt;
    }

    for (const std::string& candidate :
         config_.ExistingRomPaths({detail.platform_fs_slug, detail.fs_name})) {
      const std::string real = filesystem_.Resolve(candidate);
      if (real.empty() || FileSizeBytes(real) != detail.size_bytes) {
        continue;
      }
      if (!Check(real, detail, config_.downloads.verify_hash).verified) {
        continue;
      }
      // An earlier transfer's leftovers, which nothing else will ever clear: the
      // skip returns before `Transfer`, which is what usually tidies them, and a
      // 120 MiB orphan beside a rom that is already there is space the card does
      // not get back on a console with no file manager.
      std::remove(staging_.c_str());
      DiscardPartial(staging_);
      // Where it actually is, which is not always where it would have been
      // written: a rom found in the platform's second `roms` folder belongs in
      // the entry as that path, so the queue screen names the file a user can
      // go and look at.
      entry->destination = candidate;
      entry->bytes_done = detail.size_bytes;
      return Settle(std::move(*entry), QueueState::kDone,
                    "already on the card, with the bytes the server's hash describes");
    }
    return std::nullopt;
  }

  Verdict FetchDetail(QueueEntry* entry, RomDetail* out, Reason* why) {
    http::Request request =
        Authed(options_, options_.base_url + "/api/roms/" + std::to_string(entry->rom_id));
    request.headers.push_back({"Accept", "application/json"});
    request.timeout = options_.detail_timeout;

    for (;;) {
      const http::Result result = client_.Send(request);
      ++requests_;
      const bool more = Spend();
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
        why->entry = "the rom could not be read: " + shape.Describe();
        why->log = why->entry;
        verdict = Verdict::kFatal;
      }
      if (verdict != Verdict::kCanceled) {
        CountAttempt(entry);
      }
      if (Canceled()) {
        // Checked before the budget, so a stop that lands while a request was
        // failing for some other reason is still reported as a stop rather than
        // as a network failure the caller might retry.
        why->entry = "the download was stopped";
        why->log = why->entry;
        return Verdict::kCanceled;
      }
      if (verdict != Verdict::kRetry || !more) {
        return verdict;
      }
      Wait();
    }
  }

  Step Transfer(QueueEntry entry, const RomDetail& detail) {
    http::Request request =
        Authed(options_, ContentUrl(options_.base_url, entry.rom_id, entry.fs_name));
    // No total ceiling. A 120 MiB rom on hotel Wi-Fi is slow rather than dead,
    // and `stall_timeout` is the timeout that tells those two apart (http.hpp).
    request.timeout = std::chrono::milliseconds{0};
    request.stall_timeout = options_.stall_timeout;

    http::DownloadTarget target;
    // The staging file, **not** the rom's destination. The backend renames onto
    // `target.path` the moment the body completes, which is before anything has
    // looked at the bytes -- so the destination is not a path it may be given
    // (download.hpp).
    target.path = staging_;
    // A resume with nothing to check the seam against is a documented refusal
    // (http.hpp), and a rom whose size the server does not know is exactly that
    // case: `expected_size` would be zero. Asked as a `resume = false` here
    // rather than as a failed request there -- the transfer still works, it just
    // starts from the beginning.
    target.resume = config_.downloads.resume && entry.size_bytes > 0;
    // Always. `truncate` sends no `Content-Length` at all, so the caller's own
    // expected size is the only thing that catches a body ending early
    // (fault_proxy.py), and it is what makes `Error::kTruncated` reachable.
    target.expected_size = static_cast<std::uint64_t>(entry.size_bytes);

    // An in-flight file longer than the rom cannot be a prefix of it: it is
    // another rom's, or another release's. Resuming from it asks for a range
    // past the end of the resource, which costs a request to find out -- the
    // backend turns the 416 into `kTruncated` and starts over. A *shorter* wrong
    // prefix cannot be told apart by length, and is not meant to be: it is
    // spliced, the digest catches it, and the retry below fetches it clean.
    const std::string in_flight = http::PartialPathFor(staging_);
    if (target.resume && entry.size_bytes > 0 && FileSizeBytes(in_flight) > entry.size_bytes) {
      std::remove(in_flight.c_str());
    }

    // A staging file is a *complete* body nothing verified -- left by a power cut
    // between the backend's rename and this worker's hash. Its name is derived
    // from this rom's own destination, so the digest can say whether those bytes
    // are the rom, which is a great deal cheaper than fetching 120 MiB again. If
    // they are not, `Verify` discards them and the loop below starts clean.
    //
    // Only when there is a digest to ask: with nothing to check against, a body
    // an interruption left behind is not something to commit under a rom's name.
    if (Checkable(detail, config_.downloads.verify_hash) && entry.size_bytes > 0 &&
        FileSizeBytes(staging_) == entry.size_bytes) {
      const std::optional<Step> salvaged = Verify(&entry, detail, /*inherited=*/true);
      if (salvaged.has_value()) {
        return *salvaged;
      }
    }
    // Whatever is left is bytes nothing will build on -- and it is also a
    // destination the backend's own rename would have to replace, which
    // Horizon's refuses (atomic_file.hpp).
    std::remove(staging_.c_str());

    for (;;) {
      // Bytes this attempt did not fetch. A body built on them and then failing
      // its digest says nothing about the server, so it is worth one clean try;
      // a body fetched whole and failing its digest says the server's own hash
      // does not describe its own file, and asking again costs a rom's transfer
      // to learn the same thing.
      const bool inherited = target.resume && PartialBytes(staging_) > 0;

      entry.state = QueueState::kActive;
      // Only when the bytes are going to be built on. With `resume` off the
      // backend opens the in-flight file `"wb"` and truncates it, so counting a
      // leftover `.part` would start the overlay's bar at a stale figure and
      // then have it jump backwards (#22).
      entry.bytes_done = target.resume ? PartialBytes(staging_) : 0;
      entry.message.clear();
      Step step;
      if (Persist(entry, &step) != Written::kOk) {
        // Either the entry was removed while the worker was on it, or the queue
        // could not be written. Neither is a reason to start moving bytes.
        return step;
      }

      // What makes a 120 MiB rom a status screen rather than a frozen one
      // (#22). The sink lives exactly as long as the call it is passed to, which
      // is what `http::DownloadTarget::progress` requires of it, and it counts
      // from the bytes already on the card rather than from zero.
      Publisher publisher(queue_, entry.rom_id, entry.bytes_done, options_.progress_interval);
      target.progress = [&publisher](std::uint64_t staged, std::uint64_t) {
        publisher.Report(staged);
      };

      const http::Result result = client_.Download(request, target);
      // Nothing may hold this sink past the call: `publisher` is about to go out
      // of scope on the next pass round the loop, and `target` outlives it.
      target.progress = nullptr;
      ++requests_;
      const bool more = Spend();
      Reason why;
      const Verdict verdict = Judge(result, &why);
      if (verdict == Verdict::kOk) {
        const std::optional<Step> settled = Verify(&entry, detail, inherited);
        if (settled.has_value()) {
          return *settled;
        }
        // The digest said the bytes this built on were not the rom's. `Verify`
        // has removed both files, so the next pass starts from zero.
        CountAttempt(&entry);
        if (Canceled()) {
          return Requeue(std::move(entry),
                         {"the download was stopped", "the download was stopped"},
                         DrainOutcome::kCanceled);
        }
        if (!more) {
          return Requeue(
              std::move(entry),
              {"the bytes already on the card were not this rom's; it will be fetched again",
               "a resumed transfer failed its hash; the partial bytes were discarded"},
              DrainOutcome::kRetryable);
        }
        Wait();
        continue;
      }
      if (verdict != Verdict::kCanceled) {
        CountAttempt(&entry);
      }
      if (verdict == Verdict::kFatal) {
        // Nothing will ever build on these bytes: this rom does not download as
        // asked, however often it is tried. A retry and a cancel both want the
        // in-flight file kept, which is why only this branch discards it -- and
        // a 120 MiB orphan beside a rom that never arrived is space the card
        // does not get back, on a console with no file manager.
        DiscardPartial(staging_);
        entry.bytes_done = 0;
        return Settle(std::move(entry), QueueState::kFailed, why.entry);
      }
      if (Canceled()) {
        // Before the budget, for the reason `FetchDetail` gives.
        return Requeue(std::move(entry), {"the download was stopped", "the download was stopped"},
                       DrainOutcome::kCanceled);
      }
      if (verdict != Verdict::kRetry || !more) {
        return Requeue(std::move(entry), why, OutcomeFor(verdict));
      }
      Wait();
    }
  }

  /// A complete body is at `staging_`. Nothing has looked at it yet.
  ///
  /// This is the whole of M3-3's guarantee in one function: the destination is
  /// written only after a digest matched, and a mismatch leaves neither a file
  /// there nor bytes to build on.
  ///
  /// `inherited` says whether those bytes include any this turn did not fetch --
  /// a resumed prefix, or a whole body a power cut left staged. It changes only
  /// what a *mismatch* means. Bytes off the card that fail a digest say nothing
  /// about the server, so this returns **nothing**, having discarded them, and
  /// the caller fetches the rom clean. Bytes fetched whole that fail a digest
  /// say the server's own hash does not describe its own file, which asking
  /// again cannot change: that is `kFailed`.
  ///
  /// The hash is not interruptible. `http::CancelToken` is polled by the backend
  /// during a transfer and by `Drain` between entries, so a stop that lands here
  /// waits out one file -- seconds for a 120 MiB rom off a card. Making it
  /// finer would put cancellation into `crypto::StreamFile`, which
  /// `state::HashFile` also uses and has no cancel to offer.
  std::optional<Step> Verify(QueueEntry* entry, const RomDetail& detail, bool inherited) {
    entry->state = QueueState::kVerifying;
    entry->bytes_done = FileSizeBytes(staging_);
    entry->message.clear();
    Step step;
    if (Persist(*entry, &step) != Written::kOk) {
      // The user dequeued it, or the queue could not be written. Either way
      // nothing will record this download, and a complete body under a name no
      // entry mentions is an orphan the console has no file manager to find.
      std::remove(staging_.c_str());
      return step;
    }

    const Verification checked = Check(staging_, detail, config_.downloads.verify_hash);
    if (checked.mismatched) {
      // Both files go. Bytes that are not this rom's are not a prefix of it
      // either, so keeping them for a resume would buy a cheaper way to produce
      // the same wrong file.
      std::remove(staging_.c_str());
      DiscardPartial(staging_);
      entry->bytes_done = 0;
      if (inherited) {
        return std::nullopt;
      }
      return Settle(std::move(*entry), QueueState::kFailed, checked.message);
    }

    // Asked before the commit, because afterwards there is no telling: this is
    // the one failure that can cost a file the card already had, and saying so
    // is the difference between a user looking under `.old` and one wondering
    // where their rom went.
    const bool replacing = io::Exists(destination_);

    // The two renames Horizon needs, and the reason the commit is here rather
    // than in the backend (atomic_file.hpp): `fsFsRenameFile` refuses a
    // destination that already exists, so re-downloading a rom the card holds
    // would fail on a console and pass on every host test.
    const io::WriteResult committed = io::CommitStaged(staging_, destination_);
    if (!committed.ok()) {
      // `CommitStaged` consumes the staged file either way, so there is nothing
      // left to build on. A card that refuses a rename is the class of failure
      // `Judge` already calls fatal for `http::Error::kWriteFailed`: a mapped
      // folder that has gone away does not come back by trying again.
      DiscardPartial(staging_);
      entry->bytes_done = 0;
      // The sentence names no path, because `QueueEntry::message` never carries
      // an `fs_name` and `PreviousPathFor` is built from one. It says which name
      // to look under instead, which is what a user actually needs.
      std::string why = "the rom arrived and was checked, and could not be moved into place";
      if (replacing) {
        why += "; the copy that was already there may be beside it under a .old name";
      }
      return Settle(std::move(*entry), QueueState::kFailed, std::move(why));
    }

    entry->bytes_done = FileSizeBytes(destination_);
    return Settle(std::move(*entry), QueueState::kDone, checked.message);
  }

  http::HttpClient& client_;
  fs::FileSystem& filesystem_;
  const config::Config& config_;
  Queue& queue_;
  const WorkerOptions& options_;

  /// Requests one entry may spend, and how many the entry in hand has spent.
  const int budget_;
  int spent_ = 0;

  /// The real path the entry in hand is written to, set by `One` and cleared by
  /// it. Empty until a destination has resolved.
  std::string destination_;

  /// Where the bytes wait while nothing has checked them: `io::TempPathFor` of
  /// `destination_`, and what `http::DownloadTarget::path` is given. Set and
  /// cleared beside `destination_`, and what `Requeue` checks before asking the
  /// card how many bytes are already there -- a stale one would answer about
  /// another rom's transfer.
  std::string staging_;

  /// The entries this drain set aside, and the last reason it did. They are
  /// still in the queue and still the user's downloads -- see `Run`.
  std::vector<std::int64_t> deferred_;
  std::string deferred_message_;

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
