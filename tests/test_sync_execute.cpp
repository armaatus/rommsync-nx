// Step 2 of the sync loop, against the real RomM: the plan, executed.
//
// `sync.*` proves the client can *get* a plan; these prove it can act on one
// without losing a save. Every scenario runs `sync::ExecutePlan` -- the engine's
// own code, over the native `HttpClient`, against the docker 5.2.0 -- inside a
// `harness::Sandbox`, so docs/SYNC_PROTOCOL.md's hard rule is audited on
// teardown whether or not the scenario thought to look.
//
// Three of them need no server and must stay checked with docker stopped:
// `naming` is the backup path that used to collide, `staging` is the two `io`
// primitives an overwrite goes through, and `counting` is what the report says
// about the operations that never reach the network.
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

#include "harness.hpp"
#include "rommsync/atomic_file.hpp"
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
// a `string_view` and a save state does not fit in a 512 KiB heap -- and
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

// --- upload -------------------------------------------------------------------
//
// A plan executed as issued must never be refused by the server that issued it.
// `overwrite=true` is what makes that true: without it RomM answers `409 Slot
// has a newer save since your last sync` for exactly the no-sync-history case
// the plan calls an upload.

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
    placing.place = [&fresh](const sync::SyncOperation&) { return SavePath(fresh); };
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
  // an overwrite that never happened is a file the next tick has to reason
  // about (issue #16).
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
// The state `overwrite=true` exists for, which a fresh slot never reaches: the
// server holds a save for this `(rom_id, slot)` and this device has no sync row
// for it. That is `upload` / `Client save is newer (no sync history)`, and RomM
// answers the same POST without the flag with a 409 -- refusing the very
// operation it just planned. The scenario asserts both halves, because a test
// that only shows the flag working cannot tell you the flag is doing anything.
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
                std::string("...with the reason overwrite=true exists for: ") +
                    plan.operations[0].reason_text);

  // The same request the executor is about to make, minus the one flag. This is
  // the assertion the acceptance criterion is really about: without it, nothing
  // here would notice `overwrite=true` being dropped.
  {
    http::Request refused = harness::Authed(
        http::Method::kPost,
        base + "/api/saves?rom_id=" + std::to_string(rom.id) + "&emulator=m2-5&slot=" + slot +
            "&device_id=" + fixture.device_id,
        fixture);
    http::FormPart part;
    part.name = "saveFile";
    part.file_path = sandbox.Host(SavePath(name));
    part.file_name = name;
    part.content_type = "application/octet-stream";
    refused.form.push_back(part);
    const http::Result answered = client.Send(refused);
    checks.ExpectEq(answered.response.status, 409,
                    "without overwrite=true RomM refuses the upload it just planned: " +
                        answered.response.body);
  }

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'011));

  checks.ExpectEq(report.completed, 1,
                  "and with it the plan executes as issued" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));
  if (report.operations.empty()) {
    harness::DeleteSave(client, base, fixture, occupant.id);
    return;
  }
  const sync::OperationResult& done = report.operations[0];
  checks.Expect(done.error != sync::OperationError::kRefused,
                "no 409 for the operation the server itself planned: " + done.message);

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

  // What the acceptance criterion is actually about, and what does hold however
  // the tag falls: the plan executed as issued, the bytes that went up are this
  // device's, and the row carries the sync history the next negotiation
  // arbitrates against.
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
  const sync::ExecutionReport again = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, OptionsAt(1'757'000'012));
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
  // planned again keeps a dead `Game.srm.tmp.part` on the card forever. What
  // issue #16 still has to sweep is the same pair after a *crash*, where no
  // cleanup got to run.
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
  if (scenario == "naming" || scenario == "staging" || scenario == "counting") {
    if (scenario == "naming") {
      Naming(checks);
    } else if (scenario == "staging") {
      Staging(checks);
    } else {
      Counting(checks);
    }
    if (checks.failures() == 0) {
      std::cout << "execute." << scenario << " ok\n";
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
