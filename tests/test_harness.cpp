// The host harness, and the edge cases only a fault proxy can force.
//
// Every scenario here runs the engine's own code -- `EncodeNegotiateRequest`,
// the native `HttpClient`, `WriteAtomically` -- against the real RomM 5.2.0 in
// docker, with `server/testing/fault_proxy.py` in front of it damaging exactly
// one thing about one genuine response. Nothing is mocked, so a green run means
// a live server did that.
//
// The table in issue M0-5, and where each row is:
//
//   401 / expired token mid-flow      `expired`
//   conflict                          `conflict`, `same_timestamp`
//   partial failure mid-plan          `partial`
//   dropped connection + Range resume `resume`
//   truncated body                    `truncate`
//   timeout / stall                   `stall`
//   multi-file rom skip               `multifile`
//
// plus the harness's own two guarantees, which are the ones that keep the rest
// honest: `sandbox` (the per-test SD card, and the backup rule it enforces
// whether or not a test remembers to look) and `disarms` (a fault cannot
// outlive the scope that armed it).
//
// **What is deliberately not here.** The issue's wording asks for the client's
// *behaviour* on a conflict and on a partial plan -- resolve by policy, count
// `operations_failed`, retry next tick. That behaviour is M2-5's and M2-7's
// code, which is not written; docs/TESTING.md records the split. So these
// scenarios prove the two things M0 owes: that each case can be produced on
// demand, and what the server actually does in it -- including the parts a
// reasonable client would guess wrong, which are named at each scenario.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include "harness.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/sha1.hpp"
#include "rommsync/state_db.hpp"

namespace {

namespace crypto = rommsync::crypto;
namespace http = rommsync::http;
namespace io = rommsync::io;
namespace state = rommsync::state;
namespace json = rommsync::json;
namespace sync = rommsync::sync;

using harness::Fixture;
using harness::Sandbox;

/// The seeded fixtures these scenarios need by name. Generated deterministically
/// by server/testing/make_fixtures.py, so all three are byte-identical
/// everywhere.
constexpr const char* kLargeRom = "synthetic-large.gba";
constexpr const char* kMultiRom = "Synthetic Two Disc Game";
constexpr const char* kNestedRom = "Synthetic Nested Game";

/// The one file inside the nested rom's directory, for the byte-for-byte check
/// that the whole-rom endpoint serves *it* and not an archive of it.
constexpr const char* kNestedRomSource =
    ROMMSYNC_FIXTURE_LIBRARY "/roms/psx/Synthetic Nested Game/Synthetic Nested Game.bin";

/// The large rom as it sits in the library RomM serves, for a byte-for-byte
/// comparison against what a download produced.
constexpr const char* kLargeRomSource = ROMMSYNC_FIXTURE_LIBRARY "/roms/gba/synthetic-large.gba";

/// The engine's own reading of a plan, scoped to this run's slot.
///
/// The scenarios below assert on the raw JSON *and* on this, and the two are
/// not redundant: the raw fields prove what the server sent, and this proves
/// `sync::ParseNegotiateResponse` turns it into the one thing a client acts on.
/// The two conflict reasons are where that matters most -- they arrive as the
/// same `action`, and a client that classified only the first would send the
/// second down the default branch, which on a save is the branch that
/// overwrites it.
const sync::SyncOperation* Classified(rig::Checks& checks, const std::string& body,
                                      const std::string& slot,
                                      rommsync::auth::Parsed<sync::SyncPlan>* into) {
  *into = sync::ParseNegotiateResponse(body);
  if (!into->ok()) {
    checks.Expect(false, "the engine reads the plan: " + into->error.Describe());
    return nullptr;
  }
  for (const sync::SyncOperation& operation : into->value.operations) {
    if (operation.slot.has_value() && *operation.slot == slot) {
      return &operation;
    }
  }
  checks.Expect(false, "the parsed plan carries an operation for this run's slot");
  return nullptr;
}

using harness::PassASecond;
using harness::SavePath;

/// Swallow `std::cerr` for the duration of a scope.
///
/// Only for the handful of assertions below that *expect* a failure -- proving
/// the sandbox audit and the fault-proxy arm check are load-bearing means making
/// them fire, and `checks::Checks` reports a failure by printing one. Without
/// this a green run prints FAIL lines and reads like a broken test.
class Quiet {
 public:
  Quiet() : previous_(std::cerr.rdbuf(&sink_)) {}
  ~Quiet() { std::cerr.rdbuf(previous_); }

  Quiet(const Quiet&) = delete;
  Quiet& operator=(const Quiet&) = delete;

 private:
  /// Discards everything written to it. A null buffer would set badbit instead,
  /// which some standard libraries then report on their own.
  class Sink : public std::streambuf {
   protected:
    int overflow(int character) override { return character; }
  };

  Sink sink_;
  std::streambuf* previous_;
};

// --- sandbox ------------------------------------------------------------------
//
// The harness's own guarantee, and the only scenario here that needs no server:
// a per-test SD card that is removed afterwards, and an audit that turns
// docs/SYNC_PROTOCOL.md's hard rule into something a test cannot forget.

void SandboxScenario(rig::Checks& checks) {

  // 1. the SD-path mapping, and teardown -------------------------------------
  std::string remembered;
  {
    Sandbox sandbox(checks, "layout");
    remembered = sandbox.root().string();
    checks.Expect(sandbox.Host("/retroarch/saves/Game.srm") ==
                      (sandbox.root() / "retroarch/saves/Game.srm").string(),
                  "an SD-root path maps onto the sandbox");
    checks.Expect(sandbox.Exists(harness::kConfigDir), "/config/rommsync exists");
    checks.Expect(sandbox.Exists(harness::kBackupDir), ".backup/ exists");
    checks.Expect(sandbox.Exists(harness::kSavesDir), "the save directory exists");

    // A second sandbox in the same process gets its own tree; two tests cannot
    // see each other's files even inside one binary.
    Sandbox other(checks, "layout");
    checks.Expect(other.root() != sandbox.root(), "two sandboxes are two directories");
  }
  // ROMMSYNC_KEEP_SANDBOX is the documented way to look at what a red run wrote
  // (harness.hpp), and it keeps the tree on purpose -- so asserting removal
  // under it would turn the debugging switch into a second red test.
  if (std::getenv("ROMMSYNC_KEEP_SANDBOX") == nullptr) {
    checks.Expect(!std::filesystem::exists(remembered),
                  "the sandbox is removed when the test ends");
  }

  // 2. the audit passes for an overwrite that backed up first ------------------
  {
    rig::Checks audited;
    Sandbox sandbox(audited, "backed-up");
    sandbox.Detach();
    sandbox.SeedSave(SavePath("Game.srm"), "the previous save\n");
    sandbox.Write(sandbox.BackupPathFor(7, "retroarch-srm", "Game.srm"), "the previous save\n");
    sandbox.Write(SavePath("Game.srm"), "the new save\n");
    checks.ExpectEq(sandbox.Audit(audited), 0, "backing up first satisfies the audit");
  }

  // 3. ...and fails for every way of not doing it -----------------------------
  // Each of these is a way a real implementation gets it wrong, and none of them
  // is caught by a test that merely asserts the save now holds the new bytes.
  {
    rig::Checks audited;
    Sandbox sandbox(audited, "no-backup");
    sandbox.Detach();
    sandbox.SeedSave(SavePath("Game.srm"), "the previous save\n");
    sandbox.Write(SavePath("Game.srm"), "the new save\n");
    // `Quiet` covers the audit and nothing else. If it covered the assertion
    // below too, an audit that stopped firing would exit 1 having printed
    // nothing -- which is the failure this whole file is written against.
    int caught = 0;
    {
      const Quiet quiet;
      caught = sandbox.Audit(audited);
    }
    checks.ExpectEq(caught, 1, "an overwrite with no backup is caught");
  }
  {
    rig::Checks audited;
    Sandbox sandbox(audited, "backed-up-wrong");
    sandbox.Detach();
    sandbox.SeedSave(SavePath("Game.srm"), "the previous save\n");
    sandbox.Write(SavePath("Game.srm"), "the new save\n");
    // The commonest wrong version: back up *after* the write, so the copy holds
    // the bytes that replaced the save rather than the ones it destroyed.
    sandbox.Write(sandbox.BackupPathFor(7, "retroarch-srm", "Game.srm"), "the new save\n");
    int caught = 0;
    {
      const Quiet quiet;
      caught = sandbox.Audit(audited);
    }
    checks.ExpectEq(caught, 1, "a backup taken after the overwrite is not a backup");
  }
  {
    rig::Checks audited;
    Sandbox sandbox(audited, "interrupted");
    sandbox.Detach();
    sandbox.SeedSave(SavePath("Game.srm"), "the previous save\n");
    // An overwrite that died half way: the save is gone and the replacement
    // never arrived. This is the case the fault proxy exists to produce, and the
    // one where a missing backup costs the only copy.
    std::error_code error;
    std::filesystem::remove(sandbox.Host(SavePath("Game.srm")), error);
    int caught = 0;
    {
      const Quiet quiet;
      caught = sandbox.Audit(audited);
    }
    checks.ExpectEq(caught, 1, "an interrupted overwrite is caught too");
  }
}


// --- disarms ------------------------------------------------------------------
//
// "Each test leaves the proxy disarmed" is an acceptance criterion, and the way
// it gets broken is a scenario that returns early past its own cleanup. A red
// run then lands in whichever test happens to go next.

void Disarms(rig::Checks& checks, http::HttpClient& client, const std::string& base) {

  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"status","status":401,"path":"/api/heartbeat"})");
    http::Request request;
    request.url = base + "/__fault";
    const http::Result armed = client.Send(request);
    checks.Expect(armed.successful() && armed.response.body.find("\"status\"") != std::string::npos,
                  "the scenario is armed inside the scope: " + armed.response.body);
  }
  harness::ExpectDisarmed(checks, client, base, "the scope disarmed it");

  // The other half: a spec the proxy refuses must be loud. It answers 400 and
  // forwards everything untouched, so a scenario built on it runs clean and
  // passes having exercised nothing -- which is the failure this whole file is
  // written against.
  {
    rig::Checks rejected;
    {
      const Quiet quiet;
      harness::Fault fault(rejected, client, base, R"({"mode":"truncate"})");
    }
    checks.ExpectEq(rejected.failures(), 1,
                    "a refused spec fails the test rather than running unarmed");
  }
  harness::ExpectDisarmed(checks, client, base, "and the refusal left nothing behind");
}


// --- expired ------------------------------------------------------------------
//
// A 401 arriving in the middle of a sync. RomM issues no refresh token, so a
// genuine 401 means revoked (docs/AUTH.md) -- but a 401 is still a *response*,
// not a transport failure, and the client has to read it as one. That is the
// distinction `http::Result` draws and the one a client that checks only
// `ok()` gets wrong.

void Expired(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture) {
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;  // an empty `saves` is a legitimate "what am I missing?"

  {
    // "Mid-flow" is the point: the fault lets the tick start normally and then
    // fails its SECOND negotiate. `after` counts only requests whose path
    // matches, so the heartbeat and the fixture calls around it do not consume
    // it -- which is what makes the target deterministic rather than a race.
    harness::Fault fault(
        checks, client, base,
        R"({"mode":"status","status":401,"path":"/api/sync/negotiate","after":1,"count":1})");

    const http::Result first = harness::Negotiate(checks, client, base, fixture, payload);
    // One active session per device, and every scenario here shares the
    // fixture's. Left open, this one is cancelled by the NEXT negotiate --
    // and that cancel races with the session that negotiate creates. See
    // harness::CloseSession and issue #76.
    harness::CloseSession(client, base, fixture, first.response.body);
    checks.ExpectEq(first.response.status, 200, "the tick starts normally");

    const http::Result denied = harness::Negotiate(checks, client, base, fixture, payload);
    // One active session per device, and every scenario here shares the
    // fixture's. Left open, this one is cancelled by the NEXT negotiate --
    // and that cancel races with the session that negotiate creates. See
    // harness::CloseSession and issue #76.
    harness::CloseSession(client, base, fixture, denied.response.body);
    checks.Expect(denied.ok(),
                  "a 401 is a response, not a transport error -- Result::ok() stays true");
    checks.Expect(!denied.successful(), "...and successful() does not");
    checks.ExpectEq(denied.response.status, 401, "the token was refused mid-flow");

    // The fault disarmed itself after one use, so this is the real server again.
    // The point is what the client must NOT conclude: one 401 from something in
    // front of RomM is not a revoked pairing, and a client that shreds
    // `token.dat` on the first one sends the user to a pairing screen for a
    // proxy hiccup.
    const http::Result recovered = harness::Negotiate(checks, client, base, fixture, payload);
    // One active session per device, and every scenario here shares the
    // fixture's. Left open, this one is cancelled by the NEXT negotiate --
    // and that cancel races with the session that negotiate creates. See
    // harness::CloseSession and issue #76.
    harness::CloseSession(client, base, fixture, recovered.response.body);
    checks.ExpectEq(recovered.response.status, 200,
                    "the same token still works -- one 401 is not a verdict on the pairing");
  }

}


// --- conflict -----------------------------------------------------------------
//
// `Both sides changed since last sync`: this device has a sync record for the
// save, and each side moved past it.
//
// Arranging it needs one fact that is not guessable and that a client will meet
// the first time it uploads twice: **a slot upload gets a datetime tag in its
// file name**, so a second `POST /api/saves` a second later is a *new save row*
// with no sync history, whatever `overwrite` says. Only `PUT /api/saves/{id}`
// moves the same row forward. Get that wrong and this scenario silently becomes
// the no-history one below.

void Conflict(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "conflict");
  const std::string slot = harness::UniqueSlot("m0-5-conflict");
  const std::string name = "conflict.srm";

  // The server's first copy, uploaded with `device_id` so RomM writes this
  // device's sync row. That row is the "last sync" both sides are compared to.
  sandbox.Write(SavePath(name), "server v1\n");
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "harness",
                           sandbox.Host(SavePath(name)), name, /*with_device=*/true, &server)) {
    checks.Expect(false, "the first server copy was stored");
    return;
  }

  PassASecond();

  // The server's copy changes after that sync -- somebody else's console
  // uploaded, or the user edited it in RomM.
  sandbox.Write(SavePath(name), "server v2, longer\n");
  harness::Save moved;
  if (!harness::ReplaceSave(client, base, fixture, server.id, sandbox.Host(SavePath(name)), name,
                            &moved)) {
    checks.Expect(false, "the server copy moved forward in place");
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }
  checks.ExpectEq(moved.id, server.id,
                  "PUT moved the same save row -- a second POST would have made a new one");

  // And so did this device's, to different bytes.
  sandbox.Write(SavePath(name), "device v2\n");
  std::string device_hash;
  checks.Expect(
      harness::ServerMd5(client, base, fixture, rom.id, sandbox.Host(SavePath(name)), &device_hash),
      "the device copy's MD5");

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(
      rom.id, name, slot, "harness", device_hash,
      std::chrono::system_clock::now() + std::chrono::seconds{300}, 10));

  const http::Result result = harness::Negotiate(checks, client, base, fixture, payload);
  // One active session per device, and every scenario here shares the
  // fixture's. Left open, this one is cancelled by the NEXT negotiate --
  // and that cancel races with the session that negotiate creates. See
  // harness::CloseSession and issue #76.
  harness::CloseSession(client, base, fixture, result.response.body);
  checks.ExpectEq(result.response.status, 200, "the negotiation is answered");
  const json::ParseResult plan = json::Parse(result.response.body);
  const json::Value* operation = plan.ok() ? harness::OperationFor(plan.value, slot) : nullptr;
  if (operation == nullptr) {
    checks.Expect(false, "the plan carries an operation for this run's slot: " +
                             result.response.body);
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }

  checks.ExpectEq(harness::Field(*operation, "action"), std::string("conflict"),
                  "both sides moved past the last sync, so the server refuses to choose");
  checks.ExpectEq(harness::Field(*operation, "reason"),
                  std::string("Both sides changed since last sync"),
                  "...and says which of the two conflicts this is");

  rommsync::auth::Parsed<sync::SyncPlan> read;
  if (const sync::SyncOperation* typed = Classified(checks, result.response.body, slot, &read)) {
    checks.Expect(typed->action == sync::Action::kConflict, "the engine reads it as a conflict");
    checks.Expect(typed->reason == sync::Reason::kBothChanged,
                  std::string("...and classifies the history conflict: ") + typed->reason_text);
    checks.Expect(read.value.warnings.empty(), "with nothing unrecognised in it");
  }

  // RomM sends no resolution -- there is no server_wins/keep_both field to obey
  // (docs/SYNC_PROTOCOL.md). What it sends is what the client needs to show a
  // human and to write a backup against, so their absence is worth asserting.
  checks.ExpectEq(harness::Field(*operation, "server_content_hash"), moved.content_hash,
                  "the server's hash comes back, so the client can tell the copies apart");
  checks.Expect(!harness::Field(*operation, "server_updated_at").empty(),
                "and the server's timestamp, so a human can be shown what they are choosing");
  checks.Expect(operation->Find("resolution") == nullptr,
                "the server does NOT pick a winner -- the policy is the client's");
  checks.ExpectEq(harness::Number(plan.value, "total_conflict"), 1,
                  "and the totals count it as one");

  // The file name in the operation is the server's, datetime tag and all -- not
  // the name this device holds. Writing it to the SD produces a save no
  // emulator loads, which is why the pairing key is (rom_id, slot).
  checks.Expect(harness::Field(*operation, "file_name") != name,
                "the operation echoes the SERVER's file name, not the local one: " +
                    harness::Field(*operation, "file_name"));

  harness::DeleteSave(client, base, fixture, server.id);
}


// --- same_timestamp -----------------------------------------------------------
//
// The other conflict, and the one a client drops on the floor: **no sync
// history**, the two timestamps equal, the hashes different. RomM compares at
// second granularity, so a save written in the same second as the server's copy
// by a device that has not synced it lands here. A `switch` on the first reason
// alone sends this into the default branch -- and on a conflict the default
// branch is the one that can overwrite a save.

void SameTimestamp(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                   const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "same-timestamp");
  const std::string slot = harness::UniqueSlot("m0-5-same-ts");
  const std::string name = "same-timestamp.srm";

  // No `device_id`: this device has never synced this save, which is what puts
  // the comparison in the timestamp branch.
  sandbox.Write(SavePath(name), "server copy\n");
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "harness",
                           sandbox.Host(SavePath(name)), name, /*with_device=*/false, &server)) {
    checks.Expect(false, "the server copy was stored");
    return;
  }

  sync::Timestamp when;
  checks.Expect(harness::ParseServerTimestamp(server.updated_at, &when),
                "the server's updated_at parses: " + server.updated_at);

  sandbox.Write(SavePath(name), "device copy\n");
  std::string device_hash;
  checks.Expect(
      harness::ServerMd5(client, base, fixture, rom.id, sandbox.Host(SavePath(name)), &device_hash),
      "the device copy's MD5");
  checks.Expect(device_hash != server.content_hash, "the two copies really are different bytes");

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(harness::LocalSave(rom.id, name, slot, "harness", device_hash, when, 12));

  const http::Result result = harness::Negotiate(checks, client, base, fixture, payload);
  // One active session per device, and every scenario here shares the
  // fixture's. Left open, this one is cancelled by the NEXT negotiate --
  // and that cancel races with the session that negotiate creates. See
  // harness::CloseSession and issue #76.
  harness::CloseSession(client, base, fixture, result.response.body);
  const json::ParseResult plan = json::Parse(result.response.body);
  const json::Value* operation = plan.ok() ? harness::OperationFor(plan.value, slot) : nullptr;
  if (operation == nullptr) {
    checks.Expect(false, "the plan carries an operation for this run's slot: " +
                             result.response.body);
    harness::DeleteSave(client, base, fixture, server.id);
    return;
  }

  checks.ExpectEq(harness::Field(*operation, "action"), std::string("conflict"),
                  "equal timestamps and different content is a conflict, not a guess");
  checks.ExpectEq(harness::Field(*operation, "reason"),
                  std::string("Same timestamp but different content"),
                  "...with the second of the two reasons a client must handle");

  rommsync::auth::Parsed<sync::SyncPlan> read;
  if (const sync::SyncOperation* typed = Classified(checks, result.response.body, slot, &read)) {
    checks.Expect(typed->action == sync::Action::kConflict, "the engine reads it as a conflict");
    // Distinct from `harness.conflict`'s reason, and that is the whole point:
    // one `action`, two situations, and only the reason separates them.
    checks.Expect(typed->reason == sync::Reason::kSameTimestampDifferentContent,
                  std::string("...and classifies the no-history conflict: ") + typed->reason_text);
    checks.Expect(typed->reason != sync::Reason::kBothChanged,
                  "which is NOT the reason the other conflict scenario produces");
    checks.Expect(read.value.warnings.empty(), "with nothing unrecognised in it");
  }

  harness::DeleteSave(client, base, fixture, server.id);
}


// --- partial ------------------------------------------------------------------
//
// A plan whose Nth operation fails. The client owes the server an accurate
// `operations_failed` at `complete` and owes the next tick the files it did not
// manage -- never a half-written one.

void Partial(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "partial");

  // Three saves this device has and the server does not, so the plan is three
  // uploads. Distinct bytes per slot, so a mixed-up upload is visible.
  struct Planned {
    std::string slot;
    std::string name;
    std::int64_t save_id = 0;
    bool uploaded = false;
  };
  std::vector<Planned> planned;
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  for (int index = 0; index < 3; ++index) {
    Planned entry;
    entry.slot = harness::UniqueSlot("m0-5-partial-" + std::to_string(index));
    entry.name = "partial-" + std::to_string(index) + ".srm";
    const std::string bytes = "partial save " + std::to_string(index) + "\n";
    sandbox.Write(SavePath(entry.name), bytes);

    std::string hash;
    checks.Expect(harness::ServerMd5(client, base, fixture, rom.id,
                                     sandbox.Host(SavePath(entry.name)), &hash),
                  "the MD5 of " + entry.name);
    payload.saves.push_back(harness::LocalSave(
        rom.id, entry.name, entry.slot, "harness", hash,
        std::chrono::system_clock::now() + std::chrono::seconds{300},
        static_cast<std::int64_t>(bytes.size())));
    planned.push_back(entry);
  }

  const http::Result negotiated = harness::Negotiate(checks, client, base, fixture, payload);
  const json::ParseResult plan = json::Parse(negotiated.response.body);
  if (!plan.ok()) {
    checks.Expect(false, "the plan parses: " + negotiated.response.body);
    return;
  }
  const std::int64_t session_id = harness::Number(plan.value, "session_id");
  checks.Expect(session_id != 0, "a session was opened");
  for (Planned& entry : planned) {
    const json::Value* operation = harness::OperationFor(plan.value, entry.slot);
    checks.Expect(operation != nullptr && harness::Field(*operation, "action") == "upload",
                  "a save the server does not have is planned as an upload");
  }

  int completed = 0;
  int failed = 0;
  {
    // The *second* upload fails. `after` counts only requests whose path starts
    // with the prefix, so the negotiate above and the MD5 uploads before it do
    // not consume it -- but note that the prefix does match `/api/saves/delete`
    // too, which is why the fault is scoped to the walk and not to the cleanup.
    harness::Fault fault(checks, client, base,
                         R"({"mode":"status","status":500,"path":"/api/saves","after":1,"count":1})");

    for (Planned& entry : planned) {
      harness::Save stored;
      if (harness::UploadSave(client, base, fixture, rom.id, entry.slot, "harness",
                              sandbox.Host(SavePath(entry.name)), entry.name,
                              /*with_device=*/true, &stored)) {
        entry.save_id = stored.id;
        entry.uploaded = true;
        ++completed;
      } else {
        ++failed;
      }
    }
  }

  checks.ExpectEq(completed, 2, "two operations of the three got through");
  checks.ExpectEq(failed, 1, "and one did not");
  checks.Expect(!planned[1].uploaded, "the failure landed on the operation the fault named");

  // The one that failed left nothing on the server. "Never a half-written save"
  // is the rule; this is its server-side half.
  const http::Result listed = client.Send(harness::Authed(
      http::Method::kGet, base + "/api/saves?rom_id=" + std::to_string(rom.id), fixture));
  checks.Expect(listed.successful(), "the save list came back");
  checks.Expect(listed.response.body.find(planned[1].slot) == std::string::npos,
                "the failed upload left no save behind");

  // What the server thinks of the session, read BEFORE completing it.
  //
  // This is here because of a CI-only failure that said only
  // `expected 200, got 400` and `{"detail":"Session is already CANCELLED"}` --
  // true, and useless. It does not say who cancelled it, when, or what the
  // server had recorded by then, and none of that is reproducible locally
  // (fresh fixture or warm, macOS or the same suite in the same order). A
  // failing assertion that cannot say what it saw costs a round trip through CI
  // for every guess, so the state goes in the failure message.
  const http::Result before = client.Send(harness::Authed(
      http::Method::kGet, base + "/api/sync/sessions/" + std::to_string(session_id), fixture));
  // ...and every session this device has, because the only thing known to
  // cancel one is another negotiate for the same device. Locally this scenario
  // creates exactly ONE session and it ends COMPLETED with 2 of 3 done; if CI
  // shows a second, the negotiate happened twice and that is the whole answer.
  const http::Result all = client.Send(harness::Authed(
      http::Method::kGet, base + "/api/sync/sessions", fixture));
  const std::string state = "session " + std::to_string(session_id) + " before complete: HTTP " +
                            std::to_string(before.response.status) + " " + before.response.body +
                            " | all sessions: " + all.response.body;

  const http::Result done = harness::Complete(client, base, fixture, session_id, completed, failed);
  checks.ExpectEq(done.response.status, 200, "the session completes -- " + state);
  const json::ParseResult session = json::Parse(done.response.body);
  const json::Value* record = session.ok() ? session.value.Find("session") : nullptr;
  if (record == nullptr) {
    checks.Expect(false, "the completed session comes back: " + done.response.body +
                             " -- " + state);
  } else {
    checks.ExpectEq(harness::Number(*record, "operations_failed"), 1,
                    "the server records the accurate failure count");
    checks.ExpectEq(harness::Number(*record, "operations_completed"), 2,
                    "and the accurate completed count");
    checks.ExpectEq(harness::Field(*record, "status"), std::string("COMPLETED"),
                    "a partial plan still completes its session -- it is not an aborted sync");
    // `operations_planned` counts the operations that needed *work*, over the
    // whole plan -- not the three this test sent. Negotiate also reports every
    // server save this device has no history for, so the number is only
    // meaningful against the plan it came from.
    std::int64_t needing_work = 0;
    if (const json::Value* operations = plan.value.Find("operations"); operations != nullptr) {
      for (const json::Value& operation : operations->elements()) {
        if (harness::Field(operation, "action") != "no_op") {
          ++needing_work;
        }
      }
    }
    checks.Expect(needing_work >= 3, "the plan asked for at least this test's three uploads");
    checks.ExpectEq(harness::Number(*record, "operations_planned"), needing_work,
                    "operations_planned counts the operations that needed work");
  }

  for (const Planned& entry : planned) {
    harness::DeleteSave(client, base, fixture, entry.save_id);
  }
}


// --- resume -------------------------------------------------------------------
//
// A dropped connection mid-download, then `Range` resume -- against the 120 MiB
// seeded rom rather than a JSON body, because that is the transfer this matters
// for and the only fixture big enough to interrupt convincingly.
//
// The reset is a real TCP RST from the proxy, and the bytes that survive it are
// whatever the kernel delivered, so nothing here asserts an exact count. What is
// asserted is the pair of guarantees a downloader lives or dies by: the
// destination never holds a short file, and the resumed halves are the original
// bytes -- checked against the file RomM is serving, not against a hash, so a
// splice at the wrong offset names the byte it went wrong at.

void Resume(rig::Checks& checks, http::HttpClient& client, const std::string& base,
            const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "resume");
  sandbox.MakeDirs("/tico/roms/gba");
  const std::string sd_path = "/tico/roms/gba/synthetic-large.gba";
  const std::string destination = sandbox.Host(sd_path);

  constexpr std::uint64_t kCutAt = 4 * 1024 * 1024;
  checks.Expect(rom.size > static_cast<std::int64_t>(kCutAt),
                "the fixture rom is big enough to interrupt");

  http::Request request = harness::Authed(http::Method::kGet, base + rom.ContentPath(), fixture);
  // No total timeout on a large transfer: `stall_timeout` is what tells slow
  // from dead (http.hpp).
  request.timeout = std::chrono::milliseconds{0};

  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"drop","bytes":)" + std::to_string(kCutAt) + R"(,"path":")" +
                             rom.ContentPath() + R"("})");
    const http::Result interrupted =
        client.Download(request, rig::DownloadTo(destination, false,
                                                 static_cast<std::uint64_t>(rom.size)));
    checks.ExpectError(interrupted, http::Error::kTruncated, "a reset mid-body is an error");
  }

  checks.Expect(!sandbox.Exists(sd_path), "no short file at the destination");
  const std::uintmax_t partial = sandbox.SizeOf(sd_path + ".part");
  checks.Expect(partial > 0 && partial <= kCutAt, "the bytes that did arrive are kept to resume");

  const http::Result resumed =
      client.Download(request, rig::DownloadTo(destination, true,
                                               static_cast<std::uint64_t>(rom.size)));
  checks.ExpectOk(resumed, "the resumed attempt");
  checks.ExpectEq(resumed.response.status, 206, "the resume was a Range request RomM honoured");
  checks.ExpectEq(resumed.response.bytes_received,
                  static_cast<std::uint64_t>(rom.size) - partial,
                  "only the missing bytes were fetched");
  checks.Expect(!sandbox.Exists(sd_path + ".part"), "the partial file was renamed away");
  checks.ExpectEq(static_cast<std::int64_t>(sandbox.SizeOf(sd_path)), rom.size,
                  "the rom is its whole declared size");

  std::uint64_t differs_at = 0;
  checks.Expect(harness::SameBytes(destination, kLargeRomSource, &differs_at),
                "the two halves are the rom RomM is serving, byte for byte -- first difference at " +
                    std::to_string(differs_at));
}


// --- truncate -----------------------------------------------------------------
//
// The quiet one. `truncate` is a clean, short, plausible response with no
// declared length to compare against: no transport can fault it, and the only
// thing that catches it is the caller's own knowledge of what the file weighs.
//
// It is aimed at a *save* here rather than a rom, because that is where being
// wrong costs something irreplaceable: a short body written over a save is a
// destroyed save, and the destination must never see it.

void Truncate(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "truncate");
  const std::string slot = harness::UniqueSlot("m0-5-truncate");
  const std::string name = "truncate.srm";
  const std::string previous = "the save already on the card, which must survive\n";

  sandbox.SeedSave(SavePath(name), previous);

  // Something to download: a server copy, with different bytes and a length
  // worth checking against.
  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  const std::string server_bytes = "the server's copy of this save, longer than the local one\n";
  rig::WriteFile(staged, server_bytes);
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "harness", staged, name,
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "the server copy was stored");
    return;
  }
  checks.ExpectEq(server.file_size_bytes, static_cast<std::int64_t>(server_bytes.size()),
                  "the server reports the size the client will verify against");

  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"truncate","bytes":5,"path":")" + server.ContentPath() +
                             R"("})");

    http::Request request =
        harness::Authed(http::Method::kGet, base + server.ContentPath(), fixture);
    const http::Result cut = client.Download(
        request, rig::DownloadTo(sandbox.Host(SavePath(name)), false,
                                 static_cast<std::uint64_t>(server.file_size_bytes)));
    checks.ExpectError(cut, http::Error::kTruncated,
                       "a clean short body is caught by the caller's own expected size");
  }

  // The three things that make this a *safe* failure rather than a lost save.
  checks.ExpectEq(sandbox.Read(SavePath(name)), previous,
                  "the save on the card is untouched -- the download never reached it");
  // A partial file IS kept -- that is what a resume picks up (http.hpp). What
  // matters is that it is still under the other name and still short, so nothing
  // downstream can mistake it for the save.
  checks.Expect(static_cast<std::int64_t>(sandbox.SizeOf(SavePath(name) + ".part")) <
                    server.file_size_bytes,
                "whatever survived is short, and is not where the save lives");

  // Now prove the size check is what caught it, rather than luck: the same
  // truncated body with no expected size is accepted as a complete file. This is
  // the one assertion that makes the previous one mean something.
  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"truncate","bytes":5,"path":")" + server.ContentPath() +
                             R"("})");
    const std::string blind = sandbox.Host("/config/rommsync/blind.srm");
    http::Request request =
        harness::Authed(http::Method::kGet, base + server.ContentPath(), fixture);
    const http::Result unchecked = client.Download(request, rig::DownloadTo(blind));
    checks.ExpectOk(unchecked, "without an expected size the same body looks complete");
    checks.ExpectEq(rig::ReadFile(blind).size(), std::size_t{5},
                    "...and five bytes are accepted as the whole save");
  }

  harness::DeleteSave(client, base, fixture, server.id);
}


// --- stall --------------------------------------------------------------------
//
// A server that accepts the connection and then says nothing. The rule is that
// a tick aborts cleanly and changes nothing (docs/SYNC_PROTOCOL.md): a sync that
// hangs is a sysmodule that never gets to the next one, and `Never block boot`
// is a hard rule.

void Stall(rig::Checks& checks, http::HttpClient& client, const std::string& base,
           const Fixture& fixture) {
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
  checks.Expect(encoded.ok(), "the payload encodes");

  const auto started = std::chrono::steady_clock::now();
  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"stall","seconds":10,"path":"/api/sync/negotiate"})");

    http::Request request =
        harness::Authed(http::Method::kPost, base + "/api/sync/negotiate", fixture);
    request.headers.push_back({"Content-Type", "application/json"});
    request.body = encoded.body;
    request.timeout = std::chrono::milliseconds{2'000};
    const http::Result stalled = client.Send(request);
    checks.ExpectError(stalled, http::Error::kTimeout, "the tick gives up rather than hanging");
  }
  const auto waited =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
  checks.Expect(waited < std::chrono::seconds{9},
                "...and gives up on its own timeout, not on the server's -- waited " +
                    std::to_string(waited.count()) + "s");

  // A stall must cost one tick, not the pairing: the next one goes through.
  const http::Result next = harness::Negotiate(checks, client, base, fixture, payload);
  // One active session per device, and every scenario here shares the
  // fixture's. Left open, this one is cancelled by the NEXT negotiate --
  // and that cancel races with the session that negotiate creates. See
  // harness::CloseSession and issue #76.
  //
  // This is now the ONLY session this scenario opens. The stalled negotiate
  // above is dropped by the proxy rather than replayed once the client has
  // given up, so it reaches RomM never rather than eight seconds late, inside
  // whichever test was running by then (issue #109).
  harness::CloseSession(client, base, fixture, next.response.body);
  checks.ExpectEq(next.response.status, 200, "and the next tick negotiates normally");
}


// --- stall_dropped ------------------------------------------------------------
//
// A stalled request must never reach RomM. Issue #109.
//
// Every `stall` caller in this suite sets a client timeout well under the sleep,
// because what each of them asserts is that the CLIENT gives up first. The proxy
// used to wake afterwards and forward the request anyway -- seconds late, with
// nobody waiting for the answer and usually after the test that armed it had
// exited. What arrived was not a slow server; it was a second client nothing
// could see, writing a save row nobody asked for or opening a sync session that
// cancelled whatever the NEXT test had just started.
//
// The damage is only ever visible on the SERVER, after the sleep has elapsed, so
// that is where both halves are asserted. Every other `stall` scenario checks
// its own client and would pass either way -- which is exactly why this one has
// to exist and why it is written to fail against the old proxy rather than to
// describe the new one.
//
// Both halves poll rather than sleep a fixed span: a failure is reported the
// moment the row or the session appears, so the full deadline is only ever spent
// on the passing path.

void StallDropped(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                  const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "stall-dropped");

  constexpr int kStallSeconds = 4;
  constexpr auto kClientTimeout = std::chrono::milliseconds{1'200};
  // Past the point an abandoned request would have been replayed, with room for
  // a loaded machine: the old proxy forwarded at exactly `kStallSeconds`.
  constexpr auto kDeadline = std::chrono::seconds{kStallSeconds + 4};
  const std::string stall_for =
      R"("mode":"stall","seconds":)" + std::to_string(kStallSeconds) + R"(,"count":1)";

  // --- the upload that never left ---------------------------------------------
  const std::string slot = harness::UniqueSlot("m0-5-stall-dropped");
  const std::string name = "stall-dropped.srm";
  sandbox.Write(SavePath(name), "bytes the server must never be given\n");

  // -1 for "could not read the listing", so a hiccup cannot be mistaken for
  // "no row". Without that distinction every assertion below passes on a failed
  // GET and the scenario goes green having proved nothing -- which is the one
  // outcome worse than it failing. `SaveRowsFor` in test_sync_tick.cpp does the
  // same thing for the same reason.
  const auto rows_for_slot = [&](std::vector<std::int64_t>* ids) {
    ids->clear();
    const http::Result listed = client.Send(harness::Authed(
        http::Method::kGet, base + "/api/saves?rom_id=" + std::to_string(rom.id), fixture));
    const json::ParseResult parsed = json::Parse(listed.response.body);
    if (!listed.successful() || !parsed.ok() || !parsed.value.is_array()) {
      return -1;
    }
    for (const json::Value& save : parsed.value.elements()) {
      if (harness::Field(save, "slot") == slot) {
        ids->push_back(harness::Number(save, "id"));
      }
    }
    return static_cast<int>(ids->size());
  };

  std::vector<std::int64_t> in_slot;
  checks.ExpectEq(rows_for_slot(&in_slot), 0,
                  "the slot starts empty, so a row in it can only be this upload");

  // The fault is armed with `count:1` on a path prefix, and the proxy holds one
  // armed scenario for every client -- so this scenario, like every other rig
  // scenario, depends on running alone (RUN_SERIAL). The listings are
  // deliberately outside the fault scope: `/api/saves` is a prefix of this
  // fault's path, and a listing inside it would spend the stall on itself.
  const auto upload_started = std::chrono::steady_clock::now();
  {
    harness::Fault fault(checks, client, base,
                         "{" + stall_for + R"(,"path":"/api/saves"})");

    http::Request upload = harness::Authed(
        http::Method::kPost,
        base + "/api/saves?rom_id=" + std::to_string(rom.id) + "&emulator=harness&slot=" + slot +
            "&device_id=" + fixture.device_id,
        fixture);
    http::FormPart part;
    part.name = "saveFile";
    part.file_path = sandbox.Host(SavePath(name));
    part.file_name = name;
    part.content_type = "application/octet-stream";
    upload.form.push_back(part);
    upload.timeout = kClientTimeout;

    const http::Result stalled = client.Send(upload);
    checks.ExpectError(stalled, http::Error::kTimeout,
                       "the client gives up on the stalled upload, as every stall caller does");
  }

  int rows = 0;
  bool read_a_listing = false;
  std::chrono::seconds row_after{0};
  while (std::chrono::steady_clock::now() - upload_started < kDeadline) {
    rows = rows_for_slot(&in_slot);
    read_a_listing = read_a_listing || rows >= 0;
    if (rows > 0) {
      row_after = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - upload_started);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{250});
  }
  checks.Expect(read_a_listing, "at least one save listing was readable -- otherwise the check "
                                "below is vacuous and this scenario proves nothing");
  checks.ExpectEq(rows, 0,
                  "the abandoned upload never reaches RomM -- a row appeared in " +
                      std::to_string(row_after.count()) +
                      "s, which is the proxy replaying a request the client had already given up "
                      "on. tick.upload_stall's row count rests on this.");
  for (const std::int64_t id : in_slot) {
    harness::DeleteSave(client, base, fixture, id);
  }

  // --- the negotiate that opened nothing --------------------------------------
  //
  // RomM cancels a device's active session on every negotiate
  // (/backend/endpoints/sync.py in 5.2.0), and every scenario in this suite
  // shares the fixture's one device -- so a replayed negotiate does not merely
  // leak a session, it cancels the session whichever test is running by then
  // just opened. That is the shape #76 reports; whether it is #76's cause is not
  // established, and this scenario does not claim it. What it pins is the
  // narrower fact: an abandoned negotiate must not reach RomM at all.
  //
  // Asked as "has this device an IN_PROGRESS session?" rather than by comparing
  // ids. `GET /api/sync/sessions` is `initiated_at DESC` capped at 50 over the
  // whole user, and `initiated_at` is second-granularity -- so ids at the window
  // edge reorder between two reads and a highest-id comparison reports a
  // session that was always there as a new one. A negotiate always opens an
  // IN_PROGRESS session and it is by construction the newest row, so it cannot
  // fall outside that window; the property is stable where the id is not.
  harness::CloseOpenSessions(client, base, fixture);

  // 0 = none for this device, -1 = could not read. Same reason as above.
  const auto in_progress_session = [&]() {
    const http::Result listed =
        client.Send(harness::Authed(http::Method::kGet, base + "/api/sync/sessions", fixture));
    const json::ParseResult parsed = json::Parse(listed.response.body);
    if (!listed.successful() || !parsed.ok()) {
      return std::int64_t{-1};
    }
    // RomM has served this both as a bare array and wrapped in `items`.
    const json::Value* sessions = &parsed.value;
    if (parsed.value.is_object()) {
      sessions = parsed.value.Find("items");
    }
    if (sessions == nullptr || !sessions->is_array()) {
      return std::int64_t{-1};
    }
    for (const json::Value& session : sessions->elements()) {
      if (harness::Field(session, "device_id") != fixture.device_id) {
        continue;
      }
      if (harness::Field(session, "status") != "IN_PROGRESS") {
        continue;
      }
      return harness::Number(session, "id");
    }
    return std::int64_t{0};
  };

  checks.ExpectEq(in_progress_session(), std::int64_t{0},
                  "this device has no open session before the stall, so one afterwards can only "
                  "be the replayed negotiate");

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
  checks.Expect(encoded.ok(), "the payload encodes");

  const auto negotiate_started = std::chrono::steady_clock::now();
  {
    harness::Fault fault(checks, client, base,
                         "{" + stall_for + R"(,"path":"/api/sync/negotiate"})");

    http::Request request =
        harness::Authed(http::Method::kPost, base + "/api/sync/negotiate", fixture);
    request.headers.push_back({"Content-Type", "application/json"});
    request.body = encoded.body;
    request.timeout = kClientTimeout;

    const http::Result stalled = client.Send(request);
    checks.ExpectError(stalled, http::Error::kTimeout, "the client gives up on the stalled tick");
  }

  std::int64_t opened = 0;
  bool read_a_session_list = false;
  std::chrono::seconds session_after{0};
  while (std::chrono::steady_clock::now() - negotiate_started < kDeadline) {
    opened = in_progress_session();
    read_a_session_list = read_a_session_list || opened >= 0;
    if (opened > 0) {
      session_after = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - negotiate_started);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{250});
  }
  checks.Expect(read_a_session_list, "at least one session listing was readable -- otherwise the "
                                     "check below is vacuous");
  checks.ExpectEq(opened, std::int64_t{0},
                  "the abandoned negotiate opens no session -- session " +
                      std::to_string(opened) + " appeared in " +
                      std::to_string(session_after.count()) +
                      "s. A negotiate the client abandoned must not reach RomM: RomM cancels this "
                      "device's active session on every one, and every scenario here shares this "
                      "device.");

  // Whether or not the assertion above held, this scenario does not get to leave
  // a session open for the next one to trip over.
  harness::CloseOpenSessions(client, base, fixture);
}


// --- multifile ----------------------------------------------------------------
//
// The two-disc fixture, and the line M3-4 draws around it. This is what the
// skip hangs off, so everything the decision rests on is pinned here: a RomM
// upgrade that moves any of it turns this red rather than turning a skip into a
// silently wrong download.
//
// Three things here are not what a reading of the endpoint names suggests, and
// all three are silent:
//
//   * `GET /api/roms/{id}/content/{file_name}` on a multi-file rom does not
//     serve a rom. It serves a **zip RomM builds on the fly**, with no
//     `Content-Length` -- so a client that treats it like any other rom writes
//     an archive to the SD under the rom's name and has nothing to verify the
//     length against.
//   * the rom-level `sha1_hash` on a multi-file rom is **not the digest of
//     anything that can be downloaded**: not of the zip, and not of either
//     disc. So the archive above cannot be verified either. That, and not
//     effort, is why v1 skips rather than downloads.
//   * the `{id}` in `/api/roms/{id}/files/content/{file_name}` is the **RomFile
//     id**, not the rom id, and the `{file_name}` segment selects nothing at
//     all. Building that URL from a rom id and a file name returns 200 and the
//     bytes of whatever file happens to carry that id.
//
// And the negative, which matters as much: a rom directory holding exactly
// *one* file is `has_nested_single_file`, not `has_multiple_files`. It is a
// normal download -- the whole-rom endpoint serves the file itself, with a
// length, and the rom's digest *is* that file's. A skip that fired on it would
// refuse a rom this client can handle perfectly well.

void MultiFile(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture, const harness::Rom& multi, const harness::Rom& single,
               const harness::Rom& nested) {

  // The signal is on the LIST schema, not only the detail one, so a client can
  // skip a rom without a second call per rom.
  checks.Expect(multi.has_multiple_files,
                "the two-disc fixture reports has_multiple_files from GET /api/roms");
  checks.Expect(!single.has_multiple_files, "and a single-file rom does not");
  checks.ExpectEq(multi.files.size(), std::size_t{2}, "with one entry per disc");

  // The three shapes are exclusive, and only the first is a skip. A client that
  // read "is a directory" instead of `has_multiple_files` would refuse the
  // nested rom too, so the flags are asserted against each other rather than
  // one at a time.
  checks.Expect(!multi.has_simple_single_file && !multi.has_nested_single_file,
                "and a disc set is neither of the single-file shapes");
  checks.Expect(single.has_simple_single_file,
                "a rom that is one file on disk is has_simple_single_file");
  checks.Expect(nested.has_nested_single_file && !nested.has_multiple_files,
                "and a directory holding one file is has_nested_single_file, not multi-file");

  // The whole-rom download is an archive with nothing to verify against.
  http::Request whole = harness::Authed(http::Method::kGet, base + multi.ContentPath(), fixture);
  whole.method = http::Method::kHead;
  const http::Result archive = client.Send(whole);
  checks.Expect(archive.successful(), "the whole-rom endpoint answers for a multi-file rom");
  const std::string* type = http::FindHeader(archive.response.headers, "Content-Type");
  checks.Expect(type != nullptr && type->find("zip") != std::string::npos,
                "...with a zip, not a rom");
  checks.ExpectEq(archive.response.declared_size, std::uint64_t{0},
                  "and no length a download could be verified against");

  if (multi.files.size() != 2) {
    return;
  }

  // Each disc, by its own file id.
  std::vector<std::string> contents;
  for (const harness::RomFile& file : multi.files) {
    const http::Result disc = client.Send(harness::Authed(
        http::Method::kGet,
        base + "/api/roms/" + std::to_string(file.id) + "/files/content/" +
            harness::UrlEncode(file.file_name),
        fixture));
    checks.Expect(disc.successful(), "GET the disc " + file.file_name);
    checks.ExpectEq(static_cast<std::int64_t>(disc.response.body.size()), file.size,
                    "...and it is the size the rom's files[] declared");
    contents.push_back(disc.response.body);
  }
  checks.Expect(contents.size() == 2 && contents[0] != contents[1],
                "the two discs are different files");

  // The trap: swap the file names and the *same* bytes come back, with a 200.
  // Nothing tells a client it asked for the wrong disc.
  const http::Result mislabelled = client.Send(harness::Authed(
      http::Method::kGet,
      base + "/api/roms/" + std::to_string(multi.files[0].id) + "/files/content/" +
          harness::UrlEncode(multi.files[1].file_name),
      fixture));
  checks.Expect(mislabelled.successful(), "a mismatched {id}/{file_name} pair is not refused");
  checks.ExpectEq(mislabelled.response.body, contents[0],
                  "the id selects the file and the name selects nothing -- so the name in that "
                  "URL cannot be trusted to say what arrived");

  // The fact the whole decision rests on. The rom carries a `sha1_hash` and it
  // looks like every other rom's, but it is the digest of nothing a client can
  // fetch: not the zip above, and not either disc. There is therefore no way to
  // verify a multi-file download, which is why M3-4 skips instead of writing an
  // archive nothing checked to the card.
  checks.Expect(!multi.sha1_hash.empty(),
                "the disc set carries a rom-level sha1_hash, like any other rom");
  for (const harness::RomFile& file : multi.files) {
    checks.Expect(!file.sha1_hash.empty(),
                  "each disc carries its own sha1_hash: " + file.file_name);
    checks.Expect(multi.sha1_hash != file.sha1_hash,
                  "and the rom's digest is not it -- " + file.file_name);
  }
  checks.Expect(multi.files.size() == 2 && multi.files[0].sha1_hash != multi.files[1].sha1_hash,
                "nor are the two discs' digests each other's");
  checks.ExpectEq(crypto::Sha1Hex(contents[0]), multi.files[0].sha1_hash,
                  "the bytes each file id served do hash to that file's digest");
  checks.ExpectEq(crypto::Sha1Hex(contents[1]), multi.files[1].sha1_hash,
                  "...both of them, so the per-file digests are the ones a v2 would check");

  // The route a v2 would take, pinned so that "recorded" means "still true".
  // `file_ids` on the ROM endpoint, carrying a single `files[].id`, is the
  // per-file download: the raw bytes, a real length, `Accept-Ranges`, and a
  // digest in `files[]` to check them against -- everything the whole-rom zip
  // above lacks. It is also *scoped to the rom*, which the `/files/content/`
  // path is not.
  for (std::size_t at = 0; at < multi.files.size(); ++at) {
    const harness::RomFile& file = multi.files[at];
    const http::Result one = client.Send(harness::Authed(
        http::Method::kGet,
        base + multi.ContentPath() + "?file_ids=" + std::to_string(file.id), fixture));
    checks.Expect(one.successful(), "one disc by file_ids: " + file.file_name);
    const std::string* one_type = http::FindHeader(one.response.headers, "Content-Type");
    checks.Expect(one_type != nullptr && one_type->find("octet-stream") != std::string::npos,
                  "...served raw rather than zipped, unlike the whole rom");
    checks.ExpectEq(one.response.declared_size, static_cast<std::uint64_t>(file.size),
                    "...with the length the file declared");
    const std::string* ranges = http::FindHeader(one.response.headers, "Accept-Ranges");
    checks.Expect(ranges != nullptr && ranges->find("bytes") != std::string::npos,
                  "...and Accept-Ranges, so a v2 resumes a disc the way M3-3 resumes a rom");
    const std::string* disposition =
        http::FindHeader(one.response.headers, "content-disposition");
    checks.Expect(disposition != nullptr &&
                      disposition->find(harness::UrlEncode(file.file_name)) != std::string::npos,
                  "...named after the file the id selected, not the rom");
    checks.ExpectEq(one.response.body, contents[at],
                    "...and the same bytes the file id served on the other path");
    checks.ExpectEq(crypto::Sha1Hex(one.response.body), file.sha1_hash,
                    "...which its own sha1_hash verifies -- what a v2 would check");

    // `Accept-Ranges` is a claim; this is the answer to a `Range` request that
    // takes it up, because a header a server advertises and does not honour is
    // the one a resume discovers at the wrong moment.
    http::Request tail = harness::Authed(
        http::Method::kGet, base + multi.ContentPath() + "?file_ids=" + std::to_string(file.id),
        fixture);
    // `substr` and not a slice of whatever arrived: a body shorter than the
    // offset is a failed check, not an uncaught `std::out_of_range` that takes
    // the whole scenario's output with it.
    constexpr std::size_t kResumeFrom = 10;
    if (contents[at].size() > kResumeFrom) {
      tail.range_start = kResumeFrom;
      const http::Result resumed = client.Send(tail);
      checks.ExpectEq(resumed.response.status, 206, "...and a Range request on it comes back 206");
      checks.ExpectEq(resumed.response.body, contents[at].substr(kResumeFrom),
                      "...carrying exactly the bytes that were missing");
    }
  }

  // Both ids is the archive again, so `file_ids` is not a way around the zip:
  // one call per file is what a v2 makes.
  const http::Result both = client.Send(harness::Authed(
      http::Method::kGet,
      base + multi.ContentPath() + "?file_ids=" + std::to_string(multi.files[0].id) + "," +
          std::to_string(multi.files[1].id),
      fixture));
  const std::string* both_type = http::FindHeader(both.response.headers, "Content-Type");
  checks.Expect(both_type != nullptr && both_type->find("zip") != std::string::npos,
                "asking for both files at once is the zip, not a concatenation");

  // ...and unlike `/api/roms/{files[].id}/files/content/`, this one refuses a
  // file id that is not this rom's rather than serving somebody else's bytes.
  if (!nested.files.empty()) {
    const http::Result foreign = client.Send(harness::Authed(
        http::Method::kGet,
        base + multi.ContentPath() + "?file_ids=" + std::to_string(nested.files[0].id), fixture));
    checks.ExpectEq(foreign.response.status, 404,
                    "a file id belonging to another rom is refused, not served");
  }

  // The negative. `has_nested_single_file` is a directory too, and the whole-rom
  // endpoint on it behaves like any other rom's: the file itself, a length to
  // check it against, and a rom-level digest that is the digest of exactly those
  // bytes. Nothing here would be true of the disc set above.
  http::Request whole_nested =
      harness::Authed(http::Method::kGet, base + nested.ContentPath(), fixture);
  const http::Result served = client.Send(whole_nested);
  checks.Expect(served.successful(), "the whole-rom endpoint answers for a nested single-file rom");
  const std::string* nested_type = http::FindHeader(served.response.headers, "Content-Type");
  checks.Expect(nested_type != nullptr && nested_type->find("octet-stream") != std::string::npos,
                "...with the file, not a zip");
  checks.ExpectEq(served.response.declared_size, static_cast<std::uint64_t>(nested.size),
                  "and a Content-Length equal to the rom's fs_size_bytes");
  const std::string* nested_ranges = http::FindHeader(served.response.headers, "Accept-Ranges");
  checks.Expect(nested_ranges != nullptr && nested_ranges->find("bytes") != std::string::npos,
                "and Accept-Ranges, so M3-3's resume works on it like any other rom");
  checks.ExpectEq(served.response.body, rig::ReadFile(kNestedRomSource),
                  "the bytes are the one file inside the rom's directory, exactly");
  checks.ExpectEq(crypto::Sha1Hex(served.response.body), nested.sha1_hash,
                  "and the rom's own sha1_hash verifies them -- unlike a disc set's");
  checks.ExpectEq(nested.files.size(), std::size_t{1}, "its files[] holds the one file");
  if (nested.files.size() == 1) {
    checks.ExpectEq(nested.files[0].sha1_hash, nested.sha1_hash,
                    "whose digest is the rom's, because there is only the one");
    checks.Expect(nested.files[0].file_name != nested.fs_name,
                  "and whose name is not the rom's -- the rom is the directory, so a download "
                  "under fs_name lands without the file's extension");
  }
}


// --- content_hash -------------------------------------------------------------
//
// M2-3. `crypto::Md5` and `state::HashFile` compute what RomM will compare
// against -- which is a claim about the *server*, and no vector suite can check
// it. So this one hashes a save locally, uploads the same bytes, and asks RomM
// what digest it computed (`harness::ServerMd5`, which does exactly that and
// deletes the throwaway save again).
//
// The two failures this rules out are both silent. A SHA1 or an uppercase
// hexdigest is 40 or 32 plausible characters that the server matches against
// nothing, so every unchanged save negotiates as `upload` forever -- there is no
// error, only a client that re-uploads a library every tick. `sync.understood`
// already showed what that costs; this shows the digest is the right one.
//
// The bytes are many times `state::HashFile`'s 4 KiB buffer on purpose: a save
// that fits in one read proves nothing about a save state.

void ContentHash(rig::Checks& checks, http::HttpClient& client, const std::string& base,
                 const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "content_hash");
  const std::string name = "content-hash.srm";

  std::string bytes;
  bytes.reserve(100 * 1024);
  for (std::size_t at = 0; at < 100 * 1024; ++at) {
    bytes += static_cast<char>((at * 7 + 3) % 251);
  }
  if (!sandbox.Write(SavePath(name), bytes)) {
    checks.Expect(false, "the sandbox took the save");
    return;
  }
  const std::string path = sandbox.Host(SavePath(name));

  // The engine's own streaming hash of the file on the card, not a digest of a
  // string this test is holding: `state::HashFile` is the code the tick calls.
  const state::HashOutcome hashed = state::HashFile(path);
  checks.Expect(hashed.ok(), "the save hashed -- " + hashed.message);
  checks.ExpectEq(hashed.content_hash, crypto::Md5Hex(bytes),
                  "streaming the file gives the digest of its bytes");

  std::string server_digest;
  if (!harness::ServerMd5(client, base, fixture, rom.id, path, &server_digest)) {
    checks.Expect(false, "RomM computed a digest for the uploaded save");
    return;
  }

  // The whole point. RomM stores `hexdigest()` of the bytes it received, and
  // this is the string every later negotiation compares against.
  checks.ExpectEq(hashed.content_hash, server_digest,
                  "the local digest is the one RomM computed");

  // ...and it survives the validation the encoder puts every save through, which
  // is where a 40-digit or uppercase digest would have been caught.
  sync::ClientSaveState save;
  save.rom_id = rom.id;
  save.file_name = name;
  save.slot = "autosave";
  save.content_hash = hashed.content_hash;
  save.updated_at = sync::Timestamp{} + std::chrono::seconds{1757000000};
  save.file_size_bytes = static_cast<std::int64_t>(bytes.size());
  const json::Error refused = sync::Validate(save);
  checks.Expect(refused.ok(), "sync::Validate accepts it -- " + refused.Describe());

  // A round trip through the baseline does not change the digest either: what
  // `state.db` stores is what the next tick reports.
  state::Baseline baseline;
  state::SaveRecord row;
  row.rom_id = rom.id;
  row.slot = "autosave";
  row.content_hash = hashed.content_hash;
  row.mtime = save.updated_at;
  row.file_size_bytes = save.file_size_bytes;
  baseline.Set(row);

  const std::string state_path = sandbox.Host(std::string(harness::kConfigDir) + "/state.db");
  const state::StoreResult stored = state::SaveBaseline(state_path, baseline);
  checks.Expect(stored.ok(), "the baseline was written -- " + stored.message);

  const state::LoadedBaseline reloaded = state::LoadBaseline(state_path);
  checks.Expect(reloaded.diagnostics.empty(),
                "and read back clean -- " + reloaded.DescribeDiagnostics());

  // The second tick: nothing moved, so the file is not opened again -- and the
  // digest that goes into the payload is still the one the server holds.
  const state::HashOutcome reused = state::ContentHashFor(
      reloaded.value, rom.id, std::optional<std::string>("autosave"), path, row.mtime,
      row.file_size_bytes);
  checks.Expect(reused.reused, "an unchanged save is not re-hashed on the second tick");
  checks.ExpectEq(reused.content_hash, server_digest,
                  "and it still reports the digest RomM computed");
}

// --- backup -------------------------------------------------------------------
//
// docs/SYNC_PROTOCOL.md's hard rule, on both paths: back up *first*, then
// overwrite. The word doing the work is "first", and the only way to show the
// difference between "first" and "eventually" is to interrupt the overwrite --
// which is the fault proxy's whole reason for existing.
//
// The backup is written with the engine's own `io::WriteAtomically` and the
// overwrite is the engine's own `HttpClient::Download`; the sandbox audits the
// result independently of what this test thought it was doing.

void Backup(rig::Checks& checks, http::HttpClient& client, const std::string& base,
            const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "backup");
  const std::string slot = harness::UniqueSlot("m0-5-backup");
  const std::string name = "backup.srm";
  const std::string previous = "the only copy of this save\n";
  const std::string server_bytes = "the server's copy, which wins this round\n";

  sandbox.SeedSave(SavePath(name), previous);

  const std::string staged = sandbox.Host("/config/rommsync/server-copy.srm");
  rig::WriteFile(staged, server_bytes);
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom.id, slot, "harness", staged, name,
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "the server copy was stored");
    return;
  }

  // 1. back up, then overwrite -- the success path.
  const std::string backup = sandbox.BackupPathFor(rom.id, slot, name);
  const io::WriteResult written =
      io::WriteAtomically(sandbox.Host(backup), sandbox.Read(SavePath(name)));
  checks.Expect(written.ok(), "the backup is written first: " + written.message);

  http::Request request = harness::Authed(http::Method::kGet, base + server.ContentPath(), fixture);
  const http::Result downloaded = client.Download(
      request, rig::DownloadTo(sandbox.Host(SavePath(name)), false,
                               static_cast<std::uint64_t>(server.file_size_bytes)));
  checks.ExpectOk(downloaded, "the server's copy is written over the local one");
  checks.ExpectEq(sandbox.Read(SavePath(name)), server_bytes, "the save now holds the server's copy");
  checks.ExpectEq(sandbox.Read(backup), previous, "and the backup holds the bytes it replaced");

  // 2. the same thing, interrupted. This is where "first" earns its keep: the
  // overwrite never lands, and the copy that would have been destroyed is
  // already safe. A backup taken after a successful write would have nothing
  // here at all.
  //
  // No wait between the two, deliberately: both saves belong to one rom and are
  // backed up in the same second, which under the *documented* scheme was one
  // file and one destroyed backup. M2-5 put the slot in the name, and this is
  // the scenario that would notice if it came back out.
  const std::string second = "second.srm";
  const std::string second_previous = "the second save, also the only copy\n";
  sandbox.SeedSave(SavePath(second), second_previous);
  const std::string second_backup = sandbox.BackupPathFor(rom.id, "harness-second", second);
  checks.Expect(second_backup != backup,
                "two slots of one rom backed up in the same second are two files");
  checks.Expect(io::WriteAtomically(sandbox.Host(second_backup), second_previous).ok(),
                "backed up before touching the second save");

  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"drop","bytes":4,"path":")" + server.ContentPath() + R"("})");
    const http::Result interrupted = client.Download(
        request, rig::DownloadTo(sandbox.Host(SavePath(second)), false,
                                 static_cast<std::uint64_t>(server.file_size_bytes)));
    checks.Expect(!interrupted.ok(), "the overwrite was interrupted");
  }

  checks.ExpectEq(sandbox.Read(SavePath(second)), second_previous,
                  "the save survived, because Download stages to .part");
  checks.ExpectEq(sandbox.Read(second_backup), second_previous,
                  "and the backup taken first is there either way");

  harness::DeleteSave(client, base, fixture, server.id);
}


}  // namespace

int main(int argc, char** argv) {
  // The durability hook the sysmodule installs from its own `main` (M2-7). A
  // suite that left it null would be proving the weaker of the two promises
  // `io::CopyAtomically` can make, on the very path a backup depends on.
  rommsync::host::InstallPosixFileSync();

  const std::string scenario = argc > 1 ? argv[1] : "sandbox";
  const std::string base = rig::BaseUrl();

  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);

  // The tally lives here, above every scenario, and each scenario returns void.
  // That is not a style choice: a `Sandbox` reports its teardown audit into this
  // object, and a scenario that returned its own `checks.failures()` would copy
  // the count out *before* its sandbox was destroyed -- so an audit failure
  // would print and the process would still exit 0. See `Sandbox`'s constructor.
  rig::Checks checks;

  // The sandbox scenario is the harness's own guarantee and needs no server, so
  // it runs with docker stopped -- which is when a broken sandbox is most likely
  // to be introduced and least likely to be noticed.
  if (scenario == "sandbox") {
    SandboxScenario(checks);
    if (checks.failures() == 0) {
      std::cout << "harness.sandbox ok\n";
    }
    return checks.failures() == 0 ? 0 : 1;
  }

  const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  // Whatever an earlier run left armed would damage this one's first request.
  rig::DisarmFault(*client, base);

  Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    return rig::kSkip;
  }

  // The sibling of DisarmFault above: a session an earlier scenario left open is
  // one this scenario's negotiate has to cancel, and that cancel races with the
  // session it just created. See harness::CloseOpenSessions and issue #76.
  harness::CloseOpenSessions(*client, base, fixture);

  if (scenario == "disarms") {
    Disarms(checks, *client, base);
  } else if (scenario == "expired") {
    Expired(checks, *client, base, fixture);
  } else if (scenario == "stall") {
    Stall(checks, *client, base, fixture);
  } else {
    // Everything left needs a rom to hang a save or a download off. A library
    // that was staged but never scanned is an empty one, and reads exactly like
    // a bug in the scenario rather than an unprovisioned fixture.
    harness::Rom small;
    harness::Rom large;
    harness::Rom multi;
    harness::Rom nested;
    if (!harness::FindRom(*client, base, fixture, "gb240p.gb", &small)) {
      std::cerr << "the fixture library holds no roms\n"
                   "  scan it with: ./.venv/bin/python server/testing/provision.py\n";
      return rig::kSkip;
    }

    if (scenario == "conflict") {
      Conflict(checks, *client, base, fixture, small);
    } else if (scenario == "same_timestamp") {
      SameTimestamp(checks, *client, base, fixture, small);
    } else if (scenario == "partial") {
      Partial(checks, *client, base, fixture, small);
    } else if (scenario == "stall_dropped") {
      StallDropped(checks, *client, base, fixture, small);
    } else if (scenario == "truncate") {
      Truncate(checks, *client, base, fixture, small);
    } else if (scenario == "backup") {
      Backup(checks, *client, base, fixture, small);
    } else if (scenario == "content_hash") {
      ContentHash(checks, *client, base, fixture, small);
    } else if (scenario == "resume") {
      if (!harness::FindRom(*client, base, fixture, kLargeRom, &large)) {
        std::cerr << "the library has no " << kLargeRom
                  << "; re-seed it with: ./server/testing/seed.sh\n";
        return rig::kSkip;
      }
      Resume(checks, *client, base, fixture, large);
    } else if (scenario == "multifile") {
      if (!harness::FindRom(*client, base, fixture, kMultiRom, &multi) ||
          !harness::FindRom(*client, base, fixture, kNestedRom, &nested) ||
          !harness::FindRom(*client, base, fixture, kLargeRom, &large)) {
        std::cerr << "the library is missing one of " << kMultiRom << ", " << kNestedRom << ", "
                  << kLargeRom
                  << ". Re-seed AND rescan -- seed.sh only stages files, RomM does not import "
                     "them until the scan runs:\n"
                     "  ./server/testing/seed.sh && ./.venv/bin/python "
                     "server/testing/provision.py\n";
        return rig::kSkip;
      }
      MultiFile(checks, *client, base, fixture, multi, large, nested);
    } else {
      std::cerr << "unknown scenario: " << scenario << "\n";
      return 2;
    }
  }

  // Whatever the scenario did, it does not get to leave a fault armed for
  // whichever test runs next.
  rig::DisarmFault(*client, base);

  if (checks.failures() == 0) {
    std::cout << "harness." << scenario << " ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
