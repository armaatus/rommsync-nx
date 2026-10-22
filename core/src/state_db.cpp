#include "rommsync/state_db.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/hash_file.hpp"
#include "rommsync/json.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::state {
namespace {

std::string Describe(const std::string& path, std::string_view what) {
  return path + ": " + std::string(what);
}

/// Whether `seconds` is a moment a save can claim, checked **before** anything
/// builds a `Timestamp` out of it.
///
/// The order matters and is not obvious. `system_clock::duration` is
/// *nanoseconds* on libstdc++ -- CI's runners and devkitA64 both -- so
/// `Timestamp{} + seconds{253402300799}` is 2.5e20 nanoseconds against an
/// `int64` that stops at 9.2e18: signed overflow, which is UB, and the wrapped
/// value then sails through any range check made afterwards. `kMaxTimestampSeconds`
/// is twenty-seven times the largest representable value, so even a figure this
/// module considers *legal* is unrepresentable. It is invisible on macOS, whose
/// libc++ counts microseconds and has room -- which is exactly why the check has
/// to be on the integer and not on the `Timestamp`.
bool SecondsInRange(std::int64_t seconds) {
  // The narrower of "what a save may claim" and "what the local clock type can
  // hold", so this is right whatever the platform's duration period is.
  constexpr std::int64_t kRepresentable =
      std::chrono::duration_cast<std::chrono::seconds>(sync::Timestamp::duration::max()).count();
  const std::int64_t ceiling =
      sync::kMaxTimestampSeconds < kRepresentable ? sync::kMaxTimestampSeconds : kRepresentable;
  return seconds >= sync::kMinTimestampSeconds && seconds <= ceiling;
}

/// Only ever called on a value `SecondsInRange` has accepted.
sync::Timestamp FromUnixSeconds(std::int64_t seconds) {
  return sync::Timestamp{} + std::chrono::seconds{seconds};
}

bool LowercaseHexDigest(const std::string& value) {
  if (value.size() != sync::kContentHashDigits) {
    return false;
  }
  for (const char digit : value) {
    const bool decimal = digit >= '0' && digit <= '9';
    const bool lower = digit >= 'a' && digit <= 'f';
    if (!decimal && !lower) {
      return false;
    }
  }
  return true;
}

/// The bar a row is held to in both directions.
///
/// The same bar on the way in and on the way out, deliberately: a row this
/// refuses to write is a row `ParseBaseline` would have thrown the whole file
/// away over, so writing one would cost the *next* boot its entire baseline
/// rather than this one its single row.
bool Usable(const SaveRecord& record, std::string* why) {
  if (record.rom_id <= 0) {
    *why = "rom_id must be a positive RomM rom id";
    return false;
  }
  if (record.slot.has_value() && record.slot->empty()) {
    *why = "slot is present and empty; a row is either slotted or null-slot";
    return false;
  }
  if (record.slot.has_value() && record.slot->find('\0') != std::string::npos) {
    *why = "slot carries a NUL";
    return false;
  }
  if (!LowercaseHexDigest(record.content_hash)) {
    // Named rather than "invalid": a 40-digit value here is a SHA1, which is
    // the mistake sync.hpp is written against, and an uppercase one matches
    // nothing on the server.
    *why = "content_hash must be " + std::to_string(sync::kContentHashDigits) +
           " lowercase hex digits (MD5)";
    return false;
  }
  if (record.server_content_hash.has_value() && !LowercaseHexDigest(*record.server_content_hash)) {
    *why = "server_content_hash must be " + std::to_string(sync::kContentHashDigits) +
           " lowercase hex digits (MD5), or null";
    return false;
  }
  if (!SecondsInRange(sync::UnixSeconds(record.mtime))) {
    // The epoch is what a console with an unset clock reports, and a baseline
    // row holding one would match no real file's mtime anyway. Refusing it here
    // keeps the row from becoming a `ClientSaveState` that `sync::Validate`
    // rejects one step later, with no way left to say which save it was.
    *why = "mtime is outside the range a save can claim";
    return false;
  }
  if (record.server_updated_at.has_value() &&
      !SecondsInRange(sync::UnixSeconds(*record.server_updated_at))) {
    *why = "server_updated_at is outside the range a save can claim";
    return false;
  }
  if (record.file_size_bytes < 0) {
    *why = "file_size_bytes is negative";
    return false;
  }
  return true;
}

/// The bar a state row is held to, in both directions and for `Usable`'s
/// reasons.
///
/// It differs from a save's in the two places the kinds differ: the key is
/// `(rom_id, file_name)` rather than `(rom_id, slot)`, and the server half is
/// *required* -- a state row exists only because a transfer landed, so a row
/// naming no server state is a row nothing can arbitrate against.
bool Usable(const StateRecord& record, std::string* why) {
  if (record.rom_id <= 0) {
    *why = "rom_id must be a positive RomM rom id";
    return false;
  }
  if (record.file_name.empty() || record.file_name.find('\0') != std::string::npos) {
    // Emptiness is checked here rather than left to `IsSingleFileName`, which
    // accepts it -- for a save, `sync::Validate` is what refuses an empty name,
    // and a state never goes through one. A NUL for `Usable(SaveRecord)`'s
    // reason: every C API downstream stops at it, so the value that gets used
    // would not be the value that was validated.
    *why = "file_name must be a name; a row is keyed on it";
    return false;
  }
  if (!sync::IsSingleFileName(record.file_name)) {
    // A name with a separator in it would be read back as the pairing key and
    // then joined into a URL and a backup path. `sync::IsSingleFileName` is the
    // same bar `ClientSaveState::file_name` is held to.
    *why = "file_name must be a single file name, not a path";
    return false;
  }
  if (record.emulator.find('\0') != std::string::npos) {
    *why = "emulator carries a NUL";
    return false;
  }
  if (!LowercaseHexDigest(record.content_hash)) {
    *why = "content_hash must be " + std::to_string(sync::kContentHashDigits) +
           " lowercase hex digits (MD5)";
    return false;
  }
  if (!SecondsInRange(sync::UnixSeconds(record.mtime))) {
    *why = "mtime is outside the range a state can claim";
    return false;
  }
  if (record.file_size_bytes < 0) {
    *why = "file_size_bytes is negative";
    return false;
  }
  if (record.server_state_id <= 0) {
    *why = "server_state_id must be the positive id of the RomM state row this is paired with";
    return false;
  }
  if (!SecondsInRange(sync::UnixSeconds(record.server_updated_at))) {
    *why = "server_updated_at is outside the range a state can claim";
    return false;
  }
  if (record.server_file_size_bytes < 0) {
    *why = "server_file_size_bytes is negative";
    return false;
  }
  return true;
}

std::string SerializeRecord(const SaveRecord& record) {
  std::string out("{\"rom_id\":");
  out += std::to_string(record.rom_id);
  out += ",\"slot\":";
  out += record.slot.has_value() ? json::Quote(*record.slot) : std::string("null");
  out += ",\"content_hash\":";
  out += json::Quote(record.content_hash);
  out += ",\"mtime\":";
  out += std::to_string(sync::UnixSeconds(record.mtime));
  out += ",\"file_size_bytes\":";
  out += std::to_string(record.file_size_bytes);
  out += ",\"server_updated_at\":";
  // `null` rather than an omitted key, on `token_store`'s reasoning: "the
  // server has never told us about this save" is an answer, and a reader that
  // could not tell it from a missing field would have to guess which one it was
  // looking at.
  out += record.server_updated_at.has_value()
             ? std::to_string(sync::UnixSeconds(*record.server_updated_at))
             : std::string("null");
  out += ",\"server_content_hash\":";
  out += record.server_content_hash.has_value() ? json::Quote(*record.server_content_hash)
                                                : std::string("null");
  if (record.deleted_on_server) {
    // Written only when set, so every other row stays byte-identical to the
    // ones earlier builds wrote, and an earlier build reading this one ignores a
    // key it does not name (#233).
    out += ",\"deleted_on_server\":true";
  }
  out += "}";
  return out;
}

/// A state row. `"kind"` comes first so a reader can see what it is holding
/// before it reads anything else, and a save row carries no `kind` at all --
/// which keeps it byte-identical to the v1 row it replaced (state_db.hpp).
std::string SerializeRecord(const StateRecord& record) {
  std::string out("{\"kind\":\"state\",\"rom_id\":");
  out += std::to_string(record.rom_id);
  out += ",\"file_name\":";
  out += json::Quote(record.file_name);
  out += ",\"emulator\":";
  // `null` rather than `""`, for the reason `sync::Validate` refuses the empty
  // string: "the directory implied no emulator" is an answer, and a reader that
  // could not tell it from an emulator called nothing would have to guess.
  out += record.emulator.empty() ? std::string("null") : json::Quote(record.emulator);
  out += ",\"content_hash\":";
  out += json::Quote(record.content_hash);
  out += ",\"mtime\":";
  out += std::to_string(sync::UnixSeconds(record.mtime));
  out += ",\"file_size_bytes\":";
  out += std::to_string(record.file_size_bytes);
  out += ",\"server_state_id\":";
  out += std::to_string(record.server_state_id);
  out += ",\"server_updated_at\":";
  out += std::to_string(sync::UnixSeconds(record.server_updated_at));
  out += ",\"server_file_size_bytes\":";
  out += std::to_string(record.server_file_size_bytes);
  out += "}";
  return out;
}

/// A `number | null` field, which `json::Reader` has no overload for.
bool ReadNullableSeconds(const json::Value& object, std::string_view key,
                         std::optional<sync::Timestamp>* out, std::string* why) {
  const json::Value* member = object.Find(key);
  if (member == nullptr) {
    *why = "field " + std::string(key) + ": missing";
    return false;
  }
  if (member->is_null()) {
    out->reset();
    return true;
  }
  if (!member->is_integer()) {
    *why = "field " + std::string(key) + ": expected whole seconds or null";
    return false;
  }
  if (!SecondsInRange(member->integer())) {
    *why = "field " + std::string(key) + ": outside the range a save can claim";
    return false;
  }
  *out = FromUnixSeconds(member->integer());
  return true;
}

bool ParseSaveRecord(const json::Value& object, SaveRecord* out, std::string* why) {
  SaveRecord record;
  std::int64_t mtime = 0;
  json::Reader reader(object, "state row");
  reader.Required("rom_id", &record.rom_id);
  reader.RequiredNullable("slot", &record.slot);
  reader.Required("content_hash", &record.content_hash);
  reader.Required("mtime", &mtime);
  reader.Required("file_size_bytes", &record.file_size_bytes);
  reader.RequiredNullable("server_content_hash", &record.server_content_hash);
  if (!reader.ok()) {
    *why = reader.error().Describe();
    return false;
  }
  if (!SecondsInRange(mtime)) {
    // Before the conversion, not after: see `SecondsInRange`. A corrupt card
    // region reading `"mtime":253402300799` is the case this whole module
    // promises to answer with a diagnostic, and it must not be UB on the way to
    // producing one.
    *why = "field mtime: outside the range a save can claim";
    return false;
  }
  record.mtime = FromUnixSeconds(mtime);
  if (!ReadNullableSeconds(object, "server_updated_at", &record.server_updated_at, why)) {
    return false;
  }
  if (const json::Value* deleted = object.Find("deleted_on_server"); deleted != nullptr) {
    // Optional, since #233 added it: absent is every row that predates it.
    if (!deleted->is_bool()) {
      *why = "field deleted_on_server: expected true or false";
      return false;
    }
    record.deleted_on_server = deleted->boolean();
  }
  if (!Usable(record, why)) {
    return false;
  }

  *out = std::move(record);
  return true;
}

bool ParseStateRecord(const json::Value& object, StateRecord* out, std::string* why) {
  StateRecord record;
  std::int64_t mtime = 0;
  std::int64_t server_updated_at = 0;
  std::optional<std::string> emulator;
  json::Reader reader(object, "state row");
  reader.Required("rom_id", &record.rom_id);
  reader.Required("file_name", &record.file_name);
  reader.RequiredNullable("emulator", &emulator);
  reader.Required("content_hash", &record.content_hash);
  reader.Required("mtime", &mtime);
  reader.Required("file_size_bytes", &record.file_size_bytes);
  reader.Required("server_state_id", &record.server_state_id);
  reader.Required("server_updated_at", &server_updated_at);
  reader.Required("server_file_size_bytes", &record.server_file_size_bytes);
  if (!reader.ok()) {
    *why = reader.error().Describe();
    return false;
  }
  // Both before the conversion, never after: see `SecondsInRange`.
  if (!SecondsInRange(mtime)) {
    *why = "field mtime: outside the range a state can claim";
    return false;
  }
  if (!SecondsInRange(server_updated_at)) {
    *why = "field server_updated_at: outside the range a state can claim";
    return false;
  }
  record.mtime = FromUnixSeconds(mtime);
  record.server_updated_at = FromUnixSeconds(server_updated_at);
  record.emulator = emulator.value_or(std::string());
  if (!Usable(record, why)) {
    return false;
  }
  *out = std::move(record);
  return true;
}

/// Which kind of row a line holds, or a reason it holds neither.
///
/// An absent `kind` is a save; `"state"` is a state. Anything else is a row this
/// build cannot read, and -- like every other unreadable row here -- it discards
/// the whole file rather than half-understanding it.
enum class RowKind { kSave, kState, kUnknown };

RowKind KindOf(const json::Value& object, std::string* why) {
  const json::Value* kind = object.Find("kind");
  if (kind == nullptr) {
    return RowKind::kSave;
  }
  if (!kind->is_string()) {
    *why = "field kind: expected a string";
    return RowKind::kUnknown;
  }
  if (kind->string() == "state") {
    return RowKind::kState;
  }
  *why = "field kind: \"" + kind->string() + "\" is not a kind of row this build knows";
  return RowKind::kUnknown;
}

/// The version line a well-formed file opens with.
std::string FormatHeader() {
  return std::string(kFormatMagic) + " " + std::to_string(kFormatVersion);
}

/// One line of `text`, `\r` trimmed so a file edited on a desktop still reads.
std::string_view TrimCarriageReturn(std::string_view line) {
  while (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return line;
}

void Add(std::vector<std::string>* diagnostics, std::string message) {
  if (diagnostics->size() < kMaxDiagnostics) {
    diagnostics->push_back(std::move(message));
  }
}

/// The bounds rc1 wrote its version 2 file at, before #246 cut them to
/// `kMaxRecords` and `kMaxStateBytes`. A v2 file is read at these, once, on the
/// boot after the upgrade -- and only so many of its rows are kept as this
/// build's bound holds (`Parser`).
constexpr std::size_t kRc1MaxRecords = 512;
constexpr std::size_t kRc1MaxStateBytes = 128 * 1024;

/// The longest line a streamed read holds: a row is a few hundred bytes, and a
/// line past this is not one any build wrote.
constexpr std::size_t kMaxLineBytes = 4 * 1024;

/// A `state.db`, a line at a time, so a file can be read without its text held
/// whole (`Load`).
///
/// **Never more than `kMaxRecords` rows kept**, which is what the heap holds of
/// a baseline (`kHeapStateBaseline`). Only an rc1 file can have more, and of
/// that, states and archival rows are kept first -- nothing rebuilds a state's
/// row, and an archival row is keyed as it was -- and slotted save rows fill
/// what is left, the rest let go: each costs its save one re-hash.
class Parser {
 public:
  /// One line, without its `\n`.
  void Line(std::string_view line) {
    line = TrimCarriageReturn(line);
    if (++number_ == 1) {
      // The version before #236 is read too: its rows are this version's, keyed
      // on slots that moved, and they are the migration's input
      // (`legacy_saves`).
      version_ = line == FormatHeader() ? kFormatVersion
                 : line == std::string(kFormatMagic) + " " + std::to_string(kSourceSlotVersion)
                     ? kSourceSlotVersion
                     : 0;
      return;
    }
    if (version_ == 0 || over_) {
      return;
    }
    if (line.empty()) {
      // A trailing newline is how every file this writes ends; only a blank
      // line with a row after it is a complaint.
      blank_ = blank_ == 0 ? number_ : blank_;
      return;
    }
    if (blank_ != 0) {
      Add(&complaints_, "state.db line " + std::to_string(blank_) + " is blank");
      blank_ = 0;
    }
    if (++rows_ > Bound()) {
      over_ = true;
      return;
    }
    Row(line);
  }

  int version() const { return version_; }

  LoadedBaseline Finish() {
    LoadedBaseline loaded;
    if (version_ == 0) {
      // Includes an empty file and a truncated first line. Naming the expected
      // header rather than quoting what was found: the found bytes may be
      // anything a corrupt card region holds.
      loaded.diagnostics.push_back("state.db does not begin with \"" + FormatHeader() +
                                   "\"; the baseline is discarded and every save is hashed");
      return loaded;
    }
    if (over_) {
      loaded.diagnostics.push_back("state.db holds more than the " + std::to_string(Bound()) +
                                   " rows a baseline may have; it is discarded and every save "
                                   "is hashed");
      return loaded;
    }
    if (!complaints_.empty()) {
      // All of it, not the rows that parsed. A truncation leaves a prefix that
      // is individually well-formed and collectively a lie -- the saves written
      // before the interruption and none of the ones after -- and a caller
      // cannot tell that apart from a complete file. Dropping the lot costs one
      // tick of hashing; trusting half of it costs a comparison that is quietly
      // wrong.
      loaded.diagnostics = std::move(complaints_);
      // Unconditionally, not through `Add`: the per-row complaints are capped,
      // and a file bad enough to hit the cap is exactly the one whose reader
      // most needs to be told the *whole* baseline went.
      loaded.diagnostics.push_back(
          "state.db is not intact; the whole baseline is discarded and every save is hashed");
      return loaded;
    }
    if (dropped_ > 0) {
      loaded.diagnostics.push_back(
          "state.db was written by rc1, past the " + std::to_string(kMaxRecords) +
          " rows this build reads; " + std::to_string(dropped_) +
          " row(s) were let go, slotted saves before states -- those are hashed again");
    }
    loaded.value = std::move(baseline_);
    loaded.legacy_saves = std::move(legacy_);
    loaded.version = version_;
    return loaded;
  }

 private:
  std::size_t Bound() const {
    return version_ == kSourceSlotVersion ? kRc1MaxRecords : kMaxRecords;
  }

  /// Whether a row more than this build's bound would be kept: a held-apart
  /// save row is let go for it, or, with none left, the row itself.
  bool MakeRoom() {
    if (baseline_.size() + legacy_.size() < kMaxRecords) {
      return true;
    }
    ++dropped_;
    if (legacy_.empty()) {
      return false;
    }
    legacy_.pop_back();
    return true;
  }

  void Row(std::string_view line) {
    const std::string where = "state.db line " + std::to_string(number_) + ": ";
    const json::ParseResult document = json::Parse(line);
    if (!document.ok()) {
      Add(&complaints_, where + document.error.Describe());
      return;
    }
    std::string why;
    switch (KindOf(document.value, &why)) {
      case RowKind::kSave: {
        SaveRecord record;
        if (!ParseSaveRecord(document.value, &record, &why)) {
          Add(&complaints_, where + why);
          return;
        }
        const bool held_apart = version_ == kSourceSlotVersion && record.slot.has_value();
        const bool second =
            held_apart ? std::any_of(legacy_.begin(), legacy_.end(),
                                     [&record](const SaveRecord& held) {
                                       return held.rom_id == record.rom_id &&
                                              held.slot == record.slot;
                                     })
                       : baseline_.Find(record.rom_id, record.slot) != nullptr;
        if (second) {
          Add(&complaints_, where + "a second row for the same (rom_id, slot)");
          return;
        }
        if (!MakeRoom()) {
          return;
        }
        if (held_apart) {
          legacy_.push_back(std::move(record));
        } else {
          baseline_.Set(std::move(record));
        }
        return;
      }
      case RowKind::kState: {
        StateRecord record;
        if (!ParseStateRecord(document.value, &record, &why)) {
          Add(&complaints_, where + why);
          return;
        }
        if (baseline_.FindState(record.rom_id, record.file_name) != nullptr) {
          Add(&complaints_, where + "a second row for the same (rom_id, file_name) state");
          return;
        }
        if (MakeRoom()) {
          baseline_.SetState(std::move(record));
        }
        return;
      }
      case RowKind::kUnknown:
        Add(&complaints_, where + why);
        return;
    }
  }

  std::size_t number_ = 0;
  std::size_t blank_ = 0;
  std::size_t rows_ = 0;
  std::size_t dropped_ = 0;
  int version_ = 0;
  bool over_ = false;
  Baseline baseline_;
  std::vector<SaveRecord> legacy_;
  std::vector<std::string> complaints_;
};

/// Read `path` and parse it into `*out`, or say why not.
///
/// At most `kMaxStateBytes`, `io::ReadBounded`'s bound and reasoning, shared
/// with `config.ini`. Past it, the file is read on only while it is rc1's
/// version 2 within rc1's bound -- a line at a time, its text never held, so it
/// costs the heap what a file inside this build's bound does.
io::BoundedRead Load(const std::string& path, LoadedBaseline* out) {
  {
    std::string contents;
    const io::BoundedRead outcome = io::ReadBounded(path, kMaxStateBytes, &contents);
    if (outcome != io::BoundedRead::kTooLarge) {
      if (outcome == io::BoundedRead::kOk) {
        *out = ParseBaseline(contents);
      }
      return outcome;
    }
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return io::BoundedRead::kUnreadable;
  }
  Parser parser;
  std::string line;
  char buffer[1024];
  std::size_t total = 0;
  std::size_t got = 0;
  bool fits = true;
  while (fits && (got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    total += got;
    fits = total <= kRc1MaxStateBytes;
    for (std::size_t at = 0; fits && at < got; ++at) {
      if (buffer[at] != '\n') {
        line.push_back(buffer[at]);
        fits = line.size() <= kMaxLineBytes;
        continue;
      }
      parser.Line(line);
      line.clear();
      fits = parser.version() == kSourceSlotVersion;
    }
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) {
    return io::BoundedRead::kUnreadable;
  }
  if (!fits) {
    return io::BoundedRead::kTooLarge;
  }
  parser.Line(line);
  *out = parser.Finish();
  return io::BoundedRead::kOk;
}

}  // namespace

const SaveRecord* Baseline::Find(std::int64_t rom_id, const std::optional<std::string>& slot) const {
  const auto found = rows_.find(Key{rom_id, slot});
  return found == rows_.end() ? nullptr : &found->second;
}

void Baseline::Set(SaveRecord record) {
  const Key key{record.rom_id, record.slot};
  rows_[key] = std::move(record);
}

bool Baseline::Erase(std::int64_t rom_id, const std::optional<std::string>& slot) {
  return rows_.erase(Key{rom_id, slot}) != 0;
}

const StateRecord* Baseline::FindState(std::int64_t rom_id, std::string_view file_name) const {
  const auto found = states_.find(StateKey{rom_id, std::string(file_name)});
  return found == states_.end() ? nullptr : &found->second;
}

void Baseline::SetState(StateRecord record) {
  const StateKey key{record.rom_id, record.file_name};
  states_[key] = std::move(record);
}

bool Baseline::EraseState(std::int64_t rom_id, std::string_view file_name) {
  return states_.erase(StateKey{rom_id, std::string(file_name)}) != 0;
}

std::string LoadedBaseline::DescribeDiagnostics() const {
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
    case StoreError::kTooManyRecords:
      return "too_many_records";
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

const char* ToString(HashError error) {
  switch (error) {
    case HashError::kNone:
      return "none";
    case HashError::kUnreadable:
      return "unreadable";
  }
  return "none";
}

std::string SerializeBaseline(const Baseline& baseline) {
  std::string out = FormatHeader();
  out += "\n";
  for (const auto& [key, record] : baseline.rows()) {
    (void)key;
    out += SerializeRecord(record);
    out += "\n";
  }
  // States after saves, in their own key order. The two blocks are stable and
  // never interleave, so a baseline rewritten with nothing changed still
  // produces byte-identical contents.
  for (const auto& [key, record] : baseline.state_rows()) {
    (void)key;
    out += SerializeRecord(record);
    out += "\n";
  }
  return out;
}

LoadedBaseline ParseBaseline(std::string_view text) {
  Parser parser;
  for (std::size_t at = 0; at <= text.size();) {
    const std::size_t end = text.find('\n', at);
    const std::size_t stop = end == std::string_view::npos ? text.size() : end;
    parser.Line(text.substr(at, stop - at));
    if (end == std::string_view::npos) {
      break;
    }
    at = end + 1;
  }
  return parser.Finish();
}

std::size_t AdoptLegacySaves(
    LoadedBaseline* loaded,
    const std::function<std::optional<std::string>(std::string_view old_slot)>& slot_for) {
  // In key order, so a contest for one new key goes the same way on every boot:
  // `retroarch-srm` sorts before `tico-srm`, as RetroArch's folders are read
  // before Tico's.
  std::vector<SaveRecord> legacy = std::move(loaded->legacy_saves);
  loaded->legacy_saves.clear();
  std::sort(legacy.begin(), legacy.end(), [](const SaveRecord& a, const SaveRecord& b) {
    return Baseline::Key{a.rom_id, a.slot} < Baseline::Key{b.rom_id, b.slot};
  });
  std::size_t adopted = 0;
  for (SaveRecord& record : legacy) {
    if (!record.slot.has_value() || slot_for == nullptr) {
      continue;
    }
    std::optional<std::string> slot = slot_for(*record.slot);
    if (!slot.has_value() || slot->empty() ||
        loaded->value.Find(record.rom_id, slot) != nullptr) {
      continue;
    }
    record.slot = std::move(slot);
    // The old slot's server, not the new one's: see the header.
    record.server_updated_at.reset();
    record.server_content_hash.reset();
    loaded->value.Set(std::move(record));
    ++adopted;
  }
  return adopted;
}

LoadedBaseline LoadBaseline(const std::string& path) {
  LoadedBaseline loaded;
  const io::BoundedRead outcome = Load(path, &loaded);
  if (outcome == io::BoundedRead::kOk) {
    return loaded;
  }

  if (outcome == io::BoundedRead::kMissing) {
    // The one moment `state.db` legitimately does not exist is the window
    // `io::WriteAtomically` opens between its two renames, and the previous
    // baseline is sitting under `.old`. `token_store`, `device_identity` and
    // `config` recover from it the same way, and only from a *missing* file.
    const std::string previous_path = io::PreviousPathFor(path);
    LoadedBaseline recovered;
    if (Load(previous_path, &recovered) == io::BoundedRead::kOk) {
      // Whatever came back, this branch answers -- including an empty baseline.
      // Falling through to the `kMissing` case would report "does not exist
      // yet", the message a brand-new card produces, and drop `recovered`'s
      // diagnostics on the floor. "A commit was interrupted *and* the copy it
      // left behind is unusable" is the one state worth seeing in a log, and it
      // is exactly the state that would have been silent. `config.cpp` takes
      // the parsed result unconditionally for the same reason.
      std::vector<std::string> diagnostics{
          Describe(path, recovered.value.empty() && recovered.legacy_saves.empty()
                             ? "is missing and " + previous_path +
                                   " holds no usable baseline either -- a write of it was "
                                   "interrupted; every save is hashed this tick"
                             : "is missing and was read from " + previous_path +
                                   " instead -- a write of it was interrupted")};
      for (std::string& diagnostic : recovered.diagnostics) {
        Add(&diagnostics, std::move(diagnostic));
      }
      recovered.diagnostics = std::move(diagnostics);
      return recovered;
    }
  }

  switch (outcome) {
    case io::BoundedRead::kMissing:
      // Not a failure: it is the first tick on this card, or a baseline that
      // was never written. Reported anyway, because the second tick reporting
      // it too is the difference between "new console" and "the write is
      // silently failing".
      loaded.diagnostics.push_back(
          Describe(path, "does not exist yet; every save is hashed this tick"));
      break;
    case io::BoundedRead::kUnreadable:
      loaded.diagnostics.push_back(
          Describe(path, "exists and could not be read; every save is hashed this tick"));
      break;
    case io::BoundedRead::kTooLarge:
      loaded.diagnostics.push_back(
          Describe(path, "is larger than the " + std::to_string(kMaxStateBytes) +
                             " bytes a baseline can be; every save is hashed this tick"));
      break;
    case io::BoundedRead::kOk:
      break;
  }
  return loaded;
}

StoreResult SaveBaseline(const std::string& path, const Baseline& baseline) {
  StoreResult result;

  // A bad row is left out; the rest still get a baseline. See the header for
  // why this is a skip and not a refusal -- an unset RTC stamping one save with
  // the epoch must not cost the whole library its baseline forever.
  Baseline writable;
  for (const auto& [key, record] : baseline.rows()) {
    (void)key;
    std::string why;
    if (!Usable(record, &why)) {
      Add(&result.skipped, "rom " + std::to_string(record.rom_id) + ": " + why +
                               "; this save is hashed again next tick");
      continue;
    }
    writable.Set(record);
  }
  for (const auto& [key, record] : baseline.state_rows()) {
    (void)key;
    std::string why;
    if (!Usable(record, &why)) {
      Add(&result.skipped, "rom " + std::to_string(record.rom_id) + " state \"" +
                               record.file_name + "\": " + why +
                               "; this state is hashed again next tick");
      continue;
    }
    writable.SetState(record);
  }

  // The file-level bounds *are* refusals: a file over either of them is one
  // `ParseBaseline` discards whole, so writing it would trade one loud failure
  // here for a silent re-hash of the library on every boot from now on.
  if (writable.size() > kMaxRecords) {
    result.error = StoreError::kTooManyRecords;
    result.message =
        Describe(path, "would hold " + std::to_string(writable.size()) + " rows, more than the " +
                           std::to_string(kMaxRecords) +
                           " a baseline can be read back with; nothing was written");
    return result;
  }

  const std::string text = SerializeBaseline(writable);
  if (text.size() > kMaxStateBytes) {
    result.error = StoreError::kTooLarge;
    result.message =
        Describe(path, "would be " + std::to_string(text.size()) + " bytes, more than the " +
                           std::to_string(kMaxStateBytes) +
                           " a baseline can be read back with; nothing was written");
    return result;
  }

  const io::WriteResult written = io::WriteAtomically(path, text);
  if (written.error != io::WriteError::kNone) {
    result.message = written.message;
    switch (written.error) {
      case io::WriteError::kOpenFailed:
        result.error = StoreError::kOpenFailed;
        break;
      case io::WriteError::kWriteFailed:
        result.error = StoreError::kWriteFailed;
        break;
      case io::WriteError::kNone:
      case io::WriteError::kCommitFailed:
        result.error = StoreError::kCommitFailed;
        break;
    }
    return result;
  }

  result.rows_written = writable.size();
  return result;
}

HashOutcome HashFile(const std::string& path) {
  HashOutcome outcome;

  // 4 KiB at a time and never the whole file (`crypto::StreamFile`): the chunk
  // is a stack budget rather than a throughput knob, and the reasoning is in
  // hash_file.hpp. `opened` is why this calls the shared loop rather than
  // `crypto::Md5FileHex` -- a save the scanner cannot open at all is a different
  // diagnostic from one whose read failed half way, and only that flag keeps
  // them apart.
  crypto::Md5Hasher hasher;
  bool opened = false;
  if (!crypto::StreamFile(path, hasher, &opened)) {
    outcome.error = HashError::kUnreadable;
    outcome.message =
        Describe(path, opened ? "could not be read to hash" : "could not be opened to hash");
    return outcome;
  }

  outcome.content_hash = hasher.FinishHex();
  return outcome;
}

HashOutcome ContentHashFor(const Baseline& baseline, std::int64_t rom_id,
                           const std::optional<std::string>& slot, const std::string& path,
                           sync::Timestamp mtime, std::int64_t file_size_bytes) {
  const SaveRecord* stored = baseline.Find(rom_id, slot);
  // Both, not either. A size that matches an mtime that does not is an emulator
  // that restored the timestamp; an mtime that matches a size that does not is
  // a rewrite inside the same second. Either alone reports a stale digest, and
  // a stale digest is a save the server believes it already has.
  if (stored != nullptr && sync::UnixSeconds(stored->mtime) == sync::UnixSeconds(mtime) &&
      stored->file_size_bytes == file_size_bytes) {
    HashOutcome outcome;
    outcome.content_hash = stored->content_hash;
    outcome.reused = true;
    return outcome;
  }
  return HashFile(path);
}

HashOutcome ContentHashForState(const Baseline& baseline, std::int64_t rom_id,
                                std::string_view file_name, const std::string& path,
                                sync::Timestamp mtime, std::int64_t file_size_bytes) {
  const StateRecord* stored = baseline.FindState(rom_id, file_name);
  // Both, not either -- `ContentHashFor`'s reasoning, and it matters more here:
  // a state is tens of megabytes, so a stale digest costs a needless upload of
  // all of them.
  if (stored != nullptr && sync::UnixSeconds(stored->mtime) == sync::UnixSeconds(mtime) &&
      stored->file_size_bytes == file_size_bytes) {
    HashOutcome outcome;
    outcome.content_hash = stored->content_hash;
    outcome.reused = true;
    return outcome;
  }
  return HashFile(path);
}

bool ReadBackFile(fs::FileSystem& files, fs::Directories& directories, const std::string& sd_path,
                  FileFacts* out, std::string* why) {
  if (sd_path.empty()) {
    *why = "the operation named no local file";
    return false;
  }
  const fs::Entry* entry = directories.Find(sd_path, why);
  if (entry == nullptr) {
    return false;
  }
  if (!SecondsInRange(entry->modified_unix)) {
    // The same window `sync::Validate` holds a save's mtime to. `SaveBaseline`
    // would skip the row anyway; refusing it here is what names the file.
    *why = "its mtime of " + std::to_string(entry->modified_unix) +
           " is not one this client can claim";
    return false;
  }
  const std::string resolved = files.Resolve(sd_path);
  if (resolved.empty()) {
    *why = sd_path + " is not a path on this card";
    return false;
  }
  const HashOutcome hashed = HashFile(resolved);
  if (!hashed.ok()) {
    *why = hashed.message;
    return false;
  }
  out->content_hash = hashed.content_hash;
  out->mtime = FromUnixSeconds(entry->modified_unix);
  out->file_size_bytes = entry->size_bytes;
  return true;
}

}  // namespace rommsync::state
