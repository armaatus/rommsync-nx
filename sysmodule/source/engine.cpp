#include "engine.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/save_scan.hpp"
#include "rommsync/scheduler.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/state_sync.hpp"
#include "rommsync/sync_finish.hpp"
#include "rommsync/sync_tick.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/config.hpp"
#include "rommsync/conflict_log.hpp"
#include "rommsync/conflict_record.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/download.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/list_service.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::sysmodule {

/// One of this client's files, as a path the SD card understands. `core/` owns
/// the file names and may not know an SD path (hard rule 4), so joining them is
/// this side's job -- once, rather than at each call site.
std::string SdEngine::PathTo(const char* file_name) const { return config_dir_ + file_name; }

/// The service reads the configuration in force and the queue on the card, both
/// of which outlive it because it is a member beside them.
/// The service reads the configuration in force through a snapshot rather than a
/// reference, because `Pump()` runs on the worker while `SetConfig` replaces the
/// whole `Config` on the IPC thread (`ConfigSnapshot`).
SdEngine::SdEngine() : lists_([this] { return ConfigSnapshot(); }, queue_) {}

SdEngine::~SdEngine() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  // Before the join, not after it: a tick already in flight ends at its next
  // operation boundary rather than being waited out in full, which on the link
  // whose loss is usually why the process is going away is three timeouts and
  // two backoffs saved (`sync::TickOptions::cancel`).
  tick_cancel_.Cancel();
  wake_.notify_all();
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  if (pairing_thread_.joinable()) {
    pairing_thread_.join();
  }
}

void SdEngine::UseServer(http::HttpClient* client, std::string bearer_token) {
  std::lock_guard<std::mutex> lock(mutex_);
  server_ = client;
  // Empty means "the one on the card", which is what the console passes. See the
  // header: `token.dat` is this class's to read and re-read, and a `main.cpp`
  // that fished the token out of it would be a second reader of the one file
  // that must not be read two different ways.
  list_token_ = bearer_token.empty() ? token_.access_token : std::move(bearer_token);
  lists_.UseServer(client, list_token_);
  // Every answer this service gets is now counted towards the one verdict --
  // the seam #31 left open, where a revoked token read as "your server is
  // unreachable" on every page and nothing anywhere counted it
  // (`lists::Service::UseAuthObserver`).
  lists_.UseAuthObserver([this](auth::Answer answer) { ObserveAnswer(answer); });
}

void SdEngine::UseNetworkWait(NetworkWait wait) {
  std::lock_guard<std::mutex> lock(mutex_);
  network_wait_ = std::move(wait);
}

void SdEngine::UseCard(fs::FileSystem* filesystem) {
  card_ = filesystem;
  lists_.UseCard(filesystem);
}

bool SdEngine::PumpLists() { return lists_.Pump(); }

void SdEngine::Load(const std::string& config_dir) {
  // This reads `token.dat` and `auth.json` and writes `auth_` and `gate_`, so it
  // takes both locks in the documented order.
  //
  // It is the one place `mutex_` is held across I/O, and the header says so.
  // Whether a pairing thread exists yet depends on the caller: `main()` calls
  // this *before* `UsePairingBackend`, so on a console there is none; the tests'
  // `BootPairable` calls `UsePairingBackend` first, so there is. Neither can
  // stall on these reads. A thread that does not exist is no reader, and one
  // that does is parked in `AwaitNextPoll` with no attempt to drive, taking
  // `mutex_` only for the length of a condition-variable predicate. What the
  // rule is really about -- the frame-polled commands -- cannot reach it either
  // way, because no `ServiceCore` exists to answer them until this returns.
  std::lock_guard<std::mutex> card(card_mutex_);
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

  // The credentials themselves, for the worker: negotiate needs `device_id` as
  // well as the bearer token (M7-2, #37). A record that is there and will not
  // parse leaves this empty and `auth_` at `kPaired`, which is the honest pair
  // of answers -- the console *has* paired and this build cannot use it, and a
  // tick that cannot negotiate says so on the status screen.
  const auth::LoadedToken credentials = auth::LoadToken(PathTo(auth::kTokenFileName));
  token_ = credentials.ok() ? credentials.value : auth::StoredToken{};
  if (server_ != nullptr) {
    list_token_ = token_.access_token;
    lists_.UseServer(server_, list_token_);
  }

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

  // Beside `state.db`, and read the same way: a file that is not there is a
  // console that has overwritten nothing, which is a history and not a failure.
  // The complaints go nowhere yet -- `core/` has no logger and this is not a
  // `config.ini` problem, so putting them on `config_diagnostics()` would be the
  // placeholder M5-4 took off the settings screen. They are returned by `Load`
  // for the scheduler (M7-2, #37) to log when there is somewhere to log to.
  history_ = conflicts::History(PathTo(conflicts::kHistoryFileName));
  static_cast<void>(history_.Load());

  AdoptConfigLocked(config::LoadConfig(PathTo(config::kConfigFileName)));
}

void SdEngine::UsePairingBackend(PairingBackend backend) {
  pairing_backend_ = std::move(backend);
  if (pairing_backend_.http != nullptr && !pairing_thread_.joinable()) {
    // See the header: this is the one throwing call in the class, and it happens
    // at start rather than under a user's finger. It parks on `wake_` until
    // there is an attempt, so a console that never pairs pays a blocked thread
    // and its stack -- which is #126's to budget, along with the rest of the
    // heap the transport needs.
    pairing_thread_ = std::thread(&SdEngine::DrivePairing, this);
  }
}

void SdEngine::AdoptConfigLocked(config::LoadResult loaded) {
  config_ = std::make_shared<const config::Config>(std::move(loaded.value));
  // In the same breath, so a changed interval takes effect with no reboot and a
  // changed `server.url` lifts a TLS park (`sync::Scheduler::Reconfigure`).
  scheduler_.Reconfigure(ScheduleFrom(*config_));
  diagnostics_ = auth_diagnostics_;
  diagnostics_.reserve(diagnostics_.size() + loaded.diagnostics.size());
  for (config::Diagnostic& diagnostic : loaded.diagnostics) {
    diagnostics_.push_back(std::move(diagnostic));
  }
}

bool SdEngine::WriteQueue() {
  return download::SaveQueue(PathTo(download::kQueueFileName), queue_).ok();
}

const config::Config& SdEngine::config() const { return *config_; }

std::shared_ptr<const config::Config> SdEngine::ConfigSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

const std::vector<config::Diagnostic>& SdEngine::config_diagnostics() const {
  return diagnostics_;
}

ipc::EngineSnapshot SdEngine::Snapshot() const {
  ipc::EngineSnapshot snapshot;
  {
    // Everything the two worker threads write, in one slice under one lock --
    // which is what `EngineSnapshot` asks for: a consistent read, taken as a
    // value, never a reference into live worker state.
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.auth = auth_;
    snapshot.sync_in_progress = sync_in_progress_;
    snapshot.last_sync_at = last_sync_at_;
    snapshot.last_sync_result = last_sync_result_;
    snapshot.uploaded = uploaded_;
    snapshot.downloaded = downloaded_;
    snapshot.conflicts = conflicts_;
    snapshot.failed = failed_;
    // "The last thing this console did reached the server." Deliberately not a
    // live probe: `GetStatus` is polled every frame and may not go near the
    // network (`ipc.hpp`), so what the screen draws is the last tick's evidence
    // rather than a fresh answer nobody asked for.
    snapshot.online = last_sync_result_ == ipc::SyncResult::kOk ||
                      last_sync_result_ == ipc::SyncResult::kPartial;
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
  auth::PairingStatus status = attempt_->session.status();
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
  // The card lock, because a `server.url` change discards `token.dat`, and that
  // has to be atomic against the pairing thread committing a grant. `mutex_` is
  // taken below in slices, for `auth_`, `gate_` and the attempt; `config_` is
  // this thread's alone and needs neither (`engine.hpp`).
  std::lock_guard<std::mutex> card(card_mutex_);
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
  // `config_->server.url` is empty while the file names a perfectly good server.
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
    {
      std::lock_guard<std::mutex> lock(mutex_);
      AbandonPairingLocked();
    }
    // Every other write to `attempt_` notifies, and this one has the longest
    // wait to cut short: the thread is parked in `AwaitNextPoll` until the
    // session's next deadline, which after a `slow_down` is `max_poll_backoff`
    // away. Without this it holds the retired attempt alive for that long before
    // noticing nobody wants it.
    wake_.notify_all();
    // The verdict was about the token that has just gone, and about the server
    // that issued it. Left behind, it would put a console that has never paired
    // with the *new* server on a "pair again" screen (M1-4, #8). Not a refusal
    // if it fails: unlike the token, a stale verdict here costs a wrong sentence
    // rather than a bearer token pointed at a stranger, and the pairing is
    // already gone by this point.
    auth::ClearBlock(PathTo(auth::kAuthStateFileName));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      gate_.Reset();
    }
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
      std::lock_guard<std::mutex> lock(mutex_);
      auth_ = ipc::AuthState::kNeverPaired;
    }
    return ipc::Error::kWriteFailed;
  }

  if (server_changed) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auth_ = ipc::AuthState::kNeverPaired;
    }
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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    AdoptConfigLocked(std::move(parsed));
  }
  // The scheduler may now have something due -- a shortened interval, a switch
  // turned back on, or a park lifted -- and the worker is asleep on a deadline
  // that was computed from the configuration before this one.
  wake_.notify_all();
  return ipc::Error::kOk;
}

bool SdEngine::RequestSync() {
  bool taken = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sync_in_progress_) {
      // The one thing this is allowed to mean, and the same fact
      // `Status::sync_in_progress` carries: a tick is running. The overlay greys
      // the button off that field without pressing anything, so any other reason
      // to answer false here would put the two out of step (`ipc.hpp`).
      return false;
    }
    taken = scheduler_.RequestNow();
  }
  // Outside the lock: the worker takes `mutex_` the moment it wakes.
  wake_.notify_all();
  return taken;
}

sync::SchedulerConfig SdEngine::ScheduleFrom(const config::Config& config) {
  sync::SchedulerConfig schedule;
  schedule.enabled = config.sync.enabled;
  schedule.on_boot = config.sync.on_boot;
  // Clamped here rather than trusted: `LoadConfig` clamps what it reads, and an
  // `ApplyConfigEdit` rejects what is out of range -- but this is the one place
  // the number becomes a timer, and a timer is where a bad value costs a
  // battery (config.hpp's `kMinIntervalMinutes`/`kMaxIntervalMinutes`).
  int minutes = config.sync.interval_min;
  minutes = minutes < config::kMinIntervalMinutes ? config::kMinIntervalMinutes : minutes;
  minutes = minutes > config::kMaxIntervalMinutes ? config::kMaxIntervalMinutes : minutes;
  schedule.interval = std::chrono::minutes{minutes};
  return schedule;
}

void SdEngine::StartWorker() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (worker_thread_.joinable()) {
    return;
  }
  scheduler_.Reconfigure(ScheduleFrom(*config_));
  worker_thread_ = std::thread(&SdEngine::RunWorker, this);
}

void SdEngine::ObserveAnswer(auth::Answer answer) {
  bool persist = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool was_blocked = gate_.blocked();
    gate_.Observe(answer);
    persist = gate_.blocked() && !was_blocked;
    if (gate_.blocked()) {
      // Set in the same moment as the file is written, so the overlay draws
      // "pair this console again" on its next poll rather than after a reboot
      // (M1-4, #8 left both halves here).
      auth_ = ipc::AuthState::kUnauthenticated;
    }
  }
  if (!persist) {
    return;
  }
  // Outside `mutex_`, which is never held across an SD write, and under
  // `card_mutex_`, which is what serialises this against a pairing grant landing
  // at the same instant.
  std::lock_guard<std::mutex> card(card_mutex_);
  auth::Block block = auth::Block::kNone;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    block = gate_.block();
  }
  if (block == auth::Block::kNone) {
    // An `Unpair` landed between the two locks and reset the gate. Writing the
    // verdict now would be a file about credentials this console no longer
    // holds, which is exactly what `Load` refuses to honour.
    return;
  }
  // A failure is not reported anywhere: the file holds nothing that cannot be
  // worked out again by asking, and the cost of losing it is the rejection
  // budget on the next boot (auth_gate.hpp).
  auth::SaveBlock(PathTo(auth::kAuthStateFileName), block);
}

void SdEngine::RunWorker() {
  {
    // The wait for the network happens **here**, on this thread, and never in
    // `main`: a console that comes up with no Wi-Fi must not hold boot
    // (CLAUDE.md, docs/ARCHITECTURE.md §1). A null waiter is "the network is
    // up", which is what every host build is.
    NetworkWait wait;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
      wait = network_wait_;
    }
    if (wait) {
      wait();
    }
  }

  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    const sync::Decision decision = scheduler_.Poll();
    if (decision.run()) {
      sync_in_progress_ = true;
      lock.unlock();
      RunOneTick(decision.trigger);
      lock.lock();
      sync_in_progress_ = false;
      continue;
    }

    // A list page is one request and somebody is looking at a screen waiting for
    // it, so it is pumped between ticks rather than on a thread of its own
    // (`StartWorker`). `Pump()` returns false immediately when there is nothing
    // to do, so an idle loop pays one function call.
    lock.unlock();
    const bool pumped = PumpLists();
    lock.lock();
    if (pumped) {
      continue;
    }

    if (stopping_) {
      break;
    }
    if (decision.parked) {
      // No deadline at all. This is the idle cost the whole scheduler exists to
      // avoid: a switched-off or boot-only console waits to be woken by a
      // command and costs nothing in between (scheduler.hpp).
      wake_.wait(lock);
    } else {
      wake_.wait_for(lock, decision.sleep_for);
    }
  }
}

namespace {

/// Where a save or a state the console has no local copy of should be written.
///
/// Empty when there is nowhere to put it, which is what `ExecuteOptions::place`
/// and `StateSyncOptions::place` both document as "fail this one rather than
/// guessing" -- and it is the honest answer for a rom this client's index does
/// not hold, or a platform the user never mapped a folder for.
///
/// **The server's file name is never trusted.** A `file_name` carrying a
/// separator, or one that is `.` or `..`, would let a sync plan name a path
/// outside the folder the user mapped; `sync::Validate` refuses the same shapes
/// on the way in, and this refuses them again on the way out because this is the
/// call that turns a name into a path.
std::string PlaceUnder(const std::vector<std::string>& folders, std::string_view file_name) {
  if (folders.empty() || file_name.empty() || file_name == "." || file_name == ".." ||
      file_name.find('/') != std::string_view::npos ||
      file_name.find('\\') != std::string_view::npos) {
    return {};
  }
  // The first entry is the write target; the rest are only ever consulted when
  // asking whether something is already there (config.hpp).
  return folders.front() + "/" + std::string(file_name);
}

}  // namespace

void SdEngine::RunOneTick(sync::Trigger trigger) {
  (void)trigger;

  const std::shared_ptr<const config::Config> config = ConfigSnapshot();
  http::HttpClient* client = nullptr;
  fs::FileSystem* files = nullptr;
  auth::StoredToken token;
  bool blocked = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = server_;
    files = card_;
    token = token_;
    blocked = gate_.blocked();
  }

  if (blocked) {
    // The server has stopped accepting these credentials and the remedy is a
    // person at a pairing screen. `auth::Gate` is the only thing that lifts it,
    // and `kUnauthorized` is what tells the scheduler not to invent a backoff
    // over a decision that is not its (scheduler.hpp).
    std::lock_guard<std::mutex> lock(mutex_);
    scheduler_.Finished(sync::TickOutcome::kUnauthorized);
    return;
  }
  if (client == nullptr || files == nullptr || token.access_token.empty() ||
      !config->configured()) {
    // A build with no transport, a console with no card backend, one that has
    // never paired, or one with no `server.url`. **`kOffline` rather than a
    // member of its own**: nothing was written, nothing reached a server, and
    // the answer -- back off and try again later -- is the same one a console on
    // a train gets. The status screen says which of the four it is; the schedule
    // does not need to know.
    std::lock_guard<std::mutex> lock(mutex_);
    scheduler_.Finished(sync::TickOutcome::kOffline);
    last_sync_result_ = ipc::SyncResult::kFailed;
    return;
  }

  // Step 0, the half `sync::RunTick` deliberately does not own: the library the
  // scan matches against, and the scan itself.
  roms::FetchOptions fetch;
  fetch.base_url = config->server.url;
  fetch.bearer_token = token.access_token;
  const roms::FetchResult library = roms::FetchRomIndex(*client, fetch);
  ObserveAnswer(roms::AnswerOf(library));
  if (!library.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    scheduler_.Finished(library.status == 401 || library.status == 403
                            ? sync::TickOutcome::kUnauthorized
                            : sync::TickOutcome::kOffline,
                        library.transport);
    last_sync_result_ = ipc::SyncResult::kFailed;
    return;
  }

  const std::string baseline_path = files->Resolve(sync::kStateSdPath);
  const state::LoadedBaseline loaded = state::LoadBaseline(baseline_path);
  const scan::ScanResult scanned = scan::ScanSaves(*config, library.index, *files, loaded.value);

  std::vector<sync::ClientSaveState> reported;
  std::vector<sync::SaveTarget> targets;
  reported.reserve(scanned.saves.size());
  targets.reserve(scanned.saves.size());
  for (const scan::SaveFile& save : scanned.saves) {
    reported.push_back(save.ToClientSaveState());
    sync::SaveTarget target;
    target.rom_id = save.rom_id;
    target.slot = save.slot;
    target.sd_path = save.sd_path;
    target.file_name = save.file_name;
    targets.push_back(std::move(target));
  }

  sync::TickOptions options;
  // The switch, passed **into** the tick rather than re-decided here: the gate
  // one level down is what makes "a disabled sysmodule makes no network call"
  // true against a second caller (M6-2, #33). The scheduler never gets this far
  // with it off, which is the other half of the same promise.
  options.enabled = config->sync.enabled;
  options.execute.backup_dir = sync::kBackupDir;
  options.finish.state_sd_path = sync::kStateSdPath;
  options.cancel = &tick_cancel_;
  // The save folders and `.backup/`, and deliberately **not** `/config/rommsync`
  // itself: those records recover from their own `.old` when they are read, and
  // the overlay writes `config.ini` from another thread, where a sweep removing
  // a `.tmp` between the write and its rename would cost a setting the user had
  // just changed (`sync::RecoverStaging`).
  options.recover_dirs = config->SaveScanDirs();
  options.recover_dirs.push_back(std::string(sync::kBackupDir));
  const roms::RomIndex* index = &library.index;
  options.execute.place = [index, config](const sync::SyncOperation& operation) -> std::string {
    const roms::Rom* rom = index->ById(operation.rom_id);
    if (rom == nullptr) {
      return {};
    }
    const config::PlatformFolders* folders = config->Platform(rom->platform_fs_slug);
    return folders == nullptr ? std::string() : PlaceUnder(folders->saves, operation.file_name);
  };

  const sync::TickResult tick =
      sync::RunTick(*client, *files, token, reported, targets, loaded.value, options);
  ObserveAnswer(tick.answer);

  // M7-1 (#36)'s call site, which nothing in this build had. It is outside
  // `sync::RunTick` because `conflict_record.hpp` includes `state_sync.hpp`, so
  // recording from inside would be a cycle -- the same reason persisting the
  // baseline after `SyncStates` is the caller's.
  conflicts::RecordOptions record;
  record.rom_name = [index](std::int64_t rom_id) -> std::string {
    const roms::Rom* rom = index->ById(rom_id);
    // The index carries `fs_name_no_ext` and no display name at all
    // (rom_index.hpp), so that is what a row gets. An empty answer is not a
    // failure: the screen falls back to the file name.
    return rom == nullptr ? std::string() : rom->fs_name_no_ext;
  };
  {
    std::lock_guard<std::mutex> history(history_mutex_);
    conflicts::RecordSaves(&history_, tick.negotiated.plan, tick.executed, reported, record);
  }

  sync::StateSyncReport states;
  if (config->sync.states && tick.outcome != sync::TickOutcome::kCanceled &&
      tick.outcome != sync::TickOutcome::kDisabled) {
    // The baseline is re-read rather than carried: `sync::RunTick` persisted its
    // own copy on the way out, and `SyncStates` advances the same file. Reading
    // it back is one small file and is what keeps the two halves from writing
    // over each other's rows.
    state::Baseline baseline = state::LoadBaseline(baseline_path).value;
    sync::StateSyncOptions state_options;
    state_options.backup_dir = sync::kBackupDir;
    state_options.cancel = &tick_cancel_;
    state_options.place = [index, config](const sync::ServerState& server) -> std::string {
      const roms::Rom* rom = index->ById(server.rom_id);
      if (rom == nullptr) {
        return {};
      }
      const config::PlatformFolders* folders = config->Platform(rom->platform_fs_slug);
      return folders == nullptr ? std::string() : PlaceUnder(folders->states, server.file_name);
    };
    states = sync::SyncStates(*client, *files, token, *config, library.index, &baseline,
                              state_options);
    // Persisting it is the caller's, deliberately (state_sync.hpp).
    state::SaveBaseline(baseline_path, baseline);
    std::lock_guard<std::mutex> history(history_mutex_);
    conflicts::RecordStates(&history_, states, record);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  scheduler_.Finished(tick);
  last_sync_at_ = static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          scheduler_.last_tick_at().time_since_epoch())
          .count());
  // Counted off the operations rather than off `TickCompletion::counts`, which
  // carries only what the *server* is told -- completed and failed -- and not
  // which way each transfer went. The status screen draws three separate
  // numbers (`ipc::Status`), and a conflict is an overwrite the server arbitrated
  // rather than a fourth outcome, so it is counted where the backup was written.
  uploaded_ = 0;
  downloaded_ = 0;
  conflicts_ = 0;
  for (const sync::OperationResult& operation : tick.executed.operations) {
    if (operation.outcome == sync::OperationOutcome::kUploaded) {
      ++uploaded_;
    } else if (operation.outcome == sync::OperationOutcome::kDownloaded) {
      ++downloaded_;
    }
    if (!operation.backup_sd_path.empty()) {
      ++conflicts_;
    }
  }
  for (const sync::StateOperationResult& operation : states.operations) {
    if (operation.outcome == sync::StateOutcome::kUploaded) {
      ++uploaded_;
    } else if (operation.outcome == sync::StateOutcome::kDownloaded) {
      ++downloaded_;
    }
  }
  failed_ = static_cast<std::int64_t>(tick.executed.failed) +
            static_cast<std::int64_t>(states.failed);
  switch (tick.outcome) {
    case sync::TickOutcome::kCompleted:
      last_sync_result_ = states.failed > 0 ? ipc::SyncResult::kPartial : ipc::SyncResult::kOk;
      break;
    case sync::TickOutcome::kPartial:
    case sync::TickOutcome::kUnreported:
      last_sync_result_ = ipc::SyncResult::kPartial;
      break;
    case sync::TickOutcome::kCanceled:
    case sync::TickOutcome::kDisabled:
    case sync::TickOutcome::kRescanNeeded:
      // Nothing happened that a status screen should report as a sync. The last
      // result stands, which for a first boot is `kNever`.
      break;
    case sync::TickOutcome::kOffline:
    case sync::TickOutcome::kRefused:
    case sync::TickOutcome::kUnauthorized:
      last_sync_result_ = ipc::SyncResult::kFailed;
      break;
  }
}

ipc::Error SdEngine::StartPairing() {
  // Asked again here rather than trusted from `ServiceCore::StartPair`, which
  // refuses an unconfigured console before the engine is reached: `Engine` is an
  // interface anything may drive, and starting an attempt against an empty
  // origin would spend the console's device identifier on a request to nowhere.
  if (!config_->configured()) {
    return ipc::Error::kNotConfigured;
  }
  if (pairing_backend_.http == nullptr) {
    // Nobody called `UsePairingBackend`. On a console that no longer happens --
    // `main.cpp` installs M1-7 (#126)'s Horizon client at boot -- so this is
    // reached by a host binary that never wired one, and by the tests that pin
    // the refusal. Kept rather than asserted away: an engine with no transport
    // has to answer *something*, and `kUnavailable` says "this build cannot",
    // which is the sentence the pairing screen draws. Nothing was attempted and
    // nothing changed, exactly as `ipc.hpp` promises.
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
    // duplicating the console. `kWriteFailed` for that and for `kPersistFailed`
    // -- the card is what stopped this, and nothing was written.
    //
    // **`kNoSeed` is the one that is not about the card**, which is why it
    // answers differently. It means the platform layer handed over a seed with
    // no usable stable value and too little entropy, and `device_identity.hpp`
    // requires that layer to *fail rather than substitute* -- so reaching here
    // is a defect in this build, not a state a user is in. `kWriteFailed` would
    // send them to look at an SD card that is fine. `kInternal` is `ipc.hpp`'s
    // "failed in a way it could not name", and while this one has a name, the
    // name is not one the overlay can turn into an action.
    return identity.error == auth::IdentityError::kNoSeed ? ipc::Error::kInternal
                                                          : ipc::Error::kWriteFailed;
  }

  auth::PairingConfig pairing;
  pairing.server_url = config_->server.url;
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
    //
    // **It is discarded, not interrupted.** `http::HttpClient` has no way to be
    // cancelled from outside a call -- `CancelToken` is passed *into* one -- and
    // `PairingSession` owns its requests, so a thread inside the previous
    // attempt's `Begin()` has to finish it before it picks this one up. The
    // screen therefore reads "starting" for up to one `request_timeout` after a
    // second press. Right in every case that matters (a first attempt, or one
    // whose code is live and only polling) and wrong-looking only on the press
    // that follows one hung init; fixing it needs a `CancelToken` on
    // `PairingConfig`, which is `core/`'s to add.
    attempt_ = std::make_shared<PairingAttempt>(*pairing_backend_.http, std::move(pairing),
                                                config_->server.url);
    attempt_commit_failure_.clear();
  }
  wake_.notify_all();
  // `pairing_status()` already answers `kStarting`, which is the contract
  // `ipc::Engine::StartPairing` states: `ServiceCore::StartPair` reads it one
  // instant later and must not see `kIdle`.
  return ipc::Error::kOk;
}

void SdEngine::AbandonPairingLocked() {
  attempt_.reset();
  attempt_commit_failure_.clear();
}

bool SdEngine::AwaitNextPoll(const std::shared_ptr<PairingAttempt>& attempt) {
  // Until the session says the next request is due, rather than on a tick of our
  // own. `PairingSession::Poll` enforces RomM's interval itself and answers a
  // premature call by doing nothing, so a fixed tick would wake this thread a
  // few thousand times over a ten-minute code to be told that each time -- on a
  // process that is resident for the life of the console. Waiting *until* the
  // deadline it publishes costs one wake-up per poll actually made, and the
  // condition variable still cuts the wait short for a new attempt or for
  // shutdown, which is what promptness there actually depends on.
  const std::chrono::steady_clock::time_point due = attempt->session.next_poll_at();
  std::unique_lock<std::mutex> lock(mutex_);
  wake_.wait_until(lock, due, [this, &attempt] { return stopping_ || attempt_ != attempt; });
  return !stopping_ && attempt_ == attempt;
}

void SdEngine::DrivePairing() {
  for (;;) {
    std::shared_ptr<PairingAttempt> attempt;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || attempt_ != driven_; });
      if (stopping_) {
        return;
      }
      driven_ = attempt_;
      attempt = attempt_;
      if (attempt == nullptr) {
        // A `server.url` change abandoned the attempt before this thread ever
        // picked it up. There is nothing to drive.
        continue;
      }
    }

    // The request `StartPairing` refused to wait for. Everything from here down
    // is off the IPC thread, so it may take as long as `request_timeout`.
    auth::PairingState state = attempt->session.Begin();
    while (!auth::IsTerminal(state) && AwaitNextPoll(attempt)) {
      state = attempt->session.Poll();
    }
    if (state != auth::PairingState::kApproved) {
      // Denied, expired, failed -- or superseded, in which case the attempt that
      // replaced this one is what the next turn of this loop picks up. All four
      // leave the card exactly as it was.
      continue;
    }
    const auth::DeviceTokenResponse* granted = attempt->session.token();
    if (granted != nullptr) {
      CommitGrant(attempt, *granted);
    }
  }
}

void SdEngine::CommitGrant(const std::shared_ptr<PairingAttempt>& attempt,
                           const auth::DeviceTokenResponse& granted) {
  // The card lock for the whole commit, so an `Unpair` or a `server.url` change
  // cannot land between the two writes below. `mutex_` is taken inside it, in
  // slices, and never across a write -- `GetStatus` and `GetPairState` are
  // polled every frame and may not wait on an SD card (`engine.hpp`).
  std::lock_guard<std::mutex> card(card_mutex_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || attempt_ != attempt) {
      // A newer attempt is running, or the engine is going away. The grant is
      // real and its code is spent, so this does cost the user one approval --
      // but writing it would put a token on the card that the attempt the
      // overlay is drawing knows nothing about, and the attempt that replaced
      // this one will commit its own.
      //
      // Checked under `card_mutex_`, which is what settles it against the one
      // racer that would be dangerous: `ApplyConfigEdit` clears `attempt_` and
      // discards the token while holding that lock, so a `server.url` change
      // cannot slip between this check and the write below and leave the new
      // server's console holding the old server's token.
      //
      // It is *not* atomic against `StartPairing`, which replaces `attempt_`
      // under `mutex_` alone -- deliberately, because a command may not wait on
      // an SD write (`ipc.hpp`). So a user who presses "start over" in the
      // moment between this check and the write gets A1's token committed under
      // A2's screen. That end state is honest rather than wrong: A1 really was
      // approved, the token really does work, and A2 overwrites it if the user
      // finishes it. What the lock buys is that the *card* is never spliced.
      return;
    }
  }

  // M1-5 (#9)'s write, unchanged: the record goes to `token.dat.tmp` and is
  // renamed onto `token.dat`, so a reader sees the previous token or the new one
  // and never a splice. `device.dat` is untouched -- the identifier has to
  // survive a re-pair or RomM registers the console twice.
  const auth::StoredToken record = auth::StoredTokenFrom(attempt->server_url, granted);
  const auth::StoreResult saved = auth::SaveToken(PathTo(auth::kTokenFileName), record);
  if (!saved.ok()) {
    // Said out loud on the pairing screen rather than left to be discovered at
    // the next sync tick: the approval is spent, so the remedy is to pair again,
    // and a screen still reading `kApproved` would tell the user the opposite.
    std::lock_guard<std::mutex> lock(mutex_);
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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    gate_.Reset();
    auth_ = ipc::AuthState::kPaired;
    // The credentials the worker and the lists use, replaced in the same slice
    // as the state that says the console is paired -- so the first tick after a
    // pairing runs on the token that pairing just produced rather than on
    // whatever was there at boot (M7-2, #37).
    token_ = record;
    list_token_ = record.access_token;
    if (server_ != nullptr) {
      lists_.UseServer(server_, list_token_);
    }
  }
  // A paired console usually has a tick due: `[sync] on_boot` fired long before
  // there were credentials to fire it with.
  wake_.notify_all();
}

ipc::Error SdEngine::Unpair() {
  // The pairing thread commits a grant under this same card lock, so a discard
  // and a commit cannot interleave: one of them happens whole. `mutex_` is taken
  // afterwards, for the two assignments only, so nothing polled every frame
  // waits on these writes. **It does not abandon a live attempt**, and that is
  // the settings screen's whole design -- "Re-pair" sends `StartPair` first and
  // `Unpair` only once an attempt is under way, so the console never passes
  // through "unpaired with nothing to restart" (docs/AUTH.md, M4-4 #26).
  std::lock_guard<std::mutex> card(card_mutex_);
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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    gate_.Reset();
    auth_ = ipc::AuthState::kNeverPaired;
    // The token is off the card, so the copies in memory go with it. A worker
    // holding the discarded credentials would keep negotiating with them until
    // the next boot.
    token_ = auth::StoredToken{};
    list_token_.clear();
    if (server_ != nullptr) {
      lists_.UseServer(server_, list_token_);
    }
  }
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

/// Whether the backup an entry names is still on the card
/// (`ipc::ConflictRow::backup_present`).
///
/// **This looks, with or without an `fs::FileSystem`.** Reading whether a file
/// is there is one `io::Exists`, and this file already opens `sdmc:` paths
/// directly for `config.ini`, `token.dat` and `queue.json` -- so a build with no
/// card interface installed still answers honestly rather than claiming the
/// backup is there. Answering "yes" blind would draw every entry as restorable
/// and fail at the press, which is the outcome #36 exists to replace.
///
/// A `card_` is preferred when there is one because the host suite's is rooted
/// at a sandbox rather than at `sdmc:`.
bool SdEngine::BackupPresent(const conflicts::Entry& entry) const {
  if (!entry.restorable()) {
    return false;
  }
  const std::string resolved = card_ != nullptr ? card_->Resolve(entry.backup_sd_path)
                                                : std::string(kSdRoot) + entry.backup_sd_path;
  return !resolved.empty() && io::Exists(resolved);
}

ipc::Error SdEngine::ListConflicts(const ipc::ConflictQuery& query, ipc::ConflictPage* page) {
  // The worker appends to this after every half of a tick now (M7-2, #37), so
  // reading it is no longer a single-threaded read.
  std::lock_guard<std::mutex> history(history_mutex_);
  const std::vector<conflicts::Entry>& entries = history_.entries();
  page->offset = query.offset;
  page->total = static_cast<std::int32_t>(entries.size());

  // Past the end is an empty page rather than a refusal: a history that shrank
  // under an open screen is normal, and the screen's next request corrects it.
  std::size_t at = static_cast<std::size_t>(query.offset);
  const std::size_t wanted = static_cast<std::size_t>(query.limit);
  for (; at < entries.size() && page->entries.size() < wanted; ++at) {
    ipc::ConflictRow row;
    row.entry = entries[at];
    row.backup_present = BackupPresent(entries[at]);
    if (!ipc::AppendIfItFits(page, std::move(row))) {
      // The byte bound, not the count: the page stops here and says so, and the
      // caller's next offset is this one. See `ipc::ConflictQuery`.
      break;
    }
  }
  page->has_more = at < entries.size();
  return ipc::Error::kOk;
}

ipc::Error SdEngine::RestoreBackup(std::int64_t entry_id, conflicts::RestoreReport* report) {
  // Held across the restore, which copies two files on the card: a tick
  // appending an entry underneath a restore reading one would be a race on the
  // same vector.
  std::lock_guard<std::mutex> history(history_mutex_);
  if (card_ == nullptr) {
    // Nothing implements `fs::FileSystem` for Horizon yet (`engine.hpp`).
    // `kBackupFailed` rather than a transport failure: its promise is that
    // nothing was written, which is the one thing a build that cannot open the
    // card is certain of.
    report->outcome = conflicts::RestoreOutcome::kBackupFailed;
    report->message = "this build cannot open the SD card, so nothing was written";
    return ipc::Error::kOk;
  }
  *report = conflicts::Restore(*card_, history_, entry_id);
  return ipc::Error::kOk;
}

}  // namespace rommsync::sysmodule
