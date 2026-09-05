// What `ipc::ServiceCore` reads the console's state out of, as of M4-1.
//
// `ipc::Engine` is the seam the whole command set is driven through, and it was
// written before most of what sits behind it existed (`ipc.hpp`): the download
// queue is M3-2 (#19), live `config.ini` writes are M5-3 (#30), list paging is
// M5-4 (#31), the scheduler is M7-2. This is the implementation that lets the
// overlay have a service to talk to in the meantime.
//
// **It answers only what this build actually knows.** The console's
// configuration and whether it has ever paired are on the SD card and are read
// from it; everything else is `ipc::Error::kUnavailable`, which is a sentence
// the overlay can draw rather than a plausible refusal that sends a user looking
// for a full queue or a failing SD card. Each of the issues above replaces its
// own part of this, and `kUnavailable` disappearing entirely is what says the
// engine is finished.
//
// Nothing here has ever run: it is Horizon-side and is exercised in Ryujinx
// before the M8-1 gate, never on hardware (sysmodule/AGENTS.md).
#pragma once

#include <string>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/pairing.hpp"

namespace rommsync::sysmodule {

/// Where the client's own files live on the card. Named here rather than in
/// `core/`, which owns the file *names* and may not know an SD path (hard
/// rule 4, and the `sdmc:` prefix is libnx's).
inline constexpr const char* kConfigDir = "sdmc:/config/rommsync/";

/// The engine as far as it is built.
///
/// Constructed once at start and read from the IPC thread. It is immutable
/// after `Load()` -- there is no worker to race with yet -- so it takes no lock;
/// the first implementation that grows one owns the locking, which is what
/// `ipc::Engine` already says.
class SdEngine : public ipc::Engine {
 public:
  /// Read `config.ini` and look for `token.dat`. Never fails: a console with no
  /// config is a console with the defaults, and one with no token has never
  /// paired -- both are states the overlay draws (`config.hpp`).
  void Load();

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
  ipc::Error Enqueue(std::int64_t rom_id, std::int32_t* position) override;
  ipc::Error Dequeue(std::int64_t rom_id) override;
  ipc::Error ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor) override;
  ipc::Error ListNext(ipc::Cursor cursor, ipc::ListPage* page) override;
  ipc::Error ListEnd(ipc::Cursor cursor) override;

 private:
  config::Config config_ = config::Defaults();
  std::vector<config::Diagnostic> diagnostics_;

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
