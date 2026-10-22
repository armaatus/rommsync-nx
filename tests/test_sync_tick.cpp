// One tick, interrupted every way a console interrupts one.
//
// M2-4, M2-5 and M2-6 each proved their own stage safe. What these prove is the
// thing between them: that a tick which loses the link, is switched off, or is
// answered with half a body leaves the card holding **either the saves it
// started with or a strictly completed subset of them**, and that the next tick
// picks the rest up (issue #16, docs/SYNC_PROTOCOL.md's failure & safety rules).
//
// Seven scenarios need no server and must stay checked with docker stopped --
// `recovery` is the sweep a crash makes necessary, `durable` is the platform
// `fsync` hard rule 2 depends on, `backupdir` is the directory `core/` could not
// create until this issue, `offline` is the tick that must write nothing at all,
// `canceled` is the shutdown that must not cost a request, and `disabled` is the
// enable switch that must cost nothing at all (M6-2, #33). The rest arm the
// fault proxy at one stage of a real tick and look at what is left behind --
// `relaunched` among them, which is the kill ovl-sysmodules performs.
//
// **Every rig scenario runs `sync::RunTick` inside a `harness::Sandbox`**, so the
// backup-before-overwrite audit judges it on teardown whether or not the
// scenario thought to look, and every one ends on `harness::ExpectDisarmed`.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "crash_disk.hpp"
#include "faulty_disk.hpp"
#include "harness.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/conflict_record.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/save_scan.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/scheduler.hpp"
#include "rommsync/server_capabilities.hpp"
#include "rommsync/sync_tick.hpp"

namespace {

namespace auth = rommsync::auth;
namespace conflicts = rommsync::conflicts;
namespace crypto = rommsync::crypto;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace io = rommsync::io;
namespace json = rommsync::json;
namespace server = rommsync::server;
namespace state = rommsync::state;
namespace sync = rommsync::sync;

using harness::Fixture;
using harness::Sandbox;
using harness::SavePath;

/// The token the engine authenticates with: the fixture's, pointed at the
/// proxy, so a scenario can damage one call and nothing else.
auth::StoredToken TokenFor(const std::string& base, const Fixture& fixture) {
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  return token;
}

/// What every tick here runs with: the sandbox's own `.backup/`, a clock that
/// does not move, and the backoff spent instantly.
///
/// A test that had to wait out three seconds of retry to prove there was one is
/// a test nobody runs -- and `CallPolicy::wait` being null means the *default*
/// sleep rather than none (sync.hpp), so it has to be set explicitly.
sync::TickOptions OptionsAt(std::int64_t unix_seconds) {
  sync::TickOptions options;
  options.execute.backup_dir = harness::kBackupDir;
  options.execute.now = [unix_seconds]() {
    return sync::Timestamp{} + std::chrono::seconds{unix_seconds};
  };
  options.negotiate.wait = [](std::chrono::milliseconds) {};
  options.negotiate.max_attempts = 2;
  options.finish.complete.wait = [](std::chrono::milliseconds) {};
  options.finish.complete.max_attempts = 2;
  // The save folder and `.backup/`, and deliberately not `/config/rommsync`
  // itself: those records recover from their own `.old` when they are read, and
  // the overlay writes `config.ini` from another thread (`RecoverStaging`).
  options.recover_dirs = {harness::kSavesDir, harness::kBackupDir};
  return options;
}

/// The same, with every ceiling short enough that a `stall` costs a second
/// rather than the default thirty. The stall scenarios are the only ones that
/// spend a timeout on purpose.
void Impatient(sync::TickOptions* options) {
  const std::chrono::milliseconds ceiling{1'500};
  options->negotiate.timeout = ceiling;
  options->execute.timeout = ceiling;
  options->execute.stall_timeout = ceiling;
  options->finish.complete.timeout = ceiling;
}

/// One local save as the scanner would report it: the card's own mtime and size,
/// and the digest of the bytes that are actually there.
bool LocalSaveOnCard(rig::Checks& checks, fs::FileSystem& files, const Sandbox& sandbox,
                     std::int64_t rom_id, const std::string& file_name, const std::string& slot,
                     sync::ClientSaveState* out) {
  const fs::Listing listing = files.List(harness::kSavesDir);
  if (!listing.ok()) {
    checks.Expect(false, "the saves folder lists: " + listing.message);
    return false;
  }
  for (const fs::Entry& entry : listing.entries) {
    if (entry.is_directory || entry.name != file_name) {
      continue;
    }
    out->rom_id = rom_id;
    out->file_name = file_name;
    out->slot = slot;
    out->emulator = "m2-7";
    out->content_hash = crypto::Md5Hex(sandbox.Read(SavePath(file_name)));
    out->updated_at = sync::Timestamp{} + std::chrono::seconds{entry.modified_unix};
    out->file_size_bytes = entry.size_bytes;
    return true;
  }
  checks.Expect(false, "the seeded save is on the card at " + SavePath(file_name));
  return false;
}

/// Nothing an interrupted transfer stages is still beside `sd_path`.
///
/// A *handled* failure clears its own staging (`execute.dropped`); what these
/// scenarios add is that the tick as a whole leaves none either, so the next one
/// has nothing to reason about.
void NoLeftovers(rig::Checks& checks, const Sandbox& sandbox, const std::string& sd_path,
                 const std::string& what) {
  checks.Expect(!sandbox.Exists(http::PartialPathFor(io::TempPathFor(sd_path))),
                what + ": no partial body beside " + sd_path);
  checks.Expect(!sandbox.Exists(io::TempPathFor(sd_path)),
                what + ": no staged copy beside " + sd_path);
  checks.Expect(!sandbox.Exists(io::PreviousPathFor(sd_path)),
                what + ": no half-finished commit beside " + sd_path);
}

/// How many save rows the server holds for `(rom_id, slot)`.
///
/// The acceptance the duplication hazard earns: a re-posted upload is a *second*
/// row, because RomM matches an existing one by a second-granularity datetime
/// tag it computes at ingest (docs/API_CONTRACT.md, issue #85). A tick that
/// retried inside itself would show up here as a 2.
int SaveRowsFor(http::HttpClient& client, const std::string& base, const Fixture& fixture,
                std::int64_t rom_id, const std::string& slot, std::vector<std::int64_t>* ids) {
  const http::Result listed = client.Send(harness::Authed(
      http::Method::kGet, base + "/api/saves?rom_id=" + std::to_string(rom_id), fixture));
  const json::ParseResult parsed = json::Parse(listed.response.body);
  if (!listed.successful() || !parsed.ok() || !parsed.value.is_array()) {
    return -1;
  }
  int rows = 0;
  for (const json::Value& save : parsed.value.elements()) {
    if (harness::Field(save, "slot") != slot) {
      continue;
    }
    ++rows;
    if (ids != nullptr) {
      ids->push_back(harness::Number(save, "id"));
    }
  }
  return rows;
}

/// Everything under the sandbox: path -> bytes for a file, and a marker for a
/// directory. What "wrote nothing at all" is checked against.
///
/// Directories are in it because one of the things an offline tick must not do
/// is *create* one -- `RunTick` makes `.backup/` on entry, and a snapshot of
/// files alone could not tell that apart from nothing having happened.
std::map<std::string, std::string> Snapshot(const Sandbox& sandbox) {
  std::map<std::string, std::string> tree;
  std::error_code error;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::recursive_directory_iterator(sandbox.root(), error)) {
    const std::string name =
        std::filesystem::relative(entry.path(), sandbox.root(), error).string();
    if (entry.is_directory(error)) {
      tree[name] = "<directory>";
    } else if (entry.is_regular_file(error)) {
      tree[name] = rig::ReadFile(entry.path().string());
    }
  }
  return tree;
}

// --- recovery -----------------------------------------------------------------
//
// What a *crash* leaves beside a save, and the one correct answer for each. A
// handled failure clears its own staging; this is the case where no cleanup got
// to run at all.

void Recovery(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-recovery");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  // A save with both halves of an interrupted download beside it. The bytes in
  // the `.tmp` are deliberately *not* the save's: a crash can leave one staged
  // before anything verified it, which is the whole reason it is discarded
  // rather than committed.
  const std::string kept = "the save the card already had\n";
  sandbox.SeedSave(SavePath("kept.srm"), kept);
  sandbox.Write(io::TempPathFor(SavePath("kept.srm")), "an unverified body\n");
  sandbox.Write(http::PartialPathFor(io::TempPathFor(SavePath("kept.srm"))), "half an unverified body");

  // A save whose commit was interrupted between its two renames: the file is
  // *missing* and `.old` is the only copy of it. This is the one leftover that
  // must never be deleted.
  const std::string parked = "the only copy, moved aside\n";
  sandbox.SeedSave(SavePath("parked.srm"), parked);
  std::filesystem::rename(sandbox.Host(SavePath("parked.srm")),
                          sandbox.Host(io::PreviousPathFor(SavePath("parked.srm"))));

  // A commit that finished and did not tidy up: the new bytes are in place, the
  // previous ones are under `.backup/` where the overwrite put them, and the
  // `.old` beside the save is the copy that is now redundant -- and is *still*
  // left alone, because a `Game.srm.old` a human made by hand has exactly this
  // shape and this code does not delete files it did not write.
  const std::string before = "what this save used to hold\n";
  const std::string after = "what the server sent\n";
  sandbox.SeedSave(SavePath("moved.srm"), before);
  sandbox.Write(SavePath("moved.srm"), after);
  sandbox.Write(io::PreviousPathFor(SavePath("moved.srm")), before);
  const std::string backup = std::string(harness::kBackupDir) + "/7-m2-7-1757000000.srm";
  sandbox.Write(backup, before);

  // A rom download's partial, in a folder of its own. `DownloadTarget::resume`
  // is true for those (M3-3), so a gigabyte of one is progress rather than
  // litter -- and a sweep that took `.part` on its own would throw it away.
  sandbox.MakeDirs("/roms/gb");
  sandbox.Write("/roms/gb/big.gb.part", "480 MiB in, and resumable\n");

  const sync::RecoveryReport report = sync::RecoverStaging(
      *files, {harness::kSavesDir, harness::kBackupDir, "/roms/gb", "/never/created"});

  checks.Expect(report.warnings.empty(),
                "the sweep had nothing to complain about: " +
                    (report.warnings.empty() ? std::string() : report.warnings[0]));
  checks.ExpectEq(report.partials_removed, static_cast<std::size_t>(1),
                  "the interrupted body was removed");
  checks.ExpectEq(report.staged_removed, static_cast<std::size_t>(1),
                  "and so was the staged copy nothing had verified");
  checks.ExpectEq(report.saves_restored, static_cast<std::size_t>(1),
                  "the parked save was put back, because it was the only copy");
  checks.ExpectEq(report.previous_left, static_cast<std::size_t>(1),
                  "and the redundant one beside a finished commit was counted and left: it is "
                  "indistinguishable from a backup a user made by hand");

  checks.ExpectEq(sandbox.Read(SavePath("kept.srm")), kept,
                  "the save the staging sat beside is untouched");
  NoLeftovers(checks, sandbox, SavePath("kept.srm"), "after the sweep");

  checks.ExpectEq(sandbox.Read(SavePath("parked.srm")), parked,
                  "the interrupted commit was finished in the direction that keeps the save");
  checks.Expect(!sandbox.Exists(io::PreviousPathFor(SavePath("parked.srm"))),
                "and nothing is left under the previous name");

  checks.ExpectEq(sandbox.Read(SavePath("moved.srm")), after,
                  "the finished commit's own bytes stand");
  checks.ExpectEq(sandbox.Read(io::PreviousPathFor(SavePath("moved.srm"))), before,
                  "and the file beside it is untouched, bytes and all");

  // The rule the sweep must never break: a backup is the copy M7-1 restores
  // from, including one written for an overwrite that then failed.
  checks.ExpectEq(sandbox.Read(backup), before, "the backup is not the sweep's to touch");

  checks.Expect(sandbox.Exists("/roms/gb/big.gb.part"),
                "a rom download's partial survives: it is resumable progress, not litter");

  // Idempotent, because a tick runs this every time and a card that comes back
  // from a bad week runs it twice in a minute.
  const sync::RecoveryReport again = sync::RecoverStaging(*files, {harness::kSavesDir});
  checks.ExpectEq(again.total(), static_cast<std::size_t>(0),
                  "a second sweep acts on nothing, having left nothing to act on");
  checks.ExpectEq(again.previous_left, static_cast<std::size_t>(1),
                  "...and reports the one file it will never act on, every time");
}

// --- durable ------------------------------------------------------------------
//
// The platform hook hard rule 2 depends on. `io::CopyAtomically` is how a save's
// previous bytes reach `.backup/`, and until this issue the rename that
// published them could be durable while the bytes themselves were not -- a
// backup that reads as present and holds nothing.

/// A `FileSync` that records the sequence: which path, and whether the file the
/// commit is about to publish already existed at that moment.
///
/// The second half is what proves the ordering, and it is the whole point of the
/// hook: the bytes have to be synced *before* the rename publishes them, so on
/// the first call the destination must not be there yet.
std::vector<std::string> g_synced;
std::vector<bool> g_published;
std::string g_watched;
bool g_refuse = false;

bool CountingFileSync(const std::string& path) {
  g_synced.push_back(path);
  g_published.push_back(!g_watched.empty() && io::Exists(g_watched));
  // The real `fsync` is `InstallPosixFileSync`'s and is put back below; what
  // this stands in for is the *answer*, which is the only thing the writers
  // act on.
  return !g_refuse;
}

void Durable(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-durable");
  const std::string save = sandbox.Host(SavePath("durable.srm"));
  const std::string backup = sandbox.Host(std::string(harness::kBackupDir) + "/9-slot-1.srm");
  const std::string bytes = "the bytes a power cut must not lose\n";
  sandbox.Write(SavePath("durable.srm"), bytes);

  const io::FileSync installed = io::GetFileSync();
  g_synced.clear();
  g_published.clear();
  g_watched = backup;
  g_refuse = false;
  io::SetFileSync(&CountingFileSync);

  const io::CopyResult copied = io::CopyAtomically(save, backup);
  checks.Expect(copied.ok(), "the backup was written: " + copied.message);
  checks.ExpectEq(g_synced.size(), static_cast<std::size_t>(2),
                  "durability is two calls, not one: the bytes, and then the name");
  if (g_synced.size() == 2) {
    checks.ExpectEq(g_synced[0], io::TempPathFor(backup),
                    "the staged copy is synced first, which is the file the rename publishes");
    checks.Expect(!g_published[0],
                  "before that rename, which is the whole ordering: a backup whose name landed "
                  "and whose data did not is the state hard rule 2 rules out");
    checks.ExpectEq(g_synced[1], std::string(sandbox.Host(harness::kBackupDir)),
                    "and the directory second, because on POSIX a rename is not durable until "
                    "the directory holding the new name is");
    checks.Expect(g_published[1], "which can only be after it -- there is a name to make durable");
  }
  checks.ExpectEq(rig::ReadFile(backup), bytes, "and the backup holds the save's bytes");

  // A hook that refuses is a card that would not take the bytes, and a backup
  // that did not happen must stop the overwrite rather than be counted as one.
  const std::string second = sandbox.Host(std::string(harness::kBackupDir) + "/9-slot-2.srm");
  g_refuse = true;
  g_watched.clear();
  const io::CopyResult refused = io::CopyAtomically(save, second);
  checks.Expect(refused.error == io::CopyError::kWriteFailed,
                std::string("a refused sync fails the copy: ") + io::ToString(refused.error));
  checks.Expect(!io::Exists(second), "and leaves no backup behind to be mistaken for one");
  checks.Expect(!io::Exists(io::TempPathFor(second)), "nor the staging it was written through");

  // The same seam for the small records `WriteAtomically` owns -- `state.db` and
  // `token.dat` -- rather than a second one beside it.
  g_refuse = false;
  g_synced.clear();
  g_published.clear();
  const io::WriteResult written =
      io::WriteAtomically(sandbox.Host("/config/rommsync/durable.txt"), "a small record\n");
  checks.Expect(written.ok(), "a small record is written: " + written.message);
  checks.ExpectEq(g_synced.size(), static_cast<std::size_t>(2),
                  "through the same hook and the same two calls, not a second copy of either");

  io::SetFileSync(installed);
  checks.Expect(io::GetFileSync() != nullptr,
                "the host installs a real one from main, so a test can put it back");
}

// --- backupdir ----------------------------------------------------------------
//
// `sdmc:/config/rommsync/.backup/` missing is every download failing with
// `kBackupFailed` -- correctly, because no backup means no overwrite. Nothing in
// `core/` could create it before this issue, so on a first boot the client was
// waiting for a directory nobody made.

void BackupDir(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-backupdir");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  // The sandbox makes `.backup/` for every other scenario, so this one has to
  // take it away again to be looking at a first boot at all.
  std::error_code error;
  std::filesystem::remove_all(sandbox.Host(harness::kBackupDir), error);
  checks.Expect(!sandbox.Exists(harness::kBackupDir), "the card starts without a .backup/");

  const fs::MakeDirResult made = files->CreateDirectory(harness::kBackupDir);
  checks.Expect(made.ok(), "the platform layer creates it: " + made.message);
  checks.Expect(sandbox.Exists(harness::kBackupDir), "and it is there afterwards");

  // Every tick runs this, so "already there" has to be success rather than a
  // failure a caller learns to ignore.
  const fs::MakeDirResult again = files->CreateDirectory(harness::kBackupDir);
  checks.Expect(again.ok(), "a directory that already exists is success: " + again.message);

  // Nested, because `/config/rommsync/.backup` is two levels of a path a card
  // that has never run this client has neither of.
  std::filesystem::remove_all(sandbox.Host(harness::kConfigDir), error);
  const fs::MakeDirResult nested = files->CreateDirectory(harness::kBackupDir);
  checks.Expect(nested.ok(), "and so are the parents it needs: " + nested.message);
  checks.Expect(sandbox.Exists(harness::kBackupDir), "the whole path is there");

  // A file in the way is named rather than silently succeeding, because the next
  // thing to happen would be a backup written into it.
  sandbox.Write("/config/rommsync/occupied", "not a directory\n");
  const fs::MakeDirResult blocked = files->CreateDirectory("/config/rommsync/occupied");
  checks.Expect(blocked.error == fs::MakeDirError::kNotADirectory,
                std::string("a file where the directory should be is named: ") +
                    fs::ToString(blocked.error));

  // A path off this card is a refusal, not a directory made somewhere else.
  const fs::MakeDirResult escaped = files->CreateDirectory("/config/../../elsewhere");
  checks.Expect(escaped.error == fs::MakeDirError::kNotOnThisCard,
                std::string("a path that escapes the card is refused: ") +
                    fs::ToString(escaped.error));
}

// --- offline ------------------------------------------------------------------
//
// The acceptance this issue is sharpest about: with RomM unreachable a tick
// writes **nothing at all** -- no backup, no `.part`, no `state.db` rewrite --
// and the missed tick costs nothing. A blocked boot would.

/// An `HttpClient` that answers nothing and counts what it was asked.
class CountingClient final : public http::HttpClient {
 public:
  http::Result Send(const http::Request&) override {
    ++requests;
    http::Result result;
    result.error = http::Error::kConnectFailed;
    result.message = "there is no server in this scenario";
    return result;
  }

  http::Result Download(const http::Request& request, const http::DownloadTarget&) override {
    return Send(request);
  }

  int requests = 0;
};

/// A port on loopback with nothing behind it. Not a name that could resolve to
/// something real: `policy.loopback_only` exists because a test that reaches a
/// live host is the one thing this project may never do (CLAUDE.md hard rule 1).
constexpr const char* kNowhere = "http://127.0.0.1:1";

void Offline(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-offline");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  const std::string bytes = "the save an offline tick must not touch\n";
  sandbox.SeedSave(SavePath("offline.srm"), bytes);

  // A baseline already on the card, so "state.db was not rewritten" is a claim
  // about a file that exists rather than about one that never did.
  state::Baseline baseline;
  state::SaveRecord row;
  row.rom_id = 42;
  row.slot = "m2-7-offline";
  row.content_hash = crypto::Md5Hex(bytes);
  row.file_size_bytes = static_cast<std::int64_t>(bytes.size());
  row.mtime = sync::Timestamp{} + std::chrono::seconds{1'757'000'000};
  baseline.Set(row);
  const state::StoreResult seeded =
      state::SaveBaseline(files->Resolve(sync::kStateSdPath), baseline);
  checks.Expect(seeded.ok(), "the baseline was seeded: " + seeded.message);

  // The sandbox makes `.backup/` for every other scenario. Taking it away is what
  // makes "no directory was created either" a claim rather than a coincidence:
  // `RunTick` creates that folder, and it does so only *after* a negotiation.
  std::error_code error;
  std::filesystem::remove_all(sandbox.Host(harness::kBackupDir), error);

  const std::map<std::string, std::string> before = Snapshot(sandbox);

  auth::StoredToken token;
  token.server_url = kNowhere;
  token.access_token = "not-a-token";
  token.device_id = "fixture-offline";

  sync::ClientSaveState local;
  if (!LocalSaveOnCard(checks, *files, sandbox, 42, "offline.srm", "m2-7-offline", &local)) {
    return;
  }
  const std::vector<sync::SaveTarget> targets = {
      {42, std::string("m2-7-offline"), SavePath("offline.srm"), "offline.srm"}};

  sync::TickOptions options = OptionsAt(1'757'000'100);
  Impatient(&options);
  const sync::TickResult tick =
      sync::RunTick(*rig::MakeClient(), *files, token, {local}, targets,
                    baseline, options);

  checks.Expect(tick.outcome == sync::TickOutcome::kOffline,
                std::string("an unreachable server is an offline tick, not a refused one: ") +
                    sync::ToString(tick.outcome));
  checks.Expect(tick.negotiated.error == sync::NegotiateError::kUnreachable,
                std::string("named by the stage that met it: ") +
                    sync::ToString(tick.negotiated.error));
  checks.Expect(tick.answer == auth::Answer::kSilent,
                "and it says nothing about the token: an offline tick is no evidence that one "
                "still works");
  checks.ExpectEq(tick.executed.operations.size(), static_cast<std::size_t>(0),
                  "no operation was attempted");
  checks.ExpectEq(tick.finished.reported.attempts, 0,
                  "and the accounting call was never made: there was no session to close");

  // The claim, made against the card rather than against the report. Directories
  // are in the snapshot too, so "`.backup/` was not created" is part of it.
  const std::map<std::string, std::string> after = Snapshot(sandbox);
  checks.Expect(before == after,
                "the tick left the card byte-for-byte as it found it: no backup, no partial, no "
                "state.db rewrite");
  NoLeftovers(checks, sandbox, SavePath("offline.srm"), "after an offline tick");
  checks.ExpectEq(sandbox.Read(SavePath("offline.srm")), bytes, "and the save is what it was");
  checks.Expect(!sandbox.Exists(harness::kBackupDir),
                "not even the backup directory: nothing needs one until an operation does, and "
                "this tick never reached the server");

  // The other half of the same reading, and the one the acceptance does not
  // spell out: an offline tick still *sweeps*. It runs before the network and
  // only removes litter, so it is right to do on a tick that turns out to have
  // nowhere to go -- and it still writes no backup and does not rewrite the
  // baseline.
  sandbox.Write(http::PartialPathFor(io::TempPathFor(SavePath("offline.srm"))), "half a body");
  const std::string baseline_before = sandbox.Read(sync::kStateSdPath);

  const sync::TickResult sweeping =
      sync::RunTick(*rig::MakeClient(), *files, token, {local}, targets,
                    baseline, options);
  checks.Expect(sweeping.outcome == sync::TickOutcome::kOffline,
                std::string("the second offline tick is offline too: ") +
                    sync::ToString(sweeping.outcome));
  checks.ExpectEq(sweeping.recovered.partials_removed, static_cast<std::size_t>(1),
                  "and it swept the interrupted body on its way in");
  checks.Expect(!sandbox.HasBackupOf(bytes),
                "with no backup written, because nothing was overwritten");
  checks.ExpectEq(sandbox.Read(sync::kStateSdPath), baseline_before,
                  "and the baseline is byte-for-byte what it was");
}

// --- rescan -------------------------------------------------------------------
//
// The one thing the sweep does that the *rest* of the tick cannot survive: it
// puts back a save the scan could not have seen. Step 0 ran before this call --
// its output is the argument -- so negotiating with it would report the restored
// save as absent, and RomM answers "absent" by planning a download of its own
// copy over it. The client would have decided a conflict by leaving a save out
// of the report, which is the one thing it may never do.

void Rescan(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-rescan");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  const std::string parked = "the only copy, moved aside mid-commit\n";
  sandbox.SeedSave(SavePath("parked.srm"), parked);
  std::error_code moved;
  std::filesystem::rename(sandbox.Host(SavePath("parked.srm")),
                          sandbox.Host(io::PreviousPathFor(SavePath("parked.srm"))), moved);

  auth::StoredToken token;
  token.server_url = kNowhere;
  token.access_token = "not-a-token";
  token.device_id = "fixture-rescan";

  sync::TickOptions options = OptionsAt(1'757'000'150);
  Impatient(&options);

  // A client that would answer every request, so "no request was made" is the
  // tick's decision rather than the network's.
  CountingClient client;
  const sync::TickResult tick =
      sync::RunTick(client, *files, token, {}, {}, /*previous=*/{}, options);

  checks.Expect(tick.outcome == sync::TickOutcome::kRescanNeeded,
                std::string("a restored save stops the tick rather than negotiating without it: ") +
                    sync::ToString(tick.outcome));
  checks.ExpectEq(client.requests, 0,
                  "before any request, so nothing was planned against a stale report");
  checks.ExpectEq(tick.recovered.saves_restored, static_cast<std::size_t>(1),
                  "and the save is the thing that was put back");
  checks.ExpectEq(sandbox.Read(SavePath("parked.srm")), parked, "it is on the card again");
  checks.Expect(!sandbox.Exists(io::PreviousPathFor(SavePath("parked.srm"))),
                "and nothing is left under the previous name");
  checks.Expect(!sandbox.HasBackupOf(parked),
                "with no backup, because nothing was overwritten to need one");

  // The next tick, with the scan redone -- here, simply a sweep that finds
  // nothing left to restore. It gets past the sweep and fails on the network,
  // which is the point: the stop is about the report being stale, not about
  // there being something wrong with the card.
  const sync::TickResult next =
      sync::RunTick(client, *files, token, {}, {}, /*previous=*/{}, options);
  checks.Expect(next.outcome == sync::TickOutcome::kOffline,
                std::string("a rescanned tick runs: ") + sync::ToString(next.outcome));
  checks.Expect(client.requests > 0, "and reaches the network this time");
}

// --- canceled -----------------------------------------------------------------
//
// The shutdown. `ExecuteOptions::cancel` has been checked between operations
// since M2-5 and `CallPolicy::cancel` on both API calls since M2-6; what a tick
// adds is that one token reaches all three, and that a token already fired costs
// no request at all -- on the link whose loss is usually why the shutdown
// happened.

void Canceled(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-canceled");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string bytes = "a save a shutdown must not touch\n";
  sandbox.SeedSave(SavePath("canceled.srm"), bytes);

  // A leftover, so "recovery did not run either" is checkable. A cancelled tick
  // stops before the sweep: the caller asked for nothing to happen.
  sandbox.Write(http::PartialPathFor(io::TempPathFor(SavePath("canceled.srm"))), "half a body");

  auth::StoredToken token;
  token.server_url = kNowhere;
  token.access_token = "not-a-token";
  token.device_id = "fixture-canceled";

  http::CancelToken cancel;
  cancel.Cancel();

  sync::TickOptions options = OptionsAt(1'757'000'200);
  options.cancel = &cancel;

  CountingClient client;
  const sync::TickResult stopped =
      sync::RunTick(client, *files, token, {}, {}, /*previous=*/{}, options);

  checks.Expect(stopped.outcome == sync::TickOutcome::kCanceled,
                std::string("a token already fired ends the tick before it starts: ") +
                    sync::ToString(stopped.outcome));
  checks.ExpectEq(client.requests, 0,
                  "and costs no request -- three timeouts plus two backoffs is what this saves");
  checks.ExpectEq(stopped.recovered.total(), static_cast<std::size_t>(0),
                  "nothing was swept either: the caller asked for nothing to happen");
  checks.Expect(sandbox.Exists(http::PartialPathFor(io::TempPathFor(SavePath("canceled.srm")))),
                "so the leftover is still there for the next tick to deal with");

  // The same tick with the token unfired reaches the network, which is what
  // makes the count above mean something.
  http::CancelToken live;
  sync::TickOptions running = OptionsAt(1'757'000'201);
  running.cancel = &live;
  CountingClient reachable;
  const sync::TickResult tried =
      sync::RunTick(reachable, *files, token, {}, {}, /*previous=*/{}, running);
  checks.Expect(reachable.requests > 0, "an unfired token does not stop the tick");
  checks.Expect(tried.outcome == sync::TickOutcome::kOffline,
                std::string("it is the server that is missing, not the caller: ") +
                    sync::ToString(tried.outcome));
  checks.ExpectEq(tried.recovered.partials_removed, static_cast<std::size_t>(1),
                  "and this one did sweep the leftover on its way in");
}

// --- disabled -----------------------------------------------------------------
//
// The enable switch, which is not the boot toggle (M6-2, #33,
// docs/DEVELOPMENT.md#the-two-switches). With `[sync] enabled = false` the
// process is resident and IPC answers; what must not happen is a request, a
// file, or a sweep.
//
// The scheduler that owns the interval parks itself when the switch is off, and
// that scheduler is M7-2's -- so the promise would be untestable until it exists
// if it lived only there. `TickOptions::enabled` is the same gate one level
// down, where `download::Drain` already has it, and this is what makes "a
// disabled sysmodule is genuinely idle" a fact about the tick rather than about
// a caller remembering to ask.

void Disabled(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-disabled");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string bytes = "a save nothing may touch while sync is off\n";
  sandbox.SeedSave(SavePath("disabled.srm"), bytes);

  // A leftover, for the same reason `Canceled` plants one: "no sweep either" is
  // only checkable if there was something to sweep. A tick that tidied the card
  // while switched off would be a disabled sysmodule doing work.
  const std::string partial = http::PartialPathFor(io::TempPathFor(SavePath("disabled.srm")));
  sandbox.Write(partial, "half a body");

  auth::StoredToken token;
  token.server_url = kNowhere;
  token.access_token = "not-a-token";
  token.device_id = "fixture-disabled";

  const std::map<std::string, std::string> before = Snapshot(sandbox);

  sync::TickOptions options = OptionsAt(1'757'000'300);
  options.enabled = false;

  CountingClient client;
  const sync::TickResult parked =
      sync::RunTick(client, *files, token, {}, {}, /*previous=*/{}, options);

  checks.Expect(parked.outcome == sync::TickOutcome::kDisabled,
                std::string("[sync] enabled = false parks the tick: ") +
                    sync::ToString(parked.outcome));
  checks.ExpectEq(client.requests, 0,
                  "and it costs zero HTTP requests -- the whole of what the switch promises");
  checks.ExpectEq(parked.recovered.total(), static_cast<std::size_t>(0),
                  "no sweep ran: a disabled sysmodule does no work at all");
  checks.Expect(sandbox.Exists(partial), "so the leftover is exactly where it was");
  checks.Expect(Snapshot(sandbox) == before,
                "and nothing under the sandbox changed -- not a file, not a directory");

  // Switching it back on runs the tick, with no restart of anything: the same
  // objects, the same card, one field. That is the other half of the criterion
  // -- a switch that cannot be switched back is not a switch.
  sync::TickOptions running = OptionsAt(1'757'000'301);
  CountingClient reachable;
  const sync::TickResult resumed =
      sync::RunTick(reachable, *files, token, {}, {}, /*previous=*/{}, running);
  checks.Expect(reachable.requests > 0,
                "with enabled back on the tick reaches the network again");
  checks.Expect(resumed.outcome == sync::TickOutcome::kOffline,
                std::string("and it is the missing server that stops it, not the switch: ") +
                    sync::ToString(resumed.outcome));
  checks.ExpectEq(resumed.recovered.partials_removed, static_cast<std::size_t>(1),
                  "and this one did sweep the leftover the parked tick left alone");
}

/// M10-2 (#228): a server below `server::kMinimumVersion` is refused before
/// negotiate, by the tick itself and not only by the engine that read the
/// version -- the treatment `enabled` gets, for its reason: a second caller
/// that forgot to ask is not a console that syncs with a 5.2.
void Unsupported(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-unsupported");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string bytes = "a save a 5.2 server must not be offered\n";
  sandbox.SeedSave(SavePath("old-server.srm"), bytes);
  const std::string partial = http::PartialPathFor(io::TempPathFor(SavePath("old-server.srm")));
  sandbox.Write(partial, "half a body");

  auth::StoredToken token;
  token.server_url = kNowhere;
  token.access_token = "not-a-token";
  token.device_id = "fixture-unsupported";

  sync::ClientSaveState local;
  local.rom_id = 1;
  local.file_name = "old-server.srm";
  local.slot = "tick-unsupported";
  local.emulator = "m10-2";
  local.content_hash = "0123456789abcdef0123456789abcdef";
  local.updated_at = std::chrono::system_clock::now();
  local.file_size_bytes = static_cast<std::int64_t>(bytes.size());

  for (const char* old : {"5.2.0", "5.3.1"}) {
    sync::TickOptions options = OptionsAt(1'757'000'400);
    options.server = server::CapabilitiesFor(old);
    CountingClient client;
    const sync::TickResult refused =
        sync::RunTick(client, *files, token, {local}, {}, /*previous=*/{}, options);
    checks.Expect(refused.outcome == sync::TickOutcome::kUnsupported,
                  std::string("RomM ") + old + " ends the tick as unsupported: " +
                      sync::ToString(refused.outcome));
    checks.ExpectEq(client.requests, 0,
                    std::string("and no negotiate call is made to ") + old);
    checks.Expect(!refused.ok(), "an unsupported tick is not a completed one");
    checks.Expect(refused.answer == auth::Answer::kSilent,
                  "and says nothing about the credentials");
  }
  checks.ExpectEq(sandbox.Read(SavePath("old-server.srm")), bytes, "the save is untouched");
  // The sweep is local and puts back what an interrupted commit parked; a too
  // old server is no reason to leave a player's save stranded as `.old`.
  checks.Expect(!sandbox.Exists(partial), "the sweep still ran: it needs no server");

  sync::TickOptions current = OptionsAt(1'757'000'401);
  current.server = server::CapabilitiesFor("5.4.0");
  CountingClient reachable;
  const sync::TickResult asked =
      sync::RunTick(reachable, *files, token, {local}, {}, /*previous=*/{}, current);
  checks.Expect(reachable.requests > 0, "a 5.4.0 server is negotiated with");
  checks.Expect(asked.outcome == sync::TickOutcome::kOffline,
                std::string("and it is the missing server that stops it: ") +
                    sync::ToString(asked.outcome));

  checks.Expect(sync::TickOptions{}.server.supported,
                "a caller that passes no capabilities is assumed to talk to the floor, which is "
                "what every unit test of one stage is");
}

// --- newer_rule -----------------------------------------------------------------
//
// #232: what a save whose upload drew the slot guard's 409 becomes, given what a
// second negotiation names as the slot's head. The rule is Grout's (#226 R3):
// bytes this device has not changed since it last synced take the head as a
// download; bytes it has changed are a conflict. **Never another upload**, and
// never one when the server's answer is anything this build does not
// understand. No server: it is a function of three values.

void NewerRule(rig::Checks& checks) {
  const std::string synced = "0123456789abcdef0123456789abcdef";
  const std::string edited = "fedcba9876543210fedcba9876543210";
  const std::string theirs = "00112233445566778899aabbccddeeff";

  sync::SyncOperation head;
  head.action = sync::Action::kUpload;
  head.action_text = "upload";
  head.rom_id = 4;
  head.slot = "autosave";
  head.save_id = 20;
  head.reason = sync::Reason::kClientNewerNoHistory;
  head.reason_text = sync::ReasonText(sync::Reason::kClientNewerNoHistory);
  head.server_updated_at = "2026-10-21T09:02:14+00:00";
  head.server_content_hash = theirs;

  sync::ClientSaveState local;
  local.rom_id = 4;
  local.slot = "autosave";
  local.content_hash = synced;

  state::SaveRecord last;
  last.rom_id = 4;
  last.slot = "autosave";
  last.content_hash = synced;
  last.server_content_hash = synced;

  // Untouched since the last sync, against the head another device wrote: the
  // server asked for an upload (no history against that row), and the answer is
  // to take its head.
  {
    const std::optional<sync::SyncOperation> resolved =
        sync::ResolveNewerOnServer(head, &local, &last);
    checks.Expect(resolved.has_value() && resolved->action == sync::Action::kDownload,
                  "bytes unchanged since the last sync take the server's head as a download");
    if (resolved.has_value()) {
      checks.Expect(resolved->save_id == head.save_id &&
                        resolved->server_content_hash == head.server_content_hash,
                    "...of the row the second negotiation named, verified against its digest");
      checks.ExpectEq(resolved->reason_text, std::string(sync::kSlotNewerDetail),
                      "and the reason given is the guard's, not an upload's");
    }
  }

  // Changed here since the last sync: both sides moved, and that is a conflict.
  {
    sync::ClientSaveState moved = local;
    moved.content_hash = edited;
    const std::optional<sync::SyncOperation> resolved =
        sync::ResolveNewerOnServer(head, &moved, &last);
    checks.Expect(resolved.has_value() && resolved->action == sync::Action::kConflict,
                  "bytes changed since the last sync are a conflict, not a download");
  }

  // No record of a last sync, or one with no server digest: nothing says the
  // local bytes are safe to replace silently, so it is a conflict.
  {
    const std::optional<sync::SyncOperation> no_row =
        sync::ResolveNewerOnServer(head, &local, nullptr);
    checks.Expect(no_row.has_value() && no_row->action == sync::Action::kConflict,
                  "no baseline row is a conflict");
    state::SaveRecord bare = last;
    bare.server_content_hash = std::nullopt;
    const std::optional<sync::SyncOperation> no_digest =
        sync::ResolveNewerOnServer(head, &local, &bare);
    checks.Expect(no_digest.has_value() && no_digest->action == sync::Action::kConflict,
                  "and so is a row that never recorded the server's digest");
    sync::ClientSaveState unhashed = local;
    unhashed.content_hash = std::nullopt;
    const std::optional<sync::SyncOperation> no_hash =
        sync::ResolveNewerOnServer(head, &unhashed, &last);
    checks.Expect(no_hash.has_value() && no_hash->action == sync::Action::kConflict,
                  "and so are local bytes that were never hashed");
  }

  // What the server said, where it said more than "upload".
  {
    sync::SyncOperation conflict = head;
    conflict.action = sync::Action::kConflict;
    const std::optional<sync::SyncOperation> kept =
        sync::ResolveNewerOnServer(conflict, &local, &last);
    checks.Expect(kept.has_value() && kept->action == sync::Action::kConflict,
                  "a conflict the server names stays one, whatever the baseline says");

    sync::SyncOperation download = head;
    download.action = sync::Action::kDownload;
    download.reason = sync::Reason::kServerNewer;
    download.reason_text = sync::ReasonText(sync::Reason::kServerNewer);
    const std::optional<sync::SyncOperation> taken =
        sync::ResolveNewerOnServer(download, &local, &last);
    checks.Expect(taken.has_value() && taken->action == sync::Action::kDownload &&
                      taken->reason_text == download.reason_text,
                  "a download the server names, over unchanged bytes, is that download");
    sync::ClientSaveState moved = local;
    moved.content_hash = edited;
    const std::optional<sync::SyncOperation> guarded =
        sync::ResolveNewerOnServer(download, &moved, &last);
    checks.Expect(guarded.has_value() && guarded->action == sync::Action::kConflict,
                  "...and over changed ones, a conflict: progress never synced is not "
                  "replaced without being recorded as one");

    sync::SyncOperation nothing = head;
    nothing.action = sync::Action::kNoOp;
    nothing.reason = sync::Reason::kContentIdentical;
    const std::optional<sync::SyncOperation> idle =
        sync::ResolveNewerOnServer(nothing, &local, &last);
    checks.Expect(idle.has_value() && idle->action == sync::Action::kNoOp,
                  "a no_op is a no_op: the head already holds these bytes");

    // #233: the slot emptied between the two negotiations. Never run here --
    // not even with a save row named, which a real `delete` never carries.
    sync::SyncOperation emptied = head;
    emptied.action = sync::Action::kDelete;
    emptied.action_text = "delete";
    emptied.reason = sync::Reason::kDeletedOnServer;
    checks.Expect(!sync::ResolveNewerOnServer(emptied, &local, &last).has_value(),
                  "a delete is never something the resolution runs");

    // #233: a slot whose row is a kept delete. Its deleted bytes are never
    // uploaded, so only new progress can draw a 409 -- and against a row that
    // records no server copy, that progress is never silently replaced.
    state::SaveRecord kept_delete = last;
    kept_delete.server_content_hash = std::nullopt;
    kept_delete.deleted_on_server = true;
    sync::ClientSaveState progress = local;
    progress.content_hash = edited;
    const std::optional<sync::SyncOperation> over_kept =
        sync::ResolveNewerOnServer(head, &progress, &kept_delete);
    checks.Expect(over_kept.has_value() && over_kept->action == sync::Action::kConflict,
                  "progress on a kept delete, met by another device's head, is a conflict");
  }

  // Nothing to run: the failure stands and the next tick negotiates again.
  {
    sync::SyncOperation unknown = head;
    unknown.action = sync::Action::kNoOp;
    unknown.known_action = false;
    unknown.action_text = "merge";
    checks.Expect(!sync::ResolveNewerOnServer(unknown, &local, &last).has_value(),
                  "an action this build does not know resolves to nothing");
    sync::SyncOperation nameless = head;
    nameless.save_id = std::nullopt;
    checks.Expect(!sync::ResolveNewerOnServer(nameless, &local, &last).has_value(),
                  "and so does a head the server names no row for");
  }
}

// --- migrated_kept ----------------------------------------------------------------
//
// #236 against #233: a save RomM deleted and `on_server_delete = keep` left is
// withheld from negotiate while it holds the deleted bytes. Migrated from rc1's
// `tico-srm` to the shared slot, it is still withheld: a mark lost in the move
// would offer the deleted save to `autosave` and resurrect it there.

/// Answers every negotiation with an empty plan, and keeps the bodies.
class EmptyPlanClient final : public http::HttpClient {
 public:
  http::Result Send(const http::Request& request) override {
    http::Result result;
    result.response.status = 200;
    if (request.url.find("/api/sync/negotiate") != std::string::npos) {
      negotiated.push_back(request.body);
      result.response.body =
          R"j({"session_id":9,"operations":[],"total_upload":0,"total_download":0,)j"
          R"j("total_conflict":0,"total_no_op":0})j";
    } else {
      result.response.body = "{}";
    }
    return result;
  }
  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    http::Result result;
    result.error = http::Error::kConnectFailed;
    return result;
  }

  std::vector<std::string> negotiated;
};

void MigratedKept(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-migrated-kept");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  auth::StoredToken token;
  token.server_url = kNowhere;
  token.access_token = "not-a-token";
  token.device_id = "fixture-migrated-kept";

  const std::string deleted = "the bytes RomM deleted and the card kept\n";
  const std::string v2 =
      "rommsync-state 2\n{\"rom_id\":4,\"slot\":\"tico-srm\",\"content_hash\":\"" +
      crypto::Md5Hex(deleted) +
      "\",\"mtime\":1757000000,\"file_size_bytes\":" + std::to_string(deleted.size()) +
      ",\"server_updated_at\":null,\"server_content_hash\":null,\"deleted_on_server\":true}\n";
  state::LoadedBaseline loaded = state::ParseBaseline(v2);
  checks.ExpectEq(state::AdoptLegacySaves(
                      &loaded,
                      [](std::string_view old) -> std::optional<std::string> {
                        return rommsync::scan::MigratedSlot("autosave", old);
                      }),
                  std::size_t{1}, "the kept delete moves to the shared slot");
  const state::SaveRecord* row = loaded.value.Find(4, std::string("autosave"));
  checks.Expect(row != nullptr && row->deleted_on_server, "with its mark");

  sync::ClientSaveState kept;
  kept.rom_id = 4;
  kept.file_name = "Game.srm";
  kept.slot = "autosave";
  kept.content_hash = crypto::Md5Hex(deleted);
  kept.updated_at = sync::Timestamp{} + std::chrono::seconds{1'757'000'500};
  kept.file_size_bytes = static_cast<std::int64_t>(deleted.size());
  sync::TickOptions options = OptionsAt(1'757'000'900);
  options.recover_dirs.clear();
  {
    EmptyPlanClient client;
    sync::RunTick(client, *files, token, {kept}, {}, loaded.value, options);
    checks.Expect(client.negotiated.size() == 1 &&
                      client.negotiated[0].find("\"rom_id\":4") == std::string::npos,
                  "and negotiate is not offered the deleted bytes under the new slot");
  }
  {
    sync::ClientSaveState played = kept;
    played.content_hash = crypto::Md5Hex("new progress since the delete\n");
    EmptyPlanClient client;
    sync::RunTick(client, *files, token, {played}, {}, loaded.value, options);
    checks.Expect(client.negotiated.size() == 1 &&
                      client.negotiated[0].find("\"rom_id\":4") != std::string::npos,
                  "while new progress is");
  }
}

// --- newer_bounds ---------------------------------------------------------------
//
// What a 409's resolution must not do, whatever the server answers: run while a
// game is running (#242 -- its own check is before the plan, and this is a
// second round trip after it), or write a save step 0 never found, which is
// what a folder the scan left out -- a Tico account it could not decide, #231 --
// would be. No server: a client that answers negotiate with a plan naming the
// refused save's head, and records whether anything was fetched.

class PlanningClient final : public http::HttpClient {
 public:
  http::Result Send(const http::Request& request) override {
    ++requests;
    http::Result result;
    result.response.status = 200;
    if (request.url.find("/api/sync/negotiate") != std::string::npos) {
      negotiated.push_back(request.body);
      result.response.body =
          R"j({"session_id":9,"operations":[{"action":"upload","rom_id":4,"save_id":20,)j"
          R"j("file_name":"Game [2026-10-21_09-02-32].srm","slot":"autosave",)j"
          R"j("emulator":"snes9x","reason":"Client save is newer (no sync history)",)j"
          R"j("server_updated_at":"2026-10-21T09:02:14+00:00",)j"
          R"j("server_content_hash":"00112233445566778899aabbccddeeff"}],)j"
          R"j("total_upload":1,"total_download":0,"total_conflict":0,"total_no_op":0})j";
    } else {
      result.response.body = "{}";
    }
    return result;
  }
  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    ++downloads;
    http::Result result;
    result.error = http::Error::kConnectFailed;
    return result;
  }

  int requests = 0;
  int downloads = 0;
  std::vector<std::string> negotiated;
};

void NewerBounds(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-newer-bounds");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  auth::StoredToken token;
  token.server_url = kNowhere;
  token.access_token = "not-a-token";
  token.device_id = "fixture-newer-bounds";

  const std::string bytes = "the save a resolution must leave alone\n";
  sandbox.SeedSave(SavePath("Game.srm"), bytes);
  sync::ClientSaveState local;
  local.rom_id = 4;
  local.file_name = "Game.srm";
  local.slot = "autosave";
  local.content_hash = crypto::Md5Hex(bytes);
  local.updated_at = std::chrono::system_clock::now();
  local.file_size_bytes = static_cast<std::int64_t>(bytes.size());
  const std::vector<sync::SaveTarget> found = {{4, std::string("autosave"), SavePath("Game.srm"),
                                                "Game.srm"}};
  state::Baseline previous;
  state::SaveRecord row;
  row.rom_id = 4;
  row.slot = "autosave";
  row.content_hash = *local.content_hash;
  row.mtime = local.updated_at;
  row.file_size_bytes = local.file_size_bytes;
  row.server_content_hash = *local.content_hash;
  previous.Set(row);

  const auto refused = [](sync::SyncPlan* plan, sync::ExecutionReport* report) {
    sync::SyncOperation upload;
    upload.action = sync::Action::kUpload;
    upload.action_text = "upload";
    upload.rom_id = 4;
    upload.slot = "autosave";
    plan->session_id = 8;
    plan->operations = {upload};
    sync::OperationResult result;
    result.action = sync::Action::kUpload;
    result.rom_id = 4;
    result.slot = "autosave";
    result.outcome = sync::OperationOutcome::kFailed;
    result.error = sync::OperationError::kNewerOnServer;
    result.message = "upload rom 4, slot autosave: 409";
    report->operations = {result};
    report->failed = 1;
    report->warnings = {result.message};
  };

  // A game launched since the tick's own check: nothing is asked, nothing runs.
  {
    sync::SyncPlan plan;
    sync::ExecutionReport report;
    refused(&plan, &report);
    sync::ResolveOptions options;
    options.hold = []() { return true; };
    PlanningClient client;
    const sync::ResolveReport resolved = sync::ResolveRefusedUploads(
        client, *files, token, {local}, found, previous, &plan, &report, options);
    checks.Expect(resolved.held && !resolved.renegotiated, "a running game holds the resolution");
    checks.ExpectEq(client.requests, 0, "and it sends nothing");
    checks.Expect(report.operations[0].error == sync::OperationError::kNewerOnServer,
                  "the 409 stands for the next tick");
  }

  // No file step 0 found for the save: the head is named, and still not fetched.
  {
    sync::SyncPlan plan;
    sync::ExecutionReport report;
    refused(&plan, &report);
    PlanningClient client;
    sync::ResolveOptions options;
    options.negotiate.wait = [](std::chrono::milliseconds) {};
    options.complete.wait = [](std::chrono::milliseconds) {};
    const sync::ResolveReport resolved = sync::ResolveRefusedUploads(
        client, *files, token, {local}, /*targets=*/{}, previous, &plan, &report, options);
    checks.Expect(resolved.renegotiated, "the server is asked");
    checks.ExpectEq(resolved.resolved, std::size_t{0}, "but nothing is resolved");
    checks.ExpectEq(client.downloads, 0, "and nothing is placed where the scan found no file");
    checks.Expect(plan.operations[0].action == sync::Action::kUpload &&
                      report.operations[0].error == sync::OperationError::kNewerOnServer,
                  "the 409 stands for the next tick");
  }

  // ...and with the file found, the same answer is a download of the head.
  {
    sync::SyncPlan plan;
    sync::ExecutionReport report;
    refused(&plan, &report);
    PlanningClient client;
    sync::ResolveOptions options;
    options.negotiate.wait = [](std::chrono::milliseconds) {};
    options.complete.wait = [](std::chrono::milliseconds) {};
    options.execute.backup_dir = harness::kBackupDir;
    options.negotiate.emulators = std::vector<std::string>{"snes9x", "Snes9x"};
    const sync::ResolveReport resolved = sync::ResolveRefusedUploads(
        client, *files, token, {local}, found, previous, &plan, &report, options);
    checks.ExpectEq(resolved.resolved, std::size_t{1}, "a found save is resolved");
    // #236: the guard refused over the slot's newest save from any emulator, so
    // the question of what that save is goes out unfiltered -- a head this
    // console's filter hides would otherwise never be seen, and its 409 never end.
    checks.Expect(client.negotiated.size() == 1 &&
                      client.negotiated[0].find("\"emulators\"") == std::string::npos,
                  "the second negotiation is not filtered to the tick's emulators");
    checks.Expect(plan.operations[0].action == sync::Action::kDownload &&
                      plan.operations[0].save_id == std::optional<std::int64_t>(20),
                  "as a download of the head the server named");
    checks.Expect(client.downloads > 0, "which is fetched");
  }
  // Past `kMaxResolvedUploads` refused uploads, the rest wait for the next tick
  // (#248): each is a save asked about again, a head and its run beside the
  // tick's own, and a plan's every upload can draw the 409.
  {
    const std::size_t refused_uploads = sync::kMaxResolvedUploads + 4;
    sync::SyncPlan plan;
    sync::ExecutionReport report;
    plan.session_id = 8;
    std::vector<sync::ClientSaveState> reported;
    for (std::size_t at = 0; at < refused_uploads; ++at) {
      sync::ClientSaveState save = local;
      save.slot = "slot-" + std::to_string(at);
      reported.push_back(save);
      sync::SyncOperation upload;
      upload.action = sync::Action::kUpload;
      upload.action_text = "upload";
      upload.rom_id = 4;
      upload.slot = save.slot;
      plan.operations.push_back(upload);
      sync::OperationResult result;
      result.action = sync::Action::kUpload;
      result.rom_id = 4;
      result.slot = save.slot;
      result.outcome = sync::OperationOutcome::kFailed;
      result.error = sync::OperationError::kNewerOnServer;
      result.message = "upload rom 4, slot " + *save.slot + ": 409";
      report.operations.push_back(result);
      report.warnings.push_back(result.message);
      ++report.failed;
    }
    PlanningClient client;
    sync::ResolveOptions options;
    options.negotiate.wait = [](std::chrono::milliseconds) {};
    options.complete.wait = [](std::chrono::milliseconds) {};
    const sync::ResolveReport resolved = sync::ResolveRefusedUploads(
        client, *files, token, reported, found, previous, &plan, &report, options);
    checks.ExpectEq(resolved.refused, refused_uploads, "every refused upload is counted");
    std::size_t asked = 0;
    if (!client.negotiated.empty()) {
      for (std::size_t at = client.negotiated[0].find("\"file_name\"");
           at != std::string::npos; at = client.negotiated[0].find("\"file_name\"", at + 1)) {
        ++asked;
      }
    }
    checks.ExpectEq(asked, sync::kMaxResolvedUploads,
                    "and the second negotiation asks about no more than one tick resolves");
    std::size_t still_refused = 0;
    for (const sync::OperationResult& result : report.operations) {
      still_refused += result.error == sync::OperationError::kNewerOnServer ? 1 : 0;
    }
    checks.Expect(still_refused >= refused_uploads - sync::kMaxResolvedUploads,
                  "the rest stand refused for the next tick");
  }

  checks.ExpectEq(sandbox.Read(SavePath("Game.srm")), bytes,
                  "and the save is untouched: the fetch went nowhere");
}

/// A plan whose text is past the transport's ceiling is a plan too large, and a
/// scoped tick splits it (#248, §4).
///
/// `kPlanTooLarge` was decided while parsing, so a chunk the server answered
/// with thousands of operations was held as text first -- past 256 KiB, uncapped.
/// `http::kMaxResponseBytes` now refuses it unread, and that refusal has to mean
/// what the parse's did, or the tick would fail where it used to split.
class CeilingClient final : public http::HttpClient {
 public:
  http::Result Send(const http::Request& request) override {
    http::Result result;
    if (request.url.find("/api/sync/negotiate") == std::string::npos) {
      result.response.status = 200;
      result.response.body = "{}";
      return result;
    }
    ++negotiations;
    if (request.body.find("\"rom_ids\":[1,2]") != std::string::npos) {
      result.error = http::Error::kTooLarge;
      result.message = "past the ceiling";
      return result;
    }
    result.response.status = 200;
    result.response.body = R"({"session_id":5,"operations":[],"total_upload":0,)"
                           R"("total_download":0,"total_conflict":0,"total_no_op":0})";
    return result;
  }
  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    http::Result result;
    result.error = http::Error::kConnectFailed;
    return result;
  }
  int negotiations = 0;
};

void TooLarge(rig::Checks& checks) {
  Sandbox sandbox(checks, "tick-too-large");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  auth::StoredToken token;
  token.server_url = kNowhere;
  token.access_token = "not-a-token";
  token.device_id = "fixture-too-large";

  CeilingClient alone;
  sync::NegotiateOptions once;
  once.max_attempts = 3;
  once.wait = [](std::chrono::milliseconds) {};
  const sync::Negotiation refused = sync::NegotiateRoms(alone, token, {}, {1, 2}, once);
  checks.Expect(refused.error == sync::NegotiateError::kPlanTooLarge,
                std::string("a plan past the ceiling is a plan too large: ") +
                    sync::ToString(refused.error));
  checks.ExpectEq(alone.negotiations, 1, "which is not asked again");

  CeilingClient client;
  sync::TickOptions options = OptionsAt(1'757'000'400);
  options.server = rommsync::server::CapabilitiesFor("5.4.0");
  options.rom_scope = std::vector<std::int64_t>{1, 2};
  Impatient(&options);
  const sync::TickResult tick = sync::RunTick(client, *files, token, {}, {}, {}, options);
  checks.ExpectEq(tick.totals.plans, std::size_t{2}, "the tick split the scope and ran each half");
  checks.ExpectEq(client.negotiations, 3, "the whole scope, then each half");
}

// --- the rig scenarios --------------------------------------------------------
//
// One shape, four stages. Each arranges a plan of exactly one operation, arms
// the fault proxy at the stage under test, runs a whole tick, and then asks the
// only question that matters: what is on the card now.
//
// The fixture device answers an empty negotiation with an empty plan, so the
// operation each scenario arranges is the whole plan and the counts are
// assertable. A scenario that leaves a save behind on the server would break
// that for the next one, so every one of them deletes what it made.

/// A save only the client has, which negotiates as an `upload`.
struct UploadFixture {
  std::string slot;
  std::string name = "tick-upload.srm";
  std::string bytes = "the only copy of this save\n";
  sync::ClientSaveState local;
  std::vector<sync::SaveTarget> targets;
};

bool ArrangeUpload(rig::Checks& checks, fs::FileSystem& files, Sandbox& sandbox,
                   const harness::Rom& rom, std::string_view label, UploadFixture* out) {
  out->slot = harness::UniqueSlot(label);
  sandbox.SeedSave(SavePath(out->name), out->bytes);
  if (!LocalSaveOnCard(checks, files, sandbox, rom.id, out->name, out->slot, &out->local)) {
    return false;
  }
  out->targets = {{rom.id, out->slot, SavePath(out->name), out->name}};
  return true;
}

/// A save the server holds a newer copy of, which negotiates as a `download`.
struct DownloadFixture {
  std::string slot;
  std::string name = "tick-download.srm";
  std::string previous = "the bytes on the card\n";
  std::string server_bytes = "the bytes the server holds\n";
  harness::Save server;
  sync::ClientSaveState local;
  std::vector<sync::SaveTarget> targets;
};

bool ArrangeDownload(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                     const Fixture& fixture, Sandbox& sandbox, const harness::Rom& rom,
                     std::string_view label, DownloadFixture* out) {
  out->slot = harness::UniqueSlot(label);
  sandbox.SeedSave(SavePath(out->name), out->previous);

  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, out->server_bytes);
  if (!harness::UploadSave(client, base, fixture, rom.id, out->slot, "m2-7", staged, out->name,
                           /*with_device=*/false, &out->server)) {
    checks.Expect(false, "the server copy was stored");
    return false;
  }

  // Reported an hour old, so the server's copy is unambiguously the newer one
  // and the plan is a download rather than a conflict.
  out->local = harness::LocalSave(rom.id, out->name, out->slot, "m2-7",
                                  crypto::Md5Hex(out->previous),
                                  std::chrono::system_clock::now() - std::chrono::hours{1},
                                  static_cast<std::int64_t>(out->previous.size()));
  out->targets = {{rom.id, out->slot, SavePath(out->name), out->name}};
  return true;
}

/// The assertions every damaged tick owes, whichever stage was damaged.
void SaveSurvived(rig::Checks& checks, const Sandbox& sandbox, const std::string& name,
                  const std::string& before, const std::string& what) {
  checks.ExpectEq(sandbox.Read(SavePath(name)), before,
                  what + ": the save on the card is the one the tick started with");
  checks.Expect(!sandbox.HasBackupOf(before),
                what + ": and nothing was backed up for an overwrite that never happened");
  NoLeftovers(checks, sandbox, SavePath(name), what);
}

/// The baseline that ends a tick, read back with the engine's own reader.
state::LoadedBaseline StoredBaseline(fs::FileSystem& files) {
  return state::LoadBaseline(files.Resolve(sync::kStateSdPath));
}

// --- negotiate ----------------------------------------------------------------
//
// The stage that decides whether the tick happens at all. A failure here has to
// cost nothing, because the alternative -- a client that writes something on the
// way to finding out the plan is unreadable -- is a save changed for no reason.

void NegotiateFault(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                    const Fixture& fixture, const harness::Rom& rom, const std::string& spec,
                    sync::NegotiateError expected, sync::TickOutcome outcome,
                    const std::string& what) {
  Sandbox sandbox(checks, "tick-negotiate");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  UploadFixture arranged;
  if (!ArrangeUpload(checks, *files, sandbox, rom, "m2-7-negotiate", &arranged)) {
    return;
  }
  const std::map<std::string, std::string> before = Snapshot(sandbox);

  sync::TickOptions options = OptionsAt(1'757'000'300);
  Impatient(&options);
  {
    harness::Fault fault(checks, client, base, spec);
    const sync::TickResult tick = sync::RunTick(client, *files, TokenFor(base, fixture),
                                                {arranged.local}, arranged.targets,
                                                /*previous=*/{}, options);
    checks.Expect(tick.negotiated.error == expected,
                  what + ": the negotiation is named " + sync::ToString(expected) + ", got " +
                      sync::ToString(tick.negotiated.error) + " -- " + tick.negotiated.message);
    checks.Expect(tick.outcome == outcome,
                  what + ": the tick is " + sync::ToString(outcome) + ", got " +
                      sync::ToString(tick.outcome));
    checks.ExpectEq(tick.executed.operations.size(), static_cast<std::size_t>(0),
                    what + ": and no operation was attempted");
  }

  // The whole claim: a tick that never got a plan changed nothing.
  checks.Expect(Snapshot(sandbox) == before,
                what + ": the card is byte-for-byte what it was before the tick");
  SaveSurvived(checks, sandbox, arranged.name, arranged.bytes, what);

  // Nothing reached the server either, which is what makes the next tick a
  // clean retry rather than a second upload of the same save.
  const int rows = SaveRowsFor(client, base, fixture, rom.id, arranged.slot, nullptr);
  checks.ExpectEq(rows, 0, what + ": and the server holds no row for a save never sent");
}

/// Sync switched off for this console in RomM (#235): the real 400, from a
/// device of the fixture user's whose switch is really off.
///
/// Its own outcome rather than `kRefused`, because what a caller does about it
/// differs -- the schedule keeps its interval and the status screen names the
/// switch (sync_tick.hpp). And everything `NegotiateFault` holds a refused
/// negotiation to holds here too: no operation, nothing on the card changed.
void ServerDisabled(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                    const Fixture& fixture, const harness::Rom& rom) {
  const std::string device_id =
      harness::CreateSyncDisabledDevice(checks, client, base, fixture, "tick-server-disabled");
  if (device_id.empty()) {
    return;
  }
  Sandbox sandbox(checks, "tick-server-disabled");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  UploadFixture arranged;
  if (ArrangeUpload(checks, *files, sandbox, rom, "m10-9-server-disabled", &arranged)) {
    const std::map<std::string, std::string> before = Snapshot(sandbox);
    auth::StoredToken token = TokenFor(base, fixture);
    token.device_id = device_id;
    sync::TickOptions options = OptionsAt(1'757'000'350);
    Impatient(&options);
    const sync::TickResult tick = sync::RunTick(client, *files, token, {arranged.local},
                                                arranged.targets, /*previous=*/{}, options);
    checks.Expect(tick.negotiated.error == sync::NegotiateError::kSyncDisabled,
                  std::string("RomM's answer is read as the switch: ") +
                      sync::ToString(tick.negotiated.error) + " -- " + tick.negotiated.message);
    checks.ExpectEq(tick.negotiated.attempts, 1, "and asked once, not retried");
    checks.Expect(tick.outcome == sync::TickOutcome::kServerDisabled,
                  std::string("the tick is server_disabled, not a refusal: ") +
                      sync::ToString(tick.outcome));
    checks.Expect(tick.answer == auth::Answer::kAccepted,
                  "and the token was read to answer it, which clears the gate's count");
    checks.ExpectEq(tick.executed.operations.size(), static_cast<std::size_t>(0),
                    "no operation was attempted");
    checks.Expect(Snapshot(sandbox) == before, "and the card is byte-for-byte what it was");
    SaveSurvived(checks, sandbox, arranged.name, arranged.bytes, "sync disabled on the server");
  }
  harness::DeleteDevice(client, base, fixture, device_id);
}

// --- M7-2: the schedule around a failed tick ---------------------------------
//
// Everything above is about one tick. These two are about what happens *next*,
// which is the scheduler's (`core/include/rommsync/scheduler.hpp`) -- and they
// are here rather than in `test_scheduler.cpp` because they are the two
// acceptance cases that cannot be produced with an injected outcome. A tick
// really has to fail on the stall timeout, and a connection really has to be
// reset mid-plan, before "reschedules with backoff rather than spinning" and
// "nothing was written" mean anything.
//
// The clocks are still injected: the point is the *decision*, and a test that
// waited out thirty seconds of backoff to see one would be the test nobody runs.

/// A steady clock the scenario advances by hand, and a wall clock beside it.
struct ScheduleClocks {
  std::chrono::milliseconds steady{std::chrono::hours{5}};
  std::chrono::milliseconds wall{std::chrono::seconds{1'757'000'000}};

  void Advance(std::chrono::milliseconds by) {
    steady += by;
    wall += by;
  }
};

/// A tick that fails against a real fault reschedules on the backoff, and the
/// next one that works puts the delay back to the floor.
///
/// `spec` arms the proxy for the first tick only; the second runs clean, which
/// is the "the next success resets the delay" half of the acceptance and the
/// only way to see that a failed tick did not poison the schedule.
void Scheduled(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture, const harness::Rom& rom, const std::string& spec,
               const std::string& what) {
  Sandbox sandbox(checks, "tick-scheduled");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  UploadFixture arranged;
  if (!ArrangeUpload(checks, *files, sandbox, rom, "m7-2-schedule", &arranged)) {
    return;
  }
  const std::map<std::string, std::string> before = Snapshot(sandbox);

  ScheduleClocks clocks;
  sync::SchedulerConfig config;
  config.interval = std::chrono::minutes{30};
  config.backoff.floor = std::chrono::seconds{30};
  config.backoff.cap = std::chrono::minutes{10};
  // Off, so the delay this scenario asserts on is a number rather than a range.
  // The jitter itself is `sched.backoff`'s.
  config.backoff.jitter = 0.0;
  sync::Scheduler scheduler(
      config,
      [&clocks] { return std::chrono::steady_clock::time_point{} + clocks.steady; },
      [&clocks] { return std::chrono::system_clock::time_point{} + clocks.wall; });

  sync::TickOptions options = OptionsAt(1'757'000'500);
  Impatient(&options);

  const sync::Decision first = scheduler.Poll();
  checks.Expect(first.trigger == sync::Trigger::kBoot, what + ": the first tick is the boot one");
  {
    harness::Fault fault(checks, client, base, spec);
    const sync::TickResult tick =
        sync::RunTick(client, *files, TokenFor(base, fixture), {arranged.local}, arranged.targets,
                      /*previous=*/{}, options);
    checks.Expect(tick.outcome == sync::TickOutcome::kOffline,
                  what + ": the tick is offline, got " + sync::ToString(tick.outcome) + " -- " +
                      tick.negotiated.message);
    scheduler.Finished(tick);
  }

  // Nothing was written. That is the half of the acceptance the scheduler cannot
  // check for itself, and the reason this scenario runs a real tick.
  checks.Expect(Snapshot(sandbox) == before,
                what + ": the card is byte-for-byte what it was before the failed tick");

  checks.ExpectEq(scheduler.failures(), 1, what + ": the failed tick is counted");
  checks.Expect(scheduler.backoff() == std::chrono::seconds{30},
                what + ": and the next attempt waits the floor");
  const sync::Decision waiting = scheduler.Poll();
  checks.Expect(!waiting.run(), what + ": nothing runs inside the backoff window");
  checks.Expect(!waiting.parked && waiting.sleep_for == std::chrono::seconds{30},
                what + ": it sleeps on the backoff rather than spinning");

  // A worker that polled in a loop would have run a second tick by now. Ten
  // polls, no clock movement, no tick.
  for (int poll = 0; poll < 10; ++poll) {
    checks.Expect(!scheduler.Poll().run(), what + ": and it stays that way however often it is asked");
  }

  clocks.Advance(std::chrono::seconds{31});
  const sync::Decision retry = scheduler.Poll();
  checks.Expect(retry.trigger == sync::Trigger::kInterval,
                what + ": the retry fires when the backoff is up");
  const sync::TickResult second =
      sync::RunTick(client, *files, TokenFor(base, fixture), {arranged.local}, arranged.targets,
                    /*previous=*/{}, options);
  checks.Expect(second.outcome == sync::TickOutcome::kCompleted,
                what + ": and the tick that follows the fault completes, got " +
                    sync::ToString(second.outcome));
  scheduler.Finished(second);
  checks.ExpectEq(scheduler.failures(), 0, what + ": one success clears the run of failures");
  checks.Expect(scheduler.backoff() == std::chrono::milliseconds{0},
                what + ": and the delay is back to nothing");
  checks.Expect(!scheduler.Poll().run(),
                what + ": leaving the ordinary interval, not another retry");

  // The save the fault interrupted did reach the server on that second tick,
  // which is what makes the backoff a delay rather than a loss.
  std::vector<std::int64_t> ids;
  const int rows = SaveRowsFor(client, base, fixture, rom.id, arranged.slot, &ids);
  checks.ExpectEq(rows, 1, what + ": and the save is on the server exactly once");
  // Deleted, like every scenario's: two rows a run is what a long-lived fixture
  // collected until each console new to it met more saves than the log's tail
  // holds lines (engine.foreground, engine.server_deleted).
  for (const std::int64_t id : ids) {
    harness::DeleteSave(client, base, fixture, id);
  }
}

// --- upload -------------------------------------------------------------------
//
// An upload never touches the local save, so what is at stake here is the
// *server*: an upload whose response was lost is a duplication hazard, because
// RomM matches an existing slot row by a second-granularity datetime tag it
// computes at ingest (issue #85). There is no retry inside a tick precisely so
// that a lost response costs a tick rather than a second row.

/// `rows_expected` is the row count on the server when the scenario is over --
/// after the second tick, when `then_normally` asks for one. That second tick is
/// the acceptance's "and the next tick runs normally", which a scenario that
/// stopped at the failure would not have checked.
void UploadFault(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                 const Fixture& fixture, const harness::Rom& rom, const std::string& spec,
                 sync::OperationOutcome outcome, sync::OperationError error, int rows_expected,
                 bool then_normally, const std::string& what) {
  Sandbox sandbox(checks, "tick-upload");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  UploadFixture arranged;
  if (!ArrangeUpload(checks, *files, sandbox, rom, "m2-7-upload", &arranged)) {
    return;
  }

  sync::TickOptions options = OptionsAt(1'757'000'400);
  Impatient(&options);
  {
    harness::Fault fault(checks, client, base, spec);
    const sync::TickResult tick = sync::RunTick(client, *files, TokenFor(base, fixture),
                                                {arranged.local}, arranged.targets,
                                                /*previous=*/{}, options);
    if (tick.executed.operations.size() != 1) {
      checks.Expect(false, what + ": the plan carried the one operation this run created, got " +
                               std::to_string(tick.executed.operations.size()));
    } else {
      const sync::OperationResult& result = tick.executed.operations[0];
      checks.Expect(result.outcome == outcome,
                    what + ": the operation is " + sync::ToString(outcome) + ", got " +
                        sync::ToString(result.outcome) + " -- " + result.message);
      checks.Expect(result.error == error,
                    what + ": for the reason " + sync::ToString(error) + ", got " +
                        sync::ToString(result.error));
    }
    // The baseline is written whatever became of the plan, and *before* the
    // session is reported -- that order is what stops a failed tick from
    // costing the whole library a re-hash (sync_finish.hpp).
    checks.Expect(tick.finished.stored.ok(),
                  what + ": the baseline still reached the card: " + tick.finished.stored.message);
  }

  SaveSurvived(checks, sandbox, arranged.name, arranged.bytes, what);

  if (then_normally) {
    // The fault has disarmed itself with the scope above, so this is the next
    // schedule with nothing wrong: a tick that wedged rather than timed out
    // would not get here at all.
    const sync::TickResult next =
        sync::RunTick(client, *files, TokenFor(base, fixture), {arranged.local},
                      arranged.targets, /*previous=*/{}, OptionsAt(1'757'000'401));
    checks.Expect(next.outcome == sync::TickOutcome::kCompleted,
                  what + ": and the next tick runs normally: " + sync::ToString(next.outcome) +
                      (next.executed.warnings.empty() ? std::string()
                                                      : " -- " + next.executed.warnings[0]));
    checks.ExpectEq(next.executed.completed, 1, what + ": doing the work the damaged one did not");
    SaveSurvived(checks, sandbox, arranged.name, arranged.bytes,
                 what + ", after the next tick");
  }

  // The row count is the point. One means the upload landed and the client was
  // right not to send it again; zero means it never left.
  std::vector<std::int64_t> ids;
  const int rows = SaveRowsFor(client, base, fixture, rom.id, arranged.slot, &ids);
  checks.ExpectEq(rows, rows_expected,
                  what + ": the server holds the rows this tick can account for -- a second one "
                         "would be the re-post there is deliberately no retry for");
  for (const std::int64_t id : ids) {
    harness::DeleteSave(client, base, fixture, id);
  }
}

// --- isolated -----------------------------------------------------------------
//
// #238's acceptance, on the live 5.4.0. The systems whose Tico format no libretro
// core reads sync under their own `tico-*` ids, and a RetroArch lineage's save
// for one of them is never put on the card -- not over DuckStation's card when
// RomM pairs the two in one slot, not through #232's resolution of the 409 that
// pairing draws, and not as a first-time restore into the folder DuckStation
// reads.

/// The one operation the plan holds for `slot`, and what became of it.
bool OperationIn(const sync::TickResult& tick, const std::string& slot,
                 const sync::OperationResult** out) {
  for (std::size_t at = 0; at < tick.executed.operations.size(); ++at) {
    const sync::OperationResult& done = tick.executed.operations[at];
    if (done.slot.has_value() && *done.slot == slot) {
      *out = &done;
      return true;
    }
  }
  return false;
}

/// `emulator` of the server's save `id`, as RomM stored it.
std::string StoredEmulator(http::HttpClient& client, const std::string& base,
                           const Fixture& fixture, std::int64_t id) {
  const http::Result got = client.Send(harness::Authed(
      http::Method::kGet, base + "/api/saves/" + std::to_string(id), fixture));
  const json::ParseResult parsed = json::Parse(got.response.body);
  return got.successful() && parsed.ok() ? harness::Field(parsed.value, "emulator")
                                         : std::string("<unreadable: ") + got.response.body + ">";
}

void Isolated(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture) {
  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "Synthetic Nested Game", &rom)) {
    checks.Expect(false, "the fixture's psx rom is there");
    return;
  }
  Sandbox sandbox(checks, "tick-isolated");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string folder = "/tico/saves/psx";
  sandbox.MakeDirs(folder);

  const std::string matched = harness::UniqueSlot("m10-12-matched");
  const std::string refused = harness::UniqueSlot("m10-12-409");
  const std::string beetle = harness::UniqueSlot("m10-12-beetle");
  const std::string own = harness::UniqueSlot("m10-12-own");
  const std::string card = "DuckStation's card, the only copy of this progress\n";
  const std::string kept = "DuckStation's card, newer than anything on RomM\n";
  const std::string retroarch = "a Beetle PSX .srm, which is not a DuckStation card\n";
  const std::string switched = "DuckStation's card from another Switch\n";

  const std::string staged_retroarch = sandbox.Host("/config/rommsync/beetle.srm");
  const std::string staged_switch = sandbox.Host("/config/rommsync/other.mcd");
  rig::WriteFile(staged_retroarch, retroarch);
  rig::WriteFile(staged_switch, switched);
  std::vector<std::int64_t> planted;
  const auto sweep = [&]() {
    for (const std::int64_t id : planted) {
      harness::DeleteSave(client, base, fixture, id);
    }
  };
  // Other devices: RetroArch's Beetle core into the slots this Switch's cards
  // are in and one it has never seen, and another Switch's DuckStation.
  for (const std::string& slot : {matched, refused, beetle}) {
    harness::Save stored;
    if (!harness::UploadSave(client, base, fixture, rom.id, slot, "mednafen_psx_hw",
                             staged_retroarch, "Synthetic Nested Game.srm", false, &stored)) {
      checks.Expect(false, "RetroArch's save was stored in " + slot);
      sweep();
      return;
    }
    planted.push_back(stored.id);
  }
  harness::Save other;
  if (!harness::UploadSave(client, base, fixture, rom.id, own, "tico-duckstation", staged_switch,
                           "Synthetic Nested Game.mcd", false, &other)) {
    checks.Expect(false, "the other Switch's card was stored");
    sweep();
    return;
  }
  planted.push_back(other.id);

  const std::string matched_path = folder + "/Matched.mcd";
  const std::string refused_path = folder + "/Refused.mcd";
  sandbox.SeedSave(matched_path, card);
  sandbox.SeedSave(refused_path, kept);
  const auto now = std::chrono::system_clock::now();
  // Older than RetroArch's upload, so the plan is a download over the card; and
  // newer, so it is an upload the slot guard refuses and #232 resolves.
  const std::vector<sync::ClientSaveState> reported = {
      harness::LocalSave(rom.id, "Matched.mcd", matched, "tico-duckstation", crypto::Md5Hex(card),
                         now - std::chrono::hours{1}, static_cast<std::int64_t>(card.size())),
      harness::LocalSave(rom.id, "Refused.mcd", refused, "tico-duckstation", crypto::Md5Hex(kept),
                         now + std::chrono::seconds{300}, static_cast<std::int64_t>(kept.size()))};
  const std::vector<sync::SaveTarget> targets = {
      {rom.id, matched, matched_path, "Matched.mcd"}, {rom.id, refused, refused_path, "Refused.mcd"}};

  sync::TickOptions options = OptionsAt(1'757'000'800);
  options.recover_dirs = {folder, harness::kBackupDir};
  // A placer that puts anything of this run's into DuckStation's folder -- one
  // that does not refuse a Beetle save itself, as `place::Placer` would -- so
  // what is under test is the executor's own check, with `reads_as` saying
  // what the scan reads that folder as.
  const std::int64_t rom_id = rom.id;
  options.execute.place = [&, rom_id](const sync::SyncOperation& operation) {
    rommsync::place::Placement placement;
    if (operation.rom_id != rom_id || !operation.slot.has_value()) {
      placement.reason = "another scenario's leftover: not this run's to place";
    } else if (*operation.slot == beetle) {
      placement.sd_path = folder + "/Beetle.mcd";
    } else if (*operation.slot == own) {
      placement.sd_path = folder + "/Own.mcd";
    } else {
      placement.reason = "not one of this run's first-time restores";
    }
    return placement;
  };
  const rommsync::config::Config config = rommsync::config::Defaults();
  options.execute.reads_as = [&config](const sync::SyncOperation&, const std::string& sd_path) {
    return rommsync::scan::EmulatorAt(sd_path, "psx", config);
  };

  const sync::TickResult tick =
      sync::RunTick(client, *files, TokenFor(base, fixture), reported, targets, {}, options);

  const sync::OperationResult* done = nullptr;
  checks.Expect(OperationIn(tick, matched, &done) && done->error == sync::OperationError::kOtherEmulator,
                std::string("a Beetle save paired with DuckStation's card is refused: ") +
                    (done == nullptr ? "no operation" : done->message));
  checks.ExpectEq(sandbox.Read(matched_path), card, "and the card is untouched");

  done = nullptr;
  checks.ExpectEq(tick.resolved.refused, std::size_t{1},
                  "the newer card's upload met the slot guard");
  checks.Expect(OperationIn(tick, refused, &done) && done->error == sync::OperationError::kOtherEmulator,
                std::string("and the 409's resolution does not take RetroArch's save either: ") +
                    (done == nullptr ? "no operation" : done->message));
  checks.ExpectEq(sandbox.Read(refused_path), kept, "the newer card is untouched");
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, refused, nullptr), 1,
                  "and nothing was uploaded over RetroArch's save");

  done = nullptr;
  checks.Expect(OperationIn(tick, beetle, &done) && done->error == sync::OperationError::kOtherEmulator,
                std::string("a Beetle save is not placed where DuckStation reads: ") +
                    (done == nullptr ? "no operation" : done->message));
  checks.Expect(!sandbox.Exists(folder + "/Beetle.mcd"), "nothing is on the card for it");
  checks.Expect(!sandbox.HasBackupOf(card) && !sandbox.HasBackupOf(kept),
                "and no card needed a backup, because none was overwritten");

  done = nullptr;
  checks.Expect(OperationIn(tick, own, &done) &&
                    done->outcome == sync::OperationOutcome::kDownloaded,
                std::string("another Switch's DuckStation card is restored: ") +
                    (done == nullptr ? "no operation" : done->message));
  checks.ExpectEq(sandbox.Read(folder + "/Own.mcd"), switched, "into DuckStation's folder");
  sweep();
}

/// #238's first acceptance item: a Tico 0.8 card holding a save for each system
/// whose format is isolated, scanned by the engine's own scanner and uploaded by
/// a real tick, lands on RomM under the `tico-*` id that names its module -- and
/// PPSSPP's save folder is a named skip that uploads nothing.
///
/// The fixture's library has psx roms and no n64, dc, nds, saturn or psp ones,
/// and growing it costs every worktree a re-scan (`lists.platforms` makes the
/// same trade). So the other systems borrow a rom each, relabelled in the index
/// the scan matches against: RomM files a save by `rom_id` and takes `emulator`
/// as given, which is what this checks. The slots are this run's own, since the
/// slot is #236's and not what is being tested.
void IsolatedIds(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                 const Fixture& fixture) {
  struct System {
    const char* rom;       ///< the fixture rom it borrows
    const char* platform;  ///< RomM's slug for it here
    const char* folder;    ///< Tico's folder
    const char* extension;
    const char* emulator;  ///< what RomM must hold
  };
  const System kSystems[] = {
      {"Synthetic Nested Game", "psx", "psx", ".mcd", "tico-duckstation"},
      {"nova.nes", "n64", "n64", ".eep", "tico-mupen64plus"},
      {"240pee.nes", "dreamcast", "dc", ".bin", "tico-flycast"},
      {"240pee_mb.gba", "nds", "nds", ".dsv", "tico-drastic"},
      {"synthetic-large.gba", "saturn", "saturn", ".bkr", "tico-yabasanshiro"},
      {"gb240p.gb", "psp", "psp", "", "tico-ppsspp"},
  };
  const std::string account = "0123456789abcdef0123456789abcdef";
  const std::string saves = "/tico/saves/users/" + account;

  Sandbox sandbox(checks, "tick-isolated-ids");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  rommsync::roms::RomIndex index;
  std::int64_t psp_rom = 0;
  for (const System& system : kSystems) {
    harness::Rom rom;
    if (!harness::FindRom(client, base, fixture, system.rom, &rom)) {
      checks.Expect(false, std::string("the fixture holds ") + system.rom);
      return;
    }
    const std::string stem = rommsync::scan::BaseName(rom.fs_name);
    rommsync::roms::Rom entry;
    entry.id = rom.id;
    entry.fs_name_no_ext = stem;
    entry.fs_name_no_tags = stem;
    entry.platform_fs_slug = system.platform;
    index.Add(entry);
    const std::string directory = saves + "/" + system.folder;
    if (*system.extension == '\0') {
      // PPSSPP: a folder per game at the SAVEDATA level.
      psp_rom = rom.id;
      sandbox.MakeDirs(directory + "/ULUS10041");
      sandbox.Write(directory + "/ULUS10041/DATA.BIN", "a PPSSPP save\n");
      continue;
    }
    sandbox.MakeDirs(directory);
    sandbox.SeedSave(directory + "/" + stem + system.extension,
                     std::string(system.emulator) + " bytes for #238\n");
    if (std::string(system.platform) == "n64") {
      // Mupen64Plus splits a cart's save by type: each file is a save of its own.
      for (const char* other : {".sra", ".fla", ".mpk"}) {
        sandbox.SeedSave(directory + "/" + stem + other,
                         std::string("tico-mupen64plus ") + other + " bytes for #238\n");
      }
    }
    if (std::string(system.platform) == "psx") {
      // DuckStation's second card: a second save of the rom.
      sandbox.SeedSave(directory + "/" + stem + "_2.mcd", "DuckStation's card 2 for #238\n");
    }
  }

  const rommsync::config::Config config = rommsync::config::Defaults();
  const rommsync::scan::ScanResult scanned = rommsync::scan::ScanSaves(
      config, rommsync::retroarch::Settings{}, index, *files, state::Baseline{});
  checks.ExpectEq(scanned.saves.size(), std::size_t{9},
                  "a save per isolated system, four for N64 and two cards for psx: " +
                      scanned.DescribeSkipped());
  // The slots the scan gives them under #236's rule, before this run swaps in
  // its own: N64's four files are four saves, and card 2 is not card 1.
  std::set<std::string> slots;
  for (const rommsync::scan::SaveFile& save : scanned.saves) {
    slots.insert(save.platform_fs_slug + "/" + save.slot);
  }
  const std::set<std::string> kSlots = {
      "psx/autosave-mcd",  "psx/autosave-mcd-card2", "n64/autosave-eep",
      "n64/autosave-sra",  "n64/autosave-fla",       "n64/autosave-mpk",
      "dreamcast/autosave-bin", "nds/autosave-dsv",  "saturn/autosave-bkr"};
  checks.Expect(slots == kSlots, "each isolated save is in a slot of its own");
  bool named = false;
  for (const rommsync::scan::Skip& skip : scanned.skipped) {
    named = named || (skip.reason == rommsync::scan::SkipReason::kSaveFolder &&
                      skip.sd_path == saves + "/psp/ULUS10041");
  }
  checks.Expect(named, "PPSSPP's save folder is the named skip: " + scanned.DescribeSkipped());

  std::vector<sync::ClientSaveState> reported;
  std::vector<sync::SaveTarget> targets;
  std::map<std::string, std::string> expected;  // slot -> emulator
  for (const rommsync::scan::SaveFile& save : scanned.saves) {
    const std::string slot = harness::UniqueSlot("m10-12-" + save.platform_fs_slug);
    sync::ClientSaveState state = save.ToClientSaveState();
    state.slot = slot;
    reported.push_back(state);
    targets.push_back({save.rom_id, slot, save.sd_path, save.file_name});
    for (const System& system : kSystems) {
      if (save.platform_fs_slug == system.platform) {
        expected[slot] = system.emulator;
      }
    }
  }

  sync::TickOptions options = OptionsAt(1'757'000'900);
  options.recover_dirs = {harness::kBackupDir};
  const sync::TickResult tick =
      sync::RunTick(client, *files, TokenFor(base, fixture), reported, targets, {}, options);

  std::vector<std::int64_t> uploaded;
  for (const auto& [slot, emulator] : expected) {
    const sync::OperationResult* done = nullptr;
    if (!OperationIn(tick, slot, &done) || done->outcome != sync::OperationOutcome::kUploaded ||
        !done->save_id.has_value()) {
      checks.Expect(false, emulator + "'s save was uploaded: " +
                               (done == nullptr ? std::string("no operation") : done->message));
      continue;
    }
    uploaded.push_back(*done->save_id);
    checks.ExpectEq(StoredEmulator(client, base, fixture, *done->save_id), emulator,
                    "RomM holds the save under " + emulator);
  }
  std::size_t uploads = 0;
  for (const sync::SyncOperation& operation : tick.negotiated.plan.operations) {
    checks.Expect(!(operation.rom_id == psp_rom && operation.action == sync::Action::kUpload),
                  "nothing of PPSSPP's was offered to RomM");
    uploads += operation.action == sync::Action::kUpload ? 1 : 0;
  }
  checks.ExpectEq(uploads, expected.size(), "and nothing but the isolated saves went up");
  for (const std::int64_t id : uploaded) {
    harness::DeleteSave(client, base, fixture, id);
  }
}

// --- the rom scope (#241) -----------------------------------------------------
//
// Against a fresh device of their own: an unscoped negotiation offers a device
// with no history every save in the library, which is exactly what the scope is
// there to stop, and the fixture device's history is every other scenario's.

/// Every negotiate body sent, and how many sessions were completed.
class Recording final : public http::HttpClient {
 public:
  explicit Recording(http::HttpClient& inner) : inner_(inner) {}

  http::Result Send(const http::Request& request) override {
    const http::Result result = inner_.Send(request);
    if (request.method == http::Method::kPost &&
        request.url.find("/api/sync/negotiate") != std::string::npos) {
      negotiations.push_back(request.body);
      const json::ParseResult plan = json::Parse(result.response.body);
      const json::Value* id = plan.ok() ? plan.value.Find("session_id") : nullptr;
      sessions.push_back(id != nullptr && id->is_integer() ? id->integer() : 0);
      const json::Value* planned = plan.ok() ? plan.value.Find("operations") : nullptr;
      operations.push_back(planned != nullptr && planned->is_array() ? planned->size() : 0);
    } else if (request.method == http::Method::kPost &&
               request.url.find("/api/saves?") != std::string::npos) {
      uploads.push_back(request.url);
    } else if (request.method == http::Method::kPost &&
               request.url.find("/complete") != std::string::npos && result.successful()) {
      ++completed;
    }
    return result;
  }
  http::Result Download(const http::Request& request, const http::DownloadTarget& into) override {
    return inner_.Download(request, into);
  }

  void Clear() {
    negotiations.clear();
    sessions.clear();
    operations.clear();
    uploads.clear();
    completed = 0;
  }

  std::vector<std::string> negotiations;
  std::vector<std::int64_t> sessions;  ///< each negotiation's, in order
  std::vector<std::size_t> operations;  ///< how many each one's plan held
  std::vector<std::string> uploads;    ///< every upload's URL
  int completed = 0;

 private:
  http::HttpClient& inner_;
};

/// The operation the plan holds for `(rom_id, slot)`, and how many it holds.
const sync::SyncOperation* PlannedFor(const sync::TickResult& tick, std::int64_t rom_id,
                                      const std::string& slot, int* count) {
  const sync::SyncOperation* found = nullptr;
  *count = 0;
  for (const sync::SyncOperation& operation : tick.negotiated.plan.operations) {
    if (operation.rom_id == rom_id && operation.slot.has_value() && *operation.slot == slot) {
      found = &operation;
      ++*count;
    }
  }
  return found;
}

/// `rom_ids` of one recorded body, or nullopt when it sent none.
std::optional<std::vector<std::int64_t>> ScopeOf(const std::string& body, bool* restoring) {
  const json::ParseResult document = json::Parse(body);
  *restoring = false;
  if (!document.ok()) {
    return std::nullopt;
  }
  const json::Value* flag = document.value.Find("restore_unlisted");
  *restoring = flag != nullptr && flag->is_bool() && flag->boolean();
  const json::Value* ids = document.value.Find("rom_ids");
  if (ids == nullptr || !ids->is_array()) {
    return std::nullopt;
  }
  std::vector<std::int64_t> out;
  for (const json::Value& id : ids->elements()) {
    out.push_back(id.is_integer() ? id.integer() : 0);
  }
  return out;
}

/// #241: every negotiate names the roms on the card, in chunks of RomM's 500, and
/// what comes back is about those roms only -- a server save of a rom the card
/// does not hold is not offered, so it is not placed beside nothing (#237).
void Scoped(rig::Checks& checks, http::HttpClient& client, const std::string& base,
            const Fixture& fixture, const harness::Rom& rom) {
  const std::string device = harness::CreateDevice(checks, client, base, fixture, "tick-scoped");
  if (device.empty()) {
    return;
  }
  Sandbox sandbox(checks, "tick-scoped");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m10-15-scope");
  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, "another device's save, for the scope\n");
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m2-7", staged, "scoped.srm",
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "another device's save is on the server");
    harness::DeleteDevice(client, base, fixture, device);
    return;
  }

  auth::StoredToken token = TokenFor(base, fixture);
  token.device_id = device;
  Recording recording(client);
  sync::TickOptions options = OptionsAt(1'757'000'400);

  // A card that holds some other rom: the server's save of this one is not
  // offered at all.
  const std::int64_t elsewhere = 900'000'001;
  options.rom_scope = std::vector<std::int64_t>{elsewhere};
  {
    const sync::TickResult tick = sync::RunTick(recording, *files, token, {}, {}, {}, options);
    int count = 0;
    checks.Expect(tick.negotiated.ok(), "a scoped negotiation is answered: " +
                                            tick.negotiated.message);
    checks.Expect(PlannedFor(tick, rom.id, slot, &count) == nullptr,
                  "a save of a rom the card does not hold is not offered");
    bool restoring = true;
    checks.Expect(recording.negotiations.size() == 1 &&
                      ScopeOf(recording.negotiations[0], &restoring) ==
                          std::vector<std::int64_t>{elsewhere} &&
                      !restoring,
                  "one negotiation, naming the card's rom, not restoring");
  }

  // The card holds it: offered.
  recording.Clear();
  options.rom_scope = std::vector<std::int64_t>{rom.id};
  {
    const sync::TickResult tick = sync::RunTick(recording, *files, token, {}, {}, {}, options);
    int count = 0;
    const sync::SyncOperation* offered = PlannedFor(tick, rom.id, slot, &count);
    checks.Expect(offered != nullptr && offered->action == sync::Action::kDownload,
                  "on the card, the server's save is offered as a download");
  }

  // The rom index's bound on the card: three negotiations, each inside RomM's
  // 500 and each body inside `kMaxRomScopeBytes` of an unscoped one, and every
  // session closed.
  recording.Clear();
  std::vector<std::int64_t> card = {rom.id};
  for (std::int64_t id = 1; card.size() < rommsync::roms::kMaxIndexRoms; ++id) {
    card.push_back(900'000'000 + id);
  }
  options.rom_scope = card;
  // Each chunk's plan is carried out on its own and freed before the next
  // (#246), so the tick's operations are the ones every plan handed over.
  std::vector<sync::SyncOperation> planned;
  options.on_plan = [&planned](const sync::SyncPlan& plan, const sync::ExecutionReport&) {
    planned.insert(planned.end(), plan.operations.begin(), plan.operations.end());
  };
  {
    const sync::TickResult tick = sync::RunTick(recording, *files, token, {}, {}, {}, options);
    checks.Expect(tick.negotiated.ok(), "a chunked negotiation is answered: " +
                                            tick.negotiated.message);
    checks.ExpectEq(recording.negotiations.size(), std::size_t{3},
                    "1,024 roms on the card are three negotiations");
    sync::SyncNegotiatePayload unscoped;
    unscoped.device_id = device;
    const std::size_t plain = sync::EncodeNegotiateRequest(unscoped).body.size();
    std::size_t named = 0;
    for (const std::string& body : recording.negotiations) {
      bool restoring = true;
      const std::optional<std::vector<std::int64_t>> scope = ScopeOf(body, &restoring);
      checks.Expect(scope.has_value() && scope->size() <= sync::kMaxNegotiateRomIds && !restoring,
                    "each names at most 500 roms");
      named += scope.has_value() ? scope->size() : 0;
      checks.Expect(body.size() <= plain + sync::kMaxRomScopeBytes,
                    "and costs at most kMaxRomScopeBytes more than an unscoped body: " +
                        std::to_string(body.size()) + " against " + std::to_string(plain));
    }
    checks.ExpectEq(named, card.size(), "between them, every rom on the card once");
    int count = 0;
    for (const sync::SyncOperation& operation : planned) {
      count += operation.rom_id == rom.id && operation.slot == slot ? 1 : 0;
    }
    checks.ExpectEq(count, 1, "the server's save is offered once, by the chunk that names it");
    checks.ExpectEq(tick.totals.plans, std::size_t{3}, "and each chunk's plan ran on its own");
    bool in_scope = true;
    for (const sync::SyncOperation& operation : planned) {
      in_scope = in_scope && std::find(card.begin(), card.end(), operation.rom_id) != card.end();
    }
    checks.Expect(in_scope, "and nothing outside the card is offered by any of them");
    checks.ExpectEq(recording.completed, 3, "every session is completed");
    checks.Expect(tick.finished.reported.ok(), "the first by the tick's own accounting");
    checks.ExpectEq(tick.other_sessions.size(), std::size_t{2}, "and the other two beside it");
  }

  harness::DeleteSave(client, base, fixture, server.id);
  harness::DeleteDevice(client, base, fixture, device);
}

/// #241: a rom just downloaded has its save restored even when this device
/// synced that save before -- which RomM otherwise reads as the user having
/// deleted it -- and only that rom is asked with `restore_unlisted`.
void Restore(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const harness::Rom& rom) {
  const std::string device = harness::CreateDevice(checks, client, base, fixture, "tick-restore");
  if (device.empty()) {
    return;
  }
  Sandbox sandbox(checks, "tick-restore");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  sandbox.MakeDirs(harness::kSavesDir);
  const std::string slot = harness::UniqueSlot("m10-15-restore");
  const std::string bytes = "the save that goes with the rom " + slot + "\n";
  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, bytes);
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m2-7", staged, "restore.srm",
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "the server holds the save");
    harness::DeleteDevice(client, base, fixture, device);
    return;
  }

  auth::StoredToken token = TokenFor(base, fixture);
  token.device_id = device;
  Recording recording(client);
  const std::string placed = SavePath("restore.srm");
  sync::TickOptions options = OptionsAt(1'757'000'500);
  options.execute.place = [&slot, &placed](const sync::SyncOperation& operation) {
    rommsync::place::Placement placement;
    if (operation.slot.has_value() && *operation.slot == slot) {
      placement.sd_path = placed;
    } else {
      placement.reason = "not this scenario's save";
    }
    return placement;
  };
  options.rom_scope = std::vector<std::int64_t>{rom.id};

  // Synced once: the save is on the card, and this device has a history of it.
  sync::RunTick(recording, *files, token, {}, {}, state::Baseline{}, options);
  checks.ExpectEq(sandbox.Read(placed), bytes, "the first tick restores the save");

  // The rom and its save are deleted. RomM reads the missing save as one the
  // user deleted, and offers nothing.
  std::error_code error;
  std::filesystem::remove(sandbox.Host(placed), error);
  {
    const sync::TickResult tick =
        sync::RunTick(recording, *files, token, {}, {}, state::Baseline{}, options);
    int count = 0;
    checks.Expect(tick.negotiated.ok() && PlannedFor(tick, rom.id, slot, &count) == nullptr,
                  "an ordinary tick does not bring back a save this device synced and lost");
  }

  // Downloaded again: asked about on its own, restoring, and the save is back.
  // A save of its own in another slot rides in the same negotiation, and its
  // upload is that session's, not the first one's.
  const std::string own_slot = harness::UniqueSlot("m10-15-own");
  sandbox.SeedSave(SavePath("restore-own.srm"), "this console's own, in another slot\n");
  sync::ClientSaveState own;
  if (!LocalSaveOnCard(checks, *files, sandbox, rom.id, "restore-own.srm", own_slot, &own)) {
    harness::DeleteSave(client, base, fixture, server.id);
    harness::DeleteDevice(client, base, fixture, device);
    return;
  }
  const std::vector<sync::SaveTarget> targets = {
      {rom.id, own_slot, SavePath("restore-own.srm"), "restore-own.srm"}};
  recording.Clear();
  const std::int64_t other = 900'000'002;
  options.rom_scope = std::vector<std::int64_t>{rom.id, other};
  options.restore_roms = {rom.id};
  {
    const sync::TickResult tick =
        sync::RunTick(recording, *files, token, {own}, targets, state::Baseline{}, options);
    checks.ExpectEq(recording.negotiations.size(), std::size_t{2},
                    "the card's other roms, and the downloaded one apart");
    for (const std::string& body : recording.negotiations) {
      bool restoring = false;
      const std::optional<std::vector<std::int64_t>> scope = ScopeOf(body, &restoring);
      if (scope == std::vector<std::int64_t>{rom.id}) {
        checks.Expect(restoring, "the downloaded rom's negotiation restores");
      } else {
        checks.Expect(!restoring && scope == std::vector<std::int64_t>{other},
                      "and the other one does not, and does not name it");
      }
    }
    int count = 0;
    const sync::SyncOperation* offered = PlannedFor(tick, rom.id, slot, &count);
    checks.Expect(offered != nullptr && offered->action == sync::Action::kDownload && count == 1,
                  "the save is offered once, as a download");
    checks.ExpectEq(sandbox.Read(placed), bytes, "and it is back on the card");
    checks.ExpectEq(recording.completed, 2, "both sessions are completed");

    std::int64_t restoring_session = 0;
    for (std::size_t index = 0; index < recording.negotiations.size(); ++index) {
      bool restoring = false;
      ScopeOf(recording.negotiations[index], &restoring);
      if (restoring && index < recording.sessions.size()) {
        restoring_session = recording.sessions[index];
      }
    }
    checks.Expect(recording.uploads.size() == 1 && restoring_session != 0 &&
                      recording.uploads[0].find("&session_id=" +
                                                std::to_string(restoring_session) + "&") !=
                          std::string::npos,
                  "the save it carried is uploaded under the session that planned it: " +
                      (recording.uploads.empty() ? std::string("no upload")
                                                 : recording.uploads[0]));
  }

  std::vector<std::int64_t> own_rows;
  SaveRowsFor(client, base, fixture, rom.id, own_slot, &own_rows);
  for (const std::int64_t id : own_rows) {
    harness::DeleteSave(client, base, fixture, id);
  }
  harness::DeleteSave(client, base, fixture, server.id);
  harness::DeleteDevice(client, base, fixture, device);
}

// --- oversized (#246) ---------------------------------------------------------
//
// A plan is held for the whole tick, so it is bounded (`sync::kMaxPlanOperations`,
// 128) -- and a server can hold more saves for a card's roms than that without
// the card reporting any of them: another device's library, a desktop RetroArch.
// Refused whole, that plan was refused on every tick, and nothing on the card
// ever synced again. So a chunk whose plan is too large is split and asked
// again, down to one rom, and only a rom whose own saves are past the bound is
// left out -- for that tick, and by name.

/// The slot of the `n`th save this scenario seeds on `rom`. One slot each,
/// because a POST to a slot the server holds overwrites it.
std::string OversizedSlot(char rom, int n) {
  return std::string("m9-20-big-") + rom + "-" + std::to_string(n);
}

/// Every save a previous run of this scenario left on `rom_id`.
void ClearOversized(http::HttpClient& client, const std::string& base, const Fixture& fixture,
                    std::int64_t rom_id) {
  const http::Result listed = client.Send(harness::Authed(
      http::Method::kGet, base + "/api/saves?rom_id=" + std::to_string(rom_id), fixture));
  const json::ParseResult parsed = json::Parse(listed.response.body);
  if (!listed.successful() || !parsed.ok() || !parsed.value.is_array()) {
    return;
  }
  std::string ids;
  for (const json::Value& save : parsed.value.elements()) {
    if (harness::Field(save, "slot").rfind("m9-20-big-", 0) == 0) {
      ids += (ids.empty() ? "" : ",") + std::to_string(harness::Number(save, "id"));
    }
  }
  if (!ids.empty()) {
    harness::PostJson(client, base + "/api/saves/delete", fixture, "{\"saves\":[" + ids + "]}");
  }
}

void Oversized(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture) {
  // Two roms whose saves fit a plan each but not one together, and a third whose
  // own saves do not fit one at all.
  struct Seeded {
    const char* file;
    char label;
    int saves;
    harness::Rom rom;
  };
  Seeded seeded[] = {{"gb240p.gb", 'a', 65, {}},
                     {"nova.nes", 'b', 65, {}},
                     {"240pee.nes", 'c', static_cast<int>(sync::kMaxPlanOperations) + 1, {}}};
  for (Seeded& one : seeded) {
    if (!harness::FindRom(client, base, fixture, one.file, &one.rom)) {
      checks.Expect(false, std::string("the fixture holds ") + one.file);
      return;
    }
    ClearOversized(client, base, fixture, one.rom.id);
  }
  const std::string device =
      harness::CreateDevice(checks, client, base, fixture, "tick-oversized");
  if (device.empty()) {
    return;
  }
  Sandbox sandbox(checks, "tick-oversized");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, "another device's save\n");
  bool uploaded = true;
  for (const Seeded& one : seeded) {
    for (int n = 0; n < one.saves && uploaded; ++n) {
      harness::Save save;
      http::Result raw;
      uploaded = harness::UploadSave(client, base, fixture, one.rom.id, OversizedSlot(one.label, n),
                                     "m2-7", staged, OversizedSlot(one.label, n) + ".srm",
                                     /*with_device=*/false, &save, &raw);
      checks.Expect(uploaded, std::string("the server holds every seeded save: ") + one.file +
                                  " #" + std::to_string(n) + " answered " +
                                  std::to_string(raw.response.status) + " " +
                                  raw.response.body.substr(0, 200));
    }
  }

  auth::StoredToken token = TokenFor(base, fixture);
  token.device_id = device;
  Recording recording(client);
  sync::TickOptions options = OptionsAt(1'757'000'600);
  // Placed nowhere: what is under test is what gets negotiated, not 259 files.
  options.execute.place = [](const sync::SyncOperation&) {
    rommsync::place::Placement placement;
    placement.reason = "not this scenario's to place";
    return placement;
  };
  std::vector<std::int64_t> card;
  for (const Seeded& one : seeded) {
    card.push_back(one.rom.id);
  }
  std::sort(card.begin(), card.end());
  options.rom_scope = card;
  std::size_t executed_plans = 0;
  std::size_t largest_plan = 0;
  options.on_plan = [&](const sync::SyncPlan& plan, const sync::ExecutionReport&) {
    ++executed_plans;
    largest_plan = std::max(largest_plan, plan.operations.size());
  };

  if (uploaded) {
    const sync::TickResult tick =
        sync::RunTick(recording, *files, token, {}, {}, state::Baseline{}, options);
    checks.Expect(tick.outcome != sync::TickOutcome::kRefused,
                  std::string("a server holding more saves than one plan carries is not a tick "
                              "refused whole -- it was every tick, forever: ") +
                      sync::ToString(tick.outcome) + ", " + tick.negotiated.message);
    // Which negotiations named one rom each, and what they planned.
    std::map<std::int64_t, std::size_t> alone;
    for (std::size_t index = 0; index < recording.negotiations.size(); ++index) {
      bool restoring = false;
      const std::optional<std::vector<std::int64_t>> scope =
          ScopeOf(recording.negotiations[index], &restoring);
      if (scope.has_value() && scope->size() == 1 && index < recording.operations.size()) {
        alone[scope->front()] = recording.operations[index];
      }
    }
    for (const Seeded& one : seeded) {
      const auto asked = alone.find(one.rom.id);
      checks.Expect(asked != alone.end(),
                    std::string("the split reaches ") + one.file + " on its own");
      if (asked != alone.end()) {
        checks.ExpectEq(asked->second, static_cast<std::size_t>(one.saves),
                        std::string("and its plan is every save of ") + one.file);
      }
    }
    checks.Expect(tick.outcome == sync::TickOutcome::kPartial,
                  std::string("the rom left out makes the tick partial, not clean: ") +
                      sync::ToString(tick.outcome));
    const std::int64_t big = seeded[2].rom.id;
    checks.Expect(tick.oversized_roms == std::vector<std::int64_t>{big},
                  "and it is named: the rom whose own saves are past the bound, and no other");
    checks.Expect(tick.unasked_roms == std::vector<std::int64_t>{big},
                  "the only rom no plan covered");
    checks.ExpectEq(tick.totals.plans, std::size_t{2},
                    "the two that fit ran one plan each, one after another");
    checks.ExpectEq(executed_plans, std::size_t{2}, "each handed to on_plan before it was freed");
    checks.Expect(largest_plan <= sync::kMaxPlanOperations,
                  "and none of them held more than one plan may: " + std::to_string(largest_plan));
  }

  for (const Seeded& one : seeded) {
    ClearOversized(client, base, fixture, one.rom.id);
  }
  harness::DeleteDevice(client, base, fixture, device);
}

// --- deleted ------------------------------------------------------------------
//
// M10-7 (#233)'s acceptance, on the live 5.4.0: a save this console uploaded is
// deleted through the API -- as RomM's web UI does it, or a RetroArch that
// deleted its `.srm` -- and the next tick is answered `delete`. Removed, the
// save's bytes are under `.backup/` and the file is off the card; kept, the file
// is exactly as it was. Either way the slot's baseline row is gone, and the tick
// after that uploads nothing: the server holds no row for the slot at the end.

void Deleted(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const harness::Rom& rom,
             rommsync::config::OnServerDelete mode) {
  const bool keep = mode == rommsync::config::OnServerDelete::kKeep;
  const std::string what = keep ? "keep" : "remove";
  Sandbox sandbox(checks, keep ? "tick-deleted-keep" : "tick-deleted");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  UploadFixture arranged;
  if (!ArrangeUpload(checks, *files, sandbox, rom, keep ? "m10-7-keep" : "m10-7-remove",
                     &arranged)) {
    return;
  }
  const auth::StoredToken token = TokenFor(base, fixture);

  // 1. The save goes up, as any save only this console has does.
  const sync::TickResult first = sync::RunTick(client, *files, token, {arranged.local},
                                               arranged.targets, /*previous=*/{},
                                               OptionsAt(1'757'000'900));
  checks.Expect(first.executed.operations.size() == 1 &&
                    first.executed.operations[0].outcome == sync::OperationOutcome::kUploaded,
                what + ": the first tick uploads the save");
  std::vector<std::int64_t> ids;
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, arranged.slot, &ids), 1,
                  what + ": and the server holds it");

  // 2. Deleted on the server, and the next tick is told so.
  for (const std::int64_t id : ids) {
    harness::DeleteSave(client, base, fixture, id);
  }
  if (!keep) {
    // 2a. The tick asks the server's capabilities, not the plan alone (#228):
    // a `delete` from a server that does not offer one is not obeyed.
    sync::TickOptions ungated = OptionsAt(1'757'000'910);
    ungated.server.delete_action = false;
    const sync::TickResult refused = sync::RunTick(client, *files, token, {arranged.local},
                                                   arranged.targets, {}, ungated);
    checks.ExpectEq(refused.executed.not_understood, 1,
                    "remove: a delete the server's capabilities do not include is not obeyed");
    checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), arranged.bytes,
                    "remove: and the save is untouched");

    // 2b. The player wrote to the save after the scan reported it. The ruling
    // was about the reported bytes, so these stay on the card.
    const std::string progress = "progress made after the scan\n";
    sandbox.Reseed(SavePath(arranged.name), progress);
    const sync::TickResult moved = sync::RunTick(client, *files, token, {arranged.local},
                                                 arranged.targets, {}, OptionsAt(1'757'000'911));
    checks.Expect(moved.executed.operations.size() == 1 &&
                      moved.executed.operations[0].error == sync::OperationError::kUnverified,
                  "remove: a save that changed since the scan is not removed");
    checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), progress,
                    "remove: and its new bytes are on the card");
    sandbox.Reseed(SavePath(arranged.name), arranged.bytes);
  }
  sync::TickOptions options = OptionsAt(1'757'000'901);
  options.execute.on_server_delete = mode;
  const sync::TickResult second =
      sync::RunTick(client, *files, token, {arranged.local}, arranged.targets,
                    StoredBaseline(*files).value, options);
  checks.Expect(second.outcome == sync::TickOutcome::kCompleted,
                what + ": the tick that meets the delete completes: " +
                    sync::ToString(second.outcome) +
                    (second.executed.warnings.empty() ? std::string()
                                                      : " -- " + second.executed.warnings[0]));
  checks.ExpectEq(second.negotiated.plan.total_delete, std::int64_t{1},
                  what + ": the server counts one delete");
  if (second.executed.operations.size() != 1) {
    checks.Expect(false, what + ": the plan carried the one operation this run created, got " +
                             std::to_string(second.executed.operations.size()));
  } else {
    const sync::OperationResult& done = second.executed.operations[0];
    checks.Expect(done.action == sync::Action::kDelete, what + ": the operation is a delete");
    checks.Expect(done.outcome ==
                      (keep ? sync::OperationOutcome::kKept : sync::OperationOutcome::kRemoved),
                  what + ": obeyed as configured: " + sync::ToString(done.outcome) + " -- " +
                      done.message);
    checks.ExpectEq(second.finished.counts.operations_completed, 1,
                    what + ": and counted as completed in the session it closes");
  }
  if (keep) {
    checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), arranged.bytes,
                    "keep: the save is on the card, unchanged");
    checks.Expect(!sandbox.HasBackupOf(arranged.bytes), "keep: and nothing was backed up");
  } else {
    checks.Expect(!sandbox.Exists(SavePath(arranged.name)), "remove: the save is off the card");
    checks.Expect(sandbox.HasBackupOf(arranged.bytes),
                  "remove: and its bytes are under .backup/, which is the hard rule");
  }
  if (keep) {
    const state::LoadedBaseline stored = StoredBaseline(*files);
    const state::SaveRecord* row = stored.value.Find(rom.id, arranged.slot);
    checks.Expect(row != nullptr && row->deleted_on_server &&
                      row->content_hash == arranged.local.content_hash,
                  "keep: state.db remembers the bytes RomM deleted, so they are not offered again");
  } else {
    checks.Expect(StoredBaseline(*files).value.Find(rom.id, arranged.slot) == nullptr,
                  "remove: the slot is no longer tracked in state.db");
  }

  // 3. The tick after: what the scan reports now -- the kept file, or nothing.
  // The kept one is the hard case: the emulator wrote the same bytes back on
  // closing, so its mtime is now later than the removal, and RomM by itself
  // would read that as progress made since and plan an upload.
  std::vector<sync::ClientSaveState> reported;
  std::vector<sync::SaveTarget> targets;
  if (keep) {
    std::error_code error;
    std::filesystem::last_write_time(
        sandbox.Host(SavePath(arranged.name)),
        std::filesystem::file_time_type::clock::now() + std::chrono::hours{1}, error);
    sync::ClientSaveState rewritten;
    if (LocalSaveOnCard(checks, *files, sandbox, rom.id, arranged.name, arranged.slot,
                        &rewritten)) {
      checks.Expect(rewritten.updated_at > arranged.local.updated_at,
                    "keep: the rewrite moved the mtime past the removal");
      reported.push_back(rewritten);
    }
    targets = arranged.targets;
  }
  sync::TickOptions after = OptionsAt(1'757'000'902);
  after.execute.on_server_delete = mode;
  const sync::TickResult third = sync::RunTick(client, *files, token, reported, targets,
                                               StoredBaseline(*files).value, after);
  checks.Expect(third.outcome == sync::TickOutcome::kCompleted,
                what + ": the following tick completes: " + sync::ToString(third.outcome));
  int for_slot = 0;
  for (const sync::OperationResult& done : third.executed.operations) {
    if (done.slot == arranged.slot) {
      ++for_slot;
      checks.Expect(done.outcome != sync::OperationOutcome::kUploaded,
                    what + ": the following tick does not upload the deleted save");
    }
  }
  checks.ExpectEq(for_slot, 0, what + ": the following tick plans nothing for the slot");
  if (keep) {
    checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), arranged.bytes,
                    "keep: and the save is still on the card");
  }
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, arranged.slot, nullptr), 0,
                  what + ": and the server holds no row for the slot -- nothing went back up");

  if (keep) {
    // 4. The player plays on. Those are new bytes RomM never ruled on -- the
    // copy it deleted is still not offered, these are -- so they go up.
    sandbox.Reseed(SavePath(arranged.name), "progress made after the delete\n");
    sync::ClientSaveState played;
    if (LocalSaveOnCard(checks, *files, sandbox, rom.id, arranged.name, arranged.slot, &played)) {
      const sync::TickResult fourth =
          sync::RunTick(client, *files, token, {played}, arranged.targets,
                        StoredBaseline(*files).value, after);
      checks.Expect(fourth.executed.operations.size() == 1 &&
                        fourth.executed.operations[0].outcome ==
                            sync::OperationOutcome::kUploaded,
                    "keep: new progress on a kept save is offered, and uploaded");
      const state::LoadedBaseline synced = StoredBaseline(*files);
      const state::SaveRecord* row = synced.value.Find(rom.id, arranged.slot);
      checks.Expect(row != nullptr && !row->deleted_on_server,
                    "keep: and the save is tracked again");
    }
    std::vector<std::int64_t> progress;
    SaveRowsFor(client, base, fixture, rom.id, arranged.slot, &progress);
    for (const std::int64_t id : progress) {
      harness::DeleteSave(client, base, fixture, id);
    }
  }
}

// --- download -----------------------------------------------------------------
//
// The stage that can destroy a save, and therefore the one the whole ordering
// exists for: stage, verify, back up, commit. Every fault below has to leave the
// card holding the bytes it started with.

void DownloadFault(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                   const Fixture& fixture, const harness::Rom& rom, const std::string& mode,
                   sync::OperationError error, bool then_normally, const std::string& what) {
  Sandbox sandbox(checks, "tick-download");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  DownloadFixture arranged;
  if (!ArrangeDownload(checks, client, base, fixture, sandbox, rom, "m2-7-download", &arranged)) {
    return;
  }

  sync::TickOptions options = OptionsAt(1'757'000'500);
  Impatient(&options);
  {
    // Scoped to the content path, so the negotiation and the completion around
    // it are untouched and the damage is unambiguously the download's.
    const std::string spec = R"({"mode":")" + mode + R"(","path":")" +
                             arranged.server.ContentPath() + R"(")" +
                             (mode == "stall" ? R"(,"seconds":3})"
                                              : R"(,"status":500,"count":4})");
    harness::Fault fault(checks, client, base, spec);
    const sync::TickResult tick = sync::RunTick(client, *files, TokenFor(base, fixture),
                                                {arranged.local}, arranged.targets,
                                                /*previous=*/{}, options);
    if (tick.executed.operations.size() != 1) {
      checks.Expect(false, what + ": the plan carried one operation, got " +
                               std::to_string(tick.executed.operations.size()));
    } else {
      const sync::OperationResult& result = tick.executed.operations[0];
      checks.Expect(result.action == sync::Action::kDownload,
                    std::string(what) + ": the plan was a download, got " +
                        sync::ToString(result.action));
      checks.Expect(result.error == error,
                    what + ": named " + sync::ToString(error) + ", got " +
                        sync::ToString(result.error) + " -- " + result.message);
    }
    checks.ExpectEq(tick.executed.failed, 1, what + ": counted failed, once");
    checks.Expect(tick.outcome == sync::TickOutcome::kPartial,
                  std::string(what) + ": the tick is partial rather than lost: " +
                      sync::ToString(tick.outcome));

    // The baseline must not have moved this save forward: the bytes on the card
    // are still the old ones, and a row claiming otherwise is what makes the
    // *next* tick skip the save that needs the work (sync_finish.hpp).
    const state::LoadedBaseline stored = StoredBaseline(*files);
    checks.Expect(stored.value.Find(rom.id, arranged.slot) == nullptr,
                  std::string(what) +
                      ": and the failed save was not advanced, so the next tick retries it");
  }

  SaveSurvived(checks, sandbox, arranged.name, arranged.previous, what);

  if (then_normally) {
    const sync::TickResult next =
        sync::RunTick(client, *files, TokenFor(base, fixture), {arranged.local},
                      arranged.targets, /*previous=*/{}, OptionsAt(1'757'000'501));
    checks.Expect(next.outcome == sync::TickOutcome::kCompleted,
                  what + ": and the next tick runs normally: " + sync::ToString(next.outcome) +
                      (next.executed.warnings.empty() ? std::string()
                                                      : " -- " + next.executed.warnings[0]));
    checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), arranged.server_bytes,
                    what + ": with the server's bytes on the card");
    checks.Expect(sandbox.HasBackupOf(arranged.previous),
                  what + ": and the bytes it replaced under .backup/");
    NoLeftovers(checks, sandbox, SavePath(arranged.name), what + ", after the next tick");
  }

  harness::DeleteSave(client, base, fixture, arranged.server.id);
}

/// The card, rather than the server, refusing part of a download (M9-13, #202).
///
/// `DownloadFault`'s assertions, with the damage moved from the network to the
/// card: `faulty::Disk` refuses the writes aimed at `aimed_at` with `error`, so
/// the transfer (`.part`) or the backup the overwrite depends on (`/.backup/`)
/// is what fails. Either way the save on the card is the one the tick started
/// with, the baseline has not moved, and the operation says *why* in words a
/// user can act on.
void DiskFault(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture, const harness::Rom& rom, const std::string& aimed_at,
               int error, sync::OperationError expected, const std::string& says,
               const std::string& what) {
  Sandbox sandbox(checks, "tick-disk");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  DownloadFixture arranged;
  if (!ArrangeDownload(checks, client, base, fixture, sandbox, rom, "m9-13-disk", &arranged)) {
    return;
  }

  {
    faulty::Disk card({aimed_at, error, /*after=*/0, /*gone=*/false});
    const sync::TickResult tick = sync::RunTick(client, *files, TokenFor(base, fixture),
                                                {arranged.local}, arranged.targets,
                                                /*previous=*/{}, OptionsAt(1'757'000'600));
    checks.Expect(card.refusals() > 0, what + ": the card refused the write it was aimed at");
    if (tick.executed.operations.size() != 1) {
      checks.Expect(false, what + ": the plan carried one operation, got " +
                               std::to_string(tick.executed.operations.size()));
    } else {
      const sync::OperationResult& result = tick.executed.operations[0];
      checks.Expect(result.action == sync::Action::kDownload,
                    std::string(what) + ": the plan was a download, got " +
                        sync::ToString(result.action));
      checks.Expect(result.error == expected,
                    what + ": named " + sync::ToString(expected) + ", got " +
                        sync::ToString(result.error) + " -- " + result.message);
      checks.Expect(result.message.find(says) != std::string::npos,
                    what + ": and says \"" + says + "\" -- got \"" + result.message + "\"");
    }
    checks.ExpectEq(tick.executed.failed, 1, what + ": counted failed, once");
    const state::LoadedBaseline stored = StoredBaseline(*files);
    checks.Expect(stored.value.Find(rom.id, arranged.slot) == nullptr,
                  what + ": and the failed save was not advanced, so the next tick retries it");
  }

  SaveSurvived(checks, sandbox, arranged.name, arranged.previous, what);

  // Room on the card again: the same plan completes, backup first.
  const sync::TickResult next =
      sync::RunTick(client, *files, TokenFor(base, fixture), {arranged.local}, arranged.targets,
                    /*previous=*/{}, OptionsAt(1'757'000'601));
  checks.Expect(next.outcome == sync::TickOutcome::kCompleted,
                what + ": and with room again the next tick completes: " +
                    sync::ToString(next.outcome));
  checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), arranged.server_bytes,
                  what + ": with the server's bytes on the card");
  checks.Expect(sandbox.HasBackupOf(arranged.previous),
                what + ": and the bytes it replaced under .backup/");
  NoLeftovers(checks, sandbox, SavePath(arranged.name), what + ", after the next tick");

  harness::DeleteSave(client, base, fixture, arranged.server.id);
}

/// The 401 arriving at a download rather than at an upload: the plan stops, the
/// save is untouched, and the *tick* -- not this operation -- is what reports the
/// answer `auth::Gate` counts (M1-4).
void DownloadRevoked(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                     const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-download-401");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  DownloadFixture arranged;
  if (!ArrangeDownload(checks, client, base, fixture, sandbox, rom, "m2-7-revoked", &arranged)) {
    return;
  }

  sync::TickOptions options = OptionsAt(1'757'000'600);
  Impatient(&options);
  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"status","status":401,"path":")" +
                             arranged.server.ContentPath() + R"(","count":4})");
    const sync::TickResult tick = sync::RunTick(client, *files, TokenFor(base, fixture),
                                                {arranged.local}, arranged.targets,
                                                /*previous=*/{}, options);
    checks.Expect(tick.executed.unauthorized,
                  "a 401 at a download stops the plan where it stands");
    checks.Expect(tick.outcome == sync::TickOutcome::kUnauthorized,
                  std::string("and the tick says so in one word: ") +
                      sync::ToString(tick.outcome));
    // ...and the *credentials* verdict is the completion's, not this
    // operation's. One 401 is not a verdict -- `harness.expired` shows a live
    // server answering one mid-flow and then accepting the same token -- so a
    // tick whose accounting call went through on that token reports the
    // acceptance, which is the evidence `auth::Gate` counts consecutive
    // rejections against (M1-4, auth_gate.hpp). The stop above is separate and
    // is about spending requests, not about the pairing.
    checks.Expect(tick.answer == auth::Answer::kAccepted,
                  std::string("the completion that followed took the same token, and that is "
                              "what auth::Gate is told: ") +
                      auth::ToString(tick.answer));
    checks.Expect(tick.finished.reported.ok(),
                  "which is only meaningful because the completion really did land: " +
                      tick.finished.reported.message);
    checks.ExpectEq(tick.executed.failed, 1,
                    "the operation that met it is counted failed -- it was attempted and did not "
                    "happen, which is what keeps operations_failed honest");
    checks.Expect(tick.finished.stored.ok(),
                  "and the baseline is on the card anyway: " + tick.finished.stored.message);
  }

  SaveSurvived(checks, sandbox, arranged.name, arranged.previous, "a 401 at a download");
  harness::DeleteSave(client, base, fixture, arranged.server.id);
}

// --- complete -----------------------------------------------------------------
//
// The accounting call. It is deliberately not the commit point -- the transfers
// already landed on the server -- so a `complete` that never arrives must cost a
// row in a history a user reads and nothing else. In particular it must not take
// the baseline with it.

void CompleteFault(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                   const Fixture& fixture, const harness::Rom& rom, const std::string& spec,
                   bool then_normally, const std::string& what) {
  Sandbox sandbox(checks, "tick-complete");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  UploadFixture arranged;
  if (!ArrangeUpload(checks, *files, sandbox, rom, "m2-7-complete", &arranged)) {
    return;
  }

  sync::TickOptions options = OptionsAt(1'757'000'700);
  Impatient(&options);
  {
    harness::Fault fault(checks, client, base, spec);
    const sync::TickResult tick = sync::RunTick(client, *files, TokenFor(base, fixture),
                                                {arranged.local}, arranged.targets,
                                                /*previous=*/{}, options);
    checks.ExpectEq(tick.executed.completed, 1, what + ": the upload itself completed");
    checks.Expect(!tick.finished.reported.ok(),
                  what + ": and the accounting did not, which is what this scenario damages");
    checks.Expect(tick.outcome == sync::TickOutcome::kUnreported,
                  what + ": so the tick is unreported rather than failed: " +
                      sync::ToString(tick.outcome));

    // The reason the order in `FinishTick` is structural: lose this and the next
    // tick re-uploads a save the server already has, and RomM stamps it as a new
    // row.
    checks.Expect(tick.finished.stored.ok(),
                  what + ": the baseline reached the card first: " +
                      tick.finished.stored.message);
    const state::LoadedBaseline stored = StoredBaseline(*files);
    checks.Expect(stored.value.Find(rom.id, arranged.slot) != nullptr,
                  what + ": with the row for the save that was uploaded");
  }

  SaveSurvived(checks, sandbox, arranged.name, arranged.bytes, what);

  if (then_normally) {
    // The save is already on the server *with* this device's sync row -- the
    // upload carried `device_id` -- so the next negotiation plans nothing for
    // it. "Runs normally" is exactly that: a tick that completes, having found
    // there is no work, rather than one that re-uploads what the lost accounting
    // makes look unsent.
    const sync::TickResult next =
        sync::RunTick(client, *files, TokenFor(base, fixture), {arranged.local},
                      arranged.targets, /*previous=*/{}, OptionsAt(1'757'000'701));
    checks.Expect(next.outcome == sync::TickOutcome::kCompleted,
                  what + ": and the next tick runs normally: " + sync::ToString(next.outcome) +
                      (next.executed.warnings.empty() ? std::string()
                                                      : " -- " + next.executed.warnings[0]));
    checks.ExpectEq(next.executed.failed, 0, what + ": with nothing failed on the way");
  }

  std::vector<std::int64_t> ids;
  SaveRowsFor(client, base, fixture, rom.id, arranged.slot, &ids);
  for (const std::int64_t id : ids) {
    harness::DeleteSave(client, base, fixture, id);
  }
  harness::CloseOpenSessions(client, base, fixture);
}

/// A session something other than this tick already closed, answered with
/// RomM's own `400 "Session is already <status>"`.
///
/// Forced by the proxy because a healthy 5.4.0 no longer produces it for a
/// session this client negotiated: negotiate cancels nothing any more (#235),
/// and the scheduled cleanup's FAILED is the one closed state `complete` may
/// reopen. What is left is a session closed on purpose -- CANCELLED by a 5.2.0
/// before the server was upgraded, FAILED by whatever ran it -- and both are
/// terminal: the counts will never be recorded, a retry is refused the same
/// way, and the transfers already landed. So the tick is completed, not
/// unreported, and the completion is tried once.
void CompleteClosed(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                    const Fixture& fixture, const harness::Rom& rom) {
  for (const std::string status : {"CANCELLED", "FAILED"}) {
    const std::string what = "a session already " + status;
    Sandbox sandbox(checks, "tick-closed-" + status);
    const std::unique_ptr<fs::FileSystem> files =
        rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    UploadFixture arranged;
    if (!ArrangeUpload(checks, *files, sandbox, rom, "m10-9-closed", &arranged)) {
      continue;
    }

    sync::TickOptions options = OptionsAt(1'757'000'750);
    Impatient(&options);
    {
      harness::Fault fault(
          checks, client, base,
          R"({"mode":"status","status":400,"body":"{\"detail\":\"Session is already )" + status +
              R"(\"}","path":"/api/sync/sessions","count":9})");
      const sync::TickResult tick = sync::RunTick(client, *files, TokenFor(base, fixture),
                                                  {arranged.local}, arranged.targets,
                                                  /*previous=*/{}, options);
      checks.ExpectEq(tick.executed.completed, 1, what + ": the upload itself completed");
      checks.Expect(tick.finished.reported.error == sync::CompleteError::kSuperseded,
                    what + ": the completion is named as closed elsewhere, got " +
                        sync::ToString(tick.finished.reported.error) + " -- " +
                        tick.finished.reported.message);
      checks.ExpectEq(tick.finished.reported.attempts, 1, what + ": and is not retried");
      checks.Expect(tick.session_gone, what + ": the session is not this tick's to close");
      checks.Expect(tick.outcome == sync::TickOutcome::kCompleted,
                    what + ": so the tick is completed rather than unreported: " +
                        sync::ToString(tick.outcome));
      checks.Expect(tick.finished.stored.ok(),
                    what + ": with the baseline on the card: " + tick.finished.stored.message);
    }
    SaveSurvived(checks, sandbox, arranged.name, arranged.bytes, what);

    std::vector<std::int64_t> ids;
    SaveRowsFor(client, base, fixture, rom.id, arranged.slot, &ids);
    for (const std::int64_t id : ids) {
      harness::DeleteSave(client, base, fixture, id);
    }
    harness::CloseOpenSessions(client, base, fixture);
  }
}

// --- resumes ------------------------------------------------------------------
//
// The acceptance in full: a reset mid-download, then a second tick that finishes
// the job. The save ends correct, the server ends with one row rather than two,
// and nothing between the two ticks had to be reconciled by hand.

void Resumes(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-resumes");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  DownloadFixture arranged;
  if (!ArrangeDownload(checks, client, base, fixture, sandbox, rom, "m2-7-resumes", &arranged)) {
    return;
  }

  sync::TickOptions options = OptionsAt(1'757'000'800);
  Impatient(&options);
  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"drop","bytes":4,"path":")" +
                             arranged.server.ContentPath() + R"("})");
    const sync::TickResult interrupted =
        sync::RunTick(client, *files, TokenFor(base, fixture), {arranged.local}, arranged.targets,
                      /*previous=*/{}, options);
    checks.ExpectEq(interrupted.executed.failed, 1, "the reset cost the operation");
    checks.Expect(interrupted.outcome == sync::TickOutcome::kPartial,
                  std::string("and the tick, which is partial rather than lost: ") +
                      sync::ToString(interrupted.outcome));
  }
  SaveSurvived(checks, sandbox, arranged.name, arranged.previous, "after the reset");

  // The second tick. It negotiates again rather than re-running the plan it
  // already had -- the server is the arbiter of a state it may have moved on
  // from -- and that is what `RunTick` does by construction.
  state::Baseline previous = StoredBaseline(*files).value;
  sync::TickOptions second = OptionsAt(1'757'000'801);
  Impatient(&second);
  const sync::TickResult recovered =
      sync::RunTick(client, *files, TokenFor(base, fixture), {arranged.local}, arranged.targets,
                    std::move(previous), second);

  checks.Expect(recovered.outcome == sync::TickOutcome::kCompleted,
                std::string("the next tick finishes the job: ") +
                    sync::ToString(recovered.outcome) +
                    (recovered.executed.warnings.empty() ? std::string()
                                                         : " -- " + recovered.executed.warnings[0]));
  checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), arranged.server_bytes,
                  "with the server's bytes on the card");
  checks.Expect(sandbox.HasBackupOf(arranged.previous),
                "and the bytes it replaced under .backup/, which is the hard rule");
  NoLeftovers(checks, sandbox, SavePath(arranged.name), "after the second tick");

  // The claim the duplication hazard earns: the interrupted tick sent nothing,
  // so there is still exactly one row.
  const int rows = SaveRowsFor(client, base, fixture, rom.id, arranged.slot, nullptr);
  checks.ExpectEq(rows, 1, "and the server holds one row for the slot, not two");

  const state::LoadedBaseline stored = StoredBaseline(*files);
  const state::SaveRecord* row = stored.value.Find(rom.id, arranged.slot);
  checks.Expect(row != nullptr, "the baseline advanced for the save that was downloaded");
  if (row != nullptr) {
    checks.ExpectEq(row->content_hash, crypto::Md5Hex(arranged.server_bytes),
                    "and it holds the digest of the bytes that are actually on the card");
  }

  harness::DeleteSave(client, base, fixture, arranged.server.id);
}

// --- relaunched ---------------------------------------------------------------
//
// M6-2 (#33). ovl-sysmodules stops a dynamic sysmodule with
// `pmshellTerminateProgram`: the process is killed where it stands, with no
// shutdown hook, and its "restart" is a terminate followed immediately by a
// launch. So the tick has to survive not only a dropped connection but the loss
// of everything the process was holding.
//
// `Resumes` is the same interruption without the kill -- it keeps its client,
// its filesystem and the baseline it read. This one throws all three away
// between the two ticks, exactly as a relaunch does, and the second tick starts
// from nothing but the card. What that adds is the three claims a relaunch
// makes: the baseline is on the card and is accurate, the kill left no lock or
// pidfile a fresh process would trip over, and the save is intact with its
// backup beside it.

void Relaunched(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-relaunched");
  DownloadFixture arranged;
  {
    const std::unique_ptr<fs::FileSystem> files =
        rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    if (!ArrangeDownload(checks, client, base, fixture, sandbox, rom, "m6-2-relaunched",
                         &arranged)) {
      return;
    }

    sync::TickOptions options = OptionsAt(1'757'000'900);
    Impatient(&options);
    harness::Fault fault(checks, client, base,
                         R"({"mode":"drop","bytes":4,"path":")" +
                             arranged.server.ContentPath() + R"("})");
    const sync::TickResult interrupted =
        sync::RunTick(client, *files, TokenFor(base, fixture), {arranged.local}, arranged.targets,
                      /*previous=*/{}, options);
    checks.Expect(interrupted.outcome == sync::TickOutcome::kPartial,
                  std::string("the reset costs the operation and not the tick: ") +
                      sync::ToString(interrupted.outcome));
    // ...and here the process dies. `files` goes out of scope with it; nothing
    // below reuses anything the first tick held.
  }
  SaveSurvived(checks, sandbox, arranged.name, arranged.previous, "after the kill");

  // What a relaunched process finds. It is a *fresh* filesystem over the same
  // card, and the baseline is read off `state.db` rather than carried in memory
  // -- which is the point: a tick killed mid-plan must leave the card able to
  // answer that question on its own.
  const std::unique_ptr<fs::FileSystem> relaunched =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  // No lock file, no pidfile, nothing a second instance would read as "another
  // one is running". ovl-sysmodules' restart is terminate-then-launch with no
  // gap, so a process that wrote one would refuse to start after every toggle.
  for (const auto& [path, contents] : Snapshot(sandbox)) {
    (void)contents;
    checks.Expect(path.find(".lock") == std::string::npos && path.find(".pid") == std::string::npos,
                  "the killed tick left no lock or pidfile behind: " + path);
  }

  const state::LoadedBaseline read_back = StoredBaseline(*relaunched);
  checks.Expect(read_back.value.Find(rom.id, arranged.slot) == nullptr,
                "and no baseline row for a save that never landed -- the next tick plans it again");

  sync::TickOptions second = OptionsAt(1'757'000'901);
  Impatient(&second);
  const sync::TickResult recovered =
      sync::RunTick(client, *relaunched, TokenFor(base, fixture), {arranged.local},
                    arranged.targets, state::Baseline(read_back.value), second);

  checks.Expect(recovered.outcome == sync::TickOutcome::kCompleted,
                std::string("the relaunched engine finishes the job: ") +
                    sync::ToString(recovered.outcome));
  checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), arranged.server_bytes,
                  "with the server's bytes on the card");
  checks.Expect(sandbox.HasBackupOf(arranged.previous),
                "and the bytes it replaced under .backup/, which is hard rule 2");
  NoLeftovers(checks, sandbox, SavePath(arranged.name), "after the relaunched tick");

  // The baseline the relaunch produced, read once more with a third filesystem:
  // accurate, and about the bytes that are actually on the card.
  const std::unique_ptr<fs::FileSystem> next_boot =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const state::LoadedBaseline stored = StoredBaseline(*next_boot);
  const state::SaveRecord* row = stored.value.Find(rom.id, arranged.slot);
  checks.Expect(row != nullptr, "the baseline advanced across the relaunch");
  if (row != nullptr) {
    checks.ExpectEq(row->content_hash, crypto::Md5Hex(arranged.server_bytes),
                    "and holds the digest of the bytes the card now has");
  }

  // One row, not two: the killed tick sent nothing it could not account for.
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, arranged.slot, nullptr), 1,
                  "and the server holds one row for the slot");

  harness::DeleteSave(client, base, fixture, arranged.server.id);
}

}  // namespace

// --- newer on server ------------------------------------------------------------
//
// #232's acceptance, against a real RomM: a second device uploads to the slot
// between this tick's negotiate and its upload, and the tick ends in a download
// or a conflict -- never a blind upload over that device's save -- with the
// local bytes under `.backup/` first. Both run under `crash::Recorder`, so the
// overwrite the resolution makes is held to the ordering every other overwrite
// is: a power cut at any call leaves the old bytes, or the new ones beside a
// backup of the old (docs/SYNC_PROTOCOL.md#backups).
//
// "Another device" is an upload with no `device_id`, which RomM records against
// no device -- the same thing RetroArch's or Argosy's would be to this one. It
// lands after `PassASecond`, because a version written in the same second as
// the head it follows does not become the head (#227's same-second tie), and
// this would then be testing the tie.

/// Lets every request through, except that the first upload waits for
/// `before` -- the other device's -- to land first.
class Interposed final : public http::HttpClient {
 public:
  Interposed(http::HttpClient& inner, std::function<void()> before)
      : inner_(inner), before_(std::move(before)) {}

  http::Result Send(const http::Request& request) override {
    if (before_ != nullptr && request.method == http::Method::kPost &&
        request.url.find("/api/saves?") != std::string::npos) {
      const std::function<void()> run = std::move(before_);
      before_ = nullptr;
      run();
      fired = true;
    }
    return inner_.Send(request);
  }
  http::Result Download(const http::Request& request, const http::DownloadTarget& into) override {
    return inner_.Download(request, into);
  }

  bool fired = false;

 private:
  http::HttpClient& inner_;
  std::function<void()> before_;
};

/// A slot this device synced at `synced`, holding `local` on the card now.
struct SyncedSlot {
  std::string slot;
  std::string name = "tick-newer.srm";
  harness::Save mine;
  sync::ClientSaveState local;
  std::vector<sync::SaveTarget> targets;
  state::Baseline previous;
};

bool ArrangeSynced(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                   const Fixture& fixture, Sandbox& sandbox, const harness::Rom& rom,
                   std::string_view label, const std::string& synced, const std::string& local,
                   SyncedSlot* out) {
  out->slot = harness::UniqueSlot(label);
  const std::string staged = sandbox.Host("/config/rommsync/synced.srm");
  rig::WriteFile(staged, synced);
  if (!harness::UploadSave(client, base, fixture, rom.id, out->slot, "m2-7", staged, out->name,
                           /*with_device=*/true, &out->mine)) {
    checks.Expect(false, "this device's copy was stored, with its sync row");
    return false;
  }
  sandbox.SeedSave(SavePath(out->name), local);
  // Reported newer than anything the server holds, so the plan for it is an
  // upload -- which is the operation the guard is about.
  out->local = harness::LocalSave(rom.id, out->name, out->slot, "m2-7", crypto::Md5Hex(local),
                                  std::chrono::system_clock::now() + std::chrono::seconds{300},
                                  static_cast<std::int64_t>(local.size()));
  out->targets = {{rom.id, out->slot, SavePath(out->name), out->name}};

  state::SaveRecord row;
  row.rom_id = rom.id;
  row.slot = out->slot;
  row.content_hash = crypto::Md5Hex(synced);
  row.mtime = sync::Timestamp{} + std::chrono::seconds{1'757'000'000};
  row.file_size_bytes = static_cast<std::int64_t>(synced.size());
  row.server_content_hash = crypto::Md5Hex(synced);
  out->previous.Set(row);
  return true;
}

/// Another device's upload to `slot`, a second after whatever came before it.
bool OtherDeviceUploads(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                        const Fixture& fixture, const harness::Rom& rom, const std::string& slot,
                        const std::string& name, const std::string& staged, harness::Save* out) {
  harness::PassASecond();
  const bool stored = harness::UploadSave(client, base, fixture, rom.id, slot, "m2-7", staged,
                                          name, /*with_device=*/false, out);
  checks.Expect(stored, "the other device's upload landed");
  return stored;
}

/// Bytes unchanged since this device last synced them, and a head another
/// device wrote -- twice: once before the tick, so negotiate plans the upload
/// #227 found every device meets after another one writes, and once more inside
/// it, so the head the plan named is no longer the head. The tick must take the
/// newest, as a download.
void NewerDownload(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                   const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-newer-download");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string mine = "the bytes this device synced\n";
  const std::string before = "another device's save, before the tick\n";
  const std::string during = "another device's save, during the tick\n";

  SyncedSlot arranged;
  if (!ArrangeSynced(checks, client, base, fixture, sandbox, rom, "m10-6-download", mine, mine,
                     &arranged)) {
    return;
  }
  std::vector<std::int64_t> planted = {arranged.mine.id};
  const auto sweep = [&]() {
    for (const std::int64_t id : planted) {
      harness::DeleteSave(client, base, fixture, id);
    }
  };

  const std::string first = sandbox.Host("/config/rommsync/before.srm");
  const std::string second = sandbox.Host("/config/rommsync/during.srm");
  rig::WriteFile(first, before);
  rig::WriteFile(second, during);
  harness::Save earlier;
  if (!OtherDeviceUploads(checks, client, base, fixture, rom, arranged.slot, arranged.name, first,
                          &earlier)) {
    sweep();
    return;
  }
  planted.push_back(earlier.id);

  harness::Save later;
  Interposed racing(client, [&]() {
    if (OtherDeviceUploads(checks, client, base, fixture, rom, arranged.slot, arranged.name,
                           second, &later)) {
      planted.push_back(later.id);
    }
  });

  sync::TickResult tick;
  {
    crash::Recorder recorder(sandbox.root());
    tick = sync::RunTick(racing, *files, TokenFor(base, fixture), {arranged.local},
                         arranged.targets, arranged.previous, OptionsAt(1'757'000'600));
    crash::ExpectSurvives(checks, recorder,
                          {SavePath(arranged.name), mine, during, harness::kBackupDir},
                          rig::ScratchDir() + "/crash-newer-download",
                          "a 409 resolved as a download");
  }

  checks.Expect(racing.fired, "the other device's upload landed between negotiate and upload");
  checks.ExpectEq(tick.resolved.refused, std::size_t{1}, "the slot guard refused the upload");
  checks.Expect(tick.resolved.renegotiated, "the tick asked the server again, once");
  checks.ExpectEq(tick.resolved.resolved, std::size_t{1}, "and resolved the refusal");
  checks.Expect(tick.outcome == sync::TickOutcome::kCompleted,
                std::string("the tick completed: ") + sync::ToString(tick.outcome) +
                    (tick.executed.warnings.empty() ? std::string()
                                                    : " -- " + tick.executed.warnings[0]));
  if (tick.executed.operations.size() == 1 && tick.negotiated.plan.operations.size() == 1) {
    const sync::OperationResult& done = tick.executed.operations[0];
    checks.Expect(done.outcome == sync::OperationOutcome::kDownloaded,
                  std::string("unchanged bytes take the head as a download: ") +
                      sync::ToString(done.outcome) + " -- " + done.message);
    checks.Expect(tick.negotiated.plan.operations[0].save_id == std::optional<std::int64_t>(later.id),
                  "of the head as it is now, not the one the first plan named");
  } else {
    checks.Expect(false, "the tick carries the one operation this run arranged");
  }
  checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), during,
                  "the card holds the other device's newest save");
  checks.Expect(sandbox.HasBackupOf(mine), "and this device's bytes are under .backup/");

  // Never a blind upload: the slot holds this device's first copy and the
  // other device's two, and nothing else.
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, arranged.slot, nullptr), 3,
                  "nothing was uploaded over the other device's save");

  // The download was confirmed against the head, so the guard is satisfied and
  // the next tick has nothing to do -- the loop #227 found ends here.
  sync::ClientSaveState now;
  if (LocalSaveOnCard(checks, *files, sandbox, rom.id, arranged.name, arranged.slot, &now)) {
    const sync::TickResult next =
        sync::RunTick(client, *files, TokenFor(base, fixture), {now}, arranged.targets,
                      StoredBaseline(*files).value, OptionsAt(1'757'000'601));
    checks.Expect(next.outcome == sync::TickOutcome::kCompleted && next.resolved.refused == 0,
                  std::string("the next tick draws no 409: ") + sync::ToString(next.outcome));
    checks.Expect(next.executed.operations.size() == 1 &&
                      next.executed.operations[0].outcome == sync::OperationOutcome::kNoOp,
                  "and has nothing to do");
  }
  sweep();
}

/// Bytes changed here since the last sync, and another device's upload landing
/// between negotiate and upload: both sides moved, so it is a conflict -- the
/// head on the card, the local bytes under `.backup/`, and an entry in the
/// conflict history.
void NewerConflict(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                   const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-newer-conflict");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string mine = "the bytes this device last synced\n";
  const std::string edited = "this device played on since, and saved\n";
  const std::string theirs = "another device's save, during the tick\n";

  SyncedSlot arranged;
  if (!ArrangeSynced(checks, client, base, fixture, sandbox, rom, "m10-6-conflict", mine, edited,
                     &arranged)) {
    return;
  }
  std::vector<std::int64_t> planted = {arranged.mine.id};
  const auto sweep = [&]() {
    for (const std::int64_t id : planted) {
      harness::DeleteSave(client, base, fixture, id);
    }
  };

  const std::string staged = sandbox.Host("/config/rommsync/theirs.srm");
  rig::WriteFile(staged, theirs);
  harness::Save other;
  Interposed racing(client, [&]() {
    if (OtherDeviceUploads(checks, client, base, fixture, rom, arranged.slot, arranged.name,
                           staged, &other)) {
      planted.push_back(other.id);
    }
  });

  sync::TickResult tick;
  {
    crash::Recorder recorder(sandbox.root());
    tick = sync::RunTick(racing, *files, TokenFor(base, fixture), {arranged.local},
                         arranged.targets, arranged.previous, OptionsAt(1'757'000'700));
    crash::ExpectSurvives(checks, recorder,
                          {SavePath(arranged.name), edited, theirs, harness::kBackupDir},
                          rig::ScratchDir() + "/crash-newer-conflict",
                          "a 409 resolved as a conflict");
  }

  checks.Expect(racing.fired, "the other device's upload landed between negotiate and upload");
  checks.ExpectEq(tick.resolved.refused, std::size_t{1}, "the slot guard refused the upload");
  checks.ExpectEq(tick.resolved.resolved, std::size_t{1}, "and the tick resolved it");
  checks.Expect(tick.outcome == sync::TickOutcome::kCompleted,
                std::string("the tick completed: ") + sync::ToString(tick.outcome) +
                    (tick.executed.warnings.empty() ? std::string()
                                                    : " -- " + tick.executed.warnings[0]));
  if (tick.executed.operations.size() == 1 && tick.negotiated.plan.operations.size() == 1) {
    const sync::OperationResult& done = tick.executed.operations[0];
    checks.Expect(done.outcome == sync::OperationOutcome::kKeptBoth,
                  std::string("changed bytes against a newer head are a conflict: ") +
                      sync::ToString(done.outcome) + " -- " + done.message);
    checks.Expect(tick.negotiated.plan.operations[0].action == sync::Action::kConflict,
                  "and the plan the baseline and the history read says so");
  } else {
    checks.Expect(false, "the tick carries the one operation this run arranged");
  }
  checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), theirs,
                  "the server's head is on the card");
  checks.Expect(sandbox.HasBackupOf(edited), "and this device's progress is under .backup/");
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, arranged.slot, nullptr), 2,
                  "nothing was uploaded over the other device's save");

  // #240: a 409's conflict runs through the same keep-both, so this device's
  // progress is on the server too -- as an archival row, outside the slot.
  std::int64_t archive_id = 0;
  if (!tick.executed.operations.empty()) {
    const sync::OperationResult& done = tick.executed.operations[0];
    checks.Expect(done.archive == sync::ArchiveOutcome::kKept,
                  std::string("the losing side went up as an archival save: ") +
                      sync::ToString(done.archive));
    archive_id = done.archive_save_id.value_or(0);
  }
  bool archived = false;
  for (const harness::ArchivalRow& row : harness::ArchivalRows(client, base, fixture, rom.id)) {
    if (row.id == archive_id && archive_id != 0) {
      archived = row.content_hash == crypto::Md5Hex(edited);
      planted.push_back(row.id);
    }
  }
  checks.Expect(archived, "the server holds a null-slot row with this device's progress");

  // The conflict reaches the history the overlay reads, with the guard's own
  // sentence as the reason and the head's digest as the server side.
  conflicts::History history(sandbox.Host("/config/rommsync/conflicts.db"));
  const std::size_t recorded =
      conflicts::RecordSaves(&history, tick.negotiated.plan, tick.executed, {arranged.local});
  checks.ExpectEq(recorded, std::size_t{1}, "one conflict is recorded");
  if (history.size() == 1) {
    const conflicts::Entry& entry = history.entries()[0];
    checks.Expect(entry.event == conflicts::Event::kConflict && entry.restorable(),
                  "as a conflict whose local bytes can be restored");
    checks.ExpectEq(entry.reason, std::string(sync::kSlotNewerDetail),
                    "with the slot guard's own sentence as the reason");
    checks.Expect(entry.server_content_hash == std::optional<std::string>(crypto::Md5Hex(theirs)),
                  "and the head's digest as the server's side");
    checks.Expect(entry.local_copy == conflicts::LocalCopy::kOnServer &&
                      entry.archive_save_id == archive_id,
                  "and the archival row the local bytes are kept in (#240)");
  }
  sweep();
}

// --- baselines ----------------------------------------------------------------
//
// #234: every upload and every `/downloaded` carries `content_hash`, and RomM
// 5.4 keeps it as this device's baseline for that save row. A report matching
// the baseline proves "this side did not change", so an mtime-only touch --
// Tico rewriting a save atomically -- stops reading as a change.
//
// Each tick here runs through `harness::BaselineAudit`, which fails the run if
// either call leaves without the digest, or an upload's digest is not the MD5
// of the bytes it streams. The hazard the scenarios are written against is the
// other direction: a baseline that suppresses a conflict that is real. RomM can
// only remove a conflict with one, never create one (`compare_save_state`), so
// that would take a digest of bytes the server does not hold -- which the audit
// rules out -- or a baseline read against the wrong row, which is
// `baseline_other_row`.

/// A tick through the audit, with its findings reported.
sync::TickResult AuditedTick(rig::Checks& checks, http::HttpClient& client, fs::FileSystem& files,
                             const std::string& base, const Fixture& fixture,
                             const sync::ClientSaveState& local,
                             const std::vector<sync::SaveTarget>& targets,
                             state::Baseline previous, std::int64_t at, const std::string& what,
                             std::size_t uploads, std::size_t confirmations) {
  harness::BaselineAudit audit(client);
  sync::TickResult tick = sync::RunTick(audit, files, TokenFor(base, fixture), {local}, targets,
                                        std::move(previous), OptionsAt(at));
  for (const std::string& violation : audit.violations) {
    checks.Expect(false, what + ": " + violation);
  }
  checks.ExpectEq(audit.uploaded.size(), uploads, what + ": uploads that carried a content_hash");
  checks.ExpectEq(audit.confirmed.size(), confirmations,
                  what + ": confirmations that carried a content_hash");
  return tick;
}

/// The one operation this run arranged, as the plan named it and as it went.
bool OnlyOperation(rig::Checks& checks, const sync::TickResult& tick, const std::string& what,
                   sync::Action* action, sync::OperationResult* done) {
  if (tick.negotiated.plan.operations.size() != 1 || tick.executed.operations.size() != 1) {
    checks.Expect(false, what + ": the tick carries the one operation this run arranged" +
                             (tick.executed.warnings.empty() ? std::string()
                                                             : " -- " + tick.executed.warnings[0]));
    return false;
  }
  *action = tick.negotiated.plan.operations[0].action;
  *done = tick.executed.operations[0];
  return true;
}

/// Move a save's mtime two minutes past now and leave its bytes alone: what an
/// atomic rewrite of the same bytes looks like from the scan.
void TouchMtime(const Sandbox& sandbox, const std::string& sd_path) {
  std::error_code error;
  std::filesystem::last_write_time(
      sandbox.Host(sd_path),
      std::filesystem::file_time_type::clock::now() + std::chrono::minutes{2}, error);
}

/// The first tick of every baseline scenario: the card's only copy, uploaded through
/// the audit, so the slot has one row and this device's baseline is on it.
bool UploadedOnce(rig::Checks& checks, http::HttpClient& client, fs::FileSystem& files,
                  const std::string& base, const Fixture& fixture, Sandbox& sandbox,
                  const harness::Rom& rom, std::string_view label, UploadFixture* arranged,
                  std::int64_t* row) {
  if (!ArrangeUpload(checks, files, sandbox, rom, label, arranged)) {
    return false;
  }
  const sync::TickResult first =
      AuditedTick(checks, client, files, base, fixture, arranged->local, arranged->targets, {},
                  1'757'000'800, "the first tick", 1, 0);
  sync::Action action = sync::Action::kNoOp;
  sync::OperationResult done;
  if (!OnlyOperation(checks, first, "the first tick", &action, &done)) {
    return false;
  }
  checks.Expect(done.outcome == sync::OperationOutcome::kUploaded && done.save_id.has_value(),
                std::string("the first tick uploads the card's only copy: ") + done.message);
  if (!done.save_id.has_value()) {
    return false;
  }
  *row = *done.save_id;
  return true;
}

/// The acceptance as #234 words it: the same bytes under a newer mtime negotiate
/// as `no_op`. RomM's "content is identical" answers this one before any
/// baseline is read, so it is the floor rather than the proof -- `baseline_moved`
/// and `baseline_downloaded` are the cases only a baseline settles.
void BaselineTouch(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                   const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-baseline-touch");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  UploadFixture arranged;
  std::int64_t row = 0;
  if (!UploadedOnce(checks, client, *files, base, fixture, sandbox, rom, "m10-8-touch", &arranged,
                    &row)) {
    harness::DeleteSave(client, base, fixture, row);
    return;
  }

  TouchMtime(sandbox, SavePath(arranged.name));
  sync::ClientSaveState touched;
  if (LocalSaveOnCard(checks, *files, sandbox, rom.id, arranged.name, arranged.slot, &touched)) {
    checks.Expect(touched.updated_at > arranged.local.updated_at, "the touch moved the mtime");
    const sync::TickResult second =
        AuditedTick(checks, client, *files, base, fixture, touched, arranged.targets,
                    StoredBaseline(*files).value, 1'757'000'801, "the touched tick", 0, 0);
    sync::Action action = sync::Action::kUpload;
    sync::OperationResult done;
    if (OnlyOperation(checks, second, "the touched tick", &action, &done)) {
      checks.Expect(action == sync::Action::kNoOp,
                    std::string("an mtime-only touch negotiates as no_op: ") +
                        sync::ToString(action));
    }
  }
  checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), arranged.bytes, "the card is untouched");
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, arranged.slot, nullptr), 1,
                  "and nothing went up twice");
  harness::DeleteSave(client, base, fixture, row);
}

/// The server's copy moved in place since this device uploaded, and the card's
/// bytes only had their mtime touched. Without a baseline both sides "changed
/// since the last sync" and RomM says conflict; with the digest this device
/// uploaded, its side is proven unchanged and the answer is a download.
void BaselineMoved(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                   const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-baseline-moved");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  UploadFixture arranged;
  std::int64_t row = 0;
  if (!UploadedOnce(checks, client, *files, base, fixture, sandbox, rom, "m10-8-moved", &arranged,
                    &row)) {
    harness::DeleteSave(client, base, fixture, row);
    return;
  }

  // In place, by no device: the row this device's baseline is on moves forward.
  const std::string theirs = "the server's copy, moved on in place\n";
  const std::string staged = sandbox.Host("/config/rommsync/moved.srm");
  rig::WriteFile(staged, theirs);
  harness::PassASecond();
  harness::Save moved;
  if (!harness::ReplaceSave(client, base, fixture, row, staged, arranged.name, &moved)) {
    checks.Expect(false, "the server's copy was replaced in place");
    harness::DeleteSave(client, base, fixture, row);
    return;
  }

  TouchMtime(sandbox, SavePath(arranged.name));
  sync::ClientSaveState touched;
  if (LocalSaveOnCard(checks, *files, sandbox, rom.id, arranged.name, arranged.slot, &touched)) {
    const sync::TickResult second =
        AuditedTick(checks, client, *files, base, fixture, touched, arranged.targets,
                    StoredBaseline(*files).value, 1'757'000'802, "the touched tick", 0, 1);
    sync::Action action = sync::Action::kConflict;
    sync::OperationResult done;
    if (OnlyOperation(checks, second, "the touched tick", &action, &done)) {
      checks.Expect(action == sync::Action::kDownload,
                    std::string("unchanged bytes under a newer mtime take the server's move as a "
                                "download, not a conflict: ") +
                        sync::ToString(action));
      checks.Expect(done.outcome == sync::OperationOutcome::kDownloaded, done.message);
    }
  }
  checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), theirs, "the card holds the server's copy");
  checks.Expect(sandbox.HasBackupOf(arranged.bytes), "and the bytes it replaced are under .backup/");
  harness::DeleteSave(client, base, fixture, row);
}

/// The `/downloaded` half: a copy this device took from the server, moved in
/// place afterwards, with the card's copy only touched. The confirmation's
/// digest is the baseline that proves the card did not change.
void BaselineDownloaded(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                        const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-baseline-downloaded");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  DownloadFixture arranged;
  if (!ArrangeDownload(checks, client, base, fixture, sandbox, rom, "m10-8-downloaded",
                       &arranged)) {
    return;
  }
  const std::int64_t row = arranged.server.id;
  const sync::TickResult first =
      AuditedTick(checks, client, *files, base, fixture, arranged.local, arranged.targets, {},
                  1'757'000'803, "the first tick", 0, 1);
  sync::Action action = sync::Action::kNoOp;
  sync::OperationResult done;
  if (!OnlyOperation(checks, first, "the first tick", &action, &done) ||
      done.outcome != sync::OperationOutcome::kDownloaded) {
    checks.Expect(false, "the first tick downloads the server's copy: " + done.message);
    harness::DeleteSave(client, base, fixture, row);
    return;
  }
  checks.Expect(sandbox.HasBackupOf(arranged.previous),
                "the first download backed up the bytes it replaced");
  // The card now holds the server's bytes, and the audit judges what comes next
  // from them.
  sandbox.Reseed(SavePath(arranged.name), arranged.server_bytes);

  const std::string theirs = "the server's copy, moved on after the download\n";
  const std::string staged = sandbox.Host("/config/rommsync/moved.srm");
  rig::WriteFile(staged, theirs);
  harness::PassASecond();
  harness::Save moved;
  if (!harness::ReplaceSave(client, base, fixture, row, staged, arranged.name, &moved)) {
    checks.Expect(false, "the server's copy was replaced in place");
    harness::DeleteSave(client, base, fixture, row);
    return;
  }

  TouchMtime(sandbox, SavePath(arranged.name));
  sync::ClientSaveState touched;
  if (LocalSaveOnCard(checks, *files, sandbox, rom.id, arranged.name, arranged.slot, &touched)) {
    const sync::TickResult second =
        AuditedTick(checks, client, *files, base, fixture, touched, arranged.targets,
                    StoredBaseline(*files).value, 1'757'000'804, "the touched tick", 0, 1);
    if (OnlyOperation(checks, second, "the touched tick", &action, &done)) {
      checks.Expect(action == sync::Action::kDownload,
                    std::string("a downloaded copy only touched since takes the move as a "
                                "download, not a conflict: ") +
                        sync::ToString(action));
    }
  }
  checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), theirs, "the card holds the server's copy");
  checks.Expect(sandbox.HasBackupOf(arranged.server_bytes),
                "and the bytes it replaced are under .backup/");
  harness::DeleteSave(client, base, fixture, row);
}

/// What a baseline must never do: hide progress. The card's bytes really
/// changed while the server's copy moved in place, so both sides changed and it
/// is a conflict -- the baseline proves nothing about a side whose digest no
/// longer matches it.
void BaselineConflict(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                      const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-baseline-conflict");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  UploadFixture arranged;
  std::int64_t row = 0;
  if (!UploadedOnce(checks, client, *files, base, fixture, sandbox, rom, "m10-8-conflict",
                    &arranged, &row)) {
    harness::DeleteSave(client, base, fixture, row);
    return;
  }

  const std::string theirs = "the server's copy, moved on in place\n";
  const std::string staged = sandbox.Host("/config/rommsync/moved.srm");
  rig::WriteFile(staged, theirs);
  harness::PassASecond();
  harness::Save moved;
  if (!harness::ReplaceSave(client, base, fixture, row, staged, arranged.name, &moved)) {
    checks.Expect(false, "the server's copy was replaced in place");
    harness::DeleteSave(client, base, fixture, row);
    return;
  }

  const std::string progress = "this console played on, and saved\n";
  sandbox.Reseed(SavePath(arranged.name), progress);
  TouchMtime(sandbox, SavePath(arranged.name));
  sync::ClientSaveState played;
  if (LocalSaveOnCard(checks, *files, sandbox, rom.id, arranged.name, arranged.slot, &played)) {
    // The one upload is the conflict's archival copy (#240), held to the same
    // audit: its digest is the MD5 of the bytes it streams.
    const sync::TickResult second =
        AuditedTick(checks, client, *files, base, fixture, played, arranged.targets,
                    StoredBaseline(*files).value, 1'757'000'805, "the played tick", 1, 1);
    sync::Action action = sync::Action::kNoOp;
    sync::OperationResult done;
    if (OnlyOperation(checks, second, "the played tick", &action, &done)) {
      checks.Expect(action == sync::Action::kConflict,
                    std::string("real progress against a moved server copy is a conflict: ") +
                        sync::ToString(action));
      checks.Expect(done.outcome == sync::OperationOutcome::kKeptBoth, done.message);
    }
  }
  checks.Expect(sandbox.HasBackupOf(progress), "and the progress is under .backup/");
  harness::DeleteSave(client, base, fixture, row);
}

/// #227's finding, held to the same rule: a baseline lives on one row, and
/// another device's upload is a new row -- the head -- that this device has no
/// history with. The baseline on the superseded row must not be what answers
/// for the card. Real progress here meets the slot guard (#232) and ends a
/// conflict, with nothing uploaded over the other device's save.
void BaselineOtherRow(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                      const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-baseline-other-row");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  UploadFixture arranged;
  std::int64_t row = 0;
  if (!UploadedOnce(checks, client, *files, base, fixture, sandbox, rom, "m10-8-other-row",
                    &arranged, &row)) {
    harness::DeleteSave(client, base, fixture, row);
    return;
  }

  const std::string theirs = "another device's save, a new row\n";
  const std::string staged = sandbox.Host("/config/rommsync/theirs.srm");
  rig::WriteFile(staged, theirs);
  harness::Save other;
  if (!OtherDeviceUploads(checks, client, base, fixture, rom, arranged.slot, arranged.name, staged,
                          &other)) {
    harness::DeleteSave(client, base, fixture, row);
    return;
  }

  const std::string progress = "this console played on, and saved\n";
  sandbox.Reseed(SavePath(arranged.name), progress);
  TouchMtime(sandbox, SavePath(arranged.name));
  sync::ClientSaveState played;
  if (LocalSaveOnCard(checks, *files, sandbox, rom.id, arranged.name, arranged.slot, &played)) {
    // One upload, refused by the guard; then the head, taken keep-both, with the
    // local bytes uploaded as the conflict's archival copy (#240).
    const sync::TickResult second =
        AuditedTick(checks, client, *files, base, fixture, played, arranged.targets,
                    StoredBaseline(*files).value, 1'757'000'806, "the played tick", 2, 1);
    checks.ExpectEq(second.resolved.refused, std::size_t{1}, "the slot guard refused the upload");
    sync::Action action = sync::Action::kNoOp;
    sync::OperationResult done;
    if (OnlyOperation(checks, second, "the played tick", &action, &done)) {
      checks.Expect(done.outcome == sync::OperationOutcome::kKeptBoth,
                    std::string("real progress against another device's row is a conflict: ") +
                        sync::ToString(done.outcome) + " -- " + done.message);
    }
  }
  checks.ExpectEq(sandbox.Read(SavePath(arranged.name)), theirs, "the head is on the card");
  checks.Expect(sandbox.HasBackupOf(progress), "and the progress is under .backup/");
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, arranged.slot, nullptr), 2,
                  "and nothing was uploaded over the other device's save");
  harness::DeleteSave(client, base, fixture, row);
  harness::DeleteSave(client, base, fixture, other.id);
}

// --- migrated (#236) --------------------------------------------------------------
//
// The first tick after an upgrade from rc1: `state.db` is version 2, its save row
// in rc1's `tico-srm`, and the shared slot already holds another client's newer
// progress. The tick negotiates the save in the shared slot -- never re-uploads
// it into the old one -- and lands the head over it with the local bytes backed
// up first, under `crash::Recorder` like every other overwrite (#203).
//
// The configured slot is a unique one standing in for `autosave`, so no other
// scenario's saves are in it; the old slot is the real `tico-srm`, as rc1 wrote.
void Migrated(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "tick-migrated");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m10-10");
  const std::string name = "gb240p.srm";
  const std::string mine = "what rc1 synced into tico-srm, " + slot + "\n";
  const std::string theirs = "RetroArch's progress in the shared slot, " + slot + "\n";

  // rc1's sync: the bytes on the card, the same bytes in `tico-srm` with this
  // device's sync row, and the row rc1 wrote for them.
  sandbox.SeedSave(SavePath(name), mine);
  harness::Save old_slot;
  if (!harness::UploadSave(client, base, fixture, rom.id, "tico-srm", "tico",
                           sandbox.Host(SavePath(name)), name, /*with_device=*/true, &old_slot)) {
    checks.Expect(false, "rc1's upload to its own slot was stored");
    return;
  }
  sync::ClientSaveState local;
  if (!LocalSaveOnCard(checks, *files, sandbox, rom.id, name, slot, &local)) {
    harness::DeleteSave(client, base, fixture, old_slot.id);
    return;
  }
  local.emulator = "gambatte";
  const std::string v2 =
      "rommsync-state 2\n{\"rom_id\":" + std::to_string(rom.id) +
      ",\"slot\":\"tico-srm\",\"content_hash\":\"" + crypto::Md5Hex(mine) +
      "\",\"mtime\":" + std::to_string(sync::UnixSeconds(local.updated_at)) +
      ",\"file_size_bytes\":" + std::to_string(mine.size()) +
      ",\"server_updated_at\":null,\"server_content_hash\":\"" + crypto::Md5Hex(mine) + "\"}\n";
  state::LoadedBaseline loaded = state::ParseBaseline(v2);
  const std::size_t adopted =
      state::AdoptLegacySaves(&loaded, [&slot](std::string_view old) -> std::optional<std::string> {
        return rommsync::scan::MigratedSlot(slot, old);
      });
  checks.ExpectEq(adopted, std::size_t{1}, "rc1's row moves to the configured slot");
  checks.Expect(state::ContentHashFor(loaded.value, rom.id, slot,
                                      sandbox.Host(SavePath(name)), local.updated_at,
                                      local.file_size_bytes)
                    .reused,
                "and the save it describes is not hashed again");

  // Another client's newer progress, already in the shared slot.
  harness::PassASecond();
  const std::string staged = sandbox.Host("/config/rommsync/theirs.srm");
  rig::WriteFile(staged, theirs);
  harness::Save head;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "gambatte", staged, name,
                           /*with_device=*/false, &head)) {
    checks.Expect(false, "the other client's save was stored");
    harness::DeleteSave(client, base, fixture, old_slot.id);
    return;
  }
  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};

  sync::TickResult tick;
  {
    crash::Recorder recorder(sandbox.root());
    tick = sync::RunTick(client, *files, TokenFor(base, fixture), {local}, targets,
                         std::move(loaded.value), OptionsAt(1'757'000'800));
    crash::ExpectSurvives(checks, recorder, {SavePath(name), mine, theirs, harness::kBackupDir},
                          rig::ScratchDir() + "/crash-migrated", "a migrated save's first tick");
  }
  checks.Expect(tick.outcome == sync::TickOutcome::kCompleted,
                std::string("the tick completed: ") + sync::ToString(tick.outcome));
  checks.Expect(tick.executed.operations.size() == 1 &&
                    tick.executed.operations[0].outcome == sync::OperationOutcome::kDownloaded,
                "the shared slot's newer head is downloaded, not answered with an upload");
  checks.ExpectEq(sandbox.Read(SavePath(name)), theirs, "the card holds it");
  checks.Expect(sandbox.HasBackupOf(mine), "and rc1's bytes are under .backup/");
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, slot, nullptr), 1,
                  "nothing was uploaded into the shared slot");
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, "tico-srm", nullptr) >= 1, true,
                  "and the old slot's row is still on the server, untouched");

  const state::LoadedBaseline stored = StoredBaseline(*files);
  checks.ExpectEq(stored.version, state::kFormatVersion, "state.db is written in version 3");
  checks.Expect(stored.value.Find(rom.id, std::string("tico-srm")) == nullptr &&
                    stored.value.Find(rom.id, slot) != nullptr,
                "with the save under the slot it is synced in, and nothing under the old one");

  sync::ClientSaveState now;
  if (LocalSaveOnCard(checks, *files, sandbox, rom.id, name, slot, &now)) {
    now.emulator = "gambatte";
    const sync::TickResult next = sync::RunTick(client, *files, TokenFor(base, fixture), {now},
                                                targets, stored.value, OptionsAt(1'757'000'801));
    checks.Expect(next.executed.operations.size() == 1 &&
                      next.executed.operations[0].outcome == sync::OperationOutcome::kNoOp,
                  "the next tick has nothing to do: no duplicate upload");
  }
  checks.ExpectEq(SaveRowsFor(client, base, fixture, rom.id, slot, nullptr), 1,
                  "still one row in the shared slot");
  harness::DeleteSave(client, base, fixture, head.id);
  harness::DeleteSave(client, base, fixture, old_slot.id);
}

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "recovery";

  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);

  // Installed here, before anything writes, the way `sysmodule/source/main.cpp`
  // installs the Horizon one: the whole point of `io::FileSync` is that the
  // platform layer decides, and a test suite that never installed one would be
  // proving the weaker promise (atomic_file.hpp).
  rommsync::host::InstallPosixFileSync();

  // The tally lives above every scenario, and each scenario returns void: a
  // `Sandbox` reports its teardown audit into this object, and a scenario that
  // returned its own count would copy the number out before the audit ran.
  rig::Checks checks;

  // The eight that need no server, so they stay checked with docker stopped.
  if (scenario == "recovery" || scenario == "durable" || scenario == "backupdir" ||
      scenario == "offline" || scenario == "rescan" || scenario == "canceled" ||
      scenario == "disabled" || scenario == "unsupported" || scenario == "newer_rule" ||
      scenario == "newer_bounds" || scenario == "migrated_kept" || scenario == "too_large") {
    if (scenario == "recovery") {
      Recovery(checks);
    } else if (scenario == "durable") {
      Durable(checks);
    } else if (scenario == "backupdir") {
      BackupDir(checks);
    } else if (scenario == "offline") {
      Offline(checks);
    } else if (scenario == "rescan") {
      Rescan(checks);
    } else if (scenario == "disabled") {
      Disabled(checks);
    } else if (scenario == "unsupported") {
      Unsupported(checks);
    } else if (scenario == "newer_rule") {
      NewerRule(checks);
    } else if (scenario == "migrated_kept") {
      MigratedKept(checks);
    } else if (scenario == "newer_bounds") {
      NewerBounds(checks);
    } else if (scenario == "too_large") {
      TooLarge(checks);
    } else {
      Canceled(checks);
    }
    if (checks.failures() == 0) {
      std::cout << "tick." << scenario << " ok\n";
    }
    return checks.failures() == 0 ? 0 : 1;
  }

  const std::string base = rig::BaseUrl();
  const std::unique_ptr<http::HttpClient> client = rig::MakeClient();
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
  // A session an earlier scenario left open is one this scenario's negotiate has
  // to cancel, and that cancel races with the session it just created (#76).
  harness::CloseOpenSessions(*client, base, fixture);

  harness::Rom rom;
  if (!harness::FindRom(*client, base, fixture, "gb240p.gb", &rom)) {
    std::cerr << "the fixture library holds no roms\n"
                 "  scan it with: ./.venv/bin/python server/testing/provision.py\n";
    return rig::kSkip;
  }

  if (scenario == "negotiate_5xx") {
    // `count` past the retry budget, so every attempt meets it: a scenario that
    // damaged only the first would be proving the retry, not the failure.
    NegotiateFault(checks, *client, base, fixture, rom,
                   R"({"mode":"status","status":500,"path":"/api/sync/negotiate","count":9})",
                   sync::NegotiateError::kServerError, sync::TickOutcome::kOffline,
                   "a 500 at negotiate");
  } else if (scenario == "negotiate_drop") {
    NegotiateFault(checks, *client, base, fixture, rom,
                   R"({"mode":"drop","bytes":16,"path":"/api/sync/negotiate","count":9})",
                   sync::NegotiateError::kUnreachable, sync::TickOutcome::kOffline,
                   "a reset at negotiate");
  } else if (scenario == "server_disabled") {
    ServerDisabled(checks, *client, base, fixture, rom);
  } else if (scenario == "upload_5xx") {
    // The upload never reaches RomM: the proxy answers the status itself, so
    // there is no row for the tick to have to account for.
    UploadFault(checks, *client, base, fixture, rom,
                R"({"mode":"status","status":500,"path":"/api/saves","count":4})",
                sync::OperationOutcome::kFailed, sync::OperationError::kRefused, 0,
                /*then_normally=*/false, "a 500 at an upload");
  } else if (scenario == "upload_truncate") {
    // A clean short 200. The save *is* on the server -- the proxy cuts the
    // response, not the request -- so the operation is deliberately a success
    // whose `save_id` is simply unknown: failing it would have the next tick
    // upload the same bytes again, which RomM stores as a second row.
    UploadFault(checks, *client, base, fixture, rom,
                R"({"mode":"truncate","bytes":8,"path":"/api/saves"})",
                sync::OperationOutcome::kUploaded, sync::OperationError::kNone, 1,
                /*then_normally=*/false, "a truncated upload response");
  } else if (scenario == "upload_drop") {
    // The other half of the same story, and the reason there is no retry inside
    // a tick: the row exists and the client cannot know it. It counts the
    // operation failed, sends nothing more, and lets the next negotiation
    // arbitrate -- which is the only thing that keeps the row count at one.
    UploadFault(checks, *client, base, fixture, rom,
                R"({"mode":"drop","bytes":4,"path":"/api/saves"})",
                sync::OperationOutcome::kFailed, sync::OperationError::kTransferFailed, 1,
                /*then_normally=*/false,
                "a reset while the upload response was arriving");
  } else if (scenario == "upload_stall") {
    // The proxy holds a stalled request and then drops it without forwarding,
    // so this upload never reaches RomM at all -- a stalled request costs the
    // tick and nothing else. `rows_expected=1` IS that claim: the one row is
    // the one the next tick writes, and a second row would mean the abandoned
    // upload landed after the client had given up on it.
    //
    // Until issue #109 this comment described the intent rather than the
    // behaviour. The proxy did forward, 1.5s late -- straight into the window
    // this row count is measured in.
    UploadFault(checks, *client, base, fixture, rom,
                R"({"mode":"stall","seconds":3,"path":"/api/saves"})",
                sync::OperationOutcome::kFailed, sync::OperationError::kTransferFailed,
                /*rows_expected=*/1, /*then_normally=*/true, "a stall at an upload");
  } else if (scenario == "download_5xx") {
    DownloadFault(checks, *client, base, fixture, rom, "status", sync::OperationError::kRefused,
                  /*then_normally=*/false, "a 500 at a download");
  } else if (scenario == "download_stall") {
    DownloadFault(checks, *client, base, fixture, rom, "stall",
                  sync::OperationError::kTransferFailed, /*then_normally=*/true,
                  "a stall at a download");
  } else if (scenario == "download_401") {
    DownloadRevoked(checks, *client, base, fixture, rom);
  } else if (scenario == "download_full") {
    DiskFault(checks, *client, base, fixture, rom, ".part", ENOSPC,
              sync::OperationError::kTransferFailed, "full", "a full card under the transfer");
  } else if (scenario == "download_eio") {
    DiskFault(checks, *client, base, fixture, rom, ".part", EIO,
              sync::OperationError::kTransferFailed, "refused", "an I/O error under the transfer");
  } else if (scenario == "backup_full") {
    DiskFault(checks, *client, base, fixture, rom, "/.backup/", ENOSPC,
              sync::OperationError::kBackupFailed, "full", "a full card under the backup");
  } else if (scenario == "complete_drop") {
    CompleteFault(checks, *client, base, fixture, rom,
                  R"({"mode":"drop","bytes":4,"path":"/api/sync/sessions","count":9})",
                  /*then_normally=*/false, "a reset at complete");
  } else if (scenario == "complete_stall") {
    // The prefix is the session collection rather than `/{id}/complete`, because
    // the id does not exist until the negotiation has answered. It costs
    // nothing: inside the armed scope the tick sends two requests under that
    // prefix and both are the completion's attempts (`OptionsAt` sets
    // `finish.complete.max_attempts = 2`), so nine is four and a half times the
    // budget and nothing earlier in the scenario can spend the arm.
    //
    // Written down because #156 read that count as the explanation for a
    // `complete` that once succeeded straight through this stall, and it is not
    // the one -- the stranger was at the proxy rather than in the tick. The
    // account is docs/TESTING.md, "One proxy, several clients"; the regression
    // pin is harness.fault_owner.
    CompleteFault(checks, *client, base, fixture, rom,
                  R"({"mode":"stall","seconds":3,"path":"/api/sync/sessions","count":9})",
                  /*then_normally=*/true, "a stall at complete");
  } else if (scenario == "complete_closed") {
    CompleteClosed(checks, *client, base, fixture, rom);
  } else if (scenario == "sched_stall") {
    // A negotiate that stops moving: the tick fails on the stall timeout rather
    // than hanging, and the schedule backs off rather than retrying at once.
    Scheduled(checks, *client, base, fixture, rom,
              R"({"mode":"stall","seconds":4,"path":"/api/sync/negotiate","count":9})",
              "a stalled negotiation");
  } else if (scenario == "sched_drop") {
    // A real TCP reset mid-plan. Transient: back off, write nothing, and let the
    // next tick recover.
    Scheduled(checks, *client, base, fixture, rom,
              R"({"mode":"drop","bytes":16,"path":"/api/sync/negotiate","count":9})",
              "a reset mid-negotiation");
  } else if (scenario == "newer_download") {
    NewerDownload(checks, *client, base, fixture, rom);
  } else if (scenario == "newer_conflict") {
    NewerConflict(checks, *client, base, fixture, rom);
  } else if (scenario == "baseline_touch") {
    BaselineTouch(checks, *client, base, fixture, rom);
  } else if (scenario == "baseline_moved") {
    BaselineMoved(checks, *client, base, fixture, rom);
  } else if (scenario == "baseline_downloaded") {
    BaselineDownloaded(checks, *client, base, fixture, rom);
  } else if (scenario == "baseline_conflict") {
    BaselineConflict(checks, *client, base, fixture, rom);
  } else if (scenario == "baseline_other_row") {
    BaselineOtherRow(checks, *client, base, fixture, rom);
  } else if (scenario == "migrated") {
    Migrated(checks, *client, base, fixture, rom);
  } else if (scenario == "resumes") {
    Resumes(checks, *client, base, fixture, rom);
  } else if (scenario == "relaunched") {
    Relaunched(checks, *client, base, fixture, rom);
  } else if (scenario == "deleted") {
    Deleted(checks, *client, base, fixture, rom, rommsync::config::OnServerDelete::kRemove);
  } else if (scenario == "deleted_keep") {
    Deleted(checks, *client, base, fixture, rom, rommsync::config::OnServerDelete::kKeep);
  } else if (scenario == "isolated") {
    Isolated(checks, *client, base, fixture);
  } else if (scenario == "isolated_ids") {
    IsolatedIds(checks, *client, base, fixture);
  } else if (scenario == "oversized") {
    Oversized(checks, *client, base, fixture);
  } else if (scenario == "scoped") {
    Scoped(checks, *client, base, fixture, rom);
  } else if (scenario == "restore") {
    Restore(checks, *client, base, fixture, rom);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  // Asserted **before** the cleanup disarm, not after it. Every scenario arms
  // through `harness::Fault`, which disarms on the way out including out of an
  // early return; this is the belt to that brace, and a `DisarmFault` in front
  // of it would make it a check that can never fail. The unconditional disarm
  // still happens afterwards, because a scenario that left one armed must not
  // damage the next test's first request as well as failing its own.
  harness::ExpectDisarmed(checks, *client, base,
                          "tick." + scenario + " left the fault proxy disarmed");
  rig::DisarmFault(*client, base);
  harness::CloseOpenSessions(*client, base, fixture);
  harness::DeleteArchivalCopies(*client, base, fixture, rom.id);  // #240

  if (checks.failures() == 0) {
    std::cout << "tick." << scenario << " ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
