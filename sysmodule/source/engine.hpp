// What `ipc::ServiceCore` reads the console's state out of, as of M4-1.
//
// `ipc::Engine` is the seam the whole command set is driven through, and it was
// written before most of what sits behind it existed (`ipc.hpp`): the download
// queue is M3-2 (#19), live `config.ini` writes are M5-3 (#30), list paging is
// M5-4 (#31), the scheduler is M7-2. This is the implementation that lets the
// overlay have a service to talk to in the meantime.
//
// **It answers only what this build actually knows.** The console's
// configuration -- which it now writes as well as reads (M5-3) -- whether it has
// ever paired, whether the server has stopped accepting its token (M1-4, #8),
// and -- since M3-2 (#19) -- its download queue are on the SD card
// and are read from it; everything else is
// `ipc::Error::kUnavailable`, which is a sentence the overlay can draw rather
// than a plausible refusal that sends a user looking for a full queue or a
// failing SD card. Each of the issues above replaces its own part of this, and
// `kUnavailable` disappearing entirely is what says the engine is finished.
//
// Nothing here has ever run: it is Horizon-side and is exercised in Ryujinx
// before the M8-1 gate, never on hardware (sysmodule/AGENTS.md).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/config.hpp"
#include "rommsync/download.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/pairing.hpp"

namespace rommsync::sysmodule {

/// Where the client's own files live on the card. Named here rather than in
/// `core/`, which owns the file *names* and may not know an SD path (hard
/// rule 4, and the `sdmc:` prefix is libnx's).
inline constexpr const char* kConfigDir = "sdmc:/config/rommsync/";

/// The engine as far as it is built.
///
/// Constructed once at start and driven from the IPC thread. Two things change
/// after `Load()`: the queue, and -- since M5-3 (#30) -- the configuration,
/// which `ApplyConfigEdit` writes to the card and re-reads. `download::Queue`
/// carries its own mutex for the reason `ipc::Engine` states; the config swap
/// does not, and that is a fact about today rather than a decision to keep:
/// `ServiceServer::Run` is a single `svcReplyAndReceive` loop, so every command
/// -- the polls and the write alike -- runs on one thread and the swap races
/// with nothing.
///
/// **A mutex is not what makes it safe the day a worker exists.**
/// `ipc::Engine::config()` hands out a *reference*, so a swap under a lock would
/// still free a `Config` a caller is holding. The seam to change then is that
/// signature -- a snapshot, the way `Snapshot()` already answers -- and workers
/// have to take theirs at a tick boundary, never mid-sync and never
/// mid-download. The download worker is not started here yet; when it is, this
/// is already the object it drains.
class SdEngine : public ipc::Engine {
 public:
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
  /// **`StartPairing` is still `kUnavailable`, so the other half is not here**,
  /// and the asymmetry is worth knowing before something presses this: a console
  /// that unpairs cannot yet pair again from the sysmodule. Nothing under
  /// `overlay/` calls it today -- the settings screen's "Re-pair" button, which
  /// sends `Unpair` then `StartPair`, is M4-4 (#26) -- so the order to build
  /// them in is `StartPairing` first, or that button discards a pairing with
  /// nothing to restart.
  ///
  /// The verdict goes **after** the token, deliberately: the other order leaves
  /// a moment where the card says the pairing is fine and the credentials are
  /// gone, and a console that stopped there would report itself paired and 401
  /// on every tick. A token that could not be discarded is `kWriteFailed` with
  /// nothing changed; a verdict that could not be cleared is `kWriteFailed` with
  /// the console reported as never paired, which is what actually happened -- the
  /// credentials really are gone, and the sentence the overlay draws has to be
  /// able to say so.
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
  ipc::Error ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor) override;
  ipc::Error ListNext(ipc::Cursor cursor, ipc::ListPage* page) override;
  ipc::Error ListEnd(ipc::Cursor cursor) override;

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
  /// The queue's complaints, and `auth.json`'s, go **in front** of the file's
  /// rather than being dropped. They are not complaints about `config.ini`, and
  /// the section says so -- but a queue that vanished with nothing anywhere
  /// saying why is the
  /// failure a diagnostic exists to prevent, and the settings screen (#26) is
  /// the one place on this console a user can read one. In front because
  /// `ipc::TrimDiagnostics` keeps the first few and summarises the rest, so a
  /// `config.ini` with a handful of complaints would otherwise push the one
  /// saying the whole download queue was discarded into the "N more" line.
  void AdoptConfig(config::LoadResult loaded);

  /// One of this client's files, under the directory `Load` was given.
  std::string PathTo(const char* file_name) const;

  std::string config_dir_ = kConfigDir;

  /// `queue.json` was readable, so writing it back loses nothing. False only
  /// after a read that failed on a file that is there -- see `Commit`.
  bool queue_trusted_ = true;

  config::Config config_ = config::Defaults();

  /// What was wrong with `config.ini`, and -- see `AdoptConfig` -- with
  /// `queue.json`. Rebuilt every time the configuration is re-read.
  std::vector<config::Diagnostic> diagnostics_;

  /// The queue's half of that, kept apart so re-reading `config.ini` after a
  /// write does not silently drop it.
  std::vector<config::Diagnostic> queue_diagnostics_;

  /// And `auth.json`'s half, kept apart for the same reason. At most one line,
  /// and only when the file was there and would not read (M1-4, #8).
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

  /// The verdict `auth.json` holds, and what a worker consults before calling.
  ///
  /// Restored at boot rather than counted from zero. Nothing in this build
  /// observes an answer into it yet -- there is no scheduler to make the calls
  /// (M7-2, #37) -- so what it does today is carry the stored verdict and let
  /// `Unpair` lift it. That is the seam #37 fills in: `Observe` each call's
  /// `auth::AnswerOf(...)`, and persist the block the moment `blocked()` turns
  /// true.
  auth::Gate gate_;
};

}  // namespace rommsync::sysmodule
