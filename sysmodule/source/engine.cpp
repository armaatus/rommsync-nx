#include "engine.hpp"

#include <string>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::sysmodule {

void SdEngine::Load() {
  const config::LoadResult loaded =
      config::LoadConfig(std::string(kConfigDir) + config::kConfigFileName);
  config_ = loaded.value;
  diagnostics_ = loaded.diagnostics;

  // Presence, not validity: a `token.dat` that will not parse is still a
  // console that has been paired, and telling that user they have never paired
  // sends them through a flow that will overwrite a record somebody may want to
  // look at first. What the server thinks of the token is `kUnauthenticated`,
  // and only a request can decide that (`engine.hpp`).
  const auth::LoadedToken token =
      auth::LoadToken(std::string(kConfigDir) + auth::kTokenFileName);
  auth_ = token.error == auth::StoreError::kReadFailed ? ipc::AuthState::kNeverPaired
                                                       : ipc::AuthState::kPaired;
}

const config::Config& SdEngine::config() const { return config_; }

const std::vector<config::Diagnostic>& SdEngine::config_diagnostics() const {
  return diagnostics_;
}

ipc::EngineSnapshot SdEngine::Snapshot() const {
  ipc::EngineSnapshot snapshot;
  snapshot.auth = auth_;
  // Everything else is a default, and every default is the truthful answer for
  // a console whose engine has not been built: never synced, nothing online,
  // nothing queued, nothing downloading. `Status` renders each of those as its
  // own sentence rather than as a blank (`overlay_status_view.hpp`), which is
  // why this is safe to serve rather than something to refuse.
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

ipc::Error SdEngine::Enqueue(std::int64_t, std::int32_t*) { return ipc::Error::kUnavailable; }

ipc::Error SdEngine::Dequeue(std::int64_t) { return ipc::Error::kUnavailable; }

ipc::Error SdEngine::ListBegin(const ipc::ListRequest&, ipc::Cursor*) {
  return ipc::Error::kUnavailable;
}

ipc::Error SdEngine::ListNext(ipc::Cursor, ipc::ListPage*) { return ipc::Error::kUnavailable; }

ipc::Error SdEngine::ListEnd(ipc::Cursor) { return ipc::Error::kUnavailable; }

}  // namespace rommsync::sysmodule
