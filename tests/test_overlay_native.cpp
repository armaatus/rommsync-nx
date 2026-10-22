// The overlay's own code, compiled by a host compiler and run (M9-7, #198).
//
// Until this suite existed not one file under `overlay/source/` had been built
// by anything but devkitPro, and `overlay/source/status_screen.hpp` said so:
// *"Nothing here has ever run."* The five `overlay.*` suites next door exercise
// the view models in `core/` -- what a screen *says* -- and the greps in them
// assert what the directory must not contain. Neither reaches the code.
//
// What is compiled in here, and why it is these files:
//
//   `card_probe.cpp`   the "not installed" / "installed but not set to boot"
//                      diagnosis. Portable already; it needed only a seam for
//                      the `sdmc:` mount (`ProbeCardAt`).
//   `ipc_client.cpp`   the entire client half of the IPC wire, behind the
//                      `switch.h` shim in `tests/hostswitch/`, and the budget
//                      every call is held to (M9-6, #209).
//   `screen_frame.cpp` the version handshake and the "not running" /
//                      "unreachable" / "not responding" decision every screen
//                      shares.
//   `status_paint.cpp` the status screen's layout, behind the `DrawList` seam.
//   `conflicts_pump.cpp` the conflicts screen's IPC frame (#209).
//   `lifecycle.cpp`    start, show, and the one teardown every exit path ends
//                      in; and `Cadence`, the clock a screen polls by (#209).
//   `log_paint.cpp`    the log screen's layout (#209).
//
// The far side of the wire is the *real* dispatch table -- `ipc::Dispatch` over
// an `ipc::ServiceCore` -- and the real `Error` -> `Result` mapping the
// sysmodule uses, `sysmodule::ToResult`. So a payload that fails here fails
// between the two halves as they ship, not between a test and a mock. What is
// NOT here is the `cmif`/`hipc` unpacking in `sysmodule/source/ipc/service.cpp`:
// see `tests/hostswitch/switch.h` and docs/TESTING.md for that limit.
//
// One scenario per CTest entry (`overlay.card`, `overlay.wire`, ...), selected
// by argv[1], so a red run names the behaviour that broke. Nothing here needs a
// server, so nothing here skips.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "checks.hpp"
#include "count_new.hpp"
#include "fake_engine.hpp"
#include "scratch.hpp"

#include "rommsync/config.hpp"
#include "rommsync/conflict_log.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/log.hpp"
#include "rommsync/overlay_conflicts_view.hpp"
#include "rommsync/overlay_log_view.hpp"
#include "rommsync/overlay_status_view.hpp"

// The overlay's own headers, and the sysmodule's half of the error mapping.
// Both reach `switch.h`, which here is `tests/hostswitch/switch.h`.
#include "card_probe.hpp"
#include "conflicts_pump.hpp"
#include "draw_list.hpp"
#include "ipc_client.hpp"
#include "lifecycle.hpp"
#include "log_paint.hpp"
#include "prompts.hpp"
#include "result.hpp"
#include "screen_frame.hpp"
#include "status_paint.hpp"

namespace {

using checks::Checks;
namespace config = rommsync::config;
namespace ipc = rommsync::ipc;
namespace overlay = rommsync::overlay;
namespace sysmodule = rommsync::sysmodule;

// --- the far side of the wire -------------------------------------------------

/// What `sysmodule::HandleRequest` does once the `cmif` message is unpacked:
/// bound the request, dispatch it, bound the response, and answer with the
/// `Result` the real mapping produces.
///
/// A sysmodule that does not answer until it is told to (M9-6, #209).
///
/// What a wedged IPC thread looks like from the overlay's side: the call goes
/// out and nothing comes back. `Handle` runs on `IpcClient`'s worker, so this
/// blocks *that* thread -- which is the whole claim under test: the draw thread
/// is not the one that waits.
class Hold {
 public:
  void Arm() {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = true;
    entered_ = false;
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      armed_ = false;
    }
    changed_.notify_all();
  }

  /// The far side's half: wait here while armed.
  void Pass() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!armed_) {
      return;
    }
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return !armed_; });
  }

  /// A call is being held right now, or was within `budget`.
  bool AwaitEntered(std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, budget, [this] { return entered_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool armed_ = false;
  bool entered_ = false;
};

/// The knobs are the things a *healthy* sysmodule cannot be asked to do on
/// demand and that the overlay has code for anyway -- answer a payload from
/// another build, answer one that will not parse, claim a length that is not
/// the one it wrote, and not answer at all. Everything else goes through
/// `ipc::Dispatch`.
class HostSysmodule : public hostswitch::Server {
 public:
  explicit HostSysmodule(ipc::ServiceCore& core) : core_(core) {}

  /// Answered instead of dispatching, with a successful `Result`. A sysmodule
  /// from another release, or one whose reply was cut.
  std::optional<std::string> canned;

  /// The reply's length word, when it is to disagree with what was written.
  std::optional<std::uint64_t> claim_length;

  /// Armed, every call blocks before it is dispatched. See `Hold`.
  Hold hold;

  /// Atomic because a call the overlay gave up on is still running on the
  /// client's worker while the test reads this.
  std::atomic<int> calls{0};
  std::uint32_t last_command = 0;
  std::string last_request;

  Result Handle(u32 command_id, const void* request, std::size_t request_size, void* response,
                std::size_t capacity, u64* length) override {
    ++calls;
    hold.Pass();
    last_command = command_id;
    last_request.assign(static_cast<const char*>(request), request_size);
    *length = 0;

    std::string payload;
    ipc::Error error = ipc::Error::kOk;
    if (canned.has_value()) {
      payload = *canned;
    } else if (request_size > ipc::kMaxPayloadBytes) {
      // `HandleRequest` refuses before it copies, for the reason written there.
      error = ipc::Error::kMalformedRequest;
    } else {
      error = ipc::Dispatch(core_, command_id, last_request, &payload);
    }

    if (error == ipc::Error::kOk && !payload.empty() && capacity < payload.size()) {
      // Never a partial answer -- `HandleRequest`'s rule, for its reason.
      error = ipc::Error::kTooLarge;
      payload.clear();
    }
    if (error != ipc::Error::kOk) {
      return sysmodule::ToResult(error);
    }
    std::memcpy(response, payload.data(), payload.size());
    *length = claim_length.value_or(payload.size());
    return sysmodule::ToResult(ipc::Error::kOk);
  }

 private:
  ipc::ServiceCore& core_;
};

/// One console: an engine, the service over it, and the port registered.
///
/// `IpcClient::Open` finds the port through the shim's `smGetService`, so a
/// fixture that never registers is a console with no sysmodule -- which is the
/// state several assertions below are about.
class Console {
 public:
  Console() : core_(engine_), sysmodule_(core_) {}
  ~Console() { Stop(); }

  Console(const Console&) = delete;
  Console& operator=(const Console&) = delete;

  void Start() { hostswitch::Register(ipc::kServiceName, &sysmodule_); }
  void Stop() { hostswitch::Unregister(ipc::kServiceName, &sysmodule_); }

  fakes::FakeEngine& engine() { return engine_; }
  HostSysmodule& sysmodule() { return sysmodule_; }

 private:
  fakes::FakeEngine engine_;
  ipc::ServiceCore core_;
  HostSysmodule sysmodule_;
};

// --- overlay.card -------------------------------------------------------------

/// A card laid out under `root`, one file at a time.
void Touch(const std::filesystem::path& path, std::string_view contents = "") {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << contents;
}

std::filesystem::path CardRoot(const std::string& leaf) {
  const std::filesystem::path root = std::filesystem::path(scratch::Dir()) / leaf;
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root);
  return root;
}

std::filesystem::path ContentsOf(const std::filesystem::path& root) {
  return root / "atmosphere" / "contents" / overlay::kProgramIdHex;
}

int RunCard(Checks& checks) {
  // Nothing on the card at all: the state a user whose unzip never landed is in,
  // and the one `RenderUnreachable` has a different sentence for.
  {
    const std::filesystem::path root = CardRoot("card-empty");
    const overlay::CardState card = overlay::ProbeCardAt(root.string());
    checks.Expect(!card.installed, "an empty card is not installed");
    checks.Expect(!card.listable, "an empty card has no toolbox.json");
    checks.Expect(!card.set_to_boot, "an empty card is not set to boot");
    checks.Expect(!card.config_read, "an empty card has no config.ini to read");
    checks.Expect(!card.sync_enabled, "and reports nothing about [sync] enabled");
  }

  // Installed, and the boot toggle off. This is the whole of M6-2 (#33): one
  // sentence apart from the state above, with a different thing to do about it.
  {
    const std::filesystem::path root = CardRoot("card-installed");
    Touch(ContentsOf(root) / "exefs.nsp", "not really a sysmodule");
    Touch(ContentsOf(root) / "toolbox.json", "{}");
    const overlay::CardState card = overlay::ProbeCardAt(root.string());
    checks.Expect(card.installed, "exefs.nsp on the card is 'installed'");
    checks.Expect(card.listable, "toolbox.json is what ovl-sysmodules lists");
    checks.Expect(!card.set_to_boot, "...and the boot toggle is still off");
  }

  // Installed and set to boot, with `[sync] enabled = false` in the file. A
  // resident sysmodule that is not syncing is a third state again.
  {
    const std::filesystem::path root = CardRoot("card-booting");
    Touch(ContentsOf(root) / "exefs.nsp", "x");
    Touch(ContentsOf(root) / "toolbox.json", "{}");
    Touch(ContentsOf(root) / "flags" / "boot2.flag", "");
    Touch(root / std::string(config::kConfigSdPath).substr(1),
          "[sync]\nenabled = false\n");
    const overlay::CardState card = overlay::ProbeCardAt(root.string());
    checks.Expect(card.installed && card.listable, "the install is complete");
    checks.Expect(card.set_to_boot, "the boot flag is what ovl-sysmodules wrote");
    checks.Expect(card.config_read, "config.ini was there to read");
    checks.Expect(!card.sync_enabled, "and [sync] enabled = false is the user's own switch");
  }

  // The file present and saying nothing about `enabled`: the default applies,
  // and it applies because the file was read rather than because it was absent.
  {
    const std::filesystem::path root = CardRoot("card-default");
    Touch(root / std::string(config::kConfigSdPath).substr(1), "[server]\nurl = \n");
    const overlay::CardState card = overlay::ProbeCardAt(root.string());
    checks.Expect(card.config_read, "a config.ini that parses to defaults still read");
    checks.ExpectEq(card.sync_enabled, config::Defaults().sync.enabled,
                    "an unstated [sync] enabled is the default, not false");
  }

  // The device call site still names `sdmc:`, which no host has -- so it answers
  // the empty card. Asserted so that the seam cannot quietly become the default.
  {
    const overlay::CardState card = overlay::ProbeCard();
    checks.Expect(!card.installed && !card.config_read,
                  "ProbeCard() reads sdmc:, which is not a host mount");
  }
  return checks.failures();
}

// --- overlay.wire -------------------------------------------------------------

/// Every command the client can send, with something valid to send it.
///
/// A table rather than one call per assertion: what is under test is that the
/// client reaches the right command id with the right buffers, and that is the
/// same claim seventeen times.
struct ClientCall {
  ipc::Command command;
  const char* name;
  Result (*send)(overlay::IpcClient&);
};

constexpr ClientCall kClientCalls[] = {
    {ipc::Command::kGetInterfaceVersion, "GetInterfaceVersion",
     [](overlay::IpcClient& c) {
       std::uint32_t out = 0;
       return c.GetInterfaceVersion(&out);
     }},
    {ipc::Command::kGetStatus, "GetStatus",
     [](overlay::IpcClient& c) {
       ipc::Status out;
       return c.GetStatus(&out);
     }},
    {ipc::Command::kGetConfig, "GetConfig",
     [](overlay::IpcClient& c) {
       ipc::ConfigView out;
       return c.GetConfig(&out);
     }},
    {ipc::Command::kSetConfig, "SetConfig",
     [](overlay::IpcClient& c) {
       ipc::ConfigResult out;
       return c.SetConfig(ipc::ConfigEdit{}, &out);
     }},
    {ipc::Command::kSetEnabled, "SetEnabled",
     [](overlay::IpcClient& c) {
       ipc::EnabledResult out;
       return c.SetEnabled(true, &out);
     }},
    {ipc::Command::kSyncNow, "SyncNow",
     [](overlay::IpcClient& c) {
       ipc::SyncOutcome out = ipc::SyncOutcome::kAccepted;
       return c.SyncNow(&out);
     }},
    {ipc::Command::kStartPair, "StartPair",
     [](overlay::IpcClient& c) {
       rommsync::auth::PairingStatus out;
       return c.StartPair(&out);
     }},
    {ipc::Command::kGetPairState, "GetPairState",
     [](overlay::IpcClient& c) {
       rommsync::auth::PairingStatus out;
       return c.GetPairState(&out);
     }},
    {ipc::Command::kUnpair, "Unpair", [](overlay::IpcClient& c) { return c.Unpair(); }},
    {ipc::Command::kEnqueue, "Enqueue",
     [](overlay::IpcClient& c) {
       std::int32_t out = 0;
       return c.Enqueue(7, &out);
     }},
    {ipc::Command::kDequeue, "Dequeue", [](overlay::IpcClient& c) { return c.Dequeue(7); }},
    {ipc::Command::kListBegin, "ListBegin",
     [](overlay::IpcClient& c) {
       ipc::Cursor out = 0;
       return c.ListBegin(ipc::ListRequest{}, &out);
     }},
    {ipc::Command::kListNext, "ListNext",
     [](overlay::IpcClient& c) {
       ipc::ListPage out;
       return c.ListNext(7, &out);
     }},
    {ipc::Command::kListEnd, "ListEnd", [](overlay::IpcClient& c) { return c.ListEnd(7); }},
    {ipc::Command::kListConflicts, "ListConflicts",
     [](overlay::IpcClient& c) {
       ipc::ConflictPage out;
       return c.ListConflicts(ipc::ConflictQuery{}, &out);
     }},
    {ipc::Command::kRestoreBackup, "RestoreBackup",
     [](overlay::IpcClient& c) {
       ipc::RestoreState out;
       return c.RestoreBackup(3, &out);
     }},
    {ipc::Command::kGetRestoreState, "GetRestoreState",
     [](overlay::IpcClient& c) {
       ipc::RestoreState out;
       return c.GetRestoreState(&out);
     }},
    {ipc::Command::kGetLog, "GetLog",
     [](overlay::IpcClient& c) {
       ipc::LogTail out;
       return c.GetLog(ipc::kMaxLogLines, &out);
     }},
};

int RunWire(Checks& checks) {
  Console console;
  console.Start();
  overlay::IpcClient client;
  checks.Expect(R_SUCCEEDED(client.Open()), "the port opens");

  for (const ClientCall& call : kClientCalls) {
    console.sysmodule().last_command = 0xFFFFFFFFu;
    const Result rc = call.send(client);
    checks.ExpectEq(console.sysmodule().last_command,
                    static_cast<std::uint32_t>(call.command),
                    std::string(call.name) + " sends its own command id");
    // The request is a JSON object on every command, including the ones that
    // carry nothing: `Dispatch` holds a no-argument command to `{}` rather than
    // ignoring what it was sent (docs/DEVELOPMENT.md#ipc).
    checks.Expect(!console.sysmodule().last_request.empty(),
                  std::string(call.name) + " sends a payload, never an empty buffer");
    ipc::Error refusal = ipc::Error::kOk;
    checks.Expect(R_SUCCEEDED(rc) || overlay::DecodeError(rc, &refusal),
                  std::string(call.name) + " either succeeds or names its refusal");
  }

  // Both buffers, pointed the right way. A request tagged Out is a buffer the
  // sysmodule may not read, and on a console the symptom would be whatever the
  // kernel left in the map rather than an error.
  const hostswitch::Dispatched& last = hostswitch::LastDispatched();
  checks.ExpectEq(last.in_attr,
                  static_cast<std::uint32_t>(SfBufferAttr_HipcMapAlias | SfBufferAttr_In),
                  "the request buffer is a mapped In buffer");
  checks.ExpectEq(last.out_attr,
                  static_cast<std::uint32_t>(SfBufferAttr_HipcMapAlias | SfBufferAttr_Out),
                  "the response buffer is a mapped Out buffer");
  checks.ExpectEq(last.response_capacity, ipc::kMaxPayloadBytes,
                  "the response buffer is the contract's whole cap");

  // Every command this contract has, against the client that is supposed to be
  // able to send it. `kGetLog` was the one hole until M9-6 (#209) wired it to
  // the log screen; there is none now, so a *new* command with no client is a
  // red test rather than a discovery on a console.
  for (const ipc::Command command : ipc::kAllCommands) {
    bool covered = false;
    for (const ClientCall& call : kClientCalls) {
      covered = covered || call.command == command;
    }
    checks.Expect(covered, std::string("IpcClient can send ") + ipc::ToString(command));
  }

  // A request past the cap is refused on this side rather than sent. The far
  // side would refuse it too, and a client that could not express its own
  // request is a bug on this side.
  {
    ipc::ConfigEdit huge;
    huge.assignments.push_back(config::Assignment{
        "server", "url", std::string(ipc::kMaxPayloadBytes * 2, 'u'), false});
    const int before = console.sysmodule().calls;
    ipc::ConfigResult result;
    const Result rc = client.SetConfig(huge, &result);
    checks.Expect(R_FAILED(rc), "an over-long request fails");
    checks.ExpectEq(console.sysmodule().calls.load(), before,
                    "...without reaching the sysmodule at all");
  }
  return checks.failures();
}

// --- overlay.roundtrip --------------------------------------------------------

int RunRoundtrip(Checks& checks) {
  Console console;
  fakes::FakeEngine& engine = console.engine();
  engine.snapshot.auth = ipc::AuthState::kPaired;
  engine.snapshot.online = true;
  engine.snapshot.last_sync_at = 1710000000;
  engine.snapshot.last_sync_result = ipc::SyncResult::kPartial;
  engine.snapshot.sync_in_progress = true;
  engine.snapshot.uploaded = 3;
  engine.snapshot.downloaded = 4;
  engine.snapshot.conflicts = 5;
  engine.snapshot.failed = 6;
  engine.snapshot.queue_depth = 7;
  engine.snapshot.download.state = ipc::DownloadState::kDownloading;
  engine.snapshot.download.rom_id = 99;
  engine.snapshot.download.fs_name = "Some Game (USA) (Rev 1).gba";
  engine.snapshot.download.bytes_done = 512;
  engine.snapshot.download.bytes_total = 2048;
  engine.settings.sync.enabled = true;
  engine.settings.server.url = "http://romm.example:8080";
  console.Start();

  overlay::IpcClient client;
  checks.Expect(R_SUCCEEDED(client.Open()), "the port opens");

  // Encoded by `ipc::EncodeStatus` on the sysmodule's side and decoded by
  // `ipc::DecodeStatus` inside the overlay's own `CallAndDecode`. Every field a
  // round trip could drop carries a value that is not its default.
  ipc::Status status;
  checks.Expect(R_SUCCEEDED(client.GetStatus(&status)), "GetStatus succeeds");
  checks.ExpectEq(status.interface, ipc::kVersion, "the interface version rides on Status");
  checks.Expect(status.enabled, "[sync] enabled survives the wire");
  checks.Expect(status.configured, "a usable server.url reads as configured");
  checks.Expect(status.online, "online survives");
  checks.ExpectEq(status.last_sync_at, static_cast<std::int64_t>(1710000000),
                  "the last sync time survives");
  checks.Expect(status.last_sync_result == ipc::SyncResult::kPartial,
                "the last sync result survives");
  checks.Expect(status.sync_in_progress, "a running tick survives");
  checks.ExpectEq(status.uploaded, static_cast<std::int64_t>(3), "uploaded survives");
  checks.ExpectEq(status.downloaded, static_cast<std::int64_t>(4), "downloaded survives");
  checks.ExpectEq(status.conflicts, static_cast<std::int64_t>(5), "conflicts survives");
  checks.ExpectEq(status.failed, static_cast<std::int64_t>(6), "failed survives");
  checks.ExpectEq(status.queue_depth, static_cast<std::int64_t>(7), "the queue depth survives");
  checks.ExpectEq(status.download.fs_name, engine.snapshot.download.fs_name,
                  "the downloading file's name survives");
  checks.ExpectEq(status.download.bytes_done, static_cast<std::int64_t>(512),
                  "the bytes already on the card survive");
  checks.ExpectEq(status.download.bytes_total, static_cast<std::int64_t>(2048),
                  "the declared length survives");

  // The one payload carrying a URL, and the diagnostics the settings screen is
  // for.
  config::Diagnostic note;
  note.severity = config::Severity::kError;
  note.line = 9;
  note.section = "sync";
  note.key = "enabled";
  note.message = "expected true or false";
  engine.notes.push_back(note);
  ipc::ConfigView view;
  checks.Expect(R_SUCCEEDED(client.GetConfig(&view)), "GetConfig succeeds");
  checks.ExpectEq(view.config.server.url, engine.settings.server.url,
                  "the configured URL survives");
  checks.ExpectEq(view.diagnostics.size(), std::size_t{1}, "the diagnostic survives");
  if (!view.diagnostics.empty()) {
    checks.ExpectEq(view.diagnostics[0].line, 9, "...with its line number");
  }

  // A list page, which is the payload with a vector in it.
  ipc::ListItem item;
  item.fields.push_back(ipc::ListField{"rom_id", ipc::ListValue::Integer(11)});
  item.fields.push_back(ipc::ListField{"name", ipc::ListValue::Text("Some Game")});
  item.fields.push_back(ipc::ListField{"fs_name", ipc::ListValue::Text("some-game.gba")});
  engine.page.items.push_back(item);
  engine.page.has_more = true;
  ipc::ListPage page;
  checks.Expect(R_SUCCEEDED(client.ListNext(7, &page)), "ListNext succeeds");
  checks.ExpectEq(page.items.size(), std::size_t{1}, "the page's one item survives");
  checks.Expect(page.has_more, "...and so does has_more");
  if (!page.items.empty()) {
    const ipc::ListValue* rom_id = page.items[0].Find("rom_id");
    const ipc::ListValue* fs_name = page.items[0].Find("fs_name");
    checks.Expect(rom_id != nullptr && rom_id->number == 11, "...with its rom id, as an integer");
    checks.Expect(fs_name != nullptr && fs_name->text == "some-game.gba",
                  "...and its fs_name, as text");
  }
  checks.ExpectEq(engine.last_cursor, static_cast<ipc::Cursor>(7),
                  "the cursor the overlay sent is the one the engine was asked for");

  // `Enqueue`'s answer is a bare integer and its request is another one; the
  // two are spelled apart on purpose (`EncodeEntryId`), so a client that sent
  // one where the other was wanted is what this catches.
  engine.enqueue_position = 4;
  std::int32_t position = 0;
  checks.Expect(R_SUCCEEDED(client.Enqueue(42, &position)), "Enqueue succeeds");
  checks.ExpectEq(position, 4, "the queue position comes back");
  checks.ExpectEq(engine.last_rom_id, static_cast<std::int64_t>(42),
                  "and the rom id went the other way");

  engine.restore_report.outcome = rommsync::conflicts::RestoreOutcome::kRestored;
  engine.restore_report.message = "restored from 2026-03-01";
  engine.restore_report.backup_sd_path = "/switch/rommsync/.backup/some-game.sav";
  // Taken and finished on the spot, which is what an engine with no worker
  // does -- so the whole state, report included, crosses in one answer.
  ipc::RestoreState state;
  checks.Expect(R_SUCCEEDED(client.RestoreBackup(3, &state)), "RestoreBackup succeeds");
  checks.Expect(state.phase == ipc::RestorePhase::kDone, "the phase survives");
  checks.ExpectEq(state.entry_id, static_cast<std::int64_t>(3), "...and which restore it is");
  const rommsync::conflicts::RestoreReport& report = state.report;
  checks.Expect(report.ok(), "the restore outcome survives");
  checks.ExpectEq(report.message, engine.restore_report.message, "...and its message");
  checks.ExpectEq(report.backup_sd_path, engine.restore_report.backup_sd_path,
                  "...and the backup this restore itself wrote first (hard rule 2)");
  checks.ExpectEq(engine.last_restore_id, static_cast<std::int64_t>(3),
                  "against the entry id the overlay named");
  // The log tail, which the log screen reads (#209). The sysmodule answers it
  // from `log::Tail`, so what goes in is a line logged in this process.
  rommsync::log::Reset();
  rommsync::log::Warn(rommsync::log::Event::kNetOffline, "negotiate: connection failed");
  ipc::LogTail tail;
  checks.Expect(R_SUCCEEDED(client.GetLog(ipc::kMaxLogLines, &tail)), "GetLog succeeds");
  checks.ExpectEq(tail.total, static_cast<std::int64_t>(1), "the total survives");
  checks.Expect(tail.lines.size() == 1 &&
                    tail.lines[0].find("negotiate: connection failed") != std::string::npos,
                "and the line, as the log rendered it");

  ipc::RestoreState polled;
  checks.Expect(R_SUCCEEDED(client.GetRestoreState(&polled)), "GetRestoreState succeeds");
  checks.Expect(polled.phase == ipc::RestorePhase::kDone && polled.entry_id == 3 &&
                    polled.report.message == engine.restore_report.message,
                "and answers the same restore when polled");
  return checks.failures();
}

// --- overlay.version ----------------------------------------------------------

int RunVersion(Checks& checks) {
  Console console;
  console.Start();
  overlay::IpcClient client;
  overlay::ScreenFrame frame(client);

  // The agreeing case first, so the disagreeing ones are a change rather than
  // an absence.
  checks.Expect(frame.Ready() == overlay::Link::kOk,
                "a sysmodule speaking this contract is ready");
  checks.ExpectEq(frame.sysmodule_interface(), ipc::kVersion,
                  "and it answered with the version this build speaks");

  // Command 0's encoding is frozen -- `{"interface":N}`, from every build there
  // will ever be -- which is the whole reason a mismatch is diagnosable at all.
  // Answered here as a literal rather than through the encoder, because an
  // encoder that changed would change both sides of a round trip and say
  // nothing.
  {
    Console older;
    older.sysmodule().canned = std::string("{\"interface\":1}");
    older.Start();
    overlay::IpcClient old_client;
    overlay::ScreenFrame old_frame(old_client);
    const overlay::Link link = old_frame.Ready();
    checks.Expect(link == overlay::Link::kIncompatible,
                  "a sysmodule from an older contract is kIncompatible");
    checks.ExpectEq(old_frame.sysmodule_interface(), std::uint32_t{1},
                    "and the two numbers are the whole diagnosis");

    // Not latched: a user who exits, updates the sysmodule and comes back gets
    // a working screen without rebooting.
    older.sysmodule().canned.reset();
    checks.Expect(old_frame.Ready() == overlay::Link::kOk,
                  "the handshake is re-derived, so an updated sysmodule recovers");

    // The sentence the user is given says what to do about it, which is the
    // difference between this state and `kUnreadable`.
    const overlay::StatusView view =
        overlay::RenderUnreachable(overlay::Link::kIncompatible, overlay::CardState{}, 1);
    checks.Expect(!view.headline.empty() && !view.hint.empty(),
                  "kIncompatible renders a headline and something to do about it");
  }

  // A sysmodule answering command 0 with something this build cannot read is
  // NOT "not running": the port answered. Which of the two unreachable
  // sentences applies is decided by whether the port is still openable.
  {
    Console garbled;
    garbled.sysmodule().canned = std::string("{\"interface\":\"two\"}");
    garbled.Start();
    overlay::IpcClient bad_client;
    overlay::ScreenFrame bad_frame(bad_client);
    checks.Expect(bad_frame.Ready() == overlay::Link::kUnreadable,
                  "an undecodable command 0 is kUnreadable, not kNotRunning");
  }

  // No port at all. The state a user who forgot the boot toggle is in, and the
  // only one the card is then consulted about (`card_probe.hpp`).
  //
  // Asserted as a *contrast*, because the blocks above each registered and
  // unregistered a port of their own: a bare `Ready()` here would answer
  // `kNotRunning` whatever this code did, and the assertion would pass over a
  // `Ready()` that had stopped working entirely.
  {
    Console running;
    running.Start();
    overlay::IpcClient present;
    overlay::ScreenFrame present_frame(present);
    checks.Expect(present_frame.Ready() == overlay::Link::kOk,
                  "a fresh session on a registered port is ready");
  }
  {
    overlay::IpcClient orphan;
    overlay::ScreenFrame orphan_frame(orphan);
    checks.Expect(orphan_frame.Ready() == overlay::Link::kNotRunning,
                  "and with no rommsync port, the same first call is kNotRunning");
  }

  // **A passed handshake is not re-run while the session lives.** So `Ready()`
  // still answers `kOk` after the sysmodule exits underneath it -- a handle
  // stays a handle until somebody uses it, and it is the next *command* that
  // discovers the far end is gone (`overlay.errors` drives that, and
  // `ScreenFrame::Diagnose` is what turns it into a sentence).
  //
  // Pinned rather than left implicit. It is correct as it stands -- a screen
  // polls inside one `update()`, so the failed command and the redraw are the
  // same frame and no stale frame reaches the panel -- but it is the exact
  // shape M9-6 (#209) has to bound: that next command is the one with no
  // timeout in front of it.
  {
    Console live;
    live.Start();
    overlay::IpcClient client;
    overlay::ScreenFrame frame(client);
    checks.Expect(frame.Ready() == overlay::Link::kOk, "the handshake passes");
    live.Stop();
    checks.Expect(frame.Ready() == overlay::Link::kOk,
                  "and Ready() does not re-probe a session it has already checked");
    ipc::Status status;
    const Result rc = client.GetStatus(&status);
    checks.Expect(R_FAILED(rc), "the next command is what fails");
    checks.Expect(frame.Diagnose(rc) == overlay::Link::kNotRunning,
                  "...and Diagnose is what names it 'not running'");
  }
  return checks.failures();
}

// --- overlay.errors -----------------------------------------------------------

int RunErrors(Checks& checks) {
  // `sysmodule::ToResult` and `overlay::DecodeError` are inverses, over every
  // error this build has. They are two files in two binaries that ship
  // separately, and the ordinals are append-only precisely because of it.
  for (const ipc::Error error : ipc::kAllErrors) {
    const Result rc = sysmodule::ToResult(error);
    if (error == ipc::Error::kOk) {
      checks.ExpectEq(rc, Result{0}, "kOk is a Result of 0");
      ipc::Error decoded = ipc::Error::kInvalid;
      checks.Expect(!overlay::DecodeError(rc, &decoded),
                    "a Result of 0 has not failed, so it names no refusal");
      continue;
    }
    checks.Expect(R_FAILED(rc), std::string(ipc::ToString(error)) + " is a failing Result");
    ipc::Error decoded = ipc::Error::kOk;
    checks.Expect(overlay::DecodeError(rc, &decoded),
                  std::string("the overlay reads ") + ipc::ToString(error) + " back");
    checks.Expect(decoded == error,
                  std::string("...as itself, not as ") + ipc::ToString(decoded));
  }

  // A description in our module that this build has no name for is a sysmodule
  // from a newer release, which is `Diagnose`'s to name rather than a refusal
  // to invent.
  {
    const Result future = MAKERESULT(ipc::kResultModule, 4000);
    ipc::Error decoded = ipc::Error::kOk;
    checks.Expect(!overlay::DecodeError(future, &decoded),
                  "an unknown ordinal in our module is not guessed at");
  }
  // ...and neither is somebody else's module.
  {
    ipc::Error decoded = ipc::Error::kOk;
    checks.Expect(!overlay::DecodeError(MAKERESULT(Module_Libnx, LibnxError_NotFound), &decoded),
                  "a libnx failure is not one of our refusals");
  }

  Console console;
  console.engine().enqueue_error = ipc::Error::kDuplicate;
  console.Start();
  overlay::IpcClient client;
  overlay::ScreenFrame frame(client);
  checks.Expect(frame.Ready() == overlay::Link::kOk, "the handshake passes");

  // The case overlay/AGENTS.md names: a refusal the sysmodule meant is not a
  // transport failure, and a screen that sent it to `Diagnose` would draw
  // "sys-rommsync is not running" over a rom that was simply already queued.
  {
    std::int32_t position = 0;
    const Result rc = client.Enqueue(7, &position);
    checks.Expect(R_FAILED(rc), "an already-queued rom is refused");
    ipc::Error refusal = ipc::Error::kOk;
    checks.Expect(overlay::DecodeError(rc, &refusal), "...and the refusal is readable");
    checks.Expect(refusal == ipc::Error::kDuplicate, "...as kDuplicate");
    checks.Expect(rc != overlay::MalformedResponse(),
                  "a refusal is never the payload-unreadable result");
  }

  // A successful call whose payload this build cannot parse. One named
  // `Result`, not a half-filled struct -- three defaulted fields render as a
  // working console with odd numbers.
  {
    console.sysmodule().canned = std::string("{\"enabled\":");
    ipc::Status status;
    status.queue_depth = 1234;
    const Result rc = client.GetStatus(&status);
    checks.ExpectEq(rc, overlay::MalformedResponse(),
                    "a payload that will not parse is MalformedResponse");
    checks.ExpectEq(status.queue_depth, static_cast<std::int64_t>(1234),
                    "...and the caller's struct is left alone");
    checks.Expect(frame.Diagnose(rc) == overlay::Link::kUnreadable,
                  "which the screen draws as unreadable, not as not-running");
    console.sysmodule().canned.reset();
  }

  // A sysmodule claiming it wrote more than the buffer holds. Refused rather
  // than read: the length is another process's number.
  {
    console.sysmodule().claim_length = ipc::kMaxPayloadBytes + 1;
    ipc::Status status;
    const Result rc = client.GetStatus(&status);
    checks.ExpectEq(rc, overlay::MalformedResponse(),
                    "a reply longer than the buffer is refused, not read");
    console.sysmodule().claim_length.reset();
  }

  // The transport gone under a live session. `Diagnose` drops the session and
  // tries the port again, here rather than on the next frame, because whether
  // the port is still there is exactly what decides the sentence.
  {
    console.Stop();
    ipc::Status status;
    const Result rc = client.GetStatus(&status);
    checks.Expect(R_FAILED(rc), "a call into a sysmodule that went away fails");
    checks.Expect(frame.Diagnose(rc) == overlay::Link::kNotRunning,
                  "and a port that will not re-open is 'not running'");
    checks.Expect(!client.open(), "the session is dropped rather than kept");
  }
  return checks.failures();
}

// --- overlay.timeout ----------------------------------------------------------

/// The budget these scenarios give a call. Short, so a red run is quick; the
/// device's is `IpcClient::kCallBudget`.
constexpr std::chrono::milliseconds kTestBudget{60};

/// How long a call that is meant to return at once may take on a loaded CI
/// runner. Well under any budget a frame could notice.
constexpr std::chrono::milliseconds kPrompt{40};

/// Generous, for the waits that are not the claim: a thread starting, a late
/// answer arriving.
constexpr std::chrono::milliseconds kPatience{5000};

using Clock = std::chrono::steady_clock;

std::chrono::milliseconds Since(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
}

/// M9-6 (#209): a sysmodule that does not answer holds the overlay's draw
/// thread for one budget, once, and then not at all.
///
/// Before this, `IpcClient::Call` was `serviceDispatchOut` on the draw thread,
/// and a call into a wedged sysmodule blocked there **forever** -- the overlay
/// frozen on the panel, holding exclusive input, with nothing a user can press.
int RunTimeout(Checks& checks) {
  Console console;
  console.Start();
  overlay::IpcClient client(kTestBudget);
  overlay::ScreenFrame frame(client);
  checks.Expect(frame.Ready() == overlay::Link::kOk, "the handshake passes while it answers");

  // The bound. The call comes back at the budget with a `Result` of its own,
  // while the far side is still sitting on it.
  console.sysmodule().hold.Arm();
  {
    const Clock::time_point start = Clock::now();
    ipc::Status status;
    status.queue_depth = 1234;
    const Result rc = client.GetStatus(&status);
    const std::chrono::milliseconds took = Since(start);
    checks.Expect(console.sysmodule().hold.AwaitEntered(kPatience),
                  "the call reached the sysmodule, which is sitting on it");
    checks.ExpectEq(rc, overlay::NotResponding(), "a call past its budget is NotResponding");
    checks.Expect(took >= kTestBudget, "...after the budget, not before it");
    checks.Expect(took < kTestBudget + kPatience, "...and not long after it");
    checks.ExpectEq(status.queue_depth, static_cast<std::int64_t>(1234),
                    "the caller's struct is left alone");
    ipc::Error refusal = ipc::Error::kOk;
    checks.Expect(!overlay::DecodeError(rc, &refusal),
                  "it is not a refusal the sysmodule named -- nothing was answered");
  }

  // What it means, and what it does not do. **The session is not dropped**:
  // the worker is still inside a call on it, and closing a handle another
  // thread is dispatching on is the one thing worse than waiting.
  checks.Expect(frame.Diagnose(overlay::NotResponding()) == overlay::Link::kNotResponding,
                "Diagnose names it 'not responding', not 'not running'");
  checks.Expect(client.open(), "...and keeps the session the call is still on");
  checks.Expect(client.busy(), "the client knows a call is still out");
  {
    const overlay::StatusView view = overlay::RenderUnreachable(overlay::Link::kNotResponding);
    checks.Expect(!view.headline.empty() && !view.hint.empty(),
                  "kNotResponding renders a headline and something to do about it");
    checks.Expect(view.headline != overlay::RenderUnreachable(overlay::Link::kNotRunning).headline,
                  "...and it is not the 'not running' sentence: it is running, and stuck");
  }

  // **Once, and then not at all.** While the first call is still out, every
  // later one is answered at once without being sent -- one session answers one
  // request at a time -- so the frames after the first are not held either.
  {
    const int before = console.sysmodule().calls.load();
    for (int frame_number = 0; frame_number < 5; ++frame_number) {
      const Clock::time_point start = Clock::now();
      ipc::ConfigView view;
      const Result rc = client.GetConfig(&view);
      checks.ExpectEq(rc, overlay::NotResponding(), "a frame while it is stuck is answered...");
      checks.Expect(Since(start) < kPrompt, "...at once, not after another budget");
    }
    checks.ExpectEq(console.sysmodule().calls.load(), before,
                    "and nothing more was sent into a session that has not answered");
  }

  // The handshake is bounded the same way. A fresh client whose first call --
  // command 0 -- is held is "not responding", not "unreadable": `Ready` must not
  // drop and reopen a session a worker is still dispatching on.
  //
  // Declared out here rather than in a block: its destructor waits for the call
  // it still has out, and that call is released below.
  overlay::IpcClient fresh(kTestBudget);
  overlay::ScreenFrame fresh_frame(fresh);
  checks.Expect(fresh_frame.Ready() == overlay::Link::kNotResponding,
                "a handshake that is not answered is kNotResponding");
  checks.Expect(fresh.open(), "...on a session that is kept");

  // It recovers by itself, and the late answer goes nowhere. The `Status` the
  // sysmodule finally sends is not handed to whichever call comes next -- a
  // `GetConfig` that decoded a `Status` would be a `MalformedResponse`, and one
  // that *could* decode it would be worse.
  console.sysmodule().hold.Release();
  {
    Result rc = overlay::NotResponding();
    ipc::ConfigView view;
    const Clock::time_point start = Clock::now();
    while (rc == overlay::NotResponding() && Since(start) < kPatience) {
      rc = client.GetConfig(&view);
    }
    checks.ExpectEq(rc, Result{0}, "once it answers, the next call goes through");
    checks.Expect(!client.busy(), "and nothing is left out");
    checks.Expect(frame.Ready() == overlay::Link::kOk, "the screen is back without a reopen");
  }

  // An exit with a call still out waits for it: the worker cannot be left
  // running into an overlay that is about to be unloaded. libtesla releases
  // input before `exitServices` (tesla.hpp's tail), so this waits holding none.
  {
    console.sysmodule().hold.Arm();
    ipc::Status status;
    checks.ExpectEq(client.GetStatus(&status), overlay::NotResponding(), "a call is left out");
    std::thread releaser([&console] {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
      console.sysmodule().hold.Release();
    });
    client.Shutdown();
    releaser.join();
    checks.Expect(!client.open(), "Shutdown closes the session once the call is back");
    checks.Expect(!client.busy(), "...with nothing left out");
  }
  fresh.Shutdown();
  checks.ExpectEq(hostswitch::OpenSessions(), 0, "every session this scenario opened is closed");
  return checks.failures();
}

// --- overlay.restore ----------------------------------------------------------

/// M9-6 (#209): a restore that is still copying holds no frame.
///
/// `RestoreBackup` used to copy the save before it answered, on the
/// sysmodule's IPC thread, with the overlay's draw thread inside
/// `serviceDispatchOut` for the whole of it -- a large save on a slow card was a
/// frozen overlay holding the user's input. Now the command answers the moment
/// the restore is taken, and the screen polls. This drives the conflicts
/// screen's own frame -- `PumpConflicts`, the code `ConflictsScreen::update`
/// runs -- through the real client against an engine whose restore does not
/// finish until the test says so, and times every frame.
///
/// The sysmodule's half of the same claim -- that the copy really is off its
/// IPC thread -- is `engine.restore_polled`, against the real `SdEngine`.
int RunRestore(Checks& checks) {
  Console console;
  fakes::FakeEngine& engine = console.engine();
  engine.restore_runs_inline = false;
  engine.restore_report.outcome = rommsync::conflicts::RestoreOutcome::kRestored;
  {
    ipc::ConflictRow row;
    row.entry.id = 7;
    row.entry.kind = rommsync::conflicts::EntryKind::kSave;
    row.entry.event = rommsync::conflicts::Event::kReplaced;
    row.entry.rom_id = 4;
    row.entry.rom_name = "Game";
    row.entry.file_name = "game.srm";
    row.entry.sd_path = "/roms/gba/game.srm";
    row.entry.backup_sd_path = "/config/rommsync/.backup/4-retroarch-srm-1.srm";
    row.backup_present = true;
    engine.conflicts_.push_back(row);
  }
  console.Start();

  overlay::IpcClient client(kTestBudget);
  overlay::ScreenFrame frame(client);
  overlay::ConflictsModel model;
  overlay::Link link = overlay::Link::kOk;

  // One frame, timed. What a frame costs is the claim, so every frame below
  // goes through this.
  std::chrono::milliseconds slowest{0};
  const auto pump = [&] {
    const Clock::time_point start = Clock::now();
    overlay::PumpConflicts(frame, client, model, link);
    slowest = std::max(slowest, Since(start));
  };

  pump();  // the handshake and the first page
  checks.ExpectEq(model.loaded(), std::size_t{1}, "the history's one entry is loaded");
  model.Activate();  // open it
  model.Activate();  // confirm
  model.Activate();  // press

  pump();  // the restore goes out, and is answered at once
  checks.ExpectEq(engine.last_restore_id, static_cast<std::int64_t>(7),
                  "the restore was asked for the entry that was open");
  overlay::ConflictsView view = model.Render();
  checks.Expect(view.headline.find("Restoring") != std::string::npos,
                "the screen says it is restoring rather than freezing: " + view.headline);

  // **The copy is still running for all of these**, and not one frame waits
  // for it: each is one poll, answered from memory.
  for (int frames = 0; frames < 30; ++frames) {
    pump();
  }
  checks.Expect(slowest < kPrompt, "no frame is held while the save copies: the slowest took " +
                                       std::to_string(slowest.count()) + "ms");
  checks.Expect(model.Render().headline.find("Restoring") != std::string::npos,
                "and it still says so, thirty frames in");
  checks.ExpectEq(engine.restore_requests, 1, "the restore was asked for once, then polled");

  engine.FinishRestore();
  pump();
  view = model.Render();
  checks.Expect(view.headline.find("Restored") != std::string::npos,
                "the outcome arrives on the next frame after it finishes: " + view.headline);
  checks.Expect(!view.can_restore, "with the restore no longer on offer");
  checks.Expect(model.Next().kind == overlay::ConflictsModel::Command::Kind::kNone,
                "and the polling stops");
  checks.Expect(slowest < kPrompt, "and still no frame was held");
  return checks.failures();
}

// --- overlay.cycles -----------------------------------------------------------

std::string WithoutComment(const std::string& line);  // overlay.portable's, below

/// Every `requestForeground` the lifecycle made, in order. A plain function
/// rather than a lambda with captures, because on a console it is libtesla's
/// free function and `Lifecycle` takes a function pointer.
std::vector<bool>& ForegroundCalls() {
  static std::vector<bool> calls;
  return calls;
}
void RecordForeground(bool enabled) { ForegroundCalls().push_back(enabled); }

/// `RommsyncOverlay`, minus `tsl::Overlay`: the client and the lifecycle it
/// owns, declared in the same order `main.cpp` declares them.
struct OverlayUnderTest {
  overlay::IpcClient client{kTestBudget};
  overlay::Lifecycle lifecycle{client, &RecordForeground};
};

/// One way libtesla's loop ends, as our overlay's hooks see it.
///
/// Read off `tsl::loop` in the pinned `tesla.hpp`. Every `break` out of its
/// outer loop -- B at the root of the gui stack (`shouldClose`), a close
/// requested while hidden (`shouldCloseAfter`), another overlay being launched
/// (`launchingOverlay`), and the launch combo pressed over a notification while
/// hidden (`exitAfterPrompt`) -- lands on one tail: `requestForeground(false)`,
/// `exitScreen()`, `exitServices()`, `delete overlay`. What differs between them
/// is only how many times the overlay was shown first, and `RunCycles` greps
/// that tail so the table cannot drift from the library. The fifth row is the
/// path nothing in the pinned library takes and a bump could: the overlay
/// deleted without `exitServices` -- which the destructor has to cover, because
/// nx-ovlloader's process never exits and anything left open is open for the
/// life of the console (libtesla#37, Ultrahand#278).
struct ExitPath {
  const char* name;
  int shows;
  bool exit_services;
};

constexpr ExitPath kExitPaths[] = {
    {"B at the root (shouldClose)", 1, true},
    {"closed while hidden (shouldCloseAfter)", 2, true},
    {"another overlay launched (launchingOverlay)", 1, true},
    {"the combo over a hidden notification (exitAfterPrompt)", 0, true},
    {"deleted without exitServices", 1, false},
};

/// How many times each path is driven. Enough that a counted resource leaking
/// one per open runs out where Ultrahand#278's "sixth attempt" would.
constexpr int kCycles = 8;

/// `text` with every `//` comment removed, line by line.
std::string CodeOnly(const std::string& text) {
  std::string out;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) {
      end = text.size();
    }
    out += WithoutComment(text.substr(at, end - at));
    out += '\n';
    at = end + 1;
  }
  return out;
}

std::string ReadWhole(const std::filesystem::path& path) {
  std::ifstream file(path);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool IsIdentifier(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// `word` as a whole token: `return`, and not `returning` or `returnPath`.
std::size_t CountWord(const std::string& haystack, std::string_view word) {
  std::size_t count = 0;
  for (std::size_t at = haystack.find(word); at != std::string::npos;
       at = haystack.find(word, at + word.size())) {
    const bool starts = at == 0 || !IsIdentifier(haystack[at - 1]);
    const std::size_t after = at + word.size();
    const bool ends = after >= haystack.size() || !IsIdentifier(haystack[after]);
    if (starts && ends) {
      ++count;
    }
  }
  return count;
}

std::size_t Count(const std::string& haystack, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

/// M9-6 (#209): every way the overlay closes gives back everything it took,
/// and does it the same way.
///
/// nx-ovlloader branches back to `loadNro` when `main` returns, so the process
/// never exits and nothing the kernel would reclaim at exit is reclaimed. The
/// corpus rule is nx-ovlloader#41's: *normal teardown is safe; every other path
/// leaks state only a reboot clears.* So the claim is per path, and per cycle.
int RunCycles(Checks& checks) {
  // The table above against the library it describes. A bump that grew a
  // second exit -- a `return` out of the loop, a second `delete`, or the tail
  // reordered so `exitServices` ran before input was released -- fails here
  // rather than on the sixth open of somebody's console.
  {
    const std::string tesla = CodeOnly(ReadWhole(std::filesystem::path(ROMMSYNC_OVERLAY_SOURCE_DIR) /
                                                 ".." / "lib" / "libultrahand" / "libtesla" /
                                                 "include" / "tesla.hpp"));
    const std::size_t begin = tesla.find("overlay = new TOverlay();");
    const std::size_t end = tesla.find("delete overlay;", begin);
    checks.Expect(begin != std::string::npos && end != std::string::npos,
                  "the pinned tesla.hpp still creates and deletes the overlay in tsl::loop");
    if (begin != std::string::npos && end != std::string::npos) {
      const std::string loop = tesla.substr(begin, end - begin);
      checks.ExpectEq(Count(tesla, "delete overlay;"), std::size_t{1},
                      "the overlay is deleted in exactly one place");
      checks.ExpectEq(Count(loop, "overlay->exitServices();"), std::size_t{1},
                      "and exitServices is called in exactly one, before it");
      checks.ExpectEq(CountWord(loop, "return"), std::size_t{0},
                      "nothing returns out of tsl::loop between creating the overlay and its "
                      "tail, so every exit reaches exitServices");
      const std::size_t release = loop.rfind("hlp::requestForeground(false);");
      const std::size_t screen = loop.rfind("overlay->exitScreen();");
      const std::size_t services = loop.rfind("overlay->exitServices();");
      checks.Expect(release != std::string::npos && release < screen && screen < services,
                    "and the tail releases input and the panel before exitServices, so a "
                    "Shutdown waiting on a stuck call holds neither");
    }
  }

  // And `main.cpp` against the lifecycle: the four hooks go through it, so the
  // host scenario below is the device's teardown and not a copy of it.
  {
    const std::string main = CodeOnly(ReadWhole(std::filesystem::path(ROMMSYNC_OVERLAY_SOURCE_DIR) /
                                                "main.cpp"));
    for (const char* wired : {"lifecycle_.Start()", "lifecycle_.Shown()", "lifecycle_.Stop()"}) {
      checks.Expect(main.find(wired) != std::string::npos,
                    std::string("main.cpp's hooks call ") + wired);
    }
    checks.Expect(main.find("client_.Close()") == std::string::npos,
                  "and none of them closes the session around the lifecycle");
  }

  Console console;
  console.Start();
  const int workers_before = overlay::IpcClient::Workers();
  std::optional<count_new::Snapshot> after_first_round;

  for (int cycle = 0; cycle < kCycles; ++cycle) {
    for (const ExitPath& path : kExitPaths) {
      const std::string where = std::string(path.name) + ", cycle " + std::to_string(cycle);
      ForegroundCalls().clear();
      const int teardowns = overlay::Lifecycle::Teardowns();
      {
        auto under_test = std::make_unique<OverlayUnderTest>();
        under_test->lifecycle.Start();  // initServices
        // **Released before anything else**, on every open: a fresh instance
        // after one that crashed holding input otherwise inherits it, and
        // neither libtesla nor libultrahand gives it back (#209).
        checks.Expect(ForegroundCalls() == std::vector<bool>{false},
                      "the overlay gives input back the moment it starts: " + where);

        overlay::ScreenFrame frame(under_test->client);
        for (int show = 0; show < path.shows; ++show) {
          under_test->lifecycle.Shown();
          checks.Expect(frame.Ready() == overlay::Link::kOk, "a showing can talk: " + where);
          ipc::Status status;
          checks.Expect(R_SUCCEEDED(under_test->client.GetStatus(&status)),
                        "and polls: " + where);
        }
        // One call that ran out of budget and is still out when the overlay
        // closes -- the worst case for a teardown, released a moment later.
        if (path.shows > 0) {
          console.sysmodule().hold.Arm();
          ipc::Status status;
          checks.ExpectEq(under_test->client.GetStatus(&status), overlay::NotResponding(),
                          "a call is left out: " + where);
          std::thread releaser([&console] {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            console.sysmodule().hold.Release();
          });
          if (path.exit_services) {
            under_test->lifecycle.Stop();  // exitServices
          }
          under_test.reset();  // delete overlay
          releaser.join();
        } else {
          if (path.exit_services) {
            under_test->lifecycle.Stop();
          }
          under_test.reset();
        }
      }
      checks.ExpectEq(overlay::Lifecycle::Teardowns(), teardowns + 1,
                      "the one teardown ran exactly once: " + where);
      checks.ExpectEq(hostswitch::OpenSessions(), 0, "no session is left open: " + where);
      checks.ExpectEq(overlay::IpcClient::Workers(), workers_before,
                      "no worker thread is left running: " + where);
      checks.Expect(ForegroundCalls() == std::vector<bool>{false},
                    "and teardown does not touch input -- libtesla's tail releases it: " + where);
    }
    // The heap, once the first round has paid for whatever is allocated once
    // per process. Every round after it must give back exactly what it took.
    if (count_new::kActive) {
      const count_new::Snapshot now = count_new::Now();
      if (!after_first_round.has_value()) {
        after_first_round = now;
      } else {
        checks.ExpectEq(now.outstanding, after_first_round->outstanding,
                        "round " + std::to_string(cycle) +
                            " of open/close leaves as many blocks allocated as round 0 did");
        checks.ExpectEq(now.outstanding_bytes, after_first_round->outstanding_bytes,
                        "...and as many bytes");
      }
    }
  }
  return checks.failures();
}

// --- overlay.hidden -----------------------------------------------------------

/// M9-6 (#209): `update()` does not run while the overlay is hidden.
///
/// `tsl::loop` waits on the combo event with no timeout, so the process stays
/// resident with its gui stack frozen -- and a screen that counted frames to
/// decide when to poll again comes back from a hide mid-count, drawing what it
/// read before the hide as though it were current. `Cadence` is the one clock a
/// screen polls by, and it is due on the first frame of every showing.
int RunHidden(Checks& checks) {
  Console console;
  console.Start();
  overlay::IpcClient client(kTestBudget);
  overlay::Lifecycle lifecycle(client, &RecordForeground);
  lifecycle.Start();
  lifecycle.Shown();

  overlay::Cadence every_thirty(30);
  checks.Expect(every_thirty.Due(), "the first frame polls, rather than drawing nothing");
  int skipped = 0;
  while (!every_thirty.Due() && skipped < 100) {
    ++skipped;
  }
  checks.ExpectEq(skipped, 29, "and then once every thirty frames");

  // Ten frames in, the overlay is hidden and shown again. However long it was
  // hidden, nothing ran in between -- so the count says ten frames and the
  // clock on the wall says anything at all.
  for (int frame = 0; frame < 10; ++frame) {
    checks.Expect(!every_thirty.Due(), "mid-cadence frames do not poll");
  }
  lifecycle.Shown();
  checks.Expect(every_thirty.Due(),
                "the first frame after a showing polls, whatever the count said: a screen "
                "that assumed its polling continued while hidden would draw stale data here");
  checks.Expect(!every_thirty.Due(), "and the cadence starts again from that poll");

  // A cadence of one is every frame, which is what the status screen polls at.
  overlay::Cadence every_frame(1);
  for (int frame = 0; frame < 5; ++frame) {
    checks.Expect(every_frame.Due(), "a cadence of one polls every frame");
  }

  // A press asks for a poll on the next frame rather than waiting out the count.
  for (int frame = 0; frame < 3; ++frame) {
    every_thirty.Due();
  }
  every_thirty.Expire();
  checks.Expect(every_thirty.Due(), "Expire makes the next frame poll");

  // **No screen keeps a frame count of its own.** A member a screen increments
  // or decrements is a clock that stops while the overlay is hidden and
  // resumes as though it had not; `Cadence` is the one that knows about
  // showings. A grep, because none of the screens' `update()`s is reachable
  // from a test.
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(ROMMSYNC_OVERLAY_SOURCE_DIR)) {
    const std::string name = entry.path().filename().string();
    if (name.find("_screen.") == std::string::npos) {
      continue;
    }
    std::ifstream file(entry.path());
    std::string line;
    int number = 0;
    while (std::getline(file, line)) {
      ++number;
      const std::string code = WithoutComment(line);
      for (std::size_t at = code.find("++"); at != std::string::npos; at = code.find("++", at + 2)) {
        const bool member_before = at > 0 && code[at - 1] == '_';
        const std::size_t after = code.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", at + 2);
        const bool member_after = after != std::string::npos && after > at + 2 &&
                                  code[after - 1] == '_';
        checks.Expect(!member_before && !member_after,
                      name + ":" + std::to_string(number) +
                          " counts frames in a member of its own; poll through Cadence, which "
                          "knows the overlay was hidden (lifecycle.hpp)");
      }
      for (std::size_t at = code.find("--"); at != std::string::npos; at = code.find("--", at + 2)) {
        const bool member_before = at > 0 && code[at - 1] == '_';
        const std::size_t after = code.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", at + 2);
        const bool member_after = after != std::string::npos && after > at + 2 &&
                                  code[after - 1] == '_';
        checks.Expect(!member_before && !member_after,
                      name + ":" + std::to_string(number) +
                          " counts frames in a member of its own; poll through Cadence");
      }
    }
  }
  lifecycle.Stop();
  return checks.failures();
}

// --- overlay.portable ---------------------------------------------------------

/// The files that are only host-compilable because they name no libultrahand
/// and no libnx type, and the rule `overlay/AGENTS.md` owes a check for.
///
/// A grep rather than the compile, because the compile only fails for the four
/// files `test_overlay_native` happens to build: a *fifth* screen whose layout
/// went behind a `DrawList` -- which is what that file now tells the next agent
/// to do -- would include `screen_frame.hpp`, and a `tesla.hpp` that had crept
/// back into it would take the new painter out of every test's reach without
/// breaking this target. That is the failure this is for, and the same shape as
/// the greps `overlay.sync_actions` and `conflicts.overlay` already run.
constexpr const char* kPortableFiles[] = {
    "draw_list.hpp",    "prompts.hpp",      "screen_frame.hpp",   "screen_frame.cpp",
    "status_paint.hpp", "status_paint.cpp", "conflicts_pump.hpp", "conflicts_pump.cpp",
    "lifecycle.hpp",    "lifecycle.cpp",    "log_paint.hpp",      "log_paint.cpp",
};

/// What none of them may name. `card_probe` and `ipc_client` are deliberately
/// absent from the list above: the first names `sys/stat.h` and the second is
/// libnx by definition, and both are compiled here behind the shim instead.
constexpr const char* kForbiddenPlatform[] = {
    "tesla.hpp", "tsl::", "libultrahand", "arm_neon",
};

/// `line` with any trailing `//` comment removed.
///
/// Cut rather than skipped whole: these files explain the rules they keep, and
/// explaining a rule is not breaking it -- but a *code* line with a trailing
/// comment (`Palette p;  // the host twin of tsl::Color`) is code, and skipping
/// it whole would let the next token past while failing this one for a comment.
/// A `/* */` block is not handled, and neither is one in the greps next door;
/// nothing in this directory uses them.
std::string WithoutComment(const std::string& line) {
  const std::size_t at = line.find("//");
  return at == std::string::npos ? line : line.substr(0, at);
}

/// Every local header `path` includes, directly. `<angled>` includes are the
/// forbidden tokens' business and are not followed.
std::vector<std::string> LocalIncludes(const std::filesystem::path& path) {
  std::vector<std::string> included;
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    const std::string code = WithoutComment(line);
    const std::size_t hash = code.find("#include \"");
    if (hash == std::string::npos) {
      continue;
    }
    const std::size_t start = code.find('"', hash) + 1;
    const std::size_t end = code.find('"', start);
    if (end == std::string::npos) {
      continue;
    }
    const std::string name = code.substr(start, end - start);
    // Only this directory's own headers. `rommsync/...` is `core/`, which hard
    // rule 4 already holds to the same standard.
    if (name.find('/') == std::string::npos) {
      included.push_back(name);
    }
  }
  return included;
}

int RunPortable(Checks& checks) {
  for (const char* name : kPortableFiles) {
    // **Transitively.** A token scan of these six files alone would pass while
    // one of them grew `#include "palette.hpp"` -- and `palette.hpp` is the
    // tesla-carrying sibling sitting right next to them, so that is the likely
    // way this comes back rather than an unlikely one. The closure is what the
    // compiler would see.
    std::vector<std::string> queue{name};
    std::vector<std::string> seen{name};
    for (std::size_t at = 0; at < queue.size(); ++at) {
      const std::filesystem::path path =
          std::filesystem::path(ROMMSYNC_OVERLAY_SOURCE_DIR) / queue[at];
      std::ifstream file(path);
      checks.Expect(file.good(), std::string("the source is readable: ") + queue[at]);
      std::string line;
      int number = 0;
      while (std::getline(file, line)) {
        ++number;
        const std::string code = WithoutComment(line);
        for (const char* token : kForbiddenPlatform) {
          checks.Expect(code.find(token) == std::string::npos,
                        std::string(queue[at]) + ":" + std::to_string(number) + " names " +
                            token + ", and " + name +
                            " includes it; this half of a screen has to compile on a host "
                            "(overlay/AGENTS.md)");
        }
      }
      for (const std::string& next : LocalIncludes(path)) {
        if (std::find(seen.begin(), seen.end(), next) == seen.end()) {
          seen.push_back(next);
          queue.push_back(next);
        }
      }
    }
  }

  // ...and the other direction: the files above are the ones this suite
  // actually builds, so a rule about them is only as good as the list. Without
  // this, renaming `status_paint.cpp` leaves a check with nothing to check.
  for (const char* name : kPortableFiles) {
    checks.Expect(
        std::filesystem::exists(std::filesystem::path(ROMMSYNC_OVERLAY_SOURCE_DIR) / name),
        std::string(name) + " is still in overlay/source/ -- the rule has a file to be about");
  }

  // Nothing in this directory calls `drawString` except the one function that
  // guards it. libtesla reads `maxWidth = 0` as *no limit* and every screen
  // computes a width that can come out `0` meaning "no room", so a call around
  // `DrawBounded` is that bug back (`palette.hpp`, M9-7 #198). A grep because
  // none of the five `Draw` methods is reachable from any test.
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(ROMMSYNC_OVERLAY_SOURCE_DIR)) {
    const std::filesystem::path path = entry.path();
    if (path.extension() != ".cpp" && path.extension() != ".hpp") {
      continue;
    }
    const std::string name = path.filename().string();
    if (name == "palette.cpp") {
      continue;  // the guard itself
    }
    std::ifstream file(path);
    std::string line;
    int number = 0;
    while (std::getline(file, line)) {
      ++number;
      checks.Expect(WithoutComment(line).find("drawString") == std::string::npos,
                    name + ":" + std::to_string(number) +
                        " calls drawString around DrawBounded; a maxWidth of 0 is "
                        "\"no limit\" (palette.hpp)");
    }
  }
  return checks.failures();
}

// --- overlay.draw -------------------------------------------------------------

/// A `DrawList` that keeps what it was handed. The host half of the seam
/// `overlay/source/draw_list.hpp` exists for.
class Recorder : public overlay::DrawList {
 public:
  std::vector<overlay::DrawCommand> commands;

  void String(const std::string& text, std::int32_t x, std::int32_t y, std::int32_t font_size,
              overlay::Rgba4444 color, std::int32_t wrap_width) override {
    overlay::DrawCommand command;
    command.kind = overlay::DrawCommand::Kind::kString;
    command.text = text;
    command.x = x;
    command.y = y;
    command.width = wrap_width;
    command.font_size = font_size;
    command.color = color;
    commands.push_back(command);
  }

  void Rect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
            overlay::Rgba4444 color) override {
    overlay::DrawCommand command;
    command.kind = overlay::DrawCommand::Kind::kRect;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    command.color = color;
    commands.push_back(command);
  }

  bool Drew(std::string_view text) const {
    for (const overlay::DrawCommand& command : commands) {
      if (command.kind == overlay::DrawCommand::Kind::kString && command.text == text) {
        return true;
      }
    }
    return false;
  }

  std::vector<overlay::DrawCommand> Rects() const {
    std::vector<overlay::DrawCommand> rects;
    for (const overlay::DrawCommand& command : commands) {
      if (command.kind == overlay::DrawCommand::Kind::kRect) {
        rects.push_back(command);
      }
    }
    return rects;
  }
};

/// A palette whose every colour is distinct, so a row drawn in the wrong one is
/// visible rather than accidentally right.
overlay::Palette TestPalette() {
  overlay::Palette palette;
  palette.neutral = 0x1111;
  palette.good = 0x2222;
  palette.warn = 0x3333;
  palette.bad = 0x4444;
  palette.muted = 0x5555;
  palette.track_empty = 0x6666;
  palette.track_full = 0x7777;
  return palette;
}

/// The panel a Tesla overlay hands a `CustomDrawer`. Whole numbers rather than
/// the exact device values, which M8-2 (#44) sets.
constexpr std::int32_t kPanelX = 20;
constexpr std::int32_t kPanelY = 30;
constexpr std::int32_t kPanelWidth = 448;
constexpr std::int32_t kPanelHeight = 400;

int RunDraw(Checks& checks) {
  const overlay::Palette palette = TestPalette();

  // A console mid-download: a headline, rows, and a bar with a fraction.
  overlay::StatusView view;
  view.headline = "Syncing";
  view.tone = overlay::Tone::kGood;
  view.hint = "";
  view.lines.push_back(overlay::Line{"Server", "reachable", overlay::Tone::kGood});
  view.lines.push_back(overlay::Line{"Last sync", "4 minutes ago", overlay::Tone::kNeutral});
  view.lines.push_back(overlay::Line{"Queue", "3 waiting", overlay::Tone::kWarn});
  view.progress.kind = overlay::Progress::Kind::kFraction;
  view.progress.caption = "some-game.gba";
  view.progress.permille = 250;

  {
    Recorder out;
    overlay::PaintStatus(view, palette, out, kPanelX, kPanelY, kPanelWidth, kPanelHeight);

    checks.Expect(out.Drew("Syncing"), "the headline is drawn");
    checks.Expect(out.Drew("Server") && out.Drew("reachable"), "a row is a label and a value");
    checks.Expect(out.Drew(overlay::Prompt(overlay::kGlyphY, "Settings")),
                  "the one control this screen has is drawn");
    checks.Expect(out.Drew("some-game.gba"), "the download's caption is drawn");

    // The headline carries the view's tone and the label beside a value does
    // not: a label is a role, and drawing it in the value's colour is how a
    // warning reads as three warnings.
    for (const overlay::DrawCommand& command : out.commands) {
      if (command.text == "Syncing") {
        checks.ExpectEq(command.color, palette.good, "the headline takes the view's tone");
      }
      if (command.text == "Queue") {
        checks.ExpectEq(command.color, palette.muted, "a label is muted, whatever its row");
      }
      if (command.text == "3 waiting") {
        checks.ExpectEq(command.color, palette.warn, "a value takes its own tone");
      }
    }

    // Two rects and no more: the empty track, then the filled part over it. A
    // fill drawn past its own track is what a server under-declaring a length
    // would otherwise produce (`overlay_status_view.hpp`).
    const std::vector<overlay::DrawCommand> rects = out.Rects();
    checks.ExpectEq(rects.size(), std::size_t{2}, "a fractional bar is a track and a fill");
    if (rects.size() == 2) {
      checks.ExpectEq(rects[0].color, palette.track_empty, "the track is drawn first");
      checks.ExpectEq(rects[1].color, palette.track_full, "and the fill over it");
      checks.ExpectEq(rects[1].y, rects[0].y, "both on the same row");
      checks.ExpectEq(rects[1].height, rects[0].height, "and the same height");
      checks.Expect(rects[1].width <= rects[0].width, "the fill never exceeds its track");
      checks.ExpectEq(rects[1].width, rects[0].width / 4, "250 per mille is a quarter of it");
    }

    // Nothing is drawn past the bounds the drawer handed us, in either
    // direction. A row painted over the frame's chrome reads as a corrupted
    // overlay rather than as a too-long list.
    for (const overlay::DrawCommand& command : out.commands) {
      checks.Expect(command.x >= kPanelX, "nothing is drawn left of the panel");
      checks.Expect(command.y >= kPanelY, "nothing is drawn above it");
      checks.Expect(command.y <= kPanelY + kPanelHeight, "nor below it");
      checks.Expect(command.x + command.width <= kPanelX + kPanelWidth,
                    "nor past its right edge");
    }
  }

  // An indeterminate download -- a server that declared no length (#22). The
  // bar moves without a percentage, so there is a track and nothing in it:
  // synthesising a fraction from `bytes_done` alone is the way this gets got
  // wrong.
  {
    overlay::StatusView indeterminate = view;
    indeterminate.progress.kind = overlay::Progress::Kind::kIndeterminate;
    indeterminate.progress.permille = 0;
    Recorder out;
    overlay::PaintStatus(indeterminate, palette, out, kPanelX, kPanelY, kPanelWidth,
                         kPanelHeight);
    checks.ExpectEq(out.Rects().size(), std::size_t{1},
                    "an indeterminate download draws a track and no fill");
  }

  // No download at all: no bar, and no caption where one would have been.
  {
    overlay::StatusView idle = view;
    idle.progress = overlay::Progress{};
    Recorder out;
    overlay::PaintStatus(idle, palette, out, kPanelX, kPanelY, kPanelWidth, kPanelHeight);
    checks.ExpectEq(out.Rects().size(), std::size_t{0}, "an idle console draws no bar");
    checks.Expect(!out.Drew("some-game.gba"), "and no caption for it");
  }

  // A panel with no room for the rows. The prompt is reserved before anything
  // else, so it is the one thing that survives -- a control nobody can see is a
  // menu this overlay does not have (#26).
  {
    Recorder out;
    overlay::PaintStatus(view, palette, out, kPanelX, kPanelY, kPanelWidth, 90);
    checks.Expect(out.Drew(overlay::Prompt(overlay::kGlyphY, "Settings")),
                  "the settings prompt survives a panel with no room");
    checks.Expect(!out.Drew("3 waiting"),
                  "a row with nowhere to go is dropped rather than drawn over the chrome");
  }

  // A panel too narrow for the value column. Every string carries the width it
  // is to be held to, and **zero means zero**: libtesla reads `maxWidth = 0` as
  // "no limit", so a width that came out zero would draw the one thing the
  // guard computing it exists to prevent -- a 256-byte `fs_name` straight off a
  // RomM library, painted across the console (`draw_list.hpp`).
  {
    Recorder out;
    overlay::PaintStatus(view, palette, out, kPanelX, kPanelY, 120, kPanelHeight);
    for (const overlay::DrawCommand& command : out.commands) {
      if (command.kind != overlay::DrawCommand::Kind::kString) {
        continue;
      }
      checks.Expect(command.width > 0,
                    "a narrow panel asks for no unbounded string: \"" + command.text + "\"");
    }
    // What it does instead: the full-width half of the screen is still drawn,
    // and a row is dropped whole. Half a row is a label with nothing beside it,
    // which reads as a value that failed to load rather than as a narrow panel.
    checks.Expect(out.Drew("Syncing"), "a narrow panel still draws the headline");
    checks.Expect(out.Drew(overlay::Prompt(overlay::kGlyphY, "Settings")),
                  "...and still draws the way into the menu");
    checks.Expect(!out.Drew("Server") && !out.Drew("reachable"),
                  "...and drops a row whole rather than half of it");
  }

  // No panel at all. Nothing is drawn rather than a headline painted across
  // whatever is to the right of it.
  {
    Recorder out;
    overlay::PaintStatus(view, palette, out, kPanelX, kPanelY, 4, kPanelHeight);
    checks.Expect(out.commands.empty(), "a panel with no width draws nothing");
  }
  return checks.failures();
}

// --- overlay.log --------------------------------------------------------------

/// M9-6 (#209): the log the sysmodule serves is readable without an SD reader.
///
/// `GetLog` (M7-3, #38) was dispatched and had no client, so the only way to
/// read why a sync did not happen was to pull the card. This is the screen's
/// two portable halves -- what it says (`RenderLog`, in `core/`) and where it
/// puts it (`PaintLog`) -- the way `overlay.draw` holds the status screen's.
int RunLog(Checks& checks) {
  // What it says.
  {
    const overlay::LogView none = overlay::RenderLog(ipc::LogTail{});
    checks.Expect(!none.headline.empty(), "a console that has logged nothing says so");
    checks.Expect(none.lines.empty(), "and draws no lines");

    ipc::LogTail whole;
    whole.lines = {"1 info boot rommsync-nx/0.1.0", "2 warn net.offline negotiate: failed"};
    whole.total = 2;
    const overlay::LogView all = overlay::RenderLog(whole);
    checks.Expect(all.headline.find('2') != std::string::npos,
                  "the whole log says how many lines it is: " + all.headline);
    checks.ExpectEq(all.lines.size(), std::size_t{2}, "every line is there");
    checks.ExpectEq(all.lines.back(), whole.lines.back(), "oldest first, newest last");

    ipc::LogTail tail = whole;
    tail.total = 412;
    const overlay::LogView end = overlay::RenderLog(tail);
    checks.Expect(end.headline.find("412") != std::string::npos,
                  "a tail says it is the end of a longer log: " + end.headline);
    checks.Expect(end.hint.find(rommsync::log::kLogSdPath) != std::string::npos,
                  "and where the rest of it is: " + end.hint);

    const overlay::LogView gone = overlay::RenderLogUnreachable(overlay::Link::kNotRunning);
    checks.Expect(gone.link == overlay::Link::kNotRunning && !gone.headline.empty(),
                  "a sysmodule that is not there is the same sentence every screen uses");
    checks.Expect(gone.hint.find(rommsync::log::kLogSdPath) != std::string::npos ||
                      !gone.hint.empty(),
                  "with something to do about it");
  }

  // Where it goes.
  const overlay::Palette palette = TestPalette();
  overlay::LogView view;
  view.headline = "The last 40 of 412 lines";
  view.hint = "The rest is on the card";
  for (int line = 0; line < 40; ++line) {
    view.lines.push_back(std::to_string(line) + " info sync.tick a line that is long enough to "
                                                "need bounding on a narrow panel, " +
                         std::string(120, 'x'));
  }
  {
    Recorder out;
    overlay::PaintLog(view, palette, out, kPanelX, kPanelY, kPanelWidth, kPanelHeight);
    checks.Expect(out.Drew(view.headline), "the headline is drawn");
    checks.Expect(out.Drew(overlay::Prompt(overlay::kGlyphB, "Back")), "and the way out");
    // **The end of the log is what fits**, not the start: the newest lines are
    // the ones that say why the last tick failed.
    checks.Expect(out.Drew(view.lines.back()), "the newest line is drawn");
    checks.Expect(!out.Drew(view.lines.front()),
                  "and a panel too short for forty drops the oldest, not the newest");
    std::int32_t newest_y = -1;
    std::int32_t previous_y = -1;
    bool in_order = true;
    for (const overlay::DrawCommand& command : out.commands) {
      if (command.text.find(" info sync.tick ") == std::string::npos) {
        continue;
      }
      in_order = in_order && command.y > previous_y;
      previous_y = command.y;
      if (command.text == view.lines.back()) {
        newest_y = command.y;
      }
    }
    checks.Expect(in_order, "the lines run oldest to newest, down the panel");
    checks.ExpectEq(newest_y, previous_y, "with the newest at the bottom of them");
    for (const overlay::DrawCommand& command : out.commands) {
      checks.Expect(command.width > 0, "no string is drawn unbounded: \"" + command.text + "\"");
      checks.Expect(command.y >= kPanelY && command.y <= kPanelY + kPanelHeight,
                    "nothing is drawn outside the panel");
      checks.Expect(command.x + command.width <= kPanelX + kPanelWidth,
                    "nor past its right edge");
    }
  }
  // No room at all: nothing rather than a headline over whatever is beside it.
  {
    Recorder out;
    overlay::PaintLog(view, palette, out, kPanelX, kPanelY, 4, kPanelHeight);
    checks.Expect(out.commands.empty(), "a panel with no width draws nothing");
  }
  // A panel with room for the prompt and nothing else keeps the prompt.
  {
    Recorder out;
    overlay::PaintLog(view, palette, out, kPanelX, kPanelY, kPanelWidth, 40);
    checks.Expect(out.Drew(overlay::Prompt(overlay::kGlyphB, "Back")),
                  "the way out survives a panel with no room");
  }
  return checks.failures();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "";
  Checks checks;
  if (scenario == "card") {
    return RunCard(checks);
  }
  if (scenario == "wire") {
    return RunWire(checks);
  }
  if (scenario == "roundtrip") {
    return RunRoundtrip(checks);
  }
  if (scenario == "version") {
    return RunVersion(checks);
  }
  if (scenario == "errors") {
    return RunErrors(checks);
  }
  if (scenario == "log") {
    return RunLog(checks);
  }
  if (scenario == "cycles") {
    return RunCycles(checks);
  }
  if (scenario == "hidden") {
    return RunHidden(checks);
  }
  if (scenario == "restore") {
    return RunRestore(checks);
  }
  if (scenario == "timeout") {
    return RunTimeout(checks);
  }
  if (scenario == "portable") {
    return RunPortable(checks);
  }
  if (scenario == "draw") {
    return RunDraw(checks);
  }
  std::cerr << "usage: test_overlay_native "
               "<card|wire|roundtrip|version|errors|timeout|restore|cycles|hidden|log|portable|draw>\n";
  return 2;
}
