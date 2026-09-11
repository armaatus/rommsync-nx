// The Horizon half of `power::Module`: `psc:m` (M9-4, #208).
//
// `power.hpp` states the contract and `power.cpp` is the loop; this is the part
// that names libnx, and the only part of the three a laptop cannot run. It is
// compiled by devkitPro and by nothing else -- `card.cpp`'s arrangement, for
// `card.cpp`'s reason -- so what stands behind it is `switch.builds` compiling
// it and the static checks in `tests/test_sysmodule_boot.py` reading the grant
// out of the npdm.
//
// Nothing here has ever run: it is Horizon-side and is exercised in Ryujinx
// before the M8-1 gate, never on hardware (sysmodule/AGENTS.md).
#include <switch.h>

#include <memory>
#include <string>

#include "power.hpp"
#include "rommsync/log.hpp"
#include "sized_thread.hpp"

namespace rommsync::sysmodule::power {
namespace {

/// This sysmodule's `PscPmModuleId`, and **it is arbitrary**.
///
/// There is no registry. The ids up to `PscPmModuleId_Spsm` (127) are
/// Nintendo's, and every third-party sysmodule that subscribes picks an unused
/// value out of the air: sys-con uses 126, MissionControl 0xBD, sys-autopilot
/// 0x4150. Two of them picking the same number is possible and nothing manages
/// it, so this is written down rather than left to be inferred.
///
/// 0x524D is "RM", which is what `program_id` 0x4200000000524D53 ends with
/// (`sys-rommsync.json`). It is out of Nintendo's range and out of the three
/// above, which is the most that can be said for any choice here.
constexpr PscPmModuleId kModuleId = static_cast<PscPmModuleId>(0x524D);

/// What this module is registered as depending on: **fs**.
///
/// The dependency decides *where in the order* a module is told. `fs` is
/// notified early on the way down and late on the way back up, which is exactly
/// what a module that writes save files to the card needs -- told before the
/// filesystem goes, told again only once it is back. Atmosphere's own `erpt`
/// registers the same way for the same reason
/// (`erpt/srv/erpt_srv_service.cpp`).
constexpr u32 kDependencies[] = {PscPmModuleId_Fs};

/// `psc:m`, behind the interface `power::Watcher` drives.
class PscModule final : public Module {
 public:
  ~PscModule() override {
    if (!open_) {
      // Nothing was registered, so there is nothing to unregister. Asking anyway
      // would be an IPC on a zeroed `Service`, which answers an error nobody
      // reads -- `pscPmModuleClose` is the one of the two that checks.
      return;
    }
    // `Finalize` before `Close`: the first tells PSC this module is gone, the
    // second drops the session. Closing without finalizing leaves PSC with a
    // module it will keep waiting for an acknowledgement from, which is the
    // freeze this whole file exists to avoid.
    pscPmModuleFinalize(&module_);
    pscPmModuleClose(&module_);
  }

  /// Register with PSC. False leaves the console exactly where it was before
  /// this issue -- asleep-unaware -- rather than refusing to start.
  bool Open() {
    ueventCreate(&stop_, /*autoclear=*/false);
    const Result rc = pscmGetPmModule(&module_, kModuleId, kDependencies,
                                      sizeof(kDependencies) / sizeof(kDependencies[0]),
                                      /*autoclear=*/true);
    if (R_FAILED(rc)) {
      // Silent here, and said once by the caller: `main.cpp` logs whether there
      // is a subscription at all, and a second line from in here would be the
      // same sentence twice in the file a user is asked to attach.
      return false;
    }
    open_ = true;
    return true;
  }

  bool NextRequest(State* state) override {
    while (true) {
      s32 signalled = -1;
      // **Blocked on an event, never polled.** SysDVR#395 is a battery-drain and
      // heat report caused by a sysmodule busy-waiting through sleep; a thread
      // parked here costs nothing until PSC has something to say.
      //
      // Two objects rather than one, so `Stop()` can end this. Nothing on the
      // console calls it -- `main` never leaves its service loop -- but a wait
      // with no way out is a destructor that cannot run, and this class has one.
      const Result rc = waitMulti(&signalled, UINT64_MAX, waiterForEvent(&module_.event),
                                  waiterForUEvent(&stop_));
      if (signalled == 1) {
        // `Stop()`. Nothing failed; the caller is going away.
        return false;
      }
      if (R_FAILED(rc)) {
        Surrender("the sleep watcher's wait failed", rc);
        return false;
      }
      PscPmState raw = PscPmState_Awake;
      u32 flags = 0;
      const Result got = pscPmModuleGetRequest(&module_, &raw, &flags);
      if (R_FAILED(got)) {
        // The event fired and the request would not come out of PSC. **Not a
        // `continue`**, which is what this was until the review: the event is
        // auto-clear, so the request is gone and PSC is waiting for an
        // acknowledgement this thread can no longer produce -- and a module that
        // never answers is the whole console frozen with no fatal and no crash
        // report (sys-con#155).
        Surrender("a sleep request could not be read", got);
        return false;
      }
      // The values are libnx's `PscPmState` and the names on this side are
      // Nintendo's; `power::State` carries the mapping and why libnx's is wrong
      // for two of the six.
      *state = static_cast<State>(raw);
      return true;
    }
  }

  bool Acknowledge(State state) override {
    // **Which of the two commands this is.** `pscPmModuleAcknowledge` dispatches
    // cmd 4 -- `AcknowledgeEx`, which carries the state -- on 5.1.0 and up, and
    // cmd 2 below it, which does not. That is decided by `hosversionGet()`, and
    // `__appInit` calls `hosversionSet` before anything version-gated, aborting
    // if it cannot: an unset host version reads as 0, which would silently take
    // the pre-5.1.0 path here, and cmd 2 on newer firmware just aborts.
    return R_SUCCEEDED(pscPmModuleAcknowledge(&module_, static_cast<PscPmState>(state)));
  }

  void Stop() override { ueventSignal(&stop_); }

 private:
  /// Give up being a PSC module, out loud, and **unregister** on the way.
  ///
  /// The one thing worse than a console that does not know it is sleeping is a
  /// console that cannot sleep at all. Once this thread cannot answer, staying
  /// registered means PSC waits for an acknowledgement that will never come --
  /// so the module is finalized here rather than in a destructor that never runs
  /// (`main` does not return). What is left is the client as it was before this
  /// issue, degraded and saying so, instead of a system freeze.
  void Surrender(const char* what, Result rc) {
    log::Error(log::Event::kPower, std::string(what) + "; this console will sleep without "
                                   "waiting for rommsync from now on (rc=" +
                                       std::to_string(rc) + ")");
    if (open_) {
      pscPmModuleFinalize(&module_);
      pscPmModuleClose(&module_);
      open_ = false;
    }
  }

  PscPmModule module_{};
  UEvent stop_{};
  bool open_ = false;
};

/// The module, the loop and the thread it runs on, kept alive together.
class PscSubscription final : public Subscription {
 public:
  explicit PscSubscription(Sink& sink) : watcher_(module_, sink) {}

  ~PscSubscription() override {
    if (!thread_.joinable()) {
      return;
    }
    module_.Stop();
    thread_.join();
  }

  bool Start() {
    if (!module_.Open()) {
      return false;
    }
    // `SizedThread` rather than a raw `threadCreate`, because M9-2 (#207) made
    // every thread this process starts one whose stack it chose -- and made that
    // choice a row in `main.cpp`'s table. `kWatcherStackBytes` is this thread's,
    // and it is a quarter of the engine's for the reason stated where it is
    // declared.
    return thread_.Start<&PscSubscription::Run>(this, kWatcherStackBytes);
  }

 private:
  void Run() { watcher_.Run(); }

  PscModule module_;
  Watcher watcher_;
  SizedThread thread_;
};

}  // namespace

std::unique_ptr<Subscription> Subscribe(Sink& sink) {
  auto subscription = std::make_unique<PscSubscription>(sink);
  if (!subscription->Start()) {
    return nullptr;
  }
  return subscription;
}

}  // namespace rommsync::sysmodule::power
