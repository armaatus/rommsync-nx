// `sysmodule::SdEngine` — the `ipc::Engine` the console's service is built on,
// driven on the host.
//
// The file is Horizon-*side* and not Horizon-*specific*: it names no libnx type
// (hard rule 4 applies to `core/`, but this obeys it too), so the only thing
// that ever tied it to a console was the `sdmc:` prefix on its config
// directory. `SdEngine::Load` takes that directory, which is what lets the
// whole engine run against a `harness::Sandbox` here.
//
// M3-2 (#19) is the first issue to put real behaviour behind one of these
// commands, and the reason this file exists: `Enqueue` and `Dequeue` change a
// file on the card, so "it compiles" is no longer a description of what they
// do. M5-3 (#30) is the second: `SetConfig` and `SetEnabled` now write
// `config.ini` and re-read it, so a setting changed from the overlay is in force
// before the call returns. What is left in `SdEngine` is still `kUnavailable`,
// and `commands` pins that too — each of #31 and M7-2 replaces its own part, and
// the last `kUnavailable` to go is what says the engine is finished
// (`engine.hpp`).
//
// Driven through `ipc::Dispatch` rather than by calling the methods, because
// the dispatch table is what the console actually runs.
//
//   queue     -- enqueue, dequeue, and the depth the status screen reads
//   persists  -- every change reaches queue.json, and a fresh engine reads it back
//   rollback  -- a write that cannot happen changes neither the file nor memory
//   corrupt   -- a queue.json a yanked card left behind never blocks the boot
//   config    -- M5-3: an edit lands on the card and on the running engine
//   unpairs   -- M5-3: the pairing goes before the write, even when it fails
//   stale     -- M5-3: an unreadable boot does not make the next edit un-pair
//   server    -- M5-3: changing the server discards the token it does not own
//   unauthenticated -- M1-4: the verdict `auth.json` carries, and the re-pair
//                      that lifts it
//   commands  -- what is still `kUnavailable`, so the list shrinks deliberately
//   relaunch  -- M6-2: two engines over one card at once, and no lock file
//   reenable  -- M6-2: the enable switch off and back on, with nothing rebooted
//   unreachable -- M1-6: a server that answers nothing refuses the attempt and
//                  leaves the card alone
//   pairs     -- M1-6: a pairing attempt, start to committed token
//   nonblocking -- M1-6: `StartPair` returns while the init is still in flight
//   repairs   -- M1-6: `StartPair` then `Unpair`, with neither instant losing
//                both the old pairing and the new attempt
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "engine.hpp"
#include "harness.hpp"
#include "rig.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/download.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/token_store.hpp"

namespace auth = rommsync::auth;
namespace config = rommsync::config;
namespace download = rommsync::download;
namespace http = rommsync::http;
namespace ipc = rommsync::ipc;
namespace sysmodule = rommsync::sysmodule;

namespace {

/// An engine pointed at a sandbox, and the service in front of it.
///
/// The trailing `/` matters: `SdEngine::PathTo` joins by concatenation, exactly
/// as it does with `sdmc:/config/rommsync/`.
class Console {
 public:
  Console(checks::Checks& checks, std::string_view name) : sandbox(checks, name) {
    directory = sandbox.Host(harness::kConfigDir) + "/";
  }

  /// A second console over a card another one wrote -- a reboot. It keeps its
  /// own sandbox so the teardown audit still has one, and never looks at it.
  Console(checks::Checks& checks, std::string_view name, std::string existing)
      : sandbox(checks, name), directory(std::move(existing)) {}

  void Boot() {
    engine.Load(directory);
    core = std::make_unique<ipc::ServiceCore>(engine);
  }

  /// One command, through the table the console runs.
  ipc::Error Call(ipc::Command command, const std::string& request, std::string* response) {
    return ipc::Dispatch(*core, static_cast<std::uint32_t>(command), request, response);
  }

  ipc::Error Enqueue(std::int64_t rom_id, std::int32_t* position) {
    std::string response;
    const ipc::Error answer = Call(ipc::Command::kEnqueue, ipc::EncodeRomId(rom_id), &response);
    if (answer == ipc::Error::kOk) {
      *position = ipc::DecodeQueuePosition(response).value;
    }
    return answer;
  }

  ipc::Error Dequeue(std::int64_t rom_id) {
    std::string response;
    return Call(ipc::Command::kDequeue, ipc::EncodeRomId(rom_id), &response);
  }

  ipc::ConfigResult Set(const ipc::ConfigEdit& edit) {
    std::string response;
    Call(ipc::Command::kSetConfig, ipc::EncodeConfigEdit(edit), &response);
    return ipc::DecodeConfigResult(response).value;
  }

  ipc::EnabledResult SetEnabled(bool enabled) {
    std::string response;
    Call(ipc::Command::kSetEnabled, ipc::EncodeEnabled(enabled), &response);
    return ipc::DecodeEnabledResult(response).value;
  }

  ipc::SyncOutcome SyncNow() {
    std::string response;
    Call(ipc::Command::kSyncNow, ipc::EncodeEmpty(), &response);
    return ipc::DecodeSyncOutcome(response).value;
  }

  /// Command 6, and the status it answers with -- the attempt as it stands one
  /// instant later, which is what the overlay draws while the init is in flight.
  ipc::Error StartPair(auth::PairingStatus* status) {
    std::string response;
    const ipc::Error answer = Call(ipc::Command::kStartPair, ipc::EncodeEmpty(), &response);
    if (answer == ipc::Error::kOk) {
      *status = auth::ParsePairingStatus(response).value;
    }
    return answer;
  }

  /// Command 7, read the way the overlay reads it: off the wire, never off the
  /// engine's own object.
  auth::PairingStatus PairState() {
    std::string response;
    Call(ipc::Command::kGetPairState, ipc::EncodeEmpty(), &response);
    return auth::ParsePairingStatus(response).value;
  }

  ipc::ConfigView Configured() {
    std::string response;
    Call(ipc::Command::kGetConfig, ipc::EncodeEmpty(), &response);
    return ipc::DecodeConfigView(response).value;
  }

  ipc::Status Status() {
    std::string response;
    ipc::Dispatch(*core, static_cast<std::uint32_t>(ipc::Command::kGetStatus),
                  ipc::EncodeEmpty(), &response);
    return ipc::DecodeStatus(response).value;
  }

  /// The queue as the *card* holds it, which is the only copy that survives a
  /// reboot and the only one worth asserting on.
  download::LoadedQueue OnCard() const {
    return download::LoadQueue(directory + download::kQueueFileName);
  }

  /// The queue as the overlay reads it: one page of the `queue` list (M5-4).
  ///
  /// One page is enough for every caller here -- these queues are a handful of
  /// rows -- and `lists.queue` is what holds the paging itself.
  std::vector<ipc::ListItem> QueueList() {
    ipc::ListRequest request;
    request.kind = ipc::ListKind::kQueue;
    std::string response;
    if (Call(ipc::Command::kListBegin, ipc::EncodeListRequest(request), &response) !=
        ipc::Error::kOk) {
      return {};
    }
    const ipc::Cursor cursor = ipc::DecodeCursor(response).value;
    if (Call(ipc::Command::kListNext, ipc::EncodeCursor(cursor), &response) != ipc::Error::kOk) {
      return {};
    }
    const ipc::ListPage page = ipc::DecodeListPage(response).value;
    Call(ipc::Command::kListEnd, ipc::EncodeCursor(cursor), &response);
    return page.items;
  }

  harness::Sandbox sandbox;
  std::string directory;
  sysmodule::SdEngine engine;
  std::unique_ptr<ipc::ServiceCore> core;
};

// --- the commands M3-2 makes real ---------------------------------------------

void Queue(checks::Checks& c) {
  Console console(c, "engine-queue");
  console.Boot();

  c.ExpectEq(console.Status().queue_depth, std::int64_t{0}, "a console queues nothing at boot");
  c.Expect(console.Status().download.state == ipc::DownloadState::kIdle,
           "and its worker is idle");

  std::int32_t position = 0;
  c.Expect(console.Enqueue(4, &position) == ipc::Error::kOk,
           "Enqueue is answered rather than refused as unavailable -- which is the whole "
           "signal that M3-2 replaced M4-1's placeholder");
  c.ExpectEq(position, 1, "and says where in the queue it went, 1-based");
  c.ExpectEq(console.Status().queue_depth, std::int64_t{1}, "the status screen shows it");
  c.Expect(console.Status().download.state == ipc::DownloadState::kQueued,
           "and reports something waiting rather than an idle worker");

  c.Expect(console.Enqueue(5, &position) == ipc::Error::kOk, "a second rom is queued");
  c.ExpectEq(position, 2, "behind the first");
  c.Expect(console.Enqueue(4, &position) == ipc::Error::kDuplicate,
           "and asking for one already waiting is one entry, not two");
  c.ExpectEq(console.Status().queue_depth, std::int64_t{2}, "so the depth does not move");

  c.Expect(console.Dequeue(4) == ipc::Error::kOk, "an entry is taken out");
  c.ExpectEq(console.Status().queue_depth, std::int64_t{1}, "leaving the other");
  c.Expect(console.Dequeue(4) == ipc::Error::kNotQueued, "and taking it out again says so");
  c.Expect(console.Dequeue(999'999) == ipc::Error::kNotQueued,
           "as does one that was never there");

  // The queue is on the card, so nothing here needed the network -- which is
  // the rule `ipc.hpp` states for every command, and the reason `kUnknownRom`
  // cannot be answered until the engine holds a library.
  c.Expect(console.Enqueue(999'999, &position) == ipc::Error::kOk,
           "an id no rom has is queued rather than refused, because this build fetches no "
           "library to refuse it from; the worker settles it with a reason");
}

void Persists(checks::Checks& c) {
  Console console(c, "engine-persists");
  console.Boot();

  std::int32_t position = 0;
  console.Enqueue(4, &position);
  console.Enqueue(5, &position);

  // Every command that changes the queue writes the file, so a reboot between
  // any two of them loses nothing.
  const download::LoadedQueue after_enqueue = console.OnCard();
  c.Expect(after_enqueue.diagnostics.empty(), "the file reads back clean");
  c.ExpectEq(after_enqueue.entries.size(), std::size_t{2}, "with both entries on the card");
  c.Expect(!console.sandbox.Exists("/config/rommsync/queue.json.tmp"), "no .tmp beside it");
  c.Expect(!console.sandbox.Exists("/config/rommsync/queue.json.old"), "and no .old");

  console.Dequeue(4);
  c.ExpectEq(console.OnCard().entries.size(), std::size_t{1},
             "a dequeue reaches the card too, not just memory");

  // The reboot itself: a second engine over the same directory.
  Console rebooted(c, "engine-persists-2", console.directory);
  rebooted.Boot();
  c.ExpectEq(rebooted.Status().queue_depth, std::int64_t{1},
             "a console that restarts still owes the download it was asked for");
}

void Rollback(checks::Checks& c) {
  Console console(c, "engine-rollback");
  console.Boot();

  std::int32_t position = 0;
  console.Enqueue(4, &position);
  std::string queued = console.sandbox.Read("/config/rommsync/queue.json");
  c.Expect(!queued.empty(), "there is a queue on the card to protect");

  // Take the directory away, so the write cannot happen. `ipc::Error::kWriteFailed`
  // promises the *in-memory* state is unchanged too, so a caller that retries is
  // not fighting a half-applied edit -- and this is the only test of that.
  std::error_code error;
  std::filesystem::remove_all(console.sandbox.Host(harness::kConfigDir), error);

  c.Expect(console.Enqueue(5, &position) == ipc::Error::kWriteFailed,
           "an enqueue that cannot be written is a named failure");
  c.ExpectEq(console.Status().queue_depth, std::int64_t{1},
             "and the rom that could not be recorded is not in the queue either");

  c.Expect(console.Dequeue(4) == ipc::Error::kWriteFailed,
           "a dequeue that cannot be written is the same");
  c.ExpectEq(console.Status().queue_depth, std::int64_t{1},
             "and the entry it would have removed is still there -- so retrying after the card "
             "comes back removes it once rather than finding it already gone");

  // A `queue.json` that is *there* and will not open is not a queue to discard:
  // the card had a bad moment and the pending downloads are probably intact.
  // Writing an empty queue over them would turn "empty for this boot" into gone
  // for good, so a command that would write refuses instead.
  console.sandbox.MakeDirs(harness::kConfigDir);
  c.Expect(console.sandbox.Write("/config/rommsync/queue.json", queued),
           "the queue is back on the card");
  Console unreadable(c, "engine-unreadable", console.directory);
  // A directory where the file should be: `fopen` fails with EISDIR, which is
  // `kUnreadable` and not `kMissing` -- the distinction the whole branch rests
  // on (`atomic_file.hpp`).
  std::filesystem::remove(console.sandbox.Host("/config/rommsync/queue.json"), error);
  console.sandbox.MakeDirs("/config/rommsync/queue.json");
  unreadable.Boot();
  c.ExpectEq(unreadable.Status().queue_depth, std::int64_t{0},
             "the console boots with no queue it can see");
  c.Expect(unreadable.Enqueue(7, &position) == ipc::Error::kWriteFailed,
           "and refuses to write one over a queue it could not read");
  std::filesystem::remove(console.sandbox.Host("/config/rommsync/queue.json"), error);

  // Put the directory back and check the retry lands.
  console.sandbox.MakeDirs(harness::kConfigDir);
  c.Expect(console.Dequeue(4) == ipc::Error::kOk, "the retry works once the card is writable");
  c.ExpectEq(console.Status().queue_depth, std::int64_t{0}, "and it takes effect");
  c.ExpectEq(console.OnCard().entries.size(), std::size_t{0}, "on the card as well");
}

void Corrupt(checks::Checks& c) {
  Console console(c, "engine-corrupt");

  // What a yanked SD card leaves behind. Nothing may block boot (CLAUDE.md), so
  // this has to be an empty queue and a diagnostic rather than a refusal.
  c.Expect(console.sandbox.Write("/config/rommsync/queue.json", "\x01\x02 not json at all"),
           "a corrupt queue is on the card");
  console.Boot();

  c.ExpectEq(console.Status().queue_depth, std::int64_t{0}, "the console boots with no queue");
  c.Expect(console.Status().configured == false,
           "...and the rest of the status is still answerable");

  // The complaint is not dropped on the floor: a queue that vanished with
  // nothing anywhere saying why is the failure a diagnostic exists to prevent.
  //
  // It arrives on the **queue list** since M5-4 (#31), not on
  // `config_diagnostics()`. #22 put it under a `[downloads]` section on a report
  // about `config.ini` because the settings screen was the only screen that
  // existed; the download screen is where a user goes to ask why a rom did not
  // arrive, and that is where this now is. `lists.queue` pins the row itself;
  // what this pins is that the sentence still reaches the overlay from *this*
  // engine, and that it no longer arrives twice.
  std::string response;
  console.Call(ipc::Command::kGetConfig, ipc::EncodeEmpty(), &response);
  const ipc::ConfigView view = ipc::DecodeConfigView(response).value;
  for (const config::Diagnostic& diagnostic : view.diagnostics) {
    c.Expect(diagnostic.section != "downloads",
             "the `[downloads]` placeholder is gone from the config report");
  }
  c.Expect(!console.QueueList().empty(), "and the overlay is told the queue was discarded");

  // And it still works: a console whose queue was thrown away can queue again.
  std::int32_t position = 0;
  c.Expect(console.Enqueue(4, &position) == ipc::Error::kOk, "the queue is usable again");
  c.ExpectEq(console.OnCard().entries.size(), std::size_t{1}, "and the card holds one entry");

  // A `config.ini` full of its own complaints must not push the queue's out of
  // the payload. `ipc::TrimDiagnostics` keeps the first few and summarises the
  // rest, and "your whole download queue was discarded" is the one line a user
  // cannot infer from anything else on the screen -- so it goes in front.
  //
  // The trim is what makes the two channels worth keeping apart.
  // `ipc::TrimDiagnostics` keeps the first few complaints and summarises the
  // rest, so on a `config.ini` full of its own the queue's used to have to be
  // pushed in front of them to survive at all. On a list of its own it cannot be
  // summarised away by anything.
  Console noisy(c, "engine-corrupt-noisy");
  std::string bad("[server]\nurl = https://romm.example.com\n[sync]\n");
  for (int line = 0; line < 12; ++line) {
    bad += "enabled = perhaps\n";
  }
  c.Expect(noisy.sandbox.Write("/config/rommsync/config.ini", bad), "a noisy config.ini");
  c.Expect(noisy.sandbox.Write("/config/rommsync/queue.json", "not json"), "beside a bad queue");
  noisy.Boot();
  noisy.Call(ipc::Command::kGetConfig, ipc::EncodeEmpty(), &response);
  const ipc::ConfigView crowded = ipc::DecodeConfigView(response).value;
  c.Expect(crowded.diagnostics.size() > 1, "the config's own complaints are there too");
  c.Expect(!noisy.QueueList().empty(),
           "and the queue's complaint is on the queue list, where nothing can trim it");

  // And a card that has simply never queued anything says nothing at all: a
  // first boot produces a diagnostic saying there is no file yet, and a row
  // reading "queue.json is missing" on every new console is noise in the one
  // place a user goes to find out why a rom did not arrive (`download.hpp`).
  Console fresh(c, "engine-corrupt-fresh");
  fresh.Boot();
  c.ExpectEq(fresh.QueueList().size(), std::size_t{0},
             "a console that has never queued anything has an empty queue list");
}

// --- the commands M5-3 makes real ---------------------------------------------

/// One assignment, the shape the settings screen sends.
ipc::ConfigEdit Edit(std::string section, std::string key, std::string value) {
  ipc::ConfigEdit edit;
  edit.assignments.push_back({std::move(section), std::move(key), std::move(value), false});
  return edit;
}

void ConfigWrites(checks::Checks& c) {
  Console console(c, "engine-config");
  const std::string original =
      "; the box in the cupboard\n"
      "[server]\n"
      "url = https://romm.example.com\n"
      "\n"
      "[sync]\n"
      "enabled      = true\n"
      "interval_min = 30\n";
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini", original),
           "a config.ini is on the card");
  console.Boot();
  c.ExpectEq(console.Configured().config.sync.interval_min, 30, "and the engine is running on it");

  // The acceptance this issue exists for: an edit over IPC takes effect on the
  // *running* engine, with nothing rebooted in between. `GetConfig` is answered
  // from `config_`, so reading 45 out of it is reading it out of the live
  // configuration and not off the card.
  const ipc::ConfigResult applied = console.Set(Edit("sync", "interval_min", "45"));
  c.Expect(applied.outcome == ipc::WriteOutcome::kApplied, "the edit was applied");
  c.ExpectEq(console.Configured().config.sync.interval_min, 45,
             "and the running engine is using it, with no restart");

  // ...and it reached the card, with the rest of the file exactly as the user
  // left it. This is the whole reason `ApplyEdit` edits text.
  const std::string on_card = console.sandbox.Read("/config/rommsync/config.ini");
  c.ExpectEq(on_card, std::string("; the box in the cupboard\n"
                                  "[server]\n"
                                  "url = https://romm.example.com\n"
                                  "\n"
                                  "[sync]\n"
                                  "enabled      = true\n"
                                  "interval_min = 45\n"),
             "the card holds the edit and every other byte the user wrote");
  c.Expect(!console.sandbox.Exists("/config/rommsync/config.ini.tmp"), "no .tmp is left behind");
  c.Expect(!console.sandbox.Exists("/config/rommsync/config.ini.old"), "and no .old");

  // `SetEnabled` is the same path, and answers with the state read back rather
  // than the one it was asked for (#24).
  const ipc::EnabledResult off = console.SetEnabled(false);
  c.Expect(off.outcome == ipc::WriteOutcome::kApplied, "the switch was written");
  c.Expect(!off.enabled, "and answers with the state that took");
  c.Expect(!console.Status().enabled, "which is what the status screen draws");
  c.Expect(console.sandbox.Read("/config/rommsync/config.ini")
                   .find("enabled      = false") != std::string::npos,
           "the alignment of the line it rewrote is the user's own");

  // A refused edit is a successful call carrying the refusal, and the card is
  // untouched -- the two halves of `kInvalid`.
  const std::string before = console.sandbox.Read("/config/rommsync/config.ini");
  const ipc::ConfigResult refused = console.Set(Edit("sync", "interval_min", "-5"));
  c.Expect(refused.outcome == ipc::WriteOutcome::kInvalid, "a negative interval is refused");
  c.Expect(!refused.diagnostics.empty(), "with a diagnostic saying so");
  c.ExpectEq(console.sandbox.Read("/config/rommsync/config.ini"), before,
             "and config.ini is byte-identical");
  c.ExpectEq(console.Configured().config.sync.interval_min, 45, "as is the running engine");

  // A `config.ini` this console cannot read is not one to replace: the user's
  // settings are probably still in it. A directory where the file should be
  // gives `fopen` EISDIR, which is `kUnreadable` rather than `kMissing`.
  std::error_code error;
  std::filesystem::remove(console.sandbox.Host("/config/rommsync/config.ini"), error);
  console.sandbox.MakeDirs("/config/rommsync/config.ini");
  const ipc::ConfigResult unreadable = console.Set(Edit("sync", "saves", "false"));
  c.Expect(unreadable.outcome == ipc::WriteOutcome::kWriteFailed,
           "an unreadable config.ini is a write that did not happen");
  c.Expect(!unreadable.diagnostics.empty(), "and says which file");
  std::filesystem::remove(console.sandbox.Host("/config/rommsync/config.ini"), error);
}

// --- M6-2 (#33): the two switches, and the kill between them ------------------

/// ovl-sysmodules' "restart" is `pmshellTerminateProgram` followed immediately
/// by `pmshellLaunchProgram` -- terminate then launch, with no gap. So a second
/// engine can be constructed over a card the first is still holding open, and
/// neither may refuse to start.
///
/// The failure this rules out is the ordinary one: a lock file or a pidfile
/// written at boot and removed at shutdown. There is no shutdown here, so the
/// first one written would be the last toggle this sysmodule ever survives.
void Relaunch(checks::Checks& c) {
  Console first(c, "engine-relaunch");
  const std::string original = "[server]\nurl = https://romm.example.com\n";
  c.Expect(first.sandbox.Write("/config/rommsync/config.ini", original), "a configured card");
  first.Boot();

  std::int32_t position = 0;
  c.Expect(first.Enqueue(4, &position) == ipc::Error::kOk, "with a download owed");
  c.ExpectEq(first.OnCard().entries.size(), std::size_t{1}, "and it is on the card");

  // The kill. `first` is not destroyed: a terminate-then-launch with no gap can
  // have the second process reading the card while the first is still unloading,
  // which is the case a lock file gets wrong and a sequential test never sees.
  Console second(c, "engine-relaunch-2", first.directory);
  second.Boot();
  c.ExpectEq(second.Status().queue_depth, std::int64_t{1},
             "the relaunched engine starts, and owes the same download");
  c.ExpectEq(second.Configured().config.server.url, std::string("https://romm.example.com"),
             "reading the same configuration");

  // Nothing in the client's own directory looks like a lock, a pidfile, or the
  // debris of an interrupted write. `.tmp` and `.old` are the two `io::WriteAtomically`
  // stages, and both being absent is what says the writes committed rather than
  // that nobody has looked.
  std::error_code error;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(first.sandbox.Host(harness::kConfigDir), error)) {
    const std::string name = entry.path().filename().string();
    c.Expect(name.find("lock") == std::string::npos && name.find("pid") == std::string::npos,
             "no lock or pidfile in the client's directory: " + name);
    c.Expect(name.size() < 4 || name.substr(name.size() - 4) != ".tmp",
             "and no half-written record: " + name);
  }

  // Both are live, and the card does not end up holding half of each. The
  // second process is the one that owns the console now, so its write is the
  // one that has to land intact.
  c.Expect(second.Enqueue(5, &position) == ipc::Error::kOk, "the relaunched engine writes");
  const download::LoadedQueue after = second.OnCard();
  c.Expect(after.trusted, "and queue.json still reads back");
  c.Expect(after.diagnostics.empty(), "with no complaint about its contents");
  c.ExpectEq(after.entries.size(), std::size_t{2}, "holding both entries");

  // A third boot over the same card, after the two overlapping ones: the file is
  // a queue and not a fragment of two.
  Console third(c, "engine-relaunch-3", first.directory);
  third.Boot();
  c.ExpectEq(third.Status().queue_depth, std::int64_t{2},
             "and a third relaunch reads exactly what the second wrote");
}

/// The enable switch, on the running process. `[sync] enabled` is not the boot
/// toggle: with it off the process is resident and IPC answers, and what stops
/// is syncing (docs/DEVELOPMENT.md#the-two-switches).
///
/// The acceptance is that turning it back on needs no reboot -- so every
/// assertion here is against the *same* `Console`, and the card is checked after
/// rather than reloaded to produce the answer.
void Reenable(checks::Checks& c) {
  Console console(c, "engine-reenable");
  const std::string original =
      "[server]\n"
      "url = https://romm.example.com\n"
      "\n"
      "[sync]\n"
      "enabled = true\n";
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini", original), "a configured card");

  auth::StoredToken token;
  token.server_url = "https://romm.example.com";
  token.access_token = "not-a-real-token";
  token.device_id = "console-reenable";
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
           "and a paired one, so the switch is the only thing left to refuse a sync");
  console.Boot();
  c.Expect(console.Status().enabled, "sync is on to begin with");
  c.Expect(console.SyncNow() != ipc::SyncOutcome::kDisabled,
           "so an on-demand sync is not refused for being switched off");

  const ipc::EnabledResult off = console.SetEnabled(false);
  c.Expect(off.outcome == ipc::WriteOutcome::kApplied, "the switch goes off");
  c.Expect(!off.enabled, "and answers with the state that took");
  c.Expect(!console.Configured().config.sync.enabled,
           "the running engine is the one that changed, not just the file");
  c.Expect(console.sandbox.Read("/config/rommsync/config.ini").find("enabled = false") !=
               std::string::npos,
           "and the card agrees");

  // The refusal, which is the whole of what "disabled" costs a user who asks
  // for a sync anyway: a sentence naming the switch, not a spinner that never
  // moves (`ipc.hpp`, and #35 corrected this issue's own body to match).
  c.Expect(console.SyncNow() == ipc::SyncOutcome::kDisabled,
           "and SyncNow is refused for the one reason the user can fix");

  // Back on, in the same process. Nothing was rebooted and nothing re-read the
  // card to get here.
  const ipc::EnabledResult on = console.SetEnabled(true);
  c.Expect(on.outcome == ipc::WriteOutcome::kApplied, "the switch goes back on");
  c.Expect(on.enabled, "answering with the state that took");
  c.Expect(console.Status().enabled, "the status screen draws it immediately");
  c.Expect(console.Configured().config.sync.enabled, "and the live configuration carries it");
  c.Expect(console.sandbox.Read("/config/rommsync/config.ini").find("enabled = true") !=
               std::string::npos,
           "with the card written once more");

  // `SyncNow` stops answering `kDisabled` the moment the switch is back on, with
  // no restart in between -- which is this issue's criterion. It does not yet
  // answer `kAccepted`: `SdEngine::RequestSync` returns false until there is a
  // scheduler to start a tick, and `ServiceCore` reports that as
  // `kAlreadyRunning` (M7-2, #37, "What M4-1 left here"). The switch is what
  // this issue owns; the tick behind it is #37's.
  c.Expect(console.SyncNow() != ipc::SyncOutcome::kDisabled,
           "and a sync asked for now is refused by something other than the switch");

  // The card and the engine still agree after a real reboot, so nothing above
  // was true only in memory.
  Console rebooted(c, "engine-reenable-2", console.directory);
  rebooted.Boot();
  c.Expect(rebooted.Configured().config.sync.enabled,
           "a console that reboots comes back with the switch where the user left it");
}

void UrlWriteFails(checks::Checks& c) {
  Console console(c, "engine-url-fails");
  const std::string settings = "[server]\nurl = https://romm.example.com\n";
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini", settings),
           "a configured console");

  auth::StoredToken token;
  token.server_url = "https://romm.example.com";
  token.access_token = "not-a-real-token";
  token.device_id = "console-1";
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(), "and a pairing");
  console.Boot();
  c.Expect(console.Status().auth == ipc::AuthState::kPaired, "which it reports");

  // The token is discarded *before* the write, so that the card never names one
  // server while holding another's credential. The cost of that order is this
  // case, and it is the branch a user actually meets: the write fails, the
  // server did not change, and the pairing is gone anyway. It has to be said
  // out loud rather than left to be discovered at the pairing screen.
  //
  // A directory where `io::WriteAtomically` wants to stage its temp file makes
  // the write fail with the config directory still readable and writable, so
  // the discard succeeds and the write does not -- which is exactly the order
  // under test.
  console.sandbox.MakeDirs("/config/rommsync/config.ini.tmp");
  const ipc::ConfigResult failed =
      console.Set(Edit("server", "url", "https://elsewhere.example.com"));
  c.Expect(failed.outcome == ipc::WriteOutcome::kWriteFailed, "the write did not happen");
  c.ExpectEq(console.sandbox.Read("/config/rommsync/config.ini"), settings,
             "and config.ini is byte-identical");
  c.ExpectEq(console.Configured().config.server.url, std::string("https://romm.example.com"),
             "so the console is still pointed at the server it was");
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat"),
           "the pairing is gone, because it was discarded first on purpose");
  c.Expect(console.Status().auth == ipc::AuthState::kNeverPaired, "and the overlay is told");
  bool warned = false;
  for (const config::Diagnostic& diagnostic : failed.diagnostics) {
    warned = warned || diagnostic.message.find("paired again") != std::string::npos;
  }
  c.Expect(warned, "with a sentence saying the pairing went even though the server did not");

  std::error_code error;
  std::filesystem::remove(console.sandbox.Host("/config/rommsync/config.ini.tmp"), error);
}

void StaleConfig(checks::Checks& c) {
  Console console(c, "engine-stale");

  // A `config.ini` that is *there* and will not read. `LoadConfig` may never
  // refuse (nothing blocks boot), so the engine comes up on the built-in
  // defaults -- which have no `server.url` -- while the card names a perfectly
  // good server.
  console.sandbox.MakeDirs("/config/rommsync/config.ini");
  auth::StoredToken token;
  token.server_url = "https://romm.example.com";
  token.access_token = "not-a-real-token";
  token.device_id = "console-1";
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(), "a pairing");
  console.Boot();
  c.Expect(!console.Configured().config.configured(),
           "the engine boots unconfigured over a config.ini it could not read");
  c.Expect(console.Status().auth == ipc::AuthState::kPaired, "and still knows it is paired");

  std::error_code error;
  std::filesystem::remove(console.sandbox.Host("/config/rommsync/config.ini"), error);
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\nurl = https://romm.example.com\n[sync]\n"
                                 "enabled = true\n"),
           "the card comes back with the settings it always had");

  // Whether the server changed is a question about the *card*, not about what
  // this process managed to load at boot. Asking `config_` would make this
  // toggle -- which names no server at all -- look like a server change and
  // shred a working pairing over one bad moment from an SD card.
  const ipc::EnabledResult off = console.SetEnabled(false);
  c.Expect(off.outcome == ipc::WriteOutcome::kApplied, "an unrelated edit is applied");
  c.Expect(console.sandbox.Exists("/config/rommsync/token.dat"),
           "and the pairing is left where it is");
  c.Expect(console.Status().auth == ipc::AuthState::kPaired, "the console is still paired");
  c.ExpectEq(console.Configured().config.server.url, std::string("https://romm.example.com"),
             "and the engine has picked the real settings up");
}

void InterruptedCommit(checks::Checks& c) {
  Console console(c, "engine-commit");
  const std::string settings =
      "[server]\n"
      "url = https://romm.example.com\n"
      "[sync]\n"
      "interval_min = 90\n";
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini", settings),
           "a config.ini is on the card");

  // `io::WriteAtomically` commits with two renames, because Horizon's rename
  // refuses a destination that exists: the live file is moved to `.old` first.
  // Power cut there and this is exactly what the card holds -- the settings
  // under the other name and nothing under the real one.
  std::error_code error;
  std::filesystem::rename(console.sandbox.Host("/config/rommsync/config.ini"),
                          console.sandbox.Host("/config/rommsync/config.ini.old"), error);
  c.Expect(!error, "the commit is interrupted between the two renames");
  c.Expect(!console.sandbox.Exists("/config/rommsync/config.ini"), "config.ini is not there");

  console.Boot();
  c.ExpectEq(console.Configured().config.sync.interval_min, 90,
             "a console booting into that window finds the previous settings under .old");

  // ...and an edit made in that window keeps them, rather than rebuilding the
  // file from one line. This is the case that would quietly cost a user their
  // whole folder map.
  const ipc::ConfigResult applied = console.Set(Edit("sync", "saves", "false"));
  c.Expect(applied.outcome == ipc::WriteOutcome::kApplied, "an edit in that window is applied");
  const std::string recovered = console.sandbox.Read("/config/rommsync/config.ini");
  c.Expect(recovered.find("url = https://romm.example.com") != std::string::npos,
           "and the server they had configured is still configured");
  c.Expect(recovered.find("interval_min = 90") != std::string::npos, "as is everything else");
  c.Expect(recovered.find("saves = false") != std::string::npos, "with the edit in it");
}

// --- the token belongs to the server that issued it ---------------------------

/// A `server.url` change discards the pairing, asserted against the real RomM.
///
/// The token in `token.dat` was issued by one RomM and the record says which
/// (`token_store.hpp`). Repointing the console at another host and keeping it
/// would send a stranger's server this console's bearer token, so the pairing
/// goes with the URL. The fixture token is a live one, which is what makes the
/// discard mean something: it works against the docker RomM right up until the
/// edit, and afterwards the console holds nothing to send anywhere.
int ServerChanged(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  Console console(c, "engine-server");
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\nurl = " + base + "\n"),
           "the console is configured for the fixture RomM");

  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  token.scopes = {"roms.read"};
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
           "and paired with it");

  console.Boot();
  c.Expect(console.Status().auth == ipc::AuthState::kPaired, "so the console reports paired");

  // The token is real: the server it belongs to accepts it. Without this the
  // discard below would be a test of deleting a string.
  const http::Result mine =
      client.Send(harness::Authed(http::Method::kGet, base + "/api/users/me", fixture));
  c.Expect(mine.successful() && mine.response.status == 200,
           "the pairing on the card works against the RomM that issued it");

  const ipc::ConfigResult moved =
      console.Set(Edit("server", "url", "https://elsewhere.example.com"));
  c.Expect(moved.outcome == ipc::WriteOutcome::kApplied, "the server is repointed");
  c.ExpectEq(console.Configured().config.server.url,
             std::string("https://elsewhere.example.com"), "and the engine is using the new one");

  // Nothing is left for a request to the new host to carry -- not under
  // `token.dat`, and not under the `.tmp`/`.old` an interrupted commit leaves
  // beside it, which is why this goes through `DiscardToken` rather than an
  // unlink.
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat"), "token.dat is gone");
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat.old"), "and so is any .old");
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat.tmp"), "and any .tmp");
  c.Expect(!auth::LoadToken(console.directory + auth::kTokenFileName).ok(),
           "and there is no credential left to load");
  c.Expect(console.Status().auth == ipc::AuthState::kNeverPaired,
           "so the overlay is told to pair again rather than shown a working console");

  // ...and "no credential" is not a state a RomM waves through: the request this
  // console could still build reaches the server as an anonymous one and is
  // refused, which is what says the discard actually costs the session.
  http::Request anonymous;
  anonymous.method = http::Method::kGet;
  anonymous.url = base + "/api/users/me";
  const http::Result refused = client.Send(anonymous);
  c.Expect(refused.ok(), "the server answered");
  c.Expect(refused.response.status == 401 || refused.response.status == 403,
           "and refuses a request carrying no token");

  // A second edit that does not move the server leaves the pairing alone -- the
  // discard is tied to the URL changing, not to writing the file.
  auth::StoredToken again = token;
  again.server_url = "https://elsewhere.example.com";
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, again).ok(),
           "the console is paired with the new server");
  console.Boot();
  c.Expect(console.Set(Edit("sync", "saves", "false")).outcome == ipc::WriteOutcome::kApplied,
           "an unrelated edit is applied");
  c.Expect(console.sandbox.Exists("/config/rommsync/token.dat"),
           "and leaves the pairing where it is");
  c.Expect(console.Status().auth == ipc::AuthState::kPaired, "the console is still paired");

  return c.failures();
}

// --- M1-4: the verdict the card carries, and the re-pair that lifts it ---------

void Unauthenticated(checks::Checks& c) {
  Console console(c, "engine-unauthenticated");
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\nurl = https://romm.example.com\n"),
           "a configured console");

  auth::StoredToken token;
  token.server_url = "https://romm.example.com";
  token.access_token = "not-a-real-token";
  token.device_id = "console-1";
  const std::string token_path = console.directory + auth::kTokenFileName;
  const std::string verdict_path = console.directory + auth::kAuthStateFileName;
  c.Expect(auth::SaveToken(token_path, token).ok(), "with a pairing on the card");

  console.Boot();
  c.Expect(console.Status().auth == ipc::AuthState::kPaired,
           "a console with a token and no verdict is paired");

  // What a previous boot wrote once `auth::Gate` had counted enough consecutive
  // rejections. Serving it from the card is the whole point: the overlay's
  // re-pair prompt is up on the first poll, rather than after this boot has
  // spent the same budget of requests reaching the same conclusion.
  c.Expect(auth::SaveBlock(verdict_path, auth::Block::kRevoked).ok(),
           "an earlier boot recorded that the server stopped accepting the token");
  Console rebooted(c, "engine-unauthenticated-2", console.directory);
  rebooted.Boot();
  c.Expect(rebooted.Status().auth == ipc::AuthState::kUnauthenticated,
           "so the console comes up unauthenticated, with no request made");
  c.Expect(rebooted.sandbox.Exists("/config/rommsync/token.dat") ||
               console.sandbox.Exists("/config/rommsync/token.dat"),
           "and the token is still there -- this is a verdict, not a discard");

  // The re-pair, which the acceptance criterion asks to work without a restart:
  // the same engine object, one command, and the console is ready to pair again.
  std::string response;
  c.Expect(rebooted.Call(ipc::Command::kUnpair, ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
           "Unpair is answered");
  c.Expect(rebooted.Status().auth == ipc::AuthState::kNeverPaired,
           "and the console is ready to pair again, with no reload");
  c.Expect(!auth::LoadToken(token_path).ok(), "the credentials are gone");
  c.Expect(auth::LoadBlock(verdict_path).value == auth::Block::kNone,
           "and so is the verdict about them");
  c.Expect(!console.sandbox.Exists("/config/rommsync/auth.json"), "no file is left behind");
  c.Expect(!console.sandbox.Exists("/config/rommsync/auth.json.old"), "nor an interrupted .old");

  // A verdict left over a pairing that is already gone must not be read as one.
  // A never-paired console sent to a screen that says "pair this console again"
  // is one word wrong about a user who never paired at all.
  c.Expect(auth::SaveBlock(verdict_path, auth::Block::kScopeDenied).ok(),
           "a stale verdict is on the card");
  Console orphaned(c, "engine-unauthenticated-3", console.directory);
  orphaned.Boot();
  c.Expect(orphaned.Status().auth == ipc::AuthState::kNeverPaired,
           "a verdict with no token to be about leaves the console never-paired");
  c.Expect(auth::ClearBlock(verdict_path), "and it is cleaned up after the check");

  // An `auth.json` that will not read is not a verdict either: this file holds
  // nothing that cannot be worked out again by asking, so it never blocks a
  // boot (`auth_gate.hpp`).
  c.Expect(auth::SaveToken(token_path, token).ok(), "the console is paired again");
  c.Expect(console.sandbox.Write("/config/rommsync/auth.json", "{\"format\":\"rommsync-auth\""),
           "and a half-written verdict is on the card");
  Console corrupt(c, "engine-unauthenticated-4", console.directory);
  corrupt.Boot();
  c.Expect(corrupt.Status().auth == ipc::AuthState::kPaired,
           "which stops nothing: the console will find out by asking");
}

// --- M1-6: the pairing attempt the console starts -----------------------------

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/// How long a scenario may spend waiting for an attempt to settle. Generous
/// because RomM paces the token endpoint, and every wait here is bounded so a
/// wedged attempt fails with its own message rather than as a CTest timeout.
constexpr auto kBudget = 120s;

/// What the client asks for in production, not a convenient subset. `pair.happy`
/// already proves a live RomM grants all of them; the engine asks for the same.
const std::vector<std::string>& Scopes() {
  static const std::vector<std::string> scopes = auth::MinimumScopes();
  return scopes;
}

/// A console that has a transport.
///
/// The seed is a fixed literal for the reason `test_pairing.cpp` gives: the
/// identifier derived from it is stable across scenarios, so the fixture RomM
/// ends up with one device for this test binary rather than one per run.
sysmodule::PairingBackend Backend(http::HttpClient& client) {
  sysmodule::PairingBackend backend;
  backend.http = &client;
  backend.identity_seed.stable = "rommsync-nx-engine-test";
  return backend;
}

/// Point `console` at `server_url`, give it `client` to reach it with, and boot.
/// The three lines every pairing scenario starts with, in one place.
void BootPairable(Console& console, checks::Checks& c, http::HttpClient& client,
                  const std::string& server_url) {
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\nurl = " + server_url + "\n"),
           "the console is configured for " + server_url);
  console.engine.UsePairingBackend(Backend(client));
  console.Boot();
}

/// Poll `GetPairState` until `settled` is happy, or the budget runs out.
///
/// Through the command, not through `engine.pairing_status()`: the overlay never
/// sees a status that did not cross IPC first, and a status that cannot be
/// serialised is a bug this would otherwise miss.
auth::PairingStatus Settled(Console& console, checks::Checks& c,
                            const std::function<bool(const auth::PairingStatus&)>& settled) {
  const Clock::time_point give_up = Clock::now() + kBudget;
  auth::PairingStatus status = console.PairState();
  while (!settled(status) && Clock::now() < give_up) {
    std::this_thread::sleep_for(100ms);
    status = console.PairState();
  }
  c.Expect(settled(status), std::string("the attempt settled (") +
                                auth::ToString(status.state) + ": " + status.message + ")");
  return status;
}

auth::PairingStatus Terminal(Console& console, checks::Checks& c) {
  return Settled(console, c, [](const auth::PairingStatus& status) {
    return auth::IsTerminal(status.state);
  });
}

/// Start an attempt and wait until there is a code to show.
///
/// RomM allows ten device inits a minute per IP and the whole suite is one IP,
/// so a rate-limited init is waited out rather than failed -- a console pairs
/// once, ever, and never comes near that limit. Bounded, so an init broken for
/// any other reason still fails here.
auth::PairingStatus BeginAndShow(Console& console, checks::Checks& c) {
  const Clock::time_point give_up = Clock::now() + kBudget;
  while (Clock::now() < give_up) {
    auth::PairingStatus started;
    const ipc::Error answer = console.StartPair(&started);
    c.Expect(answer == ipc::Error::kOk,
             "StartPair is answered rather than refused as unavailable -- which is the whole "
             "signal that M1-6 replaced M4-1's placeholder");
    if (answer != ipc::Error::kOk) {
      return {};
    }
    // The contract `ipc::Engine::StartPairing` documents: the command hands the
    // work to the engine thread, so the status one instant later is *not*
    // `kIdle`, which the overlay would draw as "nothing happened when you
    // pressed Pair".
    //
    // Asserted as "not idle" rather than as `kStarting` on purpose. `StartPair`
    // notifies the thread before it returns, so how far that thread has got by
    // the time this line runs is the scheduler's business: pinning `kStarting`
    // would be pinning a race, and a test that fails on a preemption teaches the
    // next reader to re-run it rather than to believe it.
    c.Expect(started.state != auth::PairingState::kIdle,
             std::string("the attempt is reported as started, not as idle (") +
                 auth::ToString(started.state) + ")");

    const auth::PairingStatus status =
        Settled(console, c, [](const auth::PairingStatus& seen) {
          return !seen.user_code.empty() || auth::IsTerminal(seen.state);
        });
    if (status.state != auth::PairingState::kFailed ||
        status.message.find("429") == std::string::npos) {
      return status;
    }
    // The limit clears on a rolling minute, so a coarse sleep only overshoots it.
    std::this_thread::sleep_for(2s);
  }
  c.Expect(false, "no code arrived within the budget");
  return {};
}

/// init -> a code on the screen -> approve -> a token on the card. The
/// acceptance criterion the whole issue is about.
int Pairs(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  Console console(c, "engine-pairs");
  BootPairable(console, c, client, base);
  c.Expect(console.Status().auth == ipc::AuthState::kNeverPaired, "and has never paired");

  const auth::PairingStatus showing = BeginAndShow(console, c);
  c.Expect(showing.state == auth::PairingState::kPending,
           std::string("the attempt reaches a live code (") + auth::ToString(showing.state) +
               ": " + showing.message + ")");
  c.ExpectEq(showing.user_code.size(), std::size_t{8}, "the user code is 8 characters");
  c.ExpectEq(showing.verification_url, base + "/pair/device",
             "the verification path is joined onto the configured origin");
  c.ExpectEq(showing.verification_url_complete,
             base + "/pair/device?user_code=" + showing.user_code,
             "and the complete one carries the code");
  c.Expect(showing.expires_in > 0s, "the code has time left on it");
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat"),
           "nothing has been written yet: an attempt is not a pairing");
  c.Expect(console.Status().auth == ipc::AuthState::kNeverPaired,
           "and the console still reports itself unpaired");

  const http::Result approved = rig::ApproveDeviceCode(client, base, showing.user_code, Scopes());
  c.Expect(approved.successful(),
           "approving the code -- HTTP " + std::to_string(approved.response.status));

  const auth::PairingStatus granted = Terminal(console, c);
  c.Expect(granted.state == auth::PairingState::kApproved,
           std::string("an approved code yields a token (") + auth::ToString(granted.state) +
               ": " + granted.message + ")");

  // The half this issue owns: the grant reaches the card, atomically, beside
  // `device.dat`, and the console reports itself paired with no reload.
  c.Expect(console.Status().auth == ipc::AuthState::kPaired,
           "so the console is paired, without a restart");
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat.tmp"),
           "with no partial file left beside the token");
  c.Expect(console.sandbox.Exists("/config/rommsync/device.dat"),
           "and the identifier that has to survive a re-pair is on the card too");

  const auth::LoadedToken stored = auth::LoadToken(console.directory + auth::kTokenFileName);
  c.Expect(stored.ok(), "the committed token reads back: " + stored.message);
  c.ExpectEq(stored.value.server_url, base, "against the server that issued it");
  c.Expect(stored.value.access_token.rfind("rmm_", 0) == 0, "it is a RomM client token");
  c.Expect(!stored.value.device_id.empty(), "and it names the device every sync call is scoped by");

  // A token nothing ever accepted would make every assertion above a test of
  // string handling. This is the one that says the pairing is real.
  http::Request mine;
  mine.url = base + "/api/users/me";
  mine.headers.push_back({"Authorization", "Bearer " + stored.value.access_token});
  const http::Result whoami = client.Send(mine);
  c.ExpectOk(whoami, "the committed token is used against the RomM that issued it");
  c.Expect(whoami.response.status == 200,
           "and it is accepted -- HTTP " + std::to_string(whoami.response.status));

  // The status is still the overlay's to draw, and it still carries no secret.
  const std::string payload = auth::SerializePairingStatus(console.PairState());
  c.Expect(payload.find(stored.value.access_token) == std::string::npos,
           "the pairing payload does not carry the token it just committed");
  return c.failures();
}

/// `ipc.hpp` makes "no command waits on the network" a contract, so it is
/// asserted rather than assumed: the init is stalled by the fault proxy for five
/// seconds and `StartPair` still has to come back at once.
int NonBlocking(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  Console console(c, "engine-nonblocking");
  BootPairable(console, c, client, base);

  harness::Fault stalled(c, client, base,
                         R"({"mode":"stall","seconds":5,"path":"/api/auth/device/init"})");
  auth::PairingStatus started;
  const Clock::time_point sent = Clock::now();
  const ipc::Error answer = console.StartPair(&started);
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent);

  c.Expect(answer == ipc::Error::kOk, "StartPair is answered while the init is still in flight");
  c.Expect(started.state == auth::PairingState::kStarting,
           std::string("and reports the attempt as starting (") + auth::ToString(started.state) +
               ")");
  // Two orders of magnitude under the stall. The command does an SD read and a
  // thread hand-off; anything near five seconds means it waited for the socket.
  c.Expect(took < 1000ms, "and it returned in " + std::to_string(took.count()) +
                              "ms, without waiting for the server");

  // The attempt is the engine thread's problem from here, and it survives the
  // stall as a named failure rather than as a wedged screen.
  const auth::PairingStatus after = Terminal(console, c);
  c.Expect(after.state == auth::PairingState::kFailed,
           std::string("a stalled init fails the attempt (") + auth::ToString(after.state) + ")");
  c.Expect(!after.message.empty(), "with a reason: " + after.message);
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat"),
           "and a failed attempt writes no credentials");
  return c.failures();
}

/// The settings screen's "Re-pair": `StartPair` first, `Unpair` only once an
/// attempt is genuinely under way (M4-4, #26).
///
/// What this pins is the pair of instants between the two commands. An
/// interruption before `Unpair` leaves the old pairing on the card; one after it
/// leaves a live attempt. Neither leaves a console with nothing.
int Repairs(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  Console console(c, "engine-repairs");
  auth::StoredToken old_token;
  old_token.server_url = base;
  old_token.access_token = fixture.token;
  old_token.device_id = fixture.device_id;
  const std::string token_path = console.directory + auth::kTokenFileName;
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini", "[server]\nurl = " + base + "\n"),
           "the console is configured for the fixture RomM");
  c.Expect(auth::SaveToken(token_path, old_token).ok(), "and already paired with it");

  // Not `BootPairable`: the token has to reach the card *before* `Load` reads
  // it, so this scenario writes the config itself and boots afterwards.
  console.engine.UsePairingBackend(Backend(client));
  console.Boot();
  c.Expect(console.Status().auth == ipc::AuthState::kPaired, "so it comes up paired");

  const auth::PairingStatus showing = BeginAndShow(console, c);
  c.Expect(showing.state == auth::PairingState::kPending,
           std::string("the new attempt reaches a live code (") + auth::ToString(showing.state) +
               ": " + showing.message + ")");
  // The first instant: the attempt is under way and the credentials it is meant
  // to replace are still there. A console interrupted here is exactly the
  // console it was.
  c.Expect(auth::LoadToken(token_path).ok(),
           "starting an attempt touches no token -- an interruption here keeps the old pairing");
  c.Expect(console.Status().auth == ipc::AuthState::kPaired, "and the console is still paired");

  std::string response;
  c.Expect(console.Call(ipc::Command::kUnpair, ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
           "Unpair discards the old credentials");
  c.Expect(!auth::LoadToken(token_path).ok(), "which are gone from the card");
  c.Expect(console.Status().auth == ipc::AuthState::kNeverPaired, "so the console reports unpaired");
  // The second instant: the pairing is gone and the attempt that replaces it is
  // still live. A console interrupted here has a code on screen to finish with.
  const auth::PairingStatus surviving = console.PairState();
  c.Expect(!auth::IsTerminal(surviving.state),
           std::string("the attempt outlives the discard (") + auth::ToString(surviving.state) +
               ")");
  c.ExpectEq(surviving.user_code, showing.user_code, "and it is still the same code on screen");

  // A poll that RomM refuses once, forced -- the interruption a re-pair is most
  // likely to meet in the field, and the attempt has to survive it rather than
  // leave the console with neither a pairing nor a way to make one.
  //
  // Waited out by `polls` rather than by the clock: RomM sets the interval, so a
  // fixed sleep either fails on a slow one or disarms a fault the attempt never
  // met, and a fault nobody used would make the assertion below vacuous.
  {
    const int before = console.PairState().polls;
    harness::Fault refused(c, client, base,
                           R"({"mode":"status","status":503,"path":"/api/auth/device/token"})");
    const auth::PairingStatus after =
        Settled(console, c, [before](const auth::PairingStatus& seen) {
          return seen.polls > before || auth::IsTerminal(seen.state);
        });
    c.Expect(after.polls > before, "the refused poll was actually sent");
  }
  c.Expect(!auth::IsTerminal(console.PairState().state),
           "a 5xx mid-poll backs the attempt off rather than abandoning it");
  c.Expect(auth::LoadToken(token_path).error != auth::StoreError::kNone,
           "and it wrote no credentials on the way past");

  const http::Result approved = rig::ApproveDeviceCode(client, base, showing.user_code, Scopes());
  c.Expect(approved.successful(),
           "approving the new code -- HTTP " + std::to_string(approved.response.status));
  const auth::PairingStatus granted = Terminal(console, c);
  c.Expect(granted.state == auth::PairingState::kApproved,
           std::string("the re-pair completes (") + auth::ToString(granted.state) + ": " +
               granted.message + ")");
  c.Expect(console.Status().auth == ipc::AuthState::kPaired, "and the console is paired again");

  const auth::LoadedToken replaced = auth::LoadToken(token_path);
  c.Expect(replaced.ok(), "with credentials on the card: " + replaced.message);
  c.Expect(replaced.value.access_token != fixture.token,
           "and they are the new ones, not the discarded ones");
  return c.failures();
}

/// A server that answers nothing. The attempt is refused, and the refusal is the
/// attempt's own failure rather than a command that lied about starting one --
/// and, above all, the card is untouched.
void Unreachable(checks::Checks& c) {
  const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
  Console console(c, "engine-unreachable");
  // Port 9 is discard: nothing listens, so the connect is refused at once. A
  // hostname that does not resolve would test DNS timeouts instead.
  BootPairable(console, c, *client, "http://127.0.0.1:9");

  auth::PairingStatus started;
  c.Expect(console.StartPair(&started) == ipc::Error::kOk,
           "StartPair still starts an attempt: whether the server answers is not something "
           "this command can know without waiting, which it may not do");
  // Not `kStarting` exactly: the connect to a closed port is refused in
  // microseconds, so the thread can reach `kFailed` before this line runs. What
  // the command promises is that it did not answer `kIdle`.
  c.Expect(started.state != auth::PairingState::kIdle,
           std::string("and reports it started (") + auth::ToString(started.state) + ")");

  const auth::PairingStatus settled = Terminal(console, c);
  c.Expect(settled.state == auth::PairingState::kFailed,
           std::string("the attempt fails once the connect does (") +
               auth::ToString(settled.state) + ")");
  c.Expect(!settled.message.empty(), "with a reason the screen can draw: " + settled.message);
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat"),
           "and the console holds no credentials it did not earn");
  c.Expect(console.Status().auth == ipc::AuthState::kNeverPaired,
           "so it is exactly the console it was");
}

// --- what is still not built --------------------------------------------------

/// M7-2 (#37): "Sync now" starts a tick, and says so.
///
/// Before this issue `SdEngine::RequestSync` returned `false` unconditionally
/// and `ServiceCore::SyncNow` reported that as `kAlreadyRunning`, so a
/// configured, paired, idle console was told *a sync is already running* when
/// nothing was (#24 said so at length). This is that answer, and the worker
/// behind it.
void SyncNowStarts(checks::Checks& c) {
  Console console(c, "engine-syncnow");
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\n"
                                 "url = https://romm.example.com\n"
                                 "\n"
                                 "[sync]\n"
                                 "enabled = true\n"
                                 // No timer and no boot tick, so the scheduler
                                 // parks with **no deadline at all** and the
                                 // only thing that can ever run one is the
                                 // press. That is the shape a lost wake-up
                                 // would strand forever rather than merely
                                 // delay.
                                 "interval_min = 0\n"
                                 "on_boot = false\n"),
           "a configured card");
  auth::StoredToken token;
  token.server_url = "https://romm.example.com";
  token.access_token = "not-a-real-token";
  token.device_id = "console-syncnow";
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
           "and a paired one, so nothing in front of the engine refuses the command");
  console.Boot();

  c.Expect(!console.Status().sync_in_progress, "an idle console is not drawn as syncing");

  // The worker, which this build has for the first time. With no boot tick and
  // no interval it parks immediately, so nothing has run and nothing is due.
  console.engine.StartWorker();
  std::this_thread::sleep_for(std::chrono::milliseconds{200});
  c.Expect(console.Status().last_sync_result == ipc::SyncResult::kNever,
           "a parked worker runs nothing on its own");

  c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted,
           "Sync now is accepted rather than answered with 'one is already running'");

  // And the parked worker actually wakes for it. It has no `http::HttpClient`
  // -- nothing installed one -- so the tick cannot negotiate and settles as a
  // failure, which is the honest answer and the one the status screen draws.
  ipc::Status status = console.Status();
  for (int waited = 0; waited < 200 && status.last_sync_result == ipc::SyncResult::kNever;
       ++waited) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
    status = console.Status();
  }
  c.Expect(status.last_sync_result == ipc::SyncResult::kFailed,
           "a console with no transport fails the tick rather than reporting a sync");
  c.Expect(!status.sync_in_progress, "and is not left marked as syncing afterwards");

  // The one thing `RequestSync` is allowed to answer false for is a tick already
  // running, which is the same fact `sync_in_progress` carries -- so on an idle
  // console it is accepted again (`ipc.hpp`).
  c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted,
           "and the next press is accepted too, because nothing is running");
}

void Commands(checks::Checks& c) {
  Console console(c, "engine-commands");
  console.Boot();

  // `kUnavailable` is a sentence the overlay draws, and each of the issues
  // named here replaces its own. Pinned so the list shrinks deliberately: a
  // command that quietly started answering something plausible instead would
  // send a user looking for a problem that is not there (`engine.hpp`).
  std::string response;
  // `Unpair` came off this list in M1-4 (#8): it discards `token.dat` and the
  // verdict beside it, which is the half of "re-pairing recovers" the engine
  // owns. Starting the device-code flow afterwards is `StartPair`'s and is not
  // built.
  // `StartPair` never reaches the engine on this console: `ServiceCore` refuses
  // first, because there is no `server.url` to pair with -- which is the right
  // answer and a different sentence from "not built yet".
  c.Expect(console.Call(ipc::Command::kStartPair, ipc::EncodeEmpty(), &response) ==
               ipc::Error::kNotConfigured,
           "StartPair is refused for want of a server before the engine is asked at all");

  // M1-6 built the engine half. What is left on this list is the *transport*:
  // `core/` reaches a server through `http::HttpClient` and the console has no
  // implementation of one yet, so a build with a server to pair with and nothing
  // to reach it through answers `kUnavailable` -- "this build has no engine
  // behind that command" -- rather than a plausible refusal about the server or
  // the card (`ipc.hpp`).
  Console configured(c, "engine-commands-2");
  c.Expect(configured.sandbox.Write("/config/rommsync/config.ini",
                                    "[server]\nurl = https://romm.example.com\n"),
           "a console with a server and no transport");
  configured.Boot();
  c.Expect(configured.Call(ipc::Command::kStartPair, ipc::EncodeEmpty(), &response) ==
               ipc::Error::kUnavailable,
           "StartPair says the build has no transport rather than blaming the server");
  c.Expect(!configured.sandbox.Exists("/config/rommsync/token.dat"),
           "and a refused attempt writes nothing");
  c.Expect(!configured.sandbox.Exists("/config/rommsync/device.dat"),
           "not even the identifier a real attempt would mint");

  // What each issue that has landed took off that list, which is the signal
  // `engine.hpp` asks every one of them to give.
  std::int32_t position = 0;
  c.Expect(console.Enqueue(4, &position) != ipc::Error::kUnavailable, "Enqueue is built (#19)");
  c.Expect(console.Dequeue(4) != ipc::Error::kUnavailable, "and so is Dequeue (#19)");
  c.Expect(console.Call(ipc::Command::kUnpair, ipc::EncodeEmpty(), &response) !=
               ipc::Error::kUnavailable,
           "Unpair is built (#8)");
  ipc::ListRequest listing;
  listing.kind = ipc::ListKind::kQueue;
  c.Expect(console.Call(ipc::Command::kListBegin, ipc::EncodeListRequest(listing), &response) ==
               ipc::Error::kOk,
           "ListBegin is built (#31)");
  // The cursor payload, copied out first: `Dispatch` clears the response buffer
  // before it writes to it, so passing one string as both would hand the decoder
  // an empty request.
  const std::string cursor = response;
  c.Expect(console.Call(ipc::Command::kListNext, cursor, &response) == ipc::Error::kOk,
           "and so is ListNext, off the card, with no server anywhere");
  c.Expect(console.Call(ipc::Command::kSetEnabled, ipc::EncodeEnabled(true), &response) ==
               ipc::Error::kOk,
           "SetEnabled answers inside a successful reply, whatever it did");
  c.Expect(ipc::DecodeEnabledResult(response).value.outcome == ipc::WriteOutcome::kApplied,
           "...and it applies now: M5-3 (#30) built it");
  c.Expect(console.Set(Edit("sync", "saves", "false")).outcome == ipc::WriteOutcome::kApplied,
           "as does SetConfig (#30)");
}

/// The scenarios that need the docker RomM, by name. A table rather than a
/// second `if` chain, so the list of them is written down once.
struct RigScenario {
  const char* name;
  int (*run)(http::HttpClient&, const std::string&);
};

const RigScenario* FindRigScenario(const std::string& name) {
  static const RigScenario kRigScenarios[] = {
      {"server", ServerChanged},
      {"pairs", Pairs},
      {"nonblocking", NonBlocking},
      {"repairs", Repairs},
  };
  for (const RigScenario& scenario : kRigScenarios) {
    if (name == scenario.name) {
      return &scenario;
    }
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "queue";
  checks::Checks checks;

  if (scenario == "queue") {
    Queue(checks);
  } else if (scenario == "unauthenticated") {
    Unauthenticated(checks);
  } else if (scenario == "persists") {
    Persists(checks);
  } else if (scenario == "rollback") {
    Rollback(checks);
  } else if (scenario == "corrupt") {
    Corrupt(checks);
  } else if (scenario == "config") {
    ConfigWrites(checks);
  } else if (scenario == "commit") {
    InterruptedCommit(checks);
  } else if (scenario == "unpairs") {
    UrlWriteFails(checks);
  } else if (scenario == "stale") {
    StaleConfig(checks);
  } else if (scenario == "commands") {
    Commands(checks);
  } else if (scenario == "relaunch") {
    Relaunch(checks);
  } else if (scenario == "reenable") {
    Reenable(checks);
  } else if (scenario == "syncnow") {
    SyncNowStarts(checks);
  } else if (scenario == "unreachable") {
    Unreachable(checks);
  } else if (const RigScenario* rig_scenario = FindRigScenario(scenario)) {
    // The scenarios that need the fixture RomM. `server` pins that a *live*
    // credential stops being on the card, and the M1-6 three drive a real
    // device-code attempt -- a token nothing ever issued or accepted would prove
    // nothing about either.
    const std::string base = rig::BaseUrl();
    std::error_code error;
    std::filesystem::create_directories(rig::ScratchDir(), error);
    const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
    if (!rig::Reachable(*client, base)) {
      std::cerr << "rig unreachable at " << base
                << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
      return rig::kSkip;
    }
    const int failures = rig_scenario->run(*client, base);
    if (failures == 0) {
      std::cout << "engine." << scenario << " ok against " << base << "\n";
    }
    return failures == 0 ? 0 : 1;
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() == 0) {
    std::cout << "engine." << scenario << " ok\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
