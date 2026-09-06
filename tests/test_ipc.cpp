// The IPC contract: the payloads, the dispatch table, and `ServiceCore` driven
// end to end against the real docker RomM.
//
// One scenario per CTest entry (`ipc.version`, `ipc.refuses`, ...), selected by
// argv[1], so a red run names the behaviour that broke.
//
// Everything except `ipc.engine` needs no server and must stay checked with
// Docker stopped: the guarantees are about what a decoder does with a payload
// somebody else wrote -- a truncated one, an over-long one, one from a build
// that does not exist yet -- and those are exactly the cases a running RomM
// cannot help with. `ipc.engine` is the other half: the commands the overlay
// actually presses, against a live 5.2.0, through a real pairing.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "harness.hpp"
#include "rig.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/json.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/token_store.hpp"

namespace {

namespace auth = rommsync::auth;
namespace config = rommsync::config;
namespace http = rommsync::http;
namespace ipc = rommsync::ipc;
namespace json = rommsync::json;
namespace sync = rommsync::sync;

using namespace std::chrono_literals;

// --- an engine that answers whatever a scenario needs -------------------------

/// The `ipc::Engine` the unit scenarios drive `ServiceCore` through.
///
/// Every field is a knob rather than a behaviour, because what is under test is
/// `ServiceCore`'s decisions -- which `SyncNow` outcome, what the effective
/// enabled state is, what a page size gets clamped to -- and an engine that had
/// opinions of its own would be a second place for those to be decided.
class FakeEngine : public ipc::Engine {
 public:
  config::Config settings = config::Defaults();
  std::vector<config::Diagnostic> notes;
  ipc::EngineSnapshot snapshot;
  auth::PairingStatus pairing;

  ipc::Error set_enabled_error = ipc::Error::kOk;
  /// False models a write that reported success and did not take -- which is
  /// what `SetEnabled` answering with the *effective* state exists to catch.
  bool set_enabled_applies = true;

  ipc::Error apply_edit_error = ipc::Error::kOk;
  std::vector<config::Diagnostic> apply_edit_notes;
  ipc::ConfigEdit last_edit;

  bool sync_accepted = true;
  int sync_requests = 0;

  ipc::Error start_pairing_error = ipc::Error::kOk;
  ipc::Error unpair_error = ipc::Error::kOk;

  ipc::Error enqueue_error = ipc::Error::kOk;
  std::int32_t enqueue_position = 1;
  std::int64_t last_rom_id = 0;
  ipc::Error dequeue_error = ipc::Error::kOk;

  ipc::Error list_begin_error = ipc::Error::kOk;
  ipc::Cursor issued_cursor = 7;
  ipc::ListRequest last_list;
  ipc::ListPage page;
  ipc::Error list_next_error = ipc::Error::kOk;
  ipc::Error list_end_error = ipc::Error::kOk;
  ipc::Cursor last_cursor = 0;

  const config::Config& config() const override { return settings; }
  const std::vector<config::Diagnostic>& config_diagnostics() const override { return notes; }
  ipc::EngineSnapshot Snapshot() const override { return snapshot; }
  auth::PairingStatus pairing_status() const override { return pairing; }

  ipc::Error SetSyncEnabled(bool enabled) override {
    if (set_enabled_applies) {
      settings.sync.enabled = enabled;
    }
    return set_enabled_error;
  }

  ipc::Error ApplyConfigEdit(const ipc::ConfigEdit& edit,
                             std::vector<config::Diagnostic>* diagnostics) override {
    last_edit = edit;
    *diagnostics = apply_edit_notes;
    return apply_edit_error;
  }

  bool RequestSync() override {
    ++sync_requests;
    return sync_accepted;
  }

  ipc::Error StartPairing() override { return start_pairing_error; }
  ipc::Error Unpair() override { return unpair_error; }

  ipc::Error Enqueue(std::int64_t rom_id, std::int32_t* position) override {
    last_rom_id = rom_id;
    if (enqueue_error == ipc::Error::kOk) {
      *position = enqueue_position;
    }
    return enqueue_error;
  }

  ipc::Error Dequeue(std::int64_t rom_id) override {
    last_rom_id = rom_id;
    return dequeue_error;
  }

  ipc::Error ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor) override {
    last_list = request;
    if (list_begin_error == ipc::Error::kOk) {
      *cursor = issued_cursor;
    }
    return list_begin_error;
  }

  ipc::Error ListNext(ipc::Cursor cursor, ipc::ListPage* out) override {
    last_cursor = cursor;
    if (list_next_error == ipc::Error::kOk) {
      *out = page;
    }
    return list_next_error;
  }

  ipc::Error ListEnd(ipc::Cursor cursor) override {
    last_cursor = cursor;
    return list_end_error;
  }
};

// --- shared fixtures ----------------------------------------------------------

/// A status with every field carrying a value that is not its default, so a
/// round trip that dropped one is visible rather than accidentally correct.
ipc::Status LoadedStatus() {
  ipc::Status status;
  status.interface = ipc::kVersion;
  status.build = "9.9.9-test";
  status.enabled = true;
  status.auth = ipc::AuthState::kUnauthenticated;
  status.configured = true;
  status.online = true;
  status.last_sync_at = 1757000000;
  status.last_sync_result = ipc::SyncResult::kPartial;
  status.sync_in_progress = true;
  status.uploaded = 3;
  status.downloaded = 5;
  status.conflicts = 1;
  status.failed = 2;
  status.queue_depth = 4;
  status.config_error_count = 2;
  status.download.state = ipc::DownloadState::kVerifying;
  status.download.rom_id = 42;
  status.download.fs_name = "Some Game (USA).gba";
  status.download.bytes_done = 123456;
  status.download.bytes_total = 654321;
  return status;
}

ipc::ListPage LoadedPage() {
  ipc::ListPage page;
  ipc::ListItem item;
  item.fields.push_back({"id", ipc::ListValue::Integer(42)});
  item.fields.push_back({"name", ipc::ListValue::Text("Some Game")});
  item.fields.push_back({"has_multiple_files", ipc::ListValue::Flag(true)});
  page.items.push_back(item);
  ipc::ListItem second;
  second.fields.push_back({"id", ipc::ListValue::Integer(43)});
  second.fields.push_back({"name", ipc::ListValue::Text("")});
  page.items.push_back(second);
  page.has_more = true;
  return page;
}

ipc::ConfigView LoadedConfigView() {
  ipc::ConfigView view;
  view.config = config::Defaults();
  view.config.server.url = "http://romm.lan:8080/romm";
  view.config.sync.states = true;
  view.config.sync.interval_min = 45;
  view.config.downloads.resume = false;
  config::Diagnostic note;
  note.severity = config::Severity::kWarning;
  note.line = 9;
  note.section = "sync";
  note.key = "states";
  note.message = "expected true or false";
  view.diagnostics.push_back(note);
  return view;
}

bool SameDiagnostics(const std::vector<config::Diagnostic>& a,
                     const std::vector<config::Diagnostic>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t at = 0; at < a.size(); ++at) {
    if (a[at].severity != b[at].severity || a[at].line != b[at].line ||
        a[at].section != b[at].section || a[at].key != b[at].key ||
        a[at].message != b[at].message) {
      return false;
    }
  }
  return true;
}

bool SamePlatforms(const config::Config& a, const config::Config& b) {
  if (a.platforms.size() != b.platforms.size()) {
    return false;
  }
  for (const auto& [slug, folders] : a.platforms) {
    const config::PlatformFolders* other = b.Platform(slug);
    if (other == nullptr || other->roms != folders.roms || other->saves != folders.saves ||
        other->states != folders.states) {
      return false;
    }
  }
  return true;
}

/// Every field of a JSON document, keys included, flattened for a scan.
void CollectKeys(const json::Value& value, std::vector<std::string>* keys) {
  if (value.is_object()) {
    for (const json::Member& member : value.members()) {
      keys->push_back(member.key);
      CollectKeys(member.value, keys);
    }
    return;
  }
  if (value.is_array()) {
    for (const json::Value& element : value.elements()) {
      CollectKeys(element, keys);
    }
  }
}

// --- ipc.version --------------------------------------------------------------

/// Command 0's encoding is frozen, and this test is the thing that freezes it.
///
/// An overlay from a newer release meets an older sysmodule on somebody's SD
/// card, and this is the one call it makes before it knows whether it can decode
/// anything else. If these bytes ever change, every build already shipped is
/// unable to say "update the sysmodule" -- it decodes garbage instead. So the
/// literal is written out here rather than derived: a test that computed the
/// expected string from the encoder would pass through any change to it.
int Version() {
  checks::Checks checks;

  checks.ExpectEq(ipc::EncodeInterfaceVersion(1), std::string("{\"interface\":1}"),
                  "command 0's encoding is exactly this, forever");
  checks.ExpectEq(ipc::EncodeInterfaceVersion(4294967295u),
                  std::string("{\"interface\":4294967295}"), "and holds a whole u32");
  // 2 since M4-1 (#23): `Status` gained two fields the decoder requires, and a
  // required field is an incompatible change by this header's own rule. What is
  // frozen is command 0's *encoding*, asserted above -- not the number in it.
  checks.ExpectEq(static_cast<std::uint64_t>(ipc::kVersion), std::uint64_t{2},
                  "the interface version this build speaks");
  checks.ExpectEq(static_cast<int>(ipc::Command::kGetInterfaceVersion), 0,
                  "GetInterfaceVersion is command 0");

  const ipc::Decoded<std::uint32_t> back =
      ipc::DecodeInterfaceVersion(ipc::EncodeInterfaceVersion(ipc::kVersion));
  checks.Expect(back.ok(), "it round trips: " + back.error.Describe());
  checks.ExpectEq(static_cast<std::uint64_t>(back.value), static_cast<std::uint64_t>(ipc::kVersion),
                  "and survives");

  // The case the freeze is for: an answer from a build that does not exist yet.
  const ipc::Decoded<std::uint32_t> future = ipc::DecodeInterfaceVersion("{\"interface\":9999}");
  checks.Expect(future.ok(), "a future interface version still decodes");
  checks.ExpectEq(static_cast<std::uint64_t>(future.value), std::uint64_t{9999},
                  "as the number it is, so the overlay can compare it");

  FakeEngine engine;
  ipc::ServiceCore core(engine);
  std::string response;
  checks.Expect(ipc::Dispatch(core, 0, "{}", &response) == ipc::Error::kOk,
                "and command 0 answers over the dispatch table");
  checks.ExpectEq(response, ipc::EncodeInterfaceVersion(ipc::kVersion),
                  "with exactly the frozen payload");
  return checks.failures();
}

// --- ipc.commands -------------------------------------------------------------

/// The command table, and the document that publishes it.
///
/// The ids are the one part of this contract that cannot be re-derived: the
/// overlay and the sysmodule ship as separate files, so an id that changed
/// meaning is a call that does something other than what the caller asked. The
/// table in docs/DEVELOPMENT.md#ipc is what the other worktrees read, so it is
/// parsed here rather than restated -- a renumbering has to move both or go red.
int Commands() {
  checks::Checks checks;

  for (std::size_t at = 0; at < std::size(ipc::kAllCommands); ++at) {
    checks.ExpectEq(static_cast<std::size_t>(ipc::kAllCommands[at]), at,
                    "command ids are dense and in order");
  }
  checks.Expect(!ipc::IsCommand(static_cast<std::uint32_t>(std::size(ipc::kAllCommands))),
                "one past the last id is not a command");
  checks.Expect(!ipc::IsCommand(0xFFFFFFFF), "and neither is a wild one");

  std::ifstream doc(ROMMSYNC_DEVELOPMENT_DOC);
  checks.Expect(doc.good(), "docs/DEVELOPMENT.md is readable");
  std::string line;
  std::vector<std::pair<int, std::string>> documented;
  bool in_ipc = false;
  while (std::getline(doc, line)) {
    if (line.rfind("## ", 0) == 0) {
      in_ipc = line == "## IPC";
      continue;
    }
    if (!in_ipc || line.empty() || line[0] != '|') {
      continue;
    }
    // `| 4 | `SetEnabled` | ... |`
    std::size_t at = 1;
    while (at < line.size() && line[at] == ' ') {
      ++at;
    }
    if (at >= line.size() || line[at] < '0' || line[at] > '9') {
      continue;
    }
    int id = 0;
    while (at < line.size() && line[at] >= '0' && line[at] <= '9') {
      id = id * 10 + (line[at] - '0');
      ++at;
    }
    const std::size_t open = line.find('`', at);
    const std::size_t close = open == std::string::npos ? std::string::npos : line.find('`', open + 1);
    if (close == std::string::npos) {
      continue;
    }
    documented.push_back({id, line.substr(open + 1, close - open - 1)});
  }

  checks.ExpectEq(documented.size(), std::size(ipc::kAllCommands),
                  "every command is in the documented table, and nothing else is");
  for (const auto& [id, name] : documented) {
    ipc::Command command = ipc::Command::kGetInterfaceVersion;
    if (!ipc::IsCommand(static_cast<std::uint32_t>(id), &command)) {
      checks.Expect(false, "the document names an id this build does not implement: " + name);
      continue;
    }
    checks.ExpectEq(std::string(ipc::ToString(command)), name,
                    "id " + std::to_string(id) + " is the command the document says it is");
  }
  return checks.failures();
}

// --- ipc.roundtrip ------------------------------------------------------------

/// Every payload survives its own encoder and decoder, whole.
///
/// Lossless is the bar, not "close enough": the encoder runs in the sysmodule
/// and the decoder in the overlay, so a field that came back with its default is
/// a screen drawn from a value nobody chose -- an enable switch in the wrong
/// position, a queue depth of zero over four pending downloads.
int RoundTrip() {
  checks::Checks checks;

  {
    const ipc::Status sent = LoadedStatus();
    const ipc::Decoded<ipc::Status> back = ipc::DecodeStatus(ipc::EncodeStatus(sent));
    checks.Expect(back.ok(), "a status round trips: " + back.error.Describe());
    checks.ExpectEq(back.value.build, sent.build, "build survives");
    checks.ExpectEq(back.value.enabled, sent.enabled, "enabled survives");
    checks.ExpectEq(std::string(ipc::ToString(back.value.auth)), std::string(ipc::ToString(sent.auth)),
                    "the auth state survives");
    checks.ExpectEq(back.value.configured, sent.configured, "configured survives");
    checks.ExpectEq(back.value.online, sent.online, "online survives");
    checks.ExpectEq(back.value.last_sync_at, sent.last_sync_at, "last_sync_at survives");
    checks.ExpectEq(std::string(ipc::ToString(back.value.last_sync_result)),
                    std::string(ipc::ToString(sent.last_sync_result)), "the result survives");
    checks.ExpectEq(back.value.sync_in_progress, sent.sync_in_progress,
                    "sync_in_progress survives");
    checks.ExpectEq(back.value.uploaded, sent.uploaded, "uploaded survives");
    checks.ExpectEq(back.value.downloaded, sent.downloaded, "downloaded survives");
    checks.ExpectEq(back.value.conflicts, sent.conflicts, "conflicts survive");
    checks.ExpectEq(back.value.failed, sent.failed, "failed survives");
    checks.ExpectEq(back.value.queue_depth, sent.queue_depth, "the queue depth survives");
    checks.ExpectEq(back.value.config_error_count, sent.config_error_count,
                    "config_error_count survives");
    checks.ExpectEq(std::string(ipc::ToString(back.value.download.state)),
                    std::string(ipc::ToString(sent.download.state)), "the download state survives");
    checks.ExpectEq(back.value.download.rom_id, sent.download.rom_id, "its rom_id survives");
    checks.ExpectEq(back.value.download.fs_name, sent.download.fs_name, "its fs_name survives");
    checks.ExpectEq(back.value.download.bytes_done, sent.download.bytes_done, "bytes_done survives");
    checks.ExpectEq(back.value.download.bytes_total, sent.download.bytes_total,
                    "bytes_total survives");

    // A default-constructed one is mostly zeroes and empty strings, which is the
    // shape a strict reader refuses if nobody thought about it -- and it is what
    // the overlay asks for on the first frame after boot.
    const ipc::Decoded<ipc::Status> empty = ipc::DecodeStatus(ipc::EncodeStatus(ipc::Status{}));
    checks.Expect(empty.ok(), "a fresh status round trips too: " + empty.error.Describe());
  }

  // Every enum spelling decodes back to itself. The tables live in ipc.cpp; this
  // walks them from the outside, so a `ToString` that grew a case without the
  // decoder growing one is red here rather than on a console.
  for (const ipc::AuthState state :
       {ipc::AuthState::kNeverPaired, ipc::AuthState::kUnauthenticated, ipc::AuthState::kPaired}) {
    ipc::Status status;
    status.auth = state;
    const ipc::Decoded<ipc::Status> back = ipc::DecodeStatus(ipc::EncodeStatus(status));
    checks.Expect(back.ok() && back.value.auth == state,
                  std::string("auth state survives: ") + ipc::ToString(state));
  }
  for (const ipc::SyncResult result : {ipc::SyncResult::kNever, ipc::SyncResult::kOk,
                                       ipc::SyncResult::kPartial, ipc::SyncResult::kFailed}) {
    ipc::Status status;
    status.last_sync_result = result;
    const ipc::Decoded<ipc::Status> back = ipc::DecodeStatus(ipc::EncodeStatus(status));
    checks.Expect(back.ok() && back.value.last_sync_result == result,
                  std::string("sync result survives: ") + ipc::ToString(result));
  }
  for (const ipc::DownloadState state :
       {ipc::DownloadState::kIdle, ipc::DownloadState::kQueued, ipc::DownloadState::kDownloading,
        ipc::DownloadState::kVerifying, ipc::DownloadState::kFailed}) {
    ipc::Status status;
    status.download.state = state;
    const ipc::Decoded<ipc::Status> back = ipc::DecodeStatus(ipc::EncodeStatus(status));
    checks.Expect(back.ok() && back.value.download.state == state,
                  std::string("download state survives: ") + ipc::ToString(state));
  }
  for (const ipc::SyncOutcome outcome :
       {ipc::SyncOutcome::kAccepted, ipc::SyncOutcome::kAlreadyRunning,
        ipc::SyncOutcome::kNotConfigured, ipc::SyncOutcome::kUnauthenticated,
        ipc::SyncOutcome::kDisabled}) {
    const ipc::Decoded<ipc::SyncOutcome> back =
        ipc::DecodeSyncOutcome(ipc::EncodeSyncOutcome(outcome));
    checks.Expect(back.ok() && back.value == outcome,
                  std::string("sync outcome survives: ") + ipc::ToString(outcome));
  }
  for (const ipc::ListKind kind : {ipc::ListKind::kPlatforms, ipc::ListKind::kRoms,
                                   ipc::ListKind::kQueue}) {
    ipc::ListRequest request;
    request.kind = kind;
    request.page_size = 7;
    const ipc::Decoded<ipc::ListRequest> back =
        ipc::DecodeListRequest(ipc::EncodeListRequest(request));
    checks.Expect(back.ok() && back.value.kind == kind,
                  std::string("list kind survives: ") + ipc::ToString(kind));
  }
  for (const config::Severity severity :
       {config::Severity::kNotice, config::Severity::kWarning, config::Severity::kError}) {
    config::Diagnostic note;
    note.severity = severity;
    note.message = "something";
    ipc::ConfigResult sent;
    sent.diagnostics.push_back(note);
    const ipc::Decoded<ipc::ConfigResult> back =
        ipc::DecodeConfigResult(ipc::EncodeConfigResult(sent));
    checks.Expect(back.ok() && back.value.diagnostics.size() == 1 &&
                      back.value.diagnostics[0].severity == severity,
                  std::string("severity survives: ") + config::ToString(severity));
  }

  {
    const ipc::ConfigView sent = LoadedConfigView();
    const ipc::Decoded<ipc::ConfigView> back = ipc::DecodeConfigView(ipc::EncodeConfigView(sent));
    checks.Expect(back.ok(), "a config round trips: " + back.error.Describe());
    checks.ExpectEq(back.value.config.server.url, sent.config.server.url, "the url survives");
    checks.ExpectEq(back.value.config.sync.interval_min, sent.config.sync.interval_min,
                    "the interval survives");
    checks.ExpectEq(back.value.config.sync.states, sent.config.sync.states, "states survives");
    checks.ExpectEq(back.value.config.downloads.resume, sent.config.downloads.resume,
                    "resume survives");
    checks.Expect(SamePlatforms(sent.config, back.value.config), "the whole folder map survives");
    checks.Expect(SameDiagnostics(sent.diagnostics, back.value.diagnostics),
                  "and every diagnostic with it");
    checks.ExpectEq(back.value.platforms_truncated, false, "nothing was truncated");
  }

  {
    ipc::ConfigEdit sent;
    sent.assignments.push_back({"sync", "states", "true", false});
    sent.assignments.push_back({"platform.snes", "roms", "", true});
    const ipc::Decoded<ipc::ConfigEdit> back = ipc::DecodeConfigEdit(ipc::EncodeConfigEdit(sent));
    checks.Expect(back.ok(), "an edit round trips: " + back.error.Describe());
    checks.ExpectEq(back.value.assignments.size(), sent.assignments.size(), "both assignments");
    if (back.value.assignments.size() == 2) {
      checks.ExpectEq(back.value.assignments[0].value, std::string("true"), "the value survives");
      checks.ExpectEq(back.value.assignments[1].remove, true, "and so does a removal");
      checks.ExpectEq(back.value.assignments[1].section, std::string("platform.snes"),
                      "and the section it is in");
    }
    const ipc::Decoded<ipc::ConfigEdit> none = ipc::DecodeConfigEdit(ipc::EncodeConfigEdit({}));
    checks.Expect(none.ok() && none.value.assignments.empty(), "an empty edit is a legal edit");
  }

  for (const ipc::WriteOutcome outcome : {ipc::WriteOutcome::kApplied, ipc::WriteOutcome::kInvalid,
                                          ipc::WriteOutcome::kWriteFailed}) {
    ipc::ConfigResult config_result;
    config_result.outcome = outcome;
    config_result.diagnostics = LoadedConfigView().diagnostics;
    const ipc::Decoded<ipc::ConfigResult> config_back =
        ipc::DecodeConfigResult(ipc::EncodeConfigResult(config_result));
    checks.Expect(config_back.ok() && config_back.value.outcome == outcome,
                  std::string("a config outcome survives: ") + ipc::ToString(outcome));
    checks.Expect(config_back.ok() &&
                      SameDiagnostics(config_back.value.diagnostics, config_result.diagnostics),
                  "with its diagnostics -- which on a refusal are the whole answer");

    for (const bool enabled : {true, false}) {
      const ipc::EnabledResult enabled_result{outcome, enabled};
      const ipc::Decoded<ipc::EnabledResult> enabled_back =
          ipc::DecodeEnabledResult(ipc::EncodeEnabledResult(enabled_result));
      checks.Expect(enabled_back.ok() && enabled_back.value.outcome == outcome &&
                        enabled_back.value.enabled == enabled,
                    std::string("an enabled result survives: ") + ipc::ToString(outcome));
    }
  }

  {
    for (const bool enabled : {true, false}) {
      const ipc::Decoded<bool> back = ipc::DecodeEnabled(ipc::EncodeEnabled(enabled));
      checks.Expect(back.ok() && back.value == enabled, "the enable flag survives");
    }
    const ipc::Decoded<std::int64_t> rom = ipc::DecodeRomId(ipc::EncodeRomId(9007199254740993));
    checks.Expect(!rom.ok(), "a rom id past this contract's bound is refused");
    const ipc::Decoded<std::int64_t> real = ipc::DecodeRomId(ipc::EncodeRomId(42));
    checks.Expect(real.ok() && real.value == 42, "a rom id survives");
    const ipc::Decoded<std::int32_t> position =
        ipc::DecodeQueuePosition(ipc::EncodeQueuePosition(17));
    checks.Expect(position.ok() && position.value == 17, "a queue position survives");
    const ipc::Decoded<ipc::Cursor> cursor = ipc::DecodeCursor(ipc::EncodeCursor(4294967295u));
    checks.Expect(cursor.ok() && cursor.value == 4294967295u, "a cursor survives, whole");
    checks.Expect(ipc::DecodeEmpty(ipc::EncodeEmpty()).ok(), "and so does nothing at all");
  }

  {
    const ipc::ListRequest sent{ipc::ListKind::kRoms, 3, "zelda", 12};
    const ipc::Decoded<ipc::ListRequest> back =
        ipc::DecodeListRequest(ipc::EncodeListRequest(sent));
    checks.Expect(back.ok(), "a list request round trips: " + back.error.Describe());
    checks.ExpectEq(back.value.platform_id, sent.platform_id, "the platform filter survives");
    checks.ExpectEq(back.value.search, sent.search, "the search term survives");
    checks.ExpectEq(back.value.page_size, sent.page_size, "the page size survives");
  }

  {
    const ipc::ListPage sent = LoadedPage();
    const ipc::Decoded<ipc::ListPage> back = ipc::DecodeListPage(ipc::EncodeListPage(sent));
    checks.Expect(back.ok(), "a page round trips: " + back.error.Describe());
    checks.ExpectEq(back.value.items.size(), sent.items.size(), "both items");
    checks.ExpectEq(back.value.has_more, sent.has_more, "has_more survives");
    checks.ExpectEq(back.value.pending, sent.pending, "pending survives");
    if (back.value.items.size() == 2) {
      const ipc::ListValue* id = back.value.items[0].Find("id");
      const ipc::ListValue* name = back.value.items[0].Find("name");
      const ipc::ListValue* multi = back.value.items[0].Find("has_multiple_files");
      checks.Expect(id != nullptr && *id == ipc::ListValue::Integer(42), "an integer field survives");
      checks.Expect(name != nullptr && *name == ipc::ListValue::Text("Some Game"),
                    "a string field survives");
      checks.Expect(multi != nullptr && *multi == ipc::ListValue::Flag(true),
                    "a boolean field survives");
      const ipc::ListValue* blank = back.value.items[1].Find("name");
      checks.Expect(blank != nullptr && *blank == ipc::ListValue::Text(""),
                    "and an empty string is a value, not an absence");
    }
    const ipc::Decoded<ipc::ListPage> empty = ipc::DecodeListPage(ipc::EncodeListPage({}));
    checks.Expect(empty.ok() && empty.value.items.empty(), "an empty page is a page");
  }

  // Every string field is user data and may hold anything a filesystem allows.
  // `json::Quote` is what carries it rather than interpreting it, and this is
  // the check that nothing here builds a payload by concatenation instead.
  {
    ipc::Status status;
    status.download.fs_name = "quote\" backslash\\ tab\t newline\n bracket} \xc3\xa9";
    status.build = "\"}{\"";
    const ipc::Decoded<ipc::Status> back = ipc::DecodeStatus(ipc::EncodeStatus(status));
    checks.Expect(back.ok(), "a hostile file name round trips: " + back.error.Describe());
    checks.ExpectEq(back.value.download.fs_name, status.download.fs_name, "byte for byte");
    checks.ExpectEq(back.value.build, status.build, "and so does a hostile build string");
  }
  return checks.failures();
}

// --- ipc.refuses --------------------------------------------------------------

/// One decoder, named, and a canonical payload it accepts.
struct Decoder {
  const char* name;
  std::string canonical;
  std::function<bool(std::string_view)> accepts;
};

std::vector<Decoder> AllDecoders() {
  std::vector<Decoder> decoders;
  decoders.push_back({"interface version", ipc::EncodeInterfaceVersion(ipc::kVersion),
                      [](std::string_view text) { return ipc::DecodeInterfaceVersion(text).ok(); }});
  decoders.push_back({"status", ipc::EncodeStatus(LoadedStatus()),
                      [](std::string_view text) { return ipc::DecodeStatus(text).ok(); }});
  decoders.push_back({"config", ipc::EncodeConfigView(LoadedConfigView()),
                      [](std::string_view text) { return ipc::DecodeConfigView(text).ok(); }});
  ipc::ConfigResult config_result;
  config_result.outcome = ipc::WriteOutcome::kInvalid;
  config_result.diagnostics = LoadedConfigView().diagnostics;
  decoders.push_back({"config result", ipc::EncodeConfigResult(config_result),
                      [](std::string_view text) { return ipc::DecodeConfigResult(text).ok(); }});
  decoders.push_back({"enabled result",
                      ipc::EncodeEnabledResult({ipc::WriteOutcome::kWriteFailed, true}),
                      [](std::string_view text) { return ipc::DecodeEnabledResult(text).ok(); }});
  ipc::ConfigEdit edit;
  edit.assignments.push_back({"sync", "states", "true", false});
  decoders.push_back({"config edit", ipc::EncodeConfigEdit(edit),
                      [](std::string_view text) { return ipc::DecodeConfigEdit(text).ok(); }});
  decoders.push_back({"enabled", ipc::EncodeEnabled(true),
                      [](std::string_view text) { return ipc::DecodeEnabled(text).ok(); }});
  decoders.push_back({"sync outcome", ipc::EncodeSyncOutcome(ipc::SyncOutcome::kDisabled),
                      [](std::string_view text) { return ipc::DecodeSyncOutcome(text).ok(); }});
  decoders.push_back({"rom id", ipc::EncodeRomId(42),
                      [](std::string_view text) { return ipc::DecodeRomId(text).ok(); }});
  decoders.push_back({"queue position", ipc::EncodeQueuePosition(3),
                      [](std::string_view text) { return ipc::DecodeQueuePosition(text).ok(); }});
  decoders.push_back({"list request", ipc::EncodeListRequest({ipc::ListKind::kRoms, 3, "z", 8}),
                      [](std::string_view text) { return ipc::DecodeListRequest(text).ok(); }});
  decoders.push_back({"cursor", ipc::EncodeCursor(9),
                      [](std::string_view text) { return ipc::DecodeCursor(text).ok(); }});
  decoders.push_back({"list page", ipc::EncodeListPage(LoadedPage()),
                      [](std::string_view text) { return ipc::DecodeListPage(text).ok(); }});
  decoders.push_back({"empty", ipc::EncodeEmpty(),
                      [](std::string_view text) { return ipc::DecodeEmpty(text).ok(); }});
  return decoders;
}

/// A payload nobody on this side wrote.
///
/// Every one of these arrives across a process boundary from a binary that may
/// be a different release, and the rule is the same one `json::Parse` is built
/// on: a truncated or hostile buffer becomes a named refusal, never a struct
/// that looks parsed. The two things checked here are that a bad payload is
/// refused *and* that refusing it reads no further than the buffer -- which is
/// what running every prefix and every single-byte edit through every decoder
/// demonstrates.
int Refuses() {
  checks::Checks checks;
  const std::vector<Decoder> decoders = AllDecoders();

  for (const Decoder& decoder : decoders) {
    checks.Expect(decoder.accepts(decoder.canonical),
                  std::string("the canonical ") + decoder.name + " payload is accepted");

    // Truncated. Every proper prefix of a complete object is incomplete, so
    // every one of them has to be refused -- a short read that decoded would be
    // a half-payload the caller cannot tell from a whole one.
    for (std::size_t length = 0; length < decoder.canonical.size(); ++length) {
      if (decoder.accepts(std::string_view(decoder.canonical).substr(0, length))) {
        checks.Expect(false, std::string("a truncated ") + decoder.name + " payload was accepted (" +
                                 std::to_string(length) + " bytes)");
        break;
      }
    }

    // Over-long: past `kMaxPayloadBytes` it is refused before it is parsed, so a
    // peer cannot make this side spend a parse proportional to what it sent.
    const std::string pad = json::Quote("pad") + ":" +
                            json::Quote(std::string(ipc::kMaxPayloadBytes, 'x'));
    std::string padded = decoder.canonical;
    padded.insert(padded.size() - 1, decoder.canonical == "{}" ? pad : "," + pad);
    checks.Expect(!decoder.accepts(padded),
                  std::string("an over-long ") + decoder.name + " payload is refused");

    // Not an object at all.
    for (const char* nonsense : {"", "[]", "\"text\"", "17", "null", "true", "{", "}", "{},{}"}) {
      checks.Expect(!decoder.accepts(nonsense), std::string(decoder.name) +
                                                    " refuses a payload that is not an object: " +
                                                    nonsense);
    }

    // Every single-byte edit, run for its effect on the process rather than for
    // a verdict: what is under test is that the decoder *answers*. A read past
    // the end, an abort or a hang fails the run outright, which is the only way
    // "never reads out of bounds" can be shown from outside. Whether a given
    // edit is accepted is not interesting -- several are legal JSON carrying a
    // different value, and the wrong-type table below is what pins those.
    for (std::size_t at = 0; at < decoder.canonical.size(); ++at) {
      for (const char replacement : {'\0', '"', '\\', '{', '}', '[', ']', ':', ',', '-', 'x', '9'}) {
        std::string mutated = decoder.canonical;
        mutated[at] = replacement;
        (void)decoder.accepts(mutated);
      }
    }
  }

  // Wrong types, field by field, on the payload the status screen depends on.
  const std::string good = ipc::EncodeStatus(LoadedStatus());
  for (const auto& [find, replace] : std::vector<std::pair<std::string, std::string>>{
           {"\"enabled\":true", "\"enabled\":1"},
           {"\"enabled\":true", "\"enabled\":\"true\""},
           {"\"queue_depth\":4", "\"queue_depth\":\"4\""},
           {"\"queue_depth\":4", "\"queue_depth\":-1"},
           {"\"queue_depth\":4", "\"queue_depth\":4.5"},
           {"\"uploaded\":3", "\"uploaded\":99999999999"},
           {"\"last_sync_at\":1757000000", "\"last_sync_at\":-1"},
           {"\"auth\":\"unauthenticated\"", "\"auth\":\"sideways\""},
           {"\"auth\":\"unauthenticated\"", "\"auth\":null"},
           {"\"build\":\"9.9.9-test\"", "\"build\":42"},
           {"\"download\":{", "\"downloads\":{"},
       }) {
    const std::size_t at = good.find(find);
    checks.Expect(at != std::string::npos, "the fixture still holds " + find);
    if (at == std::string::npos) {
      continue;
    }
    std::string broken = good;
    broken.replace(at, find.size(), replace);
    checks.Expect(!ipc::DecodeStatus(broken).ok(), "a status is refused when " + replace);
  }

  // A missing field is refused rather than defaulted. Nothing here has a
  // sensible default: a `Status` whose `enabled` fell back to `false` is a
  // switch drawn in the wrong position.
  {
    const std::string without = good.substr(0, good.find(",\"queue_depth\"")) +
                                good.substr(good.find(",\"download\""));
    checks.Expect(!ipc::DecodeStatus(without).ok(), "a status missing a field is refused");
  }

  // The list page's one structural rule: a flat object of scalars.
  for (const char* page : {
           R"({"items":[{"id":{"nested":1}}],"has_more":false,"pending":false})",
           R"({"items":[{"id":[1,2]}],"has_more":false,"pending":false})",
           R"({"items":[{"id":null}],"has_more":false,"pending":false})",
           R"({"items":[{"id":1.5}],"has_more":false,"pending":false})",
           R"({"items":[[1]],"has_more":false,"pending":false})",
           R"({"items":{},"has_more":false,"pending":false})",
       }) {
    checks.Expect(!ipc::DecodeListPage(page).ok(),
                  std::string("a page holding something that is not a scalar is refused: ") + page);
  }

  // ...and its two size rules, which are what keep a page's cost predictable.
  {
    std::string many = R"({"items":[)";
    for (int at = 0; at <= ipc::kMaxPageSize; ++at) {
      many += at == 0 ? "" : ",";
      many += "{\"id\":" + std::to_string(at) + "}";
    }
    many += R"(],"has_more":false,"pending":false})";
    checks.Expect(!ipc::DecodeListPage(many).ok(), "a page past the item cap is refused");

    std::string wide = R"({"items":[{)";
    for (std::size_t at = 0; at <= ipc::kMaxItemFields; ++at) {
      wide += at == 0 ? "" : ",";
      wide += "\"f" + std::to_string(at) + "\":1";
    }
    wide += R"(}],"has_more":false,"pending":false})";
    checks.Expect(!ipc::DecodeListPage(wide).ok(), "an item past the field cap is refused");
  }

  // A `0` cursor is not a live cursor, so it is refused at the wire rather than
  // reaching the engine as an address that happens to miss.
  checks.Expect(!ipc::DecodeCursor(R"({"cursor":0})").ok(), "cursor 0 is refused");
  checks.Expect(!ipc::DecodeRomId(R"({"rom_id":0})").ok(), "rom_id 0 is refused");
  checks.Expect(!ipc::DecodeListRequest(
                     R"({"kind":"roms","platform_id":0,"search":"","page_size":0})")
                     .ok(),
                "a page size of zero is the one value that cannot mean anything");

  // A NUL inside a string is legal JSON and stops every C API downstream, so the
  // value that got used would not be the value that was checked.
  {
    ipc::Status status = LoadedStatus();
    status.download.fs_name = "harmless";
    std::string with_nul = ipc::EncodeStatus(status);
    const std::size_t at = with_nul.find("harmless");
    checks.Expect(at != std::string::npos, "the fixture still holds the file name");
    if (at != std::string::npos) {
      with_nul.replace(at, 8, "har\\u0000ss");
      checks.Expect(!ipc::DecodeStatus(with_nul).ok(), "a NUL inside a name is refused");
    }
  }
  return checks.failures();
}

// --- ipc.secrets --------------------------------------------------------------

/// Field names no payload on this wire may ever carry.
///
/// Checked as *keys*, at every depth, rather than as values -- because the way
/// this breaks is somebody plumbing a credential through the `Engine` to save a
/// round trip, and the field they add is the field that names it.
const char* const kForbiddenKeys[] = {
    "token", "access_token", "refresh_token", "device_code", "bearer",
    "password", "secret", "authorization", "api_key",
};

/// Nothing secret crosses this boundary, asserted over every command rather
/// than reviewed.
///
/// Two different rules, and they are not the same rule:
///
///   * A **credential** -- a bearer token, a `device_code` -- may not appear in
///     any payload at all. `PairingStatus` is already built that way
///     (pairing.hpp) and `Status` has no field for one; this is what keeps it
///     true after the next field is added.
///   * The **server URL** is a different thing. `NormalizeServerUrl` refuses
///     `user:password@` outright, so a configured URL carries no credential --
///     and the settings screen (#26) exists to show and edit it. So it is
///     served by `GetConfig`, and by nothing else: a URL in a diagnostic or in a
///     pairing message is the leak docs/SECURITY.md is about, since those go to
///     a log.
int Secrets() {
  checks::Checks checks;

  const std::string url = "http://romm.lan:8080/romm";
  const std::string token = "rommsync-fixture-bearer-token-do-not-leak";
  const std::string device_code = "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4";

  FakeEngine engine;
  engine.settings = config::Defaults();
  engine.settings.server.url = url;
  engine.snapshot.auth = ipc::AuthState::kPaired;
  engine.snapshot.download.state = ipc::DownloadState::kDownloading;
  engine.snapshot.download.rom_id = 7;
  engine.snapshot.download.fs_name = "Some Game (USA).gba";
  engine.pairing.state = auth::PairingState::kPending;
  engine.pairing.user_code = "ABCD2345";
  engine.pairing.verification_url = url + "/pair/device";
  engine.pairing.verification_url_complete = url + "/pair/device?user_code=ABCD2345";
  engine.page = LoadedPage();

  ipc::ServiceCore core(engine);

  const std::vector<std::pair<ipc::Command, std::string>> calls = {
      {ipc::Command::kGetInterfaceVersion, ipc::EncodeEmpty()},
      {ipc::Command::kGetStatus, ipc::EncodeEmpty()},
      {ipc::Command::kGetConfig, ipc::EncodeEmpty()},
      {ipc::Command::kSetConfig, ipc::EncodeConfigEdit({})},
      {ipc::Command::kSetEnabled, ipc::EncodeEnabled(true)},
      {ipc::Command::kSyncNow, ipc::EncodeEmpty()},
      {ipc::Command::kStartPair, ipc::EncodeEmpty()},
      {ipc::Command::kGetPairState, ipc::EncodeEmpty()},
      {ipc::Command::kUnpair, ipc::EncodeEmpty()},
      {ipc::Command::kEnqueue, ipc::EncodeRomId(42)},
      {ipc::Command::kDequeue, ipc::EncodeRomId(42)},
      {ipc::Command::kListBegin, ipc::EncodeListRequest({ipc::ListKind::kRoms, 1, "z", 8})},
      {ipc::Command::kListNext, ipc::EncodeCursor(7)},
      {ipc::Command::kListEnd, ipc::EncodeCursor(7)},
  };
  checks.ExpectEq(calls.size(), std::size(ipc::kAllCommands),
                  "every command is exercised, including any that was just added");

  for (const auto& [command, request] : calls) {
    const std::string name = ipc::ToString(command);
    std::string response;
    const ipc::Error error =
        ipc::Dispatch(core, static_cast<std::uint32_t>(command), request, &response);
    checks.Expect(error == ipc::Error::kOk,
                  name + " answers: " + ipc::ToString(error));
    checks.Expect(ipc::Fits(response), name + "'s answer is inside the payload cap");

    checks.Expect(response.find(token) == std::string::npos, name + " carries no bearer token");
    checks.Expect(response.find(device_code) == std::string::npos,
                  name + " carries no device_code");

    const json::ParseResult document = json::Parse(response);
    checks.Expect(document.ok(), name + " answers JSON: " + document.error.Describe());
    std::vector<std::string> keys;
    CollectKeys(document.value, &keys);
    for (const std::string& key : keys) {
      for (const char* forbidden : kForbiddenKeys) {
        checks.Expect(key != forbidden, name + " has no field named " + key);
      }
    }

    const bool has_url = response.find(url) != std::string::npos;
    if (command == ipc::Command::kGetConfig) {
      checks.Expect(has_url, "GetConfig does carry the server URL -- it is the field #26 edits");
    } else if (command == ipc::Command::kStartPair || command == ipc::Command::kGetPairState) {
      // The pairing screen has to put an absolute URL in front of the user, so
      // the two verification URLs are the deliberate exception -- and they are
      // built from the origin the user configured, not from a credential.
      checks.Expect(has_url, name + " carries the verification URL the human types");
      const json::Value* message = document.value.Find("message");
      checks.Expect(message != nullptr && message->is_string() &&
                        message->string().find(url) == std::string::npos,
                    name + "'s message names no server");
    } else {
      checks.Expect(!has_url, name + " does not carry the server URL");
    }
  }

  // The other half of the URL rule: a diagnostic goes to a log, so whatever
  // `config::Diagnostic` says about `server.url` must not quote it. That is
  // config's own guarantee; this is the assertion that the IPC layer does not
  // undo it by adding one of its own.
  {
    FakeEngine bad;
    bad.settings.server.url = std::string(ipc::kMaxServerUrlBytes + 1, 'u');
    ipc::ServiceCore withheld(bad);
    const ipc::ConfigView view = withheld.GetConfig();
    checks.Expect(view.config.server.url.empty(), "an absurd URL is withheld rather than sent");
    bool named = false;
    for (const config::Diagnostic& note : view.diagnostics) {
      named = named || (note.section == "server" && note.key == "url");
      checks.Expect(note.message.find(bad.settings.server.url) == std::string::npos,
                    "and no diagnostic quotes it back");
    }
    checks.Expect(named, "the user is told why the field is blank");
  }
  return checks.failures();
}

// --- ipc.caps -----------------------------------------------------------------

/// A folder map no payload could hold: the parser's own ceilings, all at once.
config::Config PathologicalConfig() {
  config::Config settings = config::Defaults();
  settings.server.url = "http://romm.lan:8080";
  for (std::size_t platform = 0; platform < config::kMaxPlatformSections; ++platform) {
    config::PlatformFolders folders;
    for (std::size_t path = 0; path < config::kMaxPathsPerKey; ++path) {
      folders.roms.push_back("/roms/" + std::to_string(platform) + "/" + std::to_string(path) +
                             std::string(64, 'd'));
    }
    settings.platforms["platform" + std::to_string(platform)] = folders;
  }
  return settings;
}

/// Nothing on this wire may grow with the size of the user's library or the size
/// of their `config.ini`. The heap is `0xC0000` with a download buffer and the
/// `state.db` baseline already claimed out of it, so a payload that scales with
/// a file is a payload that is one bad edit away from not being sendable.
///
/// `GetConfig` is the interesting case, because it is documented never to fail:
/// it has to answer *something* for a config nobody could send whole.
int Caps() {
  checks::Checks checks;

  {
    FakeEngine engine;
    ipc::ServiceCore core(engine);
    const std::string defaults = ipc::EncodeConfigView(core.GetConfig());
    checks.Expect(ipc::Fits(defaults),
                  "the shipped default folder map fits a payload (" +
                      std::to_string(defaults.size()) + " bytes)");
    checks.Expect(ipc::DecodeConfigView(defaults).ok(), "and comes back whole");
  }

  {
    FakeEngine engine;
    engine.settings = PathologicalConfig();
    ipc::ServiceCore core(engine);

    const ipc::ConfigView view = core.GetConfig();
    const std::string payload = ipc::EncodeConfigView(view);
    checks.Expect(ipc::Fits(payload), "a config nobody could send whole still produces a payload (" +
                                          std::to_string(payload.size()) + " bytes)");
    checks.Expect(view.platforms_truncated, "and says the folder map was left out");
    checks.Expect(view.config.platforms.empty(), "rather than serving a partial one");
    checks.ExpectEq(view.config.sync.interval_min, engine.settings.sync.interval_min,
                    "the scalar sections are still served");
    checks.ExpectEq(view.config.server.url, engine.settings.server.url, "and so is the server");
    bool explained = false;
    for (const config::Diagnostic& note : view.diagnostics) {
      explained = explained || note.message.find("folder map") != std::string::npos;
    }
    checks.Expect(explained, "and the user is told why it is missing, rather than shown none");

    std::string response;
    FakeEngine live;
    live.settings = PathologicalConfig();
    ipc::ServiceCore dispatched(live);
    checks.Expect(ipc::Dispatch(dispatched, static_cast<std::uint32_t>(ipc::Command::kGetConfig),
                                ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
                  "GetConfig never fails, whatever is in config.ini");
    checks.Expect(ipc::Fits(response), "and its answer is inside the cap");
  }

  // Diagnostics are the other half: `config::kMaxDiagnostics` of them, each
  // quoting a path, is several payloads on its own.
  {
    std::vector<config::Diagnostic> many;
    for (std::size_t at = 0; at < config::kMaxDiagnostics; ++at) {
      config::Diagnostic note;
      note.severity = config::Severity::kWarning;
      note.line = static_cast<int>(at) + 1;
      note.section = "platform.p" + std::to_string(at);
      note.key = "roms";
      note.message = "not a usable path: " + std::string(config::kMaxPathLength, 'p');
      many.push_back(note);
    }
    const std::vector<config::Diagnostic> trimmed = ipc::TrimDiagnostics(many);
    checks.Expect(trimmed.size() <= ipc::kMaxDiagnosticsInPayload,
                  "the list is cut to what a payload carries");
    checks.Expect(!trimmed.empty(), "but never to nothing");
    checks.Expect(trimmed.back().message.find("more diagnostics") != std::string::npos,
                  "and the last one says how many did not fit -- no silent cap");
    for (const config::Diagnostic& note : trimmed) {
      checks.Expect(note.message.size() <= ipc::kMaxDiagnosticTextBytes + 3,
                    "every message is cut to the documented length");
    }
    checks.Expect(ipc::Fits(ipc::EncodeConfigResult({ipc::WriteOutcome::kInvalid, trimmed})),
                  "and the whole list fits a SetConfig answer");

    FakeEngine engine;
    engine.notes = many;
    ipc::ServiceCore core(engine);
    checks.Expect(ipc::Fits(ipc::EncodeConfigView(core.GetConfig())),
                  "GetConfig fits with a file that is nothing but complaints");

    // A short list is passed through untouched -- trimming is for the case that
    // needs it, not a tax on the ordinary one.
    const std::vector<config::Diagnostic> few = LoadedConfigView().diagnostics;
    checks.Expect(SameDiagnostics(ipc::TrimDiagnostics(few), few),
                  "an ordinary list is served as it is");
  }

  // **The trim constants bound characters; the wire carries bytes.**
  // `json::Quote` doubles a backslash and turns a control character into six
  // bytes, and `config.cpp` quotes the rejected path straight back into the
  // message -- which may hold either, since holding one is often exactly why it
  // was rejected. A user with Windows-style paths under `[platform.*]` is the
  // ordinary way to reach this.
  for (const std::string& filler :
       {std::string("\\"), std::string("\""), std::string(1, '\x01')}) {
    std::vector<config::Diagnostic> escaping;
    for (std::size_t at = 0; at < config::kMaxDiagnostics; ++at) {
      std::string user_text;
      for (std::size_t byte = 0; byte < config::kMaxPathLength; ++byte) {
        user_text += filler;
      }
      config::Diagnostic note;
      note.severity = config::Severity::kWarning;
      note.line = static_cast<int>(at) + 1;
      // All three text fields, because all three carry what the user wrote: the
      // section is `[platform.<slug>]` as they typed it, the key is the key, and
      // the message quotes the value back (config.cpp).
      note.section = "platform." + user_text;
      note.key = user_text;
      note.message = "'" + user_text + "' is not a usable path, ignored";
      escaping.push_back(note);
    }

    // What the trim constants alone produce -- which is what this used to send.
    // Asserting it is over the cap is what makes the next assertion a
    // regression test rather than a coincidence.
    ipc::ConfigView bare;
    bare.config = config::Defaults();
    bare.config.platforms.clear();
    bare.platforms_truncated = true;
    bare.diagnostics = ipc::TrimDiagnostics(escaping);
    checks.Expect(!ipc::Fits(ipc::EncodeConfigView(bare)),
                  "trimming to the documented character limits is not enough on its own (" +
                      std::to_string(ipc::EncodeConfigView(bare).size()) + " bytes)");

    FakeEngine engine;
    engine.settings = PathologicalConfig();
    engine.notes = escaping;
    ipc::ServiceCore core(engine);
    const std::string payload = ipc::EncodeConfigView(core.GetConfig());
    checks.Expect(ipc::Fits(payload),
                  "GetConfig fits with diagnostics that escape (" +
                      std::to_string(payload.size()) + " bytes)");
    std::string response;
    checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(ipc::Command::kGetConfig),
                                ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
                  "and the command still never fails");
    checks.Expect(ipc::DecodeConfigView(response).ok(), "and its answer comes back whole");

    ipc::ConfigResult result;
    result.outcome = ipc::WriteOutcome::kInvalid;
    result.diagnostics = escaping;
    engine.apply_edit_error = ipc::Error::kInvalid;
    engine.apply_edit_notes = escaping;
    checks.Expect(ipc::Fits(ipc::EncodeConfigResult(core.SetConfig({}))),
                  "and so does SetConfig's refusal");
  }

  // The sentence explaining a missing folder map must survive the trimming that
  // the diagnostics beside it go through -- it is the one the overlay needs most.
  {
    std::vector<config::Diagnostic> crowd;
    for (std::size_t at = 0; at < config::kMaxDiagnostics; ++at) {
      config::Diagnostic note;
      note.severity = config::Severity::kWarning;
      note.line = static_cast<int>(at) + 1;
      note.section = "sync";
      note.key = "states";
      note.message = "expected true or false";
      crowd.push_back(note);
    }
    FakeEngine engine;
    engine.settings = PathologicalConfig();
    engine.notes = crowd;
    ipc::ServiceCore core(engine);
    const ipc::ConfigView view = core.GetConfig();
    checks.Expect(view.platforms_truncated, "the map was dropped");
    bool explained = false;
    for (const config::Diagnostic& note : view.diagnostics) {
      explained = explained || note.message.find("folder map") != std::string::npos;
    }
    checks.Expect(explained,
                  "and the sentence saying so is not itself trimmed away by the crowd");
  }

  // `Status` and the pairing payload are the two that carry a value neither
  // half chose: a `fs_name` off a RomM library, and a verification path off a
  // server response. Both commands are documented never to fail.
  {
    FakeEngine engine;
    engine.snapshot.download.state = ipc::DownloadState::kDownloading;
    engine.snapshot.download.rom_id = 7;
    engine.snapshot.download.fs_name = std::string(ipc::kMaxPayloadBytes * 2, 'n');
    ipc::ServiceCore core(engine);
    std::string response;
    checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(ipc::Command::kGetStatus),
                                ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
                  "GetStatus never fails, whatever the library named a file");
    const ipc::Decoded<ipc::Status> status = ipc::DecodeStatus(response);
    checks.Expect(status.ok(), "and answers a status: " + status.error.Describe());
    checks.Expect(status.value.download.fs_name.size() <= ipc::kMaxNameBytes + 3,
                  "with the name cut to what a payload carries");
    checks.ExpectEq(status.value.download.rom_id, std::int64_t{7},
                    "and everything else intact");
  }

  {
    FakeEngine engine;
    engine.settings.server.url = "http://romm.lan:8080";
    engine.pairing.state = auth::PairingState::kPending;
    engine.pairing.user_code = "ABCD2345";
    engine.pairing.verification_url = "http://romm.lan:8080/" + std::string(20000, 'p');
    engine.pairing.verification_url_complete = engine.pairing.verification_url + "?user_code=X";
    ipc::ServiceCore core(engine);
    std::string response;
    checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(ipc::Command::kGetPairState),
                                ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
                  "GetPairState never fails, whatever the server answered");
    const auth::Parsed<auth::PairingStatus> state = auth::ParsePairingStatus(response);
    checks.Expect(state.ok(), "and answers a status: " + state.error.Describe());
    checks.Expect(state.value.verification_url.empty(),
                  "with a URL too long to show withheld rather than sent");
    checks.ExpectEq(state.value.user_code, std::string("ABCD2345"),
                    "and the code the human types still there");
    checks.Expect(!state.value.message.empty(), "and a sentence saying what happened");
  }

  // A message cut mid-character would travel as malformed UTF-8 for the
  // overlay's font renderer to deal with.
  {
    config::Diagnostic note;
    note.severity = config::Severity::kWarning;
    // Three-byte characters, so the cap lands inside one.
    for (std::size_t at = 0; at < ipc::kMaxDiagnosticTextBytes; ++at) {
      note.message += "\xe2\x82\xac";
    }
    const std::vector<config::Diagnostic> trimmed = ipc::TrimDiagnostics({note});
    checks.ExpectEq(trimmed.size(), std::size_t{1}, "one message in, one out");
    const std::string cut = trimmed[0].message.substr(0, trimmed[0].message.size() - 3);
    checks.ExpectEq(cut.size() % 3, std::size_t{0}, "the cut lands on a character boundary");
    checks.Expect(ipc::DecodeConfigResult(
                      ipc::EncodeConfigResult({ipc::WriteOutcome::kApplied, trimmed}))
                      .ok(),
                  "and the trimmed list still round trips");
  }

  // A page is bounded by bytes, not by items -- a rom's name is the user's data
  // and 64 of the long ones are several payloads. `AppendIfItFits` is what a
  // producer (#31) fills a page with, and what makes "64" safe to publish.
  {
    // The projection #31 pins, with names at the length RomM's own titles reach.
    const auto RomItem = [](std::int32_t at, std::size_t name_length) {
      ipc::ListItem item;
      item.fields.push_back({"id", ipc::ListValue::Integer(1000000 + at)});
      item.fields.push_back({"name", ipc::ListValue::Text(std::string(name_length, 'n'))});
      item.fields.push_back({"fs_name", ipc::ListValue::Text(std::string(name_length, 'f'))});
      item.fields.push_back({"platform_fs_slug", ipc::ListValue::Text("snes")});
      item.fields.push_back({"fs_size_bytes", ipc::ListValue::Integer(123456789)});
      item.fields.push_back({"has_multiple_files", ipc::ListValue::Flag(false)});
      item.fields.push_back({"on_card", ipc::ListValue::Flag(true)});
      return item;
    };

    for (const std::size_t name_length : {std::size_t{8}, std::size_t{96}, std::size_t{512}}) {
      ipc::ListPage page;
      std::int32_t at = 0;
      while (at < ipc::kMaxPageSize && ipc::AppendIfItFits(&page, RomItem(at, name_length))) {
        ++at;
      }
      const std::string encoded = ipc::EncodeListPage(page);
      const std::string what = "with " + std::to_string(name_length) + "-character names";
      checks.Expect(!page.items.empty(), "a page holds at least one item " + what);
      checks.Expect(ipc::Fits(encoded), "and fits, " + what + " (" +
                                            std::to_string(page.items.size()) + " items, " +
                                            std::to_string(encoded.size()) + " bytes)");
      checks.Expect(ipc::DecodeListPage(encoded).ok(), "and comes back whole " + what);
      // The item that did not fit really does not fit: appending it again must
      // fail rather than intermittently succeed.
      if (page.items.size() < static_cast<std::size_t>(ipc::kMaxPageSize)) {
        const std::size_t before = page.items.size();
        checks.Expect(!ipc::AppendIfItFits(&page, RomItem(at, name_length)),
                      "a full page stays full " + what);
        checks.ExpectEq(page.items.size(), before, "and is left untouched " + what);
      }
    }

    // An item no page could ever hold is a projection to shorten, not a page to
    // split -- so it is refused rather than served alone and over the cap.
    ipc::ListPage page;
    ipc::ListItem enormous;
    enormous.fields.push_back({"name", ipc::ListValue::Text(std::string(ipc::kMaxPayloadBytes, 'n'))});
    checks.Expect(!ipc::AppendIfItFits(&page, enormous), "an item larger than a payload is refused");
    checks.Expect(page.items.empty(), "and leaves the page empty rather than over the cap");
  }
  return checks.failures();
}

// --- ipc.dispatch -------------------------------------------------------------

/// The table, and the decisions `ServiceCore` makes on the way through it.
///
/// This is the half the sysmodule does not get to have opinions about: the
/// binding hands a command id and two buffers to `Dispatch`, so every branch
/// below runs on the host under `ctest` rather than on a console.
int DispatchTable() {
  checks::Checks checks;

  {
    FakeEngine engine;
    ipc::ServiceCore core(engine);
    std::string response = "left over from the last call";
    checks.Expect(ipc::Dispatch(core, 14, ipc::EncodeEmpty(), &response) ==
                      ipc::Error::kUnknownCommand,
                  "an id past the table is a named refusal, not a crash");
    checks.Expect(response.empty(), "and the response buffer is cleared, not left stale");
    checks.Expect(ipc::Dispatch(core, 0xFFFFFFFF, ipc::EncodeEmpty(), &response) ==
                      ipc::Error::kUnknownCommand,
                  "and so is a wild one");
  }

  // Every command refuses a request payload it cannot read. Fed each other's
  // payloads, which is what an overlay from a different release actually sends.
  {
    FakeEngine engine;
    ipc::ServiceCore core(engine);
    for (const ipc::Command command : ipc::kAllCommands) {
      std::string response;
      const std::string name = ipc::ToString(command);
      checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(command), "not json at all",
                                  &response) == ipc::Error::kMalformedRequest,
                    name + " refuses a request that is not JSON");
      checks.Expect(response.empty(), name + " leaves nothing behind when it refuses");
      checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(command), "[]", &response) ==
                        ipc::Error::kMalformedRequest,
                    name + " refuses a request that is not an object");
    }
    // The commands that take an argument refuse an empty object, which is what
    // the no-argument ones send.
    for (const ipc::Command command :
         {ipc::Command::kSetConfig, ipc::Command::kSetEnabled, ipc::Command::kEnqueue,
          ipc::Command::kDequeue, ipc::Command::kListBegin, ipc::Command::kListNext,
          ipc::Command::kListEnd}) {
      std::string response;
      checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(command), ipc::EncodeEmpty(),
                                  &response) == ipc::Error::kMalformedRequest,
                    std::string(ipc::ToString(command)) + " needs its argument");
    }
  }

  // SyncNow's five outcomes, in remedy order: each is the *first* thing the user
  // has to fix, which is why the ladder is a ladder and not a set of flags.
  {
    FakeEngine engine;
    engine.settings.server.url.clear();
    engine.snapshot.auth = ipc::AuthState::kPaired;
    ipc::ServiceCore core(engine);
    checks.Expect(core.SyncNow() == ipc::SyncOutcome::kNotConfigured,
                  "no server is the first thing to fix");

    engine.settings.server.url = "http://romm.lan:8080";
    engine.snapshot.auth = ipc::AuthState::kNeverPaired;
    checks.Expect(core.SyncNow() == ipc::SyncOutcome::kUnauthenticated,
                  "then the pairing -- a console with no server is not 'unauthenticated'");
    engine.snapshot.auth = ipc::AuthState::kUnauthenticated;
    checks.Expect(core.SyncNow() == ipc::SyncOutcome::kUnauthenticated,
                  "and a revoked token is the same instruction");

    engine.snapshot.auth = ipc::AuthState::kPaired;
    engine.settings.sync.enabled = false;
    checks.Expect(core.SyncNow() == ipc::SyncOutcome::kDisabled,
                  "then the switch -- a sync asked for while off is its own answer (#24)");

    engine.settings.sync.enabled = true;
    checks.Expect(core.SyncNow() == ipc::SyncOutcome::kAccepted, "and then it is accepted");
    engine.sync_accepted = false;
    checks.Expect(core.SyncNow() == ipc::SyncOutcome::kAlreadyRunning,
                  "unless one is already running");
    checks.ExpectEq(engine.sync_requests, 2, "and the engine is only asked when it could act");
  }

  // SetEnabled answers with the state that *took*, inside a successful reply.
  //
  // The reply is what carries the outcome because a failing `Result` takes the
  // payload with it -- libnx's `cmifParseResponse` never exposes the reply's
  // data words once the result says the call failed. A refusal reported only as
  // a `Result` would reach the overlay with nothing attached, which is the one
  // thing #24 cannot render.
  {
    FakeEngine engine;
    engine.settings.sync.enabled = false;
    ipc::ServiceCore core(engine);

    ipc::EnabledResult moved = core.SetEnabled(true);
    checks.Expect(moved.outcome == ipc::WriteOutcome::kApplied, "the switch moves");
    checks.ExpectEq(moved.enabled, true, "and the answer is the new state");

    engine.set_enabled_error = ipc::Error::kWriteFailed;
    engine.set_enabled_applies = false;
    const ipc::EnabledResult stuck = core.SetEnabled(false);
    checks.Expect(stuck.outcome == ipc::WriteOutcome::kWriteFailed, "a failed write is named");
    checks.ExpectEq(stuck.enabled, true,
                    "and the answer is the state that did not move, not the one asked for");

    std::string response;
    checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(ipc::Command::kSetEnabled),
                                ipc::EncodeEnabled(false), &response) == ipc::Error::kOk,
                  "over the table it is a successful call carrying a failed write");
    const ipc::Decoded<ipc::EnabledResult> back = ipc::DecodeEnabledResult(response);
    checks.Expect(back.ok(), "and decodes: " + back.error.Describe());
    checks.Expect(back.ok() && back.value.outcome == ipc::WriteOutcome::kWriteFailed,
                  "with what happened");
    checks.Expect(back.ok() && back.value.enabled,
                  "and the effective state, so the switch can be redrawn (#24)");

    // An engine that could not name its failure is still a write that did not
    // happen, which is the true and actionable half of it.
    engine.set_enabled_error = ipc::Error::kInternal;
    checks.Expect(core.SetEnabled(true).outcome == ipc::WriteOutcome::kWriteFailed,
                  "an unnamed engine failure is reported as a write that did not happen");
  }

  // SetConfig's refusal is its diagnostics. A refusal with an empty payload
  // would leave a settings screen with nothing to say.
  {
    FakeEngine engine;
    config::Diagnostic why;
    why.severity = config::Severity::kError;
    why.section = "server";
    why.key = "url";
    why.message = "not a usable server address";
    engine.apply_edit_notes.push_back(why);
    engine.apply_edit_error = ipc::Error::kInvalid;
    ipc::ServiceCore core(engine);

    ipc::ConfigEdit edit;
    edit.assignments.push_back({"server", "url", "nonsense", false});
    std::string response;
    checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(ipc::Command::kSetConfig),
                                ipc::EncodeConfigEdit(edit), &response) == ipc::Error::kOk,
                  "a refused edit is a successful call carrying the refusal");
    const ipc::Decoded<ipc::ConfigResult> back = ipc::DecodeConfigResult(response);
    checks.Expect(back.ok(), "and decodes: " + back.error.Describe());
    checks.Expect(back.ok() && back.value.outcome == ipc::WriteOutcome::kInvalid,
                  "which says nothing was written");
    checks.Expect(back.ok() && back.value.diagnostics.size() == 1, "and says why");
    checks.ExpectEq(engine.last_edit.assignments.size(), std::size_t{1},
                    "the edit reached the engine as it was sent");
    if (!engine.last_edit.assignments.empty()) {
      checks.ExpectEq(engine.last_edit.assignments[0].value, std::string("nonsense"),
                      "value and all");
    }
  }

  // Status reads the switch off the config, not off the snapshot: what the
  // overlay draws is what is on the card.
  {
    FakeEngine engine;
    engine.settings.sync.enabled = false;
    engine.settings.server.url = "http://romm.lan:8080";
    engine.snapshot.auth = ipc::AuthState::kPaired;
    engine.snapshot.queue_depth = 3;
    ipc::ServiceCore core(engine);
    const ipc::Status status = core.GetStatus();
    checks.ExpectEq(status.enabled, false, "the switch is the one in config.ini");
    checks.ExpectEq(status.configured, true, "and configured follows from a usable server");
    checks.ExpectEq(status.queue_depth, std::int64_t{3}, "the queue depth comes off the engine");
    checks.ExpectEq(static_cast<std::uint64_t>(status.interface),
                    static_cast<std::uint64_t>(ipc::kVersion), "the interface version is carried");
    checks.ExpectEq(status.build, std::string(rommsync::version()),
                    "and so is the build, so a support thread can start");
  }

  // StartPair needs a server before it needs anything else.
  {
    FakeEngine engine;
    engine.settings.server.url.clear();
    ipc::ServiceCore core(engine);
    auth::PairingStatus status;
    checks.Expect(core.StartPair(&status) == ipc::Error::kNotConfigured,
                  "pairing with nothing to pair against is refused");
    std::string response;
    checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(ipc::Command::kStartPair),
                                ipc::EncodeEmpty(), &response) == ipc::Error::kNotConfigured,
                  "over the table too");
    checks.Expect(response.empty(), "and there is no status to read off it");
  }

  // ListBegin clamps, and drops a filter that means nothing for the kind.
  {
    FakeEngine engine;
    ipc::ServiceCore core(engine);
    ipc::Cursor cursor = 0;
    checks.Expect(core.ListBegin({ipc::ListKind::kRoms, 3, "zelda", 100000}, &cursor) ==
                      ipc::Error::kOk,
                  "an enormous page size is served rather than refused");
    checks.ExpectEq(engine.last_list.page_size, ipc::kMaxPageSize, "clamped to the cap");
    checks.ExpectEq(engine.last_list.platform_id, std::int64_t{3}, "with the filter intact");
    checks.ExpectEq(engine.last_list.search, std::string("zelda"), "and the search term");

    checks.Expect(core.ListBegin({ipc::ListKind::kQueue, 3, "zelda", 8}, &cursor) ==
                      ipc::Error::kOk,
                  "the queue is a list too");
    checks.ExpectEq(engine.last_list.platform_id, std::int64_t{0},
                    "and a platform filter on it is dropped rather than half-honoured");
    checks.ExpectEq(engine.last_list.search, std::string(""), "along with the search term");
    checks.ExpectEq(engine.last_list.page_size, 8, "what the client asked for is kept");
  }

  // A cursor of zero never addresses a live list.
  {
    FakeEngine engine;
    ipc::ServiceCore core(engine);
    ipc::ListPage page;
    checks.Expect(core.ListNext(0, &page) == ipc::Error::kBadCursor, "ListNext(0) is a bad cursor");
    checks.Expect(core.ListEnd(0) == ipc::Error::kBadCursor, "and so is ListEnd(0)");
    checks.ExpectEq(engine.last_cursor, ipc::Cursor{0}, "the engine is never asked");
  }

  // The engine's errors reach the caller unchanged: `ServiceCore` classifies
  // what it can decide and invents nothing on top of what it cannot.
  {
    FakeEngine engine;
    ipc::ServiceCore core(engine);
    std::int32_t position = 0;
    for (const ipc::Error error : {ipc::Error::kUnknownRom, ipc::Error::kQueueFull,
                                   ipc::Error::kDuplicate, ipc::Error::kMultiFile}) {
      engine.enqueue_error = error;
      checks.Expect(core.Enqueue(42, &position) == error,
                    std::string("Enqueue reports ") + ipc::ToString(error));
      std::string response;
      checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(ipc::Command::kEnqueue),
                                  ipc::EncodeRomId(42), &response) == error,
                    std::string("and so does the table: ") + ipc::ToString(error));
      checks.Expect(response.empty(), "with no position to read");
    }
    engine.enqueue_error = ipc::Error::kOk;
    engine.enqueue_position = 4;
    std::string response;
    checks.Expect(ipc::Dispatch(core, static_cast<std::uint32_t>(ipc::Command::kEnqueue),
                                ipc::EncodeRomId(42), &response) == ipc::Error::kOk,
                  "a queued rom answers");
    const ipc::Decoded<std::int32_t> back = ipc::DecodeQueuePosition(response);
    checks.Expect(back.ok() && back.value == 4, "with where in the queue it landed");
    checks.ExpectEq(engine.last_rom_id, std::int64_t{42}, "and the id it was given");

    engine.dequeue_error = ipc::Error::kNotQueued;
    checks.Expect(core.Dequeue(42) == ipc::Error::kNotQueued, "Dequeue reports not_queued");
    engine.list_next_error = ipc::Error::kOffline;
    ipc::ListPage page;
    checks.Expect(core.ListNext(7, &page) == ipc::Error::kOffline, "ListNext reports offline");
    engine.list_next_error = ipc::Error::kBadCursor;
    checks.Expect(core.ListNext(7, &page) == ipc::Error::kBadCursor,
                  "and a reclaimed cursor as itself");
  }

  // Every `Error` has a distinct name; a log line that cannot tell two apart is
  // a log line that cannot diagnose either.
  {
    const ipc::Error all[] = {
        ipc::Error::kOk,          ipc::Error::kInvalid,     ipc::Error::kWriteFailed,
        ipc::Error::kNotConfigured, ipc::Error::kUnknownRom, ipc::Error::kQueueFull,
        ipc::Error::kDuplicate,   ipc::Error::kMultiFile,   ipc::Error::kNotQueued,
        ipc::Error::kBadCursor,   ipc::Error::kOffline,     ipc::Error::kUnknownCommand,
        ipc::Error::kMalformedRequest, ipc::Error::kTooLarge, ipc::Error::kInternal,
    };
    for (std::size_t a = 0; a < std::size(all); ++a) {
      for (std::size_t b = a + 1; b < std::size(all); ++b) {
        checks.Expect(std::string(ipc::ToString(all[a])) != ipc::ToString(all[b]),
                      std::string("distinct error names: ") + ipc::ToString(all[a]));
      }
    }
  }
  return checks.failures();
}

// --- ipc.engine ---------------------------------------------------------------

std::string Base64(std::string_view raw) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t at = 0; at < raw.size(); at += 3) {
    const std::size_t left = raw.size() - at;
    const unsigned a = static_cast<unsigned char>(raw[at]);
    const unsigned b = left > 1 ? static_cast<unsigned char>(raw[at + 1]) : 0u;
    const unsigned c = left > 2 ? static_cast<unsigned char>(raw[at + 2]) : 0u;
    const unsigned triple = (a << 16) | (b << 8) | c;
    out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    out.push_back(left > 1 ? kAlphabet[(triple >> 6) & 0x3F] : '=');
    out.push_back(left > 2 ? kAlphabet[triple & 0x3F] : '=');
  }
  return out;
}

/// Be the human at the browser -- RomM's own approve endpoint takes HTTP Basic,
/// which is what makes the device grant testable unattended (docs/TESTING.md).
http::Result Approve(http::HttpClient& client, const std::string& base,
                     const std::string& user_code) {
  http::Request request;
  request.method = http::Method::kPost;
  request.url = base + "/api/auth/device/approve";
  request.headers.push_back({"Content-Type", "application/json"});
  request.headers.push_back(
      {"Authorization", "Basic " + Base64(std::string(rig::kUser) + ":" + rig::kPassword)});
  request.body = "{\"user_code\":" + json::Quote(user_code) +
                 ",\"approved_scopes\":" + json::QuoteArray(auth::MinimumScopes()) + "}";
  return client.Send(request);
}

/// An `ipc::Engine` made of the real thing: a real `PairingSession`, a real
/// `sync::Negotiate`, and a queue.
///
/// It is what the sysmodule's engine will be a bigger version of, and it is
/// written to the one rule the contract has -- **no command blocks on the
/// network**. `StartPairing` and `RequestSync` hand their work to a thread and
/// return, exactly as the auth thread and the scheduler will, so `GetStatus` and
/// `GetPairState` are answered while a request is in flight rather than after it.
class HarnessEngine : public ipc::Engine {
 public:
  explicit HarnessEngine(http::HttpClient& client) : client_(client) {}

  ~HarnessEngine() override {
    if (pairing_.joinable()) {
      stop_ = true;
      pairing_.join();
    }
    if (syncing_.joinable()) {
      syncing_.join();
    }
  }

  void Configure(const std::string& url) { settings_.server.url = url; }
  void KnowRoms(std::vector<std::int64_t> ids) { known_ = std::move(ids); }

  const config::Config& config() const override { return settings_; }
  const std::vector<config::Diagnostic>& config_diagnostics() const override { return notes_; }

  ipc::EngineSnapshot Snapshot() const override {
    std::lock_guard<std::mutex> held(mutex_);
    ipc::EngineSnapshot copy = snapshot_;
    copy.queue_depth = static_cast<std::int64_t>(queue_.size());
    return copy;
  }

  auth::PairingStatus pairing_status() const override {
    std::lock_guard<std::mutex> held(mutex_);
    // `PairingSession::status()` is safe from any thread and never blocks, which
    // is the whole reason this command can be answered at all.
    if (session_ == nullptr) {
      return {};
    }
    auth::PairingStatus status = session_->status();
    // Bridge the window `Engine::StartPairing` documents: the attempt has been
    // handed to the thread and `Begin()` has not run yet, so the session still
    // says `kIdle` -- which would tell the user that pressing Pair did nothing.
    if (status.state == auth::PairingState::kIdle && starting_) {
      status.state = auth::PairingState::kStarting;
    }
    return status;
  }

  ipc::Error SetSyncEnabled(bool enabled) override {
    settings_.sync.enabled = enabled;
    return ipc::Error::kOk;
  }

  ipc::Error ApplyConfigEdit(const ipc::ConfigEdit&, std::vector<config::Diagnostic>*) override {
    // M5-3 (#30) owns what an edit means. This engine exists to drive the
    // commands that already have an implementation behind them.
    return ipc::Error::kInternal;
  }

  bool RequestSync() override {
    if (sync_running_.exchange(true)) {
      return false;
    }
    if (syncing_.joinable()) {
      syncing_.join();
    }
    syncing_ = std::thread([this] { RunSync(); });
    return true;
  }

  ipc::Error StartPairing() override {
    if (pairing_.joinable()) {
      stop_ = true;
      pairing_.join();
    }
    stop_ = false;
    auth::PairingConfig pairing;
    pairing.server_url = settings_.server.url;
    // A fixed identifier, so two runs are one device in RomM rather than a new
    // one each time (device.repair's guarantee).
    pairing.client_device_identifier = "rommsync-nx-ipc-test";
    pairing.device_name = "rommsync-nx ipc test";
    {
      std::lock_guard<std::mutex> held(mutex_);
      session_ = std::make_unique<auth::PairingSession>(client_, pairing);
      starting_ = true;
    }
    pairing_ = std::thread([this] { RunPairing(); });
    return ipc::Error::kOk;
  }

  ipc::Error Unpair() override {
    std::lock_guard<std::mutex> held(mutex_);
    token_ = {};
    snapshot_.auth = ipc::AuthState::kNeverPaired;
    return ipc::Error::kOk;
  }

  ipc::Error Enqueue(std::int64_t rom_id, std::int32_t* position) override {
    std::lock_guard<std::mutex> held(mutex_);
    bool known = false;
    for (const std::int64_t id : known_) {
      known = known || id == rom_id;
    }
    if (!known) {
      return ipc::Error::kUnknownRom;
    }
    for (const std::int64_t id : queue_) {
      if (id == rom_id) {
        return ipc::Error::kDuplicate;
      }
    }
    queue_.push_back(rom_id);
    *position = static_cast<std::int32_t>(queue_.size());
    return ipc::Error::kOk;
  }

  ipc::Error Dequeue(std::int64_t rom_id) override {
    std::lock_guard<std::mutex> held(mutex_);
    for (std::size_t at = 0; at < queue_.size(); ++at) {
      if (queue_[at] == rom_id) {
        queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(at));
        return ipc::Error::kOk;
      }
    }
    return ipc::Error::kNotQueued;
  }

  ipc::Error ListBegin(const ipc::ListRequest&, ipc::Cursor*) override {
    return ipc::Error::kInternal;  // M5-4 (#31)
  }
  ipc::Error ListNext(ipc::Cursor, ipc::ListPage*) override { return ipc::Error::kInternal; }
  ipc::Error ListEnd(ipc::Cursor) override { return ipc::Error::kInternal; }

  auth::StoredToken token() const {
    std::lock_guard<std::mutex> held(mutex_);
    return token_;
  }

  /// The session the last sync opened, or `0`. See `RunSync`.
  std::int64_t last_session_id() const {
    std::lock_guard<std::mutex> held(mutex_);
    return last_session_id_;
  }

 private:
  void RunPairing() {
    auth::PairingState state = session_->Begin();
    {
      std::lock_guard<std::mutex> held(mutex_);
      starting_ = false;
    }
    while (!auth::IsTerminal(state) && !stop_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      state = session_->Poll();
    }
    if (state != auth::PairingState::kApproved || session_->token() == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> held(mutex_);
    token_ = auth::StoredTokenFrom(settings_.server.url, *session_->token());
    snapshot_.auth = ipc::AuthState::kPaired;
  }

  void RunSync() {
    // An empty `saves` list is how a client asks what it is missing
    // (sync.discovers), which is the negotiation a console with no saves on the
    // card makes on its first tick.
    const sync::Negotiation negotiated = sync::Negotiate(client_, token(), {});
    std::lock_guard<std::mutex> held(mutex_);
    // RomM keeps ONE active sync session per device, and a negotiate that walks
    // away from its session leaves the next one racing to cancel it (#76). A
    // real engine completes the session as step 3 of the loop (M2-6); this one
    // records the id so the scenario can, which is the same discipline every
    // other rig test in this repo now keeps.
    last_session_id_ = negotiated.ok() ? negotiated.plan.session_id : 0;
    snapshot_.online = negotiated.error != sync::NegotiateError::kUnreachable;
    if (negotiated.ok()) {
      snapshot_.last_sync_result = ipc::SyncResult::kOk;
      snapshot_.uploaded = negotiated.plan.total_upload;
      snapshot_.downloaded = negotiated.plan.total_download;
      snapshot_.conflicts = negotiated.plan.total_conflict;
      snapshot_.failed = 0;
    } else {
      snapshot_.last_sync_result = ipc::SyncResult::kFailed;
      snapshot_.failed = 1;
      if (sync::NeedsPairing(negotiated.error)) {
        snapshot_.auth = ipc::AuthState::kUnauthenticated;
      }
    }
    // Whole Unix seconds, the way `Status` carries them.
    snapshot_.last_sync_at =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    sync_running_ = false;
  }

  http::HttpClient& client_;

  config::Config settings_ = config::Defaults();
  std::vector<config::Diagnostic> notes_;

  mutable std::mutex mutex_;
  ipc::EngineSnapshot snapshot_;
  std::unique_ptr<auth::PairingSession> session_;
  auth::StoredToken token_;
  std::vector<std::int64_t> known_;
  std::vector<std::int64_t> queue_;
  std::int64_t last_session_id_ = 0;
  bool starting_ = false;

  std::atomic<bool> sync_running_{false};
  std::atomic<bool> stop_{false};
  std::thread pairing_;
  std::thread syncing_;
};

/// Every rom id the fixture library holds, as the paired device sees them.
std::vector<std::int64_t> LibraryIds(http::HttpClient& client, const std::string& base,
                                     const auth::StoredToken& token, checks::Checks& checks) {
  http::Request request;
  request.method = http::Method::kGet;
  request.url = base + "/api/roms?limit=5&order_by=id";
  request.headers.push_back({"Authorization", "Bearer " + token.access_token});
  const http::Result result = client.Send(request);
  std::vector<std::int64_t> ids;
  if (!result.successful()) {
    checks.Expect(false, "the library lists for the paired device");
    return ids;
  }
  const json::ParseResult document = json::Parse(result.response.body);
  const json::Value* items =
      document.ok() ? document.value.Find("items") : static_cast<const json::Value*>(nullptr);
  if (items == nullptr) {
    checks.Expect(false, "GET /api/roms answers the documented envelope");
    return ids;
  }
  for (const json::Value& item : items->elements()) {
    const json::Value* id = item.Find("id");
    if (id != nullptr && id->is_integer()) {
      ids.push_back(id->integer());
    }
  }
  return ids;
}

/// `ServiceCore` driven end to end against the docker RomM, over the dispatch
/// table rather than by calling its methods -- the commands the overlay presses,
/// through the encoding they travel in.
int EndToEnd(http::HttpClient& client, const std::string& base) {
  rig::Checks checks;
  HarnessEngine engine(client);
  ipc::ServiceCore core(engine);

  const auto Call = [&](ipc::Command command, const std::string& request, std::string* response) {
    return ipc::Dispatch(core, static_cast<std::uint32_t>(command), request, response);
  };
  const auto StatusNow = [&]() {
    std::string response;
    const ipc::Error error = Call(ipc::Command::kGetStatus, ipc::EncodeEmpty(), &response);
    checks.Expect(error == ipc::Error::kOk, std::string("GetStatus answers: ") + ipc::ToString(error));
    const ipc::Decoded<ipc::Status> status = ipc::DecodeStatus(response);
    checks.Expect(status.ok(), "and decodes: " + status.error.Describe());
    return status.value;
  };

  // --- a console nobody has configured -----------------------------------------
  {
    const ipc::Status status = StatusNow();
    checks.ExpectEq(status.configured, false, "an unconfigured console says so");
    checks.Expect(status.auth == ipc::AuthState::kNeverPaired, "and that it has never paired");
    checks.Expect(status.last_sync_result == ipc::SyncResult::kNever, "and never synced");

    std::string response;
    checks.Expect(Call(ipc::Command::kSyncNow, ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
                  "SyncNow still answers");
    const ipc::Decoded<ipc::SyncOutcome> outcome = ipc::DecodeSyncOutcome(response);
    checks.Expect(outcome.ok() && outcome.value == ipc::SyncOutcome::kNotConfigured,
                  "with the first thing the user has to fix");
    checks.Expect(Call(ipc::Command::kStartPair, ipc::EncodeEmpty(), &response) ==
                      ipc::Error::kNotConfigured,
                  "and pairing against nothing is refused");
  }

  // --- configured, and paired for real -----------------------------------------
  engine.Configure(base);
  {
    std::string response;
    checks.Expect(Call(ipc::Command::kGetConfig, ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
                  "GetConfig answers");
    const ipc::Decoded<ipc::ConfigView> view = ipc::DecodeConfigView(response);
    checks.Expect(view.ok(), "and decodes: " + view.error.Describe());
    checks.ExpectEq(view.value.config.server.url, base, "with the server the user configured");
  }

  {
    std::string response;
    checks.Expect(Call(ipc::Command::kStartPair, ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
                  "StartPair is accepted");
    const auth::Parsed<auth::PairingStatus> started = auth::ParsePairingStatus(response);
    checks.Expect(started.ok(), "and answers a status: " + started.error.Describe());
    // The command handed the init to a thread and returned. Whether it has come
    // back yet is a race this deliberately does not care about -- what it must
    // not be is `kIdle`, which would tell the user pressing Pair did nothing.
    checks.Expect(started.value.state == auth::PairingState::kStarting ||
                      started.value.state == auth::PairingState::kPending,
                  std::string("with an attempt in progress, not idle: ") +
                      auth::ToString(started.value.state));
  }

  // A budget per phase, not one shared across three sequential waits: a slow
  // pairing must not spend the sync's budget and have the last loop report "the
  // sync landed: never" -- a timeout dressed up as a wrong answer.
  const auto Deadline = [] { return std::chrono::steady_clock::now() + 90s; };

  std::string user_code;
  const auto code_deadline = Deadline();
  while (std::chrono::steady_clock::now() < code_deadline) {
    std::string response;
    checks.Expect(Call(ipc::Command::kGetPairState, ipc::EncodeEmpty(), &response) ==
                      ipc::Error::kOk,
                  "GetPairState answers while the init is in flight");
    const auth::Parsed<auth::PairingStatus> state = auth::ParsePairingStatus(response);
    checks.Expect(state.ok(), "and decodes: " + state.error.Describe());
    if (!state.value.user_code.empty()) {
      user_code = state.value.user_code;
      checks.Expect(response.find(base) != std::string::npos,
                    "the URL the human types is absolute, so the overlay can show it");
      break;
    }
    std::this_thread::sleep_for(200ms);
  }
  checks.Expect(!user_code.empty(), "a real code arrives over IPC");
  if (user_code.empty()) {
    return checks.failures();
  }

  checks.ExpectOk(Approve(client, base, user_code), "the human approves it in the web UI");

  bool paired = false;
  const auto paired_deadline = Deadline();
  while (!paired && std::chrono::steady_clock::now() < paired_deadline) {
    std::this_thread::sleep_for(300ms);
    paired = StatusNow().auth == ipc::AuthState::kPaired;
  }
  checks.Expect(paired, "and the console reports itself paired");
  if (!paired) {
    return checks.failures();
  }

  // --- a sync, before and after -------------------------------------------------
  // Whatever an earlier run of this scenario left open on *this* device, before
  // negotiating on it: RomM keeps one active session per device and a leftover
  // makes the next negotiate race its own session to a cancel (#76). The
  // fixture's device is another binary's to tidy; this one is ours.
  const harness::Fixture device{engine.token().access_token, engine.token().device_id};
  harness::CloseOpenSessions(client, base, device);

  const std::vector<std::int64_t> ids = LibraryIds(client, base, engine.token(), checks);
  checks.Expect(!ids.empty(), "the seeded library has roms to work with");

  const ipc::Status before = StatusNow();
  checks.Expect(before.last_sync_result == ipc::SyncResult::kNever, "nothing has synced yet");
  checks.ExpectEq(before.last_sync_at, std::int64_t{0}, "so there is no time to show");

  {
    std::string response;
    checks.Expect(Call(ipc::Command::kSyncNow, ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
                  "SyncNow is accepted");
    const ipc::Decoded<ipc::SyncOutcome> outcome = ipc::DecodeSyncOutcome(response);
    checks.Expect(outcome.ok() && outcome.value == ipc::SyncOutcome::kAccepted,
                  "as accepted, not as a result");

    // The rule the whole contract hangs on: the command handed work to the
    // engine and returned, so a status poll answers *now* rather than after a
    // negotiation. An overlay redrawing at 60 Hz cannot wait on a socket.
    const auto started = std::chrono::steady_clock::now();
    for (int at = 0; at < 20; ++at) {
      (void)StatusNow();
    }
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    checks.Expect(spent < 1s, "twenty status polls during a sync cost " +
                                  std::to_string(spent.count()) + "ms, not a round trip each");
  }

  ipc::Status after = StatusNow();
  const auto sync_deadline = Deadline();
  while (after.last_sync_result == ipc::SyncResult::kNever &&
         std::chrono::steady_clock::now() < sync_deadline) {
    std::this_thread::sleep_for(200ms);
    after = StatusNow();
  }
  checks.Expect(after.last_sync_result == ipc::SyncResult::kOk,
                std::string("the sync landed: ") + ipc::ToString(after.last_sync_result));
  checks.Expect(after.online, "and the console reports itself online");
  checks.Expect(after.last_sync_at > 0, "with a time to show for it");
  checks.ExpectEq(after.enabled, engine.config().sync.enabled,
                  "and the switch on the card, not the one the engine happens to hold");

  // Close the session this scenario opened. A real engine does it as step 3 of
  // the loop (M2-6); until that exists, a scenario that negotiated and walked
  // away is the thing that breaks the next binary's `harness.partial` (#76).
  if (engine.last_session_id() != 0) {
    harness::Complete(client, base, device, engine.last_session_id(), 0, 0);
  }

  // The switch, over the table, on the engine that actually persists it.
  {
    std::string response;
    checks.Expect(Call(ipc::Command::kSetEnabled, ipc::EncodeEnabled(false), &response) ==
                      ipc::Error::kOk,
                  "SetEnabled answers");
    const ipc::Decoded<ipc::EnabledResult> off = ipc::DecodeEnabledResult(response);
    checks.Expect(off.ok() && off.value.outcome == ipc::WriteOutcome::kApplied,
                  "the write took: " + off.error.Describe());
    checks.Expect(off.ok() && !off.value.enabled, "and the answer is the state that took");
    checks.ExpectEq(StatusNow().enabled, false, "which GetStatus agrees with");

    // ...and a sync asked for while it is off is its own outcome, not a
    // spinner that never moves (#24).
    checks.Expect(Call(ipc::Command::kSyncNow, ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
                  "SyncNow still answers with the switch off");
    const ipc::Decoded<ipc::SyncOutcome> refused = ipc::DecodeSyncOutcome(response);
    checks.Expect(refused.ok() && refused.value == ipc::SyncOutcome::kDisabled,
                  "as disabled");

    checks.Expect(Call(ipc::Command::kSetEnabled, ipc::EncodeEnabled(true), &response) ==
                      ipc::Error::kOk,
                  "and it goes back on");
  }

  // --- the queue ----------------------------------------------------------------
  engine.KnowRoms(ids);
  {
    std::string response;
    checks.Expect(Call(ipc::Command::kEnqueue, ipc::EncodeRomId(ids.front()), &response) ==
                      ipc::Error::kOk,
                  "a real rom queues");
    const ipc::Decoded<std::int32_t> position = ipc::DecodeQueuePosition(response);
    checks.Expect(position.ok() && position.value == 1, "at the front of an empty queue");
    checks.ExpectEq(StatusNow().queue_depth, std::int64_t{1}, "and the status screen shows it");

    checks.Expect(Call(ipc::Command::kEnqueue, ipc::EncodeRomId(ids.front()), &response) ==
                      ipc::Error::kDuplicate,
                  "queuing it twice is a named refusal, not a second entry");
    checks.ExpectEq(StatusNow().queue_depth, std::int64_t{1}, "and does not deepen the queue");

    // An id no library has. RomM's ids are small and sequential, so this is well
    // past anything the fixture could hold.
    checks.Expect(Call(ipc::Command::kEnqueue, ipc::EncodeRomId(999999999), &response) ==
                      ipc::Error::kUnknownRom,
                  "an id the library does not have is refused");

    checks.Expect(Call(ipc::Command::kDequeue, ipc::EncodeRomId(ids.front()), &response) ==
                      ipc::Error::kOk,
                  "and it comes back out");
    checks.ExpectEq(StatusNow().queue_depth, std::int64_t{0}, "leaving the queue empty");
    checks.Expect(Call(ipc::Command::kDequeue, ipc::EncodeRomId(ids.front()), &response) ==
                      ipc::Error::kNotQueued,
                  "dequeuing it again says so rather than succeeding twice");
  }
  return checks.failures();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: test_ipc <scenario>\n";
    return 2;
  }
  const std::string scenario = argv[1];

  if (scenario == "version") {
    return Version() == 0 ? 0 : 1;
  }
  if (scenario == "commands") {
    return Commands() == 0 ? 0 : 1;
  }
  if (scenario == "roundtrip") {
    return RoundTrip() == 0 ? 0 : 1;
  }
  if (scenario == "refuses") {
    return Refuses() == 0 ? 0 : 1;
  }
  if (scenario == "secrets") {
    return Secrets() == 0 ? 0 : 1;
  }
  if (scenario == "caps") {
    return Caps() == 0 ? 0 : 1;
  }
  if (scenario == "dispatch") {
    return DispatchTable() == 0 ? 0 : 1;
  }
  if (scenario != "engine") {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  const std::string base = rig::BaseUrl();
  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);
  if (error) {
    std::cerr << "could not create " << rig::ScratchDir() << ": " << error.message() << "\n";
    return 2;
  }
  const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  rig::EnsureUser(*client, base);
  rig::DisarmFault(*client, base);

  const int failures = EndToEnd(*client, base);
  rig::DisarmFault(*client, base);
  if (failures == 0) {
    std::cout << "ipc.engine ok against " << base << "\n";
  }
  return failures == 0 ? 0 : 1;
}
