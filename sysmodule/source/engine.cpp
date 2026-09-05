#include "engine.hpp"

#include <string>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/auth.hpp"
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
  const config::LoadResult loaded = config::LoadConfig(PathTo(config::kConfigFileName));
  config_ = loaded.value;
  diagnostics_ = loaded.diagnostics;

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

  // The queue never refuses to produce one either: a corrupt or oversized
  // `queue.json` is an empty queue plus a diagnostic, and nothing may block boot
  // (`download.hpp`, CLAUDE.md).
  download::LoadedQueue queued = download::LoadQueue(PathTo(download::kQueueFileName));
  queue_.Reset(std::move(queued.entries));
  queue_trusted_ = queued.trusted;
  // Carried on the config's diagnostics rather than dropped. It is not a
  // complaint about `config.ini`, and the section says so -- but a queue that
  // vanished with nothing anywhere saying why is the failure a diagnostic
  // exists to prevent, and the settings screen (#26) is the one place on this
  // console a user can read one. A field of its own is #22's to add.
  //
  // **In front, not appended.** `ipc::TrimDiagnostics` keeps the first few and
  // summarises the rest, so a `config.ini` with a handful of complaints would
  // otherwise push the one saying the whole download queue was discarded into
  // the "N more" line -- and that is the one a user cannot infer from anything
  // else on the screen.
  std::vector<config::Diagnostic> ordered;
  ordered.reserve(queued.diagnostics.size() + diagnostics_.size());
  for (std::string& complaint : queued.diagnostics) {
    ordered.push_back({config::Severity::kWarning, 0, "downloads", "", std::move(complaint)});
  }
  for (config::Diagnostic& diagnostic : diagnostics_) {
    ordered.push_back(std::move(diagnostic));
  }
  diagnostics_ = std::move(ordered);
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

ipc::Error SdEngine::SetSyncEnabled(bool) { return ipc::Error::kUnavailable; }

ipc::Error SdEngine::ApplyConfigEdit(const ipc::ConfigEdit&, std::vector<config::Diagnostic>*) {
  return ipc::Error::kUnavailable;
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

ipc::Error SdEngine::Unpair() { return ipc::Error::kUnavailable; }

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
