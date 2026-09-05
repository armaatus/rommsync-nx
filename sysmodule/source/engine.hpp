// What `ipc::ServiceCore` reads the console's state out of, as of M4-1.
//
// `ipc::Engine` is the seam the whole command set is driven through, and it was
// written before most of what sits behind it existed (`ipc.hpp`): the download
// queue is M3-2 (#19), live `config.ini` writes are M5-3 (#30), list paging is
// M5-4 (#31), the scheduler is M7-2. This is the implementation that lets the
// overlay have a service to talk to in the meantime.
//
// **It answers only what this build actually knows.** The console's
// configuration, whether it has ever paired, and -- since M3-2 (#19) -- its
// download queue are on the SD card and are read from it; everything else is
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
/// Constructed once at start and read from the IPC thread. Everything but the
/// queue is immutable after `Load()` -- there is no worker to race with yet --
/// so this takes no lock of its own. The queue is the one part that changes,
/// and `download::Queue` carries its own mutex for exactly the reason
/// `ipc::Engine` states: every method is callable from the IPC thread while the
/// engine's own threads run. The download worker is not started here yet; when
/// it is, this is already the object it drains.
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

  ipc::Error SetSyncEnabled(bool enabled) override;
  ipc::Error ApplyConfigEdit(const ipc::ConfigEdit& edit,
                             std::vector<config::Diagnostic>* diagnostics) override;
  bool RequestSync() override;
  ipc::Error StartPairing() override;
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

  /// One of this client's files, under the directory `Load` was given.
  std::string PathTo(const char* file_name) const;

  std::string config_dir_ = kConfigDir;

  /// `queue.json` was readable, so writing it back loses nothing. False only
  /// after a read that failed on a file that is there -- see `Commit`.
  bool queue_trusted_ = true;

  config::Config config_ = config::Defaults();

  /// What was wrong with `config.ini`, and -- see `Load` -- with `queue.json`.
  std::vector<config::Diagnostic> diagnostics_;

  /// The download queue as the card holds it. Loaded once and written by every
  /// command that changes it, so the file and this never disagree by more than
  /// one `rename` (`download.hpp`).
  download::Queue queue_;

  /// `kNeverPaired` or `kPaired`. `kUnauthenticated` is a *server's* answer --
  /// a token this console still holds and RomM has stopped accepting -- so it
  /// cannot be decided from the card, and reporting it from here would send a
  /// working console to the re-pair screen (`ipc::AuthState`).
  ///
  /// `kNeverPaired` means the file is *missing*, not that it could not be read:
  /// an SD card having a bad moment is not a console that has never paired.
  ipc::AuthState auth_ = ipc::AuthState::kNeverPaired;
};

}  // namespace rommsync::sysmodule
