// What `ipc::ServiceCore` reads the console's state out of, as of M4-1.
//
// `ipc::Engine` is the seam the whole command set is driven through, and it was
// written before most of what sits behind it existed (`ipc.hpp`): the download
// queue is M3-2 (#19), live `config.ini` writes are M5-3 (#30), list paging is
// M5-4 (#31), the scheduler is M7-2 (#37). This is the implementation they each
// filled a part of.
//
// **It answers only what this build actually knows.** The console's
// configuration -- which it now writes as well as reads (M5-3) -- whether it has
// ever paired, whether the server has stopped accepting its token (M1-4, #8),
// its download queue (M3-2, #19) and, since M5-4 (#31), the three lists the
// overlay browses are on the SD card and are read from it; everything else is
// `ipc::Error::kUnavailable`, which is a sentence the overlay can draw rather
// than a plausible refusal that sends a user looking for a full queue or a
// failing SD card. Each of the issues above replaces its own part of this, and
// `kUnavailable` disappearing entirely is what says the engine is finished --
// `StartPairing` on a build with no transport is what is left of it.
//
// **The console has a transport and a worker.** M1-7 (#126) built the Horizon
// `http::HttpClient` (`sysmodule/source/http/`) and `main.cpp` holds one; M7-2
// (#37) hands it to `UseServer` and starts the thread that drives `PumpLists()`
// in the same breath, because either alone is wrong -- a client with no worker
// turns `kOffline`, which #25's browser draws, into "pending" forever, which it
// cannot. That thread is also the one that runs `sync::RunTick` on a schedule,
// which is what makes `SyncNow` start something and `auth.json` a file this
// build writes rather than only reads. The host suite passes a libcurl client
// and drives the paging through the same seam (`lists.*`).
//
// **Three threads now, and what each one owns.** The IPC thread answers every
// command and is the only one that writes `config_` and `queue_`; the pairing
// thread (M1-6, #123) drives one device-code attempt; the worker (M7-2, #37)
// runs ticks and pumps list pages. None of them blocks on the network while
// holding `mutex_`, and the configuration crosses between them as a snapshot
// rather than as a reference -- see `ConfigSnapshot`.
//
// **`StartPairing` is answered here, and that is what M1-6 (#123) was for.** The
// engine drives a real device-code attempt on a thread of its own -- see
// `StartPairing` -- over the same Horizon client the lists are withheld from,
// because this class owns the thread that drives it and `lists::Service` does
// not own the one that would drive `Pump()`. It still answers `kUnavailable` to
// a caller that never installed a backend, which on a console no longer happens
// and in the host harness is exactly what `engine.commands` pins. The harness
// has libcurl's client and pairs against a real RomM with it (`engine.pairs`).
//
// Nothing here has ever run: it is Horizon-side and is exercised in Ryujinx
// before the M8-1 gate, never on hardware (sysmodule/AGENTS.md).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/config.hpp"
#include "rommsync/conflict_log.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/download.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/list_service.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/play_sessions.hpp"
#include "rommsync/scheduler.hpp"
#include "rommsync/sync_tick.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::sysmodule {

/// Where the client's own files live on the card. Named here rather than in
/// `core/`, which owns the file *names* and may not know an SD path (hard
/// rule 4, and the `sdmc:` prefix is libnx's).
inline constexpr const char* kConfigDir = "sdmc:/config/rommsync/";

/// What an SD-root path from `core/` is prefixed with to open it here.
///
/// The mapping `fs::FileSystem::Resolve` performs, spelled once for the callers
/// that have no `fs::FileSystem` to ask -- which on the console today is all of
/// them, because nothing implements one for Horizon yet. `kConfigDir` is this
/// plus the directory `core/` names, and is kept as its own constant because it
/// is what `Load` is *given* rather than something it derives.
inline constexpr const char* kSdRoot = "sdmc:";

/// How long the worker waits for the network before its first tick, and how
/// often it asks (M7-2, #37). See `SdEngine::AwaitNetwork` for why it is bounded
/// at all.
inline constexpr std::chrono::seconds kNetworkWaitBudget{120};
inline constexpr std::chrono::seconds kNetworkPollInterval{2};

/// What a pairing attempt needs that neither `core/` nor this file can supply.
///
/// Both are platform facilities. `core/` may not name a transport (hard rule 4),
/// and the console's stable value for `client_device_identifier` comes from
/// `setsysGetSerialNumber`, which is a libnx call and so belongs to `main.cpp`
/// rather than to a file the host build compiles. Injecting them is what lets
/// `engine.pairs` drive a whole pairing against the docker RomM with no console.
struct PairingBackend {
  /// **Null means this build has no way to reach a server**, which is the
  /// console's situation today: the Horizon `ssl` `HttpClient` backend is listed
  /// in the M8-1 gate (#43) and is not written. `StartPairing` answers
  /// `ipc::Error::kUnavailable` then -- "this build has no engine behind that
  /// command" -- rather than a refusal that blames the server or the card.
  ///
  /// Not owned, and must outlive the engine: the pairing thread holds it for as
  /// long as an attempt is running.
  http::HttpClient* http = nullptr;

  /// What `auth::LoadOrCreateDeviceIdentity` derives `device.dat` from the first
  /// time this console pairs. Consulted only when there is no file, so on every
  /// boot after the first it does not matter what is in it
  /// (`device_identity.hpp`).
  auth::IdentitySeed identity_seed;
};

/// The engine as far as it is built.
///
/// Constructed once at start and driven from the IPC thread -- and, since M1-6,
/// from one thread of its own. Three things change after `Load()`: the queue,
/// the configuration (M5-3, #30), and the pairing state.
///
/// **Two locks, and which one a method needs is decided by whether it touches
/// the card.** `card_mutex_` serialises the writes to `token.dat` and
/// `auth.json`, so a grant landing and an `Unpair` discarding cannot interleave;
/// `mutex_` guards the in-memory state the two threads share -- the attempt,
/// `auth_`, `gate_` -- and is never held across I/O, so the commands the
/// overlay polls every frame never wait on an SD card. Order is `card_mutex_`
/// then `mutex_`, never the reverse.
///
/// **`history_` and a restore are under neither, and that is deliberate.** M7-1
/// (#36) added two commands that touch the card from the IPC thread, and they
/// share no file with the pairing thread: that one writes `token.dat` and
/// `auth.json` under `card_mutex_`, a restore writes a save and a copy under
/// `.backup/`. Taking `card_mutex_` for a restore would make a pairing grant
/// wait behind a save-state copy of tens of megabytes, which is the thing the
/// two-lock split exists to prevent. What would change that is a second writer
/// of `.backup/` -- the scheduler running a tick (M7-2, #37) -- and that is the
/// moment `history_` needs a lock of its own, not this one.
///
/// `config_` and `queue_` are deliberately *not* under it, and that is still a
/// fact about today rather than a decision to keep: `ServiceServer::Run` is a
/// single `svcReplyAndReceive` loop, so every command runs on one thread, and
/// the pairing thread reads neither -- it is handed the server URL it needs when
/// the attempt starts. `download::Queue` carries its own mutex for the reason
/// `ipc::Engine` states.
///
/// **A mutex is not what would make `config()` safe the day a worker reads it.**
/// It hands out a *reference*, so a swap under a lock would still free a
/// `Config` a caller is holding. The seam to change then is that signature -- a
/// snapshot, the way `Snapshot()` already answers -- and workers have to take
/// theirs at a tick boundary, never mid-sync and never mid-download. The
/// download worker is not started here yet; when it is, this is already the
/// object it drains.
class SdEngine : public ipc::Engine {
 public:
  SdEngine();

  /// Stops the pairing thread and waits for it. A request already in flight
  /// still has to finish -- `http::HttpClient` has no way to be interrupted from
  /// outside a call -- so this can take as long as one `request_timeout`. The
  /// console never gets here: `main` never leaves its service loop.
  ~SdEngine() override;

  /// Read `config.ini`, look for `token.dat`, and read `queue.json`. Never
  /// fails: a console with no config is a console with the defaults, one with
  /// no token has never paired, and one with no queue has queued nothing --
  /// all three are states the overlay draws (`config.hpp`, `download.hpp`).
  ///
  /// `config_dir` is `kConfigDir` on the console and is a parameter so this
  /// class can be driven on the host. **Nothing in this file names a libnx
  /// type**, so the only thing that tied it to Horizon was the `sdmc:` prefix,
  /// and a seam the size of one string buys `engine.commands` -- a test of the
  /// engine behind `ipc::Dispatch`, against a directory, with no console. The
  /// alternative is glue that is only ever proven by the fact that it compiles.
  void Load(const std::string& config_dir = kConfigDir);

  /// Give the engine the platform facilities a pairing attempt needs, and start
  /// the thread that will drive attempts. Call it before the service starts
  /// answering; it is not meant to change under a running attempt.
  ///
  /// **The thread is started here rather than on the first `StartPairing`, and
  /// that is about `-fno-exceptions`** (`switch.mk`): `std::thread`'s
  /// constructor throws when the thread or its stack cannot be created, and a
  /// throw on the console calls `std::terminate`. Failing at start is what the
  /// rest of `main.cpp` already does with `sm` and `fs` -- a sysmodule that
  /// cannot build what it needs should not come up half-working -- whereas
  /// failing on a button press would kill the process under the user's hands and
  /// leave a pairing screen that never moves. A backend with no transport starts
  /// nothing, which is every console today.
  ///
  /// Separate from `UseServer` below, and it stays that way. Both hand this
  /// class an `http::HttpClient*`, and while that started as an accident -- M5-4
  /// (#31) and M1-6 (#123) landed in parallel and each needed one -- M1-7 (#126)
  /// gave the console a real transport and the two did not collapse into one
  /// call, because they are not the same promise. This one is safe the moment a
  /// client exists, because the pairing thread is this class's own, where
  /// `UseServer` was not until M7-2 (#37) started the worker that drives
  /// `PumpLists()`. They stay two setters even now that both are installed on
  /// the same line of `main.cpp`: they are two promises, and one call would make
  /// it impossible to install the half that is ready -- which is the state this
  /// build spent two milestones in.
  void UsePairingBackend(PairingBackend backend);

  /// Start the worker: the one thread that runs sync ticks and drives the list
  /// paging (M7-2, #37).
  ///
  /// Call it **after** `Load` and after whichever of `UseServer` and `UseCard`
  /// this build has, because those are what decide whether the loop has anything
  /// to do. Starting it twice does nothing.
  ///
  /// **It is one thread, not two.** `sync::RunTick` and `lists::Service::Pump`
  /// both want a thread that is not the IPC one and neither wants a thread of
  /// its own: a list page is one request and a tick is a handful, and two
  /// threads would cost two stacks out of the 768 KiB inner heap to serialise on
  /// the same `http::HttpClient` anyway -- and that heap's table already budgets
  /// for exactly two worker stacks (`kInnerHeapSize`, `main.cpp`). The loop pumps the
  /// lists first, because a human is waiting at a screen for one of those and
  /// nobody is waiting for a tick.
  ///
  /// **Nothing here blocks boot.** The thread is started and `main` returns; the
  /// wait for the network happens on this thread (`WaitForNetwork`), not before
  /// the service comes up.
  ///
  /// Like `UsePairingBackend`, the `std::thread` constructor is the one throwing
  /// call and it happens at start rather than under a user's thumb
  /// (`-fno-exceptions`, `switch.mk`).
  void StartWorker();

  /// Whether the console has an internet connection right now.
  ///
  /// **Injected, because `nifm` is libnx and this file names no libnx type.**
  /// `main.cpp` passes one line asking `nifmGetInternetConnectionStatus`; a
  /// build that passes nothing is treated as connected, which is what a laptop
  /// is.
  ///
  /// A *probe* rather than a wait, so the waiting -- the budget, the poll
  /// interval, and noticing that the process is going away -- happens on this
  /// side of the seam, where `stopping_` is visible. A wait that slept inside
  /// libnx would make a shutdown during boot take the whole budget.
  ///
  /// Called on the **worker thread**, never on the IPC one, and never while
  /// `mutex_` is held. Boot is not held either way: `main` starts this thread
  /// and returns.
  using NetworkProbe = std::function<bool()>;
  void UseNetworkProbe(NetworkProbe probe);

  /// The network the library is read over, and the token to read it with.
  ///
  /// **Null and empty on the console today, and no longer for want of a
  /// backend.** M1-7 (#126) built the Horizon one and `main.cpp` holds a client;
  /// what is missing is the thread. Handing one over without a `PumpLists()`
  /// worker leaves every page that needs a request `pending` forever -- see the
  /// note at the top of this file -- so `platforms` and `roms` answer
  /// `ipc::Error::kOffline` while `queue`, which never touches the network, is
  /// served in full. The host suite passes a libcurl client and the fixture
  /// token, which is what proves the paging (`lists.*`). **M7-2 (#37) fills this
  /// seam and starts that worker in the same commit**; neither alone is correct.
  /// **An empty `bearer_token` means "the one on the card"**, which is what the
  /// console passes: `token.dat` is this class's to read, it is re-read when a
  /// pairing commits, and a `main.cpp` that had to fish the token out of it
  /// would be a second reader of the one file that must not be read twice
  /// differently. The host suite passes the fixture token explicitly, which is
  /// what proves the paging against a server this class never paired with.
  void UseServer(http::HttpClient* client, std::string bearer_token);

  /// The SD card, behind `fs::FileSystem`. `sysmodule/source/card.hpp` is the
  /// Horizon implementation (M7-2, #37) and `main.cpp` installs it; the host
  /// suite passes its sandbox's. A null one leaves a rom row's `on_disk` false,
  /// which is the honest answer for a build that cannot look
  /// (`list_service.hpp`), and refuses a restore with `kBackupFailed`.
  ///
  /// It is also what a **tick** reads and writes every save through, so a
  /// console with none never gets past the scan.
  void UseCard(fs::FileSystem* filesystem);

  /// Drive one list page fetch, from a thread that is **not** the IPC one.
  ///
  /// That thread is the worker `StartWorker` starts (M7-2, #37), which calls
  /// this between ticks -- one loop for both, not two threads. The host suite
  /// calls it directly between two `ListNext`s, which is what makes every paging
  /// case deterministic rather than timed. The pairing thread is **not** it:
  /// that one drives one attempt and touches nothing a list reads.
  ///
  /// It returns immediately and `false` when there is nothing to do, so an idle
  /// worker loop pays one function call.
  bool PumpLists();

  const config::Config& config() const override;
  const std::vector<config::Diagnostic>& config_diagnostics() const override;
  ipc::EngineSnapshot Snapshot() const override;
  auth::PairingStatus pairing_status() const override;

  /// M5-3 (#30). `SetSyncEnabled` is one assignment through `ApplyConfigEdit`,
  /// so the enable switch (#24) and the settings screen (#26) cannot come to
  /// different conclusions about what a write means.
  ///
  /// The whole file's complaints are **not** what comes back in `diagnostics`:
  /// those belong to `GetConfig`, which the settings screen already polls, and
  /// returning both would show every one of them twice. What comes back is what
  /// this edit had to say.
  ipc::Error SetSyncEnabled(bool enabled) override;
  ipc::Error ApplyConfigEdit(const ipc::ConfigEdit& edit,
                             std::vector<config::Diagnostic>* diagnostics) override;
  /// M7-2 (#37). True when a tick was started, false when one is already
  /// running -- **exactly that and nothing else**, which is what keeps this and
  /// `Status::sync_in_progress` the same fact. An implementation that answered
  /// false for some other reason would put the overlay's button and the
  /// sysmodule back out of step (`ipc.hpp`, and `overlay.sync_actions` is what
  /// would go red).
  ///
  /// A console with no transport, no token or no server still answers true: the
  /// tick starts, finds it cannot negotiate, and the status screen says so.
  /// Refusing here would report "a sync is already running" for a console that
  /// is merely offline, which is the sentence this issue exists to remove.
  bool RequestSync() override;
  ipc::Error StartPairing() override;

  /// M1-4 (#8). Discard `token.dat` and the verdict beside it, and report the
  /// console as never paired.
  ///
  /// This is the half of "re-pairing recovers without a restart" the engine
  /// owns: an unauthenticated console is one whose `auth.json` says so, and
  /// nothing else lifts that -- `auth::Gate::Reset` is the only exit, by design,
  /// because a client that is not calling cannot be told its token works.
  ///
  /// The other half is `StartPairing`, which M1-6 (#123) built. The settings
  /// screen's "Re-pair" button (M4-4, #26) is the one caller, and it still sends
  /// `StartPair` **first** -- a refused `StartPair` writes nothing and touches
  /// no token, so the console is left as it was, while docs/AUTH.md's order
  /// would discard the token before finding out. That was a gate on an
  /// unimplemented half; it survives as the right order, because an attempt is
  /// still refusable for want of a `server.url` or of a transport.
  ///
  /// **This does not abandon a live attempt**, deliberately: the button starts
  /// one and then discards, so the instant between the two commands has to leave
  /// the console with a pairing, and the instant after it with an attempt.
  ///
  /// The verdict goes **after** the token, deliberately: the other order leaves
  /// a moment where the card says the pairing is fine and the credentials are
  /// gone, and a console that stopped there would report itself paired and 401
  /// on every tick. A token that could not be discarded is `kWriteFailed` with
  /// nothing changed. A verdict that could not be *cleared* is `kWriteFailed`
  /// with the console reported as never paired and the gate reset anyway, which
  /// is what actually happened: the credentials are gone, so the verdict is
  /// about a token that no longer exists and leaving it standing would have a
  /// worker refuse to call on a console the user has just re-paired. What the
  /// failure costs is the file surviving to the next boot, where `Load` already
  /// refuses to honour a verdict with no token to be about.
  ipc::Error Unpair() override;
  /// M3-2 (#19). Both are real: the queue is on the card and neither touches
  /// the network, which is `ipc.hpp`'s rule for every command.
  ///
  /// **`kUnknownRom` and `kMultiFile` are not reachable from here yet**, and
  /// that is a missing *library*, not a missing check. Both are answers only a
  /// `roms::RomIndex` can give, and this build fetches nothing -- so an id no
  /// rom has is queued and the worker settles it `kFailed` with a sentence
  /// rather than silently losing it. The moment the engine holds an index, the
  /// one call to make is `download::EnqueueRom(queue_, library, rom_id,
  /// position)`, which produces the whole fixed error set.
  ipc::Error Enqueue(std::int64_t rom_id, std::int32_t* position) override;
  ipc::Error Dequeue(std::int64_t rom_id) override;
  /// M5-4 (#31). All three are `lists::Service`'s, which owns the cursors, the
  /// cap, the TTL and the three projections -- so the one thing this class
  /// decides about a list is what the service is allowed to reach: the card's
  /// queue always, the network only once something hands it a client.
  ipc::Error ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor) override;
  ipc::Error ListNext(ipc::Cursor cursor, ipc::ListPage* page) override;
  ipc::Error ListEnd(ipc::Cursor cursor) override;

  /// M7-1 (#36). Both are served from `conflicts.db` beside `state.db` and
  /// neither touches the network -- the history is what a *previous* tick wrote,
  /// and a restore is one copy between two files on the card.
  ipc::Error ListConflicts(const ipc::ConflictQuery& query, ipc::ConflictPage* page) override;
  ipc::Error RestoreBackup(std::int64_t entry_id, conflicts::RestoreReport* report) override;

  /// Serialises `history_` -- the vector, and nothing else.
  ///
  /// M7-1 (#36) left `history_` under no lock and said exactly what would change
  /// that: a second writer of `.backup/`, which is the scheduler running a tick
  /// (M7-2, #37). This is it. It is deliberately **not** `card_mutex_`: taking
  /// that for a restore would make a pairing grant wait behind a save-state copy
  /// of tens of megabytes, which is the thing the two-lock split exists to
  /// prevent. It is held for an append or for one restore, never across a
  /// network call -- which is what keeps `ListConflicts`, on the IPC thread,
  /// from waiting out a tick.
  mutable std::mutex history_mutex_;

  /// **The right to write a save file.** Held by the worker for the whole of a
  /// tick's transfers, and *try*-locked by a restore.
  ///
  /// A second lock rather than a wider `history_mutex_`, because the two have
  /// opposite requirements: the history has to be readable while a tick runs, so
  /// its lock may not be held across the network, and the save bytes have to be
  /// exclusive *for* the length of a tick. One mutex cannot be both.
  ///
  /// **Try-locked and never waited on**, on the IPC thread's side. Both writers
  /// are individually atomic, so the collision they make is a lost update rather
  /// than a corrupt file -- restored bytes replaced moments later by an
  /// in-flight download, and the tick's `.backup/` copy taken of a half-restored
  /// state -- and neither is something to hand a player. But *waiting* would
  /// park the IPC thread behind a whole tick, which `ipc.hpp` forbids in as many
  /// words. So a restore that arrives mid-tick is refused with a sentence, and
  /// the user presses again.
  ///
  /// Order is this one, then `history_mutex_`, on both sides.
  mutable std::mutex save_write_mutex_;

  /// The conflict history, for the tick that writes it.
  ///
  /// **Nothing in this build drives a tick**, so nothing appends to it here yet
  /// -- since M7-2 (#37) the worker appends to it: `conflicts::RecordSaves` after
  /// the saves half of a tick and `conflicts::RecordStates` after the states
  /// half, both from `RunOneTick` rather than from inside `sync::RunTick`,
  /// because `conflict_record.hpp` includes `state_sync.hpp` and recording from
  /// in there would be a cycle. It is guarded by `history_mutex_`.
  conflicts::History& history() { return history_; }

  /// Whether the backup `entry` names is still on the card, for
  /// `ipc::ConflictRow::backup_present`.
  bool BackupPresent(const conflicts::Entry& entry) const;

  /// The configuration in force, as a snapshot that stays alive for as long as
  /// the caller holds it.
  ///
  /// **This is what `config()` cannot be.** That one hands out a reference, and
  /// `ApplyConfigEdit` replaces the whole `Config` on the IPC thread -- which is
  /// harmless while every reader is that same thread and a data race the moment
  /// one is not. The worker and `lists::Service::Pump` take one of these at an
  /// operation boundary and read it to the end of the operation, never
  /// mid-tick.
  std::shared_ptr<const config::Config> ConfigSnapshot() const;

 private:
  /// Apply `change` to the queue and write the file, or leave both exactly as
  /// they were. Shared by `Enqueue` and `Dequeue` so the rollback cannot be got
  /// right in one and wrong in the other.
  ///
  /// A template rather than a `std::function`, which is not style: both call
  /// sites pass a lambda capturing more than libstdc++'s small-buffer holds, so
  /// every enqueue would allocate on an inner heap of 512 KiB (AGENTS.md's heap
  /// discipline). It also keeps `<functional>` out of a header the console
  /// compiles.
  ///
  /// The snapshot, the change and the restore are not one atomic step. That is
  /// correct today -- nothing else touches `queue_`, because the download worker
  /// is not started here yet -- and it is the thing to fix first when it is: a
  /// failed write would otherwise `Reset` over a state transition the worker
  /// persisted in between, turning a finished download back into a queued one.
  /// The compare-and-set belongs inside `download::Queue` at that point.
  template <typename Change>
  ipc::Error Commit(Change&& change) {
    if (!queue_trusted_) {
      // The card had a bad moment and `queue.json` would not open, so the queue
      // in memory is empty and the one on the card is probably not. Writing now
      // would turn "empty for this boot" into the user's pending downloads gone
      // for good (`download::LoadedQueue::trusted`). Refusing costs them one
      // command and a reboot; the alternative costs them the queue.
      return ipc::Error::kWriteFailed;
    }
    // The whole queue, so a failed write can be undone exactly -- see
    // `download::Queue::Reset` for why reversing the change would not be.
    std::vector<download::QueueEntry> before = queue_.Snapshot();
    const ipc::Error refused = std::forward<Change>(change)();
    if (refused != ipc::Error::kOk) {
      return refused;
    }
    if (WriteQueue()) {
      return ipc::Error::kOk;
    }
    // `kWriteFailed` promises the in-memory state is unchanged too, so a caller
    // that retries is not fighting a half-applied edit (`ipc.hpp`).
    queue_.Reset(std::move(before));
    return ipc::Error::kWriteFailed;
  }

  /// Write `queue.json`. False when it did not reach the card.
  bool WriteQueue();

  /// One pairing attempt: the session, and the server it was started against.
  ///
  /// The URL is kept beside the session rather than read off `config_` when the
  /// grant arrives, because a `server.url` edit mid-attempt would otherwise
  /// commit a token under the name of a server that never issued it.
  ///
  /// The constructor exists because `auth::PairingSession` is neither copyable
  /// nor movable -- it carries the mutex `status()` is safe under -- so the
  /// session has to be built in place rather than assigned into an aggregate.
  struct PairingAttempt {
    PairingAttempt(http::HttpClient& client, auth::PairingConfig config, std::string url)
        : session(client, std::move(config)), server_url(std::move(url)) {}

    auth::PairingSession session;
    std::string server_url;
  };

  /// The pairing thread. Runs one attempt at a time: `Begin()`, then `Poll()`
  /// until the session is terminal, then the commit.
  ///
  /// It never holds `mutex_` across a request. What it holds instead is a
  /// `shared_ptr` to the attempt, which does two jobs: a `StartPairing` that
  /// replaces the attempt mid-request frees nothing under it, and comparing that
  /// pointer against `attempt_` is how the thread finds out it was superseded.
  /// Identity is safe to compare precisely because the thread's own copy keeps
  /// the old object alive, so no new attempt can land on its address.
  void DrivePairing();

  /// Sleep until the next poll is worth making. False when this attempt has been
  /// superseded or the engine is going away, which is the loop's exit.
  bool AwaitNextPoll(const std::shared_ptr<PairingAttempt>& attempt);

  /// Persist an approved grant, and leave the console reporting itself paired.
  ///
  /// Takes `card_mutex_` for the two writes and `mutex_` only for the checks and
  /// the assignments around them, so nothing polled every frame waits on the
  /// card. Re-checks that the attempt is still the engine's *inside*
  /// `card_mutex_` and before writing, which is what makes it atomic against
  /// `ApplyConfigEdit` discarding the token for a `server.url` change.
  ///
  /// The verdict goes after the token for the reason `Unpair` gives in reverse:
  /// the credentials are what matter, and a stale `auth.json` beside a fresh
  /// `token.dat` would have a worker refuse to call on a console that has just
  /// paired.
  void CommitGrant(const std::shared_ptr<PairingAttempt>& attempt,
                   const auth::DeviceTokenResponse& granted);

  /// The worker: park until something is due, then do it. `StartWorker` starts
  /// it and the destructor stops it.
  void RunWorker();

  /// Wait for `network_probe_` to say yes, or for the budget to run out.
  ///
  /// Bounded rather than indefinite: a console that lives on a coffee table with
  /// no Wi-Fi would otherwise park this thread until it is next carried into
  /// range. Giving up costs one tick, which fails `TickOutcome::kOffline` and is
  /// rescheduled on the same backoff every other offline console is on. It also
  /// gives up at once when the process is going away.
  void AwaitNetwork();

  /// Tell the worker there is something to do. Safe from any thread; takes
  /// `mutex_` for the counter and notifies outside it.
  void Wake();

  /// Record a tick that did not transfer anything. The caller holds `mutex_`.
  ///
  /// The counts go to zero with it: `ipc::Status` carries *the last sync's*
  /// counts, so leaving the previous run's numbers beside a `kFailed` would draw
  /// a failed sync that uploaded four saves.
  void RecordFailedTickLocked();

  /// One scheduled tick, start to finish: scan, `sync::RunTick`, record.
  ///
  /// **Not a second tick loop.** `sync::RunTick` is the loop (sync_tick.hpp) and
  /// this is the half it deliberately does not own: step 0's scan on the way in,
  /// and `conflicts::RecordSaves`/`RecordStates` on the way out -- which live
  /// here because `conflict_record.hpp` includes `state_sync.hpp`, so recording
  /// from inside the tick would be a cycle.
  void RunOneTick();

  /// Record what one exchange said about the credentials, and persist the
  /// verdict the moment it becomes one.
  ///
  /// The two lines M1-4 (#8) left for this issue, in one place so no caller can
  /// do half of it: `auth::SaveBlock` when `blocked()` first turns true, and
  /// `auth_` set in the same moment so the overlay draws "pair this console
  /// again" without waiting for a reboot. Idempotent -- a gate that was already
  /// blocked writes nothing.
  void ObserveAnswer(auth::Answer answer);

  /// `[sync]` as the scheduler wants it, clamped the way `config.hpp` says.
  static sync::SchedulerConfig ScheduleFrom(const config::Config& config);

  /// Abandon whatever attempt is running. The caller holds `mutex_`.
  ///
  /// Dropping `attempt_` is the whole mechanism: the thread notices after its
  /// current request that the attempt it is holding is no longer the engine's,
  /// and stops driving it.
  void AbandonPairingLocked();

  /// The text of `config.ini` as it stands, or a reason there is none to edit.
  ///
  /// A *missing* file falls back to `config.ini.old`, which is not an
  /// optimisation: that is the window `io::WriteAtomically`'s two-rename commit
  /// opens, and an edit applied during it would rebuild the file from nothing
  /// and lose every setting the user had. A file that exists and will not read
  /// is refused instead -- settings that cannot be read cannot be preserved, and
  /// overwriting them is the one outcome worse than refusing the edit.
  ///
  /// True when `*text` is what to edit; false leaves a `Diagnostic` behind.
  bool ReadConfigText(std::string* text, std::vector<config::Diagnostic>* diagnostics) const;

  /// Take `loaded` as the configuration in force.
  ///
  /// `auth.json`'s complaint goes **in front** of the file's rather than being
  /// dropped. It is not a complaint about `config.ini` -- but a credential file
  /// that would not read with nothing anywhere saying why is the failure a
  /// diagnostic exists to prevent, and the settings screen (#26) is the one
  /// place on this console a user can read one. In front because
  /// `ipc::TrimDiagnostics` keeps the first few and summarises the rest, so a
  /// `config.ini` with a handful of complaints would otherwise push it into the
  /// "N more" line.
  /// The caller holds `mutex_`: this swaps the pointer every other thread reads
  /// the configuration through, and hands the new `[sync]` section to the
  /// scheduler in the same breath, so a changed interval takes effect without a
  /// reboot and a changed `server.url` lifts a TLS park.
  void AdoptConfigLocked(config::LoadResult loaded);

  /// One of this client's files, under the directory `Load` was given.
  std::string PathTo(const char* file_name) const;

  std::string config_dir_ = kConfigDir;

  /// `queue.json` was readable, so writing it back loses nothing. False only
  /// after a read that failed on a file that is there -- see `Commit`.
  bool queue_trusted_ = true;

  /// The configuration in force, behind a pointer.
  ///
  /// A pointer rather than a value because it is **replaced** rather than
  /// edited: the worker holds a snapshot for the length of a tick and
  /// `lists::Service` for the length of a page, and an assignment into a shared
  /// `Config` underneath either of them is a race on every string in it. Swapped
  /// under `mutex_`; never modified in place.
  std::shared_ptr<const config::Config> config_ =
      std::make_shared<const config::Config>(config::Defaults());

  /// Guards `config_`, and **only** `config_`.
  ///
  /// A lock of its own rather than `mutex_`, because of the one cycle three
  /// threads make possible: `UseServer`, `Load`, `CommitGrant` and `Unpair` hold
  /// `mutex_` and call into `lists::Service`, taking its lock second -- while the
  /// worker inside `lists::Service::Pump` holds *that* lock and asks this class
  /// for a configuration snapshot. If the snapshot took `mutex_` the two orders
  /// would close a cycle and the process would wedge with the overlay open. So
  /// the order is `mutex_` -> `lists_`'s -> this, and this one is a leaf: nothing
  /// is called while it is held.
  mutable std::mutex config_mutex_;

  /// The `Config` the last swap replaced, kept alive and never read.
  ///
  /// `config()` hands out a reference into `config_`, and `AdoptConfigLocked`
  /// *replaces* the pointer -- so a caller on the IPC thread holding that
  /// reference across a `SetConfig` would have been reading freed memory rather
  /// than merely stale fields. Nothing in `ipc::ServiceCore` holds one that
  /// long, and this makes the mistake cost a stale read again instead of a
  /// crash. One generation is enough: a caller that survives two swaps is a bug
  /// no amount of keeping would fix.
  std::shared_ptr<const config::Config> previous_config_;

  /// What was wrong with `config.ini`, and -- see `AdoptConfig` -- with
  /// `auth.json`. Rebuilt every time the configuration is re-read.
  ///
  /// `queue.json`'s complaints are **not** here since M5-4 (#31): they are the
  /// queue's, they reach the user on the queue list, and a copy on a report
  /// about `config.ini` would be the same sentence in two places.
  std::vector<config::Diagnostic> diagnostics_;

  /// `auth.json`'s half, kept apart so re-reading `config.ini` after a write
  /// does not silently drop it. At most one line, and only when the file was
  /// there and would not read (M1-4, #8).
  std::vector<config::Diagnostic> auth_diagnostics_;

  /// The download queue as the card holds it. Loaded once and written by every
  /// command that changes it, so the file and this never disagree by more than
  /// one `rename` (`download.hpp`).
  download::Queue queue_;

  /// What `GetStatus` answers with.
  ///
  /// `kNeverPaired` means `token.dat` is *missing*, not that it could not be
  /// read: an SD card having a bad moment is not a console that has never
  /// paired.
  ///
  /// `kUnauthenticated` is a *server's* answer, so it is never decided here --
  /// it is read off `auth.json`, which is written only once `auth::Gate` has
  /// counted enough consecutive rejections to give up on the pairing (M1-4, #8).
  /// Serving it from the card is what puts the re-pair prompt up on the first
  /// poll after a boot, instead of after the engine has spent that budget again.
  ipc::AuthState auth_ = ipc::AuthState::kNeverPaired;

  /// The three lists the overlay browses (M5-4, #31). Declared after `config_`
  /// and `queue_` because it holds a reference to each.
  lists::Service lists_;

  /// What has been overwritten on this card, newest first (M7-1, #36). Reloaded
  /// by `Load`, which is also what points it at the right directory.
  conflicts::History history_{std::string(conflicts::kHistorySdPath)};

  /// Play time this console has recorded and not yet handed to RomM (M7-4,
  /// #39), and the moment the last tick looked at the saves. Reloaded by `Load`
  /// beside `history_`, and pointed at the right directory by it.
  ///
  /// Touched only from the worker thread, inside `RunTickLocked`'s
  /// `save_write_mutex_` -- so it needs no lock of its own, and gets none
  /// rather than one that would suggest a second writer exists. Nothing on the
  /// IPC surface reads it: play time is not on any screen.
  play::Buffer play_{std::string(play::kBufferSdPath)};

  /// The card, for a restore. The same pointer `UseCard` hands `lists_`, kept
  /// here too because a restore opens two files through it.
  ///
  /// **Installed on the console since M7-2 (#37)** -- `sysmodule/source/card.hpp`
  /// is the Horizon backend and `main.cpp` hands it over. A build that never
  /// called `UseCard` refuses a restore with `RestoreOutcome::kBackupFailed`,
  /// whose promise -- nothing was written -- is exactly what a build that cannot
  /// open the card manages to keep. Listing is not affected: `BackupPresent`
  /// falls back to `kSdRoot`, because *reading* whether a file is there needs one
  /// `io::Exists` and no interface at all.
  fs::FileSystem* card_ = nullptr;

  /// The credentials as `token.dat` holds them, re-read whenever it changes.
  ///
  /// Kept rather than re-read per tick because negotiate needs `device_id` as
  /// well as the bearer token, and because a card having a bad moment must not
  /// turn a paired console into an unpaired one for one tick. Empty until this
  /// console has paired.
  auth::StoredToken token_;

  /// When a tick may run at all, and how long to wait after one that did not
  /// work (M7-2, #37). Driven only by the worker thread and by the commands that
  /// wake it, both under `mutex_`.
  sync::Scheduler scheduler_;

  /// Fired by the destructor, and passed to every stage of a tick, so a shutdown
  /// ends the tick at an operation boundary rather than mid-write
  /// (`sync::TickOptions::cancel`).
  ///
  /// **One per process rather than one per tick**, and the difference does not
  /// arise: cancellation is one-way and the only thing that fires it is the
  /// process going away, which happens once. A second owner would need a second
  /// token -- an overlay "stop this sync" is the obvious one -- and there is no
  /// such command on the wire (`ipc::Command`), so there is nothing to give one
  /// to. `sync::RunTick` still gets exactly one token across its three stages,
  /// which is the rule that matters: a tick stops at a boundary rather than
  /// half way through the accounting call.
  http::CancelToken tick_cancel_;

  /// What `Status::sync_in_progress` draws, and the same fact `RequestSync`
  /// answers false on.
  bool sync_in_progress_ = false;

  /// The last tick's outcome, as the status screen reads it.
  std::int64_t last_sync_at_ = 0;
  ipc::SyncResult last_sync_result_ = ipc::SyncResult::kNever;
  std::int64_t uploaded_ = 0;
  std::int64_t downloaded_ = 0;
  std::int64_t conflicts_ = 0;
  std::int64_t failed_ = 0;

  /// The transport a tick runs on, and the token the *lists* read with. Both
  /// null/empty until `UseServer`.
  http::HttpClient* server_ = nullptr;
  std::string list_token_;

  /// Whether the console is on a network. Null means "up".
  NetworkProbe network_probe_;

  /// Bumped by every command that gives the worker something to do, under
  /// `mutex_`, and read by the worker as part of its wait predicate.
  ///
  /// **A counter and not a flag**, because the window it closes is a lost
  /// wake-up: the worker releases `mutex_` to run `PumpLists()` and re-takes it
  /// before waiting, and a `notify_all` landing inside that gap wakes nobody.
  /// With a parked decision -- `interval_min = 0`, or the switch off -- the wait
  /// has no deadline to save it, so a "Sync now" would be lost until some
  /// unrelated command happened along. Comparing the count the worker last
  /// *decided* on against the count now makes the notification impossible to
  /// miss.
  std::uint64_t wakes_ = 0;

  std::thread worker_thread_;

  /// The verdict `auth.json` holds, and what a worker consults before calling.
  ///
  /// Restored at boot rather than counted from zero, and driven from then on by
  /// `ObserveAnswer` -- which every call the worker and the list pages make
  /// reports into (M7-2, #37). It is the one place a `401` is counted, and the
  /// one place the verdict is written: `auth::SaveBlock` the moment `blocked()`
  /// first turns true, with `auth_` set in the same breath. `Unpair` is the only
  /// thing that lifts it.
  auth::Gate gate_;

  /// The transport and the identity seed a pairing attempt runs on. Empty on the
  /// console, which is what makes `StartPairing` answer `kUnavailable` there.
  PairingBackend pairing_backend_;

  /// Serialises the *card* writes the two threads both make -- `token.dat` and
  /// `auth.json` -- and nothing else.
  ///
  /// **It exists so that `mutex_` is never held across an SD write.** A commit
  /// and an `Unpair` still cannot interleave, which is what that ordering was
  /// for; what changes is that the commands polled every frame no longer wait
  /// behind one. `GetStatus` and `GetPairState` take `mutex_` and never this,
  /// so a slow card cannot stall the status or pairing screen -- the promise
  /// `main.cpp` already makes about `GetStatus` never going near the card.
  ///
  /// **Lock order is this one, then `mutex_`, never the reverse**, and `mutex_`
  /// is never held while acquiring this. Four methods take it: `CommitGrant`,
  /// `Unpair` and `ApplyConfigEdit` -- which discards the token when
  /// `server.url` moves, and therefore has to be atomic against a commit landing
  /// at the same moment -- and `Load`, which reads the same two files at boot.
  mutable std::mutex card_mutex_;

  /// Guards the in-memory state the two threads share: the attempt, `auth_`,
  /// and `gate_`. Held for assignments and never across I/O, with one exception:
  /// `Load` holds it over the four files it reads at boot. The pairing thread
  /// usually *does* exist by then -- `UsePairingBackend` is called before `Load`
  /// -- so what makes that safe is not the absence of a second thread but the
  /// absence of a reader: it is parked with no attempt to drive, and no
  /// `ServiceCore` exists yet to answer the frame-polled commands this rule is
  /// really about. Mutable because `Snapshot()` and `pairing_status()` are const
  /// and both read state the pairing thread writes.
  mutable std::mutex mutex_;

  /// How the pairing thread is told there is something to do, and how it is
  /// woken early from a poll interval by a new attempt or by shutdown.
  std::condition_variable wake_;

  /// Created by `UsePairingBackend`, and only when it is handed a transport, so
  /// a console with no `HttpClient` -- which is every console today -- creates no
  /// thread at all. Not on the first `StartPairing`: see `UsePairingBackend` for
  /// why the one throwing call in this class happens at start rather than under
  /// a user's finger.
  ///
  /// **Its stack is not sized here, and on Horizon that is a number somebody
  /// has to derive.** devkitA64's default comes out of the 512 KiB inner heap
  /// (`kInnerHeapSize`, `main.cpp`), which sysmodule/AGENTS.md budgets for one
  /// in-flight download buffer plus a TLS context and nothing else. It costs
  /// nothing today, because no console reaches this line; it is #126's to settle
  /// along with the rest of the heap, since that is the issue that gives a
  /// console a transport and so the first attempt that ever runs.
  std::thread pairing_thread_;
  bool stopping_ = false;

  /// The attempt in flight, or null. A `shared_ptr` because the pairing thread
  /// has to keep it alive across a request that a replacing `StartPairing` no
  /// longer refers to.
  std::shared_ptr<PairingAttempt> attempt_;

  /// The one the pairing thread has picked up. Different from `attempt_` exactly
  /// while there is a new attempt waiting to be driven, which is what the thread
  /// waits on -- and null-vs-null is "nothing is running", which that wait has to
  /// tell apart from "a new attempt started while the old one was mid-request".
  std::shared_ptr<PairingAttempt> driven_;

  /// What went wrong *after* the session succeeded -- an approved pairing whose
  /// token would not reach the card. `PairingSession` cannot know about it and
  /// would keep reporting `kApproved`, which is a console that says it paired
  /// and has no credentials.
  std::string attempt_commit_failure_;
};

}  // namespace rommsync::sysmodule
