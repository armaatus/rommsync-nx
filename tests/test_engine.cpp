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
// do. Everything else in `SdEngine` is still `kUnavailable`, and `commands`
// pins that too — each of #30, #31 and M7-2 replaces its own part, and the last
// `kUnavailable` to go is what says the engine is finished (`engine.hpp`).
//
// Driven through `ipc::Dispatch` rather than by calling the methods, because
// the dispatch table is what the console actually runs.
//
//   queue     -- enqueue, dequeue, and the depth the status screen reads
//   persists  -- every change reaches queue.json, and a fresh engine reads it back
//   rollback  -- a write that cannot happen changes neither the file nor memory
//   corrupt   -- a queue.json a yanked card left behind never blocks the boot
//   commands  -- what is still `kUnavailable`, so the list shrinks deliberately
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "engine.hpp"
#include "harness.hpp"
#include "rommsync/download.hpp"
#include "rommsync/ipc.hpp"

namespace config = rommsync::config;
namespace download = rommsync::download;
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
  std::string response;
  console.Call(ipc::Command::kGetConfig, ipc::EncodeEmpty(), &response);
  const ipc::ConfigView view = ipc::DecodeConfigView(response).value;
  bool complained = false;
  for (const config::Diagnostic& diagnostic : view.diagnostics) {
    complained = complained || diagnostic.section == "downloads";
  }
  c.Expect(complained, "and the overlay is told the queue was discarded");

  // And it still works: a console whose queue was thrown away can queue again.
  std::int32_t position = 0;
  c.Expect(console.Enqueue(4, &position) == ipc::Error::kOk, "the queue is usable again");
  c.ExpectEq(console.OnCard().entries.size(), std::size_t{1}, "and the card holds one entry");

  // A `config.ini` full of its own complaints must not push the queue's out of
  // the payload. `ipc::TrimDiagnostics` keeps the first few and summarises the
  // rest, and "your whole download queue was discarded" is the one line a user
  // cannot infer from anything else on the screen -- so it goes in front.
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
  bool survived = false;
  for (const config::Diagnostic& diagnostic : crowded.diagnostics) {
    survived = survived || diagnostic.section == "downloads";
  }
  c.Expect(crowded.diagnostics.size() > 1, "the config's own complaints are there too");
  c.Expect(survived, "and the queue's complaint survived the trim rather than being summarised");
}

// --- what is still not built --------------------------------------------------

void Commands(checks::Checks& c) {
  Console console(c, "engine-commands");
  console.Boot();

  // `kUnavailable` is a sentence the overlay draws, and each of the issues
  // named here replaces its own. Pinned so the list shrinks deliberately: a
  // command that quietly started answering something plausible instead would
  // send a user looking for a problem that is not there (`engine.hpp`).
  std::string response;
  c.Expect(console.Call(ipc::Command::kSetEnabled, ipc::EncodeEnabled(true), &response) ==
               ipc::Error::kOk,
           "SetEnabled answers inside a successful reply, whatever it did");
  c.Expect(ipc::DecodeEnabledResult(response).value.outcome != ipc::WriteOutcome::kApplied,
           "...and it did not apply: M5-3 (#30) is what makes it real");

  ipc::ListRequest listing;
  listing.kind = ipc::ListKind::kQueue;
  c.Expect(console.Call(ipc::Command::kListBegin, ipc::EncodeListRequest(listing), &response) ==
               ipc::Error::kUnavailable,
           "ListBegin is still unavailable: M5-4 (#31) is what makes it real");
  c.Expect(console.Call(ipc::Command::kUnpair, ipc::EncodeEmpty(), &response) ==
               ipc::Error::kUnavailable,
           "and so is Unpair");
  // `StartPair` never reaches the engine on this console: `ServiceCore` refuses
  // first, because there is no `server.url` to pair with -- which is the right
  // answer and a different sentence from "not built yet".
  c.Expect(console.Call(ipc::Command::kStartPair, ipc::EncodeEmpty(), &response) ==
               ipc::Error::kNotConfigured,
           "StartPair is refused for want of a server before the engine is asked at all");

  // The two this issue owns are not on that list any more, which is the signal
  // #19 asked for.
  std::int32_t position = 0;
  c.Expect(console.Enqueue(4, &position) != ipc::Error::kUnavailable, "Enqueue is built");
  c.Expect(console.Dequeue(4) != ipc::Error::kUnavailable, "and so is Dequeue");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "queue";
  checks::Checks checks;

  if (scenario == "queue") {
    Queue(checks);
  } else if (scenario == "persists") {
    Persists(checks);
  } else if (scenario == "rollback") {
    Rollback(checks);
  } else if (scenario == "corrupt") {
    Corrupt(checks);
  } else if (scenario == "commands") {
    Commands(checks);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() == 0) {
    std::cout << "engine." << scenario << " ok\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}
