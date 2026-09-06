// Step 3 of the sync loop, against the real RomM: the session closed with
// honest counts, and the baseline committed so the next tick is cheap.
//
// `execute.*` proves the client can act on a plan. These prove it can *account*
// for what it did and remember it -- which is the half nobody notices being
// wrong, because a lost baseline costs no save and no error message. It costs
// every save being re-hashed and re-uploaded on every tick, forever, and the
// only symptom is that RomM's history grows a new row for bytes it already had.
//
// Four scenarios need no server and must stay checked with docker stopped:
// `counts` is the accounting that turns an `ExecutionReport` into two integers,
// `parse` is the response read against the committed capture, `stamps` is the
// timestamp reader a baseline row's server half goes through, and `advance` is
// which operations may move a row and which may not.
//
// **Why the rig scenarios do their own negotiate rather than reusing
// `PlanFor`.** The one in test_sync_execute.cpp closes the session as soon as it
// has read the plan (issue #76), which is right there and wrong here: closing it
// is the thing under test. These leave it open and complete it themselves.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "harness.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync_execute.hpp"
#include "rommsync/sync_finish.hpp"

namespace {

namespace auth = rommsync::auth;
namespace crypto = rommsync::crypto;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace json = rommsync::json;
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

/// An `HttpClient` that fails the test if anything is sent through it.
///
/// The assertion for every refusal `CompleteSession` makes before it builds a
/// request: an unpaired token and a session id there is none of must not reach
/// the network at all.
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

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// One local save as the scanner would report it: the card's own mtime and
/// size, and the digest of the bytes that are actually there.
///
/// Built from a listing rather than from what the test just wrote, because the
/// point of the baseline is that the *next* stat matches it. A test that made
/// up an mtime would assert that `ContentHashFor` reuses a row describing a file
/// that never existed.
bool LocalSaveOnCard(rig::Checks& checks, fs::FileSystem& files, const Sandbox& sandbox,
                     std::int64_t rom_id, const std::string& file_name, const std::string& slot,
                     sync::ClientSaveState* out) {
  const std::string sd_path = SavePath(file_name);
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
    out->emulator = "m2-6";
    out->content_hash = crypto::Md5Hex(sandbox.Read(sd_path));
    out->updated_at = sync::Timestamp{} + std::chrono::seconds{entry.modified_unix};
    out->file_size_bytes = entry.size_bytes;
    return true;
  }
  checks.Expect(false, "the seeded save is on the card at " + sd_path);
  return false;
}

/// Negotiate, read the plan with the engine's own parser, and keep only the
/// operations for `slots`. **The session is left open**, because completing it
/// is what these scenarios are about.
///
/// Returns false with the reason already reported when there is no plan.
bool PlanFor(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const sync::SyncNegotiatePayload& payload,
             const std::vector<std::string>& slots, sync::SyncPlan* out) {
  const http::Result result = harness::Negotiate(checks, client, base, fixture, payload);
  if (result.response.status != 200) {
    checks.Expect(false, "the negotiation is answered: HTTP " +
                             std::to_string(result.response.status) + " " + result.response.body);
    return false;
  }
  const auth::Parsed<sync::SyncPlan> parsed = sync::ParseNegotiateResponse(result.response.body);
  if (!parsed.ok()) {
    checks.Expect(false, "the engine reads the plan: " + parsed.error.Describe());
    return false;
  }
  out->session_id = parsed.value.session_id;
  out->operations.clear();
  for (const sync::SyncOperation& operation : parsed.value.operations) {
    for (const std::string& slot : slots) {
      if (operation.slot.has_value() && *operation.slot == slot) {
        out->operations.push_back(operation);
      }
    }
  }
  return true;
}

/// The execute options every scenario here runs with: the sandbox's own
/// `.backup/`, and a clock that does not move.
sync::ExecuteOptions ExecuteAt(std::int64_t unix_seconds) {
  sync::ExecuteOptions options;
  options.backup_dir = harness::kBackupDir;
  options.now = [unix_seconds]() { return sync::Timestamp{} + std::chrono::seconds{unix_seconds}; };
  return options;
}

/// `FinishTick`'s options with the backoff spent instantly. A test that had to
/// wait out three seconds of retry to prove there was one is a test nobody runs.
sync::FinishOptions FinishInstantly() {
  sync::FinishOptions options;
  options.complete.wait = [](std::chrono::milliseconds) {};
  return options;
}

/// The baseline the engine just wrote, read back with the engine's own reader.
state::LoadedBaseline StoredBaseline(fs::FileSystem& files) {
  return state::LoadBaseline(files.Resolve(sync::kStateSdPath));
}

// --- counts -------------------------------------------------------------------
//
// The accounting, which needs no server: an `ExecutionReport` becomes the two
// integers `complete` carries. The one that is easy to get wrong is
// `not_understood` -- an `action` this build does not know is work the server
// planned and the client did not do, and reporting it completed would tell RomM
// the client did something it cannot even name.

void Counts(rig::Checks& checks) {
  sync::ExecutionReport report;
  report.completed = 3;
  report.failed = 1;
  report.not_understood = 2;

  const sync::CompletionCounts counts = sync::CountsFor(report);
  checks.ExpectEq(counts.operations_completed, 3, "the completed operations are reported as such");
  checks.ExpectEq(counts.operations_failed, 3,
                  "an action this build did not understand is reported failed, not completed");

  const sync::Encoded body = sync::EncodeCompleteRequest(counts);
  checks.Expect(body.ok(), "the counts encode: " + body.error.Describe());
  checks.ExpectEq(
      body.body,
      std::string(R"({"operations_completed":3,"operations_failed":3,"play_sessions":[]})"),
      "the body carries both counts and an explicit empty play_sessions");

  // A cancelled tick reports what it actually did. The operations after the
  // cancellation are in neither count, deliberately: they were not attempted.
  sync::ExecutionReport stopped;
  stopped.completed = 1;
  stopped.canceled = true;
  const sync::CompletionCounts partial = sync::CountsFor(stopped);
  checks.ExpectEq(partial.operations_completed, 1, "a cancelled tick reports the work it did");
  checks.ExpectEq(partial.operations_failed, 0, "...and does not call a shutdown a failure");

  // A count that went negative upstream is a bug, and one that would otherwise
  // become a permanent row in a history a user reads.
  sync::CompletionCounts negative;
  negative.operations_failed = -1;
  const sync::Encoded refused = sync::EncodeCompleteRequest(negative);
  checks.Expect(!refused.ok(), "a negative count is refused rather than sent");
  checks.Expect(refused.body.empty(), "...and there is no body to send by accident");
  checks.ExpectEq(refused.error.field, std::string("operations_failed"),
                  "...naming the field that is wrong");

  // Neither refusal below may reach the network: there is nothing to ask.
  NeverCalled client(checks);
  auth::StoredToken unpaired;
  const sync::Completion nowhere = sync::CompleteSession(client, unpaired, 1, counts);
  checks.Expect(nowhere.error == sync::CompleteError::kNotRegistered,
                std::string("an unpaired console has nothing to complete: ") +
                    sync::ToString(nowhere.error));

  auth::StoredToken token;
  token.server_url = "http://127.0.0.1:1";
  token.access_token = "not-used";
  token.device_id = "not-used";
  const sync::Completion no_session = sync::CompleteSession(client, token, 0, counts);
  checks.Expect(no_session.error == sync::CompleteError::kNoSession,
                std::string("a tick with no plan has no session to complete: ") +
                    sync::ToString(no_session.error));

  // A tick cancelled during execution reaches the accounting with the token
  // already fired. Spending three timeouts and two backoffs to discover that --
  // on the link whose loss is usually why the shutdown happened -- is the delay
  // the token exists to avoid, so nothing is sent at all.
  {
    http::CancelToken stopped;
    stopped.Cancel();
    sync::CompleteOptions giving_up;
    giving_up.cancel = &stopped;
    giving_up.wait = [&checks](std::chrono::milliseconds) {
      checks.Expect(false, "a cancelled completion must not spend a backoff");
    };
    const sync::Completion abandoned =
        sync::CompleteSession(client, token, 12, counts, giving_up);
    checks.Expect(abandoned.error == sync::CompleteError::kCanceled,
                  std::string("a cancelled completion is named as such: ") +
                      sync::ToString(abandoned.error));
    checks.ExpectEq(abandoned.attempts, 0, "...and no attempt was made");

    // The same token on the same policy, one call earlier. `CallPolicy` is
    // shared by both calls, so a field only one of them honoured would be a
    // trap rather than a knob.
    sync::NegotiateOptions negotiating;
    negotiating.cancel = &stopped;
    const sync::Negotiation gave_up = sync::Negotiate(client, token, {}, negotiating);
    checks.Expect(gave_up.error == sync::NegotiateError::kCanceled,
                  std::string("...and so is a cancelled negotiation: ") +
                      sync::ToString(gave_up.error));
    checks.Expect(!sync::ShouldRetry(sync::CompleteError::kCanceled) &&
                      !sync::ShouldRetry(sync::NegotiateError::kCanceled) &&
                      !sync::NeedsPairing(sync::NegotiateError::kCanceled),
                  "a shutdown is neither retried nor a reason to pair again");
  }

  // A retryable failure is the same two members negotiate retries on, and the
  // pairing failures are not among them.
  checks.Expect(sync::ShouldRetry(sync::CompleteError::kUnreachable) &&
                    sync::ShouldRetry(sync::CompleteError::kServerError),
                "a dropped or 5xx completion is worth another attempt");
  checks.Expect(!sync::ShouldRetry(sync::CompleteError::kUnauthorized) &&
                    !sync::ShouldRetry(sync::CompleteError::kNoSuchSession) &&
                    !sync::ShouldRetry(sync::CompleteError::kAlreadyCompleted) &&
                    !sync::ShouldRetry(sync::CompleteError::kSuperseded) &&
                    !sync::ShouldRetry(sync::CompleteError::kRejected),
                "a revoked token, a session that is gone, one already accounted for, one a later "
                "negotiation superseded and a refused body are not");
}

// --- parse --------------------------------------------------------------------
//
// The response, read against the body a live 5.2.0 actually sent
// (server/contract/captures/sync-complete.json). Everything here is offline: the
// capture is the same bytes docs/API_CONTRACT.md quotes.

void Parse(rig::Checks& checks) {
  const std::string body =
      ReadFile(std::string(ROMMSYNC_CAPTURES_DIR) + "/sync-complete.json");
  checks.Expect(!body.empty(), "the committed capture is readable");

  const auth::Parsed<sync::SyncCompletion> parsed = sync::ParseCompleteResponse(body);
  checks.Expect(parsed.ok(), "the engine reads the captured completion: " + parsed.error.Describe());
  if (!parsed.ok()) {
    return;
  }
  const sync::SyncSession& session = parsed.value.session;
  checks.ExpectEq(session.id, static_cast<std::int64_t>(139), "the session id");
  checks.ExpectEq(session.status, std::string("COMPLETED"),
                  "the status is upper-case, as the server spells it");
  checks.ExpectEq(session.operations_planned, static_cast<std::int64_t>(0),
                  "a plan of nothing that needed work is planned 0");
  checks.ExpectEq(session.operations_completed, static_cast<std::int64_t>(1),
                  "...against one the client completed, which is not an error");
  checks.Expect(session.operations_completed > session.operations_planned,
                "and nothing here treats completed > planned as one");
  checks.Expect(session.completed_at.has_value(), "a completed session carries a completed_at");
  checks.Expect(!session.error_message.has_value(), "and no error_message");
  checks.Expect(!parsed.value.play_session_ingest,
                "a completion that sent no play sessions is answered with a null ingest");
  checks.Expect(parsed.value.warnings.empty(),
                "the ordinary completion says nothing out loud: " +
                    (parsed.value.warnings.empty() ? std::string() : parsed.value.warnings[0]));

  // A body that ends early must not become a session with the counts that
  // happened to arrive -- which reads exactly like one reported correctly.
  for (std::size_t cut : {std::size_t{20}, body.size() / 2, body.size() - 3}) {
    const auth::Parsed<sync::SyncCompletion> truncated =
        sync::ParseCompleteResponse(std::string_view(body).substr(0, cut));
    checks.Expect(!truncated.ok(), "a body cut at " + std::to_string(cut) + " bytes is refused");
  }
  checks.Expect(!sync::ParseCompleteResponse("{\"play_session_ingest\":null}").ok(),
                "a 200 with no session in it is a named error, not an empty session");
  checks.Expect(!sync::ParseCompleteResponse("{\"session\":{\"id\":7}}").ok(),
                "and neither is a session missing the counts the call is about");

  // The two things the server can say that are worth a log line and are not
  // errors: a session it had already ended, and an ingest for play sessions this
  // client never sent.
  const auth::Parsed<sync::SyncCompletion> cancelled = sync::ParseCompleteResponse(
      R"({"session":{"id":7,"device_id":"d","user_id":1,"status":"CANCELLED",)"
      R"("initiated_at":"2026-09-04T11:36:26+00:00","completed_at":null,)"
      R"("operations_planned":2,"operations_completed":0,"operations_failed":0,)"
      R"("error_message":"superseded","created_at":"2026-09-04T11:36:26+00:00",)"
      R"("updated_at":"2026-09-04T11:36:26+00:00"},"play_session_ingest":{"created":0}})");
  checks.Expect(cancelled.ok(), "a session that is not COMPLETED still parses: " +
                                    cancelled.error.Describe());
  if (cancelled.ok()) {
    checks.ExpectEq(static_cast<int>(cancelled.value.warnings.size()), 3,
                    "...with a line for the status, the error_message and the ingest");
    checks.Expect(cancelled.value.play_session_ingest,
                  "an ingest for play sessions that were never sent is noticed");
  }
}

// --- stamps -------------------------------------------------------------------
//
// `sync::ParseTimestamp`, which is what turns the `server_updated_at` on a plan
// into the seconds a baseline row stores. It has to read a spelling
// `FormatTimestamp` never writes -- this client sends `Z` and RomM sends
// `+00:00` -- and it must not read one that is not a timestamp at all.

void Stamps(rig::Checks& checks) {
  const std::optional<sync::Timestamp> zulu = sync::ParseTimestamp("2026-09-04T11:36:27Z");
  const std::optional<sync::Timestamp> offset = sync::ParseTimestamp("2026-09-04T11:36:27+00:00");
  checks.Expect(zulu.has_value() && offset.has_value(),
                "both spellings of UTC are read; RomM sends one and this client writes the other");
  if (zulu.has_value() && offset.has_value()) {
    checks.ExpectEq(sync::UnixSeconds(*zulu), sync::UnixSeconds(*offset),
                    "and they are the same instant");
    // Round trip through the engine's own formatter, which is the only claim
    // that matters: what this reads is what that would write.
    checks.ExpectEq(sync::FormatTimestamp(*zulu), std::string("2026-09-04T11:36:27Z"),
                    "a parsed instant formats back to itself");
  }

  // What RomM actually sends on a plan, sub-second digits and all. Dropped
  // downwards, the same direction `FormatTimestamp` drops them: a copy stamped
  // :27.9 is not newer than one stamped :27.
  const std::optional<sync::Timestamp> fraction =
      sync::ParseTimestamp("2026-09-04T22:45:33.512340+00:00");
  checks.Expect(fraction.has_value(), "a fractional second is read");
  if (fraction.has_value()) {
    checks.ExpectEq(sync::FormatTimestamp(*fraction), std::string("2026-09-04T22:45:33Z"),
                    "...and dropped rather than rounded up");
  }

  // A real offset is applied rather than ignored. Nothing in 5.2.0 sends one,
  // and a reader that quietly dropped it would be off by hours the day something
  // does.
  const std::optional<sync::Timestamp> shifted = sync::ParseTimestamp("2026-09-04T13:36:27+02:00");
  checks.Expect(shifted.has_value() && zulu.has_value() &&
                    sync::UnixSeconds(*shifted) == sync::UnixSeconds(*zulu),
                "a non-zero offset is applied, not dropped");

  for (const char* refused : {"", "not a timestamp", "2026-09-04", "2026-9-4T11:36:27Z",
                              "2026-13-04T11:36:27Z", "2026-02-30T11:36:27Z",
                              "2026-09-04T25:36:27Z", "2026-09-04T11:36:60Z",
                              "2026-09-04T11:36:27+0000",
                              "2026-09-04T11:36:27Z junk", "2026-09-04T11:36:27."}) {
    checks.Expect(!sync::ParseTimestamp(refused).has_value(),
                  std::string("refused: \"") + refused + "\"");
  }
  // The epoch is what a console with an unset clock reports, and the window a
  // save may claim excludes it -- the same window `Validate` holds an mtime to.
  checks.Expect(!sync::ParseTimestamp("1970-01-01T00:00:00Z").has_value(),
                "the epoch is not an instant a save may claim");
}

// --- advance ------------------------------------------------------------------
//
// Which operations may move a baseline row and which may not. No server: every
// outcome here is one `ExecutePlan` can produce, and the rules are about what is
// written afterwards.

sync::SyncOperation OperationOf(sync::Action action, std::int64_t rom_id, std::string slot) {
  sync::SyncOperation operation;
  operation.action = action;
  operation.action_text = sync::ToString(action);
  operation.rom_id = rom_id;
  operation.slot = std::move(slot);
  operation.file_name = "whatever [2026-09-04_11-12-27].srm";
  operation.reason = sync::Reason::kNoChanges;
  operation.reason_text = sync::ReasonText(sync::Reason::kNoChanges);
  operation.server_updated_at = std::string("2026-09-04T11:36:27+00:00");
  operation.server_content_hash = std::string("abd8fff93894e8112c7dd17386e54a5f");
  return operation;
}

sync::OperationResult ResultOf(sync::OperationOutcome outcome, std::int64_t rom_id,
                               std::string slot, std::string sd_path) {
  sync::OperationResult result;
  result.rom_id = rom_id;
  result.slot = std::move(slot);
  result.outcome = outcome;
  result.sd_path = std::move(sd_path);
  return result;
}

sync::ClientSaveState ReportedSave(std::int64_t rom_id, const std::string& slot,
                                   const std::string& hash, std::int64_t when,
                                   std::int64_t size) {
  sync::ClientSaveState save;
  save.rom_id = rom_id;
  save.file_name = slot + ".srm";
  save.slot = slot;
  save.content_hash = hash;
  save.updated_at = sync::Timestamp{} + std::chrono::seconds{when};
  save.file_size_bytes = size;
  return save;
}

void Advance(rig::Checks& checks) {
  Sandbox sandbox(checks, "complete-advance");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());

  // A save the tick downloaded: its bytes on the card are the server's, and
  // everything the tick *reported* about it describes the bytes that are gone.
  const std::string downloaded = "the server's bytes, now on the card\n";
  sandbox.Write(SavePath("downloaded.srm"), downloaded);
  const fs::Listing listing = files->List(harness::kSavesDir);
  checks.Expect(listing.ok(), "the saves folder lists: " + listing.message);
  std::int64_t downloaded_mtime = 0;
  std::int64_t downloaded_size = 0;
  for (const fs::Entry& entry : listing.entries) {
    if (entry.name == "downloaded.srm") {
      downloaded_mtime = entry.modified_unix;
      downloaded_size = entry.size_bytes;
    }
  }

  constexpr std::int64_t kReportedMtime = 1'757'000'000;
  const std::string uploaded_hash = crypto::Md5Hex("uploaded bytes");
  const std::string stale_hash = crypto::Md5Hex("the bytes that are gone");

  std::vector<sync::ClientSaveState> reported = {
      ReportedSave(1, "uploaded", uploaded_hash, kReportedMtime, 14),
      ReportedSave(2, "downloaded", stale_hash, kReportedMtime, 99),
      ReportedSave(3, "failed", crypto::Md5Hex("still here"), kReportedMtime, 10),
      ReportedSave(4, "unhashed", "", kReportedMtime, 7),
      ReportedSave(5, "untouched", crypto::Md5Hex("nobody planned this"), kReportedMtime, 19),
      ReportedSave(6, "not-understood", crypto::Md5Hex("still here"), kReportedMtime, 6),
  };

  sync::SyncPlan plan;
  plan.session_id = 1;
  plan.operations.push_back(OperationOf(sync::Action::kUpload, 1, "uploaded"));
  plan.operations.push_back(OperationOf(sync::Action::kDownload, 2, "downloaded"));
  plan.operations.push_back(OperationOf(sync::Action::kUpload, 3, "failed"));
  plan.operations.push_back(OperationOf(sync::Action::kUpload, 4, "unhashed"));
  plan.operations.push_back(OperationOf(sync::Action::kNoOp, 6, "not-understood"));

  sync::ExecutionReport report;
  report.operations.push_back(
      ResultOf(sync::OperationOutcome::kUploaded, 1, "uploaded", SavePath("uploaded.srm")));
  report.operations.push_back(ResultOf(sync::OperationOutcome::kDownloaded, 2, "downloaded",
                                       SavePath("downloaded.srm")));
  report.operations.push_back(
      ResultOf(sync::OperationOutcome::kFailed, 3, "failed", SavePath("failed.srm")));
  report.operations.push_back(
      ResultOf(sync::OperationOutcome::kUploaded, 4, "unhashed", SavePath("unhashed.srm")));
  report.operations.push_back(ResultOf(sync::OperationOutcome::kNotUnderstood, 6,
                                       "not-understood", SavePath("not-understood.srm")));

  // What the last tick left behind. The failed save's row is the one that must
  // come through untouched.
  state::Baseline previous;
  state::SaveRecord kept;
  kept.rom_id = 3;
  kept.slot = "failed";
  kept.content_hash = crypto::Md5Hex("what the last tick saw");
  kept.mtime = sync::Timestamp{} + std::chrono::seconds{1'700'000'000};
  kept.file_size_bytes = 4;
  previous.Set(kept);
  state::SaveRecord unrecognised;
  unrecognised.rom_id = 6;
  unrecognised.slot = "not-understood";
  unrecognised.content_hash = crypto::Md5Hex("whatever the last tick saw");
  unrecognised.mtime = sync::Timestamp{} + std::chrono::seconds{1'700'000'000};
  unrecognised.file_size_bytes = 6;
  previous.Set(unrecognised);

  const sync::BaselineUpdate update =
      sync::AdvanceBaseline(previous, plan, report, reported, *files);

  // An upload does not touch the local file, so the facts the tick reported are
  // still the facts on the card.
  const state::SaveRecord* upload = update.value.Find(1, std::string("uploaded"));
  checks.Expect(upload != nullptr, "an upload advances its save's row");
  if (upload != nullptr) {
    checks.ExpectEq(upload->content_hash, uploaded_hash, "with the digest the tick reported");
    checks.ExpectEq(sync::UnixSeconds(upload->mtime), kReportedMtime, "and its mtime");
    checks.ExpectEq(upload->file_size_bytes, static_cast<std::int64_t>(14), "and its size");
    // The server now holds the bytes this client sent, so the plan's digest --
    // which described the copy the upload replaced -- is already out of date.
    checks.Expect(upload->server_content_hash.has_value() &&
                      *upload->server_content_hash == uploaded_hash,
                  "the server's copy is recorded as the bytes that were just sent to it");
    checks.Expect(!upload->server_updated_at.has_value(),
                  "and its new timestamp is left absent rather than guessed from the local mtime");
  }

  // A download replaced the bytes, so all three local fields come off the card.
  const state::SaveRecord* download = update.value.Find(2, std::string("downloaded"));
  checks.Expect(download != nullptr, "a download advances its save's row");
  if (download != nullptr) {
    checks.ExpectEq(download->content_hash, crypto::Md5Hex(downloaded),
                    "with the digest of the bytes that are now on the card");
    checks.Expect(download->content_hash != stale_hash,
                  "which is not the one the tick reported before the download");
    checks.ExpectEq(sync::UnixSeconds(download->mtime), downloaded_mtime,
                    "the card's mtime, so the next tick can skip the hash");
    checks.ExpectEq(download->file_size_bytes, downloaded_size, "and the card's size");
    checks.Expect(download->server_updated_at.has_value(),
                  "the server's timestamp is read off the plan and stored as seconds");
    checks.Expect(download->server_content_hash.has_value() &&
                      *download->server_content_hash == "abd8fff93894e8112c7dd17386e54a5f",
                  "along with its digest");
  }

  // The rule the whole retry depends on, for both outcomes that keep a row: a
  // failure, and an action this build did not recognise. The second is the one
  // worth asserting separately -- nothing was done and the client does not know
  // what should have been, so advancing its row would record a sync that never
  // happened.
  const state::SaveRecord* failed = update.value.Find(3, std::string("failed"));
  checks.Expect(failed != nullptr, "a failed operation keeps its previous row");
  if (failed != nullptr) {
    checks.ExpectEq(failed->content_hash, kept.content_hash, "...exactly as it was");
    checks.ExpectEq(sync::UnixSeconds(failed->mtime), static_cast<std::int64_t>(1'700'000'000),
                    "...mtime included");
  }
  const state::SaveRecord* unknown = update.value.Find(6, std::string("not-understood"));
  checks.Expect(unknown != nullptr, "an action this build does not know keeps its previous row");
  if (unknown != nullptr) {
    checks.ExpectEq(unknown->content_hash, unrecognised.content_hash, "...exactly as it was");
  }

  // A row with an empty digest is one `SaveBaseline` refuses and one the scanner
  // could never reuse, so the honest answer is no row.
  checks.Expect(update.value.Find(4, std::string("unhashed")) == nullptr,
                "a save whose hash failed gets no row rather than a row with an empty digest");

  // A save the plan said nothing about gets nothing invented for it. RomM
  // answers a reported, in-sync save with an explicit `no_op` rather than an
  // absence (complete.unchanged asserts that against the live server), so an
  // absence here means the client reported something the server had no
  // operation for -- and there is nothing to record about that.
  checks.Expect(update.value.Find(5, std::string("untouched")) == nullptr,
                "a reported save with no operation is not advanced");

  checks.ExpectEq(update.advanced, static_cast<std::size_t>(2),
                  "two rows moved forward: the upload and the download");
  checks.ExpectEq(update.value.size(), static_cast<std::size_t>(4),
                  "beside the two rows the tick was not allowed to move");

  // What was written has to be what can be read back, which is the one claim a
  // baseline is worth anything for.
  const state::StoreResult stored =
      state::SaveBaseline(files->Resolve(sync::kStateSdPath), update.value);
  checks.Expect(stored.ok(), "the advanced baseline is written: " + stored.message);
  checks.ExpectEq(stored.rows_written, static_cast<std::size_t>(4), "with every row in it");
  const state::LoadedBaseline loaded = StoredBaseline(*files);
  checks.Expect(loaded.diagnostics.empty(),
                "and read back with no diagnostics: " + loaded.DescribeDiagnostics());
  checks.ExpectEq(loaded.value.size(), static_cast<std::size_t>(4), "and every row again");

  // The point of the row: the next tick does not open the file.
  const state::HashOutcome reused = state::ContentHashFor(
      loaded.value, 2, std::string("downloaded"), files->Resolve(SavePath("downloaded.srm")),
      sync::Timestamp{} + std::chrono::seconds{downloaded_mtime}, downloaded_size);
  checks.Expect(reused.reused, "the downloaded save's digest is reused rather than recomputed");
  checks.ExpectEq(reused.content_hash, crypto::Md5Hex(downloaded), "and it is the right digest");

  // A `no_op` moved neither side, so what the last sync recorded about the
  // server's copy is still true and must survive a plan that does not restate
  // it. A record built from nothing would drop it silently, and the next
  // arbitration would fall into the no-sync-history branch.
  {
    state::Baseline before;
    state::SaveRecord known;
    known.rom_id = 7;
    known.slot = "kept-server-half";
    known.content_hash = crypto::Md5Hex("unchanged");
    known.mtime = sync::Timestamp{} + std::chrono::seconds{kReportedMtime};
    known.file_size_bytes = 9;
    known.server_updated_at = sync::Timestamp{} + std::chrono::seconds{1'700'000'500};
    known.server_content_hash = crypto::Md5Hex("what the server had");
    before.Set(known);

    sync::SyncOperation silent = OperationOf(sync::Action::kNoOp, 7, "kept-server-half");
    silent.server_updated_at = std::nullopt;
    silent.server_content_hash = std::nullopt;
    sync::SyncPlan quiet_plan;
    quiet_plan.session_id = 1;
    quiet_plan.operations.push_back(silent);
    sync::ExecutionReport quiet_report;
    quiet_report.operations.push_back(
        ResultOf(sync::OperationOutcome::kNoOp, 7, "kept-server-half", SavePath("kept.srm")));
    const std::vector<sync::ClientSaveState> still = {
        ReportedSave(7, "kept-server-half", crypto::Md5Hex("unchanged"), kReportedMtime, 9)};

    const sync::BaselineUpdate quiet =
        sync::AdvanceBaseline(before, quiet_plan, quiet_report, still, *files);
    const state::SaveRecord* row = quiet.value.Find(7, std::string("kept-server-half"));
    checks.Expect(row != nullptr, "a no_op advances its row");
    if (row != nullptr) {
      checks.Expect(row->server_updated_at.has_value() &&
                        sync::UnixSeconds(*row->server_updated_at) == 1'700'000'500,
                    "...and keeps what the last sync knew about the server's copy");
      checks.Expect(row->server_content_hash.has_value() &&
                        *row->server_content_hash == known.server_content_hash,
                    "...digest included, because a no_op moved neither side");
    }
  }

  // The failure #11 named, arriving from the other side: rows for saves that
  // are no longer on the card accumulate, and a baseline over `kMaxRecords` is
  // one `SaveBaseline` refuses whole -- which is a silent re-hash of the library
  // on every tick from then on. Rows nothing mentioned are dropped, but only
  // when keeping them would cost the file.
  {
    state::Baseline crowded;
    for (std::size_t index = 0; index < state::kMaxRecords; ++index) {
      state::SaveRecord stale;
      stale.rom_id = static_cast<std::int64_t>(1000 + index);
      stale.slot = "deleted-long-ago";
      stale.content_hash = crypto::Md5Hex("row " + std::to_string(index));
      stale.mtime = sync::Timestamp{} + std::chrono::seconds{kReportedMtime};
      stale.file_size_bytes = 1;
      crowded.Set(stale);
    }
    const sync::BaselineUpdate trimmed =
        sync::AdvanceBaseline(crowded, plan, report, reported, *files);
    checks.Expect(trimmed.value.size() <= state::kMaxRecords,
                  "a baseline that would outgrow the bound is trimmed rather than refused: " +
                      std::to_string(trimmed.value.size()));
    checks.Expect(trimmed.dropped > 0, "...and the drops are counted");
    checks.Expect(trimmed.value.Find(1, std::string("uploaded")) != nullptr,
                  "the rows this tick earned are the ones that survive");
    checks.Expect(state::SaveBaseline(files->Resolve(sync::kStateSdPath), trimmed.value).ok(),
                  "so the file can still be written at all");
  }

  // `kStateSdPath` spells the file name that `state::kStateFileName` owns. Two
  // spellings of it is one tick writing a baseline the next one cannot find.
  {
    const std::string path = sync::kStateSdPath;
    const std::string name = state::kStateFileName;
    checks.Expect(path.size() > name.size() &&
                      path.compare(path.size() - name.size(), name.size(), name) == 0,
                  "the SD path ends in the file name state_db.hpp declares: " + path);
  }

  // An upload whose *reported* digest is not a usable MD5 must not put that
  // digest on the row as the server's. `state::Usable` refuses a whole row over
  // it, so the save would lose its local half too and be re-hashed forever --
  // the failure this module exists to prevent, arriving through the one branch
  // that reads a client-supplied digest.
  {
    sync::SyncPlan bad_plan;
    bad_plan.session_id = 1;
    bad_plan.operations.push_back(OperationOf(sync::Action::kUpload, 8, "sha1-digest"));
    sync::ExecutionReport bad_report;
    bad_report.operations.push_back(
        ResultOf(sync::OperationOutcome::kUploaded, 8, "sha1-digest", SavePath("sha1.srm")));
    // 40 hex characters: a SHA1, which is the mistake sync.hpp is written
    // against. It is a legitimate `content_hash` field value and an illegitimate
    // digest.
    std::vector<sync::ClientSaveState> sha1 = {
        ReportedSave(8, "sha1-digest", crypto::Md5Hex("bytes"), kReportedMtime, 5)};
    sha1[0].content_hash = std::string(40, 'a');

    const sync::BaselineUpdate guarded =
        sync::AdvanceBaseline({}, bad_plan, bad_report, sha1, *files);
    checks.Expect(guarded.value.Find(8, std::string("sha1-digest")) == nullptr,
                  "a reported digest that is not an MD5 gets no row at all");
    const state::StoreResult writable =
        state::SaveBaseline(files->Resolve(sync::kStateSdPath), guarded.value);
    checks.Expect(writable.ok(), "...rather than one the writer refuses: " + writable.message);
  }

  // A save whose bytes were replaced and then vanished cannot be described, and
  // a row invented for it would claim a digest for a file nothing hashed.
  sync::SyncPlan gone_plan;
  gone_plan.session_id = 1;
  gone_plan.operations.push_back(OperationOf(sync::Action::kDownload, 6, "gone"));
  sync::ExecutionReport gone_report;
  gone_report.operations.push_back(
      ResultOf(sync::OperationOutcome::kDownloaded, 6, "gone", SavePath("gone.srm")));
  // Asserted against a baseline that *has* a row for it, not an empty one: the
  // interesting case is the row that was already there. A download replaced the
  // bytes, so a stored row describes bytes that are gone -- keeping it would
  // leave a row that is known to be false, and leaning on `ContentHashFor`
  // refusing to reuse it would be this module trusting another one to hold an
  // invariant it states itself.
  state::Baseline had_a_row;
  state::SaveRecord doomed;
  doomed.rom_id = 6;
  doomed.slot = "gone";
  doomed.content_hash = crypto::Md5Hex("the bytes the download replaced");
  doomed.mtime = sync::Timestamp{} + std::chrono::seconds{kReportedMtime};
  doomed.file_size_bytes = 31;
  had_a_row.Set(doomed);

  const sync::BaselineUpdate missing =
      sync::AdvanceBaseline(had_a_row, gone_plan, gone_report, {}, *files);
  checks.Expect(missing.value.Find(6, std::string("gone")) == nullptr,
                "a download that cannot be read back erases the row it replaced");
  checks.Expect(!missing.warnings.empty(), "...and the loss is named");

  // The opposite path, and the opposite answer: an upload leaves the card alone,
  // so a reported digest that is missing costs the save nothing -- the row the
  // last tick stored still describes the file and is kept.
  {
    state::Baseline still_good;
    state::SaveRecord stored;
    stored.rom_id = 4;
    stored.slot = "unhashed";
    stored.content_hash = crypto::Md5Hex("what the last tick hashed");
    stored.mtime = sync::Timestamp{} + std::chrono::seconds{kReportedMtime};
    stored.file_size_bytes = 7;
    still_good.Set(stored);

    const sync::BaselineUpdate kept_row =
        sync::AdvanceBaseline(still_good, plan, report, reported, *files);
    const state::SaveRecord* survivor = kept_row.value.Find(4, std::string("unhashed"));
    checks.Expect(survivor != nullptr,
                  "an upload with no usable digest keeps the row the last tick stored");
    if (survivor != nullptr) {
      checks.ExpectEq(survivor->content_hash, stored.content_hash, "...unchanged");
    }
  }

  // The report and the plan are paired by index. A pairing that slipped would
  // put one save's server digest on another save's row.
  sync::ExecutionReport crossed;
  crossed.operations.push_back(
      ResultOf(sync::OperationOutcome::kUploaded, 99, "not-in-the-plan", SavePath("x.srm")));
  const sync::BaselineUpdate mismatched =
      sync::AdvanceBaseline({}, plan, crossed, reported, *files);
  checks.Expect(mismatched.value.Find(99, std::string("not-in-the-plan")) == nullptr,
                "an operation that does not pair with the plan advances nothing");
  checks.Expect(!mismatched.warnings.empty(), "...and says so");
}

// --- session ------------------------------------------------------------------
//
// The whole loop against the docker RomM: negotiate, execute, complete, and the
// baseline on the card afterwards. The counts that go out are the ones that come
// back, and the session RomM answers with is `COMPLETED`.

void Session(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "complete-session");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-6-session");
  const std::string name = "session.srm";
  const std::string bytes = "the device's only copy\n";
  sandbox.SeedSave(SavePath(name), bytes);

  sync::ClientSaveState local;
  if (!LocalSaveOnCard(checks, *files, sandbox, rom.id, name, slot, &local)) {
    return;
  }
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(local);

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    return;
  }
  if (plan.operations.size() != 1) {
    checks.Expect(false, "the plan carries the one operation this run created");
    harness::Complete(client, base, fixture, plan.session_id, 0, 0);
    return;
  }
  checks.Expect(plan.operations[0].action == sync::Action::kUpload,
                std::string("a save only the client has is an upload: ") +
                    sync::ToString(plan.operations[0].action));

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, ExecuteAt(1'757'000'000));
  checks.ExpectEq(report.completed, 1,
                  "the upload completed" +
                      (report.warnings.empty() ? std::string() : ": " + report.warnings[0]));

  const sync::TickCompletion tick =
      sync::FinishTick(client, *files, TokenFor(base, fixture), plan, report, payload.saves,
                       /*previous=*/{}, FinishInstantly());

  checks.Expect(tick.reported.ok(), "the session is completed: " + tick.reported.message);
  checks.Expect(tick.stored.ok(), "and the baseline is written: " + tick.stored.message);
  const sync::SyncSession& session = tick.reported.value.session;
  checks.ExpectEq(session.status, std::string("COMPLETED"),
                  "the session comes back COMPLETED, upper-case");
  checks.ExpectEq(session.id, plan.session_id, "and it is the session this tick negotiated");
  checks.ExpectEq(session.device_id, fixture.device_id, "for this device");
  checks.ExpectEq(session.operations_completed,
                  static_cast<std::int64_t>(tick.counts.operations_completed),
                  "with the completed count this tick sent");
  checks.ExpectEq(session.operations_failed,
                  static_cast<std::int64_t>(tick.counts.operations_failed),
                  "and the failed one");
  checks.Expect(session.completed_at.has_value(), "and a completed_at, which is what closes it");
  checks.Expect(tick.reported.value.warnings.empty(),
                "nothing about the completion needed saying: " +
                    (tick.reported.value.warnings.empty() ? std::string()
                                                          : tick.reported.value.warnings[0]));

  // The baseline, read back with the engine's own reader. A row that cannot be
  // read is a row that saves nothing.
  const state::LoadedBaseline loaded = StoredBaseline(*files);
  checks.Expect(loaded.diagnostics.empty(),
                "the stored baseline reads back clean: " + loaded.DescribeDiagnostics());
  const state::SaveRecord* row = loaded.value.Find(rom.id, slot);
  checks.Expect(row != nullptr, "with a row for the save this tick uploaded");
  if (row != nullptr) {
    checks.ExpectEq(row->content_hash, *local.content_hash, "holding the digest that was sent");
    checks.ExpectEq(row->file_size_bytes, local.file_size_bytes,
                    "and the size, which the skip needs as much as the mtime");
  }

  // What the row is for: the next tick does not open the file.
  const state::HashOutcome reused =
      state::ContentHashFor(loaded.value, rom.id, slot, files->Resolve(SavePath(name)),
                            local.updated_at, local.file_size_bytes);
  checks.Expect(reused.reused, "the next tick reuses the digest rather than re-hashing: " +
                                   reused.message);
  checks.ExpectEq(reused.content_hash, *local.content_hash, "and it is the same digest");

  // Completing a session that is already closed, and completing one that never
  // existed. Both are asserted rather than assumed because both are on the
  // ordinary path: `CompleteSession` retries a 5xx, so a first attempt that
  // reached RomM before the connection died makes the *second* one land here --
  // and a tick that gave up on a negotiation completes an id the next negotiate
  // already cancelled (docs/API_CONTRACT.md).
  const sync::Completion again = sync::CompleteSession(
      client, TokenFor(base, fixture), plan.session_id, tick.counts, FinishInstantly().complete);
  checks.Expect(again.error == sync::CompleteError::kAlreadyCompleted,
                std::string("a session completed twice is named as already accounted for, not "
                            "as a refused body: ") +
                    sync::ToString(again.error) + " -- " + again.message);
  checks.Expect(!sync::ShouldRetry(again.error),
                "...and it is not retried, because the thing it wanted already happened");

  const sync::Completion missing =
      sync::CompleteSession(client, TokenFor(base, fixture), 999'999'999, tick.counts,
                            FinishInstantly().complete);
  checks.Expect(missing.error == sync::CompleteError::kNoSuchSession,
                std::string("a session id RomM does not know is named: ") +
                    sync::ToString(missing.error) + " -- " + missing.message);
  checks.Expect(!sync::ShouldRetry(missing.error), "...and is not worth another attempt");

  // And the stale id a tick actually ends up holding, which is a different
  // answer from both of those: negotiate twice and the first session is
  // CANCELLED, so completing it is a 400 whose detail says the counts were
  // never recorded rather than that the body was wrong. This is the reachable
  // path -- a tick that gave up on one negotiation and made another is in it.
  {
    sync::SyncNegotiatePayload empty;
    empty.device_id = fixture.device_id;
    sync::SyncPlan first;
    sync::SyncPlan second;
    if (PlanFor(checks, client, base, fixture, empty, {}, &first) &&
        PlanFor(checks, client, base, fixture, empty, {}, &second)) {
      checks.Expect(first.session_id != second.session_id,
                    "negotiating twice opens a second session");
      const sync::Completion stale =
          sync::CompleteSession(client, TokenFor(base, fixture), first.session_id, tick.counts,
                                FinishInstantly().complete);
      checks.Expect(stale.error == sync::CompleteError::kSuperseded,
                    std::string("a session a later negotiation cancelled is named apart from a "
                                "refused body: ") +
                        sync::ToString(stale.error) + " -- " + stale.message);
      checks.Expect(!sync::ShouldRetry(stale.error),
                    "...and is not retried: the next tick negotiates its own session");
      harness::Complete(client, base, fixture, second.session_id, 0, 0);
    }
  }

  std::int64_t save_id = 0;
  if (!report.operations.empty() && report.operations[0].save_id.has_value()) {
    save_id = *report.operations[0].save_id;
  }
  if (save_id != 0) {
    harness::DeleteSave(client, base, fixture, save_id);
  }
}

// --- unchanged ----------------------------------------------------------------
//
// The acceptance criterion the baseline exists for: a second tick immediately
// after a green one plans no work for the same saves, and completes cleanly with
// `operations_planned` 0.
//
// It asserts on the *plan*, not on the timing: "nothing was re-hashed" is not
// something a clock can show, and a test that measured it would pass or fail on
// how fast the machine is.

void Unchanged(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "complete-unchanged");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-6-unchanged");
  const std::string name = "unchanged.srm";
  const std::string bytes = "identical on both sides\n";
  sandbox.SeedSave(SavePath(name), bytes);

  sync::ClientSaveState local;
  if (!LocalSaveOnCard(checks, *files, sandbox, rom.id, name, slot, &local)) {
    return;
  }
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(local);
  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};

  // Tick one: the save goes up and the baseline is written.
  sync::SyncPlan first;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &first) ||
      first.operations.size() != 1) {
    checks.Expect(false, "the first tick plans the upload");
    return;
  }
  const sync::ExecutionReport uploaded = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), first, targets, ExecuteAt(1'757'000'000));
  const sync::TickCompletion done =
      sync::FinishTick(client, *files, TokenFor(base, fixture), first, uploaded, payload.saves,
                       /*previous=*/{}, FinishInstantly());
  checks.Expect(done.ok(), "the first tick ends cleanly: " + done.reported.message + " " +
                               done.stored.message);

  std::int64_t save_id = 0;
  if (!uploaded.operations.empty() && uploaded.operations[0].save_id.has_value()) {
    save_id = *uploaded.operations[0].save_id;
  }

  // Tick two, with the baseline the first one left. The save is reported from
  // the stored digest -- which is what `state::ContentHashFor` does on a real
  // tick -- and the server has nothing for it to do.
  const state::LoadedBaseline loaded = StoredBaseline(*files);
  checks.Expect(loaded.diagnostics.empty(),
                "the baseline reads back clean: " + loaded.DescribeDiagnostics());
  const state::HashOutcome hash =
      state::ContentHashFor(loaded.value, rom.id, slot, files->Resolve(SavePath(name)),
                            local.updated_at, local.file_size_bytes);
  checks.Expect(hash.reused, "the second tick reuses the stored digest: " + hash.message);
  sync::ClientSaveState again = local;
  again.content_hash = hash.content_hash;

  sync::SyncNegotiatePayload second_payload;
  second_payload.device_id = fixture.device_id;
  second_payload.saves.push_back(again);

  sync::SyncPlan second;
  if (!PlanFor(checks, client, base, fixture, second_payload, {slot}, &second)) {
    if (save_id != 0) {
      harness::DeleteSave(client, base, fixture, save_id);
    }
    return;
  }
  // Asserted as a count and not as "every operation, if any": a save this client
  // reported and is in sync with comes back as an explicit `no_op`, not as an
  // absence (verified against the live 5.2.0, now in docs/API_CONTRACT.md). A
  // loop over an empty vector would pass while proving nothing, and it is
  // exactly that absence `AdvanceBaseline` would have had to cover.
  checks.ExpectEq(second.operations.size(), static_cast<std::size_t>(1),
                  "an in-sync save is answered with an operation rather than an absence");
  for (const sync::SyncOperation& operation : second.operations) {
    checks.Expect(operation.action == sync::Action::kNoOp,
                  std::string("...and it is a no_op: ") + sync::ToString(operation.action) + " (" +
                      operation.reason_text + ")");
    checks.Expect(operation.reason == sync::Reason::kContentIdentical,
                  "...because the hashes match: " + operation.reason_text);
  }

  const sync::ExecutionReport nothing = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), second, targets, ExecuteAt(1'757'000'002));
  const sync::TickCompletion quiet =
      sync::FinishTick(client, *files, TokenFor(base, fixture), second, nothing,
                       second_payload.saves, loaded.value, FinishInstantly());

  checks.Expect(quiet.reported.ok(), "an all-no_op tick completes cleanly: " +
                                         quiet.reported.message);
  const sync::SyncSession& session = quiet.reported.value.session;
  checks.ExpectEq(session.operations_planned, static_cast<std::int64_t>(0),
                  "a plan of nothing but no-ops is planned 0");
  checks.Expect(session.operations_completed >= session.operations_planned,
                "and completing more than were planned is not an error");
  checks.ExpectEq(session.operations_failed, static_cast<std::int64_t>(0),
                  "nothing failed");
  checks.ExpectEq(session.status, std::string("COMPLETED"), "and the session is COMPLETED");
  checks.Expect(quiet.stored.ok(), "the baseline is rewritten: " + quiet.stored.message);
  checks.ExpectEq(quiet.stored.rows_written, static_cast<std::size_t>(1),
                  "still holding the one save this run has");

  if (save_id != 0) {
    harness::DeleteSave(client, base, fixture, save_id);
  }
}

// --- reported -----------------------------------------------------------------
//
// A partially failed plan: the counts that go out are the real ones, and the
// save that failed keeps the row it had so the next tick retries it.
//
// The failure is arranged the way a real one arrives -- a save the plan names
// and the card does not have -- rather than through the proxy, because it is the
// *accounting* that is under test and not the transport.

void Reported(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "complete-reported");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string good_slot = harness::UniqueSlot("m2-6-good");
  const std::string lost_slot = harness::UniqueSlot("m2-6-lost");
  const std::string name = "reported.srm";
  sandbox.SeedSave(SavePath(name), "the copy that does go up\n");

  sync::ClientSaveState good;
  if (!LocalSaveOnCard(checks, *files, sandbox, rom.id, name, good_slot, &good)) {
    return;
  }
  // Reported, and not on the card. `ExecutePlan` gets no target for it and fails
  // that operation with `kNoLocalSave`, which is exactly what a save deleted
  // between the scan and the upload looks like.
  sync::ClientSaveState lost = good;
  lost.slot = lost_slot;
  lost.file_name = "lost.srm";
  lost.content_hash = crypto::Md5Hex("bytes that are not on this card\n");

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(good);
  payload.saves.push_back(lost);

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {good_slot, lost_slot}, &plan)) {
    return;
  }
  if (plan.operations.size() != 2) {
    checks.Expect(false, "the plan carries an operation for each of this run's slots");
    harness::Complete(client, base, fixture, plan.session_id, 0, 0);
    return;
  }

  // The row the failed save had before this tick. It is what must come through
  // untouched.
  state::Baseline previous;
  state::SaveRecord kept;
  kept.rom_id = rom.id;
  kept.slot = lost_slot;
  kept.content_hash = crypto::Md5Hex("what the last tick saw");
  kept.mtime = sync::Timestamp{} + std::chrono::seconds{1'700'000'000};
  kept.file_size_bytes = 4;
  previous.Set(kept);

  const std::vector<sync::SaveTarget> targets = {
      {rom.id, good_slot, SavePath(name), name}};
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, ExecuteAt(1'757'000'000));
  checks.ExpectEq(report.completed, 1, "one operation did what the plan asked");
  checks.ExpectEq(report.failed, 1, "and one could not be attempted at all");

  const sync::TickCompletion tick =
      sync::FinishTick(client, *files, TokenFor(base, fixture), plan, report, payload.saves,
                       previous, FinishInstantly());
  checks.Expect(tick.reported.ok(), "the session is completed: " + tick.reported.message);
  checks.ExpectEq(tick.counts.operations_failed, 1, "the failure is reported as one");
  const sync::SyncSession& session = tick.reported.value.session;
  checks.ExpectEq(session.operations_failed, static_cast<std::int64_t>(1),
                  "and the server records it");
  checks.ExpectEq(session.operations_completed, static_cast<std::int64_t>(1),
                  "beside the one that worked");

  const state::LoadedBaseline loaded = StoredBaseline(*files);
  checks.Expect(loaded.diagnostics.empty(),
                "the baseline reads back clean: " + loaded.DescribeDiagnostics());
  const state::SaveRecord* advanced = loaded.value.Find(rom.id, good_slot);
  checks.Expect(advanced != nullptr && advanced->content_hash == *good.content_hash,
                "the save that uploaded advanced its row");
  const state::SaveRecord* unchanged = loaded.value.Find(rom.id, lost_slot);
  checks.Expect(unchanged != nullptr, "the failed save keeps a row, so the next tick retries it");
  if (unchanged != nullptr) {
    checks.ExpectEq(unchanged->content_hash, kept.content_hash,
                    "...the one it had, unchanged");
    checks.ExpectEq(sync::UnixSeconds(unchanged->mtime), static_cast<std::int64_t>(1'700'000'000),
                    "...mtime included");
  }

  if (!report.operations.empty() && report.operations[0].save_id.has_value()) {
    harness::DeleteSave(client, base, fixture, *report.operations[0].save_id);
  }
}

// --- revoked ------------------------------------------------------------------
//
// M1-4 (#8), and the acceptance clause that costs the most to get wrong: a 401
// arriving mid-tick has to end that tick **cleanly**. No partial writes, a
// `state.db` the next boot can still read, and an `operations_failed` that says
// what actually happened.
//
// The baseline is the thing worth protecting here, and it is protected by an
// ordering rather than by a check: `FinishTick` persists it *before* it reports
// the session, because the transfers already landed on the server and a lost
// baseline is the whole library re-hashed and re-uploaded, silently. A tick that
// was refused half way did not advance any row -- so what has to come back off
// the card is the baseline that was already there, byte for byte, not a shorter
// one and not an empty one.

void Revoked(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "complete-revoked");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m1-4-tick");
  const std::string name = "revoked-tick.srm";
  const std::string bytes = "the device's only copy\n";
  sandbox.SeedSave(SavePath(name), bytes);

  sync::ClientSaveState local;
  if (!LocalSaveOnCard(checks, *files, sandbox, rom.id, name, slot, &local)) {
    return;
  }
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(local);

  // A baseline an earlier tick left behind, for a save that is not in this
  // plan at all. It is what a real console always has, and it is what a tick
  // that rebuilt the file from what it happened to know would silently lose.
  state::Baseline previous;
  state::SaveRecord earlier;
  earlier.rom_id = rom.id + 1;
  earlier.slot = "m1-4-earlier-tick";
  earlier.content_hash = std::string(32, 'a');
  earlier.file_size_bytes = 11;
  earlier.mtime = sync::Timestamp{} + std::chrono::seconds{1'757'000'000};
  previous.Set(earlier);
  const state::StoreResult seeded =
      state::SaveBaseline(files->Resolve(sync::kStateSdPath), previous);
  checks.Expect(seeded.ok(), "an earlier tick's baseline is on the card: " + seeded.message);
  const std::string before = sandbox.Read(sync::kStateSdPath);

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan)) {
    return;
  }

  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  sync::ExecutionReport report;
  {
    harness::Fault fault(checks, client, base,
                         R"({"mode":"status","status":401,"path":"/api/saves","count":1})");
    report = sync::ExecutePlan(client, *files, TokenFor(base, fixture), plan, targets,
                               ExecuteAt(1'757'000'200));
  }
  checks.Expect(report.unauthorized, "the tick was refused part way");
  checks.ExpectEq(report.completed, 0, "nothing completed");
  checks.ExpectEq(report.failed, 1, "and the operation that met the 401 is counted failed");

  // The fault is spent, so `complete` reaches a healthy server: the accounting
  // for a tick that failed still has to land, and it has to be honest.
  const sync::TickCompletion tick =
      sync::FinishTick(client, *files, TokenFor(base, fixture), plan, report, payload.saves,
                       std::move(previous), FinishInstantly());

  checks.ExpectEq(static_cast<int>(tick.counts.operations_completed), 0,
                  "the session is told nothing completed");
  checks.ExpectEq(static_cast<int>(tick.counts.operations_failed), 1,
                  "and that exactly one operation failed -- not zero, and not the whole plan");
  checks.ExpectEq(tick.rows_advanced, std::size_t{0},
                  "no row moved forward, because nothing happened to a save");
  checks.Expect(tick.stored.ok(), "the baseline was still written: " + tick.stored.message);

  // Byte for byte, which is the strongest form of "nothing was corrupted": a
  // file that parsed but had lost the earlier tick's row would pass a weaker
  // check and cost that save a re-hash on every tick from here on.
  checks.ExpectEq(sandbox.Read(sync::kStateSdPath), before,
                  "and the baseline on the card is exactly the one that was there");
  const state::LoadedBaseline loaded = StoredBaseline(*files);
  checks.Expect(loaded.diagnostics.empty(),
                "it still reads back clean: " + loaded.DescribeDiagnostics());
  checks.Expect(loaded.value.Find(earlier.rom_id, earlier.slot) != nullptr,
                "with the earlier tick's row intact");
  checks.Expect(loaded.value.Find(rom.id, slot) == nullptr,
                "and no row for the save this tick never managed to upload");

  // The save itself, and the server's opinion of it. Neither moved.
  checks.ExpectEq(sandbox.Read(SavePath(name)), bytes, "the save is untouched");
  sync::SyncPlan again;
  if (PlanFor(checks, client, base, fixture, payload, {slot}, &again)) {
    checks.Expect(again.operations[0].action == sync::Action::kUpload,
                  std::string("the next tick still plans the upload that did not happen: ") +
                      sync::ToString(again.operations[0].action));
    harness::Complete(client, base, fixture, again.session_id, 0, 0);
  }
}

// --- refused ------------------------------------------------------------------
//
// The design note this issue turns on: complete is accounting, not the commit
// point. If the call fails, the baseline for the operations that *did* succeed
// must still be on the card -- otherwise the next tick re-uploads saves the
// server already has and RomM stamps each one as a new row.

void Refused(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "complete-refused");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-6-refused");
  const std::string name = "refused.srm";
  sandbox.SeedSave(SavePath(name), "uploaded before the accounting fell over\n");

  sync::ClientSaveState local;
  if (!LocalSaveOnCard(checks, *files, sandbox, rom.id, name, slot, &local)) {
    return;
  }
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(local);

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan) ||
      plan.operations.size() != 1) {
    checks.Expect(false, "the plan carries the one operation this run created");
    return;
  }
  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, ExecuteAt(1'757'000'000));
  checks.ExpectEq(report.completed, 1, "the upload landed on the server");

  std::int64_t save_id = 0;
  if (!report.operations.empty() && report.operations[0].save_id.has_value()) {
    save_id = *report.operations[0].save_id;
  }

  sync::TickCompletion tick;
  {
    // Every attempt, not one: a fault that auto-disarms after the first would be
    // absorbed by the retry, and the test would assert nothing.
    harness::Fault fault(checks, client, base,
                         R"({"mode":"status","status":500,"path":"/api/sync/sessions","count":9})");
    tick = sync::FinishTick(client, *files, TokenFor(base, fixture), plan, report, payload.saves,
                            /*previous=*/{}, FinishInstantly());
  }

  checks.Expect(!tick.ok(), "a completion the server refuses is a failed tick");
  checks.Expect(tick.reported.error == sync::CompleteError::kServerError,
                std::string("...named: ") + sync::ToString(tick.reported.error));
  checks.Expect(sync::ShouldRetry(tick.reported.error),
                "...and it is the kind a next schedule retries");
  checks.Expect(tick.reported.attempts > 1,
                "the 5xx was retried within the tick: " + std::to_string(tick.reported.attempts) +
                    " attempts");
  checks.Expect(tick.reported.waited.count() > 0, "with a backoff between them");

  // The whole point. The upload happened; the accounting did not.
  checks.Expect(tick.stored.ok(), "the baseline was still written: " + tick.stored.message);
  checks.ExpectEq(tick.stored.rows_written, static_cast<std::size_t>(1),
                  "with the row for the operation that succeeded");
  const state::LoadedBaseline loaded = StoredBaseline(*files);
  checks.Expect(loaded.diagnostics.empty(),
                "and it reads back clean: " + loaded.DescribeDiagnostics());
  const state::SaveRecord* row = loaded.value.Find(rom.id, slot);
  checks.Expect(row != nullptr && row->content_hash == *local.content_hash,
                "so the next tick does not re-upload a save the server already has");

  // The session was never closed, which is not a corrupted anything: the next
  // negotiate cancels it (docs/API_CONTRACT.md).
  harness::Complete(client, base, fixture, plan.session_id, 1, 0);
  if (save_id != 0) {
    harness::DeleteSave(client, base, fixture, save_id);
  }
}

// --- truncated ----------------------------------------------------------------
//
// A clean short body is a named parse error, never a half-read session. Half a
// session that parsed would be counts nobody can tell from the ones that were
// reported -- and the baseline, which is the part that matters, is on the card
// either way.

void Truncated(rig::Checks& checks, http::HttpClient& client, const std::string& base,
               const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "complete-truncated");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m2-6-truncated");
  const std::string name = "truncated.srm";
  sandbox.SeedSave(SavePath(name), "the bytes are fine; the receipt is not\n");

  sync::ClientSaveState local;
  if (!LocalSaveOnCard(checks, *files, sandbox, rom.id, name, slot, &local)) {
    return;
  }
  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(local);

  sync::SyncPlan plan;
  if (!PlanFor(checks, client, base, fixture, payload, {slot}, &plan) ||
      plan.operations.size() != 1) {
    checks.Expect(false, "the plan carries the one operation this run created");
    return;
  }
  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, SavePath(name), name}};
  const sync::ExecutionReport report = sync::ExecutePlan(
      client, *files, TokenFor(base, fixture), plan, targets, ExecuteAt(1'757'000'000));

  std::int64_t save_id = 0;
  if (!report.operations.empty() && report.operations[0].save_id.has_value()) {
    save_id = *report.operations[0].save_id;
  }

  sync::TickCompletion tick;
  {
    // 40 bytes of a real completion: `{"session":{"id":...` and nothing that
    // closes it. The server answered, the status was 200, and the body is not a
    // session.
    harness::Fault fault(
        checks, client, base,
        R"({"mode":"truncate","bytes":40,"path":"/api/sync/sessions","count":9})");
    tick = sync::FinishTick(client, *files, TokenFor(base, fixture), plan, report, payload.saves,
                            /*previous=*/{}, FinishInstantly());
  }

  checks.Expect(tick.reported.error == sync::CompleteError::kMalformed,
                std::string("a short body is a named parse error: ") +
                    sync::ToString(tick.reported.error) + " -- " + tick.reported.message);
  checks.ExpectEq(tick.reported.value.session.id, static_cast<std::int64_t>(0),
                  "and no session came out of it");
  checks.Expect(!sync::ShouldRetry(tick.reported.error),
                "a body that is not a session is not retried inside the tick");
  checks.Expect(tick.stored.ok(), "the baseline is on the card regardless: " + tick.stored.message);

  harness::Complete(client, base, fixture, plan.session_id, 1, 0);
  if (save_id != 0) {
    harness::DeleteSave(client, base, fixture, save_id);
  }
}

}  // namespace

int main(int argc, char** argv) {
  // The durability hook the sysmodule installs from its own `main` (M2-7). A
  // suite that left it null would be proving the weaker of the two promises
  // `io::CopyAtomically` can make, on the very path a backup depends on.
  rommsync::host::InstallPosixFileSync();

  const std::string scenario = argc > 1 ? argv[1] : "counts";
  const std::string base = rig::BaseUrl();

  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);

  // The tally lives here, above every scenario, so a `Sandbox`'s teardown audit
  // reports into an object that outlives it -- see `harness::Sandbox`.
  rig::Checks checks;

  if (scenario == "counts" || scenario == "parse" || scenario == "stamps" ||
      scenario == "advance") {
    if (scenario == "counts") {
      Counts(checks);
    } else if (scenario == "parse") {
      Parse(checks);
    } else if (scenario == "stamps") {
      Stamps(checks);
    } else {
      Advance(checks);
    }
    if (checks.failures() == 0) {
      std::cout << "complete." << scenario << " ok\n";
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

  if (scenario == "session") {
    Session(checks, *client, base, fixture, rom);
  } else if (scenario == "unchanged") {
    Unchanged(checks, *client, base, fixture, rom);
  } else if (scenario == "reported") {
    Reported(checks, *client, base, fixture, rom);
  } else if (scenario == "revoked") {
    Revoked(checks, *client, base, fixture, rom);
  } else if (scenario == "refused") {
    Refused(checks, *client, base, fixture, rom);
  } else if (scenario == "truncated") {
    Truncated(checks, *client, base, fixture, rom);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  rig::DisarmFault(*client, base);
  harness::ExpectDisarmed(checks, *client, base, "the scenario left the proxy disarmed");

  if (checks.failures() == 0) {
    std::cout << "complete." << scenario << " ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
