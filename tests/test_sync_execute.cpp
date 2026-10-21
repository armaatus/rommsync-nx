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
#include "harness.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/disk.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/sync_execute.hpp"

namespace {

namespace auth = rommsync::auth;
namespace crypto = rommsync::crypto;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace io = rommsync::io;
namespace json = rommsync::json;
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

/// `GET /api/saves/{id}`, and whether RomM has a sync row for this device.
///
/// That row is the whole point of passing `device_id` on an upload and of
/// `POST /api/saves/{id}/downloaded` after a download: without it every later
/// negotiation for the save falls into the no-sync-history branch
/// (docs/API_CONTRACT.md).
///
/// **`device_id` on the read is not optional either.** Verified against the
/// live 5.2.0: `GET /api/saves/{id}` and `GET /api/saves` answer
/// `device_syncs: []` unless the query names a device, and an empty array reads
/// exactly like "this device has never synced this save" -- which is the state
/// the whole arbitration hangs on. It is a filter and not a write: the
/// `last_synced_at` it answers with is the one the upload wrote.
bool DeviceIsSynced(http::HttpClient& client, const std::string& base, const Fixture& fixture,
                    std::int64_t save_id) {
  const http::Result result = client.Send(
      harness::Authed(http::Method::kGet,
                      base + "/api/saves/" + std::to_string(save_id) + "?device_id=" +
                          fixture.device_id,
                      fixture));
  if (!result.successful()) {
    return false;
  }
  const json::ParseResult document = json::Parse(result.response.body);
  const json::Value* rows = document.ok() ? document.value.Find("device_syncs") : nullptr;
  if (rows == nullptr) {
    return false;
  }
  for (const json::Value& row : rows->elements()) {
    if (harness::Field(row, "device_id") == fixture.device_id) {
      return true;
    }
  }
  return false;
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
// a `string_view` and a save state does not fit in a 1.4 MiB heap -- and
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
    const sync::ExecutionReport report =
        sync::ExecutePlan(accepted, *files, token, plan, targets, options);
    checks.ExpectEq(report.completed, 2, "both uploads are accepted");
    if (accepted.urls.size() == 2) {
      checks.Expect(accepted.urls[0].find("overwrite=false") != std::string::npos &&
                        accepted.urls[0].find("overwrite=true") == std::string::npos,
                    "an upload the plan asked for respects the slot guard: " + accepted.urls[0]);
      checks.Expect(accepted.urls[1].find("overwrite=true") != std::string::npos,
                    "and only an explicit Keep local overrides it: " + accepted.urls[1]);
    } else {
      checks.Expect(false, "one request per upload, got " + std::to_string(accepted.urls.size()));
    }
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
  checks.Expect(DeviceIsSynced(client, base, fixture, *done.save_id),
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

  checks.Expect(DeviceIsSynced(client, base, fixture, server.id),
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
    checks.ExpectEq(guessing.failed, 1, "with no policy it is a named failure, not a guess");
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
    checks.ExpectEq(report.failed, 1, "a placement onto a file is a failed operation");
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
    checks.ExpectEq(report.failed, 1, "a refused placement is a failed operation");
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
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'002));

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
// row per upload: RomM matches the existing row by a second-granularity datetime
// tag it computes at ingest, so only an upload inside the same second as the
// previous one lands on the same row (issue #85, and docs/API_CONTRACT.md).

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
  // RomM stamps a slot upload with a second-granularity datetime tag and then
  // looks the existing row up *by that tagged name*
  // (`_apply_datetime_tag` and `get_save_by_filename` in `endpoints/saves.py`,
  // 5.2.0). So a POST replaces the row in place only when it lands in the same
  // wall-clock second as the ingest that created it, with the same base file
  // name; one second later the name it computes matches nothing and RomM writes
  // a **second** row. That is a race, and it is the one this scenario used to
  // win on a fast laptop and lose in CI (issue #85).
  //
  // So the identity assertion is made in the only direction that is
  // deterministic: after a second has certainly passed, a new row appears and
  // the occupant is left alone. The same-second case is real and is what made
  // the old assertion pass; it is deliberately not asserted, because a test that
  // has to win a race is a test that reports the machine's speed.
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
  checks.Expect(DeviceIsSynced(client, base, fixture, uploaded_id),
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
      scenario == "delete" || scenario == "crash_delete") {
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
  } else if (scenario == "same_timestamp") {
    SameTimestamp(checks, *client, base, fixture, rom);
  } else if (scenario == "truncate") {
    Truncate(checks, *client, base, fixture, rom);
  } else if (scenario == "corrupted") {
    Corrupted(checks, *client, base, fixture, rom);
  } else if (scenario == "occupied") {
    Occupied(checks, *client, base, fixture, rom);
  } else if (scenario == "dropped") {
    Dropped(checks, *client, base, fixture, rom);
  } else if (scenario == "collision") {
    Collision(checks, *client, base, fixture, rom);
  } else if (scenario == "revoked") {
    Revoked(checks, *client, base, fixture, rom);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  // Whatever the scenario did, it does not get to leave a fault armed for
  // whichever test runs next.
  rig::DisarmFault(*client, base);
  harness::ExpectDisarmed(checks, *client, base, "the scenario left the proxy disarmed");

  if (checks.failures() == 0) {
    std::cout << "execute." << scenario << " ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
