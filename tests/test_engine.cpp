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
//   server    -- M5-3: changing the server discards the token it does not own
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
#include "rig.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/download.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/ipc.hpp"
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

// --- what is still not built --------------------------------------------------

void Commands(checks::Checks& c) {
  Console console(c, "engine-commands");
  console.Boot();

  // `kUnavailable` is a sentence the overlay draws, and each of the issues
  // named here replaces its own. Pinned so the list shrinks deliberately: a
  // command that quietly started answering something plausible instead would
  // send a user looking for a problem that is not there (`engine.hpp`).
  std::string response;
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

  // What each issue that has landed took off that list, which is the signal
  // `engine.hpp` asks every one of them to give.
  std::int32_t position = 0;
  c.Expect(console.Enqueue(4, &position) != ipc::Error::kUnavailable, "Enqueue is built (#19)");
  c.Expect(console.Dequeue(4) != ipc::Error::kUnavailable, "and so is Dequeue (#19)");
  c.Expect(console.Call(ipc::Command::kSetEnabled, ipc::EncodeEnabled(true), &response) ==
               ipc::Error::kOk,
           "SetEnabled answers inside a successful reply, whatever it did");
  c.Expect(ipc::DecodeEnabledResult(response).value.outcome == ipc::WriteOutcome::kApplied,
           "...and it applies now: M5-3 (#30) built it");
  c.Expect(console.Set(Edit("sync", "saves", "false")).outcome == ipc::WriteOutcome::kApplied,
           "as does SetConfig (#30)");
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
  } else if (scenario == "config") {
    ConfigWrites(checks);
  } else if (scenario == "commit") {
    InterruptedCommit(checks);
  } else if (scenario == "commands") {
    Commands(checks);
  } else if (scenario == "server") {
    // The one scenario here that needs the fixture RomM: what it pins is that a
    // *live* credential stops being on the card, and a token nothing ever
    // accepted would prove nothing.
    const std::string base = rig::BaseUrl();
    std::error_code error;
    std::filesystem::create_directories(rig::ScratchDir(), error);
    const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
    if (!rig::Reachable(*client, base)) {
      std::cerr << "rig unreachable at " << base
                << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
      return rig::kSkip;
    }
    const int failures = ServerChanged(*client, base);
    if (failures == 0) {
      std::cout << "engine.server ok against " << base << "\n";
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
