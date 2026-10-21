// What the overlay process does at each hook libtesla gives it, in one place
// that runs on a host (M9-6, #209).
//
// nx-ovlloader branches back to `loadNro` when `main` returns, so **the process
// never exits**: anything an overlay takes and does not give back is gone for
// the life of the console, across every open and close. That is libtesla#37's
// leaked fs sessions and Ultrahand#278's "crashes 100% on the sixth attempt",
// and nx-ovlloader#41 states the rule -- normal teardown is safe, every other
// path leaks. So this class is the one teardown, and every path ends in it:
// `exitServices` calls `Stop`, the destructor calls `Stop`, and `Stop` is
// idempotent. `ctest -R overlay.cycles` drives every exit path the pinned
// `tsl::loop` has, eight times over, and balances sessions, threads and heap;
// it also greps `tesla.hpp` for the loop's one tail, so a library bump that grew
// a second exit is a red test.
//
// Two smaller things live here because they are the same hooks:
//
//   * **Input is given back at startup.** Neither libtesla nor libultrahand
//     calls `requestForeground(false)` when an overlay starts, so a fresh
//     instance after one that died holding exclusive input inherits it with no
//     way back -- the ecosystem's most-reported overlay bug. `Start` releases
//     it; libtesla takes it again with `requestForeground(true)` when it shows
//     the overlay.
//   * **`update()` does not run while the overlay is hidden.** `tsl::loop`
//     waits on the combo event with no timeout, and the gui stack sits frozen.
//     A screen that polled by counting frames would come back from a hide
//     mid-count and draw what it read before it, so screens poll by `Cadence`,
//     which knows how many times the overlay has been shown.
//
// Names no libultrahand type, so `overlay.portable` holds it to that: the
// device's `main.cpp` hands it `tsl::hlp::requestForeground` and the host
// suite hands it a recorder.
#pragma once

#include <cstdint>

#include "ipc_client.hpp"

namespace rommsync::overlay {

/// How many times this process has shown the overlay. Bumped by
/// `Lifecycle::Shown`, read by `Cadence`.
///
/// Process-wide rather than a member a screen is handed: libtesla's overlay is
/// a process singleton itself (`tsl::Overlay::get()`), and a screen is built by
/// `tsl::changeTo` with no route to the `Lifecycle` short of threading a second
/// reference through every screen's constructor.
std::uint64_t Showings();

/// When a screen's `update()` should poll.
///
/// Due on the first frame, then once every `every_frames` frames, and on the
/// first frame of every showing whatever the count says -- because the count is
/// of frames that *ran*, and none ran while the overlay was hidden.
class Cadence {
 public:
  explicit Cadence(int every_frames) : every_(every_frames < 1 ? 1 : every_frames) {}

  /// Call once per `update()`. True when this frame should poll.
  bool Due();

  /// Poll on the next frame, whatever the count -- after a press that changed
  /// what the next answer will be.
  void Expire() { fresh_ = true; }

 private:
  int every_;
  int since_ = 0;
  bool fresh_ = true;
  std::uint64_t showing_ = 0;
};

/// The overlay's own half of libtesla's lifecycle.
///
/// `main.cpp`'s `RommsyncOverlay` owns one beside its `IpcClient`, declared
/// after it so it is destroyed first.
class Lifecycle {
 public:
  /// `tsl::hlp::requestForeground` on a console: true takes exclusive input,
  /// false gives it back.
  using Foreground = void (*)(bool enabled);

  Lifecycle(IpcClient& client, Foreground foreground) : client_(client), foreground_(foreground) {}

  /// `Stop()`, for the path that deletes the overlay without `exitServices`.
  ~Lifecycle();

  Lifecycle(const Lifecycle&) = delete;
  Lifecycle& operator=(const Lifecycle&) = delete;

  /// `initServices`. Gives input back, and opens nothing: the session is opened
  /// by the first screen's poll, because a sysmodule that is not running is a
  /// state the status screen draws rather than a reason for the overlay not to
  /// load.
  void Start();

  /// `onShow`. A new showing, which every `Cadence` polls on.
  void Shown();

  /// `exitServices`, and the destructor: `IpcClient::Shutdown`, once.
  ///
  /// Does not touch input. libtesla's tail calls `requestForeground(false)`
  /// before `exitServices` on every path, and a second release here would be
  /// thirty-odd IPC calls to `hid:sys` for nothing.
  void Stop();

  /// How many teardowns this process has run. What `overlay.cycles` counts to
  /// say each path ran exactly one.
  static int Teardowns();

 private:
  IpcClient& client_;
  Foreground foreground_;
  bool stopped_ = false;
};

}  // namespace rommsync::overlay
