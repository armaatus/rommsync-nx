#include "engine.hpp"

#include <string>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/config.hpp"
#include "rommsync/download.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::sysmodule {

/// One of this client's files, as a path the SD card understands. `core/` owns
/// the file names and may not know an SD path (hard rule 4), so joining them is
/// this side's job -- once, rather than at each call site.
std::string SdEngine::PathTo(const char* file_name) const { return config_dir_ + file_name; }

void SdEngine::Load(const std::string& config_dir) {
  config_dir_ = config_dir;

  // Presence, not validity: a `token.dat` that will not parse is still a
  // console that has been paired, and telling that user they have never paired
  // sends them through a flow that will overwrite a record somebody may want to
  // look at first. What the server thinks of the token is `kUnauthenticated`,
  // and only a request can decide that (`engine.hpp`).
  //
  // The question is asked of `io::ReadFile` rather than of `LoadToken`, and that
  // is not a shortcut: `StoreError::kReadFailed` covers "there is no such file"
  // *and* "it is there and the bytes would not come out of it"
  // (`token_store.cpp`), and those are the two answers that must not be
  // collapsed. An SD card having a bad moment would otherwise draw "Not paired"
  // over a console that is paired, which is the misdirection above.
  const io::ReadResult record = io::ReadFile(PathTo(auth::kTokenFileName));
  auth_ = record.error == io::ReadError::kMissing ? ipc::AuthState::kNeverPaired
                                                  : ipc::AuthState::kPaired;

  // M1-4 (#8): the one state that cannot be worked out from the card alone is
  // read off the card anyway, because a *previous* boot asked the server and
  // wrote down the answer. Without it the overlay draws "paired" until the
  // engine has spent `auth::GateConfig::max_consecutive_rejections` requests
  // reaching the same conclusion again, on every boot.
  //
  // Only over a token that is there. A verdict left behind by a pairing that
  // has since been discarded is about credentials this console no longer holds,
  // and reporting it would send a never-paired user to a "pair again" screen
  // that is one word wrong.
  const auth::LoadedBlock verdict = auth::LoadBlock(PathTo(auth::kAuthStateFileName));
  auth_diagnostics_.clear();
  if (!verdict.diagnostic.empty()) {
    // A warning and never an error: this file holds nothing that cannot be
    // worked out again by asking, so an unreadable one costs the rejection
    // budget and nothing else. It is said out loud anyway, because `core/` has
    // no logger and the settings screen (#26) is the one place on this console a
    // user can read a complaint.
    auth_diagnostics_.push_back({config::Severity::kWarning, 0, "", "", verdict.diagnostic});
  }
  if (auth_ == ipc::AuthState::kPaired) {
    gate_.Restore(verdict.value);
    if (gate_.blocked()) {
      auth_ = ipc::AuthState::kUnauthenticated;
    }
  } else {
    // A verdict about credentials this console no longer holds. Honouring it
    // would send a user who has never paired to a screen that says "pair this
    // console *again*", which is one word wrong about what happened to them.
    gate_.Reset();
  }

  // The queue never refuses to produce one either: a corrupt or oversized
  // `queue.json` is an empty queue plus a diagnostic, and nothing may block boot
  // (`download.hpp`, CLAUDE.md).
  download::LoadedQueue queued = download::LoadQueue(PathTo(download::kQueueFileName));
  queue_.Reset(std::move(queued.entries));
  queue_trusted_ = queued.trusted;
  // Kept rather than folded into `diagnostics_` on the spot, because
  // `ApplyConfigEdit` rebuilds that list from the file it just wrote and would
  // otherwise drop the one complaint no other screen can show. A field of its
  // own on the wire is #22's to add.
  queue_diagnostics_.clear();
  queue_diagnostics_.reserve(queued.diagnostics.size());
  for (const std::string& complaint : queued.diagnostics) {
    queue_diagnostics_.push_back({config::Severity::kWarning, 0, "downloads", "", complaint});
  }
  // M3-5 (#22) built the home the comment above was waiting for:
  // `download::DownloadStatus::queue_message` is a queue-level message on the
  // queue's own model rather than a `[downloads]` section on `config.ini`'s.
  // Both are filled from the same load, because the placeholder is still the
  // only one of the two that reaches a user -- `ipc::Status` carries the
  // projection and not the whole status, and the queue screen (#31) is what
  // should render this and retire the config diagnostic.
  //
  // `DescribeDiagnostics` renders one line per complaint for a log; the trailing
  // newline goes, because this one is a sentence on a screen.
  std::string queue_message = queued.DescribeDiagnostics();
  while (!queue_message.empty() && queue_message.back() == '\n') {
    queue_message.pop_back();
  }
  queue_.set_queue_message(std::move(queue_message));

  AdoptConfig(config::LoadConfig(PathTo(config::kConfigFileName)));
}

void SdEngine::AdoptConfig(config::LoadResult loaded) {
  config_ = std::move(loaded.value);
  diagnostics_ = queue_diagnostics_;
  diagnostics_.reserve(diagnostics_.size() + auth_diagnostics_.size() + loaded.diagnostics.size());
  diagnostics_.insert(diagnostics_.end(), auth_diagnostics_.begin(), auth_diagnostics_.end());
  for (config::Diagnostic& diagnostic : loaded.diagnostics) {
    diagnostics_.push_back(std::move(diagnostic));
  }
}

bool SdEngine::WriteQueue() {
  return download::SaveQueue(PathTo(download::kQueueFileName), queue_).ok();
}

const config::Config& SdEngine::config() const { return config_; }

const std::vector<config::Diagnostic>& SdEngine::config_diagnostics() const {
  return diagnostics_;
}

ipc::EngineSnapshot SdEngine::Snapshot() const {
  ipc::EngineSnapshot snapshot;
  snapshot.auth = auth_;
  // M3-2 (#19): these two come off the queue on the card rather than being the
  // zero a console with no engine reports. `pending()` counts what the worker
  // still has to do, so the finished rows kept for the queue screen do not
  // inflate the depth.
  snapshot.queue_depth = static_cast<std::int64_t>(queue_.pending());
  snapshot.download = queue_.CurrentDownload();
  // Everything else is a default, and every default is the truthful answer for
  // a console whose engine has not been built: never synced, nothing online.
  // `Status` renders each of those as its own sentence rather than as a blank
  // (`overlay_status_view.hpp`), which is why this is safe to serve rather than
  // something to refuse.
  return snapshot;
}

auth::PairingStatus SdEngine::pairing_status() const { return auth::PairingStatus{}; }

ipc::Error SdEngine::SetSyncEnabled(bool enabled) {
  ipc::ConfigEdit edit;
  edit.assignments.push_back({"sync", "enabled", enabled ? "true" : "false", false});
  // Down the same path as the settings screen rather than a shortcut of its
  // own: `ServiceCore::SetEnabled` answers with the state read back off the
  // config, and the two would have to agree about what a failed write left
  // behind anyway. The diagnostics go nowhere because `EnabledResult` carries
  // none -- a switch that did not move is the whole message (#24).
  std::vector<config::Diagnostic> unread;
  return ApplyConfigEdit(edit, &unread);
}

bool SdEngine::ReadConfigText(std::string* text,
                              std::vector<config::Diagnostic>* diagnostics) const {
  const std::string path = PathTo(config::kConfigFileName);
  const io::BoundedRead outcome = io::ReadBounded(path, config::kMaxConfigBytes, text);
  if (outcome == io::BoundedRead::kOk) {
    return true;
  }
  if (outcome == io::BoundedRead::kMissing) {
    // The one moment `config.ini` legitimately does not exist is the window
    // `io::WriteAtomically`'s two-rename commit opens, and in that window the
    // user's settings are sitting intact under the other name. Editing an empty
    // string instead would write a `config.ini` holding one line and call the
    // rest of their configuration gone. `LoadConfig` recovers from exactly the
    // same window, for exactly the same reason.
    std::string previous;
    if (io::ReadBounded(io::PreviousPathFor(path), config::kMaxConfigBytes, &previous) ==
        io::BoundedRead::kOk) {
      *text = std::move(previous);
      return true;
    }
    // Nothing under either name: a console nobody has configured yet, which is
    // a file to create rather than a failure.
    text->clear();
    return true;
  }
  // It is there and the bytes would not come out of it, or it is too large to
  // be one. Settings that cannot be read cannot be preserved, and writing a
  // fresh file over them is the one outcome worse than refusing the edit.
  diagnostics->push_back(
      {config::Severity::kError, 0, "", "",
       std::string(config::kConfigFileName) + " could not be read (" +
           io::ToString(outcome) + "), so it was not edited; your settings are still on the card"});
  return false;
}

ipc::Error SdEngine::ApplyConfigEdit(const ipc::ConfigEdit& edit,
                                     std::vector<config::Diagnostic>* diagnostics) {
  std::string current;
  if (!ReadConfigText(&current, diagnostics)) {
    return ipc::Error::kWriteFailed;
  }

  std::string written;
  if (!config::ApplyEdit(current, edit, &written, diagnostics)) {
    // `kInvalid` promises the file is untouched, and it is: nothing below has
    // run (`ipc.hpp`).
    return ipc::Error::kInvalid;
  }

  // **A `server.url` change invalidates the session.** The token in `token.dat`
  // was issued by the old RomM and the record says which one
  // (`token_store.hpp`), so carrying it to a new host would send a stranger's
  // server this console's bearer token. That is a security bug rather than a UX
  // one, so it is decided by what the file will actually parse to and not by
  // which assignments the overlay happened to send.
  //
  // Compared against the **card**, not against `config_`. Those two part company
  // exactly when `config.ini` was there and unreadable at boot: `LoadConfig`
  // answers with the built-in defaults then (it may never refuse), so
  // `config_.server.url` is empty while the file names a perfectly good server.
  // Comparing against that would make the next edit of any kind -- a plain
  // `SetEnabled` toggle included -- look like a server change and shred a
  // working pairing over an SD card that had one bad moment.
  //
  // The token goes **before** the write, deliberately. The other order leaves a
  // moment where the card names the new server and still holds the old
  // credential, and a discard that failed there would leave it there for good.
  //
  // Parsed once and kept: this is also the configuration adopted at the end, so
  // the alternative is parsing the same few hundred bytes twice on a command a
  // user pressed by hand.
  config::LoadResult parsed = config::ParseConfig(written);
  const bool server_changed =
      parsed.value.server.url != config::ParseConfig(current).value.server.url;
  if (server_changed && !auth::DiscardToken(PathTo(auth::kTokenFileName))) {
    diagnostics->push_back({config::Severity::kError, 0, "server", "url",
                            "the pairing for the previous server could not be discarded, so the "
                            "server was not changed"});
    return ipc::Error::kWriteFailed;
  }
  if (server_changed) {
    // The verdict was about the token that has just gone, and about the server
    // that issued it. Left behind, it would put a console that has never paired
    // with the *new* server on a "pair again" screen (M1-4, #8). Not a refusal
    // if it fails: unlike the token, a stale verdict here costs a wrong sentence
    // rather than a bearer token pointed at a stranger, and the pairing is
    // already gone by this point.
    auth::ClearBlock(PathTo(auth::kAuthStateFileName));
    gate_.Reset();
  }

  const io::WriteResult wrote = io::WriteAtomically(PathTo(config::kConfigFileName), written);
  if (!wrote.ok()) {
    diagnostics->push_back({config::Severity::kError, 0, "", "",
                            std::string(config::kConfigFileName) + " was not written (" +
                                io::ToString(wrote.error) + "); your settings are unchanged"});
    if (server_changed) {
      // Said out loud rather than left for the user to discover at the pairing
      // screen: the safe order above has already cost them the pairing, and the
      // server they are paired to did not change after all.
      diagnostics->push_back({config::Severity::kWarning, 0, "server", "url",
                              "this console's pairing was discarded before the write was "
                              "attempted, so it has to be paired again"});
      auth_ = ipc::AuthState::kNeverPaired;
    }
    return ipc::Error::kWriteFailed;
  }

  if (server_changed) {
    auth_ = ipc::AuthState::kNeverPaired;
    diagnostics->push_back({config::Severity::kNotice, 0, "server", "url",
                            "the server changed, so this console's pairing was discarded -- "
                            "pair again from the overlay"});
  }

  // The parse above rather than a re-read off the card, and that is the
  // difference that matters: the configuration in force is one `ParseConfig`
  // produced, so nothing in this process is a `Config` assembled by hand. `WriteAtomically` has just succeeded, so `written`
  // *is* what the card holds; a re-read that failed for a moment would hand back
  // `LoadConfig`'s built-in defaults, and adopting those would throw away a
  // configuration this process knows to be correct in favour of a worse one. At
  // boot that fallback is right, because there is nothing better to have.
  //
  // This is what makes the change take effect with no reboot: `GetStatus` and
  // `GetConfig` read `config_`, and it is now the new one.
  AdoptConfig(std::move(parsed));
  return ipc::Error::kOk;
}

bool SdEngine::RequestSync() {
  // False, which `ServiceCore::SyncNow` reports as `kAlreadyRunning`. That is
  // the one answer in this file that is not quite the truth, and it is forced:
  // `RequestSync` is a bool, so there is no `kUnavailable` to return. M7-2 is
  // what makes it true; until then the screen shows a tick that never starts
  // rather than one that claims to have.
  return false;
}

ipc::Error SdEngine::StartPairing() { return ipc::Error::kUnavailable; }

ipc::Error SdEngine::Unpair() {
  // The credentials first: see the header note on the order.
  if (!auth::DiscardToken(PathTo(auth::kTokenFileName))) {
    return ipc::Error::kWriteFailed;
  }
  // A verdict that outlived the token it was about is what would leave a
  // freshly re-paired console on the re-pair screen, so this failing is a
  // refusal rather than something to shrug at -- and the token really is gone,
  // which the message the overlay draws has to be able to say.
  const bool cleared = auth::ClearBlock(PathTo(auth::kAuthStateFileName));
  // The gate is reset either way, and that is the point: it is a verdict about
  // a token that no longer exists, so leaving it standing would have a worker
  // (M7-2, #37) refuse to call on a console the user has just re-paired, with no
  // way out short of a reboot. What a failed clear costs is the *next boot*
  // reading the file back and reporting `kUnauthenticated` over a pairing that
  // is gone -- which `Load` already refuses to do, because there is no token for
  // the verdict to be about.
  gate_.Reset();
  auth_ = ipc::AuthState::kNeverPaired;
  return cleared ? ipc::Error::kOk : ipc::Error::kWriteFailed;
}

ipc::Error SdEngine::Enqueue(std::int64_t rom_id, std::int32_t* position) {
  return Commit([&] { return queue_.Enqueue(rom_id, position); });
}

ipc::Error SdEngine::Dequeue(std::int64_t rom_id) {
  return Commit([&] { return queue_.Remove(rom_id); });
}

ipc::Error SdEngine::ListBegin(const ipc::ListRequest&, ipc::Cursor*) {
  return ipc::Error::kUnavailable;
}

ipc::Error SdEngine::ListNext(ipc::Cursor, ipc::ListPage*) { return ipc::Error::kUnavailable; }

ipc::Error SdEngine::ListEnd(ipc::Cursor) { return ipc::Error::kUnavailable; }

}  // namespace rommsync::sysmodule
