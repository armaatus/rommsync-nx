// How long `__appInit` may wait for a service, and what it leaves behind when it
// runs out (M9-1, #195).
//
// A service in this process's SAC but not yet registered does not make `sm`
// fail the request -- it makes it **defer** it, and a deferred request is never
// returned (`sm_service_manager.cpp`: `R_UNLESS(service_info != nullptr,
// tipc::ResultRequestDeferred())`). `svcStartProcess` is asynchronous, so boot2
// does not wait on us either. The console therefore boots normally, this process
// sits in `__appInit` forever, and there is no crash report and no log -- the
// log sink does not exist until `main` -- so the overlay says "not running" with
// nothing to say why. That is the worst realistic failure mode this sysmodule
// has, and it is what these two pieces are for: a bound on every acquisition,
// and a place to write the reason down before there is anywhere to write.
//
// **It names no libnx type**, so `sysmodule/AGENTS.md`'s rule applies and CMake
// compiles it for the host: the policy is the part that can be got wrong, and it
// is checked by `boot.wait` and `boot.journal` rather than argued about in a
// comment. The two platform facilities it needs -- asking `sm` whether a service
// is registered, and sleeping -- arrive through `Waiter`, whose Horizon
// implementation is in `main.cpp` beside the `__appInit` that uses it.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace rommsync::sysmodule::boot {

/// What one service is given to appear in, and how often it is asked.
///
/// Ten seconds is chosen against what actually registers late rather than as a
/// round number: everything this process asks for comes from `psc`, `fs` or
/// boot2's own list, all of which are up before boot2 launches a
/// `/atmosphere/contents` sysmodule. A service still missing after ten seconds
/// is missing because the SAC is wrong or the module is not installed, and no
/// amount of further waiting fixes either. Nothing outside this process waits on
/// it -- `svcStartProcess` does not block -- so the cost of the bound is paid by
/// this sysmodule alone and never by the console's boot (CLAUDE.md).
///
/// **What it buys at worst, since it is per service and not per boot**:
/// `__appInit` waits seven times -- `set:sys`, `fsp-srv`, `time:s`, then
/// `nifm:u`, `bsd:u`, `sfdnsres` and `ssl` -- so a console where every one of
/// them registers just before its own deadline spends **~70 seconds** in
/// `__appInit` and then comes up working. The give-up case is shorter: a name
/// that never arrives ends the chain where it sits. Whoever changes this
/// constant is changing that ceiling by seven times the delta.
inline constexpr std::chrono::milliseconds kServiceBudget{10000};

/// 100 ms, which is a fifth of what sys-clk polls `pmdmnt` at. The whole poll
/// costs one tipc round trip to `sm`, so a hundred of them is cheaper than the
/// single `fsInitialize` that follows.
inline constexpr std::chrono::milliseconds kPollInterval{100};

struct Policy {
  std::chrono::milliseconds budget = kServiceBudget;
  std::chrono::milliseconds interval = kPollInterval;
};

struct Outcome {
  bool ready = false;                    ///< the service registered within the budget
  unsigned polls = 0;                    ///< how many times it was asked
  std::chrono::milliseconds waited{0};   ///< how much of the budget was spent
};

/// The two platform facilities a bounded wait needs.
///
/// `Ready` is Atmosphere's `AtmosphereHasService` (sm command 65100), which
/// answers rather than defers -- unlike `GetServiceHandle`, which is the call
/// that hangs. libnx does not export it; `main.cpp` dispatches it directly.
class Waiter {
 public:
  virtual ~Waiter() = default;

  Waiter(const Waiter&) = delete;
  Waiter& operator=(const Waiter&) = delete;

  /// True when `service` is registered. **False when the question could not be
  /// asked**, which is not the same thing: an `sm` that does not answer 65100 is
  /// not Atmosphere's, and `Probed()` is how the caller tells the two apart.
  virtual bool Ready(const char* service) = 0;

  /// True while `Ready` is answering the question rather than failing it.
  virtual bool Probed() const = 0;

  virtual void Sleep(std::chrono::milliseconds) = 0;

 protected:
  Waiter() = default;
};

/// Ask `waiter` for `service` until it says yes or the budget is spent.
///
/// Asks once before sleeping at all, so the ordinary case -- everything already
/// registered by the time boot2 gets to us -- costs one round trip and no delay.
/// A `waiter` that cannot ask the question stops immediately with `ready` false
/// and `polls` 1: waiting out ten seconds to re-learn that there is no answer
/// helps nobody, and the caller has to decide what to do about it either way.
Outcome WaitFor(const char* service, Waiter& waiter, Policy policy = {});

/// How many notes are kept, and how long each may be.
///
/// Twelve is two per service acquired plus room, and 112 bytes is the longest
/// note this file can produce with a service name and a result code in it. The
/// whole thing is 1.3 KiB of BSS, which is not on the heap and so is not a term
/// in the budget `main.cpp` computes.
inline constexpr std::size_t kMaxNotes = 12;
inline constexpr std::size_t kMaxNoteBytes = 112;

/// What `__appInit` could not say at the time.
///
/// **Trivially constructible, and that is a requirement rather than an
/// accident.** `__libnx_init` runs `__appInit` *before* `__libc_init_array`, so
/// a file-scope object `__appInit` touches has to be constant-initialised or it
/// is used before its constructor has run. Anything with a `std::string` in it
/// -- a vector, a deque, the log's own sink -- is out for that reason, which is
/// why this is fixed-size character storage and why `main.cpp` says so where the
/// object is declared.
struct Journal {
  char notes[kMaxNotes][kMaxNoteBytes];
  std::size_t count;
  std::size_t dropped;
};
static_assert(std::is_trivially_default_constructible_v<Journal>,
              "__appInit runs before __libc_init_array; the journal must be "
              "constant-initialised");

/// Append a line, truncating rather than growing and counting what it drops.
///
/// A full journal keeps the FIRST notes and counts the rest: the earliest
/// failure is the one that explains the others, and a ring buffer would keep the
/// consequences and throw away the cause.
void Note(Journal& journal, const char* what);

/// ...with a `Result` after it, in the form a crash report and a support thread
/// both quote: `what: 0x1015`.
void Note(Journal& journal, const char* what, std::uint32_t code);

/// Line `index`, or nullptr past the end.
const char* NoteAt(const Journal& journal, std::size_t index);

}  // namespace rommsync::sysmodule::boot
