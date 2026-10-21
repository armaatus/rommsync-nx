// Every file on the card, as v1.0.0-rc1 wrote it, read by this build -- and
// every one of them as a *later* build would write it, read by this one (#205).
//
// The fixtures under tests/fixtures/v1.0.0-rc1/ are that release's own bytes:
// `generate.cpp` beside them compiles against the tag's `core/`, not this one's.
// So two things are asserted of each, and each fails on its own kind of drift:
//
//   - **reading**: the values that release wrote come back out. A parser that
//     stopped reading an old file is a user's settings, queue or pairing gone on
//     the first boot after an upgrade.
//   - **writing**: while a format's version is still the one rc1 wrote, this
//     build writes those records back byte for byte. Bytes that changed under an
//     unchanged version are a format that moved without a migration -- the one
//     outcome the version exists to rule out. Bumping the version turns that
//     check off for the file, and the reading half is then what says whether
//     the migration covers rc1.
//
// The older direction *is* the fixtures: for every file but `state.db`, rc1
// wrote the first format there has ever been, so there is nothing older to
// read. `state.db`'s version 1 predates rc1 and is test_state_db's.
//
// The newer direction is the downgrade: a file written by a release this build
// predates. Each file's answer is its policy (docs/ARCHITECTURE.md, "Formats
// across releases"): what can be rebuilt is discarded and rebuilt, what cannot
// is refused and left exactly as it was, and `device.dat` -- whose one field has
// to survive any version -- is read for that field whatever else it carries.
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "checks.hpp"
#include "scratch.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/config.hpp"
#include "rommsync/conflict_log.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/download.hpp"
#include "rommsync/play_sessions.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/token_store.hpp"

namespace fsys = std::filesystem;
namespace rs = rommsync;

namespace {

/// The formats rc1 wrote, as numbers. Spelled here rather than read off the
/// headers: they are facts about a release that has shipped, and the header's
/// constant is the thing that moves.
constexpr int kRc1Config = 1;  // no `version` line at all
constexpr int kRc1State = 2;
constexpr int kRc1Queue = 1;
constexpr int kRc1Conflicts = 1;
constexpr int kRc1Play = 1;
constexpr int kRc1Auth = 1;
constexpr int kRc1Token = 1;  // no `version` field at all

std::string Slurp(const fsys::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string Fixture(const char* name) {
  return Slurp(fsys::path(ROMMSYNC_FIXTURES) / "v1.0.0-rc1" / name);
}

void Put(const fsys::path& path, const std::string& bytes) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

/// `text` with the first `from` replaced by `to`. The newer-version fixtures are
/// the rc1 ones with the version moved on, rather than a second set of files
/// imagining what a release that does not exist would write.
std::string Bumped(std::string text, const std::string& from, const std::string& to) {
  const std::size_t at = text.find(from);
  if (at != std::string::npos) {
    text.replace(at, from.size(), to);
  }
  return text;
}

fsys::path Dir(const char* name) {
  const fsys::path dir = fsys::path(scratch::Dir()) / "compat" / name;
  fsys::remove_all(dir);
  fsys::create_directories(dir);
  return dir;
}

std::size_t Count(const rs::config::LoadResult& result, rs::config::Severity severity) {
  std::size_t total = 0;
  for (const rs::config::Diagnostic& one : result.diagnostics) {
    total += one.severity == severity ? 1 : 0;
  }
  return total;
}

bool Mentions(const std::vector<rs::config::Diagnostic>& diagnostics, const std::string& what) {
  for (const rs::config::Diagnostic& one : diagnostics) {
    if (one.Describe().find(what) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool Mentions(const std::vector<std::string>& diagnostics, const std::string& what) {
  for (const std::string& one : diagnostics) {
    if (one.find(what) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// --- config.ini: migrate older, refuse newer --------------------------------------

void ConfigIni(checks::Checks& c) {
  const std::string rc1 = Fixture("config_ini");
  c.Expect(!rc1.empty(), "config.ini: the rc1 fixture is there");

  const rs::config::LoadResult read = rs::config::ParseConfig(rc1);
  c.ExpectEq(Count(read, rs::config::Severity::kError), std::size_t{0},
             "config.ini rc1: no errors -- " + read.DescribeDiagnostics());
  c.ExpectEq(Count(read, rs::config::Severity::kWarning), std::size_t{0},
             "config.ini rc1: no warnings -- " + read.DescribeDiagnostics());
  c.ExpectEq(read.version, kRc1Config, "config.ini rc1: a file with no version line is version 1");
  c.ExpectEq(read.value.server.url, std::string("https://romm.lan:8443/romm"),
             "config.ini rc1: the server");
  c.ExpectEq(read.value.sync.interval_min, 60, "config.ini rc1: the interval");
  c.Expect(read.value.sync.states, "config.ini rc1: states on");
  c.Expect(!read.value.downloads.resume, "config.ini rc1: resume off");
  const rs::config::PlatformFolders* snes = read.value.Platform("snes");
  c.Expect(snes != nullptr && snes->saves.size() == 3 && snes->saves[2] == "/saves/snes",
           "config.ini rc1: the snes section the overlay wrote");
  c.ExpectEq(read.value.RomTarget("ps2"), std::string("/roms/ps2"),
             "config.ini rc1: a platform rc1 did not map by default");
  c.ExpectEq(read.value.RomTarget("gba"), rs::config::Defaults().RomTarget("gba"),
             "config.ini rc1: and the built-in map for everything else");

  // The key #229 added after rc1 went out, written by this build into an rc1
  // file. Additive with a default, so no version bump -- and an emulator-only
  // section keeps the platform's folders rather than replacing them with none.
  std::string upgraded;
  std::vector<rs::config::Diagnostic> notes;
  rs::config::Edit edit;
  edit.assignments.push_back({"platform.gb", "emulator", "mgba", false});
  c.Expect(rs::config::ApplyEdit(rc1, edit, &upgraded, &notes),
           "config.ini rc1: this build edits it");
  const rs::config::LoadResult after = rs::config::ParseConfig(upgraded);
  c.ExpectEq(after.version, kRc1Config, "config.ini rc1: and the edit does not move its version");
  c.ExpectEq(std::string(after.value.EmulatorOverride("gb")), std::string("mgba"),
             "config.ini rc1: the new key is read");
  const rs::config::PlatformFolders* gb = after.value.Platform("gb");
  const rs::config::Config defaults = rs::config::Defaults();
  const rs::config::PlatformFolders* gb_default = defaults.Platform("gb");
  c.Expect(gb != nullptr && gb_default != nullptr && gb->saves == gb_default->saves &&
               gb->roms == gb_default->roms,
           "config.ini rc1: an emulator-only section keeps gb's built-in folders");
  // ...and the first folder line added to that section carries the rest of the
  // folders in force with it, rather than making it a map of one folder.
  std::string folded;
  rs::config::Edit folder;
  folder.assignments.push_back({"platform.gb", "roms", "/roms/gb", false});
  c.Expect(rs::config::ApplyEdit(upgraded, folder, &folded, &notes),
           "config.ini rc1: a folder edit to the emulator-only section");
  const rs::config::LoadResult carried = rs::config::ParseConfig(folded);
  const rs::config::PlatformFolders* gb_now = carried.value.Platform("gb");
  c.Expect(gb_now != nullptr && gb_default != nullptr && gb_now->roms.size() == 1 &&
               gb_now->roms[0] == "/roms/gb" && gb_now->saves == gb_default->saves &&
               gb_now->states == gb_default->states,
           "config.ini rc1: carries gb's built-in saves and states beside the new roms");
  c.ExpectEq(std::string(carried.value.EmulatorOverride("gb")), std::string("mgba"),
             "config.ini rc1: and keeps the emulator");

  // Version 1 spelled out is the same file.
  const rs::config::LoadResult explicit_one = rs::config::ParseConfig("version = 1\n" + rc1);
  c.ExpectEq(Count(explicit_one, rs::config::Severity::kWarning), std::size_t{0},
             "config.ini: 'version = 1' is no complaint -- " + explicit_one.DescribeDiagnostics());
  c.ExpectEq(explicit_one.value.sync.interval_min, 60, "config.ini: and reads the same");

  // A version line nobody can read is a typo, not a release: said, and read as
  // this build's own, because nothing in this file may stop the client.
  const rs::config::LoadResult garbled = rs::config::ParseConfig("version = one\n" + rc1);
  c.ExpectEq(Count(garbled, rs::config::Severity::kWarning), std::size_t{1},
             "config.ini: an unreadable version is one warning -- " +
                 garbled.DescribeDiagnostics());
  c.ExpectEq(garbled.value.sync.interval_min, 60, "config.ini: and the rest still applies");

  // Newer: refused, not misparsed. The defaults are in force, the one error
  // says which release to install, and an edit leaves the file alone -- an
  // older build rewriting a newer file is how a downgrade destroys settings.
  const std::string newer = "version = " + std::to_string(rs::config::kConfigVersion + 1) + "\n" + rc1;
  const rs::config::LoadResult refused = rs::config::ParseConfig(newer);
  c.ExpectEq(refused.version, rs::config::kConfigVersion + 1, "config.ini newer: its version");
  c.ExpectEq(Count(refused, rs::config::Severity::kError), std::size_t{1},
             "config.ini newer: exactly one error -- " + refused.DescribeDiagnostics());
  c.Expect(Mentions(refused.diagnostics, "newer") && Mentions(refused.diagnostics, "sys-rommsync"),
           "config.ini newer: the error says what to update -- " + refused.DescribeDiagnostics());
  c.Expect(!refused.value.configured(), "config.ini newer: none of it is in force");
  c.ExpectEq(refused.value.sync.interval_min, rs::config::Defaults().sync.interval_min,
             "config.ini newer: not even the keys this build knows");

  // A number past anything an `int` holds is no typo either.
  const rs::config::LoadResult huge = rs::config::ParseConfig("version = 99999999999\n" + rc1);
  c.Expect(huge.newer() && !huge.value.configured(),
           "config.ini: an enormous version is refused, not read as version 1 -- " +
               huge.DescribeDiagnostics());

  std::string untouched = "sentinel";
  std::vector<rs::config::Diagnostic> why;
  rs::config::Edit any;
  any.assignments.push_back({"sync", "interval_min", "15", false});
  c.Expect(!rs::config::ApplyEdit(newer, any, &untouched, &why),
           "config.ini newer: an edit is refused");
  c.ExpectEq(untouched, std::string("sentinel"), "config.ini newer: and writes nothing");
  c.Expect(Mentions(why, "newer"), "config.ini newer: saying why");

  // A file this build creates carries its version from the first byte.
  std::string fresh;
  std::vector<rs::config::Diagnostic> ignored;
  rs::config::Edit first;
  first.assignments.push_back({"server", "url", "https://romm.lan", false});
  c.Expect(rs::config::ApplyEdit("", first, &fresh, &ignored), "config.ini: a first edit");
  c.Expect(fresh.rfind("version = " + std::to_string(rs::config::kConfigVersion) + "\n", 0) == 0,
           "config.ini: a file this build creates starts with its version -- " + fresh);
  c.Expect(rs::config::ParseConfig(fresh).value.configured(), "config.ini: and reads back");
}

// --- state.db: discard and rehash, both ways ----------------------------------------

void StateDb(checks::Checks& c) {
  const std::string rc1 = Fixture("state_db");
  const rs::state::LoadedBaseline read = rs::state::ParseBaseline(rc1);
  c.Expect(read.diagnostics.empty(), "state.db rc1: no complaints -- " + read.DescribeDiagnostics());
  c.ExpectEq(read.value.rows().size(), std::size_t{2}, "state.db rc1: both saves");
  c.ExpectEq(read.value.state_rows().size(), std::size_t{1}, "state.db rc1: and the state");
  const rs::state::SaveRecord* save = read.value.Find(101, std::string("retroarch-srm"));
  c.Expect(save != nullptr && save->content_hash == "0123456789abcdef0123456789abcdef" &&
               save->file_size_bytes == 32768 &&
               rs::sync::UnixSeconds(save->mtime) == 1757000000 &&
               save->server_content_hash == std::string("fedcba9876543210fedcba9876543210"),
           "state.db rc1: a save row's values");
  const rs::state::SaveRecord* slotless = read.value.Find(202, std::nullopt);
  c.Expect(slotless != nullptr && !slotless->server_updated_at.has_value(),
           "state.db rc1: a slotless save with no server side");
  const rs::state::StateRecord* state = read.value.FindState(101, "Super Game.state1");
  c.Expect(state != nullptr && state->server_state_id == 77 && state->emulator == "retroarch",
           "state.db rc1: the state row");
  if (rs::state::kFormatVersion == kRc1State) {
    c.ExpectEq(rs::state::SerializeBaseline(read.value), rc1,
               "state.db: written differently under rc1's version -- bump kFormatVersion");
  }

  // Older (version 1) is test_state_db's: discarded and rehashed. Newer is the
  // same answer -- a baseline is an optimisation, rebuilt from the card.
  const rs::state::LoadedBaseline newer = rs::state::ParseBaseline(Bumped(
      rc1, "rommsync-state " + std::to_string(rs::state::kFormatVersion),
      "rommsync-state " + std::to_string(rs::state::kFormatVersion + 1)));
  c.Expect(newer.value.empty(), "state.db newer: discarded");
  c.Expect(!newer.diagnostics.empty(), "state.db newer: and said");
}

// --- queue.json: discard, both ways ---------------------------------------------------

void QueueJson(checks::Checks& c) {
  const std::string rc1 = Fixture("queue_json");
  const rs::download::LoadedQueue read = rs::download::ParseQueue(rc1);
  c.Expect(!read.discarded && read.trusted, "queue.json rc1: kept -- " + read.DescribeDiagnostics());
  c.ExpectEq(read.entries.size(), std::size_t{4}, "queue.json rc1: every row");
  if (read.entries.size() == 4) {
    c.Expect(read.entries[0].rom_id == 301 && read.entries[0].sha1_hash.size() == 40 &&
                 read.entries[0].destination == "/tico/roms/gba/Pocket Game.gba",
             "queue.json rc1: a queued row");
    c.Expect(read.entries[1].state == rs::download::QueueState::kActive &&
                 read.entries[1].bytes_done == 16384,
             "queue.json rc1: a row a power cut left active, with its progress");
    c.Expect(read.entries[3].state == rs::download::QueueState::kSkipped &&
                 read.entries[3].message == "a multi-disc set is not downloaded",
             "queue.json rc1: a skipped row and its reason");
  }
  if (rs::download::kFormatVersion == kRc1Queue) {
    c.ExpectEq(rs::download::SerializeQueue(read.entries), rc1,
               "queue.json: written differently under rc1's version -- bump kFormatVersion");
  }

  const rs::download::LoadedQueue newer = rs::download::ParseQueue(
      Bumped(rc1, "\"version\":" + std::to_string(rs::download::kFormatVersion),
             "\"version\":" + std::to_string(rs::download::kFormatVersion + 1)));
  c.Expect(newer.discarded && newer.entries.empty(), "queue.json newer: discarded");
}

// --- conflicts.db: refuse newer, and never write over it ----------------------------

void ConflictsDb(checks::Checks& c) {
  const std::string rc1 = Fixture("conflicts_db");
  const rs::conflicts::LoadedHistory read = rs::conflicts::ParseHistory(rc1);
  c.Expect(read.diagnostics.empty(), "conflicts.db rc1: no complaints");
  c.ExpectEq(read.next_id, std::int64_t{3}, "conflicts.db rc1: the counter");
  c.ExpectEq(read.entries.size(), std::size_t{2}, "conflicts.db rc1: both entries");
  if (read.entries.size() == 2) {
    const rs::conflicts::Entry& save = read.entries[1];
    c.Expect(save.id == 1 && save.slot == std::string("retroarch-srm") && save.restorable() &&
                 save.backup_sd_path ==
                     "/config/rommsync/.backup/101-retroarch-srm-1757004000.srm",
             "conflicts.db rc1: the save entry still points at its backup");
    c.Expect(read.entries[0].kind == rs::conflicts::EntryKind::kState &&
                 !read.entries[0].server_content_hash.has_value(),
             "conflicts.db rc1: the state entry");
  }
  if (rs::conflicts::kFormatVersion == kRc1Conflicts) {
    c.ExpectEq(rs::conflicts::SerializeHistory(read.entries, read.next_id), rc1,
               "conflicts.db: written differently under rc1's version -- bump kFormatVersion");
  }

  // Newer: the history is the only index to backups a later release made, and
  // it cannot be rebuilt. So it is not read, and it is not written over either:
  // an entry recorded now lives in memory for this boot and the file waits for
  // the release that wrote it.
  const fsys::path dir = Dir("conflicts");
  const fsys::path path = dir / rs::conflicts::kHistoryFileName;
  const std::string newer = Bumped(
      rc1, "rommsync-conflicts " + std::to_string(rs::conflicts::kFormatVersion) + " ",
      "rommsync-conflicts " + std::to_string(rs::conflicts::kFormatVersion + 1) + " ");
  Put(path, newer);
  rs::conflicts::History history(path.string());
  const std::vector<std::string> complaints = history.Load();
  c.Expect(history.entries().empty(), "conflicts.db newer: not read");
  c.Expect(Mentions(complaints, "newer"), "conflicts.db newer: and said so");

  rs::conflicts::Entry entry = read.entries.empty() ? rs::conflicts::Entry{} : read.entries[1];
  const rs::conflicts::StoreResult appended = history.Append(entry);
  c.Expect(appended.error == rs::conflicts::StoreError::kNewerFormat,
           std::string("conflicts.db newer: an append is refused on the card -- ") +
               rs::conflicts::ToString(appended.error));
  c.ExpectEq(history.entries().size(), std::size_t{1},
             "conflicts.db newer: and kept in memory, as any failed write is");
  c.ExpectEq(Slurp(path), newer, "conflicts.db newer: the file is byte for byte as it was");
}

// --- play.db: discard, both ways -----------------------------------------------------

void PlayDb(checks::Checks& c) {
  const std::string rc1 = Fixture("play_db");
  const rs::play::LoadedBuffer read = rs::play::ParseBuffer(rc1);
  c.Expect(read.diagnostics.empty(), "play.db rc1: no complaints");
  c.ExpectEq(read.next_id, std::int64_t{3}, "play.db rc1: the counter");
  c.ExpectEq(rs::sync::UnixSeconds(read.last_seen), std::int64_t{1757007700},
             "play.db rc1: when the saves were last looked at");
  c.ExpectEq(read.sessions.size(), std::size_t{2}, "play.db rc1: both sessions");
  if (read.sessions.size() == 2) {
    c.Expect(read.sessions[0].session.rom_id == std::int64_t{101} &&
                 read.sessions[0].session.duration_ms == 1800000 &&
                 !read.sessions[1].session.save_slot.has_value(),
             "play.db rc1: the sessions' values");
  }
  if (rs::play::kFormatVersion == kRc1Play) {
    c.ExpectEq(rs::play::SerializeBuffer(read.sessions, read.next_id, read.last_seen), rc1,
               "play.db: written differently under rc1's version -- bump kFormatVersion");
  }

  const rs::play::LoadedBuffer newer = rs::play::ParseBuffer(
      Bumped(rc1, "rommsync-play-sessions " + std::to_string(rs::play::kFormatVersion) + " ",
             "rommsync-play-sessions " + std::to_string(rs::play::kFormatVersion + 1) + " "));
  c.Expect(newer.sessions.empty() && !newer.diagnostics.empty(), "play.db newer: discarded");
}

// --- auth.json: discard, both ways ---------------------------------------------------

void AuthJson(checks::Checks& c) {
  const std::string rc1 = Fixture("auth_json");
  const rs::auth::LoadedBlock read = rs::auth::ParseBlock(rc1);
  c.Expect(read.value == rs::auth::Block::kRevoked && read.diagnostic.empty(),
           "auth.json rc1: the verdict");
  if (rs::auth::kAuthFormatVersion == kRc1Auth) {
    c.ExpectEq(rs::auth::SerializeBlock(read.value) + "\n", rc1,
               "auth.json: written differently under rc1's version -- bump kAuthFormatVersion");
  }
  const rs::auth::LoadedBlock newer = rs::auth::ParseBlock(
      Bumped(rc1, "\"version\":" + std::to_string(rs::auth::kAuthFormatVersion),
             "\"version\":" + std::to_string(rs::auth::kAuthFormatVersion + 1)));
  c.Expect(newer.value == rs::auth::Block::kNone && !newer.diagnostic.empty(),
           "auth.json newer: no verdict, re-derived by asking");
}

// --- token.dat: refuse newer, and never fall back past it ----------------------------

void TokenDat(checks::Checks& c) {
  const std::string rc1 = Fixture("token_dat");
  const fsys::path dir = Dir("token");
  const fsys::path path = dir / rs::auth::kTokenFileName;
  Put(path, rc1);
  const rs::auth::LoadedToken read = rs::auth::LoadToken(path.string());
  c.Expect(read.ok(), "token.dat rc1: read -- " + read.message);
  c.ExpectEq(read.value.server_url, std::string("https://romm.lan:8443/romm"),
             "token.dat rc1: the server it was issued by");
  c.ExpectEq(read.value.device_id, std::string("0f0e0d0c-0b0a-4908-8706-050403020100"),
             "token.dat rc1: the device");
  c.ExpectEq(read.value.scopes.size(), std::size_t{8}, "token.dat rc1: the scopes");
  c.Expect(!read.value.expires_at.has_value(), "token.dat rc1: no expiry");
  if (rs::auth::kTokenFormatVersion == kRc1Token) {
    c.ExpectEq(rs::auth::SerializeStoredToken(read.value) + "\n", rc1,
               "token.dat: written differently under rc1's version -- bump kTokenFormatVersion");
  }

  // Version 1 may also say so.
  c.Expect(rs::auth::ParseStoredToken(Bumped(rc1, "{", "{\"version\":1,")).ok(),
           "token.dat: 'version':1 spelled out is the same record");

  // Newer: the one file that cannot be re-fetched without a human at a browser.
  // Refused by name, not as "malformed"; not replaced by the `.old` beside it,
  // which would be an older credential standing in for the current one; and
  // left exactly as it was for the release that can read it.
  const std::string newer = Bumped(
      rc1, "{", "{\"version\":" + std::to_string(rs::auth::kTokenFormatVersion + 1) + ",");
  Put(path, newer);
  Put(rs::io::PreviousPathFor(path.string()), rc1);
  const rs::auth::LoadedToken refused = rs::auth::LoadToken(path.string());
  c.Expect(refused.error == rs::auth::StoreError::kNewerFormat,
           std::string("token.dat newer: refused as newer, not read from .old -- ") +
               rs::auth::ToString(refused.error));
  c.Expect(refused.message.find("newer") != std::string::npos &&
               refused.message.find("sys-rommsync") != std::string::npos,
           "token.dat newer: the message says what to update -- " + refused.message);
  c.Expect(refused.message.find("fixture-token-not-a-secret") == std::string::npos,
           "token.dat newer: and never quotes the token");
  c.ExpectEq(Slurp(path), newer, "token.dat newer: untouched");
  c.Expect(!rs::auth::ParseStoredToken(newer).ok(), "token.dat newer: the parser refuses it too");
}

// --- device.dat: read the identifier, whatever the version ---------------------------

void DeviceDat(checks::Checks& c) {
  const std::string rc1 = Fixture("device_dat");
  const rs::auth::DerivedIdentity read = rs::auth::ParseDeviceIdentity(rc1);
  c.Expect(read.ok(), "device.dat rc1: read -- " + read.message);
  c.ExpectEq(read.value.client_device_identifier,
             std::string("nx-0146100a95f974a905ace04e043df100"), "device.dat rc1: the identifier");
  c.Expect(read.value.source == rs::auth::IdentitySource::kDerived, "device.dat rc1: its source");
  c.ExpectEq(rs::auth::SerializeDeviceIdentity(read.value), rc1,
             "device.dat: written differently -- a later build must still read this identifier");

  // There is no version to refuse: the identifier is the contract, and refusing
  // a record over anything else would mint a new one and register the console
  // with RomM twice. A later record is read for it and not rewritten.
  const std::string newer =
      R"({"version":7,"client_device_identifier":"nx-0146100a95f974a905ace04e043df100",)"
      R"("source":"secure-element","minted_at":1900000000})";
  const fsys::path dir = Dir("device");
  const fsys::path path = dir / rs::auth::kDeviceIdentityFileName;
  Put(path, newer);
  rs::auth::IdentitySeed seed;
  seed.stable = "a different console entirely";
  const rs::auth::IdentityResult loaded = rs::auth::LoadOrCreateDeviceIdentity(path.string(), seed);
  c.Expect(loaded.ok() && !loaded.created, "device.dat newer: read, not minted over");
  c.ExpectEq(loaded.value.client_device_identifier, read.value.client_device_identifier,
             "device.dat newer: the same identifier");
  c.ExpectEq(Slurp(path), newer, "device.dat newer: untouched");
}

}  // namespace

int main() {
  checks::Checks c;
  ConfigIni(c);
  StateDb(c);
  QueueJson(c);
  ConflictsDb(c);
  PlayDb(c);
  AuthJson(c);
  TokenDat(c);
  DeviceDat(c);
  return c.failures() == 0 ? 0 : 1;
}
