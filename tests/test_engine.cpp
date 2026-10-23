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
//   pair_commit -- `kApproved` is said only once `token.dat` is committed, and a
//                  commit the card refuses is a failure that keeps the old token
//   stale_active -- M9-5: an `active` row a power cut left behind is work, not a
//                   transfer in flight
//   drains    -- M9-5: a rom queued over IPC becomes a file on the card
//   downloading -- M9-5: progress read over IPC mid-transfer, and the Dequeue
//                  that stops it
//   preempts  -- M9-5: a Sync now mid-transfer stops the drain rather than queuing
//                behind it, and the rom resumes afterwards
//   discards  -- M9-5: an Unpair mid-transfer stops it, rather than fetching on
//                credentials the user has just discarded
//   repoints  -- M9-5: a `[server] url` change never lets one server's token reach
//                another
//   shutdown  -- M9-5: the destructor ends a transfer at a boundary, keeping the
//                `.part` the next boot resumes from
//   sleeps    -- M9-4: a save write in flight at SleepReady is finished before the
//                acknowledgement, and nothing goes out until MinimumAwake
//   sleep_download -- M9-4: a rom in flight at SleepReady is stopped, and the worker
//                is back from download::Drain before the acknowledgement
//   foreground -- M10-16: nothing reaches the server or the card while a game
//                runs, and one tick runs once it closes
//   remote_install -- M10-19: a rom queued in RomM's web UI is claimed on a tick,
//                downloaded, and reported done
//   states_guard -- the states half does not run under a game, nor after a saves
//                half refused or offline, and its own 401 counts towards the gate
//   install_settle -- a report the link did not carry stops the pass; the rest are
//                reported once it does
//   install_wake -- a refused claim is asked again after a wake, not only a boot
//   keep_local -- #249: Keep local on the conflicts screen is the one upload
//                with overwrite=true, over another lineage's head, and only once
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "engine.hpp"
#include "faulty_disk.hpp"
#include "harness.hpp"
#include "power.hpp"
#include "power_fake.hpp"
#include "rig.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/conflict_log.hpp"
#include "rommsync/disk.hpp"
#include "rommsync/download.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/json.hpp"
#include "rommsync/log.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/remote_install.hpp"
#include "rommsync/server_capabilities.hpp"
#include "rommsync/sha1.hpp"
#include "rommsync/token_store.hpp"

namespace auth = rommsync::auth;
namespace config = rommsync::config;
namespace conflicts = rommsync::conflicts;
namespace download = rommsync::download;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace install = rommsync::install;
namespace ipc = rommsync::ipc;
// Aliased `rlog` and not `log`: at global scope that name is already taken by
// `::log`, the C library's logarithm, and GCC refuses a namespace alias that
// redeclares it (clang accepts it, which is how this reached CI once).
namespace crypto = rommsync::crypto;
namespace rlog = rommsync::log;
namespace play = rommsync::play;
namespace power = rommsync::sysmodule::power;
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

  /// The log, read the way the overlay reads it: command 16, off the wire
  /// (M7-3, #38). Never `log::Tail` directly -- the point is that a console can
  /// be asked what went wrong through the service, with no SD reader.
  ipc::LogTail Log() {
    std::string response;
    Call(ipc::Command::kGetLog, ipc::EncodeLogRequest(ipc::kMaxLogLines), &response);
    return ipc::DecodeLogTail(response).value;
  }

  /// Command 15, read off the wire: where the restore stands the instant the
  /// press was taken (M9-6, #209).
  ipc::RestoreState Restore(std::int64_t entry_id, ipc::Error* answered = nullptr) {
    std::string response;
    const ipc::Error answer =
        Call(ipc::Command::kRestoreBackup, ipc::EncodeEntryId(entry_id), &response);
    if (answered != nullptr) {
      *answered = answer;
    }
    return ipc::DecodeRestoreState(response).value;
  }

  /// Command 17: what the overlay polls while a restore runs.
  ipc::RestoreState RestoreState() {
    std::string response;
    Call(ipc::Command::kGetRestoreState, ipc::EncodeEmpty(), &response);
    return ipc::DecodeRestoreState(response).value;
  }

  /// Poll command 17 until the restore of `entry_id` is done, the way the
  /// conflicts screen does. A state that never got there is returned as it
  /// last stood, so the caller's assertion names what it saw.
  ipc::RestoreState AwaitRestore(std::int64_t entry_id) {
    ipc::RestoreState state = RestoreState();
    for (int waited = 0; waited < 400; ++waited) {
      if (state.phase == ipc::RestorePhase::kDone && state.entry_id == entry_id) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
      state = RestoreState();
    }
    return state;
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

  // The edits the settings screen's rows send (#264), through the same path:
  // each lands on the running engine and on the card, `[sources]` in a section
  // of its own because the file had none.
  ipc::ConfigEdit m10;
  m10.assignments.push_back({"sync", "on_server_delete", "keep", false});
  m10.assignments.push_back({"sources", "retroarch", "off", false});
  m10.assignments.push_back({"sources", "retroarch_content_dir", "true", false});
  c.Expect(console.Set(m10).outcome == ipc::WriteOutcome::kApplied,
           "on_server_delete and [sources] are edits SetConfig applies");
  const ipc::ConfigView live = console.Configured();
  c.Expect(live.config.sync.on_server_delete == config::OnServerDelete::kKeep,
           "on_server_delete = keep is in force with no restart");
  c.Expect(live.config.sources.retroarch == config::SourceMode::kOff,
           "so is [sources] retroarch = off");
  c.Expect(live.config.sources.retroarch_content_dir, "and retroarch_content_dir");
  c.Expect(console.sandbox.Read("/config/rommsync/config.ini")
                   .find("[sources]\nretroarch = off\nretroarch_content_dir = true\n") !=
               std::string::npos,
           "and the card holds them");

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
  // no restart in between -- which is this issue's criterion. It is not pinned
  // to `kAccepted`: what answers instead -- a tick started, or one already
  // running -- has been M7-2 (#37)'s since it gave `SdEngine::RequestSync` a
  // worker to start (`engine.syncnow`). The switch is what this issue owns; the
  // tick behind it is #37's.
  c.Expect(console.SyncNow() != ipc::SyncOutcome::kDisabled,
           "and a sync asked for now is no longer refused for the switch");

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

// --- #205: files a later release wrote, read by this one ------------------------

/// The downgrade, as the engine meets it: a `config.ini` and a `token.dat` from
/// a release this build predates. Neither is misread, both are said on the one
/// screen a user can read a sentence on, and neither is written over -- not by
/// the boot, and not by an edit from the overlay.
void NewerFiles(checks::Checks& c) {
  Console console(c, "engine-newer");
  const std::string settings = "version = " + std::to_string(config::kConfigVersion + 1) +
                               "\n[server]\nurl = https://romm.example.com\n";
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini", settings),
           "a config.ini from a later release");
  const std::string credentials =
      "{\"version\":" + std::to_string(auth::kTokenFormatVersion + 1) +
      ",\"server_url\":\"https://romm.example.com\",\"access_token\":\"not-a-real-token\","
      "\"device_id\":\"console-1\",\"scopes\":[],\"expires_at\":null}\n";
  c.Expect(console.sandbox.Write("/config/rommsync/token.dat", credentials),
           "and a token.dat from one");
  console.Boot();

  const ipc::Status status = console.Status();
  c.Expect(!status.configured, "none of the newer config.ini is in force");
  c.ExpectEq(status.config_error_count, std::int64_t{2},
             "two errors on the status screen: the config and the token");
  c.Expect(status.auth == ipc::AuthState::kPaired,
           "the console is still paired -- it is this build that cannot use the token");
  bool config_said = false;
  bool token_said = false;
  for (const config::Diagnostic& diagnostic : console.Configured().diagnostics) {
    const std::string line = diagnostic.Describe();
    config_said = config_said || (diagnostic.key == "version" && line.find("newer") != std::string::npos);
    token_said = token_said || (line.find("token.dat") != std::string::npos &&
                                line.find("newer") != std::string::npos);
  }
  c.Expect(config_said, "Settings says the config.ini is a newer release's");
  c.Expect(token_said, "and that the token.dat is");

  const ipc::ConfigResult edited = console.Set(Edit("sync", "interval_min", "15"));
  c.Expect(edited.outcome != ipc::WriteOutcome::kApplied, "an edit from the overlay is refused");
  c.ExpectEq(console.sandbox.Read("/config/rommsync/config.ini"), settings,
             "and the newer config.ini is byte for byte as it was");
  c.ExpectEq(console.sandbox.Read("/config/rommsync/token.dat"), credentials,
             "and so is the newer token.dat");
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

/// A card that lets a scenario look at the console from *inside* the pairing
/// commit: every write, rename, remove and sync that touches the credentials
/// stops first to ask the engine what it is telling the overlay.
///
/// The race `engine.pairs` and `engine.repairs` lost under `-j4` was the overlay
/// reading `kApproved` while `token.dat` was still `token.dat.tmp` -- a window
/// of a few hundred microseconds a poll every 100 ms almost never lands in. This
/// lands in it every time, because the look *is* the commit's next step.
///
/// The engine is asked directly rather than through `ipc::Dispatch`: the look
/// runs on the pairing thread, concurrently with the scenario's own polls, and
/// `ServiceCore` is served by one thread on the console.
class CommitWatch final : public rommsync::io::Disk {
 public:
  CommitWatch(Console& console, std::string directory)
      : console_(console), directory_(std::move(directory)) {
    previous_ = rommsync::io::SetDisk(this);
  }
  ~CommitWatch() override { rommsync::io::SetDisk(previous_); }

  CommitWatch(const CommitWatch&) = delete;
  CommitWatch& operator=(const CommitWatch&) = delete;

  std::FILE* Open(const std::string& path, const char* mode) override {
    Look("open " + path);
    return Inner().Open(path, mode);
  }
  std::size_t Write(std::FILE* file, const void* data, std::size_t size) override {
    return Inner().Write(file, data, size);
  }
  int Rename(const std::string& from, const std::string& to) override {
    Look("rename " + from + " -> " + to);
    const int renamed = Inner().Rename(from, to);
    Look("renamed " + from + " -> " + to);
    return renamed;
  }
  int Remove(const std::string& path) override {
    Look("remove " + path);
    return Inner().Remove(path);
  }
  bool Sync(const std::string& path) override {
    Look("sync " + path);
    return Inner().Sync(path);
  }

  /// How many times the engine was asked -- non-zero, or the scenario proved
  /// nothing about the commit.
  int looks() const {
    std::lock_guard<std::mutex> hold(mutex_);
    return looks_;
  }

  /// Every moment the engine said `kApproved` while the commit was still on the
  /// card's side of the line. Empty is the whole assertion.
  std::vector<std::string> premature() const {
    std::lock_guard<std::mutex> hold(mutex_);
    return premature_;
  }

 private:
  rommsync::io::Disk& Inner() const {
    return previous_ != nullptr ? *previous_ : rommsync::io::StdioDisk();
  }

  /// The credentials, `auth.json` beside them, and the directory a commit syncs
  /// once its rename has landed. Nothing else: the worker writes this directory
  /// too, and its files are no part of what the pairing state promises.
  bool Watched(const std::string& path) const {
    return path.find(auth::kTokenFileName) != std::string::npos ||
           path.find(auth::kAuthStateFileName) != std::string::npos ||
           path + "/" == directory_;
  }

  void Look(const std::string& what) {
    if (!Watched(what.substr(what.find(' ') + 1))) {
      return;
    }
    const auth::PairingStatus seen = console_.engine.pairing_status();
    std::lock_guard<std::mutex> hold(mutex_);
    ++looks_;
    if (seen.state == auth::PairingState::kApproved) {
      // Named relative to the config directory: the sandbox path is long, and the
      // file is the part of the line that says which step of the commit it was.
      std::string moment = what;
      for (std::string::size_type at = moment.find(directory_); at != std::string::npos;
           at = moment.find(directory_)) {
        moment.erase(at, directory_.size());
      }
      premature_.push_back(moment);
    }
  }

  Console& console_;
  std::string directory_;
  rommsync::io::Disk* previous_ = nullptr;
  mutable std::mutex mutex_;
  int looks_ = 0;
  std::vector<std::string> premature_;
};

/// The pairing state follows the card, not the server (bug found by
/// `engine.pairs` and `engine.repairs` at `-j4`).
///
/// `kApproved` tells the overlay "Paired -- this console can now sync". It may
/// only be said once the grant is a committed `token.dat`: while the commit is
/// in flight the attempt reads as still pending, and a commit the card refuses
/// reads as failed -- never as approved first and failed afterwards.
///
/// Played as a re-pair over a working credential, because that is the commit
/// that overwrites one: the refused commit has to leave the previous token
/// exactly where it was, and the one that lands replaces it whole.
int PairCommit(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  Console console(c, "engine-pair-commit");
  auth::StoredToken old_token;
  old_token.server_url = base;
  old_token.access_token = fixture.token;
  old_token.device_id = fixture.device_id;
  const std::string token_path = console.directory + auth::kTokenFileName;
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini", "[server]\nurl = " + base + "\n"),
           "the console is configured for the fixture RomM");
  c.Expect(auth::SaveToken(token_path, old_token).ok(), "and already paired with it");
  console.engine.UsePairingBackend(Backend(client));
  console.Boot();
  c.Expect(console.Status().auth == ipc::AuthState::kPaired, "so it comes up paired");

  // --- a card that refuses the new token ---------------------------------------
  {
    const auth::PairingStatus showing = BeginAndShow(console, c);
    c.Expect(showing.state == auth::PairingState::kPending,
             std::string("the re-pair reaches a live code (") + auth::ToString(showing.state) +
                 ": " + showing.message + ")");

    // Full the moment the new token is written: the temp file takes nothing.
    // Declared before the watch, so the watch sits on top and sees every call.
    faulty::Plan plan;
    plan.aimed_at = auth::kTokenFileName;
    plan.error = ENOSPC;
    plan.after = 0;
    faulty::Disk full(plan);
    CommitWatch watch(console, console.directory);

    const http::Result approved =
        rig::ApproveDeviceCode(client, base, showing.user_code, Scopes());
    c.Expect(approved.successful(),
             "approving the code -- HTTP " + std::to_string(approved.response.status));
    const auth::PairingStatus settled = Terminal(console, c);

    c.Expect(full.refusals() > 0, "the card really did refuse the commit");
    c.Expect(watch.looks() > 0, "and the commit was watched while it ran");
    for (const std::string& moment : watch.premature()) {
      c.Expect(false, "the overlay was told kApproved before token.dat was committed, at: " +
                          moment);
    }
    c.Expect(settled.state == auth::PairingState::kFailed,
             std::string("a commit the card refuses is a failed pairing, not an approved one (") +
                 auth::ToString(settled.state) + ": " + settled.message + ")");
    c.Expect(settled.message.find("could not be saved") != std::string::npos,
             "and says the token could not be saved: " + settled.message);
  }
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat.tmp"),
           "a refused commit leaves no partial file beside the token");
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat.old"),
           "nor the previous one moved aside");
  {
    const auth::LoadedToken kept = auth::LoadToken(token_path);
    c.Expect(kept.ok(), "the previous credential is still on the card: " + kept.message);
    c.Expect(kept.value.access_token == fixture.token, "and it is the previous one, untouched");
  }
  c.Expect(console.Status().auth == ipc::AuthState::kPaired,
           "so the console is still paired, on the credential it had");

  // --- a card that takes it -------------------------------------------------------
  {
    const auth::PairingStatus showing = BeginAndShow(console, c);
    c.Expect(showing.state == auth::PairingState::kPending,
             std::string("pairing again reaches a live code (") + auth::ToString(showing.state) +
                 ": " + showing.message + ")");

    CommitWatch watch(console, console.directory);
    const http::Result approved =
        rig::ApproveDeviceCode(client, base, showing.user_code, Scopes());
    c.Expect(approved.successful(),
             "approving the code -- HTTP " + std::to_string(approved.response.status));
    const auth::PairingStatus granted = Terminal(console, c);

    c.Expect(watch.looks() > 0, "the commit was watched while it ran");
    for (const std::string& moment : watch.premature()) {
      c.Expect(false, "the overlay was told kApproved before token.dat was committed, at: " +
                          moment);
    }
    c.Expect(granted.state == auth::PairingState::kApproved,
             std::string("the re-pair completes (") + auth::ToString(granted.state) + ": " +
                 granted.message + ")");
    // The instant the overlay reads `kApproved` -- before anything else runs --
    // the card and the engine both have to agree with it. This is the check
    // `engine.pairs` lost to the scheduler.
    c.Expect(console.Status().auth == ipc::AuthState::kPaired,
             "and the instant it says so, the console is paired");
    c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat.tmp"),
             "with no partial file beside the token");
    c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat.old"),
             "and nothing moved aside left behind");
  }
  const auth::LoadedToken replaced = auth::LoadToken(token_path);
  c.Expect(replaced.ok(), "the new credential is on the card: " + replaced.message);
  c.Expect(replaced.value.access_token != fixture.token,
           "and it is the new one, not the previous one");

  http::Request mine;
  mine.url = base + "/api/users/me";
  mine.headers.push_back({"Authorization", "Bearer " + replaced.value.access_token});
  const http::Result whoami = client.Send(mine);
  c.Expect(whoami.response.status == 200,
           "and RomM accepts it -- HTTP " + std::to_string(whoami.response.status));
  return c.failures();
}

/// A server that answers nothing. The attempt is refused, and the refusal is the
/// attempt's own failure rather than a command that lied about starting one --
/// and, above all, the card is untouched.
void Unreachable(checks::Checks& c) {
  const std::unique_ptr<http::HttpClient> client = rig::MakeClient();
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

// --- what M4-1 left unbuilt, and the issues that built it ---------------------

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

/// A server that answers an empty library and then stops answering.
///
/// Enough for the worker to get past step 0 and into `sync::RunTick`, and no
/// further: the negotiation blocks until a scenario lets it go. That window is
/// **inside `save_write_mutex_`** -- `RunOneTick` takes it before it calls
/// `sync::RunTick` -- which is what makes it the window a save write is in
/// flight in, whether the thing being refused is a restore (M7-2, #37) or a
/// sleep (M9-4, #208).
///
/// Shared by both, and at namespace scope for that reason.
class HeldServer final : public http::HttpClient {
 public:
  http::Result Send(const http::Request& request) override {
    ++requests_;
    if (request.url.find("/api/heartbeat") != std::string::npos) {
      // A server the floor accepts (#228), so the tick gets as far as the
      // negotiation this server exists to hold.
      http::Result result;
      result.response.status = 200;
      result.response.body = R"({"SYSTEM":{"VERSION":"5.4.0"}})";
      return result;
    }
    if (request.url.find("/api/roms") != std::string::npos) {
      http::Result result;
      result.response.status = 200;
      // The envelope `rom_index.cpp` reads, whole: `total`, `limit` and
      // `offset` are all `Required`, and an empty library is what keeps this
      // scenario about the lock rather than about a save.
      result.response.body = R"({"total":0,"limit":64,"offset":0,"items":[]})";
      return result;
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      in_negotiate_ = true;
      arrived_.notify_all();
      released_.wait(lock, [this] { return release_; });
    }
    http::Result result;
    result.error = http::Error::kConnectFailed;
    result.message = "held by the test";
    return result;
  }

  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    return Send({});
  }

  /// How many requests this server has been asked for.
  ///
  /// What `engine.sleeps` reads to say that nothing was asked of the network
  /// while the console was asleep -- a request nobody made has no response to
  /// assert on, which is `Throttled::sent()`'s reason for existing too.
  int requests() const { return requests_.load(); }

  /// Block until the worker is inside the negotiation. False if it never got
  /// there, which is a scenario that proved nothing rather than one that
  /// passed.
  bool AwaitNegotiate() {
    std::unique_lock<std::mutex> lock(mutex_);
    return arrived_.wait_for(lock, std::chrono::seconds{10}, [this] { return in_negotiate_; });
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_ = true;
    }
    released_.notify_all();
  }

 private:
  std::atomic<int> requests_{0};
  std::mutex mutex_;
  std::condition_variable arrived_;
  std::condition_variable released_;
  bool in_negotiate_ = false;
  bool release_ = false;
};

/// M7-2 (#37): a restore and a running tick both write saves, and only one of
/// them may.
///
/// Until that issue nothing else in the sysmodule wrote a save, so
/// `RestoreBackup` had the card to itself. The worker changed that: it runs
/// `sync::Execute` and `sync::SyncStates` over the same `sd_path` a restore
/// targets. Each write is atomic on its own, so what a collision produces is a
/// lost update -- restored bytes replaced moments later by an in-flight
/// download, and the tick's `.backup/` copy taken of a half-restored file --
/// which is hard rule 2 defeated by timing rather than by a bug in either
/// writer.
///
/// **What M9-6 (#209) changed is the answer, not the exclusion.** The restore
/// used to run on the IPC thread and was *refused* while a tick held the card.
/// It now runs on the worker, which is the thread the tick is on -- so it
/// cannot interleave with one by construction, and a press during a tick
/// **waits its turn** instead of being turned away. Still held open inside
/// `sync::RunTick`, because the claim is about a tick that is demonstrably
/// mid-flight: the restore is accepted, stays running while the tick is held,
/// and runs the moment it lets go.
void RestoreDuringSync(checks::Checks& c) {
  Console console(c, "engine-restore-during-sync");
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\n"
                                 "url = https://romm.example.com\n"
                                 "\n"
                                 "[sync]\n"
                                 "enabled = true\n"
                                 "interval_min = 0\n"),
           "a configured card");
  auth::StoredToken token;
  token.server_url = "https://romm.example.com";
  token.access_token = "not-a-real-token";
  token.device_id = "console-restore-race";
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
           "and a paired one, so the tick gets as far as negotiating");

  HeldServer server;
  const std::unique_ptr<fs::FileSystem> card =
      rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
  console.Boot();
  console.engine.UseServer(&server, "not-a-real-token");
  console.engine.UseCard(card.get());
  console.engine.StartWorker();

  c.Expect(server.AwaitNegotiate(), "the worker reaches the negotiation and is held there");

  // The press, while the tick is demonstrably mid-flight. The entry id is one
  // nothing has, so what the restore finally answers says it reached the
  // history -- `kNoSuchEntry` -- rather than a lock.
  ipc::Error answered = ipc::Error::kInternal;
  const ipc::RestoreState taken = console.Restore(1, &answered);
  c.Expect(answered == ipc::Error::kOk, "the command is answered rather than failing");
  c.Expect(taken.phase == ipc::RestorePhase::kRunning && taken.entry_id == 1,
           std::string("a restore during a tick is taken, not refused: ") +
               ipc::ToString(taken.phase));

  // ...and it does not run while the tick does. Asked a few times over, so a
  // restore that ran *beside* the tick would have had the time to finish.
  for (int look = 0; look < 4; ++look) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
    c.Expect(console.RestoreState().phase == ipc::RestorePhase::kRunning,
             "it waits while the tick holds the card");
  }

  server.Release();

  // And once the tick has let go, it runs: `kNoSuchEntry` for an id nothing
  // has, which is the answer that proves it reached the history.
  const ipc::RestoreState after = console.AwaitRestore(1);
  c.Expect(after.phase == ipc::RestorePhase::kDone,
           std::string("the restore runs the moment the tick is done: ") +
               ipc::ToString(after.phase));
  c.Expect(after.report.outcome == conflicts::RestoreOutcome::kNoSuchEntry,
           std::string("and it reached the history: ") +
               conflicts::ToString(after.report.outcome) + " -- " + after.report.message);
}

/// A card that can be made to stop answering in the middle of a restore.
///
/// Every file `conflicts::Restore` touches is reached through `Resolve` first,
/// so holding that one call is holding the restore -- a large save copying on
/// a slow card, which is the case #209 names -- without timing anything.
class SlowCard final : public fs::FileSystem {
 public:
  explicit SlowCard(std::unique_ptr<fs::FileSystem> real) : real_(std::move(real)) {}

  fs::Listing List(std::string_view sd_path) override { return real_->List(sd_path); }
  fs::MakeDirResult CreateDirectory(std::string_view sd_path) override {
    return real_->CreateDirectory(sd_path);
  }
  fs::Volume VolumeOf(std::string_view sd_path) const override { return real_->VolumeOf(sd_path); }

  std::string Resolve(std::string_view sd_path) const override {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (armed_) {
        held_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return !armed_; });
      }
    }
    return real_->Resolve(sd_path);
  }

  void Arm() {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = true;
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      armed_ = false;
    }
    changed_.notify_all();
  }

  /// A call is being held. False if none arrived, which is a scenario that
  /// proved nothing rather than one that passed.
  bool AwaitHeld() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds{10}, [this] { return held_; });
  }

 private:
  std::unique_ptr<fs::FileSystem> real_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  bool armed_ = false;
  mutable bool held_ = false;
};

/// The bytes `SeedRestorable` lays out: what a sync overwrote, and what it
/// wrote over them.
constexpr const char* kRestorePrevious = "the bytes this console had\n";
constexpr const char* kRestoreServerCopy = "the server's copy\n";

/// One overwrite on `console`'s card, with its backup present and its entry in
/// the history, before the engine boots. Answers the entry's id.
std::int64_t SeedRestorable(checks::Checks& c, Console& console) {
  // Switched off, so the worker's only work is the restore: a tick here would
  // be a second thing on the card and a different scenario.
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini", "[sync]\nenabled = false\n"),
           "a card with sync switched off");
  const std::string save = harness::SavePath("game.srm");
  const std::string backup = std::string(harness::kBackupDir) + "/4-retroarch-srm-1757000000.srm";
  c.Expect(console.sandbox.SeedSave(save, kRestoreServerCopy),
           "the save a sync wrote is on the card");
  c.Expect(console.sandbox.Write(backup, kRestorePrevious),
           "and so is the backup of what it replaced");
  conflicts::History history(console.directory + conflicts::kHistoryFileName);
  history.Load();
  conflicts::Entry entry;
  entry.kind = conflicts::EntryKind::kSave;
  entry.event = conflicts::Event::kReplaced;
  entry.rom_id = 4;
  entry.rom_name = "Game";
  entry.file_name = "game.srm";
  entry.slot = "retroarch-srm";
  entry.when = 1'757'000'000;
  entry.sd_path = save;
  entry.backup_sd_path = backup;
  const auto appended = history.Append(entry);
  c.Expect(appended.ok(), "the overwrite is in the history");
  return appended.id;
}

/// M9-6 (#209), found in review: a restore pressed while the worker is still
/// in its boot wait for the network runs now, not when the wait ends.
///
/// `RunWorker` waits up to `kNetworkWaitBudget` -- two minutes -- for `nifm` on
/// a console that booted without Wi-Fi, and again after every wake from sleep
/// until it has seen the network once. A restore is a copy between two files
/// on the card and needs no network at all, so handing it to a worker parked
/// in that wait would draw "Restoring..." for two minutes over nothing.
void RestoreDuringBootWait(checks::Checks& c) {
  Console console(c, "engine-restore-boot-wait");
  const std::int64_t entry_id = SeedRestorable(c, console);
  const std::unique_ptr<fs::FileSystem> card =
      rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
  console.Boot();
  console.engine.UseCard(card.get());
  std::atomic<int> probes{0};
  // The network never comes up, so the worker stays in the wait for the
  // whole of this scenario unless something gets it out.
  console.engine.UseNetworkProbe([&probes] {
    ++probes;
    return false;
  });
  console.engine.StartWorker();
  for (int waited = 0; waited < 200 && probes.load() == 0; ++waited) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  c.Expect(probes.load() > 0, "the worker is in its wait for a network that is not there");

  const auto start = std::chrono::steady_clock::now();
  console.Restore(entry_id);
  const ipc::RestoreState done = console.AwaitRestore(entry_id);
  c.Expect(done.phase == ipc::RestorePhase::kDone,
           std::string("the restore runs while the worker waits for the network: ") +
               ipc::ToString(done.phase));
  c.Expect(std::chrono::steady_clock::now() - start < std::chrono::seconds{5},
           "and does not wait out the two-minute boot budget");
  c.Expect(done.report.outcome == conflicts::RestoreOutcome::kRestored,
           std::string("and it restored: ") + conflicts::ToString(done.report.outcome) + " -- " +
               done.report.message);
  c.ExpectEq(console.sandbox.Read(harness::SavePath("game.srm")), std::string(kRestorePrevious),
             "the bytes are back on the card");
}

/// M9-6 (#209): a restore that takes as long as the card does holds no IPC
/// call for that long.
///
/// `RestoreBackup` used to copy the save on the IPC thread before it answered,
/// so the overlay's draw thread -- waiting in `serviceDispatchOut` -- was held
/// for the whole copy, and so was every other command the sysmodule would have
/// answered in the meantime. Now the command starts the restore on the worker
/// and answers at once, and `GetRestoreState` is what the overlay polls.
///
/// The card is held *inside* the restore, so every assertion below is made
/// while the copy is demonstrably still running.
void RestoreIsPolled(checks::Checks& c) {
  Console console(c, "engine-restore-polled");
  const std::string previous = kRestorePrevious;
  const std::string server_copy = kRestoreServerCopy;
  const std::string save = harness::SavePath("game.srm");
  const std::int64_t entry_id = SeedRestorable(c, console);

  SlowCard card(rommsync::host::MakeNativeFileSystem(console.sandbox.root().string()));
  console.Boot();
  console.engine.UseCard(&card);
  console.engine.StartWorker();

  card.Arm();
  const auto start = std::chrono::steady_clock::now();
  ipc::Error answered = ipc::Error::kInternal;
  const ipc::RestoreState taken = console.Restore(entry_id, &answered);
  const auto took = std::chrono::steady_clock::now() - start;
  c.Expect(answered == ipc::Error::kOk, "RestoreBackup is answered");
  c.Expect(taken.phase == ipc::RestorePhase::kRunning && taken.entry_id == entry_id,
           std::string("with the restore taken and running, not finished: ") +
               ipc::ToString(taken.phase));
  c.Expect(card.AwaitHeld(), "the restore is on the worker, held inside the copy");
  c.Expect(took < std::chrono::seconds{1},
           "and the command came back without waiting for the card");

  // **The IPC thread is free while the copy runs.** Every command the overlay
  // polls is answered -- the status screen's, and the conflicts screen's own.
  const auto polled = std::chrono::steady_clock::now();
  (void)console.Status();
  const ipc::RestoreState running = console.RestoreState();
  c.Expect(std::chrono::steady_clock::now() - polled < std::chrono::seconds{1},
           "GetStatus and GetRestoreState are answered while the save is copying");
  c.Expect(running.phase == ipc::RestorePhase::kRunning && running.entry_id == entry_id,
           "and the poll says it is still running");

  // A second press while one is running starts nothing: it is answered with the
  // restore that is already out, which is what lets a screen tell "mine" from
  // "someone else's".
  const ipc::RestoreState again = console.Restore(entry_id + 100);
  c.Expect(again.phase == ipc::RestorePhase::kRunning && again.entry_id == entry_id,
           "a press during a restore is answered with the one already running");
  c.ExpectEq(console.sandbox.Read(save), server_copy, "nothing is written while it is held");

  card.Release();
  const ipc::RestoreState done = console.AwaitRestore(entry_id);
  c.Expect(done.phase == ipc::RestorePhase::kDone, "the restore finishes once the card answers");
  c.Expect(done.report.outcome == conflicts::RestoreOutcome::kRestored,
           std::string("and it restored: ") + conflicts::ToString(done.report.outcome) + " -- " +
               done.report.message);
  c.ExpectEq(console.sandbox.Read(save), previous, "the bytes are back on the card");
  c.Expect(!done.report.backup_sd_path.empty() &&
               console.sandbox.Read(done.report.backup_sd_path) == server_copy,
           "and what they replaced was backed up first (hard rule 2)");
  c.Expect(console.RestoreState().phase == ipc::RestorePhase::kDone,
           "the outcome stays readable until the next restore");
}

// --- M9-4 (#208): the console goes to sleep ----------------------------------

/// M9-4 (#208): hard rule 2 meeting sleep.
///
/// The sysmodule did not know the console had gone to sleep, so a `SleepReady`
/// arriving mid-tick was answered by nobody and the transition happened around a
/// save write in flight -- the card going away under `sync::Execute`, which is
/// the one thing hard rule 2 exists to prevent (CLAUDE.md).
///
/// Three promises, and each of them is a different failure when it is broken:
///
///   * **the acknowledgement waits for the save write**, because it is this
///     process telling PSC the card is free of it. Held open the way
///     `engine.restore_race` holds one -- a stub server blocked inside
///     `sync::RunTick`, which `RunOneTick` runs under `save_write_mutex_`;
///   * **nothing is asked of the network between `SleepReady` and
///     `MinimumAwake`**, including a "Sync now" pressed while the lid is shut.
///     Sockets do not survive sleep -- SysDVR's `sockets.c` says so in as many
///     words -- so a request issued here is one that fails, or worse, one that
///     is still in flight when `bsdsocket` goes down;
///   * **the console comes back**. A suspend that left the worker parked
///     forever would be a client that stops syncing until the next reboot, with
///     nothing on any screen saying why.
///
/// Needs no server and no rig: the transport is the stub above.
void Sleeps(checks::Checks& c) {
  Console console(c, "engine-sleeps");
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\n"
                                 "url = https://romm.example.com\n"
                                 "\n"
                                 "[sync]\n"
                                 "enabled = true\n"
                                 // No interval, so the boot tick is the only one
                                 // that runs on its own and everything after it
                                 // is a press. That is what makes the requests
                                 // counted below attributable.
                                 "interval_min = 0\n"),
           "a configured card");
  auth::StoredToken token;
  token.server_url = "https://romm.example.com";
  token.access_token = "not-a-real-token";
  token.device_id = "console-sleeps";
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
           "and a paired one, so the tick gets as far as negotiating");

  HeldServer server;
  const std::unique_ptr<fs::FileSystem> card =
      rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
  console.Boot();
  console.engine.UseServer(&server, "not-a-real-token");
  console.engine.UseCard(card.get());
  console.engine.StartWorker();

  c.Expect(server.AwaitNegotiate(),
           "the worker is inside the tick, holding the lock every save byte is written under");

  power_fake::Scripted psc;
  power_fake::Running watching(psc, console.engine);

  psc.Deliver(power::State::kSleepReady);
  c.Expect(!psc.AwaitAcks(1, std::chrono::milliseconds{300}),
           "the acknowledgement waits: sending it here is this process telling PSC the card "
           "is free of it with a save still being written to it (hard rule 2)");

  const int asked = server.requests();
  server.Release();
  c.Expect(psc.AwaitAcks(1, std::chrono::seconds{10}),
           "and goes out once the tick has let go -- a request PSC never gets back is a "
           "console that freezes with no fatal and no crash report (sys-con#155)");
  c.ExpectEq(psc.acknowledged().size(), std::size_t{1},
             "exactly one acknowledgement for one request");

  // Asleep. A press that would ordinarily start a tick starts nothing, which is
  // the only way "no network I/O while sleeping" can be checked against a
  // console nobody is touching -- an idle worker proves nothing.
  c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted,
           "Sync now is still accepted while the console sleeps, rather than refused");
  std::this_thread::sleep_for(std::chrono::milliseconds{400});
  c.ExpectEq(server.requests(), asked,
             "but nothing goes out: sockets do not survive sleep, and a request issued "
             "between SleepReady and MinimumAwake is one that fails or one that is still in "
             "flight when bsdsocket goes down (SysDVR sockets.c)");
  c.Expect(!console.Status().sync_in_progress, "and no tick is left marked as running");

  // ...and the console comes back. `EssentialServicesAwake` is deliberately not
  // enough -- it says the critical services are up and nothing about `fsp-srv`
  // -- so the press is still held after it and only `MinimumAwake` releases it.
  psc.Deliver(power::State::kEssentialServicesAwake);
  c.Expect(psc.AwaitAcks(2, std::chrono::seconds{10}), "the first wake state is answered");
  std::this_thread::sleep_for(std::chrono::milliseconds{200});
  c.ExpectEq(server.requests(), asked,
             "and changes nothing: EssentialServicesAwake is not the card coming back "
             "(erpt_srv_service.cpp turns its filesystem access on at MinimumAwake)");

  psc.Deliver(power::State::kMinimumAwake);
  c.Expect(psc.AwaitAcks(3, std::chrono::seconds{10}), "and so is the one that is");

  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (server.requests() == asked && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  c.Expect(server.requests() > asked,
           "the press the console slept through is served once it is awake, rather than "
           "leaving a client that stops syncing until the next reboot");

  // **A second sleep, and it is not decoration.** A cancel token is one-way, so
  // a suspend that fired one shared by every tick would leave this console
  // unable to run another for the rest of the boot -- and a console that woke up
  // and never synced again would look perfectly healthy. This is the assertion
  // that would catch it: the tick the wake started is stopped, and the
  // acknowledgement goes out, exactly as the first one did.
  //
  // It is also what leaves the worker parked with nothing in flight while the
  // stub server and the card below are still alive to be parked against.
  psc.Deliver(power::State::kSleepReady);
  c.Expect(psc.AwaitAcks(4, std::chrono::seconds{10}),
           "the console can go back to sleep -- a tick cancelled at the first SleepReady did "
           "not take every later tick with it");

  // **And waking does not fire the ticks the console slept through.** Nothing is
  // due here -- the schedule is parked and the press has been served -- so a
  // wake that ran anything at all would be one inventing work out of elapsed
  // time. `sync::Scheduler` states its interval on the wall clock precisely so
  // that an eleven-hour suspend is one interval due rather than twenty-two
  // (`scheduler.hpp`), and `Resume` restamps nothing; this is that promise
  // asserted where PSC meets it rather than one level down.
  const int before_waking = server.requests();
  psc.Deliver(power::State::kMinimumAwake);
  c.Expect(psc.AwaitAcks(5, std::chrono::seconds{10}), "the second wake is answered");
  std::this_thread::sleep_for(std::chrono::milliseconds{500});
  c.ExpectEq(server.requests(), before_waking,
             "and nothing is due, so nothing runs: a wake is not a backlog of the ticks the "
             "suspend hid");
}

// --- M7-3: the log docs/TROUBLESHOOTING.md is written against ------------------

/// True when some line of `tail` carries `tag` as its event.
///
/// Asked of the tag rather than of the whole sentence, which is the split
/// log.hpp draws: the tag is the pinned contract and the detail after it is
/// whatever the failing subsystem already said. A test that matched on the
/// sentence would be a second copy of a dozen other headers' output.
bool Logged(const ipc::LogTail& tail, const char* tag) {
  const std::string needle = std::string(" ") + tag;
  for (const std::string& line : tail.lines) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string Rendered(const ipc::LogTail& tail) {
  std::string out;
  for (const std::string& line : tail.lines) {
    out += "\n    " + line;
  }
  return out.empty() ? std::string(" (the log is empty)") : out;
}

/// Every misconfiguration docs/TROUBLESHOOTING.md answers that needs no server.
///
/// Three consoles, because each is a different section of the guide and a
/// console can only be in one of these states at a time. What is asserted is the
/// *event tag*, because that is what the guide's sections are keyed on and what
/// `docs.troubleshooting_events` holds the guide to.
void LogsMisconfiguration(checks::Checks& c) {
  // A console with no `config.ini` at all -- a first boot, and the state a user
  // who unpacked the zip and stopped is in.
  {
    rommsync::log::Reset();
    Console console(c, "engine-log-bare");
    console.Boot();
    const ipc::LogTail tail = console.Log();
    c.Expect(Logged(tail, "config.no_server"),
             "a console with no config.ini says so, and says which key:" + Rendered(tail));
    c.Expect(Logged(tail, "config.diagnostic"),
             "with the file's own complaint beside it:" + Rendered(tail));
    c.Expect(!Logged(tail, "auth.token"),
             "and nothing about credentials it does not have:" + Rendered(tail));
    c.Expect(tail.total >= static_cast<std::int64_t>(tail.lines.size()),
             "the total is what was written, not what was kept");
  }

  // A `config.ini` a human got wrong. The line number is the whole value of the
  // diagnostic -- "invalid boolean" sends a user hunting through a file they
  // cannot see (config.hpp) -- so it is what this asserts on.
  {
    rommsync::log::Reset();
    Console console(c, "engine-log-badconfig");
    c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                   "[server]\n"
                                   "url = https://romm.example.com\n"
                                   "\n"
                                   "[sync]\n"
                                   "states = yes please\n"),
             "a card whose config.ini has a bad boolean on line 5");
    console.Boot();
    const ipc::LogTail tail = console.Log();
    c.Expect(Logged(tail, "config.diagnostic"), "the complaint is logged:" + Rendered(tail));
    bool numbered = false;
    for (const std::string& line : tail.lines) {
      numbered = numbered || (line.find("config.diagnostic") != std::string::npos &&
                              line.find("line 5") != std::string::npos);
    }
    c.Expect(numbered,
             "and it names the line the user has to edit, which is the point:" + Rendered(tail));
    c.Expect(!Logged(tail, "config.no_server"),
             "a file with one bad line still has a usable server:" + Rendered(tail));
  }

  // The folder-map failure the guide calls the common one: a `[platform.*]`
  // section *replaces* that platform's defaults, so one that lists no `saves`
  // switches saves off for it (docs/CONFIG.md). Emptied for every platform the
  // client ships a default for, read out of `DefaultPlatforms()` rather than
  // listed here -- a platform added to that map must not quietly stop this
  // scenario from reaching zero.
  {
    rommsync::log::Reset();
    Console console(c, "engine-log-nosaves");
    std::string ini =
        "[server]\n"
        "url = https://romm.example.com\n";
    for (const auto& [slug, folders] : config::DefaultPlatforms()) {
      static_cast<void>(folders);
      ini += "\n[platform." + slug + "]\nroms = /roms/" + slug + "\n";
    }
    c.Expect(console.sandbox.Write("/config/rommsync/config.ini", ini),
             "a card where every platform maps roms and no saves");
    console.Boot();
    c.Expect(console.Configured().config.SaveScanDirs().empty(),
             "which really is a console with nowhere to look for a save");
    const ipc::LogTail tail = console.Log();
    c.Expect(Logged(tail, "config.no_save_dirs"),
             "and the log says so rather than leaving a silent console:" + Rendered(tail));
  }

  // A console that cannot tick at all: paired, configured, and with no transport
  // installed. `SyncNow` is accepted and the worker settles as a failure
  // (`engine.syncnow`); what this adds is that the *reason* is on the record.
  {
    rommsync::log::Reset();
    Console console(c, "engine-log-notransport");
    c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                   "[server]\n"
                                   "url = https://romm.example.com\n"
                                   "\n[sync]\nenabled = true\ninterval_min = 0\n"
                                   "on_boot = false\n"),
             "a configured card");
    auth::StoredToken token;
    token.server_url = "https://romm.example.com";
    token.access_token = "not-a-real-token";
    token.device_id = "console-log";
    token.scopes = {"roms.read"};
    c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
             "and a paired one");
    console.Boot();

    const ipc::LogTail booted = console.Log();
    c.Expect(Logged(booted, "auth.token"),
             "a paired console records which server, device and scopes it holds:" +
                 Rendered(booted));
    for (const std::string& line : booted.lines) {
      c.Expect(line.find("not-a-real-token") == std::string::npos,
               "and never the token itself: " + line);
    }

    console.engine.StartWorker();
    c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted, "a tick is asked for");
    ipc::Status status = console.Status();
    for (int waited = 0; waited < 200 && status.last_sync_result == ipc::SyncResult::kNever;
         ++waited) {
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
      status = console.Status();
    }
    c.Expect(status.last_sync_result == ipc::SyncResult::kFailed, "and it fails");
    const ipc::LogTail tail = console.Log();
    c.Expect(Logged(tail, "net.offline"),
             "with a line saying the tick was skipped and why:" + Rendered(tail));

    // **Every path out of a tick writes a `sync.tick`, including the ones that
    // return before a plan exists.** The guide teaches a user that a tick ends
    // with this line and that `outcome=` is where to start reading; the four
    // early returns are the commonest failures there are, so a summary that
    // skipped them would be teaching it for the cases it is least true.
    bool summarised = false;
    for (const std::string& line : tail.lines) {
      summarised = summarised || (line.find("sync.tick") != std::string::npos &&
                                  line.find("outcome=offline") != std::string::npos);
    }
    c.Expect(summarised,
             "and a tick that never reached a plan still says how it ended:" + Rendered(tail));
  }
}

/// The four transport failures, forced against the real RomM by the fault proxy.
///
/// This is the criterion #38 states in those words: the guide's offline, 401,
/// TLS and truncation sections have to match what the proxy really produces,
/// rather than what a hand-written mock would. Each case is its own console --
/// `auth::Gate` remembers a rejection, and a second scenario over the same card
/// would be reading the first one's verdict.
///
/// The one that is not the proxy's is TLS, because a proxy cannot make a healthy
/// server offer a bad certificate. `https://` against the proxy's *plaintext*
/// port is the same failure from libcurl's point of view -- a handshake that
/// does not complete, `http::Error::kTls` -- and it is a real user's commonest
/// way in: a `[server] url` whose scheme does not match what the server speaks.
int LogsTransportFailures(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  struct Case {
    const char* name;
    const char* fault;  ///< the proxy body, or "" for none
    const char* url;    ///< "" for the proxy's own base
    const char* tag;    ///< the event docs/TROUBLESHOOTING.md documents for it
  };
  // `path` is `/api/roms` on each: the library fetch is the first request a tick
  // makes, so it is where a forced fault lands and where the guide's sections
  // say to look.
  const Case cases[] = {
      {"drop", R"({"mode":"drop","bytes":8,"path":"/api/roms","count":8})", "", "net.offline"},
      {"unauthorized",
       R"({"mode":"status","status":401,"path":"/api/roms","count":8})", "", "auth.rejected"},
      {"stall", R"({"mode":"stall","seconds":1,"path":"/api/roms","count":8})", "",
       "net.offline"},
      {"truncate", R"({"mode":"truncate","bytes":8,"path":"/api/roms","count":8})", "",
       "sync.refused"},
      {"tls", "", "TLS", "net.tls"},
  };

  for (const Case& scenario : cases) {
    rig::DisarmFault(client, base);
    std::string url = base;
    if (std::string(scenario.url) == "TLS") {
      // The same host and port, asked for over TLS it does not speak.
      url = "https" + base.substr(base.find(':'));
    } else if (scenario.fault[0] != '\0') {
      c.Expect(rig::ArmFault(client, base, scenario.fault).successful(),
               std::string("the proxy arms ") + scenario.name);
    }

    rommsync::log::Reset();
    Console console(c, std::string("engine-log-") + scenario.name);
    c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                   "[server]\nurl = " + url +
                                       "\n\n[sync]\nenabled = true\ninterval_min = 0\n"
                                       "on_boot = false\n"),
             std::string(scenario.name) + ": the console points at the rig");
    auth::StoredToken token;
    token.server_url = url;
    token.access_token = fixture.token;
    token.device_id = fixture.device_id;
    token.scopes = {"roms.read"};
    c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
             std::string(scenario.name) + ": with the fixture pairing on the card");

    const std::unique_ptr<fs::FileSystem> card =
        rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
    console.Boot();
    console.engine.UseServer(&client, token.access_token);
    console.engine.UseCard(card.get());
    console.engine.StartWorker();
    c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted,
             std::string(scenario.name) + ": a tick is asked for");

    ipc::Status status = console.Status();
    for (int waited = 0; waited < 400 && status.last_sync_result == ipc::SyncResult::kNever;
         ++waited) {
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
      status = console.Status();
    }
    c.Expect(status.last_sync_result != ipc::SyncResult::kNever,
             std::string(scenario.name) + ": the tick settled");

    const ipc::LogTail tail = console.Log();
    c.Expect(Logged(tail, scenario.tag),
             std::string(scenario.name) + ": the guide's section for it is `" + scenario.tag +
                 "`, and that is what the code writes:" + Rendered(tail));
    bool summarised = false;
    for (const std::string& line : tail.lines) {
      c.Expect(line.find(fixture.token) == std::string::npos,
               std::string(scenario.name) + ": and no line carries the token: " + line);
      // `http::ToString(http::Error::kNone)` is "ok", and a failure line that
      // gave it as the reason once read `net.offline negotiate: ok` -- the
      // client blaming a request that succeeded. Nothing that reports a failure
      // may name it.
      c.Expect(!(line.find(" net.offline ") != std::string::npos && line.find(": ok;") != std::string::npos),
               std::string(scenario.name) + ": no failure is reported as `ok`: " + line);
      summarised = summarised || line.find("sync.tick") != std::string::npos;
    }
    c.Expect(summarised,
             std::string(scenario.name) + ": the tick still says how it ended:" + Rendered(tail));
  }
  rig::DisarmFault(client, base);
  return c.failures();
}

/// Sync switched off for this console in RomM, seen from the overlay (#235).
///
/// The acceptance's own words: the overlay shows "sync disabled on server" for
/// the 400. What crosses IPC is `SyncResult::kServerDisabled` with the server
/// counted as reached -- `overlay.status` holds the sentence it renders to --
/// and the log still names it under `sync.refused`, where
/// docs/TROUBLESHOOTING.md's section for it lives. A real device whose switch is
/// really off, so the 400 is RomM's and not the proxy's.
int ServerDisabled(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  const std::string device_id =
      harness::CreateSyncDisabledDevice(c, client, base, fixture, "engine-server-disabled");
  if (device_id.empty()) {
    return c.failures();
  }

  rommsync::log::Reset();
  Console console(c, "engine-server-disabled");
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\nurl = " + base +
                                     "\n\n[sync]\nenabled = true\ninterval_min = 0\n"
                                     "on_boot = false\n"),
           "the console points at the rig");
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = device_id;
  token.scopes = {"roms.read"};
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
           "with a pairing to the switched-off device on the card");

  const std::unique_ptr<fs::FileSystem> card =
      rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
  console.Boot();
  console.engine.UseServer(&client, token.access_token);
  console.engine.UseCard(card.get());
  console.engine.StartWorker();
  c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted, "a tick is asked for");

  ipc::Status status = console.Status();
  for (int waited = 0; waited < 400 && status.last_sync_result == ipc::SyncResult::kNever;
       ++waited) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
    status = console.Status();
  }
  c.Expect(status.last_sync_result == ipc::SyncResult::kServerDisabled,
           std::string("the status says the server switched sync off, not that it failed: ") +
               ipc::ToString(status.last_sync_result));
  c.Expect(status.online, "the server was reached, so the screen does not say offline");
  c.Expect(status.auth == ipc::AuthState::kPaired, "and the pairing stands");

  const ipc::LogTail tail = console.Log();
  c.Expect(Logged(tail, "sync.refused"),
           "the log names it where the guide looks for it:" + Rendered(tail));
  bool said = false;
  for (const std::string& line : tail.lines) {
    said = said || line.find("server_disabled") != std::string::npos;
  }
  c.Expect(said, "and the tick ends as server_disabled:" + Rendered(tail));

  harness::DeleteDevice(client, base, fixture, device_id);
  return c.failures();
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
  // owns. Starting the device-code flow afterwards is `StartPair`'s, which
  // M1-6 (#123) built (`engine.pairs`).
  // `StartPair` never reaches the engine on this console: `ServiceCore` refuses
  // first, because there is no `server.url` to pair with -- which is the right
  // answer and a different sentence from "not built yet".
  c.Expect(console.Call(ipc::Command::kStartPair, ipc::EncodeEmpty(), &response) ==
               ipc::Error::kNotConfigured,
           "StartPair is refused for want of a server before the engine is asked at all");

  // M1-6 built the engine half. What is left on this list is the *transport*:
  // `core/` reaches a server through `http::HttpClient`, which the console has
  // had since M1-7 (#126) and installs at boot (`main.cpp`), but a build that
  // wires none -- this harness, here -- has a server to pair with and nothing to
  // reach it through. It answers `kUnavailable` -- "this build has no engine
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

// --- M9-5 (#197): the worker drains the queue -------------------------------

/// The real client, slowed to the timescale a console's Wi-Fi gives a rom.
///
/// The 120 MiB fixture moves off a loopback RomM in well under the 250 ms a
/// console publishes progress on (`download::kProgressInterval`), so a transfer
/// watched through `GetStatus` from here would be over before the first poll --
/// which is a fact about the rig, not about the engine. `download.progress`
/// answers it by turning the publish interval down, which is not available
/// here: the interval is the *engine's* to choose, and a test that reached in
/// and changed it would be pinning a number this issue is not about.
///
/// So the transfer is slowed instead, in the one place that holds it up without
/// touching a line of the code under test: `http::ProgressCallback` runs on the
/// transfer thread between reads of the socket, thousands of times per rom, and
/// is documented as holding the transfer up for as long as it runs. A
/// millisecond each puts 120 MiB back at a few seconds.
class Throttled : public http::HttpClient {
 public:
  Throttled(http::HttpClient& inner, std::chrono::milliseconds per_callback)
      : inner_(inner), delay_(per_callback) {}

  http::Result Send(const http::Request& request) override {
    Record(request);
    return inner_.Send(request);
  }

  http::Result Download(const http::Request& request,
                        const http::DownloadTarget& target) override {
    Record(request);
    http::DownloadTarget slowed = target;
    slowed.progress = [this, sink = target.progress](std::uint64_t staged, std::uint64_t total) {
      std::this_thread::sleep_for(delay_);
      if (sink) {
        sink(staged, total);
      }
    };
    // `slowed` outlives the call: the backend borrows a pointer to its sink
    // (`curl_http_client.cpp`), so it may not be a temporary.
    return inner_.Download(request, slowed);
  }

  /// Every request that went out, with whether it carried a bearer token.
  ///
  /// Recorded here rather than asserted on a response, because what
  /// `engine.repoints` has to pin is that a request was **never made** -- and a
  /// request nobody made has no response to look at. Under a lock: the worker
  /// makes them and the scenario reads them.
  struct Sent {
    std::string url;
    bool authorized = false;
  };

  std::vector<Sent> sent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sent_;
  }

 private:
  void Record(const http::Request& request) {
    bool authorized = false;
    for (const http::Header& header : request.headers) {
      authorized = authorized || header.name == "Authorization";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sent_.push_back({request.url, authorized});
  }

  http::HttpClient& inner_;
  std::chrono::milliseconds delay_;
  mutable std::mutex mutex_;
  std::vector<Sent> sent_;
};


/// A card the engine can write roms to, and the platform folders on it.
///
/// Creating a mapped folder is the platform layer's job and never the engine's
/// (atomic_file.hpp), so a scenario that skipped this would be pinning the
/// failure path for a folder that is not there rather than a download.
std::unique_ptr<fs::FileSystem> Card(const Console& console) {
  for (const char* folder : {"/tico/roms/nes", "/tico/roms/gba"}) {
    console.sandbox.MakeDirs(folder);
  }
  return rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
}

/// A console configured for the fixture RomM with the *sync tick parked*, so the
/// only thing its worker can be doing is draining the download queue.
///
/// `interval_min = 0` with `on_boot = false` is `engine.syncnow`'s shape: the
/// scheduler waits on no deadline at all, which is what makes a rom that arrives
/// evidence about the download worker rather than about a tick that happened to
/// run.
bool Downloadable(Console& console, checks::Checks& c, const std::string& base,
                  const harness::Fixture& fixture, bool sync_enabled = false) {
  if (!console.sandbox.Write("/config/rommsync/config.ini",
                             "[server]\nurl = " + base + "\n\n[sync]\nenabled = " +
                                 (sync_enabled ? "true" : "false") +
                                 "\ninterval_min = 0\non_boot = false\n")) {
    c.Expect(false, "the console is configured for the fixture RomM");
    return false;
  }
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  token.scopes = {"roms.read"};
  if (!auth::SaveToken(console.directory + auth::kTokenFileName, token).ok()) {
    c.Expect(false, "and paired with it");
    return false;
  }
  return true;
}

/// Poll `GetStatus` through the dispatch table until `done`, or give up.
///
/// Through the service and never off the engine's own object: what M9-5 owes the
/// overlay is that the progress is readable **over IPC** while the worker moves,
/// and a check that read the struct would pass with the codec broken.
ipc::Status Until(Console& console, const std::function<bool(const ipc::Status&)>& done,
                  std::chrono::milliseconds budget) {
  const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + budget;
  ipc::Status status = console.Status();
  while (!done(status) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
    status = console.Status();
  }
  return status;
}

/// M9-5 (#197): a row that says `active` on a *boot* is a row nothing is doing.
///
/// The state a power cut leaves in `queue.json`, and the one ovl-sysmodules'
/// `pmshellTerminateProgram` leaves too (M6-2, #33) -- the process that was
/// transferring is gone and the row still claims it is. Needs no server: what
/// it pins is what `Load` makes of the card.
void StaleActiveOnBoot(checks::Checks& c) {
  Console console(c, "engine-stale-active");
  // Two rows, the stale one *behind* the healthy one, which is the order that
  // used to matter: the worker takes the first pending entry, so a `Dequeue` of
  // the second would have cancelled the first one's transfer.
  std::vector<download::QueueEntry> rows;
  for (const auto& [rom_id, state] :
       {std::pair<std::int64_t, download::QueueState>{4, download::QueueState::kQueued},
        {5, download::QueueState::kActive},
        {6, download::QueueState::kVerifying},
        {7, download::QueueState::kDone}}) {
    download::QueueEntry entry;
    entry.rom_id = rom_id;
    entry.state = state;
    entry.queued_at = rom_id;
    entry.bytes_done = state == download::QueueState::kActive ? 4096 : 0;
    rows.push_back(std::move(entry));
  }
  c.Expect(console.sandbox.Write("/config/rommsync/queue.json", download::SerializeQueue(rows)),
           "a queue a yanked card left mid-transfer");
  console.Boot();

  const ipc::Status status = console.Status();
  c.Expect(status.download.state == ipc::DownloadState::kQueued,
           "the console comes up with work waiting rather than drawing a transfer that is not "
           "moving -- which is what an `active` row nothing is working on would say");
  c.ExpectEq(status.download.rom_id, std::int64_t{4},
             "and the entry it is about is the first one still to do");
  c.ExpectEq(status.queue_depth, std::int64_t{3}, "three rows are still work");

  // The bytes are kept: that is what makes the next drain a resume rather than
  // 4 KiB fetched again.
  bool resumable = false;
  for (const download::QueueEntry& entry : console.OnCard().entries) {
    if (entry.rom_id == 5) {
      resumable = entry.state == download::QueueState::kQueued && entry.bytes_done == 4096;
    }
  }
  c.Expect(!resumable, "nothing is written back until something changes the queue");

  // ...and once something does, the normalised states are what reaches the card.
  std::int32_t position = 0;
  c.Expect(console.Enqueue(8, &position) == ipc::Error::kOk, "a rom is queued");
  for (const download::QueueEntry& entry : console.OnCard().entries) {
    if (entry.rom_id == 5) {
      c.Expect(entry.state == download::QueueState::kQueued,
               "the stale row is `queued` on the card too");
      c.ExpectEq(entry.bytes_done, std::int64_t{4096},
                 "with its bytes kept, so the drain resumes rather than starts over");
    }
    if (entry.rom_id == 7) {
      c.Expect(entry.state == download::QueueState::kDone,
               "and a finished row is left exactly as it was");
    }
  }
}

/// M9-5 (#197): a rom queued over IPC becomes a file on the card.
///
/// Before this issue `download::Drain` had no caller outside `tests/`, so the
/// shipped sysmodule never drained its own queue and **no download happened on a
/// console at all**. This scenario is that end to end -- `Enqueue` over
/// `ipc::Dispatch`, the engine's own worker, the fixture RomM, and the bytes at
/// the destination the folder map names, hashed against what the server said.
int Drains(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  Console console(c, "engine-drains");
  if (!Downloadable(console, c, base, fixture)) {
    return c.failures();
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&client, std::string());
  console.Boot();

  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "240pee.nes", &rom)) {
    c.Expect(false,
             "the seeded library holds 240pee.nes -- re-seed and rescan: "
             "./server/testing/seed.sh && ./.venv/bin/python server/testing/provision.py");
    return c.failures();
  }

  std::int32_t position = 0;
  c.Expect(console.Enqueue(rom.id, &position) == ipc::Error::kOk,
           "the overlay queues the rom by id alone");
  c.ExpectEq(console.Status().queue_depth, std::int64_t{1}, "and the status screen shows it");

  console.engine.StartWorker();
  const ipc::Status drained = Until(
      console, [](const ipc::Status& status) { return status.queue_depth == 0; },
      std::chrono::seconds{60});
  c.ExpectEq(drained.queue_depth, std::int64_t{0},
             "the console's own worker drained the queue -- which nothing in the shipped "
             "sysmodule did before this issue");
  c.Expect(drained.download.state == ipc::DownloadState::kIdle,
           "and reports itself idle once there is nothing left to do");

  c.Expect(console.sandbox.Exists("/tico/roms/nes/240pee.nes"),
           "the rom is at the destination the folder map names");
  const download::LoadedQueue on_card = console.OnCard();
  c.ExpectEq(on_card.entries.size(), std::size_t{1},
             "the finished entry stays on the card, for the queue screen");
  if (on_card.entries.size() != 1) {
    return c.failures();
  }
  const download::QueueEntry& entry = on_card.entries.front();
  c.Expect(entry.state == download::QueueState::kDone, "in `done`");
  c.ExpectEq(entry.destination, std::string("/tico/roms/nes/240pee.nes"),
             "recording where it went, as an SD path");
  c.Expect(!entry.sha1_hash.empty(), "with the digest the server declared");
  c.ExpectEq(crypto::Sha1FileHex(console.sandbox.Host("/tico/roms/nes/240pee.nes")),
             entry.sha1_hash, "and the file on the card hashes to exactly that");
  c.Expect(!console.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp.part"),
           "no .part is left beside it");
  c.Expect(!console.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp"),
           "nor the staging file the hash was taken over");

  // The queue screen is served from the same rows, so a drained queue is one the
  // overlay can still explain (#31).
  const std::vector<ipc::ListItem> listed = console.QueueList();
  c.ExpectEq(listed.size(), std::size_t{1}, "and the queue list still has a row to draw");
  return c.failures();
}

/// M9-5 (#197): the overlay can watch a transfer move, and stop it.
///
/// Two things the acceptance asks for, in one 120 MiB transfer because both need
/// one that is still going when the assertion runs: `ipc::Status::download`
/// carrying real bytes **through the codec** while the worker is mid-file, and a
/// `Dequeue` of the entry in flight actually ending the transfer rather than
/// letting it run to completion for a rom nobody wants any more.
int DrainProgress(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  // A millisecond per socket read, so the transfer is still going when the
  // assertions below run. See `Throttled`.
  Throttled slow(client, std::chrono::milliseconds{1});

  Console console(c, "engine-progress");
  if (!Downloadable(console, c, base, fixture)) {
    return c.failures();
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&slow, std::string());
  console.Boot();

  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "synthetic-large.gba", &rom)) {
    c.Expect(false,
             "the seeded library holds synthetic-large.gba -- re-seed and rescan: "
             "./server/testing/seed.sh && ./.venv/bin/python server/testing/provision.py");
    return c.failures();
  }

  std::int32_t position = 0;
  c.Expect(console.Enqueue(rom.id, &position) == ipc::Error::kOk, "the 120 MiB rom is queued");
  console.engine.StartWorker();

  const ipc::Status moving = Until(
      console,
      [](const ipc::Status& status) {
        return status.download.state == ipc::DownloadState::kDownloading &&
               status.download.bytes_done > 0;
      },
      std::chrono::seconds{120});
  c.Expect(moving.download.state == ipc::DownloadState::kDownloading,
           "the status screen sees a transfer in flight, over the wire rather than off the "
           "engine's own struct");
  c.ExpectEq(moving.download.rom_id, rom.id, "on the rom that was queued");
  c.ExpectEq(moving.download.fs_name, std::string("synthetic-large.gba"),
             "named by fs_name and never by path");
  c.Expect(moving.download.bytes_done > 0, "with bytes already on the card");
  c.ExpectEq(moving.download.bytes_total, rom.size,
             "and the size the server declared to draw the bar against");
  c.Expect(moving.download.bytes_done < moving.download.bytes_total,
           "read while the transfer was still going, not after it finished");

  // The user's cancel. `Dequeue` on the entry the worker is *on* has to end the
  // transfer: letting a 120 MiB body run to completion for a rom that is no
  // longer queued is the cost this has to not pay.
  const std::chrono::steady_clock::time_point pressed = std::chrono::steady_clock::now();
  c.Expect(console.Dequeue(rom.id) == ipc::Error::kOk, "the user takes it out of the queue");
  const ipc::Status stopped = Until(
      console,
      [](const ipc::Status& status) {
        return status.download.state == ipc::DownloadState::kIdle;
      },
      std::chrono::seconds{30});
  const std::chrono::seconds took = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - pressed);
  c.Expect(stopped.download.state == ipc::DownloadState::kIdle,
           std::string("and the worker stops rather than finishing the file -- took ") +
               std::to_string(took.count()) + "s");
  c.ExpectEq(stopped.queue_depth, std::int64_t{0}, "with nothing left in the queue");
  c.Expect(!console.sandbox.Exists("/tico/roms/gba/synthetic-large.gba"),
           "and no rom at the destination, because the user did not want one");
  c.ExpectEq(console.OnCard().entries.size(), std::size_t{0},
             "nor a row on the card for a rom nobody asked for any more");
  return c.failures();
}

/// M9-5 (#197): "Sync now" does not wait out a 4 GB rom.
///
/// The other half of the interruptibility the issue asks for, and the half
/// `engine.shutdown` does not cover: a drain and a sync tick are on the same
/// thread by design, so a press that arrives mid-transfer has to stop the
/// transfer rather than queue behind it. `RequestSync` fires the drain's cancel
/// token in the same breath as `Scheduler::RequestNow`.
///
/// Asserted on the log rather than on a stopwatch: `download.drain the drain was
/// stopped` is written on `DrainOutcome::kCanceled` and on nothing else, so its
/// presence beside a tick that ran is exactly the sequence under test -- and it
/// does not depend on the transfer still being unfinished when the assertion
/// runs.
int DrainPreempted(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  rlog::Reset();
  Throttled slow(client, std::chrono::milliseconds{1});

  Console console(c, "engine-preempts");
  // Sync **on** this time, and still parked -- `interval_min = 0` with no boot
  // tick means the press is the only thing that can ever run one, which is what
  // makes a tick that ran evidence about the press rather than about a timer.
  if (!Downloadable(console, c, base, fixture, true)) {
    return c.failures();
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&slow, std::string());
  console.Boot();

  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "synthetic-large.gba", &rom)) {
    c.Expect(false, "the seeded library holds synthetic-large.gba");
    return c.failures();
  }

  std::int32_t position = 0;
  c.Expect(console.Enqueue(rom.id, &position) == ipc::Error::kOk, "the 120 MiB rom is queued");
  console.engine.StartWorker();

  const ipc::Status moving = Until(
      console,
      [](const ipc::Status& status) {
        return status.download.state == ipc::DownloadState::kDownloading &&
               status.download.bytes_done > 0;
      },
      std::chrono::seconds{120});
  c.Expect(moving.download.state == ipc::DownloadState::kDownloading,
           "a transfer is in flight when the button is pressed");
  c.Expect(!moving.sync_in_progress, "and no tick is running");

  c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted, "Sync now is accepted");
  const ipc::Status ticked = Until(
      console,
      [](const ipc::Status& status) { return status.last_sync_result != ipc::SyncResult::kNever; },
      std::chrono::seconds{120});
  c.Expect(ticked.last_sync_result != ipc::SyncResult::kNever,
           "and the tick actually ran, rather than waiting for the rom to finish");

  const ipc::LogTail tail = console.Log();
  bool stopped = false;
  for (const std::string& line : tail.lines) {
    stopped = stopped || line.find("the drain was stopped") != std::string::npos;
  }
  c.Expect(stopped,
           "because the drain was cancelled to make room for it, rather than run to the end "
           "of a 120 MiB body:" + Rendered(tail));

  // ...and the rom is not abandoned. The entry stays queued beside its `.part`,
  // so the next pass of the worker's loop resumes it.
  const ipc::Status after = Until(
      console, [](const ipc::Status& status) { return status.queue_depth == 0; },
      std::chrono::seconds{120});
  c.ExpectEq(after.queue_depth, std::int64_t{0},
             "the worker picks the transfer back up once the tick is done, and finishes it");
  c.Expect(console.sandbox.Exists("/tico/roms/gba/synthetic-large.gba"),
           "so the rom still arrives");

  // The finished rom asks for a tick of its own, to bring its save (#241).
  // Waited out, so the card is not taken away from under it.
  const auto ticks_logged = [&console]() {
    int logged = 0;
    for (const std::string& line : console.Log().lines) {
      logged += line.find("sync.tick") != std::string::npos ? 1 : 0;
    }
    return logged;
  };
  const std::chrono::steady_clock::time_point by =
      std::chrono::steady_clock::now() + std::chrono::seconds{120};
  while ((ticks_logged() < 2 || console.Status().sync_in_progress) &&
         std::chrono::steady_clock::now() < by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  c.ExpectEq(ticks_logged(), 2, "and the download's own tick ran after the press's");
  return c.failures();
}

/// M9-5 (#197): a drain does not outlive the credentials it started with.
///
/// `RunOneDrain` copies the token and the base URL at its start and holds them
/// for a transfer the issue's own design notes say can take an hour. `Unpair`
/// clears `token_` and takes `token.dat` off the card -- and none of that
/// reaches a drain already running, so before this the console went on fetching
/// rom bodies with the credentials the user had just discarded, and writing them
/// to the card, until the queue emptied. Repointing `[server] url` is the same
/// gap with the old server's URL as well as its token.
int DrainDiscarded(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  rlog::Reset();
  Throttled slow(client, std::chrono::milliseconds{1});

  Console console(c, "engine-discards");
  if (!Downloadable(console, c, base, fixture)) {
    return c.failures();
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&slow, std::string());
  console.Boot();

  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "synthetic-large.gba", &rom)) {
    c.Expect(false, "the seeded library holds synthetic-large.gba");
    return c.failures();
  }

  std::int32_t position = 0;
  c.Expect(console.Enqueue(rom.id, &position) == ipc::Error::kOk, "the 120 MiB rom is queued");
  console.engine.StartWorker();

  const ipc::Status moving = Until(
      console,
      [](const ipc::Status& status) {
        return status.download.state == ipc::DownloadState::kDownloading &&
               status.download.bytes_done > 0;
      },
      std::chrono::seconds{120});
  c.Expect(moving.download.state == ipc::DownloadState::kDownloading,
           "a transfer is in flight on the token about to be discarded");

  std::string response;
  c.Expect(console.Call(ipc::Command::kUnpair, ipc::EncodeEmpty(), &response) == ipc::Error::kOk,
           "the console is unpaired");
  c.Expect(!console.sandbox.Exists("/config/rommsync/token.dat"),
           "and the credentials are off the card");

  // Waited for on the log rather than on a stopwatch, for `engine.preempts`'
  // reason: `download.drain the drain was stopped` is written on
  // `DrainOutcome::kCanceled` and on nothing else.
  ipc::LogTail tail = console.Log();
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{60};
  bool cancelled = false;
  while (!cancelled && std::chrono::steady_clock::now() < deadline) {
    for (const std::string& line : tail.lines) {
      cancelled = cancelled || line.find("the drain was stopped") != std::string::npos;
    }
    if (cancelled) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
    tail = console.Log();
  }
  c.Expect(cancelled,
           "the transfer is stopped rather than run on with a discarded token:" + Rendered(tail));

  // ...and it stays stopped. An unpaired console has nothing to send, so the
  // worker's guard refuses the next drain rather than retrying with an empty
  // bearer token.
  c.Expect(console.Status().auth == ipc::AuthState::kNeverPaired,
           "the console reports itself unpaired");
  std::this_thread::sleep_for(std::chrono::seconds{1});
  c.Expect(console.Status().download.state != ipc::DownloadState::kDownloading,
           "and no further transfer is started");
  c.Expect(!console.sandbox.Exists("/tico/roms/gba/synthetic-large.gba"),
           "so no rom fetched on discarded credentials reaches the card");
  c.Expect(console.Status().queue_depth > 0,
           "while the entry stays queued, for a console that pairs again");
  return c.failures();
}

/// M9-5 (#197): a drain never carries one server's token to another.
///
/// `ApplyConfigEdit` calls this "a bearer token pointed at a stranger" and takes
/// the credential off the card *before* the write for exactly that reason. It
/// had two holes, and the drain is what made both reachable.
///
/// The card was cleared and **memory was not**: `token_` and `list_token_` still
/// held the previous server's token, so the next tick -- and, since M9-5, the
/// next drain -- would have sent it to the new host. And a discard landing in the
/// window before a starting drain registered its cancel token found nothing to
/// fire, so the drain ran on credentials that were already gone.
///
/// What is asserted is a request that was never made, which is why the client
/// records rather than the test reading a response: a request nobody made has no
/// response to look at.
int DrainRepointed(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  rlog::Reset();
  Throttled slow(client, std::chrono::milliseconds{1});

  Console console(c, "engine-repoints");
  if (!Downloadable(console, c, base, fixture)) {
    return c.failures();
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&slow, std::string());
  console.Boot();

  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "synthetic-large.gba", &rom)) {
    c.Expect(false, "the seeded library holds synthetic-large.gba");
    return c.failures();
  }

  std::int32_t position = 0;
  c.Expect(console.Enqueue(rom.id, &position) == ipc::Error::kOk, "the 120 MiB rom is queued");
  console.engine.StartWorker();

  const ipc::Status moving = Until(
      console,
      [](const ipc::Status& status) {
        return status.download.state == ipc::DownloadState::kDownloading &&
               status.download.bytes_done > 0;
      },
      std::chrono::seconds{120});
  c.Expect(moving.download.state == ipc::DownloadState::kDownloading,
           "a transfer is in flight against the server about to be replaced");

  const std::string elsewhere = "https://elsewhere.example.com";
  c.Expect(console.Set(Edit("server", "url", elsewhere)).outcome == ipc::WriteOutcome::kApplied,
           "the console is repointed at another server");
  c.Expect(console.Status().auth == ipc::AuthState::kNeverPaired,
           "which discards the pairing, as it always has");

  // The drain is cancelled, and every drain after it is refused by the guard --
  // the console holds no token to send anywhere now.
  std::this_thread::sleep_for(std::chrono::seconds{2});
  const std::vector<Throttled::Sent> sent = slow.sent();
  int to_the_stranger = 0;
  int authorized_to_the_stranger = 0;
  for (const Throttled::Sent& request : sent) {
    if (request.url.rfind(elsewhere, 0) == 0) {
      ++to_the_stranger;
      authorized_to_the_stranger += request.authorized ? 1 : 0;
    }
  }
  c.ExpectEq(authorized_to_the_stranger, 0,
             "no request carrying this console's bearer token reached the new server");
  c.ExpectEq(to_the_stranger, 0, "nor any request at all, because there is nothing to send");
  c.Expect(sent.size() > 0, "and the recording client was actually the one in use");

  c.Expect(!console.sandbox.Exists("/tico/roms/gba/synthetic-large.gba"),
           "the rom did not arrive on credentials the console no longer holds");
  c.Expect(console.Status().queue_depth > 0,
           "and stays queued, for a console that pairs with the new server");
  return c.failures();
}

/// M9-5 (#197): a shutdown mid-transfer ends at a boundary, not at the end of a
/// 4 GB file.
///
/// The engine's destructor already cancels a sync tick in flight; a download
/// worker on the same thread has to answer the same way, or terminating the
/// sysmodule from ovl-sysmodules would hang for as long as the rom had left.
/// What survives is the `.part`, which is what makes the next boot resume rather
/// than restart (`download::Drain`).
int DrainShutdown(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  harness::Sandbox sandbox(c, "engine-shutdown");
  const std::string directory = sandbox.Host(harness::kConfigDir) + "/";
  c.Expect(sandbox.Write("/config/rommsync/config.ini",
                         "[server]\nurl = " + base +
                             "\n\n[sync]\nenabled = false\ninterval_min = 0\non_boot = false\n"),
           "a configured console with its sync tick parked");
  sandbox.MakeDirs("/tico/roms/gba");
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  token.scopes = {"roms.read"};
  c.Expect(auth::SaveToken(directory + auth::kTokenFileName, token).ok(), "and paired with it");

  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "synthetic-large.gba", &rom)) {
    c.Expect(false, "the seeded library holds synthetic-large.gba");
    return c.failures();
  }

  const std::unique_ptr<fs::FileSystem> card =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  Throttled slow(client, std::chrono::milliseconds{1});
  std::chrono::milliseconds tore_down{0};
  {
    // The engine behind a pointer, so it can be destroyed -- which is the only
    // way to stop its worker -- while the card it wrote to is still here to look
    // at afterwards.
    auto engine = std::make_unique<sysmodule::SdEngine>();
    engine->UseCard(card.get());
    engine->UseServer(&slow, std::string());
    engine->Load(directory);
    auto core = std::make_unique<ipc::ServiceCore>(*engine);

    std::string response;
    c.Expect(ipc::Dispatch(*core, static_cast<std::uint32_t>(ipc::Command::kEnqueue),
                           ipc::EncodeRomId(rom.id), &response) == ipc::Error::kOk,
             "the 120 MiB rom is queued");
    engine->StartWorker();

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{120};
    ipc::DownloadSnapshot snapshot;
    while (std::chrono::steady_clock::now() < deadline) {
      ipc::Dispatch(*core, static_cast<std::uint32_t>(ipc::Command::kGetStatus),
                    ipc::EncodeEmpty(), &response);
      snapshot = ipc::DecodeStatus(response).value.download;
      if (snapshot.state == ipc::DownloadState::kDownloading && snapshot.bytes_done > 0) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    c.Expect(snapshot.state == ipc::DownloadState::kDownloading,
             "and the worker is mid-transfer when the process is asked to go away");

    // The service goes first: it holds a reference to the engine.
    core.reset();
    const std::chrono::steady_clock::time_point stopping = std::chrono::steady_clock::now();
    engine.reset();
    tore_down = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stopping);
  }
  c.Expect(tore_down < std::chrono::seconds{15},
           std::string("the engine shut down at an operation boundary rather than waiting the "
                       "transfer out -- took ") +
               std::to_string(tore_down.count()) + "ms");

  c.Expect(!sandbox.Exists("/tico/roms/gba/synthetic-large.gba"),
           "nothing reached the destination, because the transfer never finished");
  c.Expect(sandbox.Exists("/tico/roms/gba/synthetic-large.gba.tmp.part"),
           "and the bytes that did arrive are kept, so the next boot resumes rather than "
           "starts over");
  const download::LoadedQueue on_card = download::LoadQueue(directory + download::kQueueFileName);
  c.ExpectEq(on_card.entries.size(), std::size_t{1}, "the entry is still on the card");
  if (on_card.entries.size() == 1) {
    c.Expect(!download::Terminal(on_card.entries.front().state),
             "and still something to do, rather than a rom recorded as finished");
  }
  return c.failures();
}

/// M9-4 (#208): a rom in flight when the console goes to sleep.
///
/// `engine.sleeps` pins the save write; this is the other thing the worker can
/// be doing when `SleepReady` lands, and it is the one that can take an hour.
/// M9-5 (#197) put `download::Drain` on the worker thread, so at a sleep there
/// may be a transfer with a socket open and a `.part` being appended to on the
/// card -- and the acknowledgement is this process telling PSC that both are
/// free of it.
///
/// **What PSC adds over the three cancels that already existed** is the *wait*.
/// `CancelDrainLocked` returns immediately; it fires a token the drain reads at
/// its next operation boundary. So the assertion here is not that the drain was
/// asked to stop, it is that it had actually stopped -- the `.part` does not
/// grow by a byte after the acknowledgement.
int SleepDownload(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }

  rlog::Reset();
  Throttled slow(client, std::chrono::milliseconds{1});

  Console console(c, "engine-sleep-download");
  if (!Downloadable(console, c, base, fixture)) {
    return c.failures();
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&slow, std::string());
  console.Boot();

  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "synthetic-large.gba", &rom)) {
    c.Expect(false, "the seeded library holds synthetic-large.gba");
    return c.failures();
  }

  std::int32_t position = 0;
  c.Expect(console.Enqueue(rom.id, &position) == ipc::Error::kOk, "the 120 MiB rom is queued");
  console.engine.StartWorker();

  const ipc::Status moving = Until(
      console,
      [](const ipc::Status& status) {
        return status.download.state == ipc::DownloadState::kDownloading &&
               status.download.bytes_done > 0;
      },
      std::chrono::seconds{120});
  c.Expect(moving.download.state == ipc::DownloadState::kDownloading,
           "a transfer is in flight when the console is put to sleep");

  power_fake::Scripted psc;
  power_fake::Running watching(psc, console.engine);

  const std::chrono::steady_clock::time_point slept = std::chrono::steady_clock::now();
  psc.Deliver(power::State::kSleepReady);
  c.Expect(psc.AwaitAcks(1, std::chrono::seconds{30}), "the sleep is acknowledged");
  const std::chrono::milliseconds took = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - slept);
  c.Expect(took < std::chrono::seconds{15},
           std::string("at an operation boundary rather than at the end of the body -- a "
                       "transition PSC waits out is a console that freezes (sys-con#155); took ") +
               std::to_string(took.count()) + "ms");

  // The assertion this scenario exists for: the drain is *over*, not merely
  // asked to stop. A `.part` that is still growing after the acknowledgement is
  // this process writing to a card it has just told PSC it is finished with.
  const std::string part = console.sandbox.Host("/tico/roms/gba/synthetic-large.gba.tmp.part");
  std::error_code failed;
  const std::uintmax_t at_ack = std::filesystem::file_size(part, failed);
  c.Expect(!failed, "the partial file is on the card, so there is something to measure");
  std::this_thread::sleep_for(std::chrono::milliseconds{500});
  c.ExpectEq(std::filesystem::file_size(part, failed), at_ack,
             "and it has not grown since: the worker had come back from download::Drain "
             "before the acknowledgement went out, rather than after it");

  const download::LoadedQueue on_card = console.OnCard();
  c.ExpectEq(on_card.entries.size(), std::size_t{1}, "the entry is still on the card");
  if (on_card.entries.size() == 1) {
    c.Expect(!download::Terminal(on_card.entries.front().state),
             "and still something to do, so the wake resumes it rather than losing it");
  }

  psc.Deliver(power::State::kMinimumAwake);
  c.Expect(psc.AwaitAcks(2, std::chrono::seconds{10}), "the wake is acknowledged");

  const ipc::Status after = Until(
      console, [](const ipc::Status& status) { return status.queue_depth == 0; },
      std::chrono::seconds{300});
  c.ExpectEq(after.queue_depth, std::int64_t{0},
             "and the worker picks the transfer back up on its own");
  c.Expect(console.sandbox.Exists("/tico/roms/gba/synthetic-large.gba"),
           "so the rom still arrives, resumed from the bytes the sleep kept");
  return c.failures();
}

/// M10-2 (#228): the engine reads which RomM it is talking to before it asks
/// that RomM anything, refuses one below the floor without a negotiate, and
/// says so on the status screen and in the log.
///
/// Two halves. The fixture runs a RomM the floor accepts, so the first raises
/// the floor past it, which is the only way to see a refusal from a real
/// server: every request the console made is recorded, and none of them may be
/// a negotiate. The second runs at the build's own floor and holds the answer
/// to the fixture's version -- the full capability set on 5.4.0, and a refusal
/// if a fixture below the floor is ever started -- which is the one place the
/// real floor meets a real server.
int RommVersion(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  rig::DisarmFault(client, base);
  const rommsync::server::Probe asked = rommsync::server::ReadServer(client, base);
  c.Expect(asked.ok() && asked.capabilities.known,
           "the fixture says which RomM it is: " + asked.message);
  const std::string fixture_version = asked.capabilities.version;

  struct Run {
    std::string name;
    rommsync::server::Version floor;
  };
  const Run runs[] = {
      {"above", rommsync::server::Version{99, 0, 0}},
      {"floor", rommsync::server::kMinimumVersion},
  };
  for (const Run& run : runs) {
    rommsync::log::Reset();
    Console console(c, "engine-romm-" + run.name);
    console.engine.UseMinimumServerVersion(run.floor);
    c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                   "[server]\nurl = " + base +
                                       "\n\n[sync]\nenabled = true\ninterval_min = 0\n"
                                       "on_boot = false\n"),
             run.name + ": the console points at the rig");
    auth::StoredToken token;
    token.server_url = base;
    token.access_token = fixture.token;
    token.device_id = fixture.device_id;
    token.scopes = {"roms.read"};
    c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
             run.name + ": with the fixture pairing on the card");

    // Every request recorded, none slowed.
    Throttled recorded(client, std::chrono::milliseconds{0});
    const std::unique_ptr<fs::FileSystem> card =
        rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
    console.Boot();
    c.ExpectEq(console.Status().server_version, std::string(),
               run.name + ": before a tick nothing has been asked");
    console.engine.UseServer(&recorded, token.access_token);
    console.engine.UseCard(card.get());
    console.engine.StartWorker();

    const auto tick = [&console, &c, &run](int number) {
      const std::int64_t before = console.Status().last_sync_at;
      c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted,
               run.name + ": tick " + std::to_string(number) + " is asked for");
      ipc::Status status = console.Status();
      for (int waited = 0;
           waited < 400 && (status.sync_in_progress ||
                            status.last_sync_result == ipc::SyncResult::kNever ||
                            (number > 1 && status.last_sync_at == before && waited < 80));
           ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
        status = console.Status();
      }
      return status;
    };
    const auto count = [&recorded](std::string_view path) {
      int n = 0;
      for (const Throttled::Sent& sent : recorded.sent()) {
        n += sent.url.find(path) != std::string::npos ? 1 : 0;
      }
      return n;
    };

    const ipc::Status first = tick(1);
    const ipc::LogTail tail = console.Log();
    const bool refused = run.floor > rommsync::server::ParseVersion(fixture_version).value_or(
                                         rommsync::server::Version{});
    c.ExpectEq(first.server_version, fixture_version,
               run.name + ": the status names the fixture's version");
    if (refused) {
      c.Expect(!first.server_supported, run.name + ": and says it is too old");
      c.Expect(first.last_sync_result == ipc::SyncResult::kFailed,
               run.name + ": the tick is a failed one");
      c.Expect(first.online, run.name + ": the server was reached, so it is not offline");
      c.ExpectEq(count("/api/sync/negotiate"), 0,
                 run.name + ": and no negotiate call was made");
      c.ExpectEq(count("/api/roms"), 0, run.name + ": nor was the library asked for");
      c.Expect(Logged(tail, "server.unsupported"),
               run.name + ": the refusal is a named event:" + Rendered(tail));
      bool summarised = false;
      for (const std::string& line : tail.lines) {
        summarised = summarised || line.find("sync.tick outcome=unsupported") != std::string::npos;
      }
      c.Expect(summarised, run.name + ": and the tick ends saying so:" + Rendered(tail));
      c.ExpectEq(rommsync::overlay::Render(first, 0).headline,
                 std::string("RomM is too old to sync with"),
                 run.name + ": which is the overlay's headline");
    } else {
      c.Expect(first.server_supported, run.name + ": a supported server is not refused");
      c.Expect(count("/api/sync/negotiate") > 0, run.name + ": and is negotiated with");
      c.Expect(Logged(tail, "server.version"),
               run.name + ": the version read is on the record:" + Rendered(tail));
    }
    c.ExpectEq(count("/api/heartbeat"), 1, run.name + ": the first tick asked once");

    // The second tick: a supported server is not asked again this boot; a
    // refused one is, so an upgrade is noticed without a reboot.
    const ipc::Status second = tick(2);
    c.ExpectEq(count("/api/heartbeat"), refused ? 2 : 1,
               run.name + (refused ? ": a refused server is asked again on the next tick"
                                   : ": a supported server is asked once a boot"));
    c.ExpectEq(second.server_supported, !refused, run.name + ": and the answer stands");
  }
  return c.failures();
}

/// A server whose heartbeat is held until the test lets it answer, and which
/// counts heartbeats and negotiations. Everything else answers as an empty
/// library, and a negotiation as a dropped connection.
class HeldHeartbeat final : public http::HttpClient {
 public:
  http::Result Send(const http::Request& request) override {
    http::Result result;
    if (request.url.find("/api/heartbeat") != std::string::npos) {
      std::unique_lock<std::mutex> lock(mutex_);
      ++heartbeats_;
      arrived_.notify_all();
      released_.wait(lock, [this] { return heartbeats_ < held_; });
      result.response.status = 200;
      result.response.body = R"({"SYSTEM":{"VERSION":"5.4.0"}})";
      return result;
    }
    if (request.url.find("/api/roms") != std::string::npos) {
      result.response.status = 200;
      result.response.body = R"({"total":0,"limit":64,"offset":0,"items":[]})";
      return result;
    }
    if (request.url.find("/api/sync/negotiate") != std::string::npos) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++negotiations_;
    }
    result.error = http::Error::kConnectFailed;
    result.message = "nothing behind this test server";
    return result;
  }

  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    return Send({});
  }

  /// Hold heartbeat number `n` and every one after it.
  void HoldFrom(int n) {
    std::lock_guard<std::mutex> lock(mutex_);
    held_ = n;
  }

  bool AwaitHeartbeat(int n) {
    std::unique_lock<std::mutex> lock(mutex_);
    return arrived_.wait_for(lock, std::chrono::seconds{10}, [this, n] { return heartbeats_ >= n; });
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      held_ = 1 << 30;
    }
    released_.notify_all();
  }

  int heartbeats() {
    std::lock_guard<std::mutex> lock(mutex_);
    return heartbeats_;
  }
  int negotiations() {
    std::lock_guard<std::mutex> lock(mutex_);
    return negotiations_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable arrived_;
  std::condition_variable released_;
  int heartbeats_ = 0;
  int held_ = 1 << 30;
  int negotiations_ = 0;
};

/// M10-2 (#228): an answer about the server that was asked for **before** the
/// engine was told to forget it must not stand for what came after.
///
/// The heartbeat is held, and while it is in flight the floor moves past what
/// the server will answer -- the same "ask again" a `server.url` change or a
/// pairing sets. When the old question's answer lands it was judged against the
/// old floor, and a tick that recorded it would mark the server read: the next
/// tick would then skip the heartbeat and negotiate with a server the new floor
/// refuses.
void RommForgotten(checks::Checks& c) {
  Console console(c, "engine-romm-forgotten");
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\nurl = https://romm.example.com\n\n[sync]\n"
                                 "enabled = true\ninterval_min = 0\non_boot = false\n"),
           "a configured card");
  auth::StoredToken token;
  token.server_url = "https://romm.example.com";
  token.access_token = "not-a-real-token";
  token.device_id = "console-romm-forgotten";
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
           "and a paired one");
  console.engine.UseMinimumServerVersion(rommsync::server::kMinimumVersion);
  HeldHeartbeat server;
  server.HoldFrom(1);
  const std::unique_ptr<fs::FileSystem> card =
      rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
  console.Boot();
  console.engine.UseServer(&server, token.access_token);
  console.engine.UseCard(card.get());
  console.engine.StartWorker();

  const auto settle = [&console](std::int64_t after) {
    ipc::Status status = console.Status();
    for (int waited = 0; waited < 400 && (status.sync_in_progress || status.last_sync_at == after);
         ++waited) {
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
      status = console.Status();
    }
    return status;
  };

  c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted, "the first tick is asked for");
  c.Expect(server.AwaitHeartbeat(1), "and it is asking the server which RomM it is");
  console.engine.UseMinimumServerVersion(rommsync::server::Version{99, 0, 0});
  server.Release();
  const ipc::Status first = settle(0);
  c.Expect(first.last_sync_result != ipc::SyncResult::kNever, "the first tick settled");
  const int negotiated = server.negotiations();
  c.Expect(negotiated > 0, "and negotiated, under the floor it asked with");

  c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted, "a second tick is asked for");
  // A refused tick returns before `last_sync_at` moves, so the wait is on the
  // tick being over rather than on the clock.
  ipc::Status second = console.Status();
  for (int waited = 0; waited < 400 && (second.sync_in_progress || server.heartbeats() < 2 ||
                                        second.server_supported);
       ++waited) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
    second = console.Status();
  }
  c.ExpectEq(server.heartbeats(), 2,
             "the second tick asks again: the first answer predates the forgetting");
  c.Expect(!second.server_supported, "and the new floor refuses the server");
  c.ExpectEq(server.negotiations(), negotiated, "so the second tick negotiated nothing");
}

/// A server whose heartbeat answers and whose library is never there: every
/// `GET /api/roms` is a connection that failed. Counts them, and says when the
/// first has been answered.
class NoLibrary final : public http::HttpClient {
 public:
  http::Result Send(const http::Request& request) override {
    http::Result result;
    if (request.url.find("/api/heartbeat") != std::string::npos) {
      result.response.status = 200;
      result.response.body = R"({"SYSTEM":{"VERSION":"5.4.0"}})";
      return result;
    }
    if (request.url.find("/api/roms") != std::string::npos) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++pages_;
      arrived_.notify_all();
    }
    result.error = http::Error::kConnectFailed;
    result.message = "nothing behind this test server";
    return result;
  }

  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    return Send({});
  }

  bool AwaitPage() {
    std::unique_lock<std::mutex> lock(mutex_);
    return arrived_.wait_for(lock, std::chrono::seconds{10}, [this] { return pages_ > 0; });
  }

  int pages() {
    std::lock_guard<std::mutex> lock(mutex_);
    return pages_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable arrived_;
  int pages_ = 0;
};

/// #251: the library fetch backs off between attempts now, and a console put to
/// sleep in that backoff cancels the tick. That has to end the tick as
/// cancelled -- one request, no retry after it, and nothing recorded -- rather
/// than as a server that is down: `kOffline` would be a failed tick on the
/// status screen and a `net.offline` line about a link nobody was using.
void IndexCanceled(checks::Checks& c) {
  rommsync::log::Reset();
  // Before the console, so it outlives the worker: a fetch that ignores the
  // cancel is still asking it for pages when the scenario ends.
  NoLibrary server;
  Console console(c, "engine-index-canceled");
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\nurl = https://romm.example.com\n\n[sync]\n"
                                 "enabled = true\ninterval_min = 0\non_boot = false\n"),
           "a configured card");
  auth::StoredToken token;
  token.server_url = "https://romm.example.com";
  token.access_token = "not-a-real-token";
  token.device_id = "console-index-canceled";
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
           "and a paired one");
  console.engine.UseMinimumServerVersion(rommsync::server::kMinimumVersion);
  const std::unique_ptr<fs::FileSystem> card =
      rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
  console.Boot();
  console.engine.UseServer(&server, token.access_token);
  console.engine.UseCard(card.get());
  console.engine.StartWorker();

  c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted, "a tick is asked for");
  c.Expect(server.AwaitPage(), "and its first page of the library failed");
  // The tick is in its backoff now, or about to be: either way the sleep ends it.
  console.engine.Quiesce();

  const ipc::Status status = console.Status();
  c.ExpectEq(server.pages(), 1, "the sleep stopped the fetch before its retry");
  c.Expect(status.last_sync_result == ipc::SyncResult::kNever,
           "and the tick is not recorded as failed: it was cancelled");
  const ipc::LogTail tail = console.Log();
  c.Expect(!Logged(tail, "net.offline"),
           "nor logged as a server that is down:" + Rendered(tail));
}

/// #231: a card where Tico 0.8 keeps saves for two accounts and `config.ini`
/// names neither. The scan syncs no Tico save there (`scan.tico`); what this adds
/// is that the settings screen -- `GetConfig`, which is where a user goes to pick
/// one -- names both accounts after a tick, as a warning on `[tico] account`.
int TicoAccounts(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  rlog::Reset();
  Console console(c, "engine-tico-accounts");
  if (!Downloadable(console, c, base, fixture, true)) {
    return c.failures();
  }
  const std::string kAccounts[] = {"0123456789abcdef0123456789abcdef",
                                   "fedcba9876543210fedcba9876543210"};
  for (const std::string& uid : kAccounts) {
    console.sandbox.MakeDirs("/tico/saves/users/" + uid + "/snes");
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&client, std::string());
  console.Boot();

  const auto warned = [&console, &kAccounts] {
    for (const config::Diagnostic& note : console.Configured().diagnostics) {
      if (note.section == "tico" && note.key == "account" &&
          note.severity == config::Severity::kWarning &&
          note.message.find(kAccounts[0]) != std::string::npos &&
          note.message.find(kAccounts[1]) != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  c.Expect(!warned(), "nothing is said before the card has been read");

  console.engine.StartWorker();
  c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted, "a tick is asked for");
  const ipc::Status ticked = Until(
      console,
      [](const ipc::Status& status) { return status.last_sync_result != ipc::SyncResult::kNever; },
      std::chrono::seconds{120});
  c.Expect(ticked.last_sync_result != ipc::SyncResult::kNever, "and it ran");
  c.Expect(warned(), "the settings screen names both accounts and asks for one");
  return c.failures();
}

/// Every save row RomM holds for `rom_id`, by id.
std::vector<std::int64_t> SaveIdsFor(http::HttpClient& client, const std::string& base,
                                     const harness::Fixture& fixture, std::int64_t rom_id) {
  const http::Result listed = client.Send(harness::Authed(
      http::Method::kGet, base + "/api/saves?rom_id=" + std::to_string(rom_id), fixture));
  const rommsync::json::ParseResult parsed = rommsync::json::Parse(listed.response.body);
  std::vector<std::int64_t> ids;
  if (listed.successful() && parsed.ok() && parsed.value.is_array()) {
    for (const rommsync::json::Value& save : parsed.value.elements()) {
      ids.push_back(harness::Number(save, "id"));
    }
  }
  return ids;
}

/// Saves other consoles left on another rom, more of them than the log's tail
/// holds lines. Deleted again when this goes.
///
/// **A library is never empty of other people's saves**, and a console new to
/// RomM is offered every one of them on its first tick. Before #237 placed them
/// under a name the scan reads back, each landed under RomM's tagged name and
/// was a `scan.skipped` line on every tick after -- so a tick wrote more lines
/// than `log::kTailLines`, and the tail no longer held the tick before it. That
/// is what main's long-lived fixture did to `engine.foreground` and
/// `engine.server_deleted` with twenty-two rows `sched.*` had left behind. How
/// many lines such a tick writes since #236 and #237 has not been re-measured,
/// which is the reason this stays: a scenario that counts ticks has to count
/// them on a server like that one, not only on a fresh fixture.
class OtherConsolesSaves {
 public:
  OtherConsolesSaves(rig::Checks& c, http::HttpClient& client, const std::string& base,
                     const harness::Fixture& fixture)
      : client_(client), base_(base), fixture_(fixture), staging_(c, "engine-other-consoles") {
    harness::Rom rom;
    c.Expect(harness::FindRom(client, base, fixture, "gb240p.gb", &rom),
             "the fixture library has gb240p.gb, for other consoles' saves");
    if (rom.id == 0) {
      return;
    }
    const std::string staged = staging_.Host("/another-console.srm");
    rig::WriteFile(staged, "a save another console uploaded\n");
    for (std::size_t i = 0; i <= rlog::kTailLines; ++i) {
      // A name of its own each, so no two rows can share RomM's datetime tag.
      const std::string name = "another-console-" + std::to_string(i) + ".srm";
      harness::Save save;
      if (harness::UploadSave(client, base, fixture, rom.id,
                              harness::UniqueSlot("engine-other-console"), "engine-other",
                              staged, name, /*with_device=*/false, &save)) {
        ids_.push_back(save.id);
      }
    }
    c.ExpectEq(ids_.size(), rlog::kTailLines + 1,
               "RomM holds more saves from other consoles than the log's tail has lines");
  }

  ~OtherConsolesSaves() {
    for (const std::int64_t id : ids_) {
      harness::DeleteSave(client_, base_, fixture_, id);
    }
  }

  OtherConsolesSaves(const OtherConsolesSaves&) = delete;
  OtherConsolesSaves& operator=(const OtherConsolesSaves&) = delete;

 private:
  http::HttpClient& client_;
  const std::string& base_;
  const harness::Fixture& fixture_;
  harness::Sandbox staging_;
  std::vector<std::int64_t> ids_;
};

/// How many `sync.tick` lines the log has written since this was installed:
/// one per tick that ran, whatever ended it (`LogTickEnd`). Installed before
/// the console it counts, so it outlives the worker that writes to it.
///
/// **Counted at the sink, not off `GetLog`.** The tail is the last
/// `log::kTailLines` lines, and one tick on a console whose library holds
/// other consoles' saves writes more than that by itself
/// (`OtherConsolesSaves`). A count of the tail is then a count of the ticks
/// that still fit in it, which is how main's fixture made both scenarios
/// below come up one tick short of ticks that had run. The sink sees every
/// line; `test_heap_steady`'s `TickCounter` counts the same way.
class TickLines final : public rlog::Sink {
 public:
  TickLines() : previous_(rlog::GetSink()) { rlog::SetSink(this); }
  ~TickLines() override { rlog::SetSink(previous_); }

  void Write(rlog::Level, std::string_view line) override {
    if (line.find(" sync.tick ") != std::string_view::npos) {
      ticks_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  int ticks() const { return ticks_.load(std::memory_order_relaxed); }

 private:
  rlog::Sink* previous_;
  std::atomic<int> ticks_{0};
};

/// The real client, with a game that starts the moment the armed tick asks for
/// the library -- the one request a tick makes before it touches a save.
///
/// What it stands for is a user launching a game while a tick is under way: the
/// probe said no when the tick started, and says yes before the first save is
/// placed. Also records every URL, for "nothing more went out".
class LaunchesMidTick final : public http::HttpClient {
 public:
  LaunchesMidTick(http::HttpClient& inner, std::atomic<bool>& running)
      : inner_(inner), running_(running) {}

  http::Result Send(const http::Request& request) override {
    Record(request.url);
    if (request.url.find("/api/roms") != std::string::npos && armed_.exchange(false)) {
      running_ = true;
    }
    return inner_.Send(request);
  }

  http::Result Download(const http::Request& request,
                        const http::DownloadTarget& target) override {
    Record(request.url);
    return inner_.Download(request, target);
  }

  void Arm() { armed_ = true; }

  std::vector<std::string> urls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return urls_;
  }

 private:
  void Record(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    urls_.push_back(url);
  }

  http::HttpClient& inner_;
  std::atomic<bool>& running_;
  std::atomic<bool> armed_{false};
  mutable std::mutex mutex_;
  std::vector<std::string> urls_;
};

/// How many of `urls` from `from` on asked RomM to plan a sync.
std::size_t Negotiations(const std::vector<std::string>& urls, std::size_t from) {
  std::size_t asked = 0;
  for (std::size_t i = from; i < urls.size(); ++i) {
    asked += urls[i].find("/api/sync/negotiate") != std::string::npos ? 1 : 0;
  }
  return asked;
}

/// M10-16 (#242): nothing is downloaded, placed or uploaded while a game runs,
/// and **one** tick runs once it closes.
///
/// Against the real RomM, on a device of its own with no history, so the tick
/// that finally runs has something real to do: the save on the card is not on
/// the server, and negotiate plans its upload. That is what makes "nothing was
/// uploaded while the game ran" a claim about a tick that *would* have, rather
/// than about one with nothing to send. The game is a fake foreground probe --
/// `pm:dmnt` is the console's half and no host can reach it. The library holds
/// other consoles' saves too (`OtherConsolesSaves`), as a real one does.
///
/// Two ticks fall due under the game -- the boot tick and two presses -- and one
/// runs after it, which is #37's coalescing doing what it already did for a
/// suspend.
int Foreground(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  harness::Rom rom;
  c.Expect(harness::FindRom(client, base, fixture, "nova.nes", &rom),
           "the fixture library has nova.nes");
  if (rom.id == 0) {
    return c.failures();
  }
  const std::string device_id =
      harness::CreateDevice(c, client, base, fixture, "engine-foreground");
  if (device_id.empty()) {
    return c.failures();
  }
  const std::vector<std::int64_t> before = SaveIdsFor(client, base, fixture, rom.id);
  const OtherConsolesSaves others(c, client, base, fixture);

  rlog::Reset();
  const TickLines ticks;
  Console console(c, "engine-foreground");
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 "[server]\nurl = " + base +
                                     "\n\n[sync]\nenabled = true\ninterval_min = 0\n"
                                     // The boot tick is the scheduled one, and it
                                     // falls due the instant the worker starts --
                                     // under the game.
                                     "on_boot = true\nstates = false\n"),
           "the console points at the rig");
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = device_id;
  token.scopes = {"roms.read"};
  c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
           "paired, as a device RomM has never seen sync");

  // Tico's layout rather than RetroArch's: what is under test is the hold, not
  // which folder a save is found in.
  const std::string save = "/tico/saves/nes/nova.srm";
  console.sandbox.MakeDirs("/tico/saves/nes");
  const std::string played = "what the game had on the card when it started\n";
  c.Expect(console.sandbox.Write(save, played), "a save on the card the server does not have");

  std::atomic<bool> running{true};
  // Which application `pm` names, for the third game (#261). Zero is one it
  // would not name, which is what the first two games are.
  std::atomic<std::uint64_t> title{0};
  LaunchesMidTick counted(client, running);
  const std::unique_ptr<fs::FileSystem> card =
      rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
  console.Boot();
  console.engine.UseForegroundProbe(
      [&running, &title] {
        play::Foreground foreground;
        foreground.running = running.load();
        if (foreground.running && title.load() != 0) {
          foreground.program_id = title.load();
        }
        return foreground;
      },
      std::chrono::milliseconds{50});
  console.engine.UseServer(&counted, token.access_token);
  console.engine.UseCard(card.get());
  console.engine.StartWorker();

  c.Expect(console.SyncNow() == ipc::SyncOutcome::kDeferred,
           "Sync now under the game is taken, and says it waits for the game");
  std::this_thread::sleep_for(std::chrono::milliseconds{500});
  c.Expect(console.SyncNow() == ipc::SyncOutcome::kDeferred,
           "and a second press is the same held tick, not a tick already running");
  // Twenty of the worker's re-polls, and every one of them finds the game.
  std::this_thread::sleep_for(std::chrono::seconds{1});

  c.ExpectEq(counted.urls().size(), std::size_t{0},
             "nothing reached the server while the game ran: no heartbeat -- this is the "
             "boot's first tick, so one was due (#228) -- no library, no negotiate (where "
             "#235's 'sync is disabled' would come from), no upload, no download");
  const std::vector<std::int64_t> during = SaveIdsFor(client, base, fixture, rom.id);
  c.ExpectEq(during.size(), before.size(), "so RomM holds no new save for the rom");
  c.ExpectEq(console.sandbox.Read(save), played, "and the save on the card is untouched");
  c.Expect(!console.sandbox.Exists("/config/rommsync/state.db"),
           "no tick recorded a baseline, because no tick ran");
  c.Expect(!console.sandbox.Exists("/config/rommsync/play.db"),
           "and none stamped the play window: a held tick did not look at the saves, so "
           "the session the game is in is not cut in half (#39)");
  const ipc::Status held = console.Status();
  c.Expect(!held.sync_in_progress, "the status screen does not draw a held tick as syncing");
  c.Expect(held.last_sync_result == ipc::SyncResult::kNever, "nor as one that ran");
  const ipc::LogTail held_log = console.Log();
  c.ExpectEq(ticks.ticks(), 0, "the log has no tick in it:" + Rendered(held_log));
  c.Expect(Logged(held_log, "sync.held"), "and says why:" + Rendered(held_log));

  // The game closes.
  running = false;
  const ipc::Status after = Until(
      console,
      [](const ipc::Status& status) {
        return status.last_sync_result != ipc::SyncResult::kNever && !status.sync_in_progress;
      },
      std::chrono::seconds{60});
  c.Expect(after.last_sync_result == ipc::SyncResult::kOk ||
               after.last_sync_result == ipc::SyncResult::kPartial,
           std::string("a tick runs once it has, and reaches the server: ") +
               ipc::ToString(after.last_sync_result));
  // Long enough for a second tick to show itself if the held ones had been
  // queued rather than coalesced.
  std::this_thread::sleep_for(std::chrono::seconds{1});
  const ipc::LogTail done_log = console.Log();
  c.ExpectEq(ticks.ticks(), 1,
             "exactly one: the boot tick and both presses were the same tick" +
                 Rendered(done_log));

  std::vector<std::int64_t> uploaded;
  for (const std::int64_t id : SaveIdsFor(client, base, fixture, rom.id)) {
    if (std::find(before.begin(), before.end(), id) == before.end()) {
      uploaded.push_back(id);
    }
  }
  c.ExpectEq(uploaded.size(), std::size_t{1}, "and that tick uploaded the save, once");
  c.ExpectEq(console.sandbox.Read(save), played, "which it read and did not rewrite");

  // **A game launched while a tick is under way.** The probe said no when the
  // tick started; the game starts as the tick fetches the library, before any
  // save is planned. The tick has to notice before it places or sends one --
  // the game has just read the save a download would replace.
  harness::Rom other;
  c.Expect(harness::FindRom(client, base, fixture, "240pee.nes", &other),
           "the fixture library has 240pee.nes");
  const std::vector<std::int64_t> other_before = SaveIdsFor(client, base, fixture, other.id);
  const std::string second = "/tico/saves/nes/240pee.srm";
  c.Expect(console.sandbox.Write(second, "a second save, new since the last tick\n"),
           "a second save the server does not have");
  const std::size_t asked_before = counted.urls().size();
  counted.Arm();
  c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted,
           "with no game running, Sync now starts a tick");
  const std::chrono::steady_clock::time_point launched_by =
      std::chrono::steady_clock::now() + std::chrono::seconds{30};
  while (!running.load() && std::chrono::steady_clock::now() < launched_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  c.Expect(running.load(), "the game starts while that tick fetches the library");
  std::this_thread::sleep_for(std::chrono::seconds{1});
  c.ExpectEq(Negotiations(counted.urls(), asked_before), std::size_t{0},
             "the tick stops before it asks for a plan: nothing is placed or sent under the "
             "game it now finds");
  c.ExpectEq(SaveIdsFor(client, base, fixture, other.id).size(), other_before.size(),
             "so the second save is not on the server yet");
  c.ExpectEq(ticks.ticks(), 1, "and that tick did not end as one that ran");
  c.Expect(!console.Status().sync_in_progress, "nor is it drawn as still running");

  running = false;
  const std::chrono::steady_clock::time_point ticked_by =
      std::chrono::steady_clock::now() + std::chrono::seconds{60};
  while (ticks.ticks() < 2 && std::chrono::steady_clock::now() < ticked_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  std::this_thread::sleep_for(std::chrono::seconds{1});
  c.ExpectEq(ticks.ticks(), 2,
             "the press it was is held, and runs once the game closes -- once");
  std::vector<std::int64_t> other_uploaded;
  for (const std::int64_t id : SaveIdsFor(client, base, fixture, other.id)) {
    if (std::find(other_before.begin(), other_before.end(), id) == other_before.end()) {
      other_uploaded.push_back(id);
    }
  }
  c.ExpectEq(other_uploaded.size(), std::size_t{1}, "and uploads the second save, once");
  std::size_t heartbeats = 0;
  for (const std::string& url : counted.urls()) {
    heartbeats += url.find("/api/heartbeat") != std::string::npos ? 1 : 0;
  }
  c.ExpectEq(heartbeats, std::size_t{1},
             "one heartbeat in the whole run: the tick that ran after the first game read "
             "it, the held ticks never did, and the second tick reused the answer (#228)");

  // **Which title was played** (M10-26, #261). A third game, one `pm` names,
  // writes nova's save while a press is held for it -- the module's exit write
  // -- and closes. The tick after it derives the session from the save and
  // names the application seen writing it. Past a whole second first, so the
  // save's mtime lands after the window the second tick stamped.
  std::this_thread::sleep_for(std::chrono::milliseconds{1100});
  title = 0x0100'0000'00AB'C000ULL;
  running = true;
  c.Expect(console.SyncNow() == ipc::SyncOutcome::kDeferred, "a third game holds a press");
  std::this_thread::sleep_for(std::chrono::milliseconds{300});
  c.Expect(console.sandbox.Write(save, "what the third game wrote as it closed\n"),
           "the game writes its save");
  std::this_thread::sleep_for(std::chrono::milliseconds{200});
  running = false;
  const std::chrono::steady_clock::time_point third_by =
      std::chrono::steady_clock::now() + std::chrono::seconds{60};
  while (ticks.ticks() < 3 && std::chrono::steady_clock::now() < third_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  const ipc::LogTail titled_log = console.Log();
  c.ExpectEq(ticks.ticks(), 3, "the tick after the third game runs" + Rendered(titled_log));
  bool named = false;
  for (const std::string& line : titled_log.lines) {
    named = named || (line.find(" play.recorded rom " + std::to_string(rom.id) + " slot ") !=
                          std::string::npos &&
                      line.find(", title 0100000000ABC000") != std::string::npos);
  }
  c.Expect(named, "and records nova's session under the title seen writing it" +
                      Rendered(titled_log));
  for (const std::int64_t id : SaveIdsFor(client, base, fixture, rom.id)) {
    if (std::find(before.begin(), before.end(), id) == before.end() &&
        std::find(uploaded.begin(), uploaded.end(), id) == uploaded.end()) {
      uploaded.push_back(id);
    }
  }

  for (const std::int64_t id : uploaded) {
    harness::DeleteSave(client, base, fixture, id);
  }
  for (const std::int64_t id : other_uploaded) {
    harness::DeleteSave(client, base, fixture, id);
  }
  harness::DeleteDevice(client, base, fixture, device_id);
  return c.failures();
}

/// M10-7 (#233): a save deleted in RomM, met by the whole engine -- the setting
/// read from `config.ini`, the tick, the log line, the conflicts screen and the
/// status counts -- once per value of `[sync] on_server_delete`.
///
/// `keep` is the run that proves the setting reaches the tick, because removing
/// is also what a tick does with no setting at all. Each run is a device of its
/// own, so the first tick's upload is the only thing negotiate plans for
/// nova.nes -- beside the other consoles' saves on another rom
/// (`OtherConsolesSaves`) that a console new to RomM is offered as well.
int ServerDeleted(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  harness::Rom rom;
  c.Expect(harness::FindRom(client, base, fixture, "nova.nes", &rom),
           "the fixture library has nova.nes");
  if (rom.id == 0) {
    return c.failures();
  }
  const OtherConsolesSaves others(c, client, base, fixture);

  for (const bool keep : {false, true}) {
    const std::string what = keep ? "keep" : "remove";
    const std::string device_id =
        harness::CreateDevice(c, client, base, fixture, "engine-deleted-" + what);
    if (device_id.empty()) {
      return c.failures();
    }
    const std::vector<std::int64_t> before = SaveIdsFor(client, base, fixture, rom.id);

    rlog::Reset();
    const TickLines ticks;
    Console console(c, "engine-deleted-" + what);
    c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                   "[server]\nurl = " + base +
                                       "\n\n[sync]\nenabled = true\ninterval_min = 0\n"
                                       "on_boot = false\nstates = false\non_server_delete = " +
                                       what + "\n"),
             what + ": the console points at the rig");
    auth::StoredToken token;
    token.server_url = base;
    token.access_token = fixture.token;
    token.device_id = device_id;
    token.scopes = {"roms.read"};
    c.Expect(auth::SaveToken(console.directory + auth::kTokenFileName, token).ok(),
             what + ": paired");
    const std::string save = "/tico/saves/nes/nova.srm";
    console.sandbox.MakeDirs("/tico/saves/nes");
    const std::string bytes = "the save that is about to be deleted in RomM (" + what + ")\n";
    console.sandbox.SeedSave(save, bytes);

    const std::unique_ptr<fs::FileSystem> card =
        rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
    console.Boot();
    console.engine.UseServer(&client, token.access_token);
    console.engine.UseCard(card.get());
    console.engine.StartWorker();

    const auto tick = [&console, &c, &what, &ticks](int n) {
      c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted, what + ": a tick is asked for");
      const std::chrono::steady_clock::time_point by =
          std::chrono::steady_clock::now() + std::chrono::seconds{60};
      while (ticks.ticks() < n && std::chrono::steady_clock::now() < by) {
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
      }
      c.ExpectEq(ticks.ticks(), n, what + ": and it ran");
    };

    tick(1);
    std::vector<std::int64_t> uploaded;
    for (const std::int64_t id : SaveIdsFor(client, base, fixture, rom.id)) {
      if (std::find(before.begin(), before.end(), id) == before.end()) {
        uploaded.push_back(id);
      }
    }
    c.ExpectEq(uploaded.size(), std::size_t{1}, what + ": the first tick uploads the save");
    for (const std::int64_t id : uploaded) {
      harness::DeleteSave(client, base, fixture, id);
    }

    tick(2);
    const ipc::LogTail log = console.Log();
    c.Expect(Logged(log, "save.deleted"), what + ": the log says RomM deleted it:" + Rendered(log));
    const ipc::Status status = console.Status();
    c.ExpectEq(status.conflicts, std::int64_t{0},
               what + ": a deletion is not counted as a conflict");
    ipc::ConflictQuery query;
    std::string response;
    console.Call(ipc::Command::kListConflicts, ipc::EncodeConflictQuery(query), &response);
    const ipc::ConflictPage page = ipc::DecodeConflictPage(response).value;
    if (keep) {
      c.ExpectEq(console.sandbox.Read(save), bytes, "keep: the save is still on the card");
      c.Expect(page.entries.empty(), "keep: and nothing was replaced, so nothing is listed");
    } else {
      c.Expect(!console.sandbox.Exists(save), "remove: the save is off the card");
      c.Expect(console.sandbox.HasBackupOf(bytes), "remove: and its bytes are under .backup/");
      c.Expect(page.entries.size() == 1 &&
                   page.entries[0].entry.event == conflicts::Event::kDeleted &&
                   page.entries[0].backup_present,
               "remove: the conflicts screen lists it, with its backup there to restore");
    }

    // ...and the tick after that sends nothing back up.
    tick(3);
    std::size_t after = 0;
    for (const std::int64_t id : SaveIdsFor(client, base, fixture, rom.id)) {
      after += std::find(before.begin(), before.end(), id) == before.end() ? 1 : 0;
    }
    c.ExpectEq(after, std::size_t{0}, what + ": nothing was uploaded again");

    harness::DeleteDevice(client, base, fixture, device_id);
  }
  return c.failures();
}

// --- the shared slot (#236) ----------------------------------------------------

/// A request as RetroArch's Cloud Sync makes it: WebDAV under
/// `/api/sync/retroarch/`, as the fixture user over Basic auth (#226 R2).
http::Result AsRetroArch(http::HttpClient& client, const std::string& base, http::Method method,
                         const std::string& path, const std::string& body = std::string()) {
  http::Request request;
  request.method = method;
  request.url = base + "/api/sync/retroarch/" + path;
  request.headers.push_back(
      {"Authorization", "Basic " + rig::Base64(std::string(rig::kUser) + ":" + rig::kPassword)});
  if (method == http::Method::kPut) {
    request.headers.push_back({"Content-Type", "application/octet-stream"});
    request.body = body;
  }
  return client.Send(request);
}

/// One save row RomM holds for a rom.
struct ServerRow {
  std::int64_t id = 0;
  std::string slot;
  std::string emulator;
  std::string file_name;
  std::string content_hash;
};

std::vector<ServerRow> RowsFor(http::HttpClient& client, const std::string& base,
                               const harness::Fixture& fixture, std::int64_t rom_id,
                               const std::string& slot) {
  const http::Result listed = client.Send(harness::Authed(
      http::Method::kGet, base + "/api/saves?rom_id=" + std::to_string(rom_id), fixture));
  const rommsync::json::ParseResult parsed = rommsync::json::Parse(listed.response.body);
  std::vector<ServerRow> rows;
  if (!listed.successful() || !parsed.ok() || !parsed.value.is_array()) {
    return rows;
  }
  for (const rommsync::json::Value& save : parsed.value.elements()) {
    ServerRow row;
    row.id = harness::Number(save, "id");
    row.slot = harness::Field(save, "slot");
    row.emulator = harness::Field(save, "emulator");
    row.file_name = harness::Field(save, "file_name");
    row.content_hash = harness::Field(save, "content_hash");
    if (row.slot == slot) {
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

/// Delete what the shared and the old slot hold for `rom_id`, so a scenario
/// starts from a slot it knows. Rig tests run serially; what is there is an
/// earlier run's leftovers.
void ClearSlots(http::HttpClient& client, const std::string& base, const harness::Fixture& fixture,
                std::int64_t rom_id) {
  for (const char* slot : {"autosave", "tico-srm"}) {
    for (const ServerRow& row : RowsFor(client, base, fixture, rom_id, slot)) {
      harness::DeleteSave(client, base, fixture, row.id);
    }
  }
}

/// A console on its own device, configured for the rig with no tick of its own:
/// each one is a press, run to its end.
bool PointAtTheRig(Console& console, rig::Checks& c, const std::string& base,
                   const harness::Fixture& fixture, const std::string& device_id) {
  if (!console.sandbox.Write("/config/rommsync/config.ini",
                             "[server]\nurl = " + base +
                                 "\n\n[sync]\nenabled = true\ninterval_min = 0\n"
                                 "on_boot = false\nstates = false\n")) {
    c.Expect(false, "the console points at the rig");
    return false;
  }
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = device_id;
  token.scopes = {"roms.read"};
  if (!auth::SaveToken(console.directory + auth::kTokenFileName, token).ok()) {
    c.Expect(false, "and is paired as its own device");
    return false;
  }
  return true;
}

/// Press Sync now and wait for that tick to end.
bool TickOnce(Console& console, const TickLines& ticks, rig::Checks& c, const std::string& what) {
  const int before = ticks.ticks();
  if (console.SyncNow() != ipc::SyncOutcome::kAccepted) {
    c.Expect(false, what + ": Sync now starts a tick");
    return false;
  }
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{120};
  while ((ticks.ticks() <= before || console.Status().sync_in_progress) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  const ipc::Status status = console.Status();
  const bool ran = ticks.ticks() > before && !status.sync_in_progress;
  c.Expect(ran && status.last_sync_result == ipc::SyncResult::kOk,
           what + ": the tick ran clean -- " + ipc::ToString(status.last_sync_result) +
               Rendered(console.Log()));
  return ran;
}

/// #236's interop acceptance, through the real engine: RetroArch's Cloud Sync
/// writes `saves/mGBA/<rom>.srm` and the console's Tico folder gets it as
/// `<rom>.srm`; the console's next save is what RetroArch is then served.
int Interop(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  harness::Rom rom;
  c.Expect(harness::FindRom(client, base, fixture, "240pee_mb.gba", &rom),
           "the fixture library has 240pee_mb.gba");
  if (rom.id == 0) {
    return c.failures();
  }
  ClearSlots(client, base, fixture, rom.id);
  const std::string device_id = harness::CreateDevice(c, client, base, fixture, "engine-interop");
  if (device_id.empty()) {
    return c.failures();
  }
  // Unique bytes per run: RomM remembers what a slot lost, and a client holding
  // a deleted version's bytes is told `delete`.
  const std::string stamp = harness::UniqueSlot("interop");

  rlog::Reset();
  const TickLines ticks;
  Console console(c, "engine-interop");
  if (!PointAtTheRig(console, c, base, fixture, device_id)) {
    return c.failures();
  }
  const std::string save = "/tico/saves/gba/240pee_mb.srm";
  console.sandbox.MakeDirs("/tico/saves/gba");
  const std::string older = "the switch's save, before RetroArch played " + stamp + "\n";
  console.sandbox.SeedSave(save, older);

  harness::PassASecond();
  const std::string retroarch = "RetroArch's progress, synced by Cloud Sync " + stamp + "\n";
  const http::Result put =
      AsRetroArch(client, base, http::Method::kPut, "saves/mGBA/240pee_mb.srm", retroarch);
  c.Expect(put.successful(), "RetroArch's Cloud Sync stores its save: HTTP " +
                                 std::to_string(put.response.status));

  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.Boot();
  console.engine.UseServer(&client, fixture.token);
  console.engine.UseCard(card.get());
  console.engine.StartWorker();

  // RetroArch -> Switch.
  TickOnce(console, ticks, c, "RetroArch to Switch");
  c.ExpectEq(console.sandbox.Read(save), retroarch,
             "RetroArch's save is in Tico's gba folder, under Tico's own name");
  c.Expect(console.sandbox.HasBackupOf(older), "with the Switch's own bytes under .backup/");
  const std::vector<ServerRow> after_download = RowsFor(client, base, fixture, rom.id, "autosave");
  c.ExpectEq(after_download.size(), std::size_t{1},
             "and nothing was uploaded: the autosave slot holds RetroArch's save alone");

  // Switch -> RetroArch.
  harness::PassASecond();
  const std::string played = "the switch played on from there " + stamp + "\n";
  console.sandbox.Reseed(save, played);
  TickOnce(console, ticks, c, "Switch to RetroArch");
  const http::Result served =
      AsRetroArch(client, base, http::Method::kGet, "saves/mGBA/240pee_mb.srm");
  c.Expect(served.successful() && served.response.body == played,
           "RetroArch is served the Switch's save at its own path: HTTP " +
               std::to_string(served.response.status) + " -- " + served.response.body);
  bool uploaded = false;
  for (const ServerRow& row : RowsFor(client, base, fixture, rom.id, "autosave")) {
    if (row.content_hash == crypto::Md5Hex(played)) {
      uploaded = true;
      c.ExpectEq(row.emulator, std::string("mgba"), "uploaded under mGBA's core id");
      c.Expect(row.file_name.rfind("240pee_mb [", 0) == 0 &&
                   row.file_name.size() > 4 &&
                   row.file_name.compare(row.file_name.size() - 4, 4, ".srm") == 0,
               "under the rom's own stem: " + row.file_name);
    }
  }
  c.Expect(uploaded, "the Switch's save is a version in the autosave slot");

  ClearSlots(client, base, fixture, rom.id);
  harness::DeleteDevice(client, base, fixture, device_id);
  return c.failures();
}

/// `POST /api/saves` as rc1 sent one: its own `tico-srm` slot, `emulator=tico`,
/// this device's sync row written.
bool UploadAsRc1(http::HttpClient& client, const std::string& base,
                 const harness::Fixture& fixture, const std::string& device_id,
                 std::int64_t rom_id, const std::string& host_path, const std::string& name) {
  http::Request request = harness::Authed(
      http::Method::kPost,
      base + "/api/saves?rom_id=" + std::to_string(rom_id) +
          "&emulator=tico&slot=tico-srm&overwrite=true&device_id=" + device_id,
      fixture);
  http::FormPart part;
  part.name = "saveFile";
  part.file_path = host_path;
  part.file_name = name;
  part.content_type = "application/octet-stream";
  request.form.push_back(part);
  return client.Send(request).successful();
}

/// True when conflicts.db has a conflict row for `rom_id`.
bool ConflictRecorded(const Console& console, std::int64_t rom_id) {
  const std::string text = console.sandbox.Read("/config/rommsync/conflicts.db");
  const std::string rom = "\"rom_id\":" + std::to_string(rom_id) + ",";
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = text.find('\n', at);
    const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (line.find(rom) != std::string::npos && line.find("\"event\":\"conflict\"") != std::string::npos) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    at = end + 1;
  }
  return false;
}

/// #236's upgrade acceptance: a console rc1 synced, with rc1's own `state.db`
/// (version 2, rows in `tico-srm`) and rc1's uploads on the server, runs its
/// first ticks on this build. Three roms, the three ways the shared slot can be:
///
///   - nova.nes: empty, so the save is uploaded into it -- once;
///   - 240pee.nes: holding RetroArch's progress, newer than the card's, which is
///     downloaded over it with the card's bytes backed up first;
///   - gb240p.gb: holding RetroArch's progress, *older* than the card's: the
///     upload is refused by the slot guard (this device never synced that slot),
///     and the 409 is a conflict -- RetroArch's copy on the card, the Switch's
///     under `.backup/` and still on the server in `tico-srm`, and an entry on
///     the conflicts screen. Not silently a download: see `AdoptLegacySaves`.
///
/// No data is lost (every byte the card held is on the card, under `.backup/` or
/// on the server), rc1's rows stay where they are, and the second tick uploads
/// nothing.
int Upgrade(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  struct Game {
    const char* rom_file;
    const char* folder;
    const char* core_folder;
    std::int64_t rom_id = 0;
    std::string save;
    std::string bytes;
    std::string retroarch;
  };
  Game games[] = {{"nova.nes", "/tico/saves/nes", nullptr, 0, {}, {}, {}},
                  {"240pee.nes", "/tico/saves/nes", "FCEUmm", 0, {}, {}, {}},
                  {"gb240p.gb", "/tico/saves/gb", "Gambatte", 0, {}, {}, {}}};
  Game& empty = games[0];
  Game& newer = games[1];
  Game& older = games[2];
  for (Game& game : games) {
    harness::Rom rom;
    c.Expect(harness::FindRom(client, base, fixture, game.rom_file, &rom),
             std::string("the fixture library has ") + game.rom_file);
    if (rom.id == 0) {
      return c.failures();
    }
    game.rom_id = rom.id;
    ClearSlots(client, base, fixture, rom.id);
    const std::string stem(game.rom_file, std::string_view(game.rom_file).find('.'));
    game.save = std::string(game.folder) + "/" + stem + ".srm";
  }
  const std::string device_id = harness::CreateDevice(c, client, base, fixture, "engine-upgrade");
  if (device_id.empty()) {
    return c.failures();
  }
  const std::string stamp = harness::UniqueSlot("upgrade");

  rlog::Reset();
  const TickLines ticks;
  Console console(c, "engine-upgrade");
  if (!PointAtTheRig(console, c, base, fixture, device_id)) {
    return c.failures();
  }
  console.sandbox.MakeDirs("/tico/saves/nes");
  console.sandbox.MakeDirs("/tico/saves/gb");

  // RetroArch played gb240p before the Switch last did.
  older.retroarch = "RetroArch's gb240p, older than the Switch's " + stamp + "\n";
  c.Expect(AsRetroArch(client, base, http::Method::kPut, "saves/Gambatte/gb240p.srm",
                       older.retroarch)
               .successful(),
           "RetroArch's gb240p is in the shared slot");
  harness::PassASecond();

  // rc1's sync: every save on the card, uploaded into rc1's own slot.
  for (Game& game : games) {
    game.bytes = std::string("rc1 synced ") + game.rom_file + " " + stamp + "\n";
    console.sandbox.SeedSave(game.save, game.bytes);
    c.Expect(UploadAsRc1(client, base, fixture, device_id, game.rom_id,
                         console.sandbox.Host(game.save), game.save.substr(game.save.rfind('/') + 1)),
             std::string("rc1 uploaded ") + game.rom_file + " into tico-srm");
  }

  // RetroArch played 240pee after the Switch last did.
  harness::PassASecond();
  newer.retroarch = "RetroArch's 240pee, newer than the Switch's " + stamp + "\n";
  c.Expect(AsRetroArch(client, base, http::Method::kPut, "saves/FCEUmm/240pee.srm",
                       newer.retroarch)
               .successful(),
           "RetroArch's 240pee is in the shared slot");

  // rc1's state.db, in rc1's format: each save's facts, as rc1 read them off the
  // card, and rc1's upload's digest as the server's.
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  std::string v2 = "rommsync-state 2\n";
  for (const Game& game : games) {
    const fs::Listing listing = card->List(game.folder);
    for (const fs::Entry& entry : listing.entries) {
      if (game.save == std::string(game.folder) + "/" + entry.name) {
        v2 += "{\"rom_id\":" + std::to_string(game.rom_id) +
              ",\"slot\":\"tico-srm\",\"content_hash\":\"" + crypto::Md5Hex(game.bytes) +
              "\",\"mtime\":" + std::to_string(entry.modified_unix) +
              ",\"file_size_bytes\":" + std::to_string(entry.size_bytes) +
              ",\"server_updated_at\":null,\"server_content_hash\":\"" +
              crypto::Md5Hex(game.bytes) + "\"}\n";
      }
    }
  }
  c.Expect(console.sandbox.Write("/config/rommsync/state.db", v2), "rc1's state.db is on the card");

  console.Boot();
  console.engine.UseServer(&client, fixture.token);
  console.engine.UseCard(card.get());
  console.engine.StartWorker();
  TickOnce(console, ticks, c, "the first tick after the upgrade");

  const std::vector<ServerRow> empty_rows = RowsFor(client, base, fixture, empty.rom_id, "autosave");
  c.Expect(empty_rows.size() == 1 && empty_rows[0].content_hash == crypto::Md5Hex(empty.bytes) &&
               empty_rows[0].emulator == "fceumm",
           "nova: an empty shared slot gets the save, once, under its core's id");
  c.ExpectEq(console.sandbox.Read(empty.save), empty.bytes, "nova: and the card is untouched");

  c.ExpectEq(console.sandbox.Read(newer.save), newer.retroarch,
             "240pee: RetroArch's newer progress is on the card");
  c.Expect(console.sandbox.HasBackupOf(newer.bytes), "240pee: the Switch's bytes under .backup/");
  c.ExpectEq(RowsFor(client, base, fixture, newer.rom_id, "autosave").size(), std::size_t{1},
             "240pee: and nothing was uploaded over it");
  c.Expect(!ConflictRecorded(console, newer.rom_id), "240pee: a download, not a conflict");

  c.ExpectEq(console.sandbox.Read(older.save), older.retroarch,
             "gb240p: the server's head is on the card, as for any conflict");
  c.Expect(console.sandbox.HasBackupOf(older.bytes),
           "gb240p: the Switch's newer progress under .backup/");
  c.ExpectEq(RowsFor(client, base, fixture, older.rom_id, "autosave").size(), std::size_t{1},
             "gb240p: nothing was uploaded over RetroArch's save");
  c.Expect(ConflictRecorded(console, older.rom_id),
           "gb240p: and it is on the conflicts screen, restorable:\n" +
               console.sandbox.Read("/config/rommsync/conflicts.db"));
  // #240: the Switch's progress is on the server deliberately, not only by
  // rc1's `tico-srm` row happening to hold it: an archival row, outside every
  // slot, and the conflicts screen knows which.
  const auto archived_copies = [&]() {
    std::vector<harness::ArchivalRow> copies;
    for (harness::ArchivalRow& row : harness::ArchivalRows(client, base, fixture, older.rom_id)) {
      if (row.content_hash == crypto::Md5Hex(older.bytes)) {
        copies.push_back(std::move(row));
      }
    }
    return copies;
  };
  std::vector<harness::ArchivalRow> archived = archived_copies();
  c.ExpectEq(archived.size(), std::size_t{1},
             "gb240p: the Switch's progress is on the server as one archival row");
  c.Expect(console.sandbox.Read("/config/rommsync/conflicts.db")
                   .find("\"local_copy\":\"server\"") != std::string::npos,
           "gb240p: and the conflicts screen says the server holds it");

  for (const Game& game : games) {
    const std::vector<ServerRow> old_rows = RowsFor(client, base, fixture, game.rom_id, "tico-srm");
    c.Expect(old_rows.size() == 1 && old_rows[0].content_hash == crypto::Md5Hex(game.bytes),
             std::string(game.rom_file) + ": rc1's row is still in tico-srm, as rc1 left it");
  }
  const std::string state_db = console.sandbox.Read("/config/rommsync/state.db");
  c.Expect(state_db.rfind("rommsync-state 3\n", 0) == 0 &&
               state_db.find("\"tico-srm\"") == std::string::npos,
           "state.db is version 3, with nothing left under the old slot:\n" + state_db);

  // The second tick: nothing to send.
  TickOnce(console, ticks, c, "the second tick");
  for (const Game& game : games) {
    c.ExpectEq(RowsFor(client, base, fixture, game.rom_id, "autosave").size(), std::size_t{1},
               std::string(game.rom_file) + ": no duplicate upload on the second tick");
  }
  c.ExpectEq(console.sandbox.Read(empty.save), empty.bytes, "nova: unchanged");
  c.ExpectEq(console.sandbox.Read(newer.save), newer.retroarch, "240pee: unchanged");
  c.ExpectEq(console.sandbox.Read(older.save), older.retroarch, "gb240p: unchanged");
  archived = archived_copies();
  c.ExpectEq(archived.size(), std::size_t{1}, "gb240p: and archived once, not once a tick");

  for (const Game& game : games) {
    ClearSlots(client, base, fixture, game.rom_id);
  }
  for (const harness::ArchivalRow& row : archived) {
    harness::DeleteSave(client, base, fixture, row.id);
  }
  harness::DeleteDevice(client, base, fixture, device_id);
  return c.failures();
}

/// #237: a save this console has never held, restored by a real tick. It lands
/// in Tico's folder for the system under the rom's own name -- not in
/// `/retroarch/saves` under RomM's datetime-tagged one -- and the next tick
/// reads it back as the save it came from, so nothing goes up again.
int Placement(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  harness::Rom rom;
  c.Expect(harness::FindRom(client, base, fixture, "nova.nes", &rom),
           "the fixture library has nova.nes");
  if (rom.id == 0) {
    return c.failures();
  }
  ClearSlots(client, base, fixture, rom.id);
  const std::string device_id =
      harness::CreateDevice(c, client, base, fixture, "engine-placement");
  if (device_id.empty()) {
    return c.failures();
  }

  // Another device's save, as it reaches RomM: the shared slot, FCEUmm's id,
  // and the datetime tag RomM stamps on the stored name. Unique bytes per run:
  // RomM remembers what a slot lost.
  const std::string server_bytes =
      "nova's save, from another device " + harness::UniqueSlot("placement") + "\n";
  harness::Save server;
  {
    harness::Sandbox staging(c, "engine-placement-upload");
    const std::string staged = staging.Host("/config/rommsync/server-copy.srm");
    rig::WriteFile(staged, server_bytes);
    c.Expect(harness::UploadSave(client, base, fixture, rom.id, "autosave", "fceumm", staged,
                                 "nova.srm", /*with_device=*/false, &server),
             "another device's save is on the server");
  }
  const std::vector<std::int64_t> before = SaveIdsFor(client, base, fixture, rom.id);

  rlog::Reset();
  const TickLines ticks;
  Console console(c, "engine-placement");
  if (!PointAtTheRig(console, c, base, fixture, device_id)) {
    return c.failures();
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&client, std::string());
  console.Boot();
  console.engine.StartWorker();

  // Counted at the sink (`TickLines`). Partial is a tick that reached the
  // server too: whatever else the shared fixture offers is not this test's.
  const auto tick = [&console, &ticks, &c](int number) {
    const int before_tick = ticks.ticks();
    c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted, "a tick is asked for");
    const std::chrono::steady_clock::time_point by =
        std::chrono::steady_clock::now() + std::chrono::seconds{120};
    while ((ticks.ticks() <= before_tick || console.Status().sync_in_progress) &&
           std::chrono::steady_clock::now() < by) {
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    const ipc::Status status = console.Status();
    c.Expect(ticks.ticks() > before_tick &&
                 (status.last_sync_result == ipc::SyncResult::kOk ||
                  status.last_sync_result == ipc::SyncResult::kPartial),
             "tick " + std::to_string(number) + " ran and reached the server: " +
                 ipc::ToString(status.last_sync_result));
  };

  // Not while the card does not hold the game (#241): negotiate names only the
  // roms on the card, so the save is not offered, let alone placed.
  const std::string placed = "/tico/saves/nes/nova.srm";
  tick(1);
  c.Expect(!console.sandbox.Exists(placed),
           "no save is placed for a rom the card does not hold" + Rendered(console.Log()));

  console.sandbox.Write("/tico/roms/nes/nova.nes", "nova's rom\n");
  tick(2);
  c.ExpectEq(console.sandbox.Read(placed), server_bytes,
             "the save is in Tico's nes folder under the rom's name" + Rendered(console.Log()));
  for (const char* folder : {"/retroarch/saves", "/retroarch/cores/savefiles"}) {
    std::error_code error;
    const std::filesystem::path real = console.sandbox.Host(folder);
    bool empty = true;
    for (auto it = std::filesystem::recursive_directory_iterator(real, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
      empty = empty && !it->is_regular_file();
    }
    c.Expect(empty, std::string("and nothing was written under ") + folder);
  }
  c.Expect(!console.sandbox.Exists("/tico/saves/nes/" + server.file_name),
           "nor under the server's tagged name");

  // The next tick reads it back as the save it came from: nothing to upload.
  tick(3);
  c.ExpectEq(SaveIdsFor(client, base, fixture, rom.id).size(), before.size(),
             "the placed save is the server's own, so no tick sends it back as another");
  c.ExpectEq(console.sandbox.Read(placed), server_bytes, "and it is left as it is");

  ClearSlots(client, base, fixture, rom.id);
  harness::DeleteDevice(client, base, fixture, device_id);
  return c.failures();
}

/// The lines one slot's save is named in, and how each tick ended -- counted at
/// the sink, for `TickLines`' reason.
class SlotLines final : public rlog::Sink {
 public:
  explicit SlotLines(std::string slot) : slot_(std::move(slot)), previous_(rlog::GetSink()) {
    rlog::SetSink(this);
  }
  ~SlotLines() override { rlog::SetSink(previous_); }

  void Write(rlog::Level, std::string_view line) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (line.find(" sync.tick ") != std::string_view::npos) {
      ticks_.emplace_back(line);
    } else if (line.find(slot_) != std::string_view::npos) {
      named_.emplace_back(line);
    }
  }

  std::vector<std::string> ticks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ticks_;
  }
  std::vector<std::string> named() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return named_;
  }

 private:
  const std::string slot_;
  rlog::Sink* previous_;
  mutable std::mutex mutex_;
  std::vector<std::string> ticks_;
  std::vector<std::string> named_;
};

/// #257, through the real engine: a save the placer will not place -- another
/// device's, in a slot the scan would not read it back in -- costs one log line
/// on the first tick that meets it, none on the second, is never written as a
/// failed save, and neither tick is partial on its account.
int Declined(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  harness::Rom rom;
  c.Expect(harness::FindRom(client, base, fixture, "nova.nes", &rom),
           "the fixture library has nova.nes");
  if (rom.id == 0) {
    return c.failures();
  }
  ClearSlots(client, base, fixture, rom.id);
  const std::string device_id = harness::CreateDevice(c, client, base, fixture, "engine-declined");
  if (device_id.empty()) {
    return c.failures();
  }

  // A `.srm` the scan would read back in `autosave`, kept in a slot of its own:
  // placed, it would be a second save of the game (`place::Placer::Save`).
  const std::string slot = harness::UniqueSlot("m10-21");
  harness::Save server;
  {
    harness::Sandbox staging(c, "engine-declined-upload");
    const std::string staged = staging.Host("/config/rommsync/server-copy.srm");
    rig::WriteFile(staged, "nova's save, in a slot of its own " + slot + "\n");
    c.Expect(harness::UploadSave(client, base, fixture, rom.id, slot, "fceumm", staged,
                                 "nova.srm", /*with_device=*/false, &server),
             "another device's save is on the server");
  }

  rlog::Reset();
  const SlotLines lines(slot);
  Console console(c, "engine-declined");
  if (!PointAtTheRig(console, c, base, fixture, device_id)) {
    harness::DeleteSave(client, base, fixture, server.id);
    return c.failures();
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&client, std::string());
  console.sandbox.Write("/tico/roms/nes/nova.nes", "nova's rom\n");
  console.Boot();
  console.engine.StartWorker();

  const auto tick = [&console, &lines, &c](std::size_t number) {
    c.Expect(console.SyncNow() == ipc::SyncOutcome::kAccepted, "a tick is asked for");
    const std::chrono::steady_clock::time_point by =
        std::chrono::steady_clock::now() + std::chrono::seconds{120};
    while ((lines.ticks().size() < number || console.Status().sync_in_progress) &&
           std::chrono::steady_clock::now() < by) {
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    const std::vector<std::string> ended = lines.ticks();
    c.Expect(ended.size() == number && ended.back().find("outcome=completed") != std::string::npos,
             "tick " + std::to_string(number) + " completed, not partial: " +
                 (ended.empty() ? std::string("no tick") : ended.back()) +
                 Rendered(console.Log()));
  };
  const auto failed_lines = [&lines]() {
    std::size_t failed = 0;
    for (const std::string& line : lines.named()) {
      failed += line.find(" save.failed ") != std::string::npos ? 1 : 0;
    }
    return failed;
  };

  tick(1);
  std::vector<std::string> named = lines.named();
  c.ExpectEq(named.size(), std::size_t{1},
             "the first tick says it once" + Rendered(console.Log()));
  c.Expect(!named.empty() && named[0].find(" warn ") != std::string::npos,
           "as a warning, not an error: " + (named.empty() ? std::string() : named[0]));
  c.ExpectEq(failed_lines(), std::size_t{0}, "and never as a failed save");
  c.ExpectEq(console.Status().failed, std::int64_t{0}, "the status screen counts no failure");
  c.Expect(!console.sandbox.Exists("/tico/saves/nes/nova.srm"), "and nothing was placed");

  tick(2);
  c.ExpectEq(lines.named().size(), std::size_t{1},
             "the second tick says nothing more" + Rendered(console.Log()));
  c.ExpectEq(console.Status().failed, std::int64_t{0}, "and counts no failure either");

  harness::DeleteSave(client, base, fixture, server.id);
  ClearSlots(client, base, fixture, rom.id);
  harness::DeleteDevice(client, base, fixture, device_id);
  return c.failures();
}

/// #241's acceptance, through the real engine: the server holds a save, the
/// console downloads the rom, and the save is in the folder the emulator reads
/// before the next scheduled tick -- of which there is none: `interval_min = 0`
/// and no boot tick, so the only tick that can run is the one the download asks
/// for.
int DownloadRestores(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  harness::Rom rom;
  c.Expect(harness::FindRom(client, base, fixture, "nova.nes", &rom),
           "the fixture library has nova.nes");
  if (rom.id == 0) {
    return c.failures();
  }
  ClearSlots(client, base, fixture, rom.id);
  const std::string device_id =
      harness::CreateDevice(c, client, base, fixture, "engine-download-restores");
  if (device_id.empty()) {
    return c.failures();
  }

  const std::string server_bytes =
      "nova's save, waiting for the rom " + harness::UniqueSlot("restores") + "\n";
  {
    harness::Sandbox staging(c, "engine-download-restores-upload");
    const std::string staged = staging.Host("/config/rommsync/server-copy.srm");
    rig::WriteFile(staged, server_bytes);
    harness::Save server;
    c.Expect(harness::UploadSave(client, base, fixture, rom.id, "autosave", "fceumm", staged,
                                 "nova.srm", /*with_device=*/false, &server),
             "another device's save is on the server");
  }

  rlog::Reset();
  const TickLines ticks;
  Console console(c, "engine-download-restores");
  if (!PointAtTheRig(console, c, base, fixture, device_id)) {
    return c.failures();
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&client, std::string());
  console.Boot();
  console.engine.StartWorker();

  // Nothing is on the card, and nothing ticks on its own.
  std::this_thread::sleep_for(std::chrono::milliseconds{300});
  c.ExpectEq(ticks.ticks(), 0, "a console with no schedule does not tick by itself");

  std::int32_t position = 0;
  c.Expect(console.Enqueue(rom.id, &position) == ipc::Error::kOk, "the overlay queues the rom");
  const std::string placed = "/tico/saves/nes/nova.srm";
  const std::chrono::steady_clock::time_point by =
      std::chrono::steady_clock::now() + std::chrono::seconds{120};
  while (console.sandbox.Read(placed) != server_bytes && std::chrono::steady_clock::now() < by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }
  c.Expect(console.sandbox.Exists("/tico/roms/nes/nova.nes"), "the rom was downloaded");
  c.ExpectEq(console.sandbox.Read(placed), server_bytes,
             "and its save is in Tico's nes folder, under the rom's name, with no tick asked "
             "for" + Rendered(console.Log()));
  // The tick says so as it ends, which is after the save is placed. Settled
  // before the cleanup, too, so the tick is not cut off half way.
  while ((ticks.ticks() < 1 || console.Status().sync_in_progress) &&
         std::chrono::steady_clock::now() < by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  c.ExpectEq(ticks.ticks(), 1, "brought by the one tick the download asked for");
  ClearSlots(client, base, fixture, rom.id);
  harness::DeleteDevice(client, base, fixture, device_id);
  return c.failures();
}

/// #245's acceptance: a rom queued for this console in RomM's web UI is
/// claimed on a tick, downloaded through the queue, and reported done.
///
/// A device of its own, paired for real: the install endpoints answer only to
/// the token bound to the device, and the fixture's device is every rig
/// process's.
int RemoteInstall(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  harness::Rom rom;
  c.Expect(harness::FindRom(client, base, fixture, "nova.nes", &rom),
           "the fixture library has nova.nes");
  const std::string name = "engine-remote-install";
  const auth::StoredToken token = harness::PairDevice(c, client, base, name);
  if (rom.id == 0 || token.device_id.empty()) {
    return c.failures();
  }
  // A re-run adopts its row: end whatever an earlier one left taken, and
  // withdraw the capability, so the first tick is what registers it.
  for (const install::Request& left : install::Claim(client, base, token).page.requests) {
    install::Report(client, base, token, left.id, {install::Outcome::kFailed, "an earlier run"});
  }
  install::Register(client, base, token, false);
  const auto queue_in_browser = [&] {
    return rig::AsTheUser(client, base, "/api/devices/" + token.device_id + "/installs",
                          "{\"rom_id\":" + std::to_string(rom.id) + "}");
  };
  c.ExpectEq(queue_in_browser().response.status, 400,
             "before the console has ticked, the web UI cannot queue for it");

  rlog::Reset();
  const TickLines ticks;
  Console console(c, name);
  if (!console.sandbox.Write("/config/rommsync/config.ini",
                             "[server]\nurl = " + base +
                                 "\n\n[sync]\nenabled = true\ninterval_min = 0\n"
                                 "on_boot = false\nstates = false\n") ||
      !auth::SaveToken(console.directory + auth::kTokenFileName, token).ok()) {
    c.Expect(false, "the console is configured and paired as its own device");
    return c.failures();
  }
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&client, std::string());
  console.Boot();
  console.engine.StartWorker();

  // Where RomM records what a device reported: the request itself is gone
  // the moment it ends. Only what is newer than this run's start counts: an
  // earlier run left its own, and RomM's cap of 200 evicts the oldest, so a
  // before-and-after count can miss the one this run added (`rig::LatestNotificationId`).
  const std::int64_t started = rig::LatestNotificationId(client, base);
  const auto notices = [&](const std::string& status) {
    return rig::InstallNoticesSince(client, base, started, "rommsync-nx " + name, status);
  };

  TickOnce(console, ticks, c, "the first tick");
  const http::Result queued = queue_in_browser();
  c.ExpectEq(queued.response.status, 201,
             "after it, the console accepts remote installs and the web UI queues nova: " +
                 queued.response.body.substr(0, 200));

  // Claimed by the next tick, then downloaded by the queue, then reported.
  TickOnce(console, ticks, c, "the tick that claims");
  const std::chrono::steady_clock::time_point by =
      std::chrono::steady_clock::now() + std::chrono::seconds{120};
  const auto open = [&] {
    // The device's own claim takes nothing new -- the console already took it
    // -- and lists what is still taken, which a report ends.
    return install::Claim(client, base, token).page.requests.size();
  };
  while ((!console.sandbox.Exists("/tico/roms/nes/nova.nes") || open() != 0) &&
         std::chrono::steady_clock::now() < by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  c.Expect(console.sandbox.Exists("/tico/roms/nes/nova.nes"),
           "the rom the browser queued is on the card" + Rendered(console.Log()));
  c.ExpectEq(open(), std::size_t{0}, "and the request is ended, not left taken");

  c.ExpectEq(notices("done"), std::size_t{1},
             "RomM was told it is done, and told the user so");
  c.Expect(Logged(console.Log(), "download.remote"), "the console's log says so too");

  // The download asks for a tick of its own (#241); let it end before teardown.
  while ((ticks.ticks() < 3 || console.Status().sync_in_progress) &&
         std::chrono::steady_clock::now() < by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }

  // Downloads switched off with a request outstanding: nothing here will fetch
  // it, so the browser is told so rather than left waiting out its expiry, and
  // stops offering the console.
  c.ExpectEq(queue_in_browser().response.status, 201, "the web UI queues nova again");
  c.Expect(console.Set(Edit("downloads", "enabled", "false")).outcome ==
               ipc::WriteOutcome::kApplied,
           "and downloads are switched off on the console");
  TickOnce(console, ticks, c, "the tick after downloads went off");
  c.ExpectEq(open(), std::size_t{0}, "that request is ended too");
  c.ExpectEq(notices("failed"), std::size_t{1}, "as failed, which the user is told");
  c.ExpectEq(queue_in_browser().response.status, 400,
             "and the web UI no longer offers this console");
  harness::DeleteDevice(client, base, fixture, token.device_id);
  return c.failures();
}

/// The real client with a hook in front of it, which may answer a request
/// itself, and a record of every request that went past.
///
/// For the answers a healthy RomM will not give on demand, aimed at exactly one
/// endpoint: a 401 on the negotiate alone, a link that is down for one call, a
/// claim refused once. The fault proxy matches a path too, but counts its uses
/// across every client of the rig; this belongs to the one console it wraps.
class Intercepting final : public http::HttpClient {
 public:
  using Hook = std::function<std::optional<http::Result>(const http::Request&)>;

  explicit Intercepting(http::HttpClient& inner) : inner_(inner) {}

  void Use(Hook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    hook_ = std::move(hook);
  }

  http::Result Send(const http::Request& request) override {
    if (std::optional<http::Result> answered = Note(request)) {
      return *answered;
    }
    return inner_.Send(request);
  }

  http::Result Download(const http::Request& request,
                        const http::DownloadTarget& target) override {
    if (std::optional<http::Result> answered = Note(request)) {
      return *answered;
    }
    return inner_.Download(request, target);
  }

  /// The URLs asked so far with `method`, containing `fragment`, in order.
  std::vector<std::string> Asked(http::Method method, std::string_view fragment) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [asked_with, url] : seen_) {
      if (asked_with == method && url.find(fragment) != std::string::npos) {
        out.push_back(url);
      }
    }
    return out;
  }

  std::size_t Count(http::Method method, std::string_view fragment) const {
    return Asked(method, fragment).size();
  }

 private:
  std::optional<http::Result> Note(const http::Request& request) {
    Hook hook;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      seen_.emplace_back(request.method, request.url);
      hook = hook_;
    }
    return hook ? hook(request) : std::nullopt;
  }

  http::HttpClient& inner_;
  mutable std::mutex mutex_;
  Hook hook_;
  std::vector<std::pair<http::Method, std::string>> seen_;
};

/// A status from "the server", with a body that is none of RomM's shapes.
http::Result Answered(int status) {
  http::Result result;
  result.response.status = status;
  result.response.body = "{\"detail\":\"answered by the test\"}";
  return result;
}

/// No response at all.
http::Result Unreachable() {
  http::Result result;
  result.error = http::Error::kConnectFailed;
  result.message = "the test took the link down for this request";
  return result;
}

bool Asks(const http::Request& request, http::Method method, std::string_view fragment) {
  return request.method == method && request.url.find(fragment) != std::string::npos;
}

/// Press Sync now and wait for that tick to end, however it ends.
bool TickEnds(Console& console, const TickLines& ticks, rig::Checks& c, const std::string& what) {
  const int before = ticks.ticks();
  if (console.SyncNow() != ipc::SyncOutcome::kAccepted) {
    c.Expect(false, what + ": Sync now starts a tick");
    return false;
  }
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{120};
  while ((ticks.ticks() <= before || console.Status().sync_in_progress) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  const bool ended = ticks.ticks() > before && !console.Status().sync_in_progress;
  c.Expect(ended, what + ": the tick ended" + Rendered(console.Log()));
  return ended;
}

/// The states half of a tick, held to what the saves half found (review of
/// #242, #235, #36): it does not run under a game that started during the saves
/// half, nor after a saves half the server refused or never answered -- and a
/// 401 it meets itself counts towards `auth::Gate`'s verdict like any other.
///
/// Each case is its own console and its own device: the gate remembers.
int StatesGuarded(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  harness::Rom rom;
  c.Expect(harness::FindRom(client, base, fixture, "nova.nes", &rom),
           "the fixture library has nova.nes");
  if (rom.id == 0) {
    return c.failures();
  }
  ClearSlots(client, base, fixture, rom.id);

  // States on, and a rom on the card, so the saves half has a negotiate to
  // make (#241 scopes it to the roms the card holds).
  const auto set_up = [&](Console& console, const std::string& device_id) {
    auth::StoredToken token;
    token.server_url = base;
    token.access_token = fixture.token;
    token.device_id = device_id;
    token.scopes = {"roms.read"};
    const bool ok = console.sandbox.Write("/config/rommsync/config.ini",
                                          "[server]\nurl = " + base +
                                              "\n\n[sync]\nenabled = true\ninterval_min = 0\n"
                                              "on_boot = false\nstates = true\n") &&
                    auth::SaveToken(console.directory + auth::kTokenFileName, token).ok() &&
                    console.sandbox.Write("/tico/roms/nes/nova.nes", "nova's rom\n");
    c.Expect(ok, console.directory + ": configured, paired, and holding nova");
    return ok;
  };

  // **A game launched while the saves half runs.** The worker asked before the
  // tick and the tick asked again after the library; the game starts as the
  // saves are negotiated. The states half is a second round of transfers, and
  // it asks again before it starts.
  {
    const std::string device_id =
        harness::CreateDevice(c, client, base, fixture, "engine-states-held");
    rlog::Reset();
    const TickLines ticks;
    Console console(c, "engine-states-held");
    const std::unique_ptr<fs::FileSystem> card = Card(console);
    if (device_id.empty() || !set_up(console, device_id)) {
      return c.failures();
    }
    Intercepting wire(client);
    std::atomic<bool> running{false};
    std::atomic<bool> armed{true};
    wire.Use([&](const http::Request& request) -> std::optional<http::Result> {
      if (Asks(request, http::Method::kPost, "/api/sync/negotiate") && armed.exchange(false)) {
        running = true;
      }
      return std::nullopt;
    });
    console.Boot();
    console.engine.UseForegroundProbe(
        [&running] {
          play::Foreground foreground;
          foreground.running = running.load();
          return foreground;
        },
        std::chrono::milliseconds{50});
    console.engine.UseServer(&wire, std::string());
    console.engine.UseCard(card.get());
    console.engine.StartWorker();

    TickEnds(console, ticks, c, "held: the tick the game starts under");
    c.Expect(running.load(), "held: the game started as the saves were negotiated");
    c.ExpectEq(wire.Count(http::Method::kGet, "/api/states"), std::size_t{0},
               "held: the states half did not run under it -- no state listed, none sent "
               "or placed" + Rendered(console.Log()));
    c.Expect(Logged(console.Log(), "sync.held"),
             "held: and the log says why" + Rendered(console.Log()));
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    c.ExpectEq(ticks.ticks(), 1, "held: nothing more runs while the game is up");

    running = false;
    const std::chrono::steady_clock::time_point by =
        std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while ((ticks.ticks() < 2 || console.Status().sync_in_progress) &&
           std::chrono::steady_clock::now() < by) {
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    c.Expect(wire.Count(http::Method::kGet, "/api/states") > 0,
             "held: once it closes, a tick runs by itself and syncs the states" +
                 Rendered(console.Log()));
    harness::DeleteDevice(client, base, fixture, device_id);
  }

  // **A saves half the server refused, or never answered.** States have no
  // negotiate of their own: a 401 there is the same token, and a link that is
  // down is down for `/api/states` too.
  {
    const std::string device_id =
        harness::CreateDevice(c, client, base, fixture, "engine-states-refused");
    rlog::Reset();
    const TickLines ticks;
    Console console(c, "engine-states-refused");
    const std::unique_ptr<fs::FileSystem> card = Card(console);
    if (device_id.empty() || !set_up(console, device_id)) {
      return c.failures();
    }
    Intercepting wire(client);
    std::atomic<int> negotiate{401};
    wire.Use([&](const http::Request& request) -> std::optional<http::Result> {
      if (!Asks(request, http::Method::kPost, "/api/sync/negotiate")) {
        return std::nullopt;
      }
      return negotiate.load() == 0 ? Unreachable() : Answered(negotiate.load());
    });
    console.Boot();
    console.engine.UseServer(&wire, std::string());
    console.engine.UseCard(card.get());
    console.engine.StartWorker();

    TickEnds(console, ticks, c, "refused: a negotiate answered 401");
    c.ExpectEq(wire.Count(http::Method::kGet, "/api/states"), std::size_t{0},
               "refused: the states half is not asked with a token the server just refused" +
                   Rendered(console.Log()));
    negotiate = 0;
    TickEnds(console, ticks, c, "offline: a negotiate that never answered");
    c.ExpectEq(wire.Count(http::Method::kGet, "/api/states"), std::size_t{0},
               "offline: nor after a negotiate the link never carried" +
                   Rendered(console.Log()));
    harness::DeleteDevice(client, base, fixture, device_id);
  }

  // **A 401 the states half meets itself.** The token is revoked between the
  // saves and the states: the listing is the first request to hear it, and it
  // is a rejection like the library's. Three in a row is the gate's budget
  // (`auth::GateConfig`), so the console stops on the third tick's -- not on a
  // fourth, as it would if the states half's were not heard.
  {
    const std::string device_id =
        harness::CreateDevice(c, client, base, fixture, "engine-states-revoked");
    rlog::Reset();
    const TickLines ticks;
    Console console(c, "engine-states-revoked");
    const std::unique_ptr<fs::FileSystem> card = Card(console);
    if (device_id.empty() || !set_up(console, device_id)) {
      return c.failures();
    }
    Intercepting wire(client);
    std::atomic<bool> revoked{false};
    wire.Use([&](const http::Request& request) -> std::optional<http::Result> {
      if (Asks(request, http::Method::kGet, "/api/states")) {
        revoked = true;
      }
      // The heartbeat is unauthenticated, and tells the gate nothing.
      if (revoked.load() && request.url.find("/api/heartbeat") == std::string::npos) {
        return Answered(401);
      }
      return std::nullopt;
    });
    console.Boot();
    console.engine.UseServer(&wire, std::string());
    console.engine.UseCard(card.get());
    console.engine.StartWorker();

    TickEnds(console, ticks, c, "revoked: the tick whose states are refused");
    c.Expect(revoked.load(), "revoked: the states half asked, and was refused");
    c.Expect(console.Status().auth != ipc::AuthState::kUnauthenticated,
             "revoked: one rejection is not a verdict");
    TickEnds(console, ticks, c, "revoked: the second rejected tick");
    TickEnds(console, ticks, c, "revoked: the third");
    c.Expect(console.Status().auth == ipc::AuthState::kUnauthenticated,
             "revoked: the third rejection in a row is the verdict -- the states half's "
             "counted as the first" + Rendered(console.Log()));
    c.Expect(console.sandbox.Exists("/config/rommsync/auth.json"),
             "revoked: and it is on the card");
    harness::DeleteDevice(client, base, fixture, device_id);
  }
  ClearSlots(client, base, fixture, rom.id);
  return c.failures();
}

/// A console paired as its own device for the remote-install endpoints, which
/// answer only to the token bound to the device, with whatever an earlier run
/// left taken ended and the capability withdrawn -- so the first tick is the
/// one that registers. Shared by the review's install scenarios.
struct InstallConsole {
  auth::StoredToken token;
  std::int64_t nova = 0;
  std::int64_t other = 0;
};

bool AdoptInstallDevice(rig::Checks& c, http::HttpClient& client, const std::string& base,
                        const harness::Fixture& fixture, const std::string& name,
                        InstallConsole* out) {
  harness::Rom nova;
  harness::Rom other;
  c.Expect(harness::FindRom(client, base, fixture, "nova.nes", &nova),
           "the fixture library has nova.nes");
  c.Expect(harness::FindRom(client, base, fixture, "240pee.nes", &other),
           "and 240pee.nes");
  out->token = harness::PairDevice(c, client, base, name);
  out->nova = nova.id;
  out->other = other.id;
  if (nova.id == 0 || other.id == 0 || out->token.device_id.empty()) {
    return false;
  }
  for (const install::Request& left : install::Claim(client, base, out->token).page.requests) {
    install::Report(client, base, out->token, left.id,
                    {install::Outcome::kFailed, "an earlier run"});
  }
  install::Register(client, base, out->token, false);
  return true;
}

bool ConfigureInstallConsole(rig::Checks& c, Console& console, const std::string& base,
                             const auth::StoredToken& token) {
  const bool ok = console.sandbox.Write("/config/rommsync/config.ini",
                                        "[server]\nurl = " + base +
                                            "\n\n[sync]\nenabled = true\ninterval_min = 0\n"
                                            "on_boot = false\nstates = false\n") &&
                  auth::SaveToken(console.directory + auth::kTokenFileName, token).ok();
  c.Expect(ok, "the console is configured and paired as its own device");
  return ok;
}

/// Wait until `wire` has asked nothing new matching `fragment` for `quiet`,
/// and no tick is running -- the end of whatever passes were under way.
void UntilQuiet(Console& console, const Intercepting& wire, http::Method method,
                std::string_view fragment, std::chrono::milliseconds quiet) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{120};
  std::size_t seen = wire.Count(method, fragment);
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const std::size_t now = wire.Count(method, fragment);
    if (now != seen || console.Status().sync_in_progress) {
      seen = now;
      since = std::chrono::steady_clock::now();
    } else if (std::chrono::steady_clock::now() - since >= quiet) {
      return;
    }
  }
}

/// The reports a console owes RomM's web UI, over a link that will not carry
/// them (review of #245). One report's link failure is every report's: the
/// pass stops at the first rather than spending each held claim's retries on
/// the worker, and the rest are reported once the link carries them.
int InstallSettle(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  const std::string name = "engine-install-settle";
  InstallConsole device;
  if (!AdoptInstallDevice(c, client, base, fixture, name, &device)) {
    return c.failures();
  }
  const auth::StoredToken& token = device.token;
  const auto queue_in_browser = [&](std::int64_t rom_id) {
    return rig::AsTheUser(client, base, "/api/devices/" + token.device_id + "/installs",
                          "{\"rom_id\":" + std::to_string(rom_id) + "}");
  };
  const auto open = [&] { return install::Claim(client, base, token).page.requests.size(); };
  const auto notices = [&](std::int64_t after, const std::string& status) {
    return rig::InstallNoticesSince(client, base, after, "rommsync-nx " + name, status);
  };

  rlog::Reset();
  const TickLines ticks;
  Console console(c, name);
  if (!ConfigureInstallConsole(c, console, base, token)) {
    return c.failures();
  }
  Intercepting wire(client);
  std::atomic<bool> down{false};
  wire.Use([&](const http::Request& request) -> std::optional<http::Result> {
    if (down.load() && Asks(request, http::Method::kPut, "/installs/")) {
      return Unreachable();
    }
    return std::nullopt;
  });
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&wire, std::string());
  console.Boot();
  console.engine.StartWorker();

  TickOnce(console, ticks, c, "the first tick");
  c.ExpectEq(queue_in_browser(device.nova).response.status, 201, "the web UI queues nova");
  c.ExpectEq(queue_in_browser(device.other).response.status, 201, "and 240pee");

  // **The link stops carrying reports**, and nothing else: the claim, the
  // downloads and the ticks all go through.
  const std::int64_t started = rig::LatestNotificationId(client, base);
  down = true;
  TickOnce(console, ticks, c, "the tick that claims both");
  const std::chrono::steady_clock::time_point by =
      std::chrono::steady_clock::now() + std::chrono::seconds{120};
  while ((!console.sandbox.Exists("/tico/roms/nes/nova.nes") ||
          !console.sandbox.Exists("/tico/roms/nes/240pee.nes")) &&
         std::chrono::steady_clock::now() < by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  c.Expect(console.sandbox.Exists("/tico/roms/nes/nova.nes") &&
               console.sandbox.Exists("/tico/roms/nes/240pee.nes"),
           "both roms the browser queued are on the card" + Rendered(console.Log()));
  // The settle after the drain, and the one in the tick the download asks for
  // (#241), each spend their retries.
  UntilQuiet(console, wire, http::Method::kPut, "/installs/", std::chrono::seconds{5});
  const std::vector<std::string> tried = wire.Asked(http::Method::kPut, "/installs/");
  const std::set<std::string> reports(tried.begin(), tried.end());
  c.Expect(!tried.empty(), "a report was tried");
  c.ExpectEq(reports.size(), std::size_t{1},
             "and only the first: the link that did not carry it would not carry the "
             "next, so each pass stops there rather than spending every claim's retries" +
                 Rendered(console.Log()));
  c.ExpectEq(open(), std::size_t{2}, "both stay taken on the server");

  // The link comes back: the next pass reports both.
  down = false;
  TickOnce(console, ticks, c, "the tick after the link comes back");
  c.ExpectEq(open(), std::size_t{0}, "both requests are ended");
  c.ExpectEq(notices(started, "done"), std::size_t{2},
             "both reported done -- the one the pass stopped at and the one it kept");
  c.ExpectEq(notices(started, "failed"), std::size_t{0}, "and neither failed");

  // The download asks for a tick of its own (#241); let it end.
  UntilQuiet(console, wire, http::Method::kPost, "/api/sync/negotiate", std::chrono::seconds{1});

  // **Downloads switched off with a report still owed.** nova is queued again,
  // claimed, and settled from the card -- it is already there -- but the report
  // does not go. Then downloads go off: what this console holds is told to the
  // browser for what it is, not as a download that will never happen.
  const std::int64_t before_off = rig::LatestNotificationId(client, base);
  c.ExpectEq(queue_in_browser(device.nova).response.status, 201, "the web UI queues nova again");
  down = true;
  TickOnce(console, ticks, c, "the tick that claims it again");
  UntilQuiet(console, wire, http::Method::kPut, "/installs/", std::chrono::seconds{5});
  c.ExpectEq(open(), std::size_t{1}, "its report did not go, so it is still taken");
  down = false;
  c.Expect(console.Set(Edit("downloads", "enabled", "false")).outcome ==
               ipc::WriteOutcome::kApplied,
           "and downloads are switched off on the console");
  TickOnce(console, ticks, c, "the tick after downloads went off");
  c.ExpectEq(open(), std::size_t{0}, "the request is ended");
  c.ExpectEq(notices(before_off, "failed"), std::size_t{0},
             "and not as failed: the rom is on the card, settled before downloads went off" +
                 Rendered(console.Log()));
  c.ExpectEq(notices(before_off, "already_installed"), std::size_t{1},
             "but as what its row says: already installed");

  // **And downloads switched off while the link still will not carry it.** The
  // switch is told once, so a report it could not make must not be dropped
  // with the claims: the next tick tells the browser what this one could not.
  c.Expect(console.Set(Edit("downloads", "enabled", "true")).outcome ==
               ipc::WriteOutcome::kApplied,
           "downloads are switched back on");
  TickOnce(console, ticks, c, "the tick that offers the console again");
  const std::int64_t before_down = rig::LatestNotificationId(client, base);
  c.ExpectEq(queue_in_browser(device.other).response.status, 201,
             "the web UI queues 240pee again");
  down = true;
  TickOnce(console, ticks, c, "the tick that claims 240pee");
  UntilQuiet(console, wire, http::Method::kPut, "/installs/", std::chrono::seconds{5});
  c.ExpectEq(open(), std::size_t{1}, "settled from the card, and its report did not go");
  c.Expect(console.Set(Edit("downloads", "enabled", "false")).outcome ==
               ipc::WriteOutcome::kApplied,
           "downloads go off with the link still down");
  TickOnce(console, ticks, c, "the tick after downloads went off, offline for reports");
  down = false;
  TickOnce(console, ticks, c, "the tick after the link comes back");
  c.ExpectEq(open(), std::size_t{0},
             "the request is ended once the link carries it" + Rendered(console.Log()));
  c.ExpectEq(notices(before_down, "failed"), std::size_t{0}, "not as failed");
  c.ExpectEq(notices(before_down, "already_installed"), std::size_t{1},
             "but as already installed");

  // **A request downloads-off fails, whose failure the link does not carry.**
  // Queued in the browser and never claimed by a tick that wanted it, so the
  // downloads-off claim is the first to take it, and its report is the only
  // word the browser gets: the switch is not told until that report went.
  c.Expect(console.Set(Edit("downloads", "enabled", "true")).outcome ==
               ipc::WriteOutcome::kApplied,
           "downloads are switched on once more");
  TickOnce(console, ticks, c, "the tick that offers the console once more");
  const std::int64_t before_unsaid = rig::LatestNotificationId(client, base);
  c.ExpectEq(queue_in_browser(device.nova).response.status, 201,
             "the web UI queues nova, and no tick claims it yet");
  down = true;
  c.Expect(console.Set(Edit("downloads", "enabled", "false")).outcome ==
               ipc::WriteOutcome::kApplied,
           "downloads go off before it is claimed");
  TickOnce(console, ticks, c, "the tick that claims nova to fail it, offline for reports");
  UntilQuiet(console, wire, http::Method::kPut, "/installs/", std::chrono::seconds{5});
  down = false;
  TickOnce(console, ticks, c, "the tick after the link carries reports again");
  c.ExpectEq(notices(before_unsaid, "failed"), std::size_t{1},
             "the browser is told nova failed once the link carries it" +
                 Rendered(console.Log()));
  c.ExpectEq(open(), std::size_t{0}, "and nothing is left taken");

  install::Register(client, base, token, false);
  harness::DeleteDevice(client, base, fixture, token.device_id);
  return c.failures();
}

/// Remote install asked again after a wake (review of #245). A Switch mostly
/// sleeps rather than reboots, so "until the server is read again" cannot mean
/// "until the next boot": a claim refused once -- the feature off for a moment,
/// as RomM answers a claim 404 then -- has to be asked again when the console
/// wakes, and so does a heartbeat that said the feature was off.
int InstallWake(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  const std::string name = "engine-install-wake";
  InstallConsole device;
  if (!AdoptInstallDevice(c, client, base, fixture, name, &device)) {
    return c.failures();
  }
  const auth::StoredToken& token = device.token;

  rlog::Reset();
  const TickLines ticks;
  Console console(c, name);
  if (!ConfigureInstallConsole(c, console, base, token)) {
    return c.failures();
  }
  Intercepting wire(client);
  std::atomic<bool> refuse{true};
  wire.Use([&](const http::Request& request) -> std::optional<http::Result> {
    if (refuse.load() && Asks(request, http::Method::kPost, "/installs/claim")) {
      return Answered(404);
    }
    return std::nullopt;
  });
  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.engine.UseCard(card.get());
  console.engine.UseServer(&wire, std::string());
  console.Boot();
  console.engine.StartWorker();

  const auto claims = [&] { return wire.Count(http::Method::kPost, "/installs/claim"); };
  TickOnce(console, ticks, c, "the tick whose claim is refused");
  c.ExpectEq(claims(), std::size_t{1}, "the claim was asked, and answered 404");

  // The server is fine again, and the user queues a rom for this console.
  refuse = false;
  const std::int64_t started = rig::LatestNotificationId(client, base);
  c.ExpectEq(rig::AsTheUser(client, base, "/api/devices/" + token.device_id + "/installs",
                            "{\"rom_id\":" + std::to_string(device.nova) + "}")
                 .response.status,
             201, "the web UI queues nova for it");
  TickOnce(console, ticks, c, "a tick on the same boot");
  c.ExpectEq(claims(), std::size_t{1},
             "the refusal stands while the console stays awake: no claim every tick");

  // Asleep, and awake again -- the console's ordinary day.
  const std::size_t heartbeats = wire.Count(http::Method::kGet, "/api/heartbeat");
  console.engine.Quiesce();
  console.engine.Resume();
  TickOnce(console, ticks, c, "the first tick after the wake");
  c.Expect(wire.Count(http::Method::kGet, "/api/heartbeat") > heartbeats,
           "the wake reads the server again: what it says of remote install may have changed");
  c.ExpectEq(claims(), std::size_t{2}, "and claims again" + Rendered(console.Log()));

  const std::chrono::steady_clock::time_point by =
      std::chrono::steady_clock::now() + std::chrono::seconds{120};
  const auto open = [&] { return install::Claim(client, base, token).page.requests.size(); };
  while ((!console.sandbox.Exists("/tico/roms/nes/nova.nes") || open() != 0) &&
         std::chrono::steady_clock::now() < by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  c.Expect(console.sandbox.Exists("/tico/roms/nes/nova.nes"),
           "the rom the browser queued is on the card" + Rendered(console.Log()));
  c.ExpectEq(rig::InstallNoticesSince(client, base, started, "rommsync-nx " + name, "done"),
             std::size_t{1}, "and reported done");

  // The download asks for a tick of its own (#241); let it end before teardown.
  UntilQuiet(console, wire, http::Method::kPost, "/api/sync/negotiate", std::chrono::seconds{1});
  install::Register(client, base, token, false);
  harness::DeleteDevice(client, base, fixture, token.device_id);
  return c.failures();
}

/// #249's acceptance, through the real engine against RomM 5.4.0: the player's
/// Keep local on the conflicts screen is the one thing that sends an upload
/// with `overwrite=true`, and only that one upload.
///
/// It is also #236's question, answered and tested: the head the conflict lost
/// to is written by a lineage this console does not load (`gpsp`, where the
/// console's gba saves are mGBA's), so the filtered negotiate never offers it,
/// and only the slot guard's 409 and the unfiltered resolution see it. Keep
/// local goes over it all the same -- the player saw that copy land on the card
/// and chose theirs -- and RomM keeps lineages apart: `overwrite=true` lifts the
/// guard and adds a version in this console's lineage, and the other lineage's
/// rows stay on the server.
///
///   1. the console's save goes up into an empty `autosave`;
///   2. another device writes the slot under `gpsp`, the console's save moves on,
///      and the tick's upload meets the guard: a conflict -- gpsp's bytes on the
///      card, the console's under `.backup/`;
///   3. the other device writes again, so an upload *without* the override would
///      meet the guard a second time;
///   4. Keep local over the wire: the console's bytes are back, and the tick the
///      engine asks for sends them with `overwrite=true` -- exactly once -- and
///      they are the slot's head, with both gpsp rows still beside them;
///   5. the next upload of that save goes with `overwrite=false` again.
int KeepLocalChoice(http::HttpClient& client, const std::string& base) {
  rig::Checks c;
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return 1;
  }
  harness::Rom rom;
  c.Expect(harness::FindRom(client, base, fixture, "240pee_mb.gba", &rom),
           "the fixture library has 240pee_mb.gba");
  if (rom.id == 0) {
    return c.failures();
  }
  ClearSlots(client, base, fixture, rom.id);
  harness::DeleteArchivalCopies(client, base, fixture, rom.id);
  const std::string device_id =
      harness::CreateDevice(c, client, base, fixture, "engine-keep-local");
  if (device_id.empty()) {
    return c.failures();
  }
  // Unique bytes per run: RomM remembers what a slot lost.
  const std::string stamp = harness::UniqueSlot("keep-local");

  rlog::Reset();
  const TickLines ticks;
  Intercepting wire(client);
  Console console(c, "engine-keep-local");
  if (!PointAtTheRig(console, c, base, fixture, device_id)) {
    return c.failures();
  }
  const std::string save = "/tico/saves/gba/240pee_mb.srm";
  console.sandbox.MakeDirs("/tico/saves/gba");
  const std::string first = "the switch's save, first synced " + stamp + "\n";
  console.sandbox.SeedSave(save, first);

  const std::unique_ptr<fs::FileSystem> card = Card(console);
  console.Boot();
  console.engine.UseServer(&wire, fixture.token);
  console.engine.UseCard(card.get());
  console.engine.StartWorker();

  // Another device's upload, in a lineage this console does not load.
  harness::Sandbox staging(c, "engine-keep-local-other");
  const auto other_device = [&](const std::string& bytes, const std::string& what) {
    const std::string staged = staging.Host("/config/rommsync/other.srm");
    rig::WriteFile(staged, bytes);
    harness::Save written;
    c.Expect(harness::UploadSave(client, base, fixture, rom.id, "autosave", "gpsp", staged,
                                 "240pee_mb.srm", /*with_device=*/false, &written),
             what + ": another device writes the slot under gpsp");
  };
  const auto rows_with = [&](const std::string& bytes) {
    std::vector<ServerRow> found;
    for (ServerRow& row : RowsFor(client, base, fixture, rom.id, "autosave")) {
      if (row.content_hash == crypto::Md5Hex(bytes)) {
        found.push_back(std::move(row));
      }
    }
    return found;
  };
  const auto overrides = [&wire] { return wire.Asked(http::Method::kPost, "overwrite=true"); };

  // 1.
  TickOnce(console, ticks, c, "the first tick");
  c.ExpectEq(rows_with(first).size(), std::size_t{1}, "the console's save went up");

  // 2.
  harness::PassASecond();
  const std::string theirs = "the other device's progress, under gpsp " + stamp + "\n";
  other_device(theirs, "before the conflict");
  harness::PassASecond();
  const std::string mine = "the switch played on: the copy the player keeps " + stamp + "\n";
  console.sandbox.Reseed(save, mine);
  TickOnce(console, ticks, c, "the conflict's tick");
  c.ExpectEq(console.sandbox.Read(save), theirs,
             "Keep server, the default: the slot's head is on the card" +
                 Rendered(console.Log()));
  c.Expect(console.sandbox.HasBackupOf(mine), "and the console's bytes are under .backup/");
  c.Expect(overrides().empty(), "nothing has gone with overwrite=true without a choice");

  std::string response;
  console.Call(ipc::Command::kListConflicts, ipc::EncodeConflictQuery(ipc::ConflictQuery{}),
               &response);
  const ipc::ConflictPage page = ipc::DecodeConflictPage(response).value;
  std::int64_t entry_id = 0;
  for (const ipc::ConflictRow& row : page.entries) {
    if (row.entry.rom_id == rom.id && row.entry.event == conflicts::Event::kConflict) {
      entry_id = row.entry.id;
    }
  }
  c.Expect(entry_id != 0, "the conflict is on the conflicts screen");
  const std::int32_t conflicts_before = page.total;

  // 3.
  harness::PassASecond();
  const std::string theirs_again = "the other device again, newer still " + stamp + "\n";
  other_device(theirs_again, "after the conflict");
  // RomM 5.4.0 stores `updated_at` to the whole second and picks a slot's
  // newest row with no tie-break, so an override landing in the same second as
  // that write could lose the head to it -- and the next tick's guard would
  // then 409 on a row this console never synced (M10-32). A second between
  // them keeps this test about the override rather than the tie.
  harness::PassASecond();

  // 4.
  const int ticks_before = ticks.ticks();
  c.Expect(console.Call(ipc::Command::kKeepLocal, ipc::EncodeEntryId(entry_id), &response) ==
               ipc::Error::kOk,
           "KeepLocal is answered");
  const ipc::RestoreState kept = console.AwaitRestore(entry_id);
  c.Expect(kept.phase == ipc::RestorePhase::kDone && kept.report.ok(),
           std::string("Keep local put the console's bytes back: ") +
               conflicts::ToString(kept.report.outcome) + " -- " + kept.report.message);
  // The tick the engine asks for itself, not a press.
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{120};
  while ((ticks.ticks() <= ticks_before || console.Status().sync_in_progress) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  c.Expect(ticks.ticks() > ticks_before, "Keep local starts a tick of its own");
  c.Expect(console.Status().last_sync_result == ipc::SyncResult::kOk,
           std::string("which runs clean: ") +
               ipc::ToString(console.Status().last_sync_result) + Rendered(console.Log()));

  const std::vector<std::string> sent = overrides();
  c.ExpectEq(sent.size(), std::size_t{1}, "exactly one upload went with overwrite=true");
  for (const std::string& url : sent) {
    c.Expect(url.find("rom_id=" + std::to_string(rom.id) + "&") != std::string::npos &&
                 url.find("slot=autosave") != std::string::npos,
             "and it is this save's, in its slot: " + url);
  }
  c.ExpectEq(console.sandbox.Read(save), mine, "the console's bytes are on the card");
  const std::vector<ServerRow> uploaded = rows_with(mine);
  c.Expect(uploaded.size() == 1 && uploaded[0].emulator == "mgba",
           "and on the server, in the console's own lineage");
  std::int64_t head = 0;
  for (const ServerRow& row : RowsFor(client, base, fixture, rom.id, "autosave")) {
    head = std::max(head, row.id);
  }
  c.Expect(!uploaded.empty() && uploaded[0].id == head, "as the slot's newest save");
  c.ExpectEq(rows_with(theirs).size(), std::size_t{1},
             "#236: the other lineage's row the conflict lost to is still on the server");
  c.ExpectEq(rows_with(theirs_again).size(), std::size_t{1},
             "and so is the one the override went over -- RomM keeps lineages apart");
  console.Call(ipc::Command::kListConflicts, ipc::EncodeConflictQuery(ipc::ConflictQuery{}),
               &response);
  c.ExpectEq(ipc::DecodeConflictPage(response).value.total, conflicts_before,
             "and the override was not itself a conflict");

  // 5.
  harness::PassASecond();
  const std::string later = "the switch's next save, an ordinary upload " + stamp + "\n";
  console.sandbox.Reseed(save, later);
  TickOnce(console, ticks, c, "the tick after");
  c.ExpectEq(overrides().size(), std::size_t{1},
             "the choice was one upload's: the next one sends no overwrite=true");
  c.ExpectEq(rows_with(later).size(), std::size_t{1}, "and goes up all the same");

  ClearSlots(client, base, fixture, rom.id);
  harness::DeleteArchivalCopies(client, base, fixture, rom.id);
  harness::DeleteDevice(client, base, fixture, device_id);
  return c.failures();
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
      {"pair_commit", PairCommit},
      {"log_faults", LogsTransportFailures},
      {"server_disabled", ServerDisabled},
      {"drains", Drains},
      {"downloading", DrainProgress},
      {"preempts", DrainPreempted},
      {"discards", DrainDiscarded},
      {"repoints", DrainRepointed},
      {"shutdown", DrainShutdown},
      {"sleep_download", SleepDownload},
      {"romm_version", RommVersion},
      {"foreground", Foreground},
      {"tico_accounts", TicoAccounts},
      {"download_restores", DownloadRestores},
      {"server_deleted", ServerDeleted},
      {"interop", Interop},
      {"upgrade", Upgrade},
      {"placement", Placement},
      {"declined", Declined},
      {"remote_install", RemoteInstall},
      {"states_guard", StatesGuarded},
      {"install_settle", InstallSettle},
      {"install_wake", InstallWake},
      {"keep_local", KeepLocalChoice},
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
  } else if (scenario == "newer") {
    NewerFiles(checks);
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
  } else if (scenario == "sleeps") {
    Sleeps(checks);
  } else if (scenario == "restore_race") {
    RestoreDuringSync(checks);
  } else if (scenario == "restore_polled") {
    RestoreIsPolled(checks);
  } else if (scenario == "restore_boot_wait") {
    RestoreDuringBootWait(checks);
  } else if (scenario == "syncnow") {
    SyncNowStarts(checks);
  } else if (scenario == "unreachable") {
    Unreachable(checks);
  } else if (scenario == "stale_active") {
    StaleActiveOnBoot(checks);
  } else if (scenario == "log") {
    LogsMisconfiguration(checks);
  } else if (scenario == "romm_forgotten") {
    RommForgotten(checks);
  } else if (scenario == "index_canceled") {
    IndexCanceled(checks);
  } else if (const RigScenario* rig_scenario = FindRigScenario(scenario)) {
    // The scenarios that need the fixture RomM. `server` pins that a *live*
    // credential stops being on the card, and the M1-6 three drive a real
    // device-code attempt -- a token nothing ever issued or accepted would prove
    // nothing about either.
    const std::string base = rig::BaseUrl();
    std::error_code error;
    std::filesystem::create_directories(rig::ScratchDir(), error);
    const std::unique_ptr<http::HttpClient> client = rig::MakeClient();
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
