// The encoded negotiate payload against a REAL RomM 5.2.0.
//
// `sync.payload` pins `ClientSaveState` to the committed OpenAPI snapshot, which
// is a document. These scenarios ask the running server, which is the only thing
// that can answer the question the snapshot cannot:
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
//
// `understood` puts a save on the fixture and deletes it again, the same way
// server/probe_contract.py's scenarios do. Nothing here calls
// `/api/sync/sessions/{id}/complete`, so no sync baseline is written.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>

#include "rig.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sync.hpp"

namespace {

namespace http = rommsync::http;
namespace json = rommsync::json;
namespace sync = rommsync::sync;

/// Everything the fixture provisioner minted for this worktree.
struct Fixture {
  std::string token;
  std::string device_id;
  std::int64_t rom_id = 0;
};

http::Request Authed(http::Method method, const std::string& url, const Fixture& fixture) {
  http::Request request;
  request.method = method;
  request.url = url;
  request.headers.push_back({"Authorization", "Bearer " + fixture.token});
  return request;
}

http::Result PostJson(http::HttpClient& client, const std::string& url, const Fixture& fixture,
                      std::string body) {
  http::Request request = Authed(http::Method::kPost, url, fixture);
  request.headers.push_back({"Content-Type", "application/json"});
  request.body = std::move(body);
  return client.Send(request);
}

/// The first rom in the library. Every save has to hang off a real one: RomM
/// rejects a `rom_id` it does not know, and a save on the wrong rom would
/// negotiate against somebody else's history.
std::int64_t FirstRomId(http::HttpClient& client, const std::string& base, const Fixture& fixture) {
  const http::Result result =
      client.Send(Authed(http::Method::kGet, base + "/api/roms?limit=1", fixture));
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

/// The operation the server planned for `slot`, or nullptr. Negotiate also
/// reports saves this test never mentioned -- anything the fixture device has no
/// history for -- so every assertion is scoped to the slot this run created.
const json::Value* OperationFor(const json::Value& plan, const std::string& slot) {
  const json::Value* operations = plan.Find("operations");
  if (operations == nullptr) {
    return nullptr;
  }
  for (const json::Value& operation : operations->elements()) {
    const json::Value* value = operation.Find("slot");
    if (value != nullptr && value->is_string() && value->string() == slot) {
      return &operation;
    }
  }
  return nullptr;
}

std::string Field(const json::Value& object, const char* key) {
  const json::Value* value = object.Find(key);
  return value != nullptr && value->is_string() ? value->string() : std::string();
}

/// A slot nobody else is using. RomM pairs saves on `(rom_id, slot)`, so a
/// constant would make one run's leftovers another run's sync history -- and a
/// run that failed before its cleanup does leave one behind. A timestamp alone
/// is not enough: `understood` finishes in a tenth of a second, so
/// `ctest --repeat until-fail:N` puts several runs inside the same second.
std::string UniqueSlot(const char* scenario) {
  std::random_device entropy;
  const std::int64_t now = sync::UnixSeconds(std::chrono::system_clock::now());
  return std::string("m2-1-") + scenario + "-" + std::to_string(now) + "-" +
         std::to_string(entropy());
}

/// The client's own view of one save, as the sysmodule will build it.
sync::ClientSaveState LocalSave(const Fixture& fixture, const std::string& slot,
                                std::int64_t size) {
  sync::ClientSaveState save;
  save.rom_id = fixture.rom_id;
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

// --- accepted ----------------------------------------------------------------

int Accepted(http::HttpClient& client, const std::string& base, const Fixture& fixture) {
  rig::Checks checks;
  const std::string slot = UniqueSlot("accepted");

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(LocalSave(fixture, slot, 16));

  const http::Result result =
      PostJson(client, base + "/api/sync/negotiate", fixture, Encode(checks, payload));
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

int Required(http::HttpClient& client, const std::string& base, const Fixture& fixture) {
  rig::Checks checks;
  const std::string slot = UniqueSlot("required");

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(LocalSave(fixture, slot, 16));

  const std::string wrong = Rename(Encode(checks, payload), "file_size_bytes", "size");
  const http::Result result = PostJson(client, base + "/api/sync/negotiate", fixture, wrong);
  checks.ExpectOk(result, "POST /api/sync/negotiate with a renamed required field");
  checks.ExpectEq(result.response.status, 422,
                  "a missing required field is refused, not guessed at");
  checks.Expect(result.response.body.find("file_size_bytes") != std::string::npos,
                "and the refusal names the field the client got wrong: " + result.response.body);
  return checks.failures();
}

// --- understood --------------------------------------------------------------

int Understood(http::HttpClient& client, const std::string& base, const Fixture& fixture) {
  rig::Checks checks;
  const std::string slot = UniqueSlot("understood");
  const std::string bytes = "m2-1 probe save\n";
  const std::string path = rig::ScratchDir() + "/m2-1-probe.srm";
  if (!rig::WriteFile(path, bytes)) {
    std::cerr << "could not write " << path << "\n";
    return 1;
  }

  // Put a save on the server so there is something to compare against. The
  // `device_id` is what writes this device's sync record, which is the state the
  // second negotiation below falls back on when the hash goes missing.
  http::Request upload = Authed(http::Method::kPost,
                                base + "/api/saves?rom_id=" + std::to_string(fixture.rom_id) +
                                    "&emulator=probe-emulator&slot=" + slot +
                                    "&device_id=" + fixture.device_id,
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
  sync::ClientSaveState local = LocalSave(fixture, slot, static_cast<std::int64_t>(bytes.size()));
  local.content_hash = content_hash;
  payload.saves.push_back(local);
  const std::string body = Encode(checks, payload);

  const http::Result matched = PostJson(client, base + "/api/sync/negotiate", fixture, body);
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
  const http::Result guessed = PostJson(client, base + "/api/sync/negotiate", fixture,
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

  const http::Result removed = PostJson(client, base + "/api/saves/delete", fixture,
                                        "{\"saves\":[" + std::to_string(save_id) + "]}");
  checks.ExpectOk(removed, "DELETE the save this scenario created");
  checks.ExpectEq(removed.response.status, 200, "the fixture is left as it was found");
  std::filesystem::remove(path);
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
  const std::string provisioned = rig::ReadFile(ROMMSYNC_FIXTURE_AUTH);
  fixture.token = rig::FixtureValue(provisioned, "ROMM_FIXTURE_TOKEN");
  fixture.device_id = rig::FixtureValue(provisioned, "ROMM_FIXTURE_DEVICE_ID");
  if (fixture.token.empty() || fixture.device_id.empty()) {
    std::cerr << "no fixture credentials in " ROMMSYNC_FIXTURE_AUTH
                 "\n  provision them with: ./.venv/bin/python server/testing/provision.py\n";
    return rig::kSkip;
  }
  fixture.rom_id = FirstRomId(*client, base, fixture);
  if (fixture.rom_id == 0) {
    std::cerr << "the fixture library holds no roms; nothing to hang a save off\n"
                 "  scan it with: ./.venv/bin/python server/testing/provision.py\n";
    return rig::kSkip;
  }

  int failures = 0;
  if (scenario == "accepted") {
    failures = Accepted(*client, base, fixture);
  } else if (scenario == "required") {
    failures = Required(*client, base, fixture);
  } else if (scenario == "understood") {
    failures = Understood(*client, base, fixture);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (failures == 0) {
    std::cout << "sync." << scenario << " ok against " << base << "\n";
  }
  return failures == 0 ? 0 : 1;
}
