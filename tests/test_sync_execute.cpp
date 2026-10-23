// Step 2 of the sync loop, against the real RomM: the plan, executed.
//
// `sync.*` proves the client can *get* a plan; these prove it can act on one
// without losing a save. Every scenario runs `sync::ExecutePlan` -- the engine's
// own code, over the native `HttpClient`, against the docker 5.2.0 -- inside a
// `harness::Sandbox`, so docs/SYNC_PROTOCOL.md's hard rule is audited on
// teardown whether or not the scenario thought to look.
//
// Several need no server and must stay checked with docker stopped: `naming` is
// the backup path that used to collide, `staging` is the two `io` primitives an
// overwrite goes through, `counting` is what the report says about the
// operations that never reach the network, `crash_model` is the power-cut
// replay, and `delete` / `crash_delete` are 5.4.0's removal, which sends no
// request at all.
//
// **Why every scenario filters the plan to its own slot.** Negotiate answers
// with every save this device has no history for, which on a shared fixture
// includes whatever other scenarios left behind. Executing those would act on
// saves this test knows nothing about; `PlanFor` keeps a run to the slots it
// created, the way `harness::OperationFor` does for the raw JSON.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "crash_disk.hpp"
#include "faulty_disk.hpp"
#include "harness.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/disk.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/sync_execute.hpp"
#include "rommsync/sync_finish.hpp"

namespace {

namespace auth = rommsync::auth;
namespace crypto = rommsync::crypto;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace io = rommsync::io;
namespace place = rommsync::place;
namespace sync = rommsync::sync;

using harness::Fixture;
using harness::Sandbox;

using harness::PassASecond;
using harness::SavePath;

/// The token the executor authenticates with. The fixture's, pointed at the
/// proxy, so a scenario can damage one call and nothing else.
auth::StoredToken TokenFor(const std::string& base, const Fixture& fixture) {
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  return token;
}

/// Negotiate, read the plan with the engine's own parser, and keep only the
/// operations for `slots`.
///
/// Returns false with the reason already reported when there is no plan, or no
/// operation for the slots this run created.
bool PlanFor(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const sync::SyncNegotiatePayload& payload,
             const std::vector<std::string>& slots, sync::SyncPlan* out) {
  const http::Result result = harness::Negotiate(checks, client, base, fixture, payload);
  // One active session per device, and every scenario here shares the
  // fixture's. Left open, this one is cancelled by the NEXT negotiate -- and
  // that cancel races with the session that negotiate creates (issue #76).
  harness::CloseSession(client, base, fixture, result.response.body);
  if (result.response.status != 200) {
    checks.Expect(false, "the negotiation is answered: HTTP " +
                             std::to_string(result.response.status) + " " + result.response.body);
    return false;
  }
  const auth::Parsed<sync::SyncPlan> parsed =
      sync::ParseNegotiateResponse(result.response.body);
  if (!parsed.ok()) {
    checks.Expect(false, "the engine reads the plan: " + parsed.error.Describe());
    return false;
  }

  out->session_id = parsed.value.session_id;
  for (const sync::SyncOperation& operation : parsed.value.operations) {
    for (const std::string& slot : slots) {
      if (operation.slot.has_value() && *operation.slot == slot) {
        out->operations.push_back(operation);
      }
    }
  }
  if (out->operations.size() != slots.size()) {
    checks.Expect(false, "the plan carries an operation for each of this run's slots: " +
                             result.response.body);
    return false;
  }
  return true;
}

/// Expect RomM to arbitrate `slot` against a sync row this device holds for
/// `save_id`; `what` names the call that should have written it.
///
/// That row is the whole point of passing `device_id` on an upload and of
/// `POST /api/saves/{id}/downloaded` after a download: without it every later
/// negotiation for the save falls into the no-sync-history branch
/// (docs/API_CONTRACT.md).
///
/// **Reading the row back proves nothing on 5.4.0, so this does not.**
/// `GET /api/saves/{id}?device_id=` answers an entry for the calling device
/// whether or not one exists -- RomM synthesizes it, at the save's own
/// `updated_at` and `is_current: false`, which is also what a real one looks
/// like (#265). The witness is the thing the row exists for: the next
/// negotiation. Offered a copy changed after the save was written -- a later
/// mtime *and* different bytes -- a device with a row is answered `Client save
/// is newer than last sync` and one without `Client save is newer (no sync
/// history)`. The bytes have to differ because `Content is identical` is
/// answered before history is read at all. Nothing is executed, and `PlanFor`
/// closes the session, so the scenario's own plan is untouched.
void ExpectDeviceSynced(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                        const Fixture& fixture, std::int64_t rom_id, const std::string& slot,
                        std::int64_t save_id, const std::string& what) {
  const std::string changed = "a copy this device changed after save " +
                              std::to_string(save_id) + " was written\n";
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(rom_id, "witness.srm", slot, "m2-5",
                                             crypto::Md5Hex(changed),
                                             std::chrono::system_clock::now() +
                                                 std::chrono::hours{1},
                                             static_cast<std::int64_t>(changed.size())));
  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    return;
  }
  const sync::SyncOperation& operation = plan.operations[0];
  // Arbitrated against `save_id` and no other row: a row this device holds
  // for some other save in the slot is not the one the scenario wrote.
  checks.Expect(operation.action == sync::Action::kUpload &&
                    operation.reason == sync::Reason::kClientNewer &&
                    operation.save_id == save_id,
                what + ": negotiate answered " + sync::ToString(operation.action) + ", \"" +
                    operation.reason_text + "\", save " +
                    (operation.save_id.has_value() ? std::to_string(*operation.save_id)
                                                   : std::string("null")));
}

/// The options every rig scenario runs with: the sandbox's own `.backup/`, and
/// a clock that does not move.
///
/// The frozen clock is not a convenience. The acceptance criterion this file
/// exists for is that two saves of one rom backed up **in the same second** are
/// two files, and a test that let the clock run would pass or fail on how fast
/// the machine is.
sync::ExecuteOptions OptionsAt(std::int64_t unix_seconds) {
  sync::ExecuteOptions options;
  options.backup_dir = harness::kBackupDir;
  options.now = [unix_seconds]() {
    return sync::Timestamp{} + std::chrono::seconds{unix_seconds};
  };
  return options;
}

// --- naming -------------------------------------------------------------------
//
// The collision docs/SYNC_PROTOCOL.md named and this issue fixed:
// `<rom_id>-<ts>.<ext>` carries neither the slot nor the save's own name, so two
// saves of one rom backed up in the same second were one file and the second
// backup destroyed the first. Needs no server, so it stays checked with docker
// stopped -- which is when a change to the scheme is least likely to be noticed.

void Naming(rig::Checks& checks) {
  constexpr std::int64_t kWhen = 1'757'000'000;

  checks.ExpectEq(sync::BackupFileName(4, "retroarch-srm", "Game (USA).srm", kWhen),
                  std::string("4-retroarch-srm-1757000000.srm"),
                  "the backup name carries the rom, the slot, the second and the extension");

  // The acceptance criterion, as arithmetic: one rom, one second, two slots.
  checks.Expect(sync::BackupFileName(4, "retroarch-srm", "Game.srm", kWhen) !=
                    sync::BackupFileName(4, "retroarch-state", "Game.state", kWhen),
                "two slots of one rom in the same second are two different files");

  // A save and its state under the *same* slot would still collide on the name
  // alone, which is what the uniquifier is for.
  checks.ExpectEq(sync::BackupFileName(4, "retroarch-srm", "Game.srm", kWhen, 1),
                  std::string("4-retroarch-srm-1757000000-1.srm"),
                  "an occupied name is stepped past rather than over");

  // `null` is archival, which pairs with nothing -- and it needs a name that is
  // a name. It cannot collide with a derived slot: `scan::SlotFor` always
  // carries an emulator and an extension.
  checks.ExpectEq(sync::BackupFileName(9, std::nullopt, "Game.srm", kWhen),
                  std::string("9-archival-1757000000.srm"),
                  "an archival save's backup is named rather than left blank");

  // The name a conflict's losing copy goes up under (#240). RomM tags no
  // `null`-slot upload and replaces a row whose name it already holds, so the
  // conflict's own second is what keeps the next conflict's copy from landing
  // on this one.
  checks.ExpectEq(sync::ArchiveFileName("Game (USA).srm", kWhen),
                  std::string("Game (USA) [switch-conflict 2025-09-04_15-33-20].srm"),
                  "the archival name is the client's stem, the marker and the second");
  checks.Expect(sync::ArchiveFileName("Game.srm", kWhen) !=
                    sync::ArchiveFileName("Game.srm", kWhen + 1),
                "two conflicts of one save a second apart are two archival rows");
  checks.ExpectEq(sync::ArchiveFileName("Game", kWhen),
                  std::string("Game [switch-conflict 2025-09-04_15-33-20]"),
                  "a save with no extension keeps having none");

  // The slot on an operation is whatever some other client chose. A separator
  // in it would put the backup somewhere other than `.backup/`, and `..` would
  // put it above the directory entirely.
  const std::string escaped = sync::BackupFileName(3, "../../etc/passwd", "Game.srm", kWhen);
  checks.Expect(escaped.find('/') == std::string::npos,
                "a slot cannot carry a separator into the backup path: " + escaped);
  // ...and a name that is *not* `archival`, because a save whose slot is `..`
  // and a save with no slot at all are two different saves and must not share a
  // backup.
  checks.ExpectEq(sync::BackupFileName(3, "..", "Game.srm", kWhen),
                  std::string("3-__-1757000000.srm"),
                  "a slot that reduces to a directory name gets a name of its own");
  checks.Expect(sync::BackupFileName(3, "..", "Game.srm", kWhen) !=
                    sync::BackupFileName(3, std::nullopt, "Game.srm", kWhen),
                "which is not the archival one");

  // `scan::BaseName` draws the same line and the two must not disagree: a
  // leading dot is a whole name, not an extension.
  checks.ExpectEq(sync::BackupFileName(5, "slot", "savefile", kWhen),
                  std::string("5-slot-1757000000"), "a save with no extension keeps none");
  checks.ExpectEq(sync::BackupFileName(5, "slot", ".DS_Store", kWhen),
                  std::string("5-slot-1757000000"), "a leading dot is not an extension");

  // Matching is on `(rom_id, slot)` and never on the name, because the name on
  // an operation is the server's -- RomM's ingest tag and all.
  std::vector<sync::SaveTarget> targets = {
      {4, std::string("retroarch-srm"), "/retroarch/saves/Game.srm", "Game.srm"},
      {4, std::nullopt, "/retroarch/saves/Archive.srm", "Archive.srm"},
  };
  sync::SyncOperation operation;
  operation.rom_id = 4;
  operation.slot = "retroarch-srm";
  operation.file_name = "Game [2026-09-04_11-12-27].srm";
  const sync::SaveTarget* matched = sync::MatchTarget(targets, operation);
  checks.Expect(matched != nullptr && matched->sd_path == "/retroarch/saves/Game.srm",
                "the operation matches the local file on (rom_id, slot), not on the name");

  operation.slot = std::nullopt;
  const sync::SaveTarget* archival = sync::MatchTarget(targets, operation);
  checks.Expect(archival != nullptr && archival->sd_path == "/retroarch/saves/Archive.srm",
                "an archival operation pairs only with an archival save");

  operation.rom_id = 5;
  checks.Expect(sync::MatchTarget(targets, operation) == nullptr,
                "and another rom's save is not a match at all");
}

// --- staging ------------------------------------------------------------------
//
// The two `io` primitives an overwrite goes through. `CopyAtomically` is how a
// save is backed up -- streamed, because `WriteAtomically` takes the contents as
// a `string_view` and a save state does not fit in a 1.5 MiB heap -- and
// `CommitStaged` is how the downloaded bytes take its place.

void Staging(rig::Checks& checks) {
  Sandbox sandbox(checks, "execute-staging");
  const std::string from = sandbox.Host("/config/rommsync/source");
  const std::string to = sandbox.Host("/config/rommsync/destination");

  // Nothing there to copy is its own outcome, and the caller reads it as "this
  // overwrite destroys nothing" rather than as a failure.
  const io::CopyResult absent = io::CopyAtomically(from, to);
  checks.Expect(absent.error == io::CopyError::kSourceMissing,
                std::string("a missing source is named: ") + io::ToString(absent.error));
  checks.Expect(!std::filesystem::exists(to), "...and nothing was created at the destination");

  // Bigger than the 4 KiB chunk, so the loop is exercised rather than the first
  // read. A copy that only ever works for small files is a copy that works for
  // every test and no save state.
  std::string bytes;
  for (int index = 0; index < 5000; ++index) {
    bytes += static_cast<char>('a' + (index % 26));
  }
  checks.Expect(rig::WriteFile(from, bytes), "the source is written");
  const io::CopyResult copied = io::CopyAtomically(from, to);
  checks.Expect(copied.ok(), "a multi-chunk copy: " + copied.message);
  checks.ExpectEq(copied.bytes_copied, static_cast<std::uint64_t>(bytes.size()),
                  "the whole file was copied");
  checks.ExpectEq(rig::ReadFile(to), bytes, "byte for byte");
  checks.ExpectEq(rig::ReadFile(from), bytes, "and the source is still there -- it is a copy");
  checks.Expect(!std::filesystem::exists(io::TempPathFor(to)) &&
                    !std::filesystem::exists(io::PreviousPathFor(to)),
                "with no .tmp or .old left behind");

  // Onto something that already exists, which is the case Horizon's rename
  // refuses and the two-rename commit exists for.
  checks.Expect(rig::WriteFile(from, "shorter\n"), "the source changes");
  checks.Expect(io::CopyAtomically(from, to).ok(), "a copy over an existing destination");
  checks.ExpectEq(rig::ReadFile(to), std::string("shorter\n"), "replaces it completely");
  checks.Expect(!std::filesystem::exists(io::PreviousPathFor(to)),
                "and does not leave the previous contents under .old");

  // `CommitStaged` with nothing staged must not touch the destination: it is
  // the step a download reaches only after verifying, and a failure here that
  // removed the save would be the exact loss this module exists to rule out.
  const std::string staged = sandbox.Host("/config/rommsync/staged");
  const io::WriteResult nothing = io::CommitStaged(staged, to);
  checks.Expect(nothing.error == io::WriteError::kOpenFailed,
                std::string("committing nothing is named: ") + io::ToString(nothing.error));
  checks.ExpectEq(rig::ReadFile(to), std::string("shorter\n"), "and the destination is untouched");

  checks.Expect(rig::WriteFile(staged, "the verified bytes\n"), "something is staged");
  checks.Expect(io::CommitStaged(staged, to).ok(), "and committed");
  checks.ExpectEq(rig::ReadFile(to), std::string("the verified bytes\n"), "onto the destination");
  checks.Expect(!std::filesystem::exists(staged), "the staged file is consumed");
  checks.Expect(!std::filesystem::exists(io::PreviousPathFor(to)), "and the .old is cleaned up");
}

// --- crash_model --------------------------------------------------------------
//
// M9-14 (#203). `staging` proves the two primitives an overwrite goes through
// in the order they are *called*; this proves them in the order they *persist*,
// by cutting the recorded calls at every point and asking what a card that
// does not journal may have kept (tests/crash_disk.hpp).
//
// The first three are the replay's own check: overwrites that are wrong in the
// three ways a rename protocol goes wrong, each of which it must catch. A
// replay that passed them would pass anything. Then the protocol itself, and
// then the protocol with the card refusing to confirm the backup's *name* --
// the case the call order alone said nothing about.

/// A replay of `recorder` against hard rule 2's invariant, with no checks
/// attached: for the overwrites that are supposed to fail it.
crash::Verdict Replayed(const crash::Recorder& recorder, const crash::Overwrite& expected) {
  return crash::Replay(recorder, {harness::kSavesDir, harness::kBackupDir}, {harness::kSavesDir},
                       rig::ScratchDir() + "/crash-model", crash::OldOrNew(expected));
}

/// `BackUpFirst` with the values every case below shares.
sync::OperationError BackUpForCrash(fs::FileSystem& files, const std::string& save,
                                    std::string* backup, std::string* message) {
  return sync::BackUpFirst(files, harness::kBackupDir, save, 7, "slot-1", "crash.srm",
                           1'757'000'100, backup, message);
}

void CrashModel(rig::Checks& checks) {
  const std::string old_bytes = "the save a power cut must not cost\n";
  const std::string new_bytes = "the server's copy, which replaces it after a backup\n";
  const std::string save = SavePath("crash.srm");
  const crash::Overwrite expected{save, old_bytes, new_bytes, harness::kBackupDir};

  // 1. Written in place, over the save. A cut mid-write leaves half of each.
  {
    Sandbox sandbox(checks, "crash-in-place");
    sandbox.Detach();  // the audit is not the subject; the replay is
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.Write(save, old_bytes);
    crash::Recorder recorder(sandbox.root());
    std::string backup;
    std::string message;
    BackUpForCrash(*files, save, &backup, &message);
    std::FILE* file = io::OpenForWriting(sandbox.Host(save), "wb");
    if (file != nullptr) {
      io::Write(file, new_bytes.data(), new_bytes.size());
      std::fclose(file);
      io::Sync(sandbox.Host(save));
    }
    const crash::Verdict verdict = Replayed(recorder, expected);
    checks.Expect(verdict.failures > 0 && verdict.first.find("neither") != std::string::npos,
                  "the replay catches an overwrite written in place: " + verdict.first);
  }

  // 2. Atomic and durable, and with no backup. The save survives every cut and
  // the old bytes do not.
  {
    Sandbox sandbox(checks, "crash-no-backup");
    sandbox.Detach();
    sandbox.Write(save, old_bytes);
    crash::Recorder recorder(sandbox.root());
    io::WriteAtomically(sandbox.Host(save), new_bytes);
    const crash::Verdict verdict = Replayed(recorder, expected);
    checks.Expect(verdict.failures > 0 && verdict.first.find("no backup") != std::string::npos,
                  "the replay catches an overwrite with no backup: " + verdict.first);
  }

  // 3. Backed up, staged, renamed -- and the staged bytes never synced. The
  // rename can land with data blocks that did not: ext4's zero-length file.
  {
    Sandbox sandbox(checks, "crash-unsynced");
    sandbox.Detach();
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.Write(save, old_bytes);
    crash::Recorder recorder(sandbox.root());
    std::string backup;
    std::string message;
    BackUpForCrash(*files, save, &backup, &message);
    const std::string staged = io::TempPathFor(sandbox.Host(save));
    std::FILE* file = io::OpenForWriting(staged, "wb");
    if (file != nullptr) {
      io::Write(file, new_bytes.data(), new_bytes.size());
      std::fclose(file);
    }
    io::CommitStaged(staged, sandbox.Host(save));
    const crash::Verdict verdict = Replayed(recorder, expected);
    checks.Expect(verdict.failures > 0 && verdict.first.find("neither") != std::string::npos,
                  "the replay catches a rename that published unsynced bytes: " + verdict.first);
  }

  // 4. The protocol: `BackUpFirst`, then `WriteAtomically`, which is
  // `CommitStaged` behind a synced temp file.
  {
    Sandbox sandbox(checks, "crash-protocol");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(save, old_bytes);
    crash::Recorder recorder(sandbox.root());
    std::string backup;
    std::string message;
    checks.Expect(BackUpForCrash(*files, save, &backup, &message) == sync::OperationError::kNone,
                  "the backup is written: " + message);
    checks.Expect(io::WriteAtomically(sandbox.Host(save), new_bytes).ok(), "and the save replaced");
    crash::ExpectSurvives(checks, recorder, expected, rig::ScratchDir() + "/crash-model",
                          "backup, then an atomic overwrite");
  }

  // 5. The same, with the card refusing to confirm the backup directory. The
  // backup's bytes are on the card and its *name* may not be, and an overwrite
  // in a different directory can land without it. `BackUpFirst` has to say no.
  {
    Sandbox sandbox(checks, "crash-unconfirmed");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(save, old_bytes);
    crash::Recorder recorder(sandbox.root(), {harness::kBackupDir});
    std::string backup;
    std::string message;
    const sync::OperationError backed_up = BackUpForCrash(*files, save, &backup, &message);
    if (backed_up == sync::OperationError::kNone) {
      io::WriteAtomically(sandbox.Host(save), new_bytes);
    }
    checks.Expect(recorder.refused() > 0, "the card refused to confirm the backup directory");
    checks.Expect(backed_up == sync::OperationError::kBackupFailed,
                  std::string("a backup whose name the card would not confirm is not one: ") +
                      sync::ToString(backed_up) + " -- " + message);
    crash::ExpectSurvives(checks, recorder, expected, rig::ScratchDir() + "/crash-model",
                          "a backup the card would not confirm");
  }
}

// --- counting -----------------------------------------------------------------
//
// What the report says, for the operations that need no server at all. The
// counts are what M2-6 reports to `complete`, and the one that is easy to get
// wrong is the downgraded action: M2-4 delivers an `action` this build does not
// know as a `no_op`, and counting it as an ordinary completed no-op would tell
// the server the client did what was asked when it does not know what was
// asked.
//
// The client here fails the test if it is used, which is the assertion: none of
// these operations may reach the network.

class NeverCalled : public http::HttpClient {
 public:
  explicit NeverCalled(rig::Checks& checks) : checks_(&checks) {}

  http::Result Send(const http::Request& request) override { return Refuse(request); }
  http::Result Download(const http::Request& request, const http::DownloadTarget&) override {
    return Refuse(request);
  }

 private:
  http::Result Refuse(const http::Request& request) {
    checks_->Expect(false, "nothing should have been sent, and a request went to " + request.url);
    http::Result result;
    result.error = http::Error::kTransport;
    return result;
  }

  rig::Checks* checks_;
};

/// An `HttpClient` whose every exchange reports the caller's cancellation.
///
/// The real one is the backend polling `http::CancelToken` mid-transfer, which
/// no test can time. What matters is what the executor does with the answer, and
/// that is the same either way.
class AlwaysCanceled : public http::HttpClient {
 public:
  http::Result Send(const http::Request&) override { return Canceled(); }
  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    return Canceled();
  }

 private:
  static http::Result Canceled() {
    http::Result result;
    result.error = http::Error::kCanceled;
    result.message = "the caller cancelled";
    return result;
  }
};

sync::SyncOperation OperationOf(sync::Action action, std::int64_t rom_id, std::string slot) {
  sync::SyncOperation operation;
  operation.action = action;
  operation.action_text = sync::ToString(action);
  operation.rom_id = rom_id;
  operation.slot = std::move(slot);
  operation.file_name = "whatever [2026-09-04_11-12-27].srm";
  operation.reason = sync::Reason::kNoChanges;
  operation.reason_text = sync::ReasonText(sync::Reason::kNoChanges);
  return operation;
}

void Counting(rig::Checks& checks) {
  Sandbox sandbox(checks, "execute-counting");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  NeverCalled client(checks);
  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "not-used";

  sync::SyncPlan plan;
  plan.session_id = 1;
  plan.operations.push_back(OperationOf(sync::Action::kNoOp, 1, "planned-no-op"));

  // What M2-4 produces for an action this build does not know: `no_op`, with
  // the server's word kept and `known_action` false.
  sync::SyncOperation unknown = OperationOf(sync::Action::kNoOp, 2, "server-moved-on");
  unknown.known_action = false;
  unknown.action_text = "merge";
  plan.operations.push_back(unknown);

  // An upload of a save that is not on this card, and a download of a save the
  // server named no id for. Neither can be attempted, and neither is a reason
  // to stop the plan.
  plan.operations.push_back(OperationOf(sync::Action::kUpload, 3, "gone-from-the-card"));
  plan.operations.push_back(OperationOf(sync::Action::kDownload, 4, "no-save-id"));

  sync::ExecuteOptions options = OptionsAt(1'757'000'000);
  const sync::ExecutionReport report =
      sync::ExecutePlan(client, *files, token, plan, /*targets=*/{}, options);

  checks.ExpectEq(report.completed, 1, "only the planned no-op counts as work done");
  checks.ExpectEq(report.not_understood, 1, "the downgraded action is counted apart");
  checks.ExpectEq(report.failed, 2, "and the two that could not be attempted are failures");
  if (report.operations.size() == 4) {
    checks.Expect(report.operations[0].outcome == sync::OperationOutcome::kNoOp,
                  "the planned no-op did nothing, on purpose");
    checks.Expect(report.operations[1].outcome == sync::OperationOutcome::kNotUnderstood,
                  std::string("the unknown action is not an ordinary no-op: ") +
                      sync::ToString(report.operations[1].outcome));
    checks.Expect(report.operations[1].message.find("merge") != std::string::npos,
                  "and the log line says what the server actually asked for: " +
                      report.operations[1].message);
    checks.Expect(report.operations[2].error == sync::OperationError::kNoLocalSave,
                  std::string("an upload with nothing to send is named: ") +
                      sync::ToString(report.operations[2].error));
    checks.Expect(report.operations[3].error == sync::OperationError::kNoSaveId,
                  std::string("a download the server named no save for is named: ") +
                      sync::ToString(report.operations[3].error));

    // Each line written for an operation is that operation's by position, not by
    // its text (#259): a resolution removes the 409's line through this.
    checks.Expect(report.operations[0].warning == sync::kNoWarning,
                  "a planned no-op writes no line");
    for (std::size_t at = 1; at < report.operations.size(); ++at) {
      const std::int32_t line = report.operations[at].warning;
      checks.Expect(line >= 0 && static_cast<std::size_t>(line) < report.warnings.size() &&
                        report.warnings[static_cast<std::size_t>(line)] ==
                            report.operations[at].message,
                    "operation " + std::to_string(at) + " names its own line in warnings");
    }
  }

  // One mapping from an outcome to a total (#259): what `ExecutePlan` counted,
  // recounted from the results, is the same three numbers.
  {
    sync::ExecutionReport recounted = report;
    recounted.completed = 0;
    recounted.failed = 0;
    recounted.not_understood = 0;
    sync::Recount(&recounted);
    checks.Expect(recounted.completed == 1 && recounted.failed == 2 &&
                      recounted.not_understood == 1,
                  "Recount agrees with what ExecutePlan counted");
  }

  // Cancellation lands on an operation boundary. The plan above is all no-ops
  // and refusals, so what this proves is where the loop stops, not that a write
  // was interrupted -- nothing here can be mid-write.
  http::CancelToken cancel;
  cancel.Cancel();
  options.cancel = &cancel;
  const sync::ExecutionReport stopped =
      sync::ExecutePlan(client, *files, token, plan, /*targets=*/{}, options);
  checks.Expect(stopped.canceled, "a cancelled token stops the plan");
  checks.ExpectEq(static_cast<int>(stopped.operations.size()), 0,
                  "before the first operation, since it was already cancelled");

  // A cancellation that lands *during* an exchange, which is the case the token
  // is really for. It must not be reported as a failed operation: M2-6 sends
  // `operations_failed` to the server, and a shutdown is not work that went
  // wrong. The rest of the plan is not attempted either.
  AlwaysCanceled interrupted;
  const std::string name = "canceled.srm";
  sandbox.Write(SavePath(name), "bytes nobody gets to send\n");
  sync::SyncPlan two;
  two.session_id = 1;
  two.operations.push_back(OperationOf(sync::Action::kUpload, 7, "cut-short"));
  two.operations.push_back(OperationOf(sync::Action::kNoOp, 8, "never-reached"));
  const std::vector<sync::SaveTarget> targets = {
      {7, std::string("cut-short"), SavePath(name), name}};
  const sync::ExecutionReport cut = sync::ExecutePlan(
      interrupted, *files, token, two, targets, OptionsAt(1'757'000'000));
  checks.Expect(cut.canceled, "a cancelled exchange cancels the plan");
  checks.ExpectEq(cut.failed, 0, "and is not counted as a failure");
  checks.ExpectEq(cut.completed, 0, "nor as work done");
  checks.ExpectEq(static_cast<int>(cut.operations.size()), 1,
                  "the operations after it are not attempted");
  if (!cut.operations.empty()) {
    checks.Expect(cut.operations[0].outcome == sync::OperationOutcome::kCanceled,
                  std::string("...and it has an outcome of its own: ") +
                      sync::ToString(cut.operations[0].outcome));
    checks.Expect(cut.operations[0].error == sync::OperationError::kCanceled,
                  std::string("...named: ") + sync::ToString(cut.operations[0].error));
  }
}

// --- overwrite ----------------------------------------------------------------
//
// #232: an upload respects RomM's slot guard. `overwrite=false` unless the
// player chose "Keep local" for that save, and the 409 the guard answers with
// is named as itself -- a newer save in the slot -- rather than as a bare
// refusal, because the tick resolves it (sync_tick.hpp) and a refusal it does
// not. Needs no server: what is being checked is the request that leaves and
// how the answer is read, so a client that records the one and scripts the
// other is the whole of it.

/// Records every request and answers each with `status` and `body`.
class Scripted final : public http::HttpClient {
 public:
  Scripted(int status, std::string body) : status_(status), body_(std::move(body)) {}

  http::Result Send(const http::Request& request) override {
    urls.push_back(request.url);
    methods.push_back(request.method);
    http::Result result;
    result.response.status = status_;
    result.response.body = body_;
    return result;
  }
  http::Result Download(const http::Request& request, const http::DownloadTarget&) override {
    urls.push_back(request.url);
    http::Result result;
    result.error = http::Error::kConnectFailed;
    return result;
  }

  std::vector<std::string> urls;
  std::vector<http::Method> methods;

 private:
  int status_;
  std::string body_;
};

void Overwrite(rig::Checks& checks) {
  Sandbox sandbox(checks, "execute-overwrite");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "this-console";

  const std::string bytes = "the bytes this console has\n";
  sandbox.SeedSave(SavePath("guarded.srm"), bytes);
  sandbox.SeedSave(SavePath("kept.srm"), bytes);
  sync::SyncPlan plan;
  plan.session_id = 1;
  plan.operations.push_back(OperationOf(sync::Action::kUpload, 7, "guarded"));
  plan.operations.push_back(OperationOf(sync::Action::kUpload, 8, "kept"));
  const std::vector<sync::SaveTarget> targets = {
      {7, std::string("guarded"), SavePath("guarded.srm"), "guarded.srm"},
      {8, std::string("kept"), SavePath("kept.srm"), "kept.srm"}};

  // Accepted, so what is being read is the request alone.
  {
    Scripted accepted(201, R"({"id":41})");
    sync::ExecuteOptions options = OptionsAt(1'757'000'000);
    options.keep_local = {{8, std::string("kept")}};
    options.wait = [](std::chrono::milliseconds) {};
    const sync::ExecutionReport report =
        sync::ExecutePlan(accepted, *files, token, plan, targets, options);
    checks.ExpectEq(report.completed, 2, "both uploads are accepted");
    checks.Expect(report.warnings.empty(),
                  "with nothing to say" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
    // A POST per upload, and the Keep local's row rewritten in place after it
    // (#272): the second has to turn under it, so the row is its slot's newest
    // however RomM's negotiate breaks a tie.
    if (accepted.urls.size() == 3) {
      checks.Expect(accepted.urls[0].find("overwrite=false") != std::string::npos &&
                        accepted.urls[0].find("overwrite=true") == std::string::npos,
                    "an upload the plan asked for respects the slot guard: " + accepted.urls[0]);
      checks.Expect(accepted.urls[1].find("overwrite=true") != std::string::npos,
                    "and only an explicit Keep local overrides it: " + accepted.urls[1]);
      checks.Expect(accepted.methods[2] == http::Method::kPut &&
                        accepted.urls[2].find("/api/saves/41?") != std::string::npos &&
                        accepted.urls[2].find("device_id=this-console") != std::string::npos,
                    "which then rewrites the row it wrote, as this device: " + accepted.urls[2]);
    } else {
      checks.Expect(false, "a request per upload and the Keep local's rewrite, got " +
                               std::to_string(accepted.urls.size()));
    }
  }

  // A rewrite that does not happen leaves the upload standing -- it landed --
  // and is said, because the next tick may then undo the choice (#272). It is
  // the one call here that is retried: the same bytes onto the same row.
  {
    // Answers the upload, and the rewrite with `rewrite_status`.
    class Rewrites final : public http::HttpClient {
     public:
      explicit Rewrites(int rewrite_status) : rewrite_status_(rewrite_status) {}
      http::Result Send(const http::Request& request) override {
        http::Result result;
        const bool put = request.method == http::Method::kPut;
        puts += put ? 1 : 0;
        result.response.status = put ? rewrite_status_ : 201;
        result.response.body = R"({"id":41,"updated_at":"2026-10-23T18:00:00.250000+00:00"})";
        return result;
      }
      http::Result Download(const http::Request&, const http::DownloadTarget&) override {
        http::Result result;
        result.error = http::Error::kConnectFailed;
        return result;
      }
      int puts = 0;

     private:
      int rewrite_status_;
    };
    sync::SyncPlan one = plan;
    one.operations.erase(one.operations.begin());

    Rewrites unavailable(503);
    std::vector<std::chrono::milliseconds> waited;
    sync::ExecuteOptions options = OptionsAt(1'757'000'002);
    options.keep_local = {{8, std::string("kept")}};
    options.wait = [&waited](std::chrono::milliseconds delay) { waited.push_back(delay); };
    const sync::ExecutionReport report =
        sync::ExecutePlan(unavailable, *files, token, one, targets, options);
    checks.ExpectEq(report.completed, 1, "the Keep local upload still counts as done");
    checks.ExpectEq(unavailable.puts, 3, "a 503 is tried three times");
    checks.Expect(waited.size() == 3 && waited[0] == std::chrono::milliseconds{1'100} &&
                      waited[1] > waited[0] && waited[2] > waited[1],
                  "the first past the second the upload landed in, each after a longer wait");
    checks.Expect(report.warnings.size() == 1 &&
                      report.warnings[0].find("not moved past it (the rewrite was refused: "
                                              "HTTP 503)") != std::string::npos,
                  "and the rewrite that failed is said: " +
                      (report.warnings.empty() ? std::string("nothing") : report.warnings[0]));

    Rewrites refused(404);
    options.wait = [](std::chrono::milliseconds) {};
    const sync::ExecutionReport gone =
        sync::ExecutePlan(refused, *files, token, one, targets, options);
    checks.ExpectEq(refused.puts, 1, "a refusal of the rewrite is not asked again");
    checks.ExpectEq(gone.warnings.size(), static_cast<std::size_t>(1), "and is said too");

    // The player writes the save in the second the rewrite waits out: those are
    // new bytes, not the ones the Keep local sent, and they are not put up as if
    // they were.
    Rewrites moved(200);
    options.wait = [&sandbox](std::chrono::milliseconds) {
      sandbox.Write(SavePath("kept.srm"), "progress made in the meantime\n");
    };
    const sync::ExecutionReport changed =
        sync::ExecutePlan(moved, *files, token, one, targets, options);
    checks.ExpectEq(moved.puts, 0, "a save that changed on the card is not rewritten");
    checks.Expect(changed.warnings.size() == 1 &&
                      changed.warnings[0].find("the save changed on the card since") !=
                          std::string::npos,
                  "and that is said: " + (changed.warnings.empty() ? std::string("nothing")
                                                                    : changed.warnings[0]));
    sandbox.Write(SavePath("kept.srm"), bytes);
  }

  // Refused with the guard's own answer.
  {
    Scripted guarded(409, R"({"detail":"Slot has a newer save since your last sync"})");
    sync::SyncPlan one = plan;
    one.operations.resize(1);
    const sync::ExecutionReport report =
        sync::ExecutePlan(guarded, *files, token, one, targets, OptionsAt(1'757'000'001));
    checks.ExpectEq(report.failed, 1, "a 409 is not an upload that happened");
    if (report.operations.size() == 1) {
      const sync::OperationResult& done = report.operations[0];
      checks.Expect(done.error == sync::OperationError::kNewerOnServer,
                    std::string("...and is named as a newer save in the slot: ") +
                        sync::ToString(done.error) + " -- " + done.message);
      checks.Expect(sync::AnswerOf(done.error) == auth::Answer::kSilent,
                    "and says nothing about the pairing");
    }
    checks.ExpectEq(guarded.urls.size(), static_cast<std::size_t>(1),
                    "and it is not re-posted with overwrite=true behind the server's back");
    checks.ExpectEq(sandbox.Read(SavePath("guarded.srm")), bytes, "the local save is untouched");
  }
}

// --- content_hash -------------------------------------------------------------
//
// #234: RomM 5.4 keeps a per-device baseline from the `content_hash` an upload
// and a `/downloaded` carry, and treats a later report matching it as proof
// that side did not change. So both carry one, and it is the digest of the
// bytes that actually moved: the file as the upload streams it, and the
// verified bytes a download put on the card. Needs no server -- what is checked
// is what leaves -- and the digests are literals, worked out once, not
// recomputed the way the code computes them.

/// Answers the four calls an upload and a download make, and nothing else.
class SaveServer final : public http::HttpClient {
 public:
  explicit SaveServer(std::string bytes) : bytes_(std::move(bytes)) {}

  http::Result Send(const http::Request& request) override {
    http::Result result;
    result.response.status = 200;
    if (request.url.find("/api/saves?") != std::string::npos) {
      result.response.body = R"({"id":41})";
    } else if (request.url.find("/api/saves/5/downloaded") != std::string::npos) {
      result.response.body = R"({"id":5})";
    } else if (request.url.find("/api/saves/5") != std::string::npos) {
      result.response.body = R"({"id":5,"file_size_bytes":)" + std::to_string(bytes_.size()) +
                             R"(,"content_hash":"0931bd07b94c9c07f0645fa14e9a5092"})";
    } else {
      result.response.status = 404;
    }
    return result;
  }
  http::Result Download(const http::Request&, const http::DownloadTarget& into) override {
    rig::WriteFile(into.path, bytes_);
    http::Result result;
    result.response.status = 200;
    return result;
  }

 private:
  std::string bytes_;
};

void ContentHash(rig::Checks& checks) {
  Sandbox sandbox(checks, "execute-content-hash");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "this-console";

  // The card moved on after the scan that negotiated: what goes up is these
  // bytes, so the baseline must be their digest and not the scan's.
  sandbox.SeedSave(SavePath("up.srm"), "the bytes on the card when the upload went\n");
  sandbox.SeedSave(SavePath("down.srm"), "the local copy the download replaces\n");

  sync::SyncPlan plan;
  plan.session_id = 1;
  plan.operations.push_back(OperationOf(sync::Action::kUpload, 7, "up"));
  sync::SyncOperation download = OperationOf(sync::Action::kDownload, 8, "down");
  download.save_id = 5;
  download.server_content_hash = "0931bd07b94c9c07f0645fa14e9a5092";
  plan.operations.push_back(download);
  const std::vector<sync::SaveTarget> targets = {
      {7, std::string("up"), SavePath("up.srm"), "up.srm"},
      {8, std::string("down"), SavePath("down.srm"), "down.srm"}};

  SaveServer server("the server copy, verified\n");
  harness::BaselineAudit audit(server);
  const sync::ExecutionReport report =
      sync::ExecutePlan(audit, *files, token, plan, targets, OptionsAt(1'757'000'000));

  checks.ExpectEq(report.completed, 2,
                  "the upload and the download both completed" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
  for (const std::string& violation : audit.violations) {
    checks.Expect(false, violation);
  }
  checks.Expect(audit.uploaded == std::vector<std::string>{"52aa82374597bb39c0ceac4261823731"},
                "the upload carries the MD5 of the bytes it sent");
  checks.Expect(audit.confirmed == std::vector<std::string>{"0931bd07b94c9c07f0645fa14e9a5092"},
                "and the confirmation the MD5 of the verified bytes now on the card");
  checks.ExpectEq(sandbox.Read(SavePath("down.srm")), std::string("the server copy, verified\n"),
                  "which are the server's");
}

// --- isolated -----------------------------------------------------------------
//
// #238: a save in a format only its own emulator reads -- a `tico-*` id -- is
// never written where anything else reads, and nothing else is ever written over
// one. RomM pairs saves on `(rom_id, slot)` whatever wrote them, so the plan can
// hand this client a Beetle `.srm` for DuckStation's card; the refusal happens
// before a byte moves, which `NeverCalled` asserts. `tick.isolated` is the same
// thing against a live 5.4.0.

/// A download or a conflict of the server's copy, written by `emulator`.
/// A placement at `sd_path`, naming the emulator it writes for -- or not, when
/// `emulator` is empty, so the executor asks `reads_as`.
rommsync::place::Placement PlacedAt(std::string sd_path, std::string emulator) {
  rommsync::place::Placement placement;
  placement.sd_path = std::move(sd_path);
  placement.emulator = std::move(emulator);
  return placement;
}

sync::SyncOperation FetchOf(sync::Action action, std::int64_t rom_id, const std::string& slot,
                            const std::string& emulator) {
  sync::SyncOperation operation = OperationOf(action, rom_id, slot);
  operation.save_id = 5;
  operation.emulator = emulator;
  operation.server_content_hash = "0931bd07b94c9c07f0645fa14e9a5092";
  operation.reason = action == sync::Action::kConflict ? sync::Reason::kBothChanged
                                                       : sync::Reason::kServerNewerNoHistory;
  operation.reason_text = sync::ReasonText(operation.reason);
  return operation;
}

// --- declined -----------------------------------------------------------------
//
// #257: a save this console will not take -- a pairing across formats, a
// first-time restore with no placement or onto a file already there -- is
// declined rather than failed, and said once until either side changes. All of
// it is decided before a byte moves, which `NeverCalled` asserts.
// `tick.declined` is the same against a live 5.4.0, with the file moving.

/// Whether `lines` holds the line `done` would be said with.
bool Said(const std::vector<std::string>& lines, const sync::OperationResult& done) {
  return std::find(lines.begin(), lines.end(), "declined " + done.message) != lines.end();
}

void Declined(rig::Checks& checks) {
  Sandbox sandbox(checks, "execute-declined");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "this-console";

  const std::string card = "DuckStation's memory card\n";
  const std::string other = "a file the scan did not pair with this row\n";
  sandbox.SeedSave(SavePath("Card.mcd"), card);
  sandbox.SeedSave(SavePath("Taken.mcd"), other);
  const std::vector<sync::ClientSaveState> reported = {
      harness::LocalSave(7, "Card.mcd", "card", "tico-duckstation", crypto::Md5Hex(card),
                         std::chrono::system_clock::now(), 26)};
  const std::vector<sync::SaveTarget> targets = {
      {7, std::string("card"), SavePath("Card.mcd"), "Card.mcd"}};

  sync::SyncPlan plan;
  plan.session_id = 1;
  // A Beetle save paired with DuckStation's card, a save with nowhere to go,
  // and one whose place is taken.
  plan.operations.push_back(FetchOf(sync::Action::kDownload, 7, "card", "mednafen_psx_hw"));
  plan.operations.push_back(FetchOf(sync::Action::kDownload, 8, "unmapped", "tico-duckstation"));
  plan.operations.push_back(FetchOf(sync::Action::kDownload, 9, "taken", "tico-duckstation"));

  std::string reason = "psx is not mapped to any folder";
  sync::ExecuteOptions options = OptionsAt(1'757'000'000);
  options.reported = &reported;
  options.place = [&reason](const sync::SyncOperation& operation) {
    if (*operation.slot == "taken") {
      return PlacedAt(SavePath("Taken.mcd"), "tico-duckstation");
    }
    rommsync::place::Placement nowhere;
    nowhere.reason = reason;
    return nowhere;
  };
  sync::Declines declines;
  options.declines = &declines;
  NeverCalled client(checks);

  const sync::ExecutionReport first =
      sync::ExecutePlan(client, *files, token, plan, targets, options);
  checks.ExpectEq(first.declined, 3, "all three are declined");
  checks.ExpectEq(first.failed, 0, "and none of them is a failure");
  checks.ExpectEq(first.completed, 0, "nor work done");
  checks.Expect(first.warnings.empty(),
                "a decline writes no failed save's line" +
                    (first.warnings.empty() ? std::string() : ": " + first.warnings[0]));
  checks.ExpectEq(first.declines.size(), std::size_t{3}, "each is said, the first time");
  if (first.operations.size() == 3) {
    const sync::OperationError expected[] = {sync::OperationError::kOtherEmulator,
                                             sync::OperationError::kNoLocalSave,
                                             sync::OperationError::kNoLocalSave};
    for (std::size_t at = 0; at < 3; ++at) {
      const sync::OperationResult& done = first.operations[at];
      checks.Expect(done.outcome == sync::OperationOutcome::kDeclined &&
                        done.error == expected[at] && done.warning == sync::kNoWarning,
                    std::string("operation ") + std::to_string(at) + " is declined as " +
                        sync::ToString(expected[at]) + ": " + sync::ToString(done.outcome) +
                        " " + sync::ToString(done.error) + " -- " + done.message);
      checks.Expect(Said(first.declines, done), "in its own words: " + done.message);
    }
  }
  checks.ExpectEq(std::string(sync::ToString(sync::OperationOutcome::kDeclined)),
                  std::string("declined"), "named for the log");
  checks.ExpectEq(sync::CountsFor(first).operations_failed, 3,
                  "RomM is still told the three did not happen");
  checks.ExpectEq(sandbox.Read(SavePath("Card.mcd")), card, "the card is untouched");
  checks.ExpectEq(sandbox.Read(SavePath("Taken.mcd")), other, "and so is the file in the way");

  const sync::ExecutionReport second =
      sync::ExecutePlan(client, *files, token, plan, targets, options);
  checks.ExpectEq(second.declined, 3, "met again, they are declined again");
  checks.Expect(second.declines.empty(),
                "and nothing is said" +
                    (second.declines.empty() ? std::string() : ": " + second.declines[0]));

  // Either side changing is news: a new server save in the card's slot, and the
  // placement's reason moving. The file in the way has not moved.
  plan.operations[0].save_id = 6;
  reason = "Tico keeps saves for 2 accounts and [tico] account does not say whose";
  const sync::ExecutionReport changed =
      sync::ExecutePlan(client, *files, token, plan, targets, options);
  checks.ExpectEq(changed.declines.size(), std::size_t{2}, "the two that changed are said again");
  if (changed.operations.size() == 3) {
    checks.Expect(Said(changed.declines, changed.operations[0]), "a new server save is news");
    checks.Expect(Said(changed.declines, changed.operations[1]), "and a new reason");
    checks.Expect(!Said(changed.declines, changed.operations[2]), "and the taken path is not");
  }

  // The local side changing in place is news too: the card played on, so what
  // step 0 reported for it is new bytes -- which, on #232's path, is the
  // conflict a refused upload resolves to, declined again.
  {
    std::vector<sync::ClientSaveState> played = reported;
    played[0].content_hash = crypto::Md5Hex(card + "and an hour more\n");
    sync::ExecuteOptions newer = options;
    newer.reported = &played;
    const sync::ExecutionReport moved_on =
        sync::ExecutePlan(client, *files, token, plan, targets, newer);
    checks.Expect(moved_on.declines.size() == 1 && moved_on.operations.size() == 3 &&
                      Said(moved_on.declines, moved_on.operations[0]),
                  "new bytes in the card are said again, and only the card's");
    const sync::ExecutionReport same =
        sync::ExecutePlan(client, *files, token, plan, targets, newer);
    checks.Expect(same.declines.empty(), "once");
    sync::ExecutePlan(client, *files, token, plan, targets, options);
  }

  // A tick that met only one of them forgets the others: their saves left the
  // plan. One no plan asked about -- its rom in `unasked_roms` -- is kept.
  declines.EndTick({});
  sync::SyncPlan one;
  one.session_id = 2;
  one.operations.push_back(plan.operations[2]);
  sync::ExecutePlan(client, *files, token, one, targets, options);
  declines.EndTick({8});
  checks.ExpectEq(declines.size(), std::size_t{2}, "the one met and the one unasked are kept");
  const sync::ExecutionReport back =
      sync::ExecutePlan(client, *files, token, plan, targets, options);
  checks.ExpectEq(back.declines.size(), std::size_t{1}, "only the forgotten one is said again");
  if (back.operations.size() == 3) {
    checks.Expect(Said(back.declines, back.operations[0]), "which is the card's");
  }

  // With nothing to remember them in, every decline is said every time -- and
  // still is not a failure.
  {
    sync::ExecuteOptions forgetful = options;
    forgetful.declines = nullptr;
    const sync::ExecutionReport again =
        sync::ExecutePlan(client, *files, token, plan, targets, forgetful);
    checks.Expect(again.declines.size() == 3 && again.failed == 0 && again.declined == 3,
                  "with no memory each is said, and none fails");
  }

  // Past the bound, the ones remembered stay quiet and the rest are said.
  {
    sync::SyncPlan many;
    many.session_id = 3;
    for (std::size_t at = 0; at <= sync::kMaxDeclines; ++at) {
      many.operations.push_back(FetchOf(sync::Action::kDownload,
                                        100 + static_cast<std::int64_t>(at), "unmapped",
                                        "tico-duckstation"));
    }
    sync::Declines bounded;
    sync::ExecuteOptions full = options;
    full.declines = &bounded;
    const sync::ExecutionReport filled =
        sync::ExecutePlan(client, *files, token, many, targets, full);
    checks.ExpectEq(filled.declines.size(), sync::kMaxDeclines + 1, "every one is said once");
    checks.ExpectEq(bounded.size(), sync::kMaxDeclines, "and the bound's worth is remembered");
    const sync::ExecutionReport over =
        sync::ExecutePlan(client, *files, token, many, targets, full);
    checks.ExpectEq(over.declines.size(), std::size_t{1}, "only the one past it is said again");
  }
}

void Isolated(rig::Checks& checks) {
  Sandbox sandbox(checks, "execute-isolated");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "this-console";

  const std::string card = "DuckStation's memory card\n";
  const std::string retroarch = "a RetroArch core's save\n";
  sandbox.SeedSave(SavePath("Card.mcd"), card);
  sandbox.SeedSave(SavePath("Kept.mcd"), card);
  sandbox.SeedSave(SavePath("Core.srm"), retroarch);
  const std::vector<sync::ClientSaveState> reported = {
      harness::LocalSave(7, "Card.mcd", "card", "tico-duckstation", crypto::Md5Hex(card),
                         std::chrono::system_clock::now(), 26),
      harness::LocalSave(8, "Kept.mcd", "kept", "tico-duckstation", crypto::Md5Hex(card),
                         std::chrono::system_clock::now(), 26),
      harness::LocalSave(9, "Core.srm", "core", "pcsx_rearmed", crypto::Md5Hex(retroarch),
                         std::chrono::system_clock::now(), 24)};
  const std::vector<sync::SaveTarget> targets = {
      {7, std::string("card"), SavePath("Card.mcd"), "Card.mcd"},
      {8, std::string("kept"), SavePath("Kept.mcd"), "Kept.mcd"},
      {9, std::string("core"), SavePath("Core.srm"), "Core.srm"}};

  // Everything the refusals need is local, so none of them sends a request.
  {
    sync::SyncPlan plan;
    plan.session_id = 1;
    plan.operations.push_back(FetchOf(sync::Action::kDownload, 7, "card", "mednafen_psx_hw"));
    plan.operations.push_back(FetchOf(sync::Action::kConflict, 8, "kept", "pcsx_rearmed"));
    plan.operations.push_back(FetchOf(sync::Action::kDownload, 9, "core", "tico-duckstation"));
    sync::SyncOperation unnamed = FetchOf(sync::Action::kDownload, 7, "card", "");
    unnamed.emulator.reset();
    plan.operations.push_back(unnamed);
    // Never seen here, placed where DuckStation reads: a Beetle save.
    plan.operations.push_back(FetchOf(sync::Action::kDownload, 10, "placed", "mednafen_psx_hw"));
    // ...and one whose placement says itself who reads it, so `reads_as` is
    // not asked.
    plan.operations.push_back(FetchOf(sync::Action::kDownload, 13, "named", "mednafen_psx_hw"));

    sync::ExecuteOptions options = OptionsAt(1'757'000'000);
    options.reported = &reported;
    options.place = [](const sync::SyncOperation& operation) {
      return PlacedAt(SavePath("sub/" + *operation.slot + ".mcd"),
                      *operation.slot == "named" ? "tico-duckstation" : "");
    };
    std::vector<std::string> asked;
    options.reads_as = [&asked](const sync::SyncOperation&, const std::string& sd_path) {
      asked.push_back(sd_path);
      return std::string("tico-duckstation");
    };
    NeverCalled client(checks);
    const sync::ExecutionReport report =
        sync::ExecutePlan(client, *files, token, plan, targets, options);

    checks.ExpectEq(report.declined, 6, "every write across formats is declined (#257)");
    checks.ExpectEq(report.failed, 0, "and none of them is a failure");
    for (const sync::OperationResult& done : report.operations) {
      checks.Expect(done.error == sync::OperationError::kOtherEmulator,
                    std::string("...as another emulator's save: ") + sync::ToString(done.error) +
                        " -- " + done.message);
      checks.Expect(done.backup_sd_path.empty(), "and no backup was needed: " + done.message);
    }
    checks.Expect(sync::AnswerOf(sync::OperationError::kOtherEmulator) == auth::Answer::kSilent,
                  "a refusal says nothing about the pairing");
    checks.ExpectEq(std::string(sync::ToString(sync::OperationError::kOtherEmulator)),
                    std::string("other_emulator"), "and is named for the log");
    checks.ExpectEq(sandbox.Read(SavePath("Card.mcd")), card, "DuckStation's card is untouched");
    checks.ExpectEq(sandbox.Read(SavePath("Kept.mcd")), card, "on a conflict too");
    checks.ExpectEq(sandbox.Read(SavePath("Core.srm")), retroarch,
                    "and a RetroArch core never takes DuckStation's card");
    checks.Expect(!sandbox.Exists(SavePath("sub")),
                  "nothing is placed for a first restore -- not even its folder");
    checks.Expect(asked == std::vector<std::string>{SavePath("sub/placed.mcd")},
                  "reads_as is asked once, for the placement that named no emulator");
    checks.Expect(!sandbox.HasBackupOf(card) && !sandbox.HasBackupOf(retroarch),
                  "and nothing went under .backup/");
  }

  // With nothing to say what a placed file is read as, an isolated format is not
  // placed at all: it cannot be shown to land where its own module reads.
  {
    sync::SyncPlan plan;
    plan.session_id = 2;
    plan.operations.push_back(FetchOf(sync::Action::kDownload, 11, "blind", "tico-mupen64plus"));
    sync::ExecuteOptions options = OptionsAt(1'757'000'001);
    options.reported = &reported;
    options.place = [](const sync::SyncOperation&) { return PlacedAt(SavePath("blind.eep"), ""); };
    NeverCalled client(checks);
    const sync::ExecutionReport report =
        sync::ExecutePlan(client, *files, token, plan, targets, options);
    checks.Expect(report.declined == 1 && report.operations.size() == 1 &&
                      report.operations[0].error == sync::OperationError::kOtherEmulator,
                  "an isolated save is not placed blind");
    checks.Expect(!sandbox.Exists(SavePath("blind.eep")), "and nothing is on the card");

    // ...and with no `reported` to say what the card is, none is written over one.
    sync::SyncPlan matched;
    matched.session_id = 2;
    matched.operations.push_back(FetchOf(sync::Action::kDownload, 7, "card", "tico-duckstation"));
    const sync::ExecutionReport unreported = sync::ExecutePlan(
        client, *files, token, matched, targets, OptionsAt(1'757'000'001));
    checks.Expect(unreported.declined == 1 && unreported.operations.size() == 1 &&
                      unreported.operations[0].error == sync::OperationError::kOtherEmulator,
                  "an isolated save is not written over a card nothing reported");
    checks.ExpectEq(sandbox.Read(SavePath("Card.mcd")), card, "which is untouched");
  }

  // The same module's card is a save like any other: verified, backed up, written.
  {
    sync::SyncPlan plan;
    plan.session_id = 3;
    plan.operations.push_back(FetchOf(sync::Action::kDownload, 7, "card", "tico-duckstation"));
    plan.operations.push_back(FetchOf(sync::Action::kDownload, 12, "fresh", "tico-duckstation"));
    sync::ExecuteOptions options = OptionsAt(1'757'000'002);
    options.reported = &reported;
    // The placement names its emulator, as `place::Placer` does; no `reads_as`.
    options.place = [](const sync::SyncOperation&) {
      return PlacedAt(SavePath("Fresh.mcd"), "tico-duckstation");
    };
    SaveServer server("the server copy, verified\n");
    const sync::ExecutionReport report =
        sync::ExecutePlan(server, *files, token, plan, targets, options);
    checks.ExpectEq(report.completed, 2,
                    "DuckStation's own card downloads, and is placed" +
                        (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
    checks.ExpectEq(sandbox.Read(SavePath("Card.mcd")), std::string("the server copy, verified\n"),
                    "over the local card");
    checks.Expect(sandbox.HasBackupOf(card), "whose bytes went under .backup/ first");
    checks.ExpectEq(sandbox.Read(SavePath("Fresh.mcd")),
                    std::string("the server copy, verified\n"), "and where DuckStation reads");
  }
}

// --- delete -------------------------------------------------------------------
//
// M10-7 (#233). 5.4.0 answers `delete` for a save whose slot was emptied on the
// server after this copy was written. It is the one action that takes a file off
// the card, so it is held to hard rule 2 exactly as an overwrite is: a backup on
// the card first, and only then the removal. Needs no server -- a delete makes
// no request at all, which `NeverCalled` asserts -- so it stays checked with
// docker stopped. `tick.deleted` is the same thing against a live 5.4.0.

/// A one-operation `delete` plan for `slot`, spelled as RomM spells it.
sync::SyncPlan DeletePlan(std::int64_t rom_id, const std::string& slot) {
  sync::SyncPlan plan;
  plan.session_id = 1;
  sync::SyncOperation operation = OperationOf(sync::Action::kDelete, rom_id, slot);
  operation.reason = sync::Reason::kDeletedOnServer;
  operation.reason_text = sync::ReasonText(sync::Reason::kDeletedOnServer);
  operation.file_name = "deleted.srm";
  plan.operations.push_back(std::move(operation));
  plan.total_delete = 1;
  return plan;
}

/// `OptionsAt`, for a server that means its `delete` -- what `RunTick` copies
/// in from `TickOptions::server`, and what a bare `ExecuteOptions` does not
/// assume.
sync::ExecuteOptions DeleteOptionsAt(std::int64_t unix_seconds) {
  sync::ExecuteOptions options = OptionsAt(unix_seconds);
  options.delete_action = true;
  return options;
}

/// What step 0 reported for one save: only the digest matters to a delete.
sync::ClientSaveState ReportedAs(std::int64_t rom_id, const std::string& slot,
                                 const std::string& name, const std::string& bytes) {
  return harness::LocalSave(rom_id, name, slot, "m10-7", crypto::Md5Hex(bytes),
                            std::chrono::system_clock::now() - std::chrono::hours{1},
                            static_cast<std::int64_t>(bytes.size()));
}

void Delete(rig::Checks& checks) {
  Sandbox sandbox(checks, "execute-delete");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  NeverCalled client(checks);
  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "not-used";
  constexpr std::int64_t kWhen = 1'757'000'500;

  // 1. remove, the default: backed up, then gone.
  {
    const std::string name = "removed.srm";
    const std::string bytes = "the copy RomM deleted\n";
    sandbox.SeedSave(SavePath(name), bytes);
    const std::vector<sync::ClientSaveState> reported = {ReportedAs(21, "gone", name, bytes)};
    sync::ExecuteOptions options = DeleteOptionsAt(kWhen);
    options.reported = &reported;
    checks.Expect(options.on_server_delete == rommsync::config::OnServerDelete::kRemove,
                  "removing is the default");
    checks.Expect(!sync::ExecuteOptions{}.delete_action && sync::ExecuteOptions{}.reported == nullptr,
                  "and a bare ExecuteOptions obeys no delete: it has to be told the server means "
                  "one, and given the digests to hold it to");
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, token, DeletePlan(21, "gone"), {{21, "gone", SavePath(name), name}},
        options);
    checks.ExpectEq(report.completed, 1, "a delete is counted as work done");
    checks.ExpectEq(report.failed, 0, "...and not as a failure");
    if (report.operations.size() == 1) {
      const sync::OperationResult& done = report.operations[0];
      checks.Expect(done.outcome == sync::OperationOutcome::kRemoved,
                    std::string("the save is removed: ") + sync::ToString(done.outcome) + " -- " +
                        done.message);
      checks.ExpectEq(done.sd_path, SavePath(name), "the result names the save");
      checks.Expect(!done.backup_sd_path.empty() && sandbox.Read(done.backup_sd_path) == bytes,
                    "and the backup it left, which holds the removed bytes: " +
                        done.backup_sd_path);
    }
    checks.Expect(!sandbox.Exists(SavePath(name)), "the save is no longer on the card");
    checks.Expect(sandbox.HasBackupOf(bytes), "and its bytes are under .backup/");
  }

  // 2. keep: nothing on the card moves, and nothing is backed up.
  {
    const std::string name = "kept.srm";
    const std::string bytes = "a copy the user keeps\n";
    sandbox.SeedSave(SavePath(name), bytes);
    const std::vector<sync::ClientSaveState> reported = {ReportedAs(22, "kept", name, bytes)};
    sync::ExecuteOptions options = DeleteOptionsAt(kWhen);
    options.reported = &reported;
    options.on_server_delete = rommsync::config::OnServerDelete::kKeep;
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, token, DeletePlan(22, "kept"), {{22, "kept", SavePath(name), name}},
        options);
    checks.ExpectEq(report.completed, 1, "a kept delete is still the plan obeyed");
    checks.Expect(report.operations.size() == 1 &&
                      report.operations[0].outcome == sync::OperationOutcome::kKept,
                  "and is named as kept");
    checks.ExpectEq(sandbox.Read(SavePath(name)), bytes, "the save is untouched");
    checks.Expect(!sandbox.HasBackupOf(bytes), "and nothing was backed up for it");
  }

  // 3. The save changed after the scan reported it. The server ruled on the
  // reported bytes, not these, so they stay -- and no copy of them is left
  // behind as a backup of something nothing replaced.
  {
    const std::string name = "changed.srm";
    const std::string bytes = "progress made after the scan\n";
    sandbox.SeedSave(SavePath(name), bytes);
    const std::vector<sync::ClientSaveState> reported = {
        ReportedAs(23, "changed", name, "the bytes the server deleted\n")};
    sync::ExecuteOptions options = DeleteOptionsAt(kWhen);
    options.reported = &reported;
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, token, DeletePlan(23, "changed"),
        {{23, "changed", SavePath(name), name}}, options);
    checks.ExpectEq(report.failed, 1, "a save that moved since the plan is not removed");
    checks.Expect(report.operations.size() == 1 &&
                      report.operations[0].error == sync::OperationError::kUnverified,
                  "and the refusal is named");
    checks.ExpectEq(sandbox.Read(SavePath(name)), bytes, "the save is untouched");
    checks.Expect(!sandbox.HasBackupOf(bytes), "and no stray copy of it is left in .backup/");
  }

  // 3b. Reported with no digest -- or not reported to the executor at all.
  // Nothing says these bytes are the ones RomM ruled on, so they stay, and no
  // copy is made of a save that is not going anywhere.
  {
    const std::string name = "undigested.srm";
    const std::string bytes = "a save step 0 could not hash\n";
    sandbox.SeedSave(SavePath(name), bytes);
    sync::ClientSaveState undigested = ReportedAs(28, "undigested", name, bytes);
    undigested.content_hash.reset();
    const std::vector<sync::ClientSaveState> reported = {undigested};
    for (const bool with_reported : {true, false}) {
      sync::ExecuteOptions options = DeleteOptionsAt(kWhen);
      options.reported = with_reported ? &reported : nullptr;
      const sync::ExecutionReport report = sync::ExecutePlan(
          client, *files, token, DeletePlan(28, "undigested"),
          {{28, "undigested", SavePath(name), name}}, options);
      checks.Expect(report.operations.size() == 1 &&
                        report.operations[0].error == sync::OperationError::kUnverified,
                    std::string("a delete with no digest to hold it to is refused (") +
                        (with_reported ? "none reported" : "nothing handed over") + ")");
    }
    checks.ExpectEq(sandbox.Read(SavePath(name)), bytes, "the save is untouched");
    checks.Expect(!sandbox.HasBackupOf(bytes), "and nothing was copied for it");
  }

  // 4. Nothing the scan reported. A delete never invents a path: `place` is
  // for a download, and a save the scan did not report -- a Tico account it
  // has not decided on (#231), a folder it could not read -- is not touched.
  {
    const std::string name = "unscanned.srm";
    const std::string bytes = "a save the scan never reported\n";
    sandbox.SeedSave(SavePath(name), bytes);
    sync::ExecuteOptions options = DeleteOptionsAt(kWhen);
    options.place = [&name](const sync::SyncOperation&) {
      place::Placement placement;
      placement.sd_path = SavePath(name);
      return placement;
    };
    const sync::ExecutionReport report =
        sync::ExecutePlan(client, *files, token, DeletePlan(24, "unscanned"), {}, options);
    checks.Expect(report.operations.size() == 1 &&
                      report.operations[0].error == sync::OperationError::kNoLocalSave,
                  "a delete with no matched save is refused");
    checks.ExpectEq(sandbox.Read(SavePath(name)), bytes, "and the file `place` names is untouched");
  }

  // 5. No backup, no removal: the directory is missing, which core/ cannot fix.
  {
    const std::string name = "unbacked.srm";
    const std::string bytes = "a save with nowhere to be backed up\n";
    sandbox.SeedSave(SavePath(name), bytes);
    const std::vector<sync::ClientSaveState> reported = {
        ReportedAs(25, "unbacked", name, bytes)};
    sync::ExecuteOptions options = DeleteOptionsAt(kWhen);
    options.reported = &reported;
    options.backup_dir = "/config/rommsync/no-such-dir";
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, token, DeletePlan(25, "unbacked"),
        {{25, "unbacked", SavePath(name), name}}, options);
    checks.Expect(report.operations.size() == 1 &&
                      report.operations[0].error == sync::OperationError::kBackupFailed,
                  "a backup that cannot be written stops the removal");
    checks.ExpectEq(sandbox.Read(SavePath(name)), bytes, "and the save is still there");
  }

  // 6. Already gone from the card between the scan and the plan: the plan's
  // end state, reached with nothing to back up.
  {
    const std::vector<sync::ClientSaveState> reported = {
        ReportedAs(26, "vanished", "vanished.srm", "bytes")};
    sync::ExecuteOptions options = DeleteOptionsAt(kWhen);
    options.reported = &reported;
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, token, DeletePlan(26, "vanished"),
        {{26, "vanished", SavePath("vanished.srm"), "vanished.srm"}}, options);
    checks.ExpectEq(report.completed, 1, "a save already gone is the delete done");
    checks.Expect(report.operations.size() == 1 &&
                      report.operations[0].outcome == sync::OperationOutcome::kRemoved &&
                      report.operations[0].backup_sd_path.empty(),
                  "with no backup, because nothing was there to protect");
  }

  // 7. A server without the capability (#228). Unreachable on a tick -- below
  // 5.4.0 a tick ends before negotiate -- and asked anyway, so a `delete` is
  // only ever honoured where the server says it means one.
  {
    const std::string name = "ungated.srm";
    const std::string bytes = "a save on a server that never deletes\n";
    sandbox.SeedSave(SavePath(name), bytes);
    const std::vector<sync::ClientSaveState> reported = {ReportedAs(27, "ungated", name, bytes)};
    sync::ExecuteOptions options = DeleteOptionsAt(kWhen);
    options.reported = &reported;
    options.delete_action = false;
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, token, DeletePlan(27, "ungated"),
        {{27, "ungated", SavePath(name), name}}, options);
    checks.ExpectEq(report.not_understood, 1, "it is not understood rather than obeyed");
    checks.ExpectEq(sandbox.Read(SavePath(name)), bytes, "and the save is untouched");
  }
}

// --- crash_delete ---------------------------------------------------------------
//
// M10-7 (#233) under M9-14's replay (#203). A removal is the one save path whose
// "new" state is no file at all, so it is judged by `crash::OldOrGone`: after a
// power cut at any call and the boot sweep, the save is its old bytes or absent,
// and absent only with those bytes under `.backup/`. First the replay's own
// check -- a removal with no backup must be caught -- then the executor, then the
// executor on a card that will not confirm the backup's name.

void CrashDelete(rig::Checks& checks) {
  const std::string bytes = "the copy RomM deleted, which a power cut must not cost\n";
  const std::string name = "crash-delete.srm";
  const std::string save = SavePath(name);
  const crash::Overwrite expected{save, bytes, std::string(), harness::kBackupDir};
  const std::string scratch = rig::ScratchDir() + "/crash-delete";
  NeverCalled client(checks);
  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "not-used";
  const std::vector<sync::ClientSaveState> reported = {ReportedAs(31, "crash", name, bytes)};
  const std::vector<sync::SaveTarget> targets = {{31, "crash", save, name}};

  // 1. Removed with no backup. Every cut after the unlink loses the save.
  {
    Sandbox sandbox(checks, "crash-delete-unbacked");
    sandbox.Detach();  // the audit is not the subject; the replay is
    sandbox.Write(save, bytes);
    crash::Recorder recorder(sandbox.root());
    io::Remove(sandbox.Host(save));
    io::Sync(sandbox.Host(harness::kSavesDir));
    const crash::Verdict verdict =
        crash::Replay(recorder, {harness::kSavesDir, harness::kBackupDir}, {harness::kSavesDir},
                      scratch, crash::OldOrGone(expected));
    checks.Expect(verdict.failures > 0 && verdict.first.find("no backup") != std::string::npos,
                  "the replay catches a removal with no backup: " + verdict.first);
  }

  // 2. The executor's own removal.
  {
    Sandbox sandbox(checks, "crash-delete");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(save, bytes);
    crash::Recorder recorder(sandbox.root());
    sync::ExecuteOptions options = DeleteOptionsAt(1'757'000'600);
    options.reported = &reported;
    const sync::ExecutionReport report =
        sync::ExecutePlan(client, *files, token, DeletePlan(31, "crash"), targets, options);
    checks.Expect(report.operations.size() == 1 &&
                      report.operations[0].outcome == sync::OperationOutcome::kRemoved,
                  "the save was removed, so the replay has a removal to cut");
    crash::ExpectSurvives(checks, recorder, expected, scratch, "backup, then a removal",
                          crash::OldOrGone(expected));
  }

  // 3. The card will not confirm the backup's name. `BackUpFirst` says no, so
  // nothing is removed -- and no cut of what it did write loses the save.
  {
    Sandbox sandbox(checks, "crash-delete-unconfirmed");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(save, bytes);
    crash::Recorder recorder(sandbox.root(), {harness::kBackupDir});
    sync::ExecuteOptions options = DeleteOptionsAt(1'757'000'601);
    options.reported = &reported;
    const sync::ExecutionReport report =
        sync::ExecutePlan(client, *files, token, DeletePlan(31, "crash"), targets, options);
    checks.Expect(recorder.refused() > 0, "the card refused to confirm the backup directory");
    checks.Expect(report.operations.size() == 1 &&
                      report.operations[0].error == sync::OperationError::kBackupFailed,
                  "a backup whose name the card would not confirm stops the removal");
    checks.ExpectEq(sandbox.Read(save), bytes, "and the save is still on the card");
    crash::ExpectSurvives(checks, recorder, expected, scratch,
                          "a removal behind a backup the card would not confirm",
                          crash::OldOrGone(expected));
  }
}

// --- late_write -----------------------------------------------------------------
//
// #263: a game writing its save while the sync acts on it. Hard rule 2's backup
// is a copy of the bytes the save held *at the copy*; a write that lands after it
// and before the save is replaced or removed is in no copy anywhere, so the
// replacement or removal has to see it and stand down. `faulty::LateWrite`
// (tests/faulty_disk.hpp) is that game, timed to the one call that separates the
// two.

void LateWriteDelete(rig::Checks& checks) {
  Sandbox sandbox(checks, "execute-late-delete");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  NeverCalled client(checks);
  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "not-used";

  const std::string name = "late-delete.srm";
  const std::string bytes = "the copy RomM deleted\n";
  const std::string late = "progress the game wrote after the backup\n";
  sandbox.SeedSave(SavePath(name), bytes);
  const std::vector<sync::ClientSaveState> reported = {ReportedAs(41, "late", name, bytes)};
  sync::ExecuteOptions options = DeleteOptionsAt(1'757'000'700);
  options.reported = &reported;

  sync::ExecutionReport report;
  {
    // `Reseed` rather than `Write`: from here on the late bytes are the save,
    // and the teardown audit holds the executor to *them*.
    faulty::LateWrite game([&] { sandbox.Reseed(SavePath(name), late); });
    report = sync::ExecutePlan(client, *files, token, DeletePlan(41, "late"),
                               {{41, "late", SavePath(name), name}}, options);
    checks.Expect(game.fired(), "the game wrote between the backup and the removal");
  }

  checks.ExpectEq(sandbox.Read(SavePath(name)), late,
                  "the write that landed after the backup is still on the card");
  checks.ExpectEq(report.failed, 1, "the removal is a failed operation, not a done one");
  if (report.operations.size() == 1) {
    const sync::OperationResult& done = report.operations[0];
    checks.Expect(done.error == sync::OperationError::kUnverified,
                  std::string("named as bytes the ruling was not about: ") +
                      sync::ToString(done.error) + " -- " + done.message);
    checks.Expect(!done.backup_sd_path.empty() && sandbox.Read(done.backup_sd_path) == bytes,
                  "and the backup is left beside it, holding the bytes RomM ruled on: " +
                      done.backup_sd_path);
  }
}

/// `sync::UnchangedSinceBackup` on its own, for the case no call through
/// `io::Disk` can time: `BackUpFirst` found nothing, so there is no backup to
/// publish, and a file appears before the commit. Nothing on the way from one
/// to the other touches the card, so the scenario is the function's answer.
void LateWriteUnbacked(rig::Checks& checks) {
  Sandbox sandbox(checks, "execute-late-unbacked");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string save = SavePath("late-unbacked.srm");
  const std::string backup = std::string(harness::kBackupDir) + "/late-unbacked-backup.srm";
  std::string message;

  checks.Expect(sync::UnchangedSinceBackup(*files, save, "", &message) ==
                    sync::OperationError::kNone,
                "nothing backed up and still nothing there: the commit may go ahead");

  sandbox.Write(save, "");
  checks.Expect(sync::UnchangedSinceBackup(*files, save, "", &message) ==
                    sync::OperationError::kUnverified,
                "nothing backed up and a file there now, even an empty one, stands it down: " +
                    message);

  sandbox.Write(save, "the bytes backed up\n");
  sandbox.Write(backup, "the bytes backed up\n");
  checks.Expect(sync::UnchangedSinceBackup(*files, save, backup, &message) ==
                    sync::OperationError::kNone,
                "a save that is still its backup may go");

  sandbox.Write(save, "the bytes backed vp\n");
  checks.Expect(sync::UnchangedSinceBackup(*files, save, backup, &message) ==
                    sync::OperationError::kUnverified,
                "one of the same size that is not stands it down: " + message);

  std::error_code error;
  std::filesystem::remove(sandbox.Host(save), error);
  checks.Expect(sync::UnchangedSinceBackup(*files, save, backup, &message) ==
                    sync::OperationError::kUnverified,
                "and so does one that is gone since: " + message);

  sandbox.Write(save, "the bytes backed up\n");
  checks.Expect(sync::UnchangedSinceBackup(*files, save, backup + ".missing", &message) ==
                    sync::OperationError::kUnreadableCard,
                "a backup that cannot be read back is the card's failure: " + message);
}

// --- upload -------------------------------------------------------------------
//
// An upload that respects RomM's slot guard: `overwrite=false`, `session_id`
// and `device_id`. Into a fresh slot the guard has nothing to say, so the plan
// executes as issued.

void Upload(rig::Checks& checks, http::HttpClient& client, const std::string& base,
            const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-upload");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-5-upload");
  const std::string name = "upload.srm";
  const std::string bytes = "the device's only copy\n";
  sandbox.SeedSave(SavePath(name), bytes);

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(rom.id, name, slot, "m2-5", crypto::Md5Hex(bytes),
                                             std::chrono::system_clock::now(),
                                             static_cast<std::int64_t>(bytes.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    return;
  }
  checks.Expect(plan.operations[0].action == sync::Action::kUpload,
                std::string("a save only the client has is an upload: ") +
                    sync::ToString(plan.operations[0].action));
  checks.Expect(!plan.operations[0].save_id.has_value(),
                "with no save_id, because the server has nothing yet");

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'000));

  checks.ExpectEq(report.completed, 1, "the upload completed");
  checks.ExpectEq(report.failed, 0,
                  "and nothing failed: " + (report.warnings.empty() ? "" : report.warnings[0]));
  if (report.operations.empty()) {
    return;
  }
  const sync::OperationResult& done = report.operations[0];
  checks.Expect(done.outcome == sync::OperationOutcome::kUploaded,
                std::string("the operation is an upload: ") + sync::ToString(done.outcome));
  checks.Expect(done.error != sync::OperationError::kRefused,
                "a plan executed as issued draws no 409: " + done.message);
  checks.ExpectEq(sandbox.Read(SavePath(name)), bytes, "an upload does not touch the local save");
  checks.Expect(done.backup_sd_path.empty(), "and backs nothing up -- it destroys nothing");

  if (!done.save_id.has_value()) {
    checks.Expect(false, "the upload reports the save row it created: " + done.message);
    return;
  }
  // The row `device_id` on the upload wrote. It is what the *next* negotiation
  // arbitrates against, so an upload that skipped it leaves this device in the
  // no-history branch forever.
  ExpectDeviceSynced(checks, client, base, fixture, rom.id, slot, *done.save_id,
                     "the upload wrote a device_syncs row for this device");

  harness::DeleteSave(client, base, fixture, *done.save_id);
}

// --- download -----------------------------------------------------------------
//
// The overwrite path, and the one docs/SYNC_PROTOCOL.md's hard rule is about.
// The sandbox's teardown audit judges it independently of everything asserted
// here: the seeded save's bytes changed, so its previous bytes had better be
// under `.backup/`.

void Download(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-download");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-5-download");
  const std::string name = "download.srm";
  const std::string previous = "the local copy, one tick old\n";
  const std::string server_bytes = "the server's newer copy, which wins\n";

  sandbox.SeedSave(SavePath(name), previous);

  // No `device_id` on the upload: this device has never synced this save, which
  // is what puts the comparison in the timestamp branch.
  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, server_bytes);
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m2-5", staged, name,
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "the server copy was stored");
    return;
  }

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(
      rom.id, name, slot, "m2-5", crypto::Md5Hex(previous),
      std::chrono::system_clock::now() - std::chrono::hours{1},
      static_cast<std::int64_t>(previous.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  checks.Expect(plan.operations[0].action == sync::Action::kDownload,
                std::string("the server's newer copy is a download: ") +
                    sync::ToString(plan.operations[0].action));

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'001));

  checks.ExpectEq(report.completed, 1,
                  "the download completed" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
  checks.ExpectEq(report.failed, 0, "and nothing failed");
  if (report.operations.empty()) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  const sync::OperationResult& done = report.operations[0];
  checks.ExpectEq(sandbox.Read(SavePath(name)), server_bytes, "the save holds the server's bytes");
  checks.Expect(!done.backup_sd_path.empty(), "a backup was written: " + done.message);
  checks.ExpectEq(sandbox.Read(done.backup_sd_path), previous,
                  "and it holds the bytes the download replaced");
  // The server's name carries RomM's ingest tag; writing it to the card would
  // produce a file no emulator loads. The save is at the client's own path.
  checks.Expect(server.file_name != name,
                "the server renamed the save on ingest: " + server.file_name);
  checks.Expect(!sandbox.Exists(SavePath(server.file_name)),
                "and nothing was written under the server's name");
  checks.Expect(!sandbox.Exists(io::TempPathFor(SavePath(name))),
                "the staged copy is consumed, not left beside the save");

  ExpectDeviceSynced(checks, client, base, fixture, rom.id, slot, server.id,
                     "POST /api/saves/{id}/downloaded recorded that this device holds it");

  // A save this client has no local file for -- the `Save exists on server but
  // not on client` case, and the only way a client ever learns a save exists.
  // The plan cannot say where it goes (the server's `file_name` carries the
  // ingest tag and the directory depends on the rom's platform), so the policy
  // is injected. Here the test plays the part M3-1's folder map will.
  {
    const std::string fresh = "placed.srm";
    sync::ExecuteOptions placing = OptionsAt(1'757'000'008);
    placing.place = [&fresh](const sync::SyncOperation&) {
      place::Placement placement;
      placement.sd_path = SavePath(fresh);
      return placement;
    };
    const sync::ExecutionReport placed = sync::ExecutePlan(
        client, *files, TokenFor(base, fixture), plan, /*targets=*/{}, placing);
    checks.ExpectEq(placed.completed, 1,
                    "a save with no local file lands where `place` said" +
                        (placed.warnings.empty() ? std::string() : ": " + placed.warnings[0]));
    checks.ExpectEq(sandbox.Read(SavePath(fresh)), server_bytes, "with the server's bytes");
    if (!placed.operations.empty()) {
      checks.Expect(placed.operations[0].backup_sd_path.empty(),
                    "and no backup, because it replaced nothing");
    }
    // Without the policy the same operation refuses rather than guessing at a
    // path -- writing the server's tagged name would produce a file no emulator
    // loads, and the next tick would upload it back as a second save.
    const sync::ExecutionReport guessing = sync::ExecutePlan(
        client, *files, TokenFor(base, fixture), plan, /*targets=*/{}, OptionsAt(1'757'000'008));
    checks.Expect(guessing.declined == 1 && guessing.failed == 0,
                  "with no policy it is declined, by name, rather than guessed at (#257)");
    if (!guessing.operations.empty()) {
      checks.Expect(guessing.operations[0].error == sync::OperationError::kNoLocalSave,
                    std::string("...named: ") + sync::ToString(guessing.operations[0].error));
    }
    checks.Expect(!sandbox.Exists(SavePath(server.file_name)),
                  "and nothing was written under the server's own name");
  }

  // And the same download again with nowhere to put the backup. `core/` cannot
  // create a directory with only standard headers -- that is the platform
  // layer's job, the rule `io::WriteAtomically` already states -- so a missing
  // `.backup/` has to stop the overwrite rather than proceed without a copy.
  // On a first boot that directory is exactly what does not exist yet.
  sync::ExecuteOptions nowhere = OptionsAt(1'757'000'009);
  nowhere.backup_dir = "/config/rommsync/.backup-that-was-never-created";
  const sync::ExecutionReport refused =
      sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, targets, nowhere);
  checks.ExpectEq(refused.failed, 1, "a download with nowhere to back up is a failed operation");
  if (!refused.operations.empty()) {
    checks.Expect(refused.operations[0].error == sync::OperationError::kBackupFailed,
                  std::string("...named as the backup, not the transfer: ") +
                      sync::ToString(refused.operations[0].error) + " -- " +
                      refused.operations[0].message);
  }
  checks.ExpectEq(sandbox.Read(SavePath(name)), server_bytes,
                  "and the save is left exactly as it was: no backup, no overwrite");

  harness::DeleteSave(client, base, fixture, server.id);
}

// --- late_fetch ---------------------------------------------------------------
//
// #263, for the two actions that write over a save: a `download` and a
// `conflict`, each with the game writing the save between its backup and the
// commit. The conflict's window is the wider of the two -- its archival upload
// (#240) sits inside it -- and the write is put at its start.

/// One late write against one plan: the server's copy must not land, and the
/// write must still be the save. Returns the operation's result, if there was one.
std::optional<sync::OperationResult> ExecuteUnderLateWrite(
    rig::Checks& checks, Sandbox& sandbox, http::HttpClient& client, const std::string& base,
    const Fixture& fixture, const sync::SyncPlan& plan, const sync::SaveTarget& target,
    const sync::ExecuteOptions& options, const std::string& late, const std::string& what) {
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string& save = target.sd_path;
  sync::ExecutionReport report;
  {
    faulty::LateWrite game([&] { sandbox.Reseed(save, late); });
    report = sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, {target}, options);
    checks.Expect(game.fired(), what + ": the game wrote between the backup and the commit");
  }
  checks.ExpectEq(sandbox.Read(save), late,
                  what + ": the write that landed after the backup is still the save");
  checks.ExpectEq(report.failed, 1, what + ": counted failed, not done");
  checks.Expect(!sandbox.Exists(io::TempPathFor(save)),
                what + ": and the server's copy is not left staged beside it");
  if (report.operations.size() != 1) {
    return std::nullopt;
  }
  const sync::OperationResult& done = report.operations[0];
  checks.Expect(done.error == sync::OperationError::kUnverified,
                what + ": named as a save that moved since its backup: " +
                    sync::ToString(done.error) + " -- " + done.message);
  return done;
}

void LateWriteFetch(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                    const Fixture& fixture, const harness::Rom& rom) {
  const std::string late = "progress the game wrote after the backup\n";

  // 1. download.
  {
    Sandbox sandbox(checks, "execute-late-download");
    const std::string slot = harness::UniqueSlot("m9-26-download");
    const std::string name = "late-download.srm";
    const std::string previous = "the local copy, one tick old\n";
    sandbox.SeedSave(SavePath(name), previous);

    const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
    rig::WriteFile(staged, "the server's newer copy, which would replace it\n");
    harness::Save server;
    if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m9-26", staged, name,
                             /*with_device=*/false, &server)) {
      checks.Expect(false, "the server copy was stored");
      return;
    }
    sync::SyncNegotiatePayload payload;
    payload.device_id = fixture.device_id;
    payload.saves.push_back(harness::LocalSave(
        rom.id, name, slot, "m9-26", crypto::Md5Hex(previous),
        std::chrono::system_clock::now() - std::chrono::hours{1},
        static_cast<std::int64_t>(previous.size())));
    sync::SyncPlan plan;
    if (PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
      checks.Expect(plan.operations[0].action == sync::Action::kDownload,
                    std::string("the plan is a download: ") +
                        sync::ToString(plan.operations[0].action));
      const auto done = ExecuteUnderLateWrite(checks, sandbox, client, base, fixture, plan,
                                              {rom.id, slot, SavePath(name), name},
                                              OptionsAt(1'757'000'710), late, "download");
      if (done.has_value()) {
        checks.Expect(!done->backup_sd_path.empty() &&
                          sandbox.Read(done->backup_sd_path) == previous,
                      "download: the backup is left, holding the bytes it was taken of");
      }
    }
    harness::DeleteSave(client, base, fixture, server.id);
  }

  // 2. conflict, keep both.
  {
    Sandbox sandbox(checks, "execute-late-conflict");
    const std::string slot = harness::UniqueSlot("m9-26-conflict");
    const std::string name = "late-conflict.srm";
    const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
    rig::WriteFile(staged, "server v1\n");
    harness::Save server;
    if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m9-26", staged, name,
                             /*with_device=*/true, &server)) {
      checks.Expect(false, "the first server copy was stored");
      return;
    }
    PassASecond();
    rig::WriteFile(staged, "server v2, longer\n");
    harness::Save moved;
    if (!harness::ReplaceSave(client, base, fixture, server.id, staged, name, &moved)) {
      checks.Expect(false, "the server copy moved forward in place");
      harness::DeleteSave(client, base, fixture, server.id);
      return;
    }
    const std::string previous = "device v2\n";
    sandbox.SeedSave(SavePath(name), previous);

    sync::SyncNegotiatePayload payload;
    payload.device_id = fixture.device_id;
    payload.saves.push_back(harness::LocalSave(
        rom.id, name, slot, "m9-26", crypto::Md5Hex(previous),
        std::chrono::system_clock::now() + std::chrono::seconds{300},
        static_cast<std::int64_t>(previous.size())));
    sync::SyncPlan plan;
    if (PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
      checks.Expect(plan.operations[0].action == sync::Action::kConflict,
                    std::string("the plan is a conflict: ") +
                        sync::ToString(plan.operations[0].action));
      sync::ExecuteOptions options = OptionsAt(1'757'000'711);
      options.reported = &payload.saves;
      const auto done = ExecuteUnderLateWrite(checks, sandbox, client, base, fixture, plan,
                                              {rom.id, slot, SavePath(name), name}, options,
                                              late, "conflict");
      if (done.has_value()) {
        checks.Expect(!done->backup_sd_path.empty() &&
                          sandbox.Read(done->backup_sd_path) == previous,
                      "conflict: the backup is left, holding the bytes it was taken of");
      }
    }
    // The archival copy the conflict made before the write was noticed goes with
    // `main`'s `DeleteArchivalCopies`.
    harness::DeleteSave(client, base, fixture, server.id);
  }
}

// --- crash --------------------------------------------------------------------
//
// M9-14 (#203): `download`, with a power cut at every call it makes. The
// overwrite's bytes arrive through the HTTP backend's `.part` file, so this is
// the one place the backend's own durability is on the line: a staged file
// renamed onto the save before its bytes were synced is a save that can come
// back empty, backup or no backup.

void Crash(rig::Checks& checks, http::HttpClient& client, const std::string& base,
           const Fixture& fixture, const harness::Rom& rom) {
  const std::string slot = harness::UniqueSlot("m9-14-crash");
  const std::string name = "crash.srm";
  const std::string previous = "the local copy a power cut must not cost\n";
  const std::string server_bytes = "the server's newer copy, which replaces it\n";
  const crash::Overwrite expected{SavePath(name), previous, server_bytes, harness::kBackupDir};

  harness::Save server;
  {
    Sandbox upload(checks, "execute-crash-upload");
    const std::string staged = upload.Host("/config/rommsync/server-copy.srm");
    rig::WriteFile(staged, server_bytes);
    if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m9-14", staged, name,
                             /*with_device=*/false, &server)) {
      checks.Expect(false, "the server copy was stored");
      return;
    }
  }

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(
      rom.id, name, slot, "m9-14", crypto::Md5Hex(previous),
      std::chrono::system_clock::now() - std::chrono::hours{1},
      static_cast<std::int64_t>(previous.size())));
  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};

  // The download as it runs every tick.
  {
    Sandbox sandbox(checks, "execute-crash");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(SavePath(name), previous);
    crash::Recorder recorder(sandbox.root());
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'201));
    checks.ExpectEq(report.completed, 1,
                    "the download completed" +
                        (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
    checks.ExpectEq(sandbox.Read(SavePath(name)), server_bytes, "the save holds the server's bytes");
    crash::ExpectSurvives(checks, recorder, expected, rig::ScratchDir() + "/crash-download",
                          "a download over a save");
  }

  // ...and with the card refusing to confirm `.backup/`. The overwrite must
  // not happen, because the backup it would depend on may not survive it.
  {
    Sandbox sandbox(checks, "execute-crash-unconfirmed");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sandbox.SeedSave(SavePath(name), previous);
    crash::Recorder recorder(sandbox.root(), {harness::kBackupDir});
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'202));
    checks.Expect(recorder.refused() > 0, "the card refused to confirm the backup directory");
    checks.ExpectEq(report.failed, 1, "the download is a failed operation");
    if (!report.operations.empty()) {
      checks.Expect(report.operations[0].error == sync::OperationError::kBackupFailed,
                    std::string("...named as the backup: ") +
                        sync::ToString(report.operations[0].error) + " -- " +
                        report.operations[0].message);
    }
    checks.ExpectEq(sandbox.Read(SavePath(name)), previous, "and the save is untouched");
    crash::ExpectSurvives(checks, recorder, expected, rig::ScratchDir() + "/crash-download",
                          "a download whose backup the card would not confirm");
  }

  harness::DeleteSave(client, base, fixture, server.id);
}

// --- placed -------------------------------------------------------------------
//
// #237: a save the console has never held, written where `place` says. A
// placement creates: it makes the folder an emulator has not made yet, never
// writes over a file the scan did not pair with the row -- and a power cut at
// any call leaves no file or the whole save, never a part of one.

void Placed(rig::Checks& checks, http::HttpClient& client, const std::string& base,
            const Fixture& fixture, const harness::Rom& rom) {
  const std::string slot = harness::UniqueSlot("m10-11-placed");
  const std::string server_bytes = "a save this console has never held\n";

  harness::Save server;
  {
    Sandbox upload(checks, "execute-placed-upload");
    const std::string staged = upload.Host("/config/rommsync/server-copy.srm");
    rig::WriteFile(staged, server_bytes);
    if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m10-11", staged, "placed.srm",
                             /*with_device=*/false, &server)) {
      checks.Expect(false, "the server copy was stored");
      return;
    }
  }
  // The device reports nothing for the slot: the server's copy is a download
  // with no local file to match.
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  const auto placing = [](std::string sd_path, std::int64_t at) {
    sync::ExecuteOptions options = OptionsAt(at);
    options.place = [sd_path](const sync::SyncOperation&) {
      place::Placement placement;
      placement.sd_path = sd_path;
      return placement;
    };
    return options;
  };

  // Into a folder nothing has made yet: Tico's account folder for a system
  // never played on this console.
  {
    Sandbox sandbox(checks, "execute-placed-folder");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    const std::string target = "/tico/saves/users/0123456789abcdef0123456789abcdef/gb/gb240p.srm";
    sandbox.MakeDirs("/tico/saves/users/0123456789abcdef0123456789abcdef");
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, TokenFor(base, fixture), plan, /*targets=*/{},
        placing(target, 1'757'000'301));
    checks.ExpectEq(report.completed, 1,
                    "the placement completed" +
                        (report.warnings.empty() ? std::string() : ": " + report.warnings[0]) +
                        (report.operations.empty() ? std::string()
                                                   : " / " + report.operations[0].message));
    checks.ExpectEq(sandbox.Read(target), server_bytes, "into the folder it made for it");
  }

  // #258: a fetch that fails after the folder was made takes the folder away
  // again -- every one it made, and none it did not -- rather than leaving an
  // empty system folder behind for an emulator to find.
  {
    Sandbox sandbox(checks, "execute-placed-unmade");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    const std::string account = "/tico/saves/users/0123456789abcdef0123456789abcdef";
    sandbox.MakeDirs(account);
    sync::SyncPlan wrong = plan;
    if (!wrong.operations.empty()) {
      wrong.operations[0].server_content_hash = std::string(sync::kContentHashDigits, '0');
    }
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, TokenFor(base, fixture), wrong, /*targets=*/{},
        placing(account + "/gb/deeper/gb240p.srm", 1'757'000'305));
    checks.ExpectEq(report.failed, 1, "bytes that are not the plan's fail the placement");
    checks.Expect(!sandbox.Exists(account + "/gb"),
                  "and the folders it made for them are gone again");
    checks.Expect(sandbox.Exists(account), "but not the one that was there before");
  }

  // Onto a file the scan did not pair with this row: someone else's save.
  {
    Sandbox sandbox(checks, "execute-placed-taken");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    const std::string target = SavePath("taken.srm");
    const std::string theirs = "a save the scan skipped, or one in another slot\n";
    sandbox.SeedSave(target, theirs);
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, TokenFor(base, fixture), plan, /*targets=*/{},
        placing(target, 1'757'000'302));
    checks.Expect(report.declined == 1 && report.failed == 0,
                  "a placement onto a file is declined, not failed (#257)");
    if (!report.operations.empty()) {
      checks.Expect(report.operations[0].error == sync::OperationError::kNoLocalSave,
                    std::string("...named as having nowhere to go: ") +
                        sync::ToString(report.operations[0].error) + " -- " +
                        report.operations[0].message);
      checks.Expect(report.operations[0].backup_sd_path.empty(),
                    "nothing was backed up, because nothing was about to be replaced");
    }
    checks.ExpectEq(sandbox.Read(target), theirs, "and the file is exactly as it was");
    checks.Expect(!sandbox.Exists(io::TempPathFor(target)), "with nothing staged beside it");
  }

  // `place` refusing, with its reason carried into the result.
  {
    Sandbox sandbox(checks, "execute-placed-refused");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    sync::ExecuteOptions options = OptionsAt(1'757'000'303);
    options.place = [](const sync::SyncOperation&) {
      place::Placement placement;
      placement.reason = "Tico keeps saves for more than one account here";
      return placement;
    };
    const sync::ExecutionReport report =
        sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, /*targets=*/{}, options);
    checks.Expect(report.declined == 1 && report.failed == 0,
                  "a refused placement is declined, not failed (#257)");
    if (!report.operations.empty()) {
      checks.Expect(report.operations[0].error == sync::OperationError::kNoLocalSave &&
                        report.operations[0].message.find("more than one account") !=
                            std::string::npos,
                    "whose message says why: " + report.operations[0].message);
    }
  }

  // A power cut at every call of a placement.
  {
    Sandbox sandbox(checks, "execute-placed-crash");
    const auto files = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    const std::string target = "/tico/saves/gb/gb240p.srm";
    sandbox.MakeDirs("/tico/saves/gb");
    sandbox.Write("/tico/saves/gb/other.srm", "another game's save, beside it\n");
    crash::Recorder recorder(sandbox.root());
    const sync::ExecutionReport report = sync::ExecutePlan(
        client, *files, TokenFor(base, fixture), plan, /*targets=*/{},
        placing(target, 1'757'000'304));
    checks.ExpectEq(report.completed, 1, "the placement completed");
    checks.ExpectEq(sandbox.Read(target), server_bytes, "the save holds the server's bytes");
    crash::ExpectCreated(checks, recorder, crash::Created{target, server_bytes},
                         rig::ScratchDir() + "/crash-placed", "a placement");
  }

  harness::DeleteSave(client, base, fixture, server.id);
}

// --- conflict -----------------------------------------------------------------
//
// RomM sends no resolution: there is no `server_wins` / `keep_both` field to
// obey (docs/SYNC_PROTOCOL.md#conflicts). The client picks, and the safe policy
// is to lose nothing -- the server's copy goes on the card and the local bytes
// stay under `.backup/` for the overlay (M7-1).
//
// This arranges the first of the two reasons: a sync record, and both sides
// moved past it. `same_timestamp` arranges the other, which a client that
// switched on this reason alone would drop into its default branch.

void Conflict(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-conflict");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-5-conflict");
  const std::string name = "conflict.srm";

  // The server's first copy, with `device_id`, so RomM writes the sync row both
  // sides are then compared against.
  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, "server v1\n");
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m2-5", staged, name,
                           /*with_device=*/true, &server)) {
    checks.Expect(false, "the first server copy was stored");
    return;
  }

  PassASecond();

  // The server's copy moves forward *in place*. A second POST would be a new
  // save row with no history, which is the other conflict entirely
  // (docs/SYNC_PROTOCOL.md step 2).
  const std::string server_bytes = "server v2, longer\n";
  rig::WriteFile(staged, server_bytes);
  harness::Save moved;
  if (!harness::ReplaceSave(client, base, fixture, server.id, staged, name, &moved)) {
    checks.Expect(false, "the server copy moved forward in place");
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }

  // And so did this device's, to different bytes.
  const std::string previous = "device v2\n";
  sandbox.SeedSave(SavePath(name), previous);

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(
      rom.id, name, slot, "m2-5", crypto::Md5Hex(previous),
      std::chrono::system_clock::now() + std::chrono::seconds{300},
      static_cast<std::int64_t>(previous.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  checks.Expect(plan.operations[0].action == sync::Action::kConflict,
                std::string("both sides moved, so the server refuses to choose: ") +
                    sync::ToString(plan.operations[0].action));
  checks.Expect(plan.operations[0].reason == sync::Reason::kBothChanged,
                std::string("...with the history reason: ") + plan.operations[0].reason_text);

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  // What step 0 reported is where the archive's emulator comes from (#240), as
  // `RunTick` sets it.
  sync::ExecuteOptions options = OptionsAt(1'757'000'002);
  options.reported = &payload.saves;
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, options);

  checks.ExpectEq(report.completed, 1,
                  "the conflict resolved" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
  if (report.operations.empty()) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  const sync::OperationResult& done = report.operations[0];
  checks.Expect(done.outcome == sync::OperationOutcome::kKeptBoth,
                std::string("keep both, which is the only policy that loses nothing: ") +
                    sync::ToString(done.outcome));
  checks.ExpectEq(sandbox.Read(SavePath(name)), server_bytes, "the server's copy is on the card");
  checks.Expect(!done.backup_sd_path.empty(), "and the local copy was kept: " + done.message);
  checks.ExpectEq(sandbox.Read(done.backup_sd_path), previous,
                  "...as the bytes this device had, recoverable by the overlay");

  // #240: and not on this card alone. The losing bytes are on the server as an
  // archival row -- no slot, so negotiate never pairs it -- under the local
  // emulator and the conflict's own name.
  checks.Expect(done.archive == sync::ArchiveOutcome::kKept,
                std::string("the result says the local copy is on the server: ") +
                    sync::ToString(done.archive));
  // Found by the id the result names: an earlier run's copy of the same bytes
  // may still be on the fixture.
  harness::ArchivalRow archived;
  for (const harness::ArchivalRow& row : harness::ArchivalRows(client, base, fixture, rom.id)) {
    if (done.archive_save_id == row.id) {
      archived = row;
    }
  }
  checks.Expect(archived.id != 0, "the server holds the null-slot row the result names");
  checks.ExpectEq(archived.content_hash, crypto::Md5Hex(previous), "holding the local bytes");
  checks.ExpectEq(archived.emulator, std::string("m2-5"), "under the local copy's emulator");
  checks.ExpectEq(archived.file_name, sync::ArchiveFileName(name, 1'757'000'002),
                  "under the conflict's archival name, untagged");
  // The server's copy is still the slot's head: the archive did not take part.
  checks.ExpectEq(sandbox.Read(SavePath(name)), server_bytes,
                  "the server copy still won on the card");

  harness::DeleteSave(client, base, fixture, archived.id);
  harness::DeleteSave(client, base, fixture, server.id);
}

// --- conflict_unarchived ----------------------------------------------------------
//
// #240's other half: the archival upload is refused. The conflict is resolved
// all the same -- the server copy on the card, the local bytes in `.backup/` --
// and the result says the server holds no copy, which is what the overlay shows.

void ConflictUnarchived(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                        const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-conflict-unarchived");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m10-14-unarchived");
  const std::string name = "unarchived.srm";

  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  const std::string server_bytes = "server copy " + slot + "\n";
  rig::WriteFile(staged, server_bytes);
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m10-14", staged, name,
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "the server copy was stored");
    return;
  }
  sync::Timestamp when;
  if (!harness::ParseServerTimestamp(server.updated_at, &when)) {
    checks.Expect(false, "the server's updated_at parses: " + server.updated_at);
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  const std::string previous = "device copy " + slot + "\n";
  sandbox.SeedSave(SavePath(name), previous);

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(rom.id, name, slot, "m10-14",
                                             crypto::Md5Hex(previous), when,
                                             static_cast<std::int64_t>(previous.size())));
  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  checks.Expect(plan.operations[0].action == sync::Action::kConflict,
                std::string("the scenario is a conflict: ") +
                    sync::ToString(plan.operations[0].action));

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  sync::ExecutionReport report;
  {
    // The only `POST /api/saves?...` a conflict sends is the archival upload:
    // the download is `/api/saves/{id}/content` and its confirmation
    // `/api/saves/{id}/downloaded`.
    harness::Fault fault(checks, client, base,
                         R"({"mode":"status","status":503,"path":"/api/saves?"})");
    report = sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, targets,
                               OptionsAt(1'757'000'005));
  }

  checks.ExpectEq(report.completed, 1,
                  "the conflict is resolved without the archive" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
  if (report.operations.empty()) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  const sync::OperationResult& done = report.operations[0];
  checks.Expect(done.outcome == sync::OperationOutcome::kKeptBoth,
                std::string("keep both still: ") + sync::ToString(done.outcome));
  checks.ExpectEq(sandbox.Read(SavePath(name)), server_bytes, "the server's copy is on the card");
  checks.Expect(!done.backup_sd_path.empty() && sandbox.Read(done.backup_sd_path) == previous,
                "and the local bytes are in .backup/: " + done.message);
  checks.Expect(done.archive == sync::ArchiveOutcome::kFailed,
                std::string("the result says the server holds no copy: ") +
                    sync::ToString(done.archive));
  checks.Expect(!done.archive_save_id.has_value(), "and names no row");
  checks.Expect(std::any_of(report.warnings.begin(), report.warnings.end(),
                            [](const std::string& line) {
                              return line.find("only in") != std::string::npos;
                            }),
                "a warning says the bytes are only on this card");
  const harness::ArchivalRow archived =
      harness::ArchivalWith(client, base, fixture, rom.id, crypto::Md5Hex(previous));
  checks.Expect(archived.id == 0, "and the server really holds none");

  harness::DeleteSave(client, base, fixture, archived.id);
  harness::DeleteSave(client, base, fixture, server.id);
}

// --- same_timestamp -----------------------------------------------------------
//
// The other conflict: no sync history, equal timestamps, different hashes. Same
// action, different situation, and the client must resolve it the same way
// rather than letting it reach a default branch.

void SameTimestamp(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                   const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-same-timestamp");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-5-same-ts");
  const std::string name = "same-timestamp.srm";
  const std::string server_bytes = "server copy\n";
  const std::string previous = "device copy\n";

  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, server_bytes);
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m2-5", staged, name,
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "the server copy was stored");
    return;
  }

  sync::Timestamp when;
  if (!harness::ParseServerTimestamp(server.updated_at, &when)) {
    checks.Expect(false, "the server's updated_at parses: " + server.updated_at);
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }

  sandbox.SeedSave(SavePath(name), previous);

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(rom.id, name, slot, "m2-5",
                                             crypto::Md5Hex(previous), when,
                                             static_cast<std::int64_t>(previous.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  checks.Expect(plan.operations[0].reason == sync::Reason::kSameTimestampDifferentContent,
                std::string("the no-history conflict: ") + plan.operations[0].reason_text);

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'003));

  checks.ExpectEq(report.completed, 1,
                  "the second conflict reason resolves too" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
  if (report.operations.empty()) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  const sync::OperationResult& done = report.operations[0];
  checks.Expect(done.outcome == sync::OperationOutcome::kKeptBoth,
                std::string("...keep both, exactly as the other one does: ") +
                    sync::ToString(done.outcome));
  checks.ExpectEq(sandbox.Read(SavePath(name)), server_bytes, "the server's copy is on the card");
  checks.Expect(!done.backup_sd_path.empty() && sandbox.Read(done.backup_sd_path) == previous,
                "and the local bytes are recoverable from .backup/: " + done.message);

  harness::DeleteSave(client, base, fixture, server.id);
}

// --- truncate -----------------------------------------------------------------
//
// A body that ends early and cleanly, which no transport can fault: the proxy
// sends no `Content-Length` at all in this mode, so only the size the client
// asked for can catch it. A save must never be replaced by one.

void Truncate(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-truncate");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-5-truncate");
  const std::string name = "truncate.srm";
  const std::string previous = "the only copy of this save, and it must survive\n";

  sandbox.SeedSave(SavePath(name), previous);

  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, "the server's copy, which never arrives whole\n");
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m2-5", staged, name,
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "the server copy was stored");
    return;
  }

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(
      rom.id, name, slot, "m2-5", crypto::Md5Hex(previous),
      std::chrono::system_clock::now() - std::chrono::hours{1},
      static_cast<std::int64_t>(previous.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  sync::ExecutionReport report;
  {
    // Armed on the content path only. The preflight `GET /api/saves/{id}` is a
    // *shorter* path, so it does not match this prefix and answers with the real
    // size -- which is what turns a clean short body into a named failure.
    harness::Fault fault(checks, client, base,
                         R"({"mode":"truncate","bytes":8,"path":")" + server.ContentPath() +
                             R"("})");
    report = sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, targets,
                               OptionsAt(1'757'000'004));
  }

  checks.ExpectEq(report.failed, 1, "the truncated download is counted failed");
  checks.ExpectEq(report.completed, 0, "and not as work done");
  checks.ExpectEq(sandbox.Read(SavePath(name)), previous, "the local save is untouched");
  if (!report.operations.empty()) {
    const sync::OperationResult& done = report.operations[0];
    // Specifically the transfer, not the digest. Both would refuse this body,
    // and asserting the first pins the mechanism the issue asks for: the
    // preflight supplied a size, so the short body was caught as it arrived
    // rather than after being written and hashed.
    checks.Expect(done.error == sync::OperationError::kTransferFailed,
                  std::string("...caught by the expected size, as it arrived: ") +
                      sync::ToString(done.error) + " -- " + done.message);
    checks.Expect(done.backup_sd_path.empty(),
                  "and nothing was backed up, because nothing was going to be overwritten");
  }
  // Not "no backup was needed" -- no backup exists at all. A backup written for
  // an overwrite that never happened is a file the next tick would have to
  // reason about, and `sync::RecoverStaging` deliberately never touches one.
  checks.Expect(!sandbox.HasBackupOf(previous), "there is no stray backup under .backup/");

  harness::DeleteSave(client, base, fixture, server.id);
}

// --- corrupted ----------------------------------------------------------------
//
// The other half of "verify before anything replaces a save": a body that is
// the right *length* and the wrong bytes. `execute.truncate` proves the size
// check refuses a short body; only this one proves the digest refuses a
// complete one, and the digest is the check that survives a proxy, a cache or a
// server serving the wrong row.

void Corrupted(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-corrupted");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-5-corrupted");
  const std::string name = "corrupted.srm";
  const std::string previous = "the only copy of this save, and it must survive\n";
  const std::string server_bytes = "the server's copy, the honest bytes\n";
  // Byte for byte the same length, so the size check cannot be what refuses it.
  const std::string impostor = std::string(server_bytes.size(), 'X');

  sandbox.SeedSave(SavePath(name), previous);

  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, server_bytes);
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m2-5", staged, name,
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "the server copy was stored");
    return;
  }

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(
      rom.id, name, slot, "m2-5", crypto::Md5Hex(previous),
      std::chrono::system_clock::now() - std::chrono::hours{1},
      static_cast<std::int64_t>(previous.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  checks.Expect(plan.operations[0].server_content_hash.has_value(),
                "the plan carries the digest the download is judged against");

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  sync::ExecutionReport report;
  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"status","status":200,"body":")" + impostor +
                             R"(","path":")" + server.ContentPath() + R"("})");
    report = sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, targets,
                               OptionsAt(1'757'000'010));
  }

  checks.ExpectEq(report.failed, 1, "bytes that are not the save are refused");
  checks.ExpectEq(report.completed, 0, "and not counted as work done");
  if (!report.operations.empty()) {
    checks.Expect(report.operations[0].error == sync::OperationError::kUnverified,
                  std::string("...by the digest, which is the only thing that could: ") +
                      sync::ToString(report.operations[0].error) + " -- " +
                      report.operations[0].message);
  }
  checks.ExpectEq(sandbox.Read(SavePath(name)), previous, "the local save is untouched");
  checks.Expect(!sandbox.HasBackupOf(previous),
                "and nothing was backed up: a refusal is not an overwrite");
  checks.Expect(!sandbox.Exists(io::TempPathFor(SavePath(name))),
                "the rejected bytes are gone, not left staged beside the save");

  harness::DeleteSave(client, base, fixture, server.id);
}

// --- occupied -------------------------------------------------------------------
//
// The state RomM's slot guard exists for: the server holds a save for this
// `(rom_id, slot)` that this device has no sync row for -- another device's. That
// is `upload` / `Client save is newer (no sync history)`, and since #232 the
// executor sends `overwrite=false`, so the guard answers 409 and **nothing is
// uploaded over the other device's save**: the operation is named
// `kNewerOnServer` and left to the tick, which resolves it (sync_resolve.hpp,
// `tick.newer_*`). Only an explicit "Keep local" sends `overwrite=true`, and the
// second half of the scenario is that override, in the one state where it does
// anything.
//
// It also pins what the flag does *not* do. It does not stop a slot accreting a
// row per upload: RomM 5.4.0 reserves a fresh name for every slotted upload, and
// `overwrite=true` even turns off the promotion that would otherwise answer
// identical bytes with the row already holding them (docs/API_CONTRACT.md).

void Occupied(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-occupied");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-5-occupied");
  const std::string name = "occupied.srm";
  const std::string bytes = "the bytes this device wants to send\n";

  // Somebody else's upload: no `device_id`, so this device has no sync row for
  // the save now sitting in the slot.
  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, "the copy another device left in the slot\n");
  harness::Save occupant;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m2-5", staged, name,
                           /*with_device=*/false, &occupant)) {
    checks.Expect(false, "the slot was occupied");
    return;
  }

  sandbox.SeedSave(SavePath(name), bytes);
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(
      rom.id, name, slot, "m2-5", crypto::Md5Hex(bytes),
      std::chrono::system_clock::now() + std::chrono::seconds{300},
      static_cast<std::int64_t>(bytes.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, occupant.id);
    return;
  }
  checks.Expect(plan.operations[0].action == sync::Action::kUpload,
                std::string("the client's newer save is an upload: ") +
                    sync::ToString(plan.operations[0].action));
  checks.Expect(plan.operations[0].reason == sync::Reason::kClientNewerNoHistory,
                std::string("...with the reason the slot guard is about: ") +
                    plan.operations[0].reason_text);

  // The plan executed as issued meets the guard. Nothing goes up over the other
  // device's save, and nothing on the card changes.
  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  {
    const sync::ExecutionReport guarded = sync::ExecutePlan(
        client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'010));
    checks.ExpectEq(guarded.failed, 1, "the guard refuses the upload the plan asked for");
    if (!guarded.operations.empty()) {
      const sync::OperationResult& refused = guarded.operations[0];
      checks.Expect(refused.error == sync::OperationError::kNewerOnServer,
                    std::string("...as a newer save in the slot: ") +
                        sync::ToString(refused.error) + " -- " + refused.message);
      checks.Expect(!refused.save_id.has_value(), "and no row was written");
    }
    checks.ExpectEq(sandbox.Read(SavePath(name)), bytes, "the local save is untouched");
  }

  // "Keep local": the player's override, and the only thing that sends the flag.
  //
  // A second has to pass first, or the row it writes is not the slot's head.
  // Negotiate takes the newest row per slot by `updated_at`, which 5.4.0 stores
  // to the whole second, and a tie goes to whichever row its unordered query
  // returns first -- the occupant. Arbitrating against the occupant, this device
  // has no history, and the sync-history check below would fail for a reason
  // that has nothing to do with whether the upload recorded one (#265). The
  // executor now settles that tie itself (#272, `tie` below); the wait keeps
  // this scenario about the guard rather than about the tie.
  harness::PassASecond();
  sync::ExecuteOptions keep = OptionsAt(1'757'000'011);
  keep.keep_local = {{rom.id, slot}};
  const sync::ExecutionReport report =
      sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, targets, keep);

  checks.ExpectEq(report.completed, 1,
                  "with Keep local the upload goes over the guard" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
  if (report.operations.empty()) {
    harness::DeleteSave(client, base, fixture, occupant.id);
    return;
  }
  const sync::OperationResult& done = report.operations[0];
  checks.Expect(done.error == sync::OperationError::kNone,
                "no 409 for an upload the player chose: " + done.message);

  // What `overwrite=true` actually does to the occupant, measured rather than
  // assumed -- and **this scenario asserted the wrong thing until it went red in
  // CI**. It does not reliably replace the occupying row.
  //
  // 5.2.0 looked the existing row up by a second-granularity datetime tag, so a
  // POST replaced the row in place only inside the second of the ingest that
  // created it -- a race this scenario used to win on a fast laptop and lose in
  // CI (issue #85). 5.4.0 reserves a fresh tagged name for every slotted upload
  // (`reserve_version_name`, `handler/asset_store.py`), so the re-post below is
  // a second row however fast it lands, and under `overwrite=true` that holds
  // even for the same bytes. The wait before it stays: it costs two seconds and
  // keeps the assertion from resting on which release the fixture runs.
  // Every row this scenario leaves on the fixture, removed on the way out
  // however it exits. `save_id` is best-effort on an `OperationResult` -- it is
  // parsed for the report and decides nothing -- so an upload can succeed and
  // still name no row, and a scenario that only deleted the ids it managed to
  // read would leak one into a shared library.
  std::vector<std::int64_t> planted = {occupant.id};
  const auto sweep = [&client, &base, &fixture, &planted]() {
    std::sort(planted.begin(), planted.end());
    planted.erase(std::unique(planted.begin(), planted.end()), planted.end());
    for (const std::int64_t id : planted) {
      harness::DeleteSave(client, base, fixture, id);
    }
  };

  if (!done.save_id.has_value()) {
    checks.Expect(false, "the upload reports the save row it wrote: " + done.message);
    sweep();
    return;
  }
  const std::int64_t uploaded_id = *done.save_id;
  planted.push_back(uploaded_id);

  // What does hold however the tag falls: the upload the player chose landed,
  // the bytes that went up are this device's, and the row carries the sync
  // history the next negotiation arbitrates against.
  harness::Save written;
  checks.Expect(harness::ReadSave(client.Send(harness::Authed(
                                      http::Method::kGet,
                                      base + "/api/saves/" + std::to_string(uploaded_id), fixture)),
                                  &written),
                "the row the upload wrote is readable");
  checks.ExpectEq(written.file_size_bytes, static_cast<std::int64_t>(bytes.size()),
                  "and holds this device's bytes");
  ExpectDeviceSynced(checks, client, base, fixture, rom.id, slot, uploaded_id,
                     "and carries this device's sync history");

  // Now the part that decides whether a slot accretes a row per tick. A second
  // has to have passed for this to be a fact rather than a coin toss, which is
  // exactly what `PassASecond` is for: it sleeps two seconds of the same wall
  // clock RomM stamps its tag from, so the tag it computes cannot be the one it
  // computed above.
  harness::PassASecond();
  keep.now = OptionsAt(1'757'000'012).now;
  const sync::ExecutionReport again =
      sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, targets, keep);
  checks.ExpectEq(again.completed, 1, "the same plan re-executed is still accepted");
  if (again.operations.empty() || !again.operations[0].save_id.has_value()) {
    checks.Expect(false, "the re-executed upload reports the row it wrote");
    sweep();
    return;
  }
  const std::int64_t second_id = *again.operations[0].save_id;
  planted.push_back(second_id);
  checks.Expect(second_id != uploaded_id,
                "a re-posted upload a second later is a SECOND row, not the same one moved "
                "forward -- overwrite=true does not prevent that (docs/API_CONTRACT.md)");

  // ...and the row it did not land on is untouched, which is the half that makes
  // this accretion rather than a move: the bytes of the earlier row are still
  // the bytes that were put there.
  harness::Save earlier;
  checks.Expect(harness::ReadSave(client.Send(harness::Authed(
                                      http::Method::kGet,
                                      base + "/api/saves/" + std::to_string(uploaded_id), fixture)),
                                  &earlier),
                "the row the first upload wrote is still there");
  checks.ExpectEq(earlier.file_size_bytes, static_cast<std::int64_t>(bytes.size()),
                  "with its own bytes, untouched by the second");

  sweep();
}

// --- tie ------------------------------------------------------------------------
//
// #272. RomM 5.4.0 stores `updated_at` to the whole second, and negotiate keeps
// whichever row of a slot's newest second its unordered query meets first --
// the older one without `rom_ids`, the newer with them, on this fixture -- where
// its own slot guard and `promote_to_slot_head` order by `updated_at`, then id.
// So a Keep local upload landing in the second another device's did can lose
// the head to it, and the next negotiation, with no history against the other
// row, plans a download of it: the player's choice undone (backed up, but
// undone, and with nothing said).
//
// The client's side (`ExecuteOptions::keep_local`): every Keep local upload
// rewrites its own row with the bytes it already holds once the second has
// turned, which makes it the slot's newest by a whole second, however a query
// orders the slot.

/// Whether two of RomM's timestamps name the same whole second.
bool SameSecond(const std::string& left, const std::string& right) {
  const std::optional<sync::Timestamp> a = sync::ParseTimestamp(left);
  const std::optional<sync::Timestamp> b = sync::ParseTimestamp(right);
  return a.has_value() && b.has_value() && *a == *b;
}

void Tie(rig::Checks& checks, http::HttpClient& client, const std::string& base,
         const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-tie");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string name = "tie.srm";
  const std::string bytes = "the bytes the player chose to keep\n";
  sandbox.SeedSave(SavePath(name), bytes);
  const std::string staged = sandbox.Host("/config/rommsync/other-device.srm");
  rig::WriteFile(staged, "the copy another device uploaded in the same second\n");

  std::vector<std::int64_t> planted;
  const auto sweep = [&]() {
    for (const std::int64_t id : planted) {
      harness::DeleteSave(client, base, fixture, id);
    }
    planted.clear();
  };

  // RomM gives no way to stamp a row, so the tie is arranged by doing the two
  // uploads back to back -- the plan is negotiated first, into an empty slot --
  // which loses a second boundary now and then and not five times running.
  // `created_at` is what says whether it was arranged: the fix moves the
  // Keep local row's `updated_at` on, and that is what this checks below.
  for (int attempt = 0; attempt < 5; ++attempt) {
    const std::string slot = harness::UniqueSlot("m10-32-tie");
    sync::SyncNegotiatePayload payload;
    payload.device_id = fixture.device_id;
    // An mtime older than both rows: what the player kept was restored onto the
    // card before the tick, and it is what makes the older row read as
    // `Server save is newer (no sync history)` to the next negotiation.
    payload.saves.push_back(harness::LocalSave(
        rom.id, name, slot, "m2-5", crypto::Md5Hex(bytes),
        std::chrono::system_clock::now() - std::chrono::hours{1},
        static_cast<std::int64_t>(bytes.size())));
    sync::SyncPlan plan;
    if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
      sweep();
      return;
    }

    harness::Save occupant;
    if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m2-5", staged, "other.srm",
                             /*with_device=*/false, &occupant)) {
      checks.Expect(false, "the other device's upload landed");
      sweep();
      return;
    }
    planted.push_back(occupant.id);
    sync::ExecuteOptions keep = OptionsAt(1'757'000'020);
    keep.keep_local = {{rom.id, slot}};
    keep.reported = &payload.saves;
    const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
    const sync::ExecutionReport report =
        sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, targets, keep);
    if (report.operations.empty() || !report.operations[0].save_id.has_value()) {
      checks.Expect(false, "the Keep local upload reports the row it wrote");
      sweep();
      return;
    }
    const sync::OperationResult& done = report.operations[0];
    planted.push_back(*done.save_id);
    harness::Save mine;
    if (!harness::ReadSave(client.Send(harness::Authed(
                               http::Method::kGet,
                               base + "/api/saves/" + std::to_string(*done.save_id), fixture)),
                           &mine)) {
      checks.Expect(false, "the Keep local row is readable");
      sweep();
      return;
    }
    if (!SameSecond(occupant.created_at, mine.created_at)) {
      sweep();
      continue;
    }

    checks.Expect(occupant.id < mine.id, "the other device's row is the older");
    checks.ExpectEq(report.completed, 1,
                    "the Keep local upload completed" +
                        (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
    checks.Expect(done.outcome == sync::OperationOutcome::kUploaded,
                  std::string("as an upload: ") + sync::ToString(done.outcome) + " -- " +
                      done.message);
    checks.Expect(!SameSecond(mine.updated_at, occupant.updated_at),
                  "and its row has moved out of the second it tied in: " + mine.updated_at +
                      " vs the other device's " + occupant.updated_at);
    checks.ExpectEq(sandbox.Read(SavePath(name)), bytes, "the card still holds the kept bytes");
    checks.Expect(!sandbox.HasBackupOf(bytes), "and nothing was backed up: nothing was replaced");

    // What the player chose is what the next negotiation names -- not a
    // download of the other device's row -- in both of the shapes measured to
    // break the tie differently: unscoped, and scoped as a tick is.
    for (const bool scoped : {false, true}) {
      sync::SyncNegotiatePayload asked = payload;
      if (scoped) {
        asked.rom_ids = std::vector<std::int64_t>{rom.id};
      }
      sync::SyncPlan next;
      if (!PlanFor(checks, client, base, fixture, asked, {slot}, &next)) {
        continue;
      }
      checks.Expect(next.operations[0].action == sync::Action::kNoOp &&
                        next.operations[0].save_id == mine.id,
                    std::string("the next negotiation") + (scoped ? ", scoped," : "") +
                        " names the Keep local row as the head: " +
                        sync::ToString(next.operations[0].action) + " save " +
                        (next.operations[0].save_id.has_value()
                             ? std::to_string(*next.operations[0].save_id)
                             : std::string("null")) +
                        ", \"" + next.operations[0].reason_text + "\"");
    }
    sweep();
    return;
  }
  checks.Expect(false, "two uploads landed in one second at least once in five tries");
}

// --- dropped ------------------------------------------------------------------
//
// A real TCP reset mid-download. The save must survive it, and the next tick
// must be able to finish the job -- which is the same operation run again, since
// nothing about the first attempt is left to reconcile.

void Dropped(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-dropped");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-5-dropped");
  const std::string name = "dropped.srm";
  const std::string previous = "the only copy of this save\n";
  const std::string server_bytes = "the server's copy, arriving at the second attempt\n";

  sandbox.SeedSave(SavePath(name), previous);

  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, server_bytes);
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "m2-5", staged, name,
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "the server copy was stored");
    return;
  }

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(
      rom.id, name, slot, "m2-5", crypto::Md5Hex(previous),
      std::chrono::system_clock::now() - std::chrono::hours{1},
      static_cast<std::int64_t>(previous.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"drop","bytes":4,"path":")" + server.ContentPath() + R"("})");
    const sync::ExecutionReport interrupted = sync::ExecutePlan(
        client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'005));
    checks.ExpectEq(interrupted.failed, 1, "the dropped download is counted failed");
    if (!interrupted.operations.empty()) {
      checks.Expect(interrupted.operations[0].error == sync::OperationError::kTransferFailed,
                    std::string("...as a transfer that did not complete: ") +
                        sync::ToString(interrupted.operations[0].error));
    }
  }
  checks.ExpectEq(sandbox.Read(SavePath(name)), previous, "the save survived the reset");
  checks.Expect(!sandbox.HasBackupOf(previous),
                "and nothing was backed up for an overwrite that never happened");
  // And leaves nothing behind. The backend keeps its `.part` for a resume that
  // this client never performs, so a handled failure clears both it and the
  // staging path -- otherwise a save whose download is interrupted and never
  // planned again keeps a dead `Game.srm.tmp.part` on the card forever. The
  // same pair after a *crash*, where no cleanup got to run, is
  // `sync::RecoverStaging`'s -- `tick.recovery`.
  checks.Expect(!sandbox.Exists(io::TempPathFor(SavePath(name)) + ".part"),
                "the partial is cleared: it is never resumed, so it is litter");
  checks.Expect(!sandbox.Exists(io::TempPathFor(SavePath(name))),
                "and no staged copy, because none completed");

  // The next tick, which for this operation is the same operation: the plan is
  // still valid and nothing on the server changed. This is the whole claim --
  // an interrupted download costs a tick, not a save.
  const sync::ExecutionReport retried = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'006));
  checks.ExpectEq(retried.completed, 1,
                  "the retry completed" +
                      (retried.warnings.empty() ? std::string() : ": " + retried.warnings[0]));
  checks.ExpectEq(sandbox.Read(SavePath(name)), server_bytes, "with the server's bytes on the card");
  if (!retried.operations.empty()) {
    checks.Expect(!retried.operations[0].backup_sd_path.empty() &&
                      sandbox.Read(retried.operations[0].backup_sd_path) == previous,
                  "and the previous bytes under .backup/ this time");
  }
  checks.Expect(!sandbox.Exists(io::TempPathFor(SavePath(name)) + ".part"),
                "the partial file from the reset did not survive the completed download");

  harness::DeleteSave(client, base, fixture, server.id);
}

// --- revoked ------------------------------------------------------------------
//
// M1-4 (#8): a 401 arriving part way through a plan.
//
// It is not this operation's problem and it is not the next one's either -- the
// token is gone, so every remaining operation would be refused the same way.
// Twenty requests to prove that is nineteen too many on a battery, and each one
// of them is a chance to have half-written something. `ExecutePlan` stops where
// it stands, the call `download::Drain` already makes for its queue.
//
// Two uploads, and the fault spends itself on the first. That is what makes the
// stop observable rather than assumed: if the plan carried on, the second upload
// would meet a healthy server and succeed.

void Revoked(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-revoked");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  const std::string first_slot = harness::UniqueSlot("m1-4-revoked-a");
  const std::string second_slot = harness::UniqueSlot("m1-4-revoked-b");
  const std::string first_name = "revoked-a.srm";
  const std::string second_name = "revoked-b.srm";
  const std::string first_bytes = "the save the 401 lands on\n";
  const std::string second_bytes = "the save that is never attempted\n";
  sandbox.SeedSave(SavePath(first_name), first_bytes);
  sandbox.SeedSave(SavePath(second_name), second_bytes);

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  const auto now = std::chrono::system_clock::now();
  payload.saves.push_back(harness::LocalSave(rom.id, first_name, first_slot, "m1-4",
                                             crypto::Md5Hex(first_bytes), now,
                                             static_cast<std::int64_t>(first_bytes.size())));
  payload.saves.push_back(harness::LocalSave(rom.id, second_name, second_slot, "m1-4",
                                             crypto::Md5Hex(second_bytes), now,
                                             static_cast<std::int64_t>(second_bytes.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {first_slot, second_slot}, &plan)) {
    return;
  }

  const std::vector<sync::SaveTarget> targets = {
      {rom.id, first_slot, SavePath(first_name), first_name},
      {rom.id, second_slot, SavePath(second_name), second_name}};

  sync::ExecutionReport report;
  {
    // One use, so a plan that carried on would find the server healthy again and
    // upload the second save -- which is exactly what must not happen.
    harness::Fault fault(checks, client, base,
                         R"({"mode":"status","status":401,"path":"/api/saves","count":1})");
    report = sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, targets,
                               OptionsAt(1'757'000'100));
  }

  checks.Expect(report.unauthorized, "the report says the token was refused");
  checks.ExpectEq(static_cast<int>(report.operations.size()), 1,
                  "and the plan stopped at the operation that met it");
  checks.ExpectEq(report.completed, 0, "nothing completed");
  checks.ExpectEq(report.failed, 1,
                  "and the one that was attempted is counted failed, not skipped");
  checks.ExpectEq(report.not_understood, 0, "nothing was downgraded");
  checks.Expect(!report.canceled, "and this is not a cancellation -- nobody stopped it");

  if (report.operations.empty()) {
    return;
  }
  const sync::OperationResult& refused = report.operations[0];
  checks.Expect(refused.outcome == sync::OperationOutcome::kFailed,
                std::string("the operation failed: ") + sync::ToString(refused.outcome));
  checks.Expect(refused.error == sync::OperationError::kUnauthorized,
                std::string("with the token as the reason, not a refused body: ") +
                    sync::ToString(refused.error) + " -- " + refused.message);
  checks.Expect(refused.message.find(fixture.token) == std::string::npos,
                "and the message does not quote the token back");
  checks.Expect(sync::AnswerOf(refused.error) == auth::Answer::kRejected,
                "which is what the gate counts");

  // Nothing was written and nothing was destroyed. The sandbox's teardown audit
  // makes the second half of that claim independently -- a save whose bytes
  // changed with no copy under `.backup/` fails the run whatever is asserted
  // here.
  checks.ExpectEq(sandbox.Read(SavePath(first_name)), first_bytes,
                  "the save the 401 landed on is untouched");
  checks.ExpectEq(sandbox.Read(SavePath(second_name)), second_bytes,
                  "and so is the one that was never attempted");
  checks.Expect(!refused.save_id.has_value(), "no server row was created");

  // The server agrees: a second negotiation still plans an upload for the save
  // that was never sent, rather than a no-op over one RomM thinks it has.
  sync::SyncPlan again;
  if (PlanFor(checks, client, base, fixture, payload, {first_slot, second_slot}, &again)) {
    for (const sync::SyncOperation& operation : again.operations) {
      checks.Expect(operation.action == sync::Action::kUpload,
                    std::string("both saves are still the client's alone: ") +
                        sync::ToString(operation.action));
    }
  }
}

// --- collision ----------------------------------------------------------------
//
// The acceptance criterion the old backup scheme failed: two saves of ONE rom,
// overwritten in the same second, must produce two backups. The clock is frozen
// so "the same second" is a property of the test rather than of the machine.

void Collision(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "execute-collision");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string first_slot = harness::UniqueSlot("m2-5-collide-a");
  const std::string second_slot = harness::UniqueSlot("m2-5-collide-b");
  const std::string first_name = "collide.srm";
  const std::string second_name = "collide.state";
  const std::string first_previous = "the save's own bytes\n";
  const std::string second_previous = "the save state's bytes, quite different\n";
  const std::string first_server = "the server's save\n";
  const std::string second_server = "the server's save state\n";

  sandbox.SeedSave(SavePath(first_name), first_previous);
  sandbox.SeedSave(SavePath(second_name), second_previous);

  const std::string staged = sandbox.Host("/config/rommsync/server-copy");
  harness::Save first_remote;
  harness::Save second_remote;
  rig::WriteFile(staged, first_server);
  const bool stored_first =
      harness::UploadSave(client, base, fixture, rom.id, first_slot, "m2-5", staged, first_name,
                          /*with_device=*/false, &first_remote);
  rig::WriteFile(staged, second_server);
  const bool stored_second =
      harness::UploadSave(client, base, fixture, rom.id, second_slot, "m2-5", staged, second_name,
                          /*with_device=*/false, &second_remote);
  if (!stored_first || !stored_second) {
    checks.Expect(false, "both server copies were stored");
    harness::DeleteSave(client, base, fixture, first_remote.id);
    harness::DeleteSave(client, base, fixture, second_remote.id);
    return;
  }

  const sync::Timestamp stale = std::chrono::system_clock::now() - std::chrono::hours{1};
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(rom.id, first_name, first_slot, "m2-5",
                                             crypto::Md5Hex(first_previous), stale,
                                             static_cast<std::int64_t>(first_previous.size())));
  payload.saves.push_back(harness::LocalSave(rom.id, second_name, second_slot, "m2-5",
                                             crypto::Md5Hex(second_previous), stale,
                                             static_cast<std::int64_t>(second_previous.size())));

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {first_slot, second_slot}, &plan)) {
    harness::DeleteSave(client, base, fixture, first_remote.id);
    harness::DeleteSave(client, base, fixture, second_remote.id);
    return;
  }

  const std::vector<sync::SaveTarget> targets = {
      {rom.id, first_slot, SavePath(first_name), first_name},
      {rom.id, second_slot, SavePath(second_name), second_name},
  };
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'007));

  checks.ExpectEq(report.completed, 2,
                  "both downloads completed" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
  if (report.operations.size() == 2) {
    const std::string& first_backup = report.operations[0].backup_sd_path;
    const std::string& second_backup = report.operations[1].backup_sd_path;
    checks.Expect(!first_backup.empty() && !second_backup.empty(), "both saves were backed up");
    checks.Expect(first_backup != second_backup,
                  "two saves of one rom in the same second are two backups: " + first_backup);
    // The failure the old scheme produced was not a missing file -- it was one
    // file holding the wrong bytes, because the second copy landed on the
    // first's name. So the contents are what this checks.
    checks.Expect(sandbox.HasBackupOf(first_previous) && sandbox.HasBackupOf(second_previous),
                  "and each holds the bytes of the save it belongs to");
  }

  harness::DeleteSave(client, base, fixture, first_remote.id);
  harness::DeleteSave(client, base, fixture, second_remote.id);
}

}  // namespace

int main(int argc, char** argv) {
  // The durability hook the sysmodule installs from its own `main` (M2-7). A
  // suite that left it null would be proving the weaker of the two promises
  // `io::CopyAtomically` can make, on the very path a backup depends on.
  rommsync::host::InstallPosixFileSync();

  const std::string scenario = argc > 1 ? argv[1] : "naming";
  const std::string base = rig::BaseUrl();

  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);

  // The tally lives here, above every scenario, and each scenario returns void:
  // a `Sandbox` reports its teardown audit into this object, and a scenario that
  // returned its own count would copy the number out before the audit ran. See
  // `harness::Sandbox`'s constructor.
  rig::Checks checks;

  // The two that need no server, so they stay checked with docker stopped.
  if (scenario == "naming" || scenario == "staging" || scenario == "counting" ||
      scenario == "crash_model" || scenario == "overwrite" || scenario == "content_hash" ||
      scenario == "delete" || scenario == "crash_delete" || scenario == "isolated" ||
      scenario == "late_write" || scenario == "declined") {
    if (scenario == "naming") {
      Naming(checks);
    } else if (scenario == "overwrite") {
      Overwrite(checks);
    } else if (scenario == "content_hash") {
      ContentHash(checks);
    } else if (scenario == "staging") {
      Staging(checks);
    } else if (scenario == "crash_model") {
      CrashModel(checks);
    } else if (scenario == "delete") {
      Delete(checks);
    } else if (scenario == "crash_delete") {
      CrashDelete(checks);
    } else if (scenario == "isolated") {
      Isolated(checks);
    } else if (scenario == "declined") {
      Declined(checks);
    } else if (scenario == "late_write") {
      LateWriteDelete(checks);
      LateWriteUnbacked(checks);
    } else {
      Counting(checks);
    }
    if (checks.failures() == 0) {
      std::cout << "execute." << scenario << " ok\n";
    }
    return checks.failures() == 0 ? 0 : 1;
  }

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

  if (scenario == "upload") {
    Upload(checks, *client, base, fixture, rom);
  } else if (scenario == "download") {
    Download(checks, *client, base, fixture, rom);
  } else if (scenario == "crash") {
    Crash(checks, *client, base, fixture, rom);
  } else if (scenario == "placed") {
    Placed(checks, *client, base, fixture, rom);
  } else if (scenario == "conflict") {
    Conflict(checks, *client, base, fixture, rom);
  } else if (scenario == "conflict_unarchived") {
    ConflictUnarchived(checks, *client, base, fixture, rom);
  } else if (scenario == "same_timestamp") {
    SameTimestamp(checks, *client, base, fixture, rom);
  } else if (scenario == "truncate") {
    Truncate(checks, *client, base, fixture, rom);
  } else if (scenario == "corrupted") {
    Corrupted(checks, *client, base, fixture, rom);
  } else if (scenario == "occupied") {
    Occupied(checks, *client, base, fixture, rom);
  } else if (scenario == "tie") {
    Tie(checks, *client, base, fixture, rom);
  } else if (scenario == "dropped") {
    Dropped(checks, *client, base, fixture, rom);
  } else if (scenario == "collision") {
    Collision(checks, *client, base, fixture, rom);
  } else if (scenario == "late_fetch") {
    LateWriteFetch(checks, *client, base, fixture, rom);
  } else if (scenario == "revoked") {
    Revoked(checks, *client, base, fixture, rom);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  // Whatever the scenario did, it does not get to leave a fault armed for
  // whichever test runs next -- nor the archival copies its conflicts left
  // (#240).
  rig::DisarmFault(*client, base);
  harness::DeleteArchivalCopies(*client, base, fixture, rom.id);
  harness::ExpectDisarmed(checks, *client, base, "the scenario left the proxy disarmed");

  if (checks.failures() == 0) {
    std::cout << "execute." << scenario << " ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
