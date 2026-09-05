// The negotiate PLAN against the real captured responses.
//
// The bodies here are not typed out from the docs: they are read from
// server/contract/captures/, the unedited replies a live RomM 5.2.0 sent
// (issue M0-4), which `contract.captures` re-checks against a running server.
// So this keeps `SyncPlan` honest against those bytes, the way `auth.shapes`
// does for the pairing responses.
//
// Two things the captures cannot cover, and that are checked here anyway:
//
//   - **The other eight reasons.** Only five of the thirteen strings 5.2.0 can
//     emit appear in a capture, so the rest are pinned against the table in
//     docs/API_CONTRACT.md, in both directions -- a `reason` this client stopped
//     recognising is a save it would silently take no action on.
//   - **A server that moved.** An `action` this build does not know cannot be
//     produced by a healthy 5.2.0 at all, and it is the one case where the
//     default branch is what overwrites a save. It is downgraded to `no_op`,
//     reported, and checked here on a hand-built body -- the only shape in this
//     file that no server sent.
//
// No network, so this never skips.
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/sync.hpp"

namespace auth = rommsync::auth;
namespace sync = rommsync::sync;

namespace {

std::string ReadCapture(checks::Checks& c, const std::string& name) {
  const std::string path = std::string(ROMMSYNC_CAPTURES_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  c.Expect(!body.empty(), "capture " + name + " is readable at " + path);
  return body;
}

/// Parse a capture and hand back its single operation, or nullptr with the
/// reason recorded. Never dereferences an empty plan, so a capture that stopped
/// carrying an operation is a named failure rather than a crash.
const sync::SyncOperation* OnlyOperation(checks::Checks& c, const sync::SyncPlan& plan,
                                         const char* what) {
  if (plan.operations.size() != 1) {
    c.Expect(false, std::string(what) + " carries exactly one operation, got " +
                        std::to_string(plan.operations.size()));
    return nullptr;
  }
  return &plan.operations.front();
}

auth::Parsed<sync::SyncPlan> ParseCapture(checks::Checks& c, const char* name) {
  auth::Parsed<sync::SyncPlan> parsed = sync::ParseNegotiateResponse(ReadCapture(c, name));
  c.Expect(parsed.ok(), std::string("the ") + name + " capture parses: " + parsed.error.Describe());
  c.Expect(parsed.value.warnings.empty(),
           std::string("...with nothing this client did not understand in ") + name);
  return parsed;
}

/// The `upload` capture: the one where the server has nothing, so `save_id`,
/// `server_updated_at` and `server_content_hash` are all null. That combination
/// is why those three are `optional` rather than sentinels.
void UploadCapture(checks::Checks& c) {
  const auth::Parsed<sync::SyncPlan> parsed = ParseCapture(c, "sync-negotiate-upload.json");
  const sync::SyncPlan& plan = parsed.value;
  c.ExpectEq(plan.session_id, std::int64_t{138}, "session_id");
  c.ExpectEq(plan.total_upload, std::int64_t{1}, "total_upload");
  c.ExpectEq(plan.total_download, std::int64_t{0}, "total_download");
  c.ExpectEq(plan.total_conflict, std::int64_t{0}, "total_conflict");
  c.ExpectEq(plan.total_no_op, std::int64_t{0}, "total_no_op");

  const sync::SyncOperation* operation = OnlyOperation(c, plan, "the upload capture");
  if (operation == nullptr) {
    return;
  }
  c.Expect(operation->action == sync::Action::kUpload, "action is upload");
  c.Expect(operation->known_action, "...and it is an action this build knows");
  c.ExpectEq(operation->action_text, std::string("upload"), "the raw action is kept for the log");
  c.ExpectEq(operation->rom_id, std::int64_t{4}, "rom_id");
  c.Expect(!operation->save_id.has_value(),
           "save_id is empty for a save the server does not have -- not a zero");
  c.ExpectEq(operation->file_name, std::string("probe.srm"), "file_name");
  c.Expect(operation->slot.has_value() && *operation->slot == "probe-4bb29c6c-a", "slot");
  c.Expect(operation->emulator.has_value() && *operation->emulator == "probe-emulator",
           "emulator");
  c.Expect(operation->reason == sync::Reason::kClientOnly, "reason classifies");
  c.ExpectEq(operation->reason_text, std::string("Save exists on client but not on server"),
             "the raw reason is kept, for the sentence an overlay shows");
  c.Expect(!operation->server_updated_at.has_value(), "no server timestamp; there is no copy");
  c.Expect(!operation->server_content_hash.has_value(), "and no server hash");
}

/// The `download` capture: the mirror, where every nullable field is filled in.
void DownloadCapture(checks::Checks& c) {
  const auth::Parsed<sync::SyncPlan> parsed = ParseCapture(c, "sync-negotiate-download.json");
  const sync::SyncPlan& plan = parsed.value;
  c.ExpectEq(plan.session_id, std::int64_t{140}, "session_id");
  c.ExpectEq(plan.total_download, std::int64_t{1}, "total_download");

  const sync::SyncOperation* operation = OnlyOperation(c, plan, "the download capture");
  if (operation == nullptr) {
    return;
  }
  c.Expect(operation->action == sync::Action::kDownload, "action is download");
  c.Expect(operation->save_id.has_value() && *operation->save_id == 57, "save_id");
  // The server's name, datetime tag and all -- not the name the client sent.
  c.ExpectEq(operation->file_name, std::string("probe [2026-09-04_11-36-26].srm"),
             "file_name is the SERVER's");
  c.Expect(operation->reason == sync::Reason::kServerOnly, "reason classifies");
  c.Expect(operation->server_updated_at.has_value() &&
               *operation->server_updated_at == "2026-09-04T11:36:26+00:00",
           "the server's timestamp comes through as text");
  // Carried, not parsed: RomM spells the offset `+00:00` where this client
  // writes `Z`, and nothing here compares the two.
  c.Expect(operation->server_updated_at.has_value() &&
               operation->server_updated_at->find('Z') == std::string::npos,
           "...in the server's own spelling, not this client's");
  c.Expect(operation->server_content_hash.has_value() &&
               operation->server_content_hash->size() == sync::kContentHashDigits,
           "and the server's MD5");
}

/// Both conflicts, which are the pair a client must not collapse. `harness.conflict`
/// and `harness.same_timestamp` arrange each on a live server; these are the
/// bytes those arrangements produced.
void ConflictCaptures(checks::Checks& c) {
  const auth::Parsed<sync::SyncPlan> both = ParseCapture(c, "sync-negotiate-conflict.json");
  const sync::SyncOperation* first = OnlyOperation(c, both.value, "the conflict capture");
  const auth::Parsed<sync::SyncPlan> same =
      ParseCapture(c, "sync-negotiate-conflict-same-timestamp.json");
  const sync::SyncOperation* second =
      OnlyOperation(c, same.value, "the same-timestamp conflict capture");
  if (first == nullptr || second == nullptr) {
    return;
  }

  c.Expect(first->action == sync::Action::kConflict, "both sides changed is a conflict");
  c.Expect(second->action == sync::Action::kConflict, "so is the same-timestamp one");
  c.Expect(first->reason == sync::Reason::kBothChanged, "the history conflict classifies");
  c.Expect(second->reason == sync::Reason::kSameTimestampDifferentContent,
           "and the no-history one classifies too");
  // The whole point of carrying `reason` at all: a client that switched on
  // `action` alone would have one branch for two different situations, and the
  // second needs no sync history to reach.
  c.Expect(first->reason != second->reason,
           "the two conflict reasons are distinct, not one branch");
  c.ExpectEq(both.value.total_conflict, std::int64_t{1}, "and each is counted as a conflict");
  c.ExpectEq(same.value.total_conflict, std::int64_t{1}, "...both of them");
}

void NoOpCapture(checks::Checks& c) {
  const auth::Parsed<sync::SyncPlan> parsed = ParseCapture(c, "sync-negotiate-no-op.json");
  const sync::SyncOperation* operation = OnlyOperation(c, parsed.value, "the no-op capture");
  if (operation == nullptr) {
    return;
  }
  c.Expect(operation->action == sync::Action::kNoOp, "no_op, with the underscore, classifies");
  c.Expect(operation->known_action,
           "...as a KNOWN no-op -- which is what separates it from a downgraded one");
  c.Expect(operation->reason == sync::Reason::kContentIdentical, "on the hash, not the timestamps");
  c.ExpectEq(parsed.value.total_no_op, std::int64_t{1}, "total_no_op");
}

/// An empty plan is a fully-synced device, not an error.
void EmptyCapture(checks::Checks& c) {
  const auth::Parsed<sync::SyncPlan> parsed = ParseCapture(c, "sync-negotiate-empty.json");
  c.ExpectEq(parsed.value.session_id, std::int64_t{137}, "a session was still opened");
  c.Expect(parsed.value.operations.empty(), "no operations is a plan, not a failure");
  c.ExpectEq(parsed.value.total_upload + parsed.value.total_download +
                 parsed.value.total_conflict + parsed.value.total_no_op,
             std::int64_t{0}, "and the totals agree");
}

// --- the reason table ---------------------------------------------------------

/// The complete set 5.2.0 can emit, from
/// docs/API_CONTRACT.md#save-sync--negotiate--execute--complete. Typed out here
/// on purpose: this table and the one in core/src/sync_negotiate.cpp are the two
/// directions, and a reason that drifted in one of them is caught by the other.
struct ReasonCase {
  sync::Reason reason;
  sync::Action action;  ///< the action the docs pair this reason with
  const char* text;
};

const ReasonCase kReasons[] = {
    {sync::Reason::kClientOnly, sync::Action::kUpload,
     "Save exists on client but not on server"},
    {sync::Reason::kClientNewerNoHistory, sync::Action::kUpload,
     "Client save is newer (no sync history)"},
    {sync::Reason::kClientNewer, sync::Action::kUpload, "Client save is newer than last sync"},
    {sync::Reason::kServerOnly, sync::Action::kDownload,
     "Save exists on server but not on client"},
    {sync::Reason::kServerNewerNoHistory, sync::Action::kDownload,
     "Server save is newer (no sync history)"},
    {sync::Reason::kServerNewer, sync::Action::kDownload, "Server save is newer than last sync"},
    {sync::Reason::kServerChangedClientMissing, sync::Action::kDownload,
     "Server save updated since last sync, not present on client"},
    {sync::Reason::kBothChanged, sync::Action::kConflict, "Both sides changed since last sync"},
    {sync::Reason::kSameTimestampDifferentContent, sync::Action::kConflict,
     "Same timestamp but different content"},
    {sync::Reason::kContentIdentical, sync::Action::kNoOp, "Content is identical"},
    {sync::Reason::kNoChanges, sync::Action::kNoOp, "No changes since last sync"},
    {sync::Reason::kAppearIdentical, sync::Action::kNoOp, "Saves appear identical"},
    {sync::Reason::kUntracked, sync::Action::kNoOp, "Save is untracked on this device"},
};

/// A plan carrying one operation with the given action and reason, spelled the
/// way RomM spells them. Only used for the strings no capture holds.
std::string PlanWith(const std::string& action, const std::string& reason) {
  return "{\"session_id\":1,\"operations\":[{"
         "\"action\":\"" + action + "\",\"rom_id\":4,\"save_id\":57,"
         "\"file_name\":\"probe.srm\",\"slot\":\"a\",\"emulator\":\"retroarch\","
         "\"reason\":\"" + reason + "\","
         "\"server_updated_at\":null,\"server_content_hash\":null}],"
         "\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,\"total_no_op\":0}";
}

void EveryDocumentedReason(checks::Checks& c) {
  // A new enumerator added without a spelling beside it would classify as
  // `kUnrecognized` on a live plan and take a real save through the unknown
  // branch. Pinning the count is what makes that a red test rather than a
  // silent no-op months later.
  c.ExpectEq(static_cast<int>(sync::Reason::kUnrecognized), 0,
             "kUnrecognized is what a default-constructed operation carries");
  c.ExpectEq(static_cast<int>(sync::Reason::kUntracked), static_cast<int>(std::size(kReasons)),
             "the enum holds exactly the documented reasons and the unrecognised one");

  std::vector<std::string> slugs;
  for (const ReasonCase& scenario : kReasons) {
    c.Expect(sync::ClassifyReason(scenario.text) == scenario.reason,
             std::string("classifies: ") + scenario.text);
    c.ExpectEq(std::string(sync::ReasonText(scenario.reason)), std::string(scenario.text),
               std::string("...and spells it back: ") + scenario.text);

    const std::string slug = sync::ToString(scenario.reason);
    c.Expect(!slug.empty() && slug != "unrecognized",
             std::string("...and has a log slug: ") + scenario.text);
    c.Expect(std::find(slugs.begin(), slugs.end(), slug) == slugs.end(),
             "the log slug is unique: " + slug);
    slugs.push_back(slug);

    // Through a whole plan, not just the classifier: the action and the reason
    // the docs pair are the ones a real body carries together.
    const auth::Parsed<sync::SyncPlan> parsed =
        sync::ParseNegotiateResponse(PlanWith(sync::ToString(scenario.action), scenario.text));
    if (!parsed.ok()) {
      c.Expect(false, std::string("a plan carrying it parses: ") + parsed.error.Describe());
      continue;
    }
    const sync::SyncOperation* operation = OnlyOperation(c, parsed.value, scenario.text);
    if (operation == nullptr) {
      continue;
    }
    c.Expect(operation->reason == scenario.reason, std::string("read off a plan: ") + scenario.text);
    c.Expect(operation->action == scenario.action,
             std::string("...with the action the docs pair it with: ") + scenario.text);
    c.Expect(parsed.value.warnings.empty(),
             std::string("...and nothing to warn about: ") + scenario.text);
  }

  c.Expect(sync::ClassifyReason("") == sync::Reason::kUnrecognized, "an empty reason is unknown");
  c.Expect(sync::ClassifyReason("Content is identical.") == sync::Reason::kUnrecognized,
           "and so is one that only nearly matches");
  c.ExpectEq(std::string(sync::ReasonText(sync::Reason::kUnrecognized)), std::string(),
             "kUnrecognized spells nothing -- it is the absence of a reason");
}

void EveryAction(checks::Checks& c) {
  struct Case {
    sync::Action action;
    const char* text;
  };
  const Case kActions[] = {
      {sync::Action::kUpload, "upload"},
      {sync::Action::kDownload, "download"},
      {sync::Action::kConflict, "conflict"},
      {sync::Action::kNoOp, "no_op"},
  };
  for (const Case& scenario : kActions) {
    bool recognized = false;
    c.Expect(sync::ClassifyAction(scenario.text, &recognized) == scenario.action,
             std::string("classifies the action ") + scenario.text);
    c.Expect(recognized, std::string("...as a known one: ") + scenario.text);
    c.ExpectEq(std::string(sync::ToString(scenario.action)), std::string(scenario.text),
               std::string("...and spells it back: ") + scenario.text);
  }

  // The near-misses a client writes by hand. `noop` is the one worth naming: it
  // is what an obvious guess produces, and it would send every no-op through the
  // unknown branch.
  for (const char* unknown : {"noop", "no-op", "NO_OP", "Upload", "delete", ""}) {
    bool recognized = true;
    c.Expect(sync::ClassifyAction(unknown, &recognized) == sync::Action::kNoOp,
             std::string("an unknown action is a no_op: ") + unknown);
    c.Expect(!recognized, std::string("...and says it was not recognised: ") + unknown);
  }
}

/// An action from a RomM newer than this client. The default branch is the one
/// that can overwrite a save, so it must be the one that does nothing -- and it
/// must say so.
void AnUnknownActionIsANoOp(checks::Checks& c) {
  const auth::Parsed<sync::SyncPlan> parsed =
      sync::ParseNegotiateResponse(PlanWith("merge", "Content is identical"));
  if (!parsed.ok()) {
    c.Expect(false, "a plan with an unknown action still parses: " + parsed.error.Describe());
    return;
  }
  const sync::SyncOperation* operation = OnlyOperation(c, parsed.value, "the unknown-action plan");
  if (operation == nullptr) {
    return;
  }
  c.Expect(operation->action == sync::Action::kNoOp, "an unknown action is treated as no_op");
  c.Expect(!operation->known_action, "...and is flagged, so it is not mistaken for a real no-op");
  c.ExpectEq(operation->action_text, std::string("merge"), "...with the string kept for the log");
  c.ExpectEq(parsed.value.warnings.size(), std::size_t{1}, "and it is logged");
  c.Expect(parsed.value.warnings.front().find("merge") != std::string::npos,
           "the log line names the action: " + parsed.value.warnings.front());
  c.Expect(parsed.value.warnings.front().find("operations[0]") != std::string::npos,
           "...and which operation it was: " + parsed.value.warnings.front());

  // The rest of the operation is still read. A downgraded action is a server
  // that moved, not a body to throw away -- the plan's other entries are fine.
  c.ExpectEq(operation->rom_id, std::int64_t{4}, "the rest of the operation still reads");
  c.Expect(operation->reason == sync::Reason::kContentIdentical, "...including the reason");
}

/// The server's `content_hash` held to the same shape the client's own is.
///
/// RomM stores whatever any client sent it, so a save uploaded by some other
/// tool can come back carrying a SHA1 or an uppercase digest -- which compares
/// equal to nothing, so that save negotiates as changed on every tick forever,
/// with no other symptom. Reported rather than refused: it is one save's
/// problem, not the plan's.
void AServerHashThatIsNotAnMd5IsReported(checks::Checks& c) {
  const auto plan = [](const char* hash) {
    return std::string(
               "{\"session_id\":1,\"operations\":[{"
               "\"action\":\"download\",\"rom_id\":4,\"save_id\":57,"
               "\"file_name\":\"probe.srm\",\"slot\":null,\"emulator\":null,"
               "\"reason\":\"Content is identical\","
               "\"server_updated_at\":null,\"server_content_hash\":") +
           hash +
           "}],\"total_upload\":0,\"total_download\":1,\"total_conflict\":0,"
           "\"total_no_op\":0}";
  };

  struct Case {
    const char* what;
    const char* hash;
    bool warns;
  };
  const Case cases[] = {
      {"a real MD5", "\"abd8fff93894e8112c7dd17386e54a5f\"", false},
      {"no hash at all", "null", false},
      {"a SHA1, which the rom schema uses and saves do not",
       "\"da39a3ee5e6b4b0d3255bfef95601890afd80709\"", true},
      {"an uppercase digest of the same bytes", "\"ABD8FFF93894E8112C7DD17386E54A5F\"", true},
      {"32 characters that are not hex", "\"zzd8fff93894e8112c7dd17386e54a5f\"", true},
  };

  for (const Case& scenario : cases) {
    const auth::Parsed<sync::SyncPlan> parsed = sync::ParseNegotiateResponse(plan(scenario.hash));
    if (!parsed.ok()) {
      c.Expect(false, std::string("the plan still parses, for ") + scenario.what + ": " +
                          parsed.error.Describe());
      continue;
    }
    c.ExpectEq(parsed.value.warnings.size(), scenario.warns ? std::size_t{1} : std::size_t{0},
               std::string("warned about: ") + scenario.what);
    if (scenario.warns) {
      c.Expect(parsed.value.warnings.front().find("MD5") != std::string::npos,
               "...and says what it should have been: " + parsed.value.warnings.front());
    }
  }
}

void AnUnknownReasonIsReported(checks::Checks& c) {
  const auth::Parsed<sync::SyncPlan> parsed =
      sync::ParseNegotiateResponse(PlanWith("download", "Server save smells fresher"));
  if (!parsed.ok()) {
    c.Expect(false, "a plan with an unknown reason still parses: " + parsed.error.Describe());
    return;
  }
  const sync::SyncOperation* operation = OnlyOperation(c, parsed.value, "the unknown-reason plan");
  if (operation == nullptr) {
    return;
  }
  // The action is still obeyed: the server decided, and the reason is its
  // explanation, not its decision.
  c.Expect(operation->action == sync::Action::kDownload, "the action is still obeyed");
  c.Expect(operation->reason == sync::Reason::kUnrecognized, "the reason is not guessed at");
  c.ExpectEq(operation->reason_text, std::string("Server save smells fresher"),
             "and the sentence is kept");
  c.ExpectEq(parsed.value.warnings.size(), std::size_t{1}, "it is logged");
}

/// The server's `file_name` is echoed out of its database and must never be
/// joined into an SD path. It is not refused -- that would fail a whole tick
/// over one save's name -- but it does not pass unremarked either.
void APathWhereANameBelongsIsFlagged(checks::Checks& c) {
  const std::string body =
      "{\"session_id\":1,\"operations\":[{"
      "\"action\":\"download\",\"rom_id\":4,\"save_id\":57,"
      "\"file_name\":\"../../config/rommsync/token.dat\",\"slot\":null,\"emulator\":null,"
      "\"reason\":\"Content is identical\","
      "\"server_updated_at\":null,\"server_content_hash\":null}],"
      "\"total_upload\":0,\"total_download\":1,\"total_conflict\":0,\"total_no_op\":0}";
  const auth::Parsed<sync::SyncPlan> parsed = sync::ParseNegotiateResponse(body);
  if (!parsed.ok()) {
    c.Expect(false, "the plan still parses: " + parsed.error.Describe());
    return;
  }
  c.ExpectEq(parsed.value.warnings.size(), std::size_t{1},
             "a file_name that is not one path component is reported");
  c.Expect(parsed.value.warnings.front().find("path component") != std::string::npos,
           "...and says why: " + parsed.value.warnings.front());
  // A null slot and a null emulator are legitimate on the way back, too.
  const sync::SyncOperation* operation = OnlyOperation(c, parsed.value, "the odd-name plan");
  if (operation != nullptr) {
    c.Expect(!operation->slot.has_value(), "a null slot reads as absent");
    c.Expect(!operation->emulator.has_value(), "so does a null emulator");
  }
}

// --- refusals -----------------------------------------------------------------

/// Every body that must NOT come back as a plan. Each one is a response the
/// engine could otherwise act on: a truncated plan looks like a short one, and a
/// short one looks like a device that is already in sync.
void RefusesWhatItCannotRead(checks::Checks& c) {
  const std::string good = ReadCapture(c, "sync-negotiate-download.json");

  struct Case {
    const char* what;
    const char* field;  ///< the field the error must name; empty for a syntax error
    std::string body;
  };

  const Case cases[] = {
      {"a truncated body", "", good.substr(0, good.size() / 2)},
      {"a body cut inside a string", "", good.substr(0, 120)},
      {"an empty body", "", ""},
      // A JSON array parses; it is just not the object a plan is. The reader
      // names the context rather than a field, so the field is left blank here.
      {"a plan that is an array", "", "[]"},
      {"a plan that is a bare null", "", "null"},
      {"a plan with no session", "session_id",
       "{\"operations\":[],\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,"
       "\"total_no_op\":0}"},
      {"a session id that is not a number", "session_id",
       "{\"session_id\":\"1\",\"operations\":[],\"total_upload\":0,\"total_download\":0,"
       "\"total_conflict\":0,\"total_no_op\":0}"},
      {"a session id of zero", "session_id",
       "{\"session_id\":0,\"operations\":[],\"total_upload\":0,\"total_download\":0,"
       "\"total_conflict\":0,\"total_no_op\":0}"},
      {"a plan with no operations field", "operations",
       "{\"session_id\":1,\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,"
       "\"total_no_op\":0}"},
      {"operations that are not an array", "operations",
       "{\"session_id\":1,\"operations\":{},\"total_upload\":0,\"total_download\":0,"
       "\"total_conflict\":0,\"total_no_op\":0}"},
      {"a plan with no totals", "total_upload",
       "{\"session_id\":1,\"operations\":[]}"},
      {"an operation missing its action", "operations[0].action",
       "{\"session_id\":1,\"operations\":[{\"rom_id\":4,\"save_id\":null,"
       "\"file_name\":\"a.srm\",\"slot\":null,\"emulator\":null,\"reason\":\"x\","
       "\"server_updated_at\":null,\"server_content_hash\":null}],"
       "\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,\"total_no_op\":0}"},
      {"an operation missing its save_id -- absent is not null", "operations[0].save_id",
       "{\"session_id\":1,\"operations\":[{\"action\":\"upload\",\"rom_id\":4,"
       "\"file_name\":\"a.srm\",\"slot\":null,\"emulator\":null,\"reason\":\"x\","
       "\"server_updated_at\":null,\"server_content_hash\":null}],"
       "\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,\"total_no_op\":0}"},
      {"a save_id written as a string", "operations[0].save_id",
       "{\"session_id\":1,\"operations\":[{\"action\":\"upload\",\"rom_id\":4,"
       "\"save_id\":\"57\",\"file_name\":\"a.srm\",\"slot\":null,\"emulator\":null,"
       "\"reason\":\"x\",\"server_updated_at\":null,\"server_content_hash\":null}],"
       "\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,\"total_no_op\":0}"},
      {"an operation naming no rom", "operations[0].rom_id",
       "{\"session_id\":1,\"operations\":[{\"action\":\"upload\",\"rom_id\":0,"
       "\"save_id\":null,\"file_name\":\"a.srm\",\"slot\":null,\"emulator\":null,"
       "\"reason\":\"x\",\"server_updated_at\":null,\"server_content_hash\":null}],"
       "\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,\"total_no_op\":0}"},
      {"a nameless save", "operations[0].file_name",
       "{\"session_id\":1,\"operations\":[{\"action\":\"upload\",\"rom_id\":4,"
       "\"save_id\":null,\"file_name\":\"\",\"slot\":null,\"emulator\":null,"
       "\"reason\":\"x\",\"server_updated_at\":null,\"server_content_hash\":null}],"
       "\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,\"total_no_op\":0}"},
      // Not an object at all: the reader names the context, so there is no field
      // to put behind the dot and `operations[0].` would name nothing.
      {"an operation that is not an object", "operations[0]",
       "{\"session_id\":1,\"operations\":[\"x\"],"
       "\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,\"total_no_op\":0}"},
      {"a blank slot, which is neither a slot nor archival", "operations[0].slot",
       "{\"session_id\":1,\"operations\":[{\"action\":\"upload\",\"rom_id\":4,"
       "\"save_id\":null,\"file_name\":\"a.srm\",\"slot\":\"\",\"emulator\":null,"
       "\"reason\":\"x\",\"server_updated_at\":null,\"server_content_hash\":null}],"
       "\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,\"total_no_op\":0}"},
  };

  for (const Case& scenario : cases) {
    const auth::Parsed<sync::SyncPlan> parsed = sync::ParseNegotiateResponse(scenario.body);
    c.Expect(!parsed.ok(), std::string("refused: ") + scenario.what);
    c.Expect(parsed.value.operations.empty(),
             std::string("...and carries no plan: ") + scenario.what);
    c.ExpectEq(parsed.value.session_id, std::int64_t{0},
               std::string("...not even a session: ") + scenario.what);
    if (*scenario.field != '\0') {
      c.ExpectEq(parsed.error.field, std::string(scenario.field),
                 std::string("...and names the field, for ") + scenario.what);
    }
    c.Expect(!parsed.error.Describe().empty(),
             std::string("...with something to log: ") + scenario.what);
  }

  // A body that is not an object at all names what was being read, since there
  // is no field to name.
  const auth::Parsed<sync::SyncPlan> array = sync::ParseNegotiateResponse("[]");
  c.Expect(array.error.Describe().find("negotiate response") != std::string::npos,
           "an array says what it was being read as: " + array.error.Describe());

  // The second operation, not the first: a plan is refused whole, so a body
  // whose *last* entry is unreadable must not come back as a partial plan of
  // the entries before it.
  const std::string second =
      "{\"session_id\":1,\"operations\":["
      "{\"action\":\"no_op\",\"rom_id\":4,\"save_id\":null,\"file_name\":\"a.srm\","
      "\"slot\":null,\"emulator\":null,\"reason\":\"Content is identical\","
      "\"server_updated_at\":null,\"server_content_hash\":null},"
      "{\"action\":\"download\",\"rom_id\":4,\"save_id\":null,\"file_name\":\"b.srm\","
      "\"slot\":null,\"emulator\":null,"
      "\"server_updated_at\":null,\"server_content_hash\":null}],"
      "\"total_upload\":0,\"total_download\":1,\"total_conflict\":0,\"total_no_op\":1}";
  const auth::Parsed<sync::SyncPlan> partial = sync::ParseNegotiateResponse(second);
  c.Expect(!partial.ok(), "one unreadable operation refuses the whole plan");
  c.ExpectEq(partial.error.field, std::string("operations[1].reason"),
             "...and says which entry, because the index is all that identifies it");
  c.Expect(partial.value.operations.empty(),
           "...and the readable entry before it is not handed back on its own");
}

}  // namespace

int main() {
  checks::Checks c;

  UploadCapture(c);
  DownloadCapture(c);
  ConflictCaptures(c);
  NoOpCapture(c);
  EmptyCapture(c);
  EveryDocumentedReason(c);
  EveryAction(c);
  AnUnknownActionIsANoOp(c);
  AnUnknownReasonIsReported(c);
  AServerHashThatIsNotAnMd5IsReported(c);
  APathWhereANameBelongsIsFlagged(c);
  RefusesWhatItCannotRead(c);

  if (c.failures() != 0) {
    std::cerr << c.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << "sync plan: the captures, the reason table and the refusals agree\n";
  return 0;
}
