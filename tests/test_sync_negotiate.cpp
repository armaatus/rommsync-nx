// The negotiate call against a REAL RomM 5.2.0.
//
// `sync.payload` and `sync.plan` pin the two shapes against the committed
// snapshot and captures, which are documents. These scenarios ask the running
// server, which is the only thing that can answer what a document cannot:
//
//   accepted   -- a body this client built is taken, and the entry comes back
//                 with our file_name, slot and emulator on it. The server read
//                 the fields; it did not merely tolerate the body.
//   required   -- rename a *required* field and the server says so: 422, naming
//                 file_size_bytes. That half of a typo is loud.
//   understood -- rename an *optional* one and nothing says anything. RomM
//                 answers 200 with a plan, and the plan is different: the same
//                 save that negotiates `no_op / Content is identical` under
//                 `content_hash` negotiates `upload` under `hash`, because the
//                 hash it needed was never there. This is the failure the typed
//                 struct exists to make impossible, and it is invisible from a
//                 status code.
//   negotiates -- `sync::Negotiate` end to end: the engine builds the body,
//                 makes the call and hands back a `SyncPlan` with a session and
//                 an operation read field by field.
//   discovers  -- an EMPTY `saves` array, which is how a client asks what it is
//                 missing. The server-only save comes back as a `download`, and
//                 it is the operation where every nullable field is filled in.
//   revoked    -- a 401 is a revoked token, not a parse failure, and the tick
//                 ends with no plan and nothing changed.
//   truncated  -- a clean, short, plausible body is a named `json` error and no
//                 plan. This is the failure mode that would otherwise look like
//                 a device that is already in sync.
//   stalled    -- a server that says nothing times out and is retried, with the
//                 backoff doubling. The wait is injected, so the test proves the
//                 backoff without spending it.
//   refused    -- the answers that are NOT "the network": a device deleted in
//                 RomM's web UI, sync switched off for it, and the 404 a wrong
//                 server_url produces, which must not be read as either.
//
// `understood` and `discovers` put a save on the fixture and delete it again,
// the same way server/probe_contract.py's scenarios do. Nothing here calls
// `/api/sync/sessions/{id}/complete`, so no sync baseline is written.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "harness.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/token_store.hpp"

namespace {

namespace auth = rommsync::auth;
namespace http = rommsync::http;
namespace json = rommsync::json;
namespace sync = rommsync::sync;

using harness::Fixture;

/// The first rom in the library. Every save has to hang off a real one: RomM
/// rejects a `rom_id` it does not know, and a save on the wrong rom would
/// negotiate against somebody else's history.
std::int64_t FirstRomId(http::HttpClient& client, const std::string& base, const Fixture& fixture) {
  const http::Result result =
      client.Send(harness::Authed(http::Method::kGet, base + "/api/roms?limit=1", fixture));
  if (!result.successful()) {
    return 0;
  }
  const json::ParseResult document = json::Parse(result.response.body);
  const json::Value* items = document.ok() ? document.value.Find("items") : nullptr;
  if (items == nullptr || items->size() == 0) {
    return 0;
  }
  const json::Value* id = items->elements()[0].Find("id");
  return id != nullptr && id->is_integer() ? id->integer() : 0;
}

// Reading a plan, and naming a slot nobody else is using, are the harness's
// (tests/harness.hpp): every scenario that negotiates needs them, and two
// copies of "find the operation for my slot" is two places for the same
// mistake.
using harness::Field;
using harness::OperationFor;
using harness::UniqueSlot;

/// The credentials as the engine holds them. `sync::Negotiate` takes a
/// `StoredToken` rather than a bearer string because that record is what carries
/// the `device_id` every sync call is scoped by.
auth::StoredToken TokenFor(const std::string& base, const Fixture& fixture) {
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  return token;
}

/// The client's own view of one save, as the sysmodule will build it.
sync::ClientSaveState LocalSave(std::int64_t rom_id, const std::string& slot, std::int64_t size) {
  sync::ClientSaveState save;
  save.rom_id = rom_id;
  save.file_name = "m2-1-probe.srm";
  save.slot = slot;
  save.emulator = "probe-emulator";
  save.file_size_bytes = size;
  // Deliberately ahead of the server's clock. RomM compares at second
  // granularity with a strict >, so a save written in the same second as the
  // sync record reads as unchanged -- which would make `understood` pass for
  // the wrong reason.
  save.updated_at = std::chrono::system_clock::now() + std::chrono::seconds{120};
  return save;
}

std::string Encode(rig::Checks& checks, const sync::SyncNegotiatePayload& payload) {
  const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
  checks.Expect(encoded.ok(), "the payload encodes: " + encoded.error.Describe());
  return encoded.body;
}

/// Rename one JSON key in an encoded body, the way a client that guessed the
/// field name would have written it.
std::string Rename(std::string body, const std::string& from, const std::string& to) {
  const std::string needle = "\"" + from + "\":";
  const std::size_t at = body.find(needle);
  if (at == std::string::npos) {
    return body;
  }
  return body.replace(at, needle.size(), "\"" + to + "\":");
}

/// The operation the server planned for `slot`, out of a parsed `SyncPlan`.
/// Negotiate also reports saves a scenario never mentioned, so every assertion
/// has to be scoped to the slot this run created.
const sync::SyncOperation* PlannedFor(const sync::SyncPlan& plan, const std::string& slot) {
  for (const sync::SyncOperation& operation : plan.operations) {
    if (operation.slot.has_value() && *operation.slot == slot) {
      return &operation;
    }
  }
  return nullptr;
}

// --- accepted ----------------------------------------------------------------

int Accepted(http::HttpClient& client, const std::string& base, const Fixture& fixture,
             std::int64_t rom_id) {
  rig::Checks checks;
  const std::string slot = UniqueSlot("m2-1-accepted");

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(LocalSave(rom_id, slot, 16));

  const http::Result result = harness::PostJson(client, base + "/api/sync/negotiate", fixture,
                                                Encode(checks, payload));
  checks.ExpectOk(result, "POST /api/sync/negotiate");
  checks.ExpectEq(result.response.status, 200,
                  "a live 5.2.0 accepts the body EncodeNegotiateRequest built");

  const json::ParseResult plan = json::Parse(result.response.body);
  if (!plan.ok()) {
    checks.Expect(false, "the plan parses: " + plan.error.Describe());
    return checks.failures();
  }
  const json::Value* session = plan.value.Find("session_id");
  checks.Expect(session != nullptr && session->is_integer(), "a session was opened");

  const json::Value* operation = OperationFor(plan.value, slot);
  if (operation == nullptr) {
    checks.Expect(false, "the plan carries an operation for this run's slot");
    return checks.failures();
  }
  // The echo is the point: a server that ignored these fields could not send
  // them back. This is what proves the names are the ones RomM reads.
  checks.ExpectEq(Field(*operation, "action"), std::string("upload"),
                  "a save the server does not have negotiates as an upload");
  checks.ExpectEq(Field(*operation, "file_name"), std::string("m2-1-probe.srm"),
                  "the file_name comes back");
  checks.ExpectEq(Field(*operation, "slot"), slot, "the slot comes back");
  checks.ExpectEq(Field(*operation, "emulator"), std::string("probe-emulator"),
                  "the emulator comes back");
  return checks.failures();
}

// --- required ----------------------------------------------------------------

int Required(http::HttpClient& client, const std::string& base, const Fixture& fixture,
             std::int64_t rom_id) {
  rig::Checks checks;
  const std::string slot = UniqueSlot("m2-1-required");

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(LocalSave(rom_id, slot, 16));

  const std::string wrong = Rename(Encode(checks, payload), "file_size_bytes", "size");
  const http::Result result =
      harness::PostJson(client, base + "/api/sync/negotiate", fixture, wrong);
  checks.ExpectOk(result, "POST /api/sync/negotiate with a renamed required field");
  checks.ExpectEq(result.response.status, 422,
                  "a missing required field is refused, not guessed at");
  checks.Expect(result.response.body.find("file_size_bytes") != std::string::npos,
                "and the refusal names the field the client got wrong: " + result.response.body);
  return checks.failures();
}

// --- understood --------------------------------------------------------------

int Understood(http::HttpClient& client, const std::string& base, const Fixture& fixture,
               std::int64_t rom_id) {
  rig::Checks checks;
  const std::string slot = UniqueSlot("m2-1-understood");
  const std::string bytes = "m2-1 probe save\n";
  const std::string path = rig::ScratchDir() + "/m2-1-probe.srm";
  if (!rig::WriteFile(path, bytes)) {
    std::cerr << "could not write " << path << "\n";
    return 1;
  }

  // Put a save on the server so there is something to compare against. The
  // `device_id` is what writes this device's sync record, which is the state the
  // second negotiation below falls back on when the hash goes missing.
  http::Request upload = harness::Authed(
      http::Method::kPost,
      base + "/api/saves?rom_id=" + std::to_string(rom_id) +
          "&emulator=probe-emulator&slot=" + slot + "&device_id=" + fixture.device_id,
      fixture);
  http::FormPart part;
  part.name = "saveFile";
  part.file_path = path;
  part.file_name = "m2-1-probe.srm";
  part.content_type = "application/octet-stream";
  upload.form.push_back(part);

  const http::Result stored = client.Send(upload);
  checks.ExpectOk(stored, "POST /api/saves");
  const json::ParseResult save = json::Parse(stored.response.body);
  if (!save.ok() || save.value.Find("id") == nullptr) {
    checks.Expect(false, "the save was stored: " + stored.response.body);
    return checks.failures();
  }
  const std::int64_t save_id = save.value.Find("id")->integer();

  // The server's own MD5 of those bytes, read back rather than recomputed: what
  // matters is that the client sends the string RomM will compare against.
  const std::string content_hash = Field(save.value, "content_hash");
  checks.ExpectEq(content_hash.size(), sync::kContentHashDigits,
                  "the server's content_hash is an MD5");

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  sync::ClientSaveState local = LocalSave(rom_id, slot, static_cast<std::int64_t>(bytes.size()));
  local.content_hash = content_hash;
  payload.saves.push_back(local);
  const std::string body = Encode(checks, payload);

  const http::Result matched =
      harness::PostJson(client, base + "/api/sync/negotiate", fixture, body);
  checks.ExpectOk(matched, "POST /api/sync/negotiate with the hash under its real name");
  const json::ParseResult matched_plan = json::Parse(matched.response.body);
  const json::Value* identical =
      matched_plan.ok() ? OperationFor(matched_plan.value, slot) : nullptr;
  if (identical == nullptr) {
    checks.Expect(false, "the plan carries an operation for this run's slot: " +
                             matched.response.body);
  } else {
    checks.ExpectEq(Field(*identical, "action"), std::string("no_op"),
                    "identical content is recognised -- nothing to do");
    checks.ExpectEq(Field(*identical, "reason"), std::string("Content is identical"),
                    "...on the hash, not on the timestamps");
  }

  // The same save, the same bytes, the same everything -- under the name a
  // client would guess. Note what does NOT happen: no 422, no error field, no
  // warning. Only the plan changes.
  const http::Result guessed = harness::PostJson(client, base + "/api/sync/negotiate", fixture,
                                                 Rename(body, "content_hash", "hash"));
  checks.ExpectOk(guessed, "POST /api/sync/negotiate with the hash misnamed");
  checks.ExpectEq(guessed.response.status, 200,
                  "a misnamed OPTIONAL field is accepted in silence -- this is the quiet half");
  const json::ParseResult guessed_plan = json::Parse(guessed.response.body);
  const json::Value* ignored = guessed_plan.ok() ? OperationFor(guessed_plan.value, slot) : nullptr;
  if (ignored == nullptr) {
    checks.Expect(false, "the misnamed plan carries an operation for this run's slot: " +
                             guessed.response.body);
  } else {
    checks.ExpectEq(Field(*ignored, "action"), std::string("upload"),
                    "and the plan is different: the same save re-uploads every tick");
  }

  harness::DeleteSave(client, base, fixture, save_id);
  std::filesystem::remove(path);
  return checks.failures();
}

// --- negotiates --------------------------------------------------------------
//
// The engine's own call, end to end: `sync::Negotiate` encodes the body, sends
// it, and hands back a `SyncPlan` rather than a string to scrape.

int Negotiates(http::HttpClient& client, const std::string& base, const Fixture& fixture,
         std::int64_t rom_id) {
  rig::Checks checks;
  const std::string slot = UniqueSlot("m2-4-plan");

  const sync::Negotiation negotiated =
      sync::Negotiate(client, TokenFor(base, fixture), {LocalSave(rom_id, slot, 16)});
  if (!negotiated.ok()) {
    checks.Expect(false, "the negotiation succeeded: " + negotiated.message);
    return checks.failures();
  }
  checks.ExpectEq(negotiated.attempts, 1, "one request, because nothing failed");
  checks.Expect(negotiated.plan.session_id > 0,
                "a session was opened, and its id is what `complete` is posted to");
  checks.Expect(negotiated.plan.warnings.empty(),
                "nothing in a healthy 5.2.0's plan is unrecognised");

  const sync::SyncOperation* operation = PlannedFor(negotiated.plan, slot);
  if (operation == nullptr) {
    checks.Expect(false, "the plan carries an operation for this run's slot");
    return checks.failures();
  }
  checks.Expect(operation->action == sync::Action::kUpload,
                "a save the server does not have is an upload");
  checks.Expect(operation->known_action, "...an action this build knows");
  checks.Expect(operation->reason == sync::Reason::kClientOnly,
                "and the reason classifies: " + operation->reason_text);
  checks.ExpectEq(operation->rom_id, rom_id, "the rom comes back");
  checks.ExpectEq(operation->file_name, std::string("m2-1-probe.srm"), "the file_name comes back");
  checks.Expect(operation->emulator.has_value() && *operation->emulator == "probe-emulator",
                "the emulator comes back");
  // The three that are null exactly when the server has no copy. A client that
  // read them as sentinels would be dereferencing a save id of 0 here.
  checks.Expect(!operation->save_id.has_value(), "save_id is empty: the server has no save yet");
  checks.Expect(!operation->server_updated_at.has_value(), "and no timestamp for one");
  checks.Expect(!operation->server_content_hash.has_value(), "and no hash for one");
  checks.Expect(negotiated.plan.total_upload >= 1, "the server counts it as an upload");

  // A record with no device id never reaches the wire: every sync call is
  // scoped by a device, and an unscoped negotiation reports every save as a
  // first encounter.
  auth::StoredToken anonymous = TokenFor(base, fixture);
  anonymous.device_id.clear();
  const sync::Negotiation refused = sync::Negotiate(client, anonymous, {});
  checks.Expect(refused.error == sync::NegotiateError::kNotRegistered,
                "a token naming no device is refused before anything is sent");
  checks.ExpectEq(refused.attempts, 0, "...and costs no request");
  checks.Expect(sync::NeedsPairing(refused.error), "the remedy is to pair again");

  // And a save the engine could not express faithfully stops the call rather
  // than producing a plan that looks complete.
  sync::ClientSaveState unmatched = LocalSave(rom_id, slot, 16);
  unmatched.rom_id = 0;
  const sync::Negotiation unusable =
      sync::Negotiate(client, TokenFor(base, fixture), {unmatched});
  checks.Expect(unusable.error == sync::NegotiateError::kUnusablePayload,
                "a save that matched no rom is not negotiated");
  checks.ExpectEq(unusable.attempts, 0, "...and costs no request either");
  checks.Expect(unusable.message.find("rom_id") != std::string::npos,
                "and the reason names the field: " + unusable.message);
  // An index into a vector this call built is not something anyone can go and
  // look at, and this failure repeats on every tick until the save is fixed.
  checks.Expect(unusable.message.find(slot) != std::string::npos,
                "...and which save, by the key it pairs on: " + unusable.message);
  return checks.failures();
}

// --- discovers ---------------------------------------------------------------
//
// The read-only negotiation: an EMPTY `saves` array is how a client asks what it
// is missing, and it is also the only operation where every nullable field is
// filled in -- so it is where "reads every operation field" is actually
// checkable.

int Discovers(http::HttpClient& client, const std::string& base, const Fixture& fixture,
              std::int64_t rom_id) {
  rig::Checks checks;
  const std::string slot = UniqueSlot("m2-4-discovers");
  const std::string name = "m2-4-discovers.srm";
  const std::string path = rig::ScratchDir() + "/" + name;
  if (!rig::WriteFile(path, "m2-4 server-only save\n")) {
    std::cerr << "could not write " << path << "\n";
    return 1;
  }

  // No `device_id`: this device has never synced this save, which is what makes
  // the server report it at all.
  harness::Save server;
  if (!harness::UploadSave(client, base, fixture, rom_id, slot, "probe-emulator", path, name,
                           /*with_device=*/false, &server)) {
    checks.Expect(false, "the server-only save was stored");
    std::filesystem::remove(path);
    return checks.failures();
  }

  const sync::Negotiation negotiated =
      sync::Negotiate(client, TokenFor(base, fixture), /*saves=*/{});
  if (!negotiated.ok()) {
    checks.Expect(false, "an empty saves array is a legitimate negotiation: " + negotiated.message);
    harness::DeleteSave(client, base, fixture, server.id);
    std::filesystem::remove(path);
    return checks.failures();
  }

  const sync::SyncOperation* operation = PlannedFor(negotiated.plan, slot);
  if (operation == nullptr) {
    checks.Expect(false, "reporting nothing still returns the save this device is missing");
    harness::DeleteSave(client, base, fixture, server.id);
    std::filesystem::remove(path);
    return checks.failures();
  }

  checks.Expect(operation->action == sync::Action::kDownload,
                "a save the client never mentioned comes back as a download");
  checks.Expect(operation->reason == sync::Reason::kServerOnly,
                "and the reason says why: " + operation->reason_text);
  checks.ExpectEq(operation->rom_id, rom_id, "rom_id");
  checks.Expect(operation->save_id.has_value() && *operation->save_id == server.id,
                "save_id names the row the bytes are fetched from");
  // RomM stamps a slot upload with a datetime tag, so this is NOT the name that
  // was uploaded -- which is the trap the field comment in sync.hpp is about.
  checks.ExpectEq(operation->file_name, server.file_name, "file_name is the SERVER's name");
  checks.Expect(operation->file_name != name,
                "...which is not the name this scenario uploaded: " + operation->file_name);
  checks.Expect(operation->emulator.has_value() && *operation->emulator == "probe-emulator",
                "emulator");
  checks.Expect(operation->server_content_hash.has_value() &&
                    *operation->server_content_hash == server.content_hash,
                "the server's MD5 comes back, so the client can compare before overwriting");
  checks.Expect(operation->server_updated_at.has_value() &&
                    !operation->server_updated_at->empty(),
                "and the server's timestamp, for what a human is shown on a conflict");
  checks.Expect(negotiated.plan.total_download >= 1, "the server counts it as a download");

  harness::DeleteSave(client, base, fixture, server.id);
  std::filesystem::remove(path);
  return checks.failures();
}

// --- revoked -----------------------------------------------------------------

int Revoked(http::HttpClient& client, const std::string& base, const Fixture& fixture) {
  rig::Checks checks;

  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"status","status":401,"path":"/api/sync/negotiate"})");

    sync::NegotiateOptions options;
    // A wait that would be a failure if it were called: a 401 is not something
    // to retry, and a client that did would retry forever -- `expires_at` is
    // null, so there is nothing that becomes valid again on its own.
    options.max_attempts = 3;
    options.wait = [&checks](std::chrono::milliseconds) {
      checks.Expect(false, "a revoked token is never retried");
    };

    const sync::Negotiation denied =
        sync::Negotiate(client, TokenFor(base, fixture), /*saves=*/{}, options);
    checks.Expect(denied.error == sync::NegotiateError::kUnauthorized,
                  std::string("a 401 is a revoked token, not a parse failure -- got ") +
                      sync::ToString(denied.error) + ": " + denied.message);
    checks.ExpectEq(denied.attempts, 1, "and it is not retried");
    checks.Expect(!sync::ShouldRetry(denied.error), "...because nothing about it will change");
    checks.Expect(sync::NeedsPairing(denied.error), "the remedy is to pair this console again");

    // The tick changed nothing: there is no plan to execute, not an empty one.
    checks.Expect(denied.plan.operations.empty(), "no operations came back");
    checks.ExpectEq(denied.plan.session_id, std::int64_t{0},
                    "and no session was opened, so there is nothing to complete");
    checks.Expect(denied.message.find(fixture.token) == std::string::npos,
                  "and the message does not quote the token back");
  }

  // The fault disarmed itself. What the client must NOT have concluded is that
  // the pairing is dead: one 401 from something in front of RomM is a bad
  // minute, and shredding `token.dat` over it sends the user to a pairing screen
  // for a proxy hiccup. `NeedsPairing` classifies; it does not act.
  const sync::Negotiation recovered =
      sync::Negotiate(client, TokenFor(base, fixture), /*saves=*/{});
  checks.Expect(recovered.ok(),
                "the same token still works -- one 401 is not a verdict on the pairing: " +
                    recovered.message);
  return checks.failures();
}

// --- truncated ---------------------------------------------------------------
//
// The quiet failure: a clean, short, plausible body that no transport can fault.
// Half a plan that parsed would be a plan with saves missing from it, and a plan
// with saves missing from it looks exactly like a device that is already in sync.

int Truncated(http::HttpClient& client, const std::string& base, const Fixture& fixture) {
  rig::Checks checks;

  {
    harness::Fault fault(
        checks, client, base,
        R"({"mode":"truncate","bytes":40,"path":"/api/sync/negotiate"})");

    sync::NegotiateOptions options;
    options.max_attempts = 3;
    options.wait = [&checks](std::chrono::milliseconds) {
      checks.Expect(false, "a body this client cannot read is not something to hammer");
    };

    const sync::Negotiation cut =
        sync::Negotiate(client, TokenFor(base, fixture), /*saves=*/{}, options);
    checks.Expect(cut.error == sync::NegotiateError::kMalformed,
                  std::string("a truncated plan is a json error, not a plan -- got ") +
                      sync::ToString(cut.error) + ": " + cut.message);
    checks.Expect(cut.plan.operations.empty(), "and yields no plan");
    checks.ExpectEq(cut.plan.session_id, std::int64_t{0}, "not even a session id");
    checks.ExpectEq(cut.attempts, 1, "and is not retried");
    // The message is the point: a named error says where the body stopped
    // making sense, which is the difference between this and a silent short plan.
    checks.Expect(cut.message.find("offset") != std::string::npos,
                  "the failure says where it gave up: " + cut.message);
  }

  const sync::Negotiation whole =
      sync::Negotiate(client, TokenFor(base, fixture), /*saves=*/{});
  checks.Expect(whole.ok(), "the next tick reads a whole plan: " + whole.message);
  return checks.failures();
}

// --- stalled -----------------------------------------------------------------
//
// A server that accepts the connection and then says nothing. One tick must be
// lost, not the sysmodule: the request times out, the retry backs off, and the
// backoff doubles.

int Stalled(http::HttpClient& client, const std::string& base, const Fixture& fixture) {
  rig::Checks checks;
  std::vector<std::chrono::milliseconds> waits;

  // Nothing left armed by an earlier scenario, or this one's first request is
  // damaged by somebody else's fault and the counts below mean nothing.
  harness::ExpectDisarmed(checks, client, base, "nothing is armed before this scenario");

  {
    // Two stalls, so a second retry is available to show the doubling. `claim`
    // counts a matching request before it sleeps, so a client that gave up still
    // spent one of the two.
    harness::Fault fault(
        checks, client, base,
        R"({"mode":"stall","seconds":6,"path":"/api/sync/negotiate","count":2})");

    sync::NegotiateOptions options;
    options.timeout = std::chrono::milliseconds{2'000};
    options.max_attempts = 3;
    options.backoff = std::chrono::milliseconds{50};
    options.max_backoff = std::chrono::milliseconds{1'000};
    // Injected rather than slept: a test that had to spend the backoff to prove
    // there was one is a test nobody runs, and a sysmodule may park on its own
    // primitive anyway.
    options.wait = [&waits](std::chrono::milliseconds delay) { waits.push_back(delay); };

    const sync::Negotiation negotiated =
        sync::Negotiate(client, TokenFor(base, fixture), /*saves=*/{}, options);
    checks.Expect(negotiated.ok(),
                  "a later attempt reaches the real server: " + negotiated.message);
    checks.Expect(negotiated.plan.session_id > 0, "the plan that came back is a real one");

    // Asserted as properties rather than as exact counts, and deliberately so:
    // the proxy's abandoned stall thread sleeps out the rest of its delay and
    // then answers, so how many of the two stalls a client actually meets is a
    // race with its own timeout -- `pair.retry` has the same one, and it is the
    // proxy's, not the engine's. What the engine owes is that a stall is
    // retried and that the wait doubles, and that is what is checked.
    checks.Expect(negotiated.attempts >= 2,
                  "a stall is retried rather than abandoned -- attempts: " +
                      std::to_string(negotiated.attempts));
    checks.ExpectEq(waits.size(), static_cast<std::size_t>(negotiated.attempts - 1),
                    "every retry waited first");
    std::chrono::milliseconds expected{50};
    std::chrono::milliseconds total{0};
    for (const std::chrono::milliseconds delay : waits) {
      checks.ExpectEq(delay.count(), expected.count(), "the backoff doubles per retry");
      total += delay;
      expected = expected * 2 > options.max_backoff ? options.max_backoff : expected * 2;
    }
    checks.ExpectEq(negotiated.waited.count(), total.count(),
                    "and the total is reported, so a caller can see a slow tick");
  }

  // The classification a stall lands on, checked separately from the retry: a
  // timeout says nothing about the pairing, so it must not send anyone to a
  // pairing screen.
  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"stall","seconds":6,"path":"/api/sync/negotiate"})");
    sync::NegotiateOptions once;
    once.timeout = std::chrono::milliseconds{2'000};
    // One attempt because the caller said one, not because no wait was injected:
    // a null `wait` is the default sleep, so `max_attempts` is the only field
    // that answers "how many requests may this cost".
    once.max_attempts = 1;
    const sync::Negotiation lost =
        sync::Negotiate(client, TokenFor(base, fixture), /*saves=*/{}, once);
    checks.Expect(lost.error == sync::NegotiateError::kUnreachable,
                  std::string("a stall is unreachable, not a verdict -- got ") +
                      sync::ToString(lost.error));
    checks.Expect(sync::ShouldRetry(lost.error), "and it is worth retrying");
    checks.Expect(!sync::NeedsPairing(lost.error), "but never worth re-pairing over");
    checks.ExpectEq(lost.attempts, 1, "a budget of one is spent as one");
    checks.ExpectEq(lost.waited.count(), std::int64_t{0}, "and nothing was waited");
  }
  return checks.failures();
}

// --- refused -----------------------------------------------------------------
//
// The two refusals whose whole point is that they are NOT "the network": a
// device deleted in RomM's web UI, and sync switched off for it there. Each has
// a remedy the other two classes of failure would send the user away from, so
// each is worth a branch and a test. Both bodies are RomM's own, replayed
// through the proxy: a healthy fixture cannot be made to answer either on demand
// without deleting this worktree's device or toggling its settings, and the
// point being checked is the classification, not that RomM can say it.

int Refused(http::HttpClient& client, const std::string& base, const Fixture& fixture) {
  rig::Checks checks;

  // Built with `json::Quote` rather than typed out: the body is a JSON document
  // nested inside the fault spec's JSON, and hand-escaping that is how a spec
  // becomes a 400 the proxy never applies -- a scenario that then runs clean and
  // passes having exercised nothing.
  const auto spec = [](int status, const std::string& detail) {
    std::string fault = "{\"mode\":\"status\",\"status\":" + std::to_string(status) +
                        ",\"path\":\"/api/sync/negotiate\"";
    if (!detail.empty()) {
      fault += ",\"body\":" + json::Quote("{\"detail\":" + json::Quote(detail) + "}");
    }
    return fault + "}";
  };

  struct Case {
    const char* what;
    int status;
    const char* detail;  ///< RomM's own `detail` string; empty for no body
    sync::NegotiateError error;
    bool retry;
    bool repair;
  };

  const Case cases[] = {
      {"a device deleted in RomM's web UI", 404, "Device with ID abc not found",
       sync::NegotiateError::kNoSuchDevice, false, true},
      // Not a revocation, and not reported as one: RomM approves what the user
      // ticked, so this is a scope missing from a pairing that otherwise works.
      // Telling that user their token was revoked sends them looking for
      // something that did not happen (docs/AUTH.md#scopes-to-request).
      {"a scope the user did not approve", 403, "Not enough permissions",
       sync::NegotiateError::kForbidden, false, true},
      // FastAPI's own 404, which is what a `server_url` pointing at something
      // that is not this RomM answers. Reading it as "your device was deleted"
      // would discard a working token over a typo in a URL.
      {"a 404 from something that is not RomM", 404, "Not Found",
       sync::NegotiateError::kRejected, false, false},
      {"the user's own sync switch, turned off", 400, "Sync is disabled for this device",
       sync::NegotiateError::kSyncDisabled, false, false},
      {"some other 400", 400, "something else entirely", sync::NegotiateError::kRejected, false,
       false},
      {"a server having a bad minute", 503, "", sync::NegotiateError::kServerError, true, false},
      // A rate limiter's answer is "not now", so it must not land somewhere that
      // neither retries nor re-pairs -- that would wedge the tick until reboot.
      {"a rate limiter", 429, "", sync::NegotiateError::kServerError, true, false},
  };

  for (const Case& scenario : cases) {
    harness::Fault fault(checks, client, base, spec(scenario.status, scenario.detail));
    sync::NegotiateOptions options;
    // One attempt, so a retryable case does not spend the fault twice and turn
    // into whatever the real server says next.
    options.max_attempts = 1;
    const sync::Negotiation refused =
        sync::Negotiate(client, TokenFor(base, fixture), /*saves=*/{}, options);
    checks.Expect(refused.error == scenario.error,
                  std::string(scenario.what) + " is " + sync::ToString(scenario.error) +
                      ", got " + sync::ToString(refused.error) + ": " + refused.message);
    checks.ExpectEq(sync::ShouldRetry(refused.error), scenario.retry,
                    std::string("...retryable, for ") + scenario.what);
    checks.ExpectEq(sync::NeedsPairing(refused.error), scenario.repair,
                    std::string("...worth re-pairing over, for ") + scenario.what);
    checks.Expect(refused.plan.operations.empty(),
                  std::string("...and no plan came back: ") + scenario.what);
    checks.ExpectEq(refused.plan.session_id, std::int64_t{0},
                    std::string("...nor a session: ") + scenario.what);
  }

  harness::ExpectDisarmed(checks, client, base, "every fault disarmed itself");
  const sync::Negotiation recovered =
      sync::Negotiate(client, TokenFor(base, fixture), /*saves=*/{});
  checks.Expect(recovered.ok(), "and the real server still answers: " + recovered.message);
  return checks.failures();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "accepted";
  const std::string base = rig::BaseUrl();

  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);

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
  const std::int64_t rom_id = FirstRomId(*client, base, fixture);
  if (rom_id == 0) {
    std::cerr << "the fixture library holds no roms; nothing to hang a save off\n"
                 "  scan it with: ./.venv/bin/python server/testing/provision.py\n";
    return rig::kSkip;
  }

  int failures = 0;
  if (scenario == "accepted") {
    failures = Accepted(*client, base, fixture, rom_id);
  } else if (scenario == "required") {
    failures = Required(*client, base, fixture, rom_id);
  } else if (scenario == "understood") {
    failures = Understood(*client, base, fixture, rom_id);
  } else if (scenario == "negotiates") {
    failures = Negotiates(*client, base, fixture, rom_id);
  } else if (scenario == "discovers") {
    failures = Discovers(*client, base, fixture, rom_id);
  } else if (scenario == "revoked") {
    failures = Revoked(*client, base, fixture);
  } else if (scenario == "truncated") {
    failures = Truncated(*client, base, fixture);
  } else if (scenario == "stalled") {
    failures = Stalled(*client, base, fixture);
  } else if (scenario == "refused") {
    failures = Refused(*client, base, fixture);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (failures == 0) {
    std::cout << "sync." << scenario << " ok against " << base << "\n";
  }
  return failures == 0 ? 0 : 1;
}
