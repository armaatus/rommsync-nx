// The overlay's own code, compiled by a host compiler and run (M9-7, #198).
//
// Until this suite existed not one file under `overlay/source/` had been built
// by anything but devkitPro, and `overlay/source/status_screen.hpp` said so:
// *"Nothing here has ever run."* The five `overlay.*` suites next door exercise
// the view models in `core/` -- what a screen *says* -- and the greps in them
// assert what the directory must not contain. Neither reaches the code.
//
// What is compiled in here, and why it is these four files:
//
//   `card_probe.cpp`   the "not installed" / "installed but not set to boot"
//                      diagnosis. Portable already; it needed only a seam for
//                      the `sdmc:` mount (`ProbeCardAt`).
//   `ipc_client.cpp`   the entire client half of the IPC wire, behind the
//                      `switch.h` shim in `tests/hostswitch/`.
//   `screen_frame.cpp` the version handshake and the "not running" /
//                      "unreachable" decision every screen shares.
//   `status_paint.cpp` the status screen's layout, behind the `DrawList` seam.
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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "checks.hpp"
#include "fake_engine.hpp"
#include "scratch.hpp"

#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

// The overlay's own headers, and the sysmodule's half of the error mapping.
// Both reach `switch.h`, which here is `tests/hostswitch/switch.h`.
#include "card_probe.hpp"
#include "draw_list.hpp"
#include "ipc_client.hpp"
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
/// The knobs are the three things a *healthy* sysmodule cannot be asked to do
/// on demand and that the overlay has code for anyway -- answer a payload from
/// another build, answer one that will not parse, and claim a length that is
/// not the one it wrote. Everything else goes through `ipc::Dispatch`.
class HostSysmodule : public hostswitch::Server {
 public:
  explicit HostSysmodule(ipc::ServiceCore& core) : core_(core) {}

  /// Answered instead of dispatching, with a successful `Result`. A sysmodule
  /// from another release, or one whose reply was cut.
  std::optional<std::string> canned;

  /// The reply's length word, when it is to disagree with what was written.
  std::optional<std::uint64_t> claim_length;

  int calls = 0;
  std::uint32_t last_command = 0;
  std::string last_request;

  Result Handle(u32 command_id, const void* request, std::size_t request_size, void* response,
                std::size_t capacity, u64* length) override {
    ++calls;
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
       rommsync::conflicts::RestoreReport out;
       return c.RestoreBackup(3, &out);
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
  // able to send it. `kGetLog` is the one hole and it is a known one: the
  // sysmodule dispatches it and no screen calls it (#209, which decides whether
  // to wire it or drop it). Pinned rather than left implicit, so a *new*
  // command with no client is a red test rather than a discovery on a console.
  for (const ipc::Command command : ipc::kAllCommands) {
    bool covered = false;
    for (const ClientCall& call : kClientCalls) {
      covered = covered || call.command == command;
    }
    if (command == ipc::Command::kGetLog) {
      checks.Expect(!covered, "kGetLog still has no client -- #209 decides its fate");
      continue;
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
    checks.ExpectEq(console.sysmodule().calls, before,
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
  rommsync::conflicts::RestoreReport report;
  checks.Expect(R_SUCCEEDED(client.RestoreBackup(3, &report)), "RestoreBackup succeeds");
  checks.Expect(report.ok(), "the restore outcome survives");
  checks.ExpectEq(report.message, engine.restore_report.message, "...and its message");
  checks.ExpectEq(report.backup_sd_path, engine.restore_report.backup_sd_path,
                  "...and the backup this restore itself wrote first (hard rule 2)");
  checks.ExpectEq(engine.last_restore_id, static_cast<std::int64_t>(3),
                  "against the entry id the overlay named");
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
  {
    overlay::IpcClient orphan;
    overlay::ScreenFrame orphan_frame(orphan);
    console.Stop();
    checks.Expect(orphan_frame.Ready() == overlay::Link::kNotRunning,
                  "no rommsync port is kNotRunning");
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
  if (scenario == "draw") {
    return RunDraw(checks);
  }
  std::cerr << "usage: test_overlay_native "
               "<card|wire|roundtrip|version|errors|draw>\n";
  return 2;
}
