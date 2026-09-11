// The console going to sleep, and coming back (M9-4, #208).
//
// Until this issue `sys-rommsync` did not know either had happened. Horizon
// tells a process through **PSC** -- `psc:m`, the power state controller -- and
// a process that never subscribes is one the transition happens *around*: its
// `fsp-srv` sessions and its sockets are still in use when the services behind
// them go down, which is the pattern that crashes consoles for other projects.
// sys-autopilot's crash reports were `omm` aborting with `2165-1001` --
// module 165 is `Spsm`, description 1001 is "PmRequest aborted" -- alongside
// `bsdsocket` aborts, on a console that hard-restarts on wake
// (TooTallNate/sys-autopilot#4).
//
// For us it is worse than a crash. The worker can be mid-`sync::Execute` or
// mid-`download::Drain` when the card goes away, which is a save write cut in
// half -- the one thing hard rule 2 exists to prevent (CLAUDE.md).
//
// ## The contract, in three sentences
//
// **Every request is acknowledged, exactly once.** PSC hands a module one
// request at a time and waits for the acknowledgement before it moves the
// console on. A module that does not answer is a console that does not sleep:
// sys-con#155 is a whole system freezing with no fatal and no crash report,
// because one sysmodule waited indefinitely on services that were already
// asleep, and sys-clk#85 is a console that never wakes for the same reason.
//
// **What happens between the request and the acknowledgement is bounded.** That
// is the other half of the same rule. `Sink::Quiesce` may wait -- it has to, or
// the acknowledgement is a lie about the save write still in flight -- but it
// may not wait forever, and it returns whether or not it got what it wanted.
//
// **Do not poll.** SysDVR#395 is a battery-drain and heat report caused by a
// sysmodule busy-waiting through sleep. `Module::NextRequest` blocks on an event
// and costs nothing until PSC signals it.
//
// ## Why there is an interface here at all
//
// The same reason `fs::FileSystem` and `http::HttpClient` have one: the half
// that matters is testable and the half that names libnx is not. `Watcher` and
// `SdEngine::Quiesce` are compiled into `test_power` and `test_engine` and
// driven through a whole `Awake -> SleepReady -> MinimumAwake -> Awake` on a
// laptop, where the console cannot be reached before the M8-1 gate. `psc:m`
// itself lives in `power_psc.cpp`, which only devkitPro compiles -- `card.hpp`'s
// arrangement, for `card.hpp`'s reason.
#pragma once

#include <memory>

namespace rommsync::sysmodule::power {

/// The states PSC moves a module through, under **Nintendo's** names.
///
/// The values are libnx's `PscPmState` and the names are not, because two of
/// libnx's are wrong and reading the code with them produces the opposite
/// meaning:
///
///   | value | libnx                        | Nintendo                      |
///   |-------|------------------------------|-------------------------------|
///   | 0     | `PscPmState_Awake`           | `FullAwake`                   |
///   | 1     | `PscPmState_ReadyAwaken`     | **`MinimumAwake`**            |
///   | 2     | `PscPmState_ReadySleep`      | `SleepReady`                  |
///   | 3     | `PscPmState_ReadySleepCritical` | `EssentialServicesSleepReady` |
///   | 4     | `PscPmState_ReadyAwakenCritical` | **`EssentialServicesAwake`** |
///   | 5     | `PscPmState_ReadyShutdown`   | `ShutdownReady`               |
///
/// "ReadyAwaken" reads as *about to wake up*; the state it names is one where
/// the minimum set of services is **already** awake, which is the moment
/// Atmosphere's own `erpt` turns its filesystem access back on
/// (`erpt_srv_service.cpp`). Getting that backwards is a module that resumes one
/// state too early or too late, and nothing on a console would say which.
enum class State : unsigned {
  kFullAwake = 0,
  kMinimumAwake = 1,
  kSleepReady = 2,
  kEssentialServicesSleepReady = 3,
  kEssentialServicesAwake = 4,
  kShutdownReady = 5,
};

/// Whether `state` is one this process has to go quiet for.
///
/// The two sleep states and the shutdown one. A shutdown is quiesced and never
/// resumed, which is right: there is nothing after it.
///
/// **False for anything unrecognised**, which is the conservative direction:
/// `Watcher` acknowledges every request whatever it is, and a state neither this
/// nor `Resumes` claims is one it acknowledges without touching the sink. The
/// alternative -- guessing -- is a module that tears its card access down on a
/// state that was never about sleep.
bool Quiesces(State state);

/// ...and whether it is one the console comes back on.
///
/// `MinimumAwake` and `FullAwake`, and deliberately **not**
/// `EssentialServicesAwake`. That one means the *critical* services are back and
/// says nothing about `fsp-srv` or the network -- resuming there is a tick
/// issuing card and socket work on a console that is still bringing its services
/// up, which is the failure this issue is about arriving from the other
/// direction. Atmosphere's own `erpt` turns its filesystem access back on at
/// `MinimumAwake` and not before (`erpt_srv_service.cpp`), and this follows it.
bool Resumes(State state);

/// The platform half: this process's subscription to the console's power
/// transitions.
///
/// One request at a time, and never two outstanding -- PSC does not send a
/// second before the first is acknowledged, and `Watcher` does not ask for one.
class Module {
 public:
  virtual ~Module() = default;

  /// Block until PSC has a request, and hand over the state it is for.
  ///
  /// False means there will be no more: `Stop()` was called, or the
  /// subscription failed in a way it will not recover from. A caller that gets
  /// false stops asking.
  ///
  /// **It blocks on an event and does not poll** -- see the top of this file for
  /// the battery report that is.
  virtual bool NextRequest(State* state) = 0;

  /// Acknowledge the request `NextRequest` last handed over. False when the
  /// acknowledgement did not reach PSC, which is worth a log line and nothing
  /// else: there is no second chance for one request and the console is
  /// transitioning either way.
  ///
  /// `state` is passed back because that is what the newer of the two service
  /// commands wants -- see `power_psc.cpp`, which is the file that has to know
  /// which one it is calling.
  virtual bool Acknowledge(State state) = 0;

  /// Make the blocked `NextRequest` return false, and keep it that way.
  ///
  /// Safe from another thread; that is the whole point of it. Nothing on the
  /// console calls it -- `main` never leaves its service loop -- and the tests
  /// end the watcher's thread with it.
  virtual void Stop() = 0;
};

/// What a power transition does to the process behind it.
///
/// `SdEngine` is the one implementation that matters; the tests have a recording
/// one. **The two calls alternate**: a suspend is one `Quiesce` however many
/// states PSC takes to get there, and the wake after it is one `Resume`.
/// Collapsing the repeats is `Watcher`'s -- PSC sends two sleep states in a row
/// (`SleepReady` then `EssentialServicesSleepReady`), and a sink asked twice
/// would do the whole tear-down twice for one sleep.
class Sink {
 public:
  virtual ~Sink() = default;

  /// Stop everything this process is doing to the card and to the network, and
  /// **return once none of it is in flight** -- no request, no drain, and above
  /// all no save write.
  ///
  /// Bounded, and the bound is the implementation's. Returning late is a console
  /// that will not sleep (sys-con#155); returning early is hard rule 2 with a
  /// save half written. What an implementation may not do is choose the second
  /// silently: a quiesce that gave up says so in the log.
  virtual void Quiesce() = 0;

  /// Let it all go again. Called on the way back up, and on nothing else.
  virtual void Resume() = 0;
};

/// The loop: one request, one dispatch, one acknowledgement, forever.
///
/// **It owns no thread**, deliberately. On the console the thread is a libnx
/// `Thread` with a stack sized against the 768 KiB inner heap
/// (`kInnerHeapSize`, `main.cpp`), which `std::thread` gives no way to ask for;
/// in the tests it is the test's own. So this is the part both share -- the
/// order of the calls and the promise that each request is answered exactly once
/// -- and the thread is the platform's.
class Watcher {
 public:
  /// Neither is owned and both must outlive this.
  Watcher(Module& module, Sink& sink) : module_(module), sink_(sink) {}

  /// Run until `Module::Stop()`. One `Module::Acknowledge` per request always --
  /// including for a state this build does not recognise, which is acknowledged
  /// and otherwise ignored -- and a `Sink` call only where the console actually
  /// changed direction.
  ///
  /// **It never returns leaving the sink quiesced.** A subscription that fails
  /// mid-sleep would otherwise strand the process: nothing is left to deliver
  /// the wake, so the sink stays suspended for the rest of the boot. Coming out
  /// of this loop resumes it, which is the difference between a client that is
  /// degraded and one that is silently inert.
  void Run();

 private:
  Module& module_;
  Sink& sink_;

  /// Whether the last thing the sink was told was to go quiet. The edge
  /// detector `Sink` is documented against: `false` at start, so a console that
  /// is already awake when this subscribes is not resumed out of a suspend that
  /// never happened.
  bool quiesced_ = false;
};

/// The console's subscription, and the thread that answers it (`power_psc.cpp`).
///
/// Returns null when `psc:m` could not be reached or the module could not be
/// registered, which leaves the console exactly where it was before this issue
/// -- asleep-unaware -- rather than refusing to start. The caller says so in the
/// log; there is nothing else to be done about it from here.
///
/// The returned object owns the thread and stops it in its destructor. On the
/// console it is never destroyed: `main` does not return.
class Subscription {
 public:
  virtual ~Subscription() = default;
};
std::unique_ptr<Subscription> Subscribe(Sink& sink);

}  // namespace rommsync::sysmodule::power
