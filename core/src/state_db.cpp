#include "rommsync/state_db.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/json.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::state {
namespace {

std::string Describe(const std::string& path, std::string_view what) {
  return path + ": " + std::string(what);
}

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
  const std::int64_t mtime = sync::UnixSeconds(record.mtime);
  if (mtime < sync::kMinTimestampSeconds || mtime > sync::kMaxTimestampSeconds) {
    // The epoch is what a console with an unset clock reports, and a baseline
    // row holding one would match no real file's mtime anyway. Refusing it here
    // keeps the row from becoming a `ClientSaveState` that `sync::Validate`
    // rejects one step later, with no way left to say which save it was.
    *why = "mtime is outside the range a save can claim";
    return false;
  }
  if (record.server_updated_at.has_value()) {
    const std::int64_t updated = sync::UnixSeconds(*record.server_updated_at);
    if (updated < sync::kMinTimestampSeconds || updated > sync::kMaxTimestampSeconds) {
      *why = "server_updated_at is outside the range a save can claim";
      return false;
    }
  }
  if (record.file_size_bytes < 0) {
    *why = "file_size_bytes is negative";
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
  *out = FromUnixSeconds(member->integer());
  return true;
}

bool ParseRecord(std::string_view line, SaveRecord* out, std::string* why) {
  const json::ParseResult document = json::Parse(line);
  if (!document.ok()) {
    *why = document.error.Describe();
    return false;
  }

  SaveRecord record;
  std::int64_t mtime = 0;
  json::Reader reader(document.value, "state row");
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
  record.mtime = FromUnixSeconds(mtime);
  if (!ReadNullableSeconds(document.value, "server_updated_at", &record.server_updated_at, why)) {
    return false;
  }
  if (!Usable(record, why)) {
    return false;
  }

  *out = std::move(record);
  return true;
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

/// Read at most `kMaxStateBytes` of `path`. The bound and its reasoning are
/// `io::ReadBounded`'s, shared with `config.ini` rather than restated here.
io::BoundedRead ReadBounded(const std::string& path, std::string* out) {
  return io::ReadBounded(path, kMaxStateBytes, out);
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
    case StoreError::kUnusableRecord:
      return "unusable_record";
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
  return out;
}

LoadedBaseline ParseBaseline(std::string_view text) {
  LoadedBaseline loaded;

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
  // A trailing newline is how every file this writes ends; it is not a row.
  while (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }

  if (lines.empty() || lines.front() != FormatHeader()) {
    // Includes an empty file and a truncated first line. Naming the expected
    // header rather than quoting what was found: the found bytes may be
    // anything a corrupt card region holds.
    loaded.diagnostics.push_back("state.db does not begin with \"" + FormatHeader() +
                                 "\"; the baseline is discarded and every save is hashed");
    return loaded;
  }

  const std::size_t rows = lines.size() - 1;
  if (rows > kMaxRecords) {
    loaded.diagnostics.push_back("state.db holds more than the " + std::to_string(kMaxRecords) +
                                 " rows a baseline may have; it is discarded and every save is "
                                 "hashed");
    return loaded;
  }

  Baseline baseline;
  std::vector<std::string> complaints;
  for (std::size_t at = 1; at < lines.size(); ++at) {
    if (lines[at].empty()) {
      Add(&complaints, "state.db line " + std::to_string(at + 1) + " is blank");
      continue;
    }
    SaveRecord record;
    std::string why;
    if (!ParseRecord(lines[at], &record, &why)) {
      Add(&complaints, "state.db line " + std::to_string(at + 1) + ": " + why);
      continue;
    }
    if (baseline.Find(record.rom_id, record.slot) != nullptr) {
      Add(&complaints, "state.db line " + std::to_string(at + 1) +
                           ": a second row for the same (rom_id, slot)");
      continue;
    }
    baseline.Set(std::move(record));
  }

  if (!complaints.empty()) {
    // All of it, not the rows that parsed. A truncation leaves a prefix that is
    // individually well-formed and collectively a lie -- the saves written
    // before the interruption and none of the ones after -- and a caller cannot
    // tell that apart from a complete file. Dropping the lot costs one tick of
    // hashing; trusting half of it costs a comparison that is quietly wrong.
    loaded.diagnostics = std::move(complaints);
    // Unconditionally, not through `Add`: the per-row complaints are capped, and
    // a file bad enough to hit the cap is exactly the one whose reader most
    // needs to be told the *whole* baseline went, not just the rows named above.
    loaded.diagnostics.push_back(
        "state.db is not intact; the whole baseline is discarded and every save is hashed");
    return loaded;
  }

  loaded.value = std::move(baseline);
  return loaded;
}

LoadedBaseline LoadBaseline(const std::string& path) {
  std::string contents;
  const io::BoundedRead outcome = ReadBounded(path, &contents);
  if (outcome == io::BoundedRead::kOk) {
    return ParseBaseline(contents);
  }

  if (outcome == io::BoundedRead::kMissing) {
    // The one moment `state.db` legitimately does not exist is the window
    // `io::WriteAtomically` opens between its two renames, and the previous
    // baseline is sitting under `.old`. `token_store`, `device_identity` and
    // `config` recover from it the same way, and only from a *missing* file.
    std::string previous;
    if (ReadBounded(io::PreviousPathFor(path), &previous) == io::BoundedRead::kOk) {
      LoadedBaseline recovered = ParseBaseline(previous);
      if (!recovered.value.empty()) {
        std::vector<std::string> diagnostics{
            Describe(path, "is missing and was read from " + io::PreviousPathFor(path) +
                               " instead -- a write of it was interrupted")};
        for (std::string& diagnostic : recovered.diagnostics) {
          Add(&diagnostics, std::move(diagnostic));
        }
        recovered.diagnostics = std::move(diagnostics);
        return recovered;
      }
    }
  }

  LoadedBaseline loaded;
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
  // Refused before anything is written, on `token_store`'s reasoning: a file
  // that exists and cannot be parsed is worse than no file, because every later
  // boot finds one, discards all of it, and re-hashes the library.
  for (const auto& [key, record] : baseline.rows()) {
    (void)key;
    std::string why;
    if (!Usable(record, &why)) {
      return {StoreError::kUnusableRecord,
              Describe(path, "refusing to write a row for rom " + std::to_string(record.rom_id) +
                                 ": " + why)};
    }
  }

  const io::WriteResult written = io::WriteAtomically(path, SerializeBaseline(baseline));
  switch (written.error) {
    case io::WriteError::kNone:
      return {};
    case io::WriteError::kOpenFailed:
      return {StoreError::kOpenFailed, written.message};
    case io::WriteError::kWriteFailed:
      return {StoreError::kWriteFailed, written.message};
    case io::WriteError::kCommitFailed:
      return {StoreError::kCommitFailed, written.message};
  }
  return {StoreError::kCommitFailed, written.message};
}

HashOutcome HashFile(const std::string& path) {
  HashOutcome outcome;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    outcome.error = HashError::kUnreadable;
    outcome.message = Describe(path, "could not be opened to hash");
    return outcome;
  }

  // 4 KiB at a time, on the stack, which is the same chunk `io::ReadBounded`
  // uses next door. The size is not a throughput knob: `sys-rommsync.json` sets
  // `main_thread_stack_size` to 0x4000, so this frame gets **16 KiB in total**
  // and a buffer sized by how fast it felt on a desktop is a stack overflow on
  // the console. The read count is not the cost anyway -- stdio buffers
  // underneath, and a save state is bounded by the MD5 and the card, not by how
  // many times `fread` was called. The chunk size cannot change the digest,
  // which `core.md5` asserts directly.
  crypto::Md5Hasher hasher;
  char buffer[4096];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    hasher.Update(std::string_view(buffer, got));
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) {
    outcome.error = HashError::kUnreadable;
    outcome.message = Describe(path, "could not be read to hash");
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

}  // namespace rommsync::state
