// `state.db`: the format, the recovery, and the hashing it exists to skip.
//
// Pure filesystem and parsing, so this never skips -- and the guarantee it is
// written against is exactly the one a server cannot help with: **a lost
// baseline costs time, never correctness.** Every way this file can be wrong is
// below, and what is asserted about each is the same pair: that an empty
// baseline comes back rather than a refusal, and that the caller was told why.
//
// The other half is the optimisation itself. A file whose mtime and size both
// match its row is not re-opened, and one where either moved is. `HashOutcome`
// carries `reused` for no other reason than that the digest is identical either
// way, so nothing else in the process can tell the two apart -- including a
// test that only compared digests.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "checks.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"

namespace crypto = rommsync::crypto;
namespace io = rommsync::io;
namespace state = rommsync::state;
namespace sync = rommsync::sync;

namespace {

std::filesystem::path ScratchDir() {
  const std::filesystem::path dir = std::filesystem::path(ROMMSYNC_TEST_SCRATCH) / "state_db";
  std::filesystem::create_directories(dir);
  return dir;
}

/// A path with nothing beside it -- no `state.db`, no `.tmp`, no `.old`.
///
/// Every scenario below starts from one. These tests run in one process against
/// one scratch directory, so a run that left debris would otherwise be read by
/// the next test as a commit window to recover from.
std::filesystem::path FreshPath(const std::string& name) {
  const std::filesystem::path path = ScratchDir() / name;
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(io::TempPathFor(path.string()), ignored);
  std::filesystem::remove(io::PreviousPathFor(path.string()), ignored);
  return path;
}

sync::Timestamp At(std::int64_t seconds) {
  return sync::Timestamp{} + std::chrono::seconds{seconds};
}

std::string ReadWhole(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteWhole(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

/// A row with every field populated, including the two the server fills in.
state::SaveRecord Full() {
  state::SaveRecord record;
  record.rom_id = 12;
  record.slot = "autosave";
  record.content_hash = crypto::Md5Hex("Game (USA).srm contents");
  record.mtime = At(1757000000);
  record.file_size_bytes = 32768;
  record.server_updated_at = At(1756999000);
  record.server_content_hash = crypto::Md5Hex("what the server held");
  return record;
}

/// ...and one with neither, which is what a save with no sync history looks
/// like: the client has hashed it, the server has never mentioned it.
state::SaveRecord NoHistory() {
  state::SaveRecord record;
  record.rom_id = 12;
  record.slot.reset();  // archival / manual upload, a different row from any slotted one
  record.content_hash = crypto::Md5Hex("an archival save");
  record.mtime = At(1756000000);
  record.file_size_bytes = 1024;
  return record;
}

bool Mentions(const state::LoadedBaseline& loaded, const std::string& fragment) {
  return loaded.DescribeDiagnostics().find(fragment) != std::string::npos;
}

// --- the format ---------------------------------------------------------------

void EveryFieldRoundTrips(checks::Checks& c) {
  state::Baseline baseline;
  baseline.Set(Full());
  baseline.Set(NoHistory());

  const state::LoadedBaseline loaded = state::ParseBaseline(state::SerializeBaseline(baseline));
  c.Expect(loaded.diagnostics.empty(),
           "a file this module wrote parses silently -- " + loaded.DescribeDiagnostics());
  c.ExpectEq(loaded.value.size(), std::size_t{2}, "both rows came back");

  const state::SaveRecord* slotted = loaded.value.Find(12, std::optional<std::string>("autosave"));
  if (slotted == nullptr) {
    c.Expect(false, "the slotted row survived");
    return;
  }
  const state::SaveRecord expected = Full();
  c.ExpectEq(slotted->rom_id, expected.rom_id, "rom_id");
  c.Expect(slotted->slot == expected.slot, "slot");
  c.ExpectEq(slotted->content_hash, expected.content_hash, "content_hash");
  c.ExpectEq(sync::UnixSeconds(slotted->mtime), sync::UnixSeconds(expected.mtime), "mtime");
  c.ExpectEq(slotted->file_size_bytes, expected.file_size_bytes, "file_size_bytes");
  c.Expect(slotted->server_updated_at.has_value() &&
               sync::UnixSeconds(*slotted->server_updated_at) ==
                   sync::UnixSeconds(*expected.server_updated_at),
           "server_updated_at");
  c.Expect(slotted->server_content_hash == expected.server_content_hash, "server_content_hash");

  // The null-slot row is a *different* row from the slotted one, exactly as the
  // server pairs them. A reader that folded `null` into "" would have found one.
  const state::SaveRecord* archival = loaded.value.Find(12, std::nullopt);
  if (archival == nullptr) {
    c.Expect(false, "the null-slot row is its own row");
    return;
  }
  c.Expect(!archival->slot.has_value(), "and its slot is still null");
  c.Expect(!archival->server_updated_at.has_value(), "with no server history");
  c.Expect(!archival->server_content_hash.has_value(), "and no server digest");
}

/// A slot is user data. RomM takes whatever the client sends, and a filesystem
/// allows a tab, a quote and a backslash in a name -- which is the whole reason
/// the rows are JSON rather than a separator-delimited record.
void AwkwardSlotsSurvive(checks::Checks& c) {
  state::Baseline baseline;
  for (const std::string& slot : {std::string("with\ttab"), std::string("with\"quote"),
                                  std::string("with\\backslash"), std::string("with\nnewline"),
                                  std::string("héllo")}) {
    state::SaveRecord record = Full();
    record.slot = slot;
    record.content_hash = crypto::Md5Hex(slot);
    baseline.Set(record);
  }

  const state::LoadedBaseline loaded = state::ParseBaseline(state::SerializeBaseline(baseline));
  c.Expect(loaded.diagnostics.empty(), "the awkward slots parse -- " + loaded.DescribeDiagnostics());
  c.ExpectEq(loaded.value.size(), std::size_t{5}, "all five rows survived");
  // The newline is the one that matters: a slot carrying one has to not become
  // two lines, or the row after it is a fragment and the whole file is dropped.
  c.Expect(loaded.value.Find(12, std::optional<std::string>("with\nnewline")) != nullptr,
           "a slot with a newline in it is still one row");
}

/// Two identical baselines serialize to identical bytes.
///
/// Not cosmetic: M2-6 rewrites this file every tick, and a stable order is what
/// makes "nothing changed" visible as an unchanged file rather than as a diff
/// that means nothing.
void TheOrderIsStable(checks::Checks& c) {
  state::Baseline forwards;
  state::Baseline backwards;
  for (std::int64_t rom_id : {3, 1, 2}) {
    state::SaveRecord record = Full();
    record.rom_id = rom_id;
    forwards.Set(record);
  }
  for (std::int64_t rom_id : {2, 1, 3}) {
    state::SaveRecord record = Full();
    record.rom_id = rom_id;
    backwards.Set(record);
  }
  c.ExpectEq(state::SerializeBaseline(forwards), state::SerializeBaseline(backwards),
             "insertion order does not reach the file");
}

// --- the write ----------------------------------------------------------------

void AWriteLeavesNothingBeside(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("clean.db");

  state::Baseline baseline;
  baseline.Set(Full());
  const state::StoreResult first = state::SaveBaseline(path.string(), baseline);
  c.Expect(first.ok(), "the first write succeeded -- " + first.message);

  baseline.Set(NoHistory());
  const state::StoreResult again = state::SaveBaseline(path.string(), baseline);
  c.Expect(again.ok(), "and the rewrite -- " + again.message);

  c.Expect(!std::filesystem::exists(io::TempPathFor(path.string())),
           "a rewrite leaves no .tmp behind");

  const state::LoadedBaseline loaded = state::LoadBaseline(path.string());
  c.Expect(loaded.diagnostics.empty(), "and reads back clean -- " + loaded.DescribeDiagnostics());
  c.ExpectEq(loaded.value.size(), std::size_t{2}, "with both rows");
}

/// A commit interrupted after the first rename: `state.db` is gone and `.old`
/// holds the previous baseline. The same window `token.dat` and `device.dat`
/// recover from, and the same recovery (atomic_file.hpp).
void RecoversFromTheCommitWindow(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("window.db");

  state::Baseline baseline;
  baseline.Set(Full());
  WriteWhole(io::PreviousPathFor(path.string()), state::SerializeBaseline(baseline));

  const state::LoadedBaseline loaded = state::LoadBaseline(path.string());
  c.ExpectEq(loaded.value.size(), std::size_t{1}, "the previous baseline was recovered");
  c.Expect(loaded.value.Find(12, std::optional<std::string>("autosave")) != nullptr,
           "and it is the row that was there");
  c.Expect(Mentions(loaded, io::PreviousPathFor(path.string())),
           "the recovery is reported, not silent -- " + loaded.DescribeDiagnostics());
}

/// A row that could not be read back is refused before anything is written.
///
/// `token_store`'s reasoning: a file that exists and cannot be parsed is worse
/// than no file, because the next boot finds one, discards the lot, and
/// re-hashes the library for the life of the bug.
void RefusesARowItCouldNotReadBack(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("unusable.db");

  state::Baseline baseline;
  baseline.Set(Full());
  c.Expect(state::SaveBaseline(path.string(), baseline).ok(), "a good baseline is written");
  const std::string kept = ReadWhole(path.string());

  // A SHA1, which is the mistake this whole issue is written against.
  state::SaveRecord sha1 = Full();
  sha1.rom_id = 13;
  sha1.content_hash = std::string(40, 'a');
  baseline.Set(sha1);
  const state::StoreResult refused = state::SaveBaseline(path.string(), baseline);
  c.ExpectEq(state::ToString(refused.error), std::string("unusable_record"),
             "a 40-digit digest is refused");
  c.Expect(refused.message.find("13") != std::string::npos,
           "and the refusal names the rom -- " + refused.message);
  c.ExpectEq(ReadWhole(path.string()), kept, "the working baseline is untouched");

  // ...and an uppercase one, which is the same failure from a subtler cause.
  state::Baseline shouty;
  state::SaveRecord upper = Full();
  upper.content_hash = "D41D8CD98F00B204E9800998ECF8427E";
  shouty.Set(upper);
  c.ExpectEq(state::ToString(state::SaveBaseline(path.string(), shouty).error),
             std::string("unusable_record"), "an uppercase digest is refused too");

  // An empty slot is neither "autosave" nor archival, and the server pairs
  // those two differently.
  state::Baseline blank_slot;
  state::SaveRecord empty = Full();
  empty.slot = "";
  blank_slot.Set(empty);
  c.ExpectEq(state::ToString(state::SaveBaseline(path.string(), blank_slot).error),
             std::string("unusable_record"), "an empty slot is refused");

  c.ExpectEq(ReadWhole(path.string()), kept, "and none of them reached the file");
}

void NamesAMissingDirectory(checks::Checks& c) {
  const std::filesystem::path path = ScratchDir() / "no-such-directory" / "state.db";
  std::error_code ignored;
  std::filesystem::remove_all(path.parent_path(), ignored);

  state::Baseline baseline;
  baseline.Set(Full());
  const state::StoreResult result = state::SaveBaseline(path.string(), baseline);
  c.ExpectEq(state::ToString(result.error), std::string("open_failed"),
             "a missing directory is named, not a crash");
  c.Expect(result.message.find("state.db") != std::string::npos,
           "and the message says which file -- " + result.message);
}

// --- every way the file can be wrong ------------------------------------------

/// Each of these must yield an empty baseline **and** a diagnostic. Neither
/// alone is the contract: a silent empty baseline is a re-hash nobody can
/// explain, and a refusal is a tick that does not happen.
void CorruptFilesAreAnEmptyBaselineAndADiagnostic(checks::Checks& c) {
  state::Baseline baseline;
  baseline.Set(Full());
  baseline.Set(NoHistory());
  const std::string good = state::SerializeBaseline(baseline);

  struct Case {
    const char* what;
    std::string text;
  };
  const Case kCases[] = {
      {"an empty file", ""},
      {"no version line", good.substr(good.find('\n') + 1)},
      {"a future version", "rommsync-state 2\n"},
      {"a truncated header", "rommsync-sta"},
      {"a row that is not JSON", "rommsync-state 1\n{\"rom_id\":12,\n"},
      {"a row cut off mid-line", good.substr(0, good.size() - 20)},
      {"a field of the wrong type",
       "rommsync-state 1\n{\"rom_id\":\"12\",\"slot\":null,\"content_hash\":\"" +
           crypto::Md5Hex("x") +
           "\",\"mtime\":1757000000,\"file_size_bytes\":1,\"server_updated_at\":null,"
           "\"server_content_hash\":null}\n"},
      {"a missing field",
       "rommsync-state 1\n{\"rom_id\":12,\"slot\":null,\"content_hash\":\"" + crypto::Md5Hex("x") +
           "\",\"mtime\":1757000000,\"file_size_bytes\":1}\n"},
      {"a SHA1 where the MD5 goes",
       "rommsync-state 1\n{\"rom_id\":12,\"slot\":null,\"content_hash\":\"" + std::string(40, 'a') +
           "\",\"mtime\":1757000000,\"file_size_bytes\":1,\"server_updated_at\":null,"
           "\"server_content_hash\":null}\n"},
      {"a second row for the same (rom_id, slot)", good + good.substr(good.find('\n') + 1)},
      {"a card region full of zeroes", std::string(200, '\0')},
  };

  for (const Case& scenario : kCases) {
    const state::LoadedBaseline loaded = state::ParseBaseline(scenario.text);
    c.Expect(loaded.value.empty(), std::string(scenario.what) + " yields an empty baseline");
    c.Expect(!loaded.diagnostics.empty(), std::string(scenario.what) + " is diagnosed");
    c.Expect(loaded.diagnostics.size() <= state::kMaxDiagnostics + 1,
             std::string(scenario.what) + " is bounded");
  }

  // A file bad enough to exhaust the per-row cap still says the *whole* baseline
  // went. That sentence is the one a reader acts on, and capping it away is
  // capping away the only diagnostic that explains the re-hash.
  std::string flood = std::string(state::kFormatMagic) + " " +
                      std::to_string(state::kFormatVersion) + "\n";
  for (std::size_t at = 0; at < state::kMaxDiagnostics + 10; ++at) {
    flood += "not a row\n";
  }
  const state::LoadedBaseline flooded = state::ParseBaseline(flood);
  c.Expect(flooded.value.empty(), "a file of nothing but bad rows is an empty baseline");
  c.Expect(flooded.diagnostics.size() <= state::kMaxDiagnostics + 1,
           "the per-row complaints are still bounded");
  c.Expect(flooded.DescribeDiagnostics().find("the whole baseline is discarded") !=
               std::string::npos,
           "and the verdict survives the cap -- " + flooded.DescribeDiagnostics());
}

/// **Not the rows that happened to parse.** A truncation leaves a prefix that is
/// individually well-formed and collectively a lie, and there is no way for a
/// caller to tell it from a complete file.
void APartialFileIsNotAPartialBaseline(checks::Checks& c) {
  state::Baseline baseline;
  for (std::int64_t rom_id : {1, 2, 3, 4}) {
    state::SaveRecord record = Full();
    record.rom_id = rom_id;
    baseline.Set(record);
  }
  const std::string text = state::SerializeBaseline(baseline);
  const state::LoadedBaseline loaded = state::ParseBaseline(text.substr(0, text.size() - 15));

  c.Expect(loaded.value.empty(),
           "three intact rows and one fragment is not a baseline of three");
  c.Expect(!loaded.diagnostics.empty(), "and it is diagnosed");
}

/// A file too large to be one this client wrote is refused by size, not read.
void OversizedFilesAreRefusedBySize(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("huge.db");
  WriteWhole(path.string(), std::string(state::kMaxStateBytes + 1, 'x'));

  const state::LoadedBaseline loaded = state::LoadBaseline(path.string());
  c.Expect(loaded.value.empty(), "an oversized state.db is an empty baseline");
  c.Expect(Mentions(loaded, std::to_string(state::kMaxStateBytes)),
           "and the bound is named -- " + loaded.DescribeDiagnostics());
}

void TooManyRowsIsRefused(checks::Checks& c) {
  std::string text = std::string(state::kFormatMagic) + " " +
                     std::to_string(state::kFormatVersion) + "\n";
  std::string row;
  {
    state::Baseline one;
    one.Set(Full());
    const std::string serialized = state::SerializeBaseline(one);
    row = serialized.substr(serialized.find('\n') + 1);
  }
  for (std::size_t at = 0; at <= state::kMaxRecords; ++at) {
    text += row;
  }

  const state::LoadedBaseline loaded = state::ParseBaseline(text);
  c.Expect(loaded.value.empty(), "more rows than a baseline may hold is an empty baseline");
  c.Expect(Mentions(loaded, std::to_string(state::kMaxRecords)),
           "and the bound is named -- " + loaded.DescribeDiagnostics());
}

/// The first tick on a card. A diagnostic, because the *second* tick reporting
/// it too is the difference between "new console" and "the write silently
/// fails".
void AMissingFileIsReportedAndNotAFailure(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("absent.db");
  const state::LoadedBaseline loaded = state::LoadBaseline(path.string());
  c.Expect(loaded.value.empty(), "a card with no state.db has an empty baseline");
  c.ExpectEq(loaded.diagnostics.size(), std::size_t{1}, "and says so exactly once");
  c.Expect(Mentions(loaded, path.string()), "naming the path -- " + loaded.DescribeDiagnostics());
}

// --- the optimisation itself --------------------------------------------------

void HashesAFileTheWayTheServerWill(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("save.srm");
  // Many times HashFile's 4 KiB buffer, so the chunking is actually exercised --
  // the digest of a file that fits in one read proves nothing about a save
  // state, which is tens of megabytes.
  std::string bytes;
  bytes.reserve(100 * 1024);
  for (std::size_t at = 0; at < 100 * 1024; ++at) {
    bytes += static_cast<char>(at % 251);
  }
  WriteWhole(path.string(), bytes);

  const state::HashOutcome hashed = state::HashFile(path.string());
  c.Expect(hashed.ok(), "the file hashed -- " + hashed.message);
  c.ExpectEq(hashed.content_hash, crypto::Md5Hex(bytes),
             "and streaming it gives the digest of its bytes");
  c.Expect(!hashed.reused, "a fresh hash is not a reuse");

  const state::HashOutcome missing = state::HashFile((ScratchDir() / "not-there.srm").string());
  c.ExpectEq(state::ToString(missing.error), std::string("unreadable"),
             "a file that is not there is named, not a crash");
  c.Expect(missing.content_hash.empty(), "and no digest is invented for it");
}

/// The acceptance criterion this whole module exists for, in three parts: an
/// unchanged file is not re-hashed, its payload entry still carries the hash,
/// and a file that moved is re-hashed.
void UnchangedIsSkippedChangedIsNot(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("tick.srm");
  WriteWhole(path.string(), "the save as it was");
  const std::string first_digest = crypto::Md5Hex("the save as it was");

  const std::int64_t mtime_seconds = 1757000000;
  const std::int64_t size = 18;

  state::Baseline baseline;
  {
    state::SaveRecord record;
    record.rom_id = 12;
    record.slot = "autosave";
    record.content_hash = first_digest;
    record.mtime = At(mtime_seconds);
    record.file_size_bytes = size;
    baseline.Set(record);
  }

  // Second tick, nothing moved. The path is one that does not exist, which is
  // the only way to prove the file was never opened: a reader that fell back to
  // hashing would report `unreadable` instead of the stored digest.
  const std::string absent = (ScratchDir() / "never-opened.srm").string();
  const state::HashOutcome unchanged = state::ContentHashFor(
      baseline, 12, std::optional<std::string>("autosave"), absent, At(mtime_seconds), size);
  c.Expect(unchanged.reused, "an unchanged file is not re-hashed");
  c.Expect(unchanged.ok(), "and that is not a failure");
  c.ExpectEq(unchanged.content_hash, first_digest,
             "and the payload entry still carries the stored hash");

  // The size moved, the mtime did not -- an emulator that restores timestamps.
  // This one has to be re-read, so it points at the real file.
  WriteWhole(path.string(), "the save as it is now, longer");
  const state::HashOutcome resized = state::ContentHashFor(
      baseline, 12, std::optional<std::string>("autosave"), path.string(), At(mtime_seconds), 29);
  c.Expect(!resized.reused, "a file whose size changed is re-hashed");
  c.ExpectEq(resized.content_hash, crypto::Md5Hex("the save as it is now, longer"),
             "with the new bytes' digest");

  // ...and the mirror image: the same size, a later mtime. A same-size
  // overwrite is the case a size check alone would miss.
  WriteWhole(path.string(), "the save as it WAS");
  const state::HashOutcome touched =
      state::ContentHashFor(baseline, 12, std::optional<std::string>("autosave"), path.string(),
                            At(mtime_seconds + 1), size);
  c.Expect(!touched.reused, "a file whose mtime moved is re-hashed");
  c.ExpectEq(touched.content_hash, crypto::Md5Hex("the save as it WAS"), "with its digest");

  // A save the baseline has never seen is hashed, and a slotted row is not the
  // null-slot save's row.
  WriteWhole(path.string(), "an unknown save");
  const state::HashOutcome unknown = state::ContentHashFor(baseline, 99, std::nullopt,
                                                           path.string(), At(mtime_seconds), size);
  c.Expect(!unknown.reused, "a save with no row is hashed");
  const state::HashOutcome wrong_slot = state::ContentHashFor(
      baseline, 12, std::nullopt, path.string(), At(mtime_seconds), size);
  c.Expect(!wrong_slot.reused, "and the null-slot save does not reuse the slotted row's digest");
}

/// A baseline that could not be read is a slow tick, not a wrong one: every
/// save is hashed, and every one still gets a digest into the payload.
void ALostBaselineCostsTimeNotCorrectness(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("garbage.db");
  WriteWhole(path.string(), "\x01\x02 not a state.db at all\n");
  WriteWhole((ScratchDir() / "real.srm").string(), "the save");

  const state::LoadedBaseline loaded = state::LoadBaseline(path.string());
  c.Expect(loaded.value.empty(), "the garbage is discarded");
  c.Expect(!loaded.diagnostics.empty(), "with a diagnostic");

  const state::HashOutcome outcome =
      state::ContentHashFor(loaded.value, 12, std::optional<std::string>("autosave"),
                            (ScratchDir() / "real.srm").string(), At(1757000000), 8);
  c.Expect(outcome.ok(), "and the tick proceeds -- " + outcome.message);
  c.Expect(!outcome.reused, "by hashing");
  c.ExpectEq(outcome.content_hash, crypto::Md5Hex("the save"), "into a real digest");
}

}  // namespace

int main() {
  checks::Checks c;
  EveryFieldRoundTrips(c);
  AwkwardSlotsSurvive(c);
  TheOrderIsStable(c);
  AWriteLeavesNothingBeside(c);
  RecoversFromTheCommitWindow(c);
  RefusesARowItCouldNotReadBack(c);
  NamesAMissingDirectory(c);
  CorruptFilesAreAnEmptyBaselineAndADiagnostic(c);
  APartialFileIsNotAPartialBaseline(c);
  OversizedFilesAreRefusedBySize(c);
  TooManyRowsIsRefused(c);
  AMissingFileIsReportedAndNotAFailure(c);
  HashesAFileTheWayTheServerWill(c);
  UnchangedIsSkippedChangedIsNot(c);
  ALostBaselineCostsTimeNotCorrectness(c);
  return c.failures() == 0 ? 0 : 1;
}
