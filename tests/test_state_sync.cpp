// Save states, which the server does not arbitrate.
//
// M2's other suites can lean on RomM to be right: negotiate hands back an
// `action` and the client obeys it. There is no negotiate for a state, so what
// these scenarios pin is the client's *own* policy -- and the one thing that
// policy must never do is what an obvious reading of `/api/states` would do,
// which is POST a local state over a server row that belongs to somebody else.
//
// Four need no server and must stay checked with docker stopped, because each
// pins a decision rather than a round trip:
//
//   silent    -- `sync.states = false` reads no directory and sends no request
//   naming    -- a state's backup name, which has no slot to be built from
//   scanning  -- an ambiguous state is skipped, and two states of one name are one
//   decides   -- the whole decision table, including every keep-both branch
//
// ...and five need the real RomM 5.2.0, because they are claims about the
// server that no fake can be wrong about:
//
//   upsert    -- POST /api/states replaces the row with that name, in place
//   roundtrip -- local -> server -> a second sandbox -> identical bytes
//   overwrite -- the server's copy moved: back up first, then replace
//   truncate  -- a clean short body never replaces a local state
//   keeps_both -- both sides moved: nothing is transferred, both survive
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "harness.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/state_sync.hpp"

namespace {

namespace auth = rommsync::auth;
namespace config = rommsync::config;
namespace crypto = rommsync::crypto;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace io = rommsync::io;
namespace json = rommsync::json;
namespace roms = rommsync::roms;
namespace scan = rommsync::scan;
namespace state = rommsync::state;
namespace sync = rommsync::sync;

using harness::Fixture;
using harness::PassASecond;
using harness::Sandbox;
using harness::StatePath;

constexpr std::int64_t kStamp = 1'757'000'000;

/// A config with states turned on and the default folder map, which already
/// maps `/retroarch/states` for every platform the client ships with.
config::Config StatesOn(bool on) {
  config::Config configured = config::Defaults();
  configured.sync.states = on;
  return configured;
}

/// An index holding one rom, matched by the base name of the state files below.
roms::RomIndex IndexOf(std::int64_t id, const std::string& base, const std::string& slug) {
  roms::RomIndex index;
  roms::Rom rom;
  rom.id = id;
  rom.fs_name_no_ext = base;
  rom.fs_name_no_tags = base;
  rom.platform_fs_slug = slug;
  index.Add(std::move(rom));
  return index;
}

auth::StoredToken TokenFor(const std::string& base, const Fixture& fixture) {
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  return token;
}

sync::StateSyncOptions OptionsAt(std::int64_t seconds) {
  sync::StateSyncOptions options;
  options.backup_dir = harness::kBackupDir;
  options.now = [seconds] { return sync::Timestamp{} + std::chrono::seconds{seconds}; };
  return options;
}

// --- the doubles the offline scenarios drive ---------------------------------

/// A `FileSystem` that counts what it was asked to read.
///
/// The count is the point: `silent` is a claim about calls that were *not* made,
/// and a scan that read the card and then discarded the result would look
/// identical from the outside.
class CountingFileSystem final : public fs::FileSystem {
 public:
  explicit CountingFileSystem(std::unique_ptr<fs::FileSystem> inner) : inner_(std::move(inner)) {}

  fs::Listing List(std::string_view sd_path) override {
    listed.push_back(std::string(sd_path));
    return inner_->List(sd_path);
  }

  std::string Resolve(std::string_view sd_path) const override { return inner_->Resolve(sd_path); }

  std::vector<std::string> listed;

 private:
  std::unique_ptr<fs::FileSystem> inner_;
};

/// An `HttpClient` that answers `/api/states` from a script, and records every
/// request so a scenario can assert on what was and was not sent.
class ScriptedClient final : public http::HttpClient {
 public:
  /// The `GET /api/states` body. A scenario sets it to arrange the server.
  std::string listing = "[]";

  /// What a `POST /api/states` answers with. Empty means "refuse".
  std::string upload_response;

  /// What a content download writes.
  std::string content;

  std::vector<std::string> requests;

  http::Result Send(const http::Request& request) override {
    requests.push_back(std::string(Verb(request.method)) + " " + Path(request.url));
    http::Result result;
    result.response.status = 200;
    if (request.method == http::Method::kGet) {
      result.response.body = listing;
      return result;
    }
    if (request.method == http::Method::kPost && !upload_response.empty()) {
      result.response.body = upload_response;
      return result;
    }
    result.response.status = 500;
    return result;
  }

  http::Result Download(const http::Request& request, const http::DownloadTarget& target) override {
    requests.push_back("GET " + Path(request.url));
    http::Result result;
    result.response.status = 200;
    if (!rig::WriteFile(target.path, content)) {
      result.error = http::Error::kWriteFailed;
      return result;
    }
    result.response.bytes_received = content.size();
    if (target.expected_size != 0 && target.expected_size != content.size()) {
      // What the native backend does with a body that ends early: the file is
      // not promoted, and the caller is told.
      std::remove(target.path.c_str());
      result.error = http::Error::kTruncated;
    }
    return result;
  }

  bool Sent(const std::string& what) const {
    return std::find(requests.begin(), requests.end(), what) != requests.end();
  }

  int Count(const std::string& verb) const {
    return static_cast<int>(std::count_if(requests.begin(), requests.end(),
                                          [&verb](const std::string& one) {
                                            return one.compare(0, verb.size(), verb) == 0;
                                          }));
  }

 private:
  static const char* Verb(http::Method method) {
    switch (method) {
      case http::Method::kGet:
        return "GET";
      case http::Method::kPost:
        return "POST";
      case http::Method::kPut:
        return "PUT";
      case http::Method::kDelete:
        return "DELETE";
      case http::Method::kHead:
        return "HEAD";
    }
    return "?";
  }

  /// The path and query of a URL, so an assertion names an endpoint rather than
  /// a port that changes per worktree.
  static std::string Path(const std::string& url) {
    const std::size_t scheme = url.find("://");
    const std::size_t slash =
        url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    return slash == std::string::npos ? url : url.substr(slash);
  }
};

/// One `StateSchema` row, as RomM writes it.
std::string StateRowJson(std::int64_t id, std::int64_t rom_id, const std::string& file_name,
                         const std::string& emulator, std::int64_t size,
                         const std::string& updated_at) {
  return "{\"id\":" + std::to_string(id) + ",\"rom_id\":" + std::to_string(rom_id) +
         ",\"user_id\":1,\"file_name\":" + json::Quote(file_name) +
         ",\"file_name_no_tags\":\"x\",\"file_name_no_ext\":\"x\",\"file_extension\":\"state\","
         "\"file_path\":\"users/1/states/nes/1\",\"file_size_bytes\":" +
         std::to_string(size) + ",\"full_path\":\"users/1/states/nes/1/x\",\"download_path\":\"/x\","
         "\"missing_from_fs\":false,\"created_at\":" + json::Quote(updated_at) +
         ",\"updated_at\":" + json::Quote(updated_at) +
         ",\"emulator\":" + (emulator.empty() ? "null" : json::Quote(emulator)) +
         ",\"is_public\":false,\"screenshot\":null}";
}

// --- silent -------------------------------------------------------------------

/// Off by default means *silent*: no directory is read and no request is sent.
///
/// Both halves are asserted, and then the same run with the toggle on is
/// asserted to do both -- otherwise the scenario would pass just as well against
/// a `SyncStates` that never worked at all.
void Silent(rig::Checks& checks) {
  Sandbox sandbox(checks, "states-silent");
  sandbox.Write(StatePath("Game.state"), "a snapshot of a core's memory");

  const roms::RomIndex index = IndexOf(1, "Game", "nes");
  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "not-used";

  {
    CountingFileSystem files(rommsync::host::MakeNativeFileSystem(sandbox.root().string()));
    ScriptedClient client;
    state::Baseline baseline;
    const sync::StateSyncReport report =
        sync::SyncStates(client, files, token, StatesOn(false), index, &baseline, OptionsAt(kStamp));

    checks.Expect(!report.ran, "with sync.states off the states half of a tick does not run");
    checks.ExpectEq(files.listed.size(), std::size_t{0},
                    "and nothing under StateScanDirs() was read");
    checks.ExpectEq(client.requests.size(), std::size_t{0},
                    "and no request reached /api/states");
    checks.Expect(baseline.state_rows().empty(), "and no state row was written");
  }

  {
    CountingFileSystem files(rommsync::host::MakeNativeFileSystem(sandbox.root().string()));
    ScriptedClient client;
    client.upload_response = StateRowJson(9, 1, "Game.state", "retroarch", 29,
                                          "2026-09-06T03:00:00+00:00");
    state::Baseline baseline;
    const sync::StateSyncReport report =
        sync::SyncStates(client, files, token, StatesOn(true), index, &baseline, OptionsAt(kStamp));

    checks.Expect(report.ran, "with it on, the states half runs");
    checks.Expect(!files.listed.empty(), "and it reads the state directories");
    checks.Expect(std::find(files.listed.begin(), files.listed.end(),
                            std::string("/retroarch/states")) != files.listed.end(),
                  "the flat RetroArch states folder among them");
    checks.Expect(client.Sent("GET /api/states"), "and asks the server what it holds");
    checks.ExpectEq(report.completed, 1, "the one local state is uploaded");
    checks.Expect(baseline.FindState(1, "Game.state") != nullptr,
                  "and it now has a row naming the server copy");
  }
}

// --- naming -------------------------------------------------------------------

/// A state has no slot, so the backup name has to be built from something else.
///
/// The failure this is written against is the one M2-5 found for saves, arriving
/// from the other side: `BackupFileName(rom, std::nullopt, ...)` spells
/// `archival`, so every state of one rom backed up in one second would be the
/// same name and only the uniquifier would separate them.
void Naming(rig::Checks& checks) {
  Sandbox sandbox(checks, "states-naming");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  const std::string first =
      sync::BackupNameFor(4, sync::StateBackupDiscriminator("Game (USA).state"),
                          "Game (USA).state", kStamp);
  const std::string second =
      sync::BackupNameFor(4, sync::StateBackupDiscriminator("Other Game.state"),
                          "Other Game.state", kStamp);
  checks.Expect(first != second,
                "two states of one rom in one second are two backups: " + first + " / " + second);
  checks.ExpectEq(first, std::string("4-state-Game__USA_-1757000000.state"),
                  "the name carries the state it came from");

  const std::string archival = sync::BackupFileName(4, std::nullopt, "Game (USA).state", kStamp);
  checks.Expect(first != archival,
                "and a state does not collide with an archival save of the same rom: " + first +
                    " / " + archival);

  // Off the card, so it may hold anything a filesystem allows. A separator would
  // put the backup somewhere other than `.backup/`.
  const std::string hostile = sync::BackupNameFor(
      4, sync::StateBackupDiscriminator("../../atmosphere/x.state"), "x.state", kStamp);
  checks.Expect(hostile.find('/') == std::string::npos,
                "a hostile state name cannot escape the backup directory: " + hostile);
  // A name whose base sanitises to one of the two a join treats as a directory
  // still needs a name that is a name.
  for (const char* degenerate : {"..", "."}) {
    const std::string discriminator = sync::StateBackupDiscriminator(degenerate);
    checks.ExpectEq(discriminator, std::string("state-state"),
                    std::string("a state called \"") + degenerate + "\" still gets a usable name");
  }

  // The uniquifier walk: a name already taken is a backup of something, so the
  // second copy steps past it rather than over it.
  sandbox.Write(StatePath("Game (USA).state"), "the bytes about to be replaced");
  std::string backup_path;
  std::string message;
  for (int at = 0; at < 2; ++at) {
    const sync::OperationError error = sync::BackUpFirst(
        *files, harness::kBackupDir, StatePath("Game (USA).state"), 4,
        sync::StateBackupDiscriminator("Game (USA).state"), "Game (USA).state", kStamp,
        &backup_path, &message);
    checks.Expect(error == sync::OperationError::kNone,
                  "the backup is written: " + message);
  }
  checks.ExpectEq(backup_path,
                  std::string(harness::kBackupDir) + "/4-state-Game__USA_-1757000000-1.state",
                  "the second backup in the same second is a second file");

  // Nothing at the path is success with nothing to show for it: a placement
  // overwrites nothing, so there is nothing to protect.
  backup_path.clear();
  const sync::OperationError absent =
      sync::BackUpFirst(*files, harness::kBackupDir, StatePath("Never (USA).state"), 4,
                        sync::StateBackupDiscriminator("Never (USA).state"), "Never (USA).state",
                        kStamp, &backup_path, &message);
  checks.Expect(absent == sync::OperationError::kNone, "a state that is not there is not a failure");
  checks.Expect(backup_path.empty(), "and no backup is claimed for it");
}

// --- scanning -----------------------------------------------------------------

/// The scan's two rules, over states: an ambiguous name is skipped rather than
/// guessed, and two files that RomM would store as one state are one state.
void Scanning(rig::Checks& checks) {
  Sandbox sandbox(checks, "states-scanning");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  config::Config configured = StatesOn(true);
  sandbox.MakeDirs("/tico/states/nes");
  sandbox.Write("/retroarch/states/Game.state", "the RetroArch copy");
  sandbox.Write("/tico/states/nes/Game.state", "the Tico copy, which is different");
  sandbox.Write("/retroarch/states/Twice.state", "a name two platforms both have");

  roms::RomIndex index;
  roms::Rom nes;
  nes.id = 1;
  nes.fs_name_no_ext = "Game";
  nes.fs_name_no_tags = "Game";
  nes.platform_fs_slug = "nes";
  index.Add(nes);
  // One name on two platforms, found in a folder that implies neither.
  for (const char* slug : {"gb", "gba"}) {
    roms::Rom rom;
    rom.id = slug[1] == 'b' ? 2 : 3;
    rom.fs_name_no_ext = "Twice";
    rom.fs_name_no_tags = "Twice";
    rom.platform_fs_slug = slug;
    index.Add(rom);
  }

  state::Baseline baseline;
  const scan::ScanResult result = scan::ScanStates(configured, index, *files, baseline);

  checks.ExpectEq(result.states.size(), std::size_t{1},
                  "one state is reported: " + result.DescribeSkipped());
  if (result.states.size() == 1) {
    checks.ExpectEq(result.states.front().file_name, std::string("Game.state"),
                    "and it is the first one in scan order");
    checks.ExpectEq(result.states.front().emulator, std::string("retroarch"),
                    "carrying the emulator its folder implied");
    checks.Expect(!result.states.front().content_hash.empty(), "and a digest of its bytes");
  }

  bool ambiguous = false;
  bool duplicate = false;
  for (const scan::Skip& skip : result.skipped) {
    ambiguous = ambiguous || skip.reason == scan::SkipReason::kAmbiguous;
    duplicate = duplicate || skip.reason == scan::SkipReason::kDuplicateName;
  }
  checks.Expect(ambiguous, "a name two platforms carry, in a hintless folder, is skipped");
  checks.Expect(duplicate,
                "and the second file of one (rom_id, file_name) is skipped as a duplicate name -- " +
                    result.DescribeSkipped());
}

// --- decides ------------------------------------------------------------------

/// The decision table in state_sync.cpp, every row of it.
///
/// Offline because none of it is a claim about RomM: it is what this client does
/// with a listing, a baseline and a file. The keep-both rows are the ones worth
/// having here -- they are the branches where an obvious implementation would
/// POST over somebody else's state, and no server would ever complain.
void Decides(rig::Checks& checks) {
  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "not-used";
  const roms::RomIndex index = IndexOf(1, "Game", "nes");
  const std::string kBytes = "a snapshot of a core's memory";
  const std::string kServerRow =
      StateRowJson(9, 1, "Game.state", "retroarch", 40, "2026-09-06T03:00:00+00:00");

  /// One run: a card with `local` in it, a server holding `listing`, and a
  /// baseline of `rows`.
  const auto run = [&](const std::string& name, const std::string& listing,
                       bool with_row, bool local_matches_row, bool row_matches_server,
                       sync::StateOutcome expected) {
    Sandbox sandbox(checks, "states-decides-" + name);
    const std::unique_ptr<fs::FileSystem> inner =
        rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedState(StatePath("Game.state"), kBytes);

    ScriptedClient client;
    client.listing = listing;
    client.upload_response = kServerRow;
    client.content = std::string(40, 'z');

    state::Baseline baseline;
    if (with_row) {
      // The row as the last sync left it. Its local half is read off the card so
      // "unchanged" is a fact rather than a guess.
      fs::Directories directories(*inner);
      state::FileFacts facts;
      std::string why;
      checks.Expect(state::ReadBackFile(*inner, directories, StatePath("Game.state"), &facts, &why),
                    name + ": the seeded state can be read back -- " + why);
      state::StateRecord row;
      row.rom_id = 1;
      row.file_name = "Game.state";
      row.emulator = "retroarch";
      row.content_hash =
          local_matches_row ? facts.content_hash : crypto::Md5Hex("some other bytes entirely");
      row.mtime = facts.mtime;
      row.file_size_bytes =
          local_matches_row ? facts.file_size_bytes : facts.file_size_bytes + 1;
      row.server_state_id = 9;
      row.server_updated_at = sync::Timestamp{} + std::chrono::seconds{
          row_matches_server ? 1'788'663'600 : 1'700'000'000};
      row.server_file_size_bytes = 40;
      baseline.SetState(row);
    }

    const sync::StateSyncReport report =
        sync::SyncStates(client, *inner, token, StatesOn(true), index, &baseline, OptionsAt(kStamp));
    checks.ExpectEq(report.operations.size(), std::size_t{1},
                    name + ": exactly one state was considered");
    if (report.operations.size() == 1) {
      checks.ExpectEq(std::string(sync::ToString(report.operations.front().outcome)),
                      std::string(sync::ToString(expected)), name);
    }
    return report;
  };

  // The server's `updated_at` in `kServerRow` is 2026-09-06T03:00:00Z.
  constexpr std::int64_t kServerSeconds = 1'788'663'600;
  (void)kServerSeconds;

  // No row, no server copy: upload. The only branch that writes to a name
  // nothing else claims.
  {
    const sync::StateSyncReport fresh =
        run("no history, nothing on the server", "[]", false, true, true,
            sync::StateOutcome::kUploaded);
    checks.ExpectEq(fresh.kept_both, 0, "an upload is not a keep-both");
  }

  // No row, and the server already holds that name. **This is the one that
  // matters**: POST would replace it, so nothing is sent.
  {
    const sync::StateSyncReport kept =
        run("no history, a server copy under the same name", "[" + kServerRow + "]", false, true,
            true, sync::StateOutcome::kKeptBoth);
    checks.ExpectEq(kept.kept_both, 1, "it is counted as a keep-both");
    checks.Expect(kept.operations.front().action == sync::StateAction::kKeepBoth,
                  "and decided as one");
  }

  // A row, and the server no longer has it: uploading puts it back, and is the
  // only way a state deleted elsewhere returns.
  run("a row the server no longer holds", "[]", true, true, true, sync::StateOutcome::kUploaded);

  // A row naming a different id than the one under that name now.
  run("a different row under the same name",
      "[" + StateRowJson(11, 1, "Game.state", "retroarch", 40, "2026-09-06T03:00:00+00:00") + "]",
      true, true, true, sync::StateOutcome::kKeptBoth);

  // Both sides where the last sync left them.
  run("neither side moved", "[" + kServerRow + "]", true, true, true, sync::StateOutcome::kNoOp);

  // Only the local file moved.
  run("only this console moved", "[" + kServerRow + "]", true, false, true,
      sync::StateOutcome::kUploaded);

  // Only the server moved: the one branch that may replace a local state.
  run("only the server moved", "[" + kServerRow + "]", true, true, false,
      sync::StateOutcome::kDownloaded);

  // Both moved, and RomM arbitrates neither.
  run("both moved", "[" + kServerRow + "]", true, false, false, sync::StateOutcome::kKeptBoth);
}

// --- the rig scenarios --------------------------------------------------------

/// `POST /api/states` with a name a rom already has.
///
/// The finding the whole policy is built on, and the one thing here no fake can
/// be wrong about: unlike `POST /api/saves`, which stores a second row, this
/// **replaces the row in place** -- same id, new bytes, and the `emulator` moved
/// to whatever the second request said.
void Upsert(rig::Checks& checks, http::HttpClient& client, const std::string& base,
            const Fixture& fixture, std::int64_t rom_id) {
  Sandbox sandbox(checks, "states-upsert");
  const std::string name = harness::UniqueSlot("upsert") + ".state";
  sandbox.Write(StatePath(name), "the first console's session");

  sync::ServerState first;
  if (!harness::UploadState(checks, client, base, fixture, rom_id, "retroarch",
                            sandbox.Host(StatePath(name)), name, &first)) {
    return;
  }

  sandbox.Write(StatePath(name), "a different console's session, and longer");
  sync::ServerState second;
  if (!harness::UploadState(checks, client, base, fixture, rom_id, "tico",
                            sandbox.Host(StatePath(name)), name, &second)) {
    harness::DeleteState(client, base, fixture, first.id);
    return;
  }

  checks.ExpectEq(second.id, first.id,
                  "a second POST under the same name is the same row, not a second one");
  checks.Expect(second.file_size_bytes != first.file_size_bytes,
                "and its bytes were replaced in place");

  const std::vector<sync::ServerState> rows =
      harness::ListStates(checks, client, base, fixture, rom_id);
  int matching = 0;
  for (const sync::ServerState& row : rows) {
    if (row.file_name == name) {
      ++matching;
      checks.ExpectEq(row.emulator, std::string("tico"),
                      "and the emulator moved with it, so it is not part of the key either");
    }
  }
  checks.ExpectEq(matching, 1, "there is exactly one row under that name");
  harness::DeleteState(client, base, fixture, first.id);
}

/// local -> server -> a second sandbox -> identical bytes.
void RoundTrip(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture, std::int64_t rom_id, const std::string& rom_base) {
  const auth::StoredToken token = TokenFor(base, fixture);
  const roms::RomIndex index = IndexOf(rom_id, rom_base, "gb");
  const std::string name = rom_base + ".state";
  const std::string bytes = "a session worth " + harness::UniqueSlot("keeping");

  std::int64_t uploaded_id = 0;
  {
    Sandbox first(checks, "states-roundtrip-a");
    const std::unique_ptr<fs::FileSystem> files =
        rommsync::host::MakeNativeFileSystem(first.root().string());
    first.Write(StatePath(name), bytes);

    state::Baseline baseline;
    const sync::StateSyncReport report =
        sync::SyncStates(client, *files, token, StatesOn(true), index, &baseline, OptionsAt(kStamp));
    checks.ExpectEq(report.failed, 0, "the first console's run had no failure");
    const state::StateRecord* row = baseline.FindState(rom_id, name);
    checks.Expect(row != nullptr, "and it recorded the server row it uploaded to");
    if (row == nullptr) {
      return;
    }
    uploaded_id = row->server_state_id;
    checks.ExpectEq(row->content_hash, crypto::Md5Hex(bytes),
                    "with the digest of the bytes on the card");
  }

  {
    Sandbox second(checks, "states-roundtrip-b");
    const std::unique_ptr<fs::FileSystem> files =
        rommsync::host::MakeNativeFileSystem(second.root().string());

    sync::StateSyncOptions options = OptionsAt(kStamp);
    // A state the console does not have needs a path, and nothing in the row
    // says which folder it belongs in -- that is the caller's, exactly as
    // `sync::ExecuteOptions::place` is.
    options.place = [](const sync::ServerState& server) {
      return StatePath(server.file_name);
    };

    state::Baseline baseline;
    const sync::StateSyncReport report =
        sync::SyncStates(client, *files, token, StatesOn(true), index, &baseline, options);
    checks.ExpectEq(report.failed, 0, "the second console's run had no failure");
    checks.Expect(second.Exists(StatePath(name)), "the state landed on the second card");
    checks.ExpectEq(second.Read(StatePath(name)), bytes, "byte for byte");
    checks.Expect(baseline.FindState(rom_id, name) != nullptr,
                  "and the second console recorded it too");
  }

  harness::DeleteState(client, base, fixture, uploaded_id);
}

/// The server's copy moved and this console's did not: back up first, then
/// replace. The `Sandbox` teardown audit judges the order whether or not this
/// scenario thought to look.
void Overwrite(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture, std::int64_t rom_id, const std::string& rom_base) {
  Sandbox sandbox(checks, "states-overwrite");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const auth::StoredToken token = TokenFor(base, fixture);
  const roms::RomIndex index = IndexOf(rom_id, rom_base, "gb");
  const std::string name = rom_base + ".state";
  const std::string mine = "the session this console had";
  const std::string theirs = "the session the other console pushed, which is longer";

  sandbox.SeedState(StatePath(name), mine);
  state::Baseline baseline;
  sync::StateSyncReport first =
      sync::SyncStates(client, *files, token, StatesOn(true), index, &baseline, OptionsAt(kStamp));
  checks.ExpectEq(first.failed, 0, "the first run uploaded this console's state");
  const state::StateRecord* row = baseline.FindState(rom_id, name);
  if (row == nullptr) {
    checks.Expect(false, "the first run recorded a row");
    return;
  }
  const std::int64_t state_id = row->server_state_id;

  // Another console moves the server's copy. A second later, so the timestamps
  // are distinguishable at the granularity RomM renders them at.
  PassASecond();
  const std::string elsewhere = sandbox.Host("/elsewhere.state");
  rig::WriteFile(elsewhere, theirs);
  sync::ServerState replaced;
  if (!harness::UploadState(checks, client, base, fixture, rom_id, "retroarch", elsewhere, name,
                            &replaced)) {
    harness::DeleteState(client, base, fixture, state_id);
    return;
  }

  const sync::StateSyncReport second =
      sync::SyncStates(client, *files, token, StatesOn(true), index, &baseline, OptionsAt(kStamp));
  checks.ExpectEq(second.failed, 0, "the second run had no failure");
  checks.ExpectEq(second.completed, 1, "and did one thing");
  if (second.operations.size() == 1) {
    checks.Expect(second.operations.front().outcome == sync::StateOutcome::kDownloaded,
                  std::string("it replaced the local state: ") +
                      sync::ToString(second.operations.front().outcome) + " -- " +
                      second.operations.front().message);
    checks.Expect(!second.operations.front().backup_sd_path.empty(),
                  "and named the backup it wrote first");
  }
  checks.ExpectEq(sandbox.Read(StatePath(name)), theirs, "the card holds the server's copy");
  checks.Expect(sandbox.HasBackupOf(mine), "and the previous bytes are under .backup/");

  // The warning every state download owes: this was checked against a length
  // and nothing more.
  bool said = false;
  for (const std::string& warning : second.warnings) {
    said = said || warning.find("not an integrity check") != std::string::npos;
  }
  checks.Expect(said, "and the report says a length match is not an integrity check");

  harness::DeleteState(client, base, fixture, state_id);
}

/// A clean short body never replaces a local state.
///
/// The only check a state download gets is its length, so this is the scenario
/// that proves the length is actually used -- `truncate` sends no
/// `Content-Length` at all, so nothing but `expected_size` can catch it.
void Truncate(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, std::int64_t rom_id, const std::string& rom_base) {
  Sandbox sandbox(checks, "states-truncate");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const auth::StoredToken token = TokenFor(base, fixture);
  const roms::RomIndex index = IndexOf(rom_id, rom_base, "gb");
  const std::string name = rom_base + ".state";
  const std::string mine = std::string(4096, 'm');

  sandbox.SeedState(StatePath(name), mine);
  state::Baseline baseline;
  sync::SyncStates(client, *files, token, StatesOn(true), index, &baseline, OptionsAt(kStamp));
  const state::StateRecord* row = baseline.FindState(rom_id, name);
  if (row == nullptr) {
    checks.Expect(false, "the first run recorded a row");
    return;
  }
  const std::int64_t state_id = row->server_state_id;

  PassASecond();
  const std::string elsewhere = sandbox.Host("/elsewhere.state");
  rig::WriteFile(elsewhere, std::string(8192, 't'));
  sync::ServerState replaced;
  if (!harness::UploadState(checks, client, base, fixture, rom_id, "retroarch", elsewhere, name,
                            &replaced)) {
    harness::DeleteState(client, base, fixture, state_id);
    return;
  }

  {
    harness::Fault fault(checks, client, base,
                         "{\"mode\":\"truncate\",\"bytes\":512,\"path\":\"/api/states/" +
                             std::to_string(state_id) + "/content\"}");
    const sync::StateSyncReport report =
        sync::SyncStates(client, *files, token, StatesOn(true), index, &baseline, OptionsAt(kStamp));
    checks.ExpectEq(report.completed, 0, "a truncated download completes nothing");
    checks.ExpectEq(report.failed, 1, "and is counted as a failure");
  }

  checks.ExpectEq(sandbox.Read(StatePath(name)), mine, "the local state is exactly as it was");
  checks.Expect(!io::Exists(io::TempPathFor(sandbox.Host(StatePath(name)))),
                "and the staged bytes were not left beside it");
  harness::DeleteState(client, base, fixture, state_id);
}

/// Both sides moved: nothing is transferred, and both copies survive.
void KeepsBoth(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture, std::int64_t rom_id, const std::string& rom_base) {
  Sandbox sandbox(checks, "states-keeps-both");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const auth::StoredToken token = TokenFor(base, fixture);
  const roms::RomIndex index = IndexOf(rom_id, rom_base, "gb");
  const std::string name = rom_base + ".state";
  const std::string mine = "the session this console had";

  sandbox.SeedState(StatePath(name), mine);
  state::Baseline baseline;
  sync::SyncStates(client, *files, token, StatesOn(true), index, &baseline, OptionsAt(kStamp));
  const state::StateRecord* row = baseline.FindState(rom_id, name);
  if (row == nullptr) {
    checks.Expect(false, "the first run recorded a row");
    return;
  }
  const std::int64_t state_id = row->server_state_id;

  // Both sides move: the other console pushes, and this one plays on.
  PassASecond();
  const std::string elsewhere = sandbox.Host("/elsewhere.state");
  rig::WriteFile(elsewhere, "the other console's session, which is longer");
  sync::ServerState replaced;
  if (!harness::UploadState(checks, client, base, fixture, rom_id, "retroarch", elsewhere, name,
                            &replaced)) {
    harness::DeleteState(client, base, fixture, state_id);
    return;
  }
  // The player carries on. `Reseed` rather than `Write`, so the teardown audit
  // judges the client against the bytes that are there now instead of demanding
  // a backup for an edit this test made itself.
  const std::string moved = mine + ", and then played on";
  sandbox.Reseed(StatePath(name), moved);

  const sync::StateSyncReport report =
      sync::SyncStates(client, *files, token, StatesOn(true), index, &baseline, OptionsAt(kStamp));
  checks.ExpectEq(report.kept_both, 1, "both sides moved, so both copies are kept");
  checks.ExpectEq(report.completed, 0, "and nothing was transferred");
  checks.ExpectEq(sandbox.Read(StatePath(name)), moved, "this console's state is untouched");

  const std::vector<sync::ServerState> rows =
      harness::ListStates(checks, client, base, fixture, rom_id);
  bool still_theirs = false;
  for (const sync::ServerState& one : rows) {
    still_theirs = still_theirs ||
                   (one.id == state_id && one.file_size_bytes == replaced.file_size_bytes);
  }
  checks.Expect(still_theirs, "and the server's is too");
  harness::DeleteState(client, base, fixture, state_id);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "silent";
  const std::string base = rig::BaseUrl();

  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);

  // Above every scenario, for `harness::Sandbox`'s reason: a scenario that
  // returned its own count would copy the number out before the teardown audit
  // ran.
  rig::Checks checks;

  if (scenario == "silent" || scenario == "naming" || scenario == "scanning" ||
      scenario == "decides") {
    if (scenario == "silent") {
      Silent(checks);
    } else if (scenario == "naming") {
      Naming(checks);
    } else if (scenario == "scanning") {
      Scanning(checks);
    } else {
      Decides(checks);
    }
    if (checks.failures() == 0) {
      std::cout << "states." << scenario << " ok\n";
    }
    return checks.failures() == 0 ? 0 : 1;
  }

  const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  rig::DisarmFault(*client, base);

  Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    return rig::kSkip;
  }

  harness::Rom rom;
  if (!harness::FindRom(*client, base, fixture, "gb240p.gb", &rom)) {
    std::cerr << "the fixture library holds no roms\n"
                 "  scan it with: ./.venv/bin/python server/testing/provision.py\n";
    return rig::kSkip;
  }
  // The base name every state file below is called, so the scan matches it back
  // to this rom the way it would on a card.
  const std::string rom_base = "gb240p";

  if (scenario == "upsert") {
    Upsert(checks, *client, base, fixture, rom.id);
  } else if (scenario == "roundtrip") {
    RoundTrip(checks, *client, base, fixture, rom.id, rom_base);
  } else if (scenario == "overwrite") {
    Overwrite(checks, *client, base, fixture, rom.id, rom_base);
  } else if (scenario == "truncate") {
    Truncate(checks, *client, base, fixture, rom.id, rom_base);
  } else if (scenario == "keeps_both") {
    KeepsBoth(checks, *client, base, fixture, rom.id, rom_base);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() == 0) {
    std::cout << "states." << scenario << " ok\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
