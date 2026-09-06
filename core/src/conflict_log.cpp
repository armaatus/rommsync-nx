// The conflict history: the entry, the format, and the store.
//
// The recorders and the restore are `conflict_record.cpp` next door -- they
// need the sync engine's report types and nothing here does, which is why the
// module is two files (conflict_log.hpp, "Where the halves live").
//
// See conflict_log.hpp for what this is for. What is worth saying here is what
// the file *is*: a header line carrying the magic, the format version and the
// next id, then one JSON object per entry, newest first, written with
// `io::WriteAtomically`. It is `state.db`'s shape on purpose -- the reader, the
// bounds and the `.old` recovery are all the same problem, and a second format
// under `/config/rommsync` would be a second place to get a truncation wrong.
#include "rommsync/conflict_log.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/config.hpp"
#include "rommsync/json.hpp"
#include "rommsync/text.hpp"

namespace rommsync::conflicts {
namespace {

/// Whole seconds a file can plausibly claim: 2000-01-01 to 2100-01-01.
///
/// `state_db.cpp`'s bound, for its reason -- the check happens before the value
/// is turned into a `Timestamp`, so a corrupt card region cannot overflow the
/// conversion on the way to producing a diagnostic. Zero is allowed here and is
/// not there: an entry whose *local* mtime could not be read is still an entry
/// worth listing, because the backup it points at is real either way.
constexpr std::int64_t kEarliest = 946'684'800;
constexpr std::int64_t kLatest = 4'102'444'800;

bool SecondsUsable(std::int64_t seconds) {
  return seconds == 0 || (seconds >= kEarliest && seconds <= kLatest);
}

std::string FormatHeader(std::int64_t next_id) {
  return std::string(kFormatMagic) + " " + std::to_string(kFormatVersion) + " " +
         std::to_string(next_id);
}

std::string Describe(const std::string& path, std::string_view what) {
  return path + ": " + std::string(what);
}

std::string_view TrimCarriageReturn(std::string_view line) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return line;
}

void AppendKey(std::string* out, std::string_view key, bool first = false) {
  if (!first) {
    *out += ',';
  }
  *out += json::Quote(key);
  *out += ':';
}

void AppendInteger(std::string* out, std::string_view key, std::int64_t value,
                   bool first = false) {
  AppendKey(out, key, first);
  *out += std::to_string(value);
}

void AppendText(std::string* out, std::string_view key, std::string_view value) {
  AppendKey(out, key);
  *out += json::Quote(value);
}

/// `null` rather than `""`, for `state_db.cpp`'s reason: "there was none" is an
/// answer, and a reader that could not tell it from a value of nothing would
/// have to guess which it was looking at.
void AppendOptionalText(std::string* out, std::string_view key, std::string_view value) {
  AppendKey(out, key);
  *out += value.empty() ? std::string("null") : json::Quote(value);
}

void AppendNullableText(std::string* out, std::string_view key,
                        const std::optional<std::string>& value) {
  AppendKey(out, key);
  *out += value.has_value() ? json::Quote(*value) : std::string("null");
}

const char* KindText(EntryKind kind) { return kind == EntryKind::kState ? "state" : "save"; }

bool ReadKind(std::string_view text, EntryKind* out) {
  if (text == "save") {
    *out = EntryKind::kSave;
    return true;
  }
  if (text == "state") {
    *out = EntryKind::kState;
    return true;
  }
  return false;
}

const char* EventText(Event event) {
  switch (event) {
    case Event::kConflict:
      return "conflict";
    case Event::kReplaced:
      return "replaced";
    case Event::kKeptBoth:
      return "kept_both";
  }
  return "conflict";
}

bool ReadEvent(std::string_view text, Event* out) {
  if (text == "conflict") {
    *out = Event::kConflict;
    return true;
  }
  if (text == "replaced") {
    *out = Event::kReplaced;
    return true;
  }
  if (text == "kept_both") {
    *out = Event::kKeptBoth;
    return true;
  }
  return false;
}

}  // namespace

std::string SerializeEntry(const Entry& entry) {
  std::string out("{\"id\":");
  out += std::to_string(entry.id);
  AppendText(&out, "kind", KindText(entry.kind));
  AppendText(&out, "event", EventText(entry.event));
  AppendInteger(&out, "rom_id", entry.rom_id);
  AppendOptionalText(&out, "rom_name", entry.rom_name);
  AppendText(&out, "file_name", entry.file_name);
  AppendNullableText(&out, "slot", entry.slot);
  AppendOptionalText(&out, "emulator", entry.emulator);
  AppendInteger(&out, "when", entry.when);
  AppendOptionalText(&out, "reason", entry.reason);
  AppendText(&out, "sd_path", entry.sd_path);
  AppendInteger(&out, "local_size_bytes", entry.local_size_bytes);
  AppendOptionalText(&out, "local_content_hash", entry.local_content_hash);
  AppendInteger(&out, "local_modified", entry.local_modified);
  AppendNullableText(&out, "server_content_hash", entry.server_content_hash);
  AppendOptionalText(&out, "server_updated_at", entry.server_updated_at);
  AppendInteger(&out, "server_size_bytes", entry.server_size_bytes);
  AppendOptionalText(&out, "backup_sd_path", entry.backup_sd_path);
  out += "}";
  return out;
}

bool ParseEntry(const json::Value& object, Entry* out, std::string* why) {
  Entry entry;
  std::string kind;
  std::string event;
  std::optional<std::string> rom_name;
  std::optional<std::string> emulator;
  std::optional<std::string> reason;
  std::optional<std::string> local_hash;
  std::optional<std::string> server_updated_at;
  std::optional<std::string> backup;

  json::Reader reader(object, "conflict entry");
  reader.Required("id", &entry.id);
  reader.Required("kind", &kind);
  reader.Required("event", &event);
  reader.Required("rom_id", &entry.rom_id);
  reader.RequiredNullable("rom_name", &rom_name);
  reader.Required("file_name", &entry.file_name);
  reader.RequiredNullable("slot", &entry.slot);
  reader.RequiredNullable("emulator", &emulator);
  reader.Required("when", &entry.when);
  reader.RequiredNullable("reason", &reason);
  reader.Required("sd_path", &entry.sd_path);
  reader.Required("local_size_bytes", &entry.local_size_bytes);
  reader.RequiredNullable("local_content_hash", &local_hash);
  reader.Required("local_modified", &entry.local_modified);
  reader.RequiredNullable("server_content_hash", &entry.server_content_hash);
  reader.RequiredNullable("server_updated_at", &server_updated_at);
  reader.Required("server_size_bytes", &entry.server_size_bytes);
  reader.RequiredNullable("backup_sd_path", &backup);
  if (!reader.ok()) {
    *why = reader.error().Describe();
    return false;
  }
  if (!ReadKind(kind, &entry.kind)) {
    *why = "field kind: " + kind + " is not a kind this build records";
    return false;
  }
  if (!ReadEvent(event, &entry.event)) {
    *why = "field event: " + event + " is not an event this build records";
    return false;
  }
  // Both before anything is derived from them, never after.
  if (!SecondsUsable(entry.when) || !SecondsUsable(entry.local_modified)) {
    *why = "a timestamp outside the range a file can claim";
    return false;
  }
  if (entry.id <= 0 || entry.rom_id <= 0) {
    *why = "an id that names nothing";
    return false;
  }
  if (entry.local_size_bytes < 0 || entry.server_size_bytes < 0) {
    *why = "a negative size";
    return false;
  }
  entry.rom_name = rom_name.value_or(std::string());
  entry.emulator = emulator.value_or(std::string());
  entry.reason = reason.value_or(std::string());
  entry.local_content_hash = local_hash.value_or(std::string());
  entry.server_updated_at = server_updated_at.value_or(std::string());
  entry.backup_sd_path = backup.value_or(std::string());

  *out = std::move(entry);
  return true;
}

const char* ToString(EntryKind kind) { return KindText(kind); }

const char* ToString(Event event) { return EventText(event); }

bool Overwrote(Event event) { return event == Event::kConflict || event == Event::kReplaced; }

const char* ToString(StoreError error) {
  switch (error) {
    case StoreError::kNone:
      return "none";
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

const char* ToString(RestoreOutcome outcome) {
  switch (outcome) {
    case RestoreOutcome::kRestored:
      return "restored";
    case RestoreOutcome::kNoSuchEntry:
      return "no_such_entry";
    case RestoreOutcome::kNothingToRestore:
      return "nothing_to_restore";
    case RestoreOutcome::kBackupMissing:
      return "backup_missing";
    case RestoreOutcome::kBackupFailed:
      return "backup_failed";
    case RestoreOutcome::kWriteFailed:
      return "write_failed";
  }
  return "no_such_entry";
}

std::string Shorten(std::string_view text) {
  // On a UTF-8 boundary, and `text::Shorten`'s rather than a fourth copy of the
  // walk: a page of these crosses the IPC wire, and half a code point there is a
  // row a renderer draws as a replacement glyph or refuses outright.
  return text::Shorten(text, kMaxTextBytes);
}

std::string SerializeHistory(const std::vector<Entry>& entries, std::int64_t next_id) {
  std::string out = FormatHeader(next_id);
  out += "\n";
  for (const Entry& entry : entries) {
    out += SerializeEntry(entry);
    out += "\n";
  }
  return out;
}

LoadedHistory ParseHistory(std::string_view text) {
  LoadedHistory loaded;

  std::vector<std::string_view> lines;
  for (std::size_t at = 0; at <= text.size();) {
    const std::size_t end = text.find('\n', at);
    const std::size_t stop = end == std::string_view::npos ? text.size() : end;
    lines.push_back(TrimCarriageReturn(text.substr(at, stop - at)));
    if (end == std::string_view::npos) {
      break;
    }
    at = end + 1;
  }
  while (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }

  // The header carries the next id, so it is parsed rather than compared. A
  // prefix match on the magic and the version is what says "these bytes are
  // this file"; the number after it is data.
  const std::string prefix = std::string(kFormatMagic) + " " + std::to_string(kFormatVersion) + " ";
  if (lines.empty() || lines.front().size() <= prefix.size() ||
      lines.front().substr(0, prefix.size()) != prefix) {
    // Includes an empty file and a truncated first line. The expected header is
    // named rather than what was found: the found bytes may be anything a
    // corrupt card region holds.
    loaded.diagnostics.push_back("conflicts.db does not begin with \"" + prefix +
                                 "<next id>\"; the conflict history is discarded, and the "
                                 "backups under .backup/ are untouched");
    return loaded;
  }
  const std::string_view counter = lines.front().substr(prefix.size());
  std::int64_t next_id = 0;
  for (const char digit : counter) {
    if (digit < '0' || digit > '9' || next_id > (kLatest * 1000)) {
      next_id = 0;
      break;
    }
    next_id = next_id * 10 + (digit - '0');
  }
  if (next_id <= 0) {
    loaded.diagnostics.push_back("conflicts.db's header carries no usable next id; the conflict "
                                 "history is discarded, and the backups under .backup/ are "
                                 "untouched");
    return loaded;
  }
  loaded.next_id = next_id;

  for (std::size_t at = 1; at < lines.size(); ++at) {
    if (loaded.entries.size() >= kMaxEntries) {
      if (loaded.diagnostics.size() < kMaxDiagnostics) {
        loaded.diagnostics.push_back("conflicts.db holds more than the " +
                                     std::to_string(kMaxEntries) +
                                     " entries a history keeps; the oldest are not listed");
      }
      break;
    }
    const json::ParseResult document = json::Parse(lines[at]);
    std::string why;
    Entry entry;
    if (!document.ok()) {
      why = document.error.Describe();
    } else if (!ParseEntry(document.value, &entry, &why)) {
      // `why` is set.
    } else {
      if (entry.id >= loaded.next_id) {
        // A row claiming an id the header does not know about. Kept -- it names
        // a real backup -- and the counter is moved past it, so the next append
        // cannot hand out the same number twice.
        loaded.next_id = entry.id + 1;
      }
      loaded.entries.push_back(std::move(entry));
      continue;
    }
    // One bad row is one row, not the file: see the header note on
    // `ParseHistory`.
    if (loaded.diagnostics.size() < kMaxDiagnostics) {
      loaded.diagnostics.push_back("conflicts.db row " + std::to_string(at) + ": " + why +
                                   "; the entry is dropped and its backup, if it had one, is "
                                   "still on the card");
    }
  }
  return loaded;
}

LoadedHistory LoadHistory(const std::string& path) {
  // **Bounded, not `ReadFile`.** `state::LoadBaseline`'s rule, and its reason:
  // this file sits on a FAT32 card that gets yanked mid-write, so a corrupt
  // directory entry claiming four gigabytes has to be a named refusal rather
  // than a `bad_alloc` on a 512 KiB heap before the diagnostic exists.
  std::string contents;
  io::BoundedRead outcome = io::ReadBounded(path, kMaxHistoryBytes, &contents);
  if (outcome == io::BoundedRead::kMissing) {
    // The window between `io::WriteAtomically`'s two renames, where the previous
    // file is intact under `.old`. `state::LoadBaseline` makes the same
    // recovery, and only for a *missing* file: one that exists and will not open
    // is a bad moment rather than a commit window.
    outcome = io::ReadBounded(io::PreviousPathFor(path), kMaxHistoryBytes, &contents);
    if (outcome == io::BoundedRead::kMissing) {
      LoadedHistory loaded;
      loaded.diagnostics.push_back(
          Describe(path, "there is no conflict history yet; nothing has been overwritten on this "
                         "console, or it has never synced"));
      return loaded;
    }
  }
  if (outcome != io::BoundedRead::kOk) {
    LoadedHistory loaded;
    loaded.diagnostics.push_back(
        Describe(path, std::string(io::ToString(outcome)) +
                           ": the conflict history is empty for this boot, and the backups under "
                           ".backup/ are untouched"));
    return loaded;
  }
  return ParseHistory(contents);
}

History::History(std::string path) : path_(std::move(path)) {}

std::vector<std::string> History::Load() {
  LoadedHistory loaded = LoadHistory(path_);
  entries_ = std::move(loaded.entries);
  next_id_ = loaded.next_id;
  return std::move(loaded.diagnostics);
}

const Entry* History::Find(std::int64_t id) const {
  for (const Entry& entry : entries_) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

StoreResult History::Persist() {
  StoreResult result;
  const std::string text = SerializeHistory(entries_, next_id_);
  if (text.size() > kMaxHistoryBytes) {
    // Unreachable with `kMaxEntries` entries of bounded strings -- which is what
    // `conflicts.store` asserts -- so reaching it means a bound moved. Refuse
    // rather than write a file the reader would discard whole.
    result.error = StoreError::kTooLarge;
    result.message = Describe(path_, "would be " + std::to_string(text.size()) +
                                         " bytes, more than a conflict history can be read back "
                                         "with; nothing was written");
    return result;
  }
  const io::WriteResult written = io::WriteAtomically(path_, text);
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
  if (result.error != StoreError::kNone) {
    result.message = written.message;
  }
  return result;
}

StoreResult History::Append(Entry entry) {
  StoreResult result;
  // The two paths are opened rather than read, so a shortened one is a restore
  // that writes somewhere else. An entry that cannot carry them whole is refused
  // instead -- see `kMaxTextBytes`.
  if (entry.rom_id <= 0 || entry.file_name.empty() || entry.sd_path.empty()) {
    result.error = StoreError::kUnusableEntry;
    result.message = "an entry naming no rom, no file or no path was not recorded";
    return result;
  }
  if (entry.sd_path.size() > config::kMaxPathLength ||
      entry.backup_sd_path.size() > config::kMaxPathLength) {
    result.error = StoreError::kUnusableEntry;
    result.message = "an entry whose paths are longer than a card can open was not recorded";
    return result;
  }
  entry.rom_name = Shorten(entry.rom_name);
  entry.file_name = Shorten(entry.file_name);
  entry.emulator = Shorten(entry.emulator);
  entry.reason = Shorten(entry.reason);
  entry.server_updated_at = Shorten(entry.server_updated_at);
  if (entry.slot.has_value()) {
    entry.slot = Shorten(*entry.slot);
  }

  entry.id = next_id_++;
  result.id = entry.id;
  entries_.insert(entries_.begin(), std::move(entry));
  // The oldest fall off the end. Their backups stay exactly where they are: see
  // `kMaxEntries`.
  if (entries_.size() > kMaxEntries) {
    entries_.resize(kMaxEntries);
  }

  const StoreResult written = Persist();
  if (!written.ok()) {
    // The entry stays in memory: a card that would not write must not also cost
    // the running console the only name it has for the backup that was just
    // made. See the header note on `Append`.
    result.error = written.error;
    result.message = written.message;
  }
  return result;
}

}  // namespace rommsync::conflicts
