#include "engine.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/config.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/download.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/list_service.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::sysmodule {
namespace {

/// How often the pairing thread offers the session a chance to act.
///
/// Not the poll interval: `PairingSession::Poll` enforces the one RomM asked
/// for and returns immediately when the next request is not due, because a
/// client that undercuts that interval earns `slow_down` until the code expires
/// (`pairing.hpp`). This is only how promptly the thread notices that the
/// interval has elapsed, that a new attempt has replaced this one, or that the
/// engine is going away.
constexpr std::chrono::milliseconds kPairingTick{200};

}  // namespace

/// One of this client's files, as a path the SD card understands. `core/` owns
/// the file names and may not know an SD path (hard rule 4), so joining them is
/// this side's job -- once, rather than at each call site.
std::string SdEngine::PathTo(const char* file_name) const { return config_dir_ + file_name; }

<<<<<<< HEAD
/// The service reads the configuration in force and the queue on the card, both
/// of which outlive it because it is a member beside them.
SdEngine::SdEngine() : lists_(config_, queue_) {}

void SdEngine::UseServer(http::HttpClient* client, std::string bearer_token) {
  lists_.UseServer(client, std::move(bearer_token));
}

void SdEngine::UseCard(fs::FileSystem* filesystem) { lists_.UseCard(filesystem); }

bool SdEngine::PumpLists() { return lists_.Pump(); }

=======
SdEngine::~SdEngine() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (pairing_thread_.joinable()) {
    pairing_thread_.join();
  }
}

>>>>>>> 960b9da (M1-6: the console starts a pairing, on a thread of its own)
void SdEngine::Load(const std::string& config_dir) {
  // The pairing thread reads `auth_` and `gate_`, and this writes both. It
  // cannot be running yet -- it is started by the first `StartPairing`, and
  // nothing has answered a command -- but taking the lock here costs one
  // uncontended acquire at boot and means no future caller has to know that.
  std::lock_guard<std::mutex> lock(mutex_);
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
  // The complaint goes to the queue and nowhere else.
  //
  // It used to go on `config_diagnostics()` under a `[downloads]` section as
  // well, because `ipc::Status` carries a projection rather than the whole
  // `DownloadStatus` and the settings screen was the only place on this console
  // a user could read a sentence (M3-5, #22). M5-4 (#31) is what that
  // placeholder was waiting for: the `queue` list serves this as a row of its
  // own, on the screen the user opens to ask why a rom never arrived, so a
  // `[downloads]` section on a report about `config.ini` is now a second copy
  // in the wrong place.
  //
  // `discarded` rather than "there are diagnostics": a card that has never
  // queued anything produces one saying so, and a row on the download screen
  // reading "queue.json is missing" on every new console is noise in the one
  // place a user goes to find out why a rom did not arrive (`download.hpp`).
  //
  // `DescribeDiagnostics` renders one line per complaint for a log; the trailing
  // newline goes, because this one is a sentence on a screen.
  std::string queue_message = queued.discarded ? queued.DescribeDiagnostics() : std::string();
  while (!queue_message.empty() && queue_message.back() == '\n') {
    queue_message.pop_back();
  }
  queue_.set_queue_message(std::move(queue_message));

  AdoptConfig(config::LoadConfig(PathTo(config::kConfigFileName)));
}

void SdEngine::UsePairingBackend(PairingBackend backend) {
  pairing_backend_ = std::move(backend);
}

void SdEngine::AdoptConfig(config::LoadResult loaded) {
  config_ = std::move(loaded.value);
  diagnostics_ = auth_diagnostics_;
  diagnostics_.reserve(diagnostics_.size() + loaded.diagnostics.size());
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
  {
    // The one field here the pairing thread writes: a completed attempt turns a
    // never-paired console into a paired one without a reload.
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.auth = auth_;
  }
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

auth::PairingStatus SdEngine::pairing_status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (attempt_ == nullptr) {
    // `kIdle`: nothing has been started, or a `server.url` change discarded what
    // had been (`pairing.hpp`).
    return auth::PairingStatus{};
  }
  auth::PairingStatus status = attempt_->status();
  if (status.state == auth::PairingState::kIdle) {
    // The window `ipc::Engine::StartPairing` documents: the command has handed
    // the attempt over and `Begin()` has not run yet, so the session still says
    // `kIdle`. Reporting that would have the overlay tell the user that pressing
    // Pair did nothing, which is the exact failure `kStarting` exists to prevent.
    status.state = auth::PairingState::kStarting;
  }
  if (!attempt_commit_failure_.empty()) {
    // The session succeeded and the card did not. It has no way to know that and
    // would go on reporting `kApproved` -- a console that says it paired and
    // holds no credentials.
    status.state = auth::PairingState::kFailed;
    status.message = attempt_commit_failure_;
  }
  return status;
}

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
  // For `auth_`, `gate_` and the token below, which the pairing thread also
  // writes. `config_` is this thread's alone (`engine.hpp`).
  std::lock_guard<std::mutex> lock(mutex_);
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
    // ...and so is any pairing attempt in flight, which was started against the
    // server that has just been replaced. Its grant would be a token issued by
    // one RomM committed under a `config.ini` naming another -- the same
    // confusion the discard above exists to prevent, arriving a minute later.
    AbandonPairingLocked();
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

ipc::Error SdEngine::StartPairing() {
  // Asked again here rather than trusted from `ServiceCore::StartPair`, which
  // refuses an unconfigured console before the engine is reached: `Engine` is an
  // interface anything may drive, and starting an attempt against an empty
  // origin would spend the console's device identifier on a request to nowhere.
  if (!config_.configured()) {
    return ipc::Error::kNotConfigured;
  }
  if (pairing_backend_.http == nullptr) {
    // The one `kUnavailable` this command has left, and it is the honest one:
    // `core/` reaches a server through `http::HttpClient` and no Horizon backend
    // for it exists yet (#43's gate item). Nothing was attempted and nothing
    // changed, exactly as `ipc.hpp` promises.
    return ipc::Error::kUnavailable;
  }

  // Before the attempt, because RomM keys the device on this value and a pairing
  // begun with an identifier that never reached the card would register a second
  // console on the next re-pair (`device_identity.hpp`). It touches the card and
  // not the network, so it is not a wait this command may not make.
  const auth::IdentityResult identity = auth::LoadOrCreateDeviceIdentity(
      PathTo(auth::kDeviceIdentityFileName), pairing_backend_.identity_seed);
  if (!identity.ok()) {
    // `kUnreadable` is the important one: `device.dat` is there and would not
    // open, and minting over it is unrecoverable, so this refuses rather than
    // duplicating the console. `kWriteFailed` is the right sentence for both --
    // the card is what stopped this, and nothing was written.
    return identity.error == auth::IdentityError::kNoSeed ? ipc::Error::kInternal
                                                          : ipc::Error::kWriteFailed;
  }

  auth::PairingConfig pairing;
  pairing.server_url = config_.server.url;
  pairing.client_device_identifier = identity.value.client_device_identifier;
  // `device_name` and `requested_scopes` are `pairing.hpp`'s defaults on
  // purpose: the scopes are the least-privilege set `auth.scopes` pins to the
  // document, and widening them here would be asking for a token that can do
  // more than this client ever does.

  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Whatever the last attempt left behind is discarded, which is what the
    // pairing screen's "Start over" means: a second attempt over a live one is
    // that, not an error (`PairingSession::Begin`).
    attempt_ = std::make_shared<auth::PairingSession>(*pairing_backend_.http, std::move(pairing));
    attempt_server_url_ = config_.server.url;
    attempt_commit_failure_.clear();
    ++attempt_generation_;
    if (!pairing_thread_.joinable()) {
      pairing_thread_ = std::thread(&SdEngine::DrivePairing, this);
    }
  }
  wake_.notify_all();
  // `pairing_status()` already answers `kStarting`, which is the contract
  // `ipc::Engine::StartPairing` states: `ServiceCore::StartPair` reads it one
  // instant later and must not see `kIdle`.
  return ipc::Error::kOk;
}

void SdEngine::AbandonPairingLocked() {
  attempt_.reset();
  attempt_server_url_.clear();
  attempt_commit_failure_.clear();
  ++attempt_generation_;
}

bool SdEngine::AwaitNextPoll(std::uint64_t generation) {
  std::unique_lock<std::mutex> lock(mutex_);
  wake_.wait_for(lock, kPairingTick,
                 [this, generation] { return stopping_ || attempt_generation_ != generation; });
  return !stopping_ && attempt_generation_ == generation;
}

void SdEngine::DrivePairing() {
  for (;;) {
    std::shared_ptr<auth::PairingSession> session;
    std::string server_url;
    std::uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || attempt_generation_ != driven_generation_; });
      if (stopping_) {
        return;
      }
      generation = attempt_generation_;
      driven_generation_ = generation;
      session = attempt_;
      server_url = attempt_server_url_;
      if (session == nullptr) {
        // A `server.url` change abandoned the attempt before this thread ever
        // picked it up. There is nothing to drive.
        continue;
      }
    }

    // The request `StartPairing` refused to wait for. Everything from here down
    // is off the IPC thread, so it may take as long as `request_timeout`.
    auth::PairingState state = session->Begin();
    while (!auth::IsTerminal(state) && AwaitNextPoll(generation)) {
      state = session->Poll();
    }
    if (state != auth::PairingState::kApproved) {
      // Denied, expired, failed -- or superseded, in which case the attempt that
      // replaced this one is what the next turn of this loop picks up. All four
      // leave the card exactly as it was.
      continue;
    }
    const auth::DeviceTokenResponse* granted = session->token();
    if (granted != nullptr) {
      CommitGrant(*granted, server_url, generation);
    }
  }
}

void SdEngine::CommitGrant(const auth::DeviceTokenResponse& granted, const std::string& server_url,
                           std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || attempt_generation_ != generation) {
    // A newer attempt is running, or the engine is going away. The grant is
    // real and its code is spent, so this does cost the user one approval --
    // but writing it would put a token on the card that the attempt the overlay
    // is drawing knows nothing about, and the attempt that replaced this one
    // will commit its own.
    return;
  }

  // M1-5 (#9)'s write, unchanged: the record goes to `token.dat.tmp` and is
  // renamed onto `token.dat`, so a reader sees the previous token or the new one
  // and never a splice. `device.dat` is untouched -- the identifier has to
  // survive a re-pair or RomM registers the console twice.
  const auth::StoredToken record = auth::StoredTokenFrom(server_url, granted);
  const auth::StoreResult saved = auth::SaveToken(PathTo(auth::kTokenFileName), record);
  if (!saved.ok()) {
    // Said out loud on the pairing screen rather than left to be discovered at
    // the next sync tick: the approval is spent, so the remedy is to pair again,
    // and a screen still reading `kApproved` would tell the user the opposite.
    attempt_commit_failure_ =
        std::string("the pairing was approved and the token could not be saved (") +
        auth::ToString(saved.error) + ") -- pair again";
    return;
  }

  // A verdict about the credentials that have just been replaced. Left standing
  // it would have a worker (M7-2, #37) refuse to call on a console the user has
  // this second paired. Not a refusal if it fails, for the reason `Unpair`
  // gives: what a failed clear costs is the next boot reading it back, and
  // `Load` already declines to honour a verdict over a token it is not about.
  auth::ClearBlock(PathTo(auth::kAuthStateFileName));
  gate_.Reset();
  auth_ = ipc::AuthState::kPaired;
}

ipc::Error SdEngine::Unpair() {
  // The pairing thread commits a grant under this same lock, so a discard and a
  // commit cannot interleave: one of them happens whole. **It does not abandon a
  // live attempt**, and that is the settings screen's whole design -- "Re-pair"
  // sends `StartPair` first and `Unpair` only once an attempt is under way, so
  // the console never passes through "unpaired with nothing to restart"
  // (docs/AUTH.md, M4-4 #26).
  std::lock_guard<std::mutex> lock(mutex_);
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

ipc::Error SdEngine::ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor) {
  return lists_.ListBegin(request, cursor);
}

ipc::Error SdEngine::ListNext(ipc::Cursor cursor, ipc::ListPage* page) {
  return lists_.ListNext(cursor, page);
}

ipc::Error SdEngine::ListEnd(ipc::Cursor cursor) { return lists_.ListEnd(cursor); }

}  // namespace rommsync::sysmodule
