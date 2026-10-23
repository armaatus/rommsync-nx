// The overlay's half of the IPC contract.
//
// `ovl-rommsync` is a thin client (overlay/AGENTS.md): it renders what the
// sysmodule reports and asks it to do things. This is the only place it talks
// to `sys-rommsync`, and every payload it sends or reads goes through the *same*
// codecs the sysmodule uses (`rommsync/ipc.hpp`) -- one copy of the field names,
// so the two halves cannot disagree about one.
//
// That is why this target compiles `core/` at all. It links no engine: the
// screens call nothing but the methods below, and `--gc-sections` drops what is
// unreferenced. The rule that matters is unchanged -- no sync, download or auth
// logic lives here.
//
// Nothing here blocks on the network, because nothing on the other side does
// either (docs/DEVELOPMENT.md#ipc). `SyncNow`, `StartPair` and `RestoreBackup`
// return as soon as the sysmodule has taken the work; a screen polls
// `GetStatus`, `GetPairState` or `GetRestoreState` while it runs.
//
// **And nothing here waits on the sysmodule for longer than a budget** (M9-6,
// #209). That rule above is a promise about the far side, and a promise is not
// a bound: a sysmodule whose IPC thread is stuck would otherwise hold
// `serviceDispatchOut` -- and with it the draw thread, the panel and the
// user's exclusive input -- forever. So every call is dispatched on a worker
// thread of this client's, and the draw thread waits for it for at most
// `kCallBudget`. Past that the call answers `NotResponding()`, the screen draws
// `Link::kNotResponding`, and every later call answers the same thing at once
// until the one that is out comes back. Its late answer is dropped rather than
// handed to whichever call is next.
//
// `cmif` cannot cancel a request once it is sent, so "past the budget" means
// the *caller* stopped waiting, not that the call stopped. That is why the
// worker exists at all, and why `Shutdown` -- the one place this client cannot
// stop waiting -- is the exit path rather than a draw.
//
// **This file runs.** M9-7 (#198) compiles it on the host behind
// `tests/hostswitch/switch.h` and drives every method below against the real
// `ipc::Dispatch` and the real `sysmodule::ToResult` -- `ctest -R overlay.wire`,
// `overlay.roundtrip`, `overlay.errors`. What is still hardware-only is the
// Horizon transport underneath: sm answering 65100 for `rommsync` (`Open`),
// `smGetService` finding a real port, and the
// `cmif` message `sysmodule/source/ipc/service.cpp` unpacks. Those are verified
// after the M8-1 gate (overlay/AGENTS.md).
#pragma once

#include <pthread.h>
#include <switch.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"

namespace rommsync::overlay {

/// The `Result` every typed method below answers when the sysmodule replied
/// with a payload this build cannot read.
///
/// Distinct from the sysmodule's own errors on purpose: those say what the
/// *command* could not do, and this says the two halves disagree about the
/// payload -- which is a different sentence for the user ("update the overlay")
/// and is what `GetInterfaceVersion` exists to diagnose. Named here rather than
/// kept private because a screen has to tell it from a transport failure to
/// pick between "not running" and "unreachable" (`status_screen.cpp`).
constexpr Result MalformedResponse() {
  return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
}

/// The `Result` a call answers when the sysmodule did not answer it within the
/// client's budget -- or when an earlier call that did not is still out.
///
/// The kernel's own "timed out", which is what `svcWaitSynchronization` answers
/// when a wait runs out, and for `MalformedResponse`'s reason it is named here:
/// a screen has to tell it from a transport failure, because the cure for one
/// is reopening the session and the cure for this is not touching it (the
/// worker is still dispatching on it). `ScreenFrame::Diagnose` reads it as
/// `Link::kNotResponding`. It is not in `ipc::kResultModule`, so `DecodeError`
/// never mistakes it for a refusal.
constexpr Result NotResponding() { return MAKERESULT(Module_Kernel, KernelError_TimedOut); }

/// The `ipc::Error` a failing `Result` carries, when it carries one.
///
/// The sysmodule maps every refusal onto `MAKERESULT(ipc::kResultModule, <the
/// enum's ordinal>)` (`sysmodule/source/ipc/result.hpp`), and until this
/// existed the overlay threw that away: `Enqueue` answering `kDuplicate` and a
/// sysmodule that is not running both reached a screen as "a `Result` that
/// failed", so `ScreenFrame::Diagnose` read the first as the second and drew
/// "sys-rommsync is not running" over a rom that was simply already queued.
/// M4-3 (#25) is the first screen whose refusals mean different things.
///
/// True when `rc` is one of ours and `out` now holds it -- the *command*
/// refused, and the sysmodule is there. False for anything else, including a
/// description in our module that this build has no `Error` for: a sysmodule
/// from a newer release, which is `Diagnose`'s to name rather than a refusal to
/// invent. `kOk` is never reported here; a `Result` of 0 has not failed.
bool DecodeError(Result rc, ipc::Error* out);

/// A session on `sys-rommsync`.
///
/// `Open` assumes `smInitialize()` has already been called -- Tesla does it
/// before an overlay draws, and an overlay that initialised sm itself would be
/// fighting its host for the session.
///
/// Called from one thread -- the draw thread -- and only from it. One session
/// answers one request at a time, so there is never more than one call out, and
/// the worker that carries it is this class's own business (see the header
/// note).
class IpcClient {
 public:
  /// How long the draw thread waits for one answer.
  ///
  /// Half a second, which is a frame held long enough to see, once, rather than
  /// forever. It is sized against the slowest command that is still answered on
  /// the IPC thread -- `SetConfig`/`SetEnabled`, a `config.ini` written
  /// atomically to the card, tens of milliseconds and a few hundred under a
  /// card that is busy with a download -- because a budget a healthy sysmodule
  /// can miss turns a slow write into a "not responding" screen and loses its
  /// answer. Everything that takes longer than that is a command that starts
  /// work and is polled (`RestoreBackup`, #209), not a call to wait for.
  static constexpr std::chrono::milliseconds kCallBudget{500};

  explicit IpcClient(std::chrono::milliseconds budget = kCallBudget);

  /// `Shutdown()`.
  ~IpcClient();

  IpcClient(const IpcClient&) = delete;
  IpcClient& operator=(const IpcClient&) = delete;

  /// `smGetService("rommsync")`. `MAKERESULT(Module_Libnx, LibnxError_NotFound)`
  /// when the sysmodule is not running, which is the case an overlay has to be
  /// able to say something about: it is what a user who forgot to enable it
  /// sees.
  ///
  /// **Why this is the one call #209's budget does not bound, decided in M9-27
  /// (#254).** It goes to `sm`, not to the sysmodule, and the two ways it could
  /// hang are not the same problem:
  ///
  ///   * **A stuck `sm`** is not bounded, deliberately. Every process on the
  ///     console acquires its services through it, so a console whose `sm` has
  ///     stopped answering has stopped -- there is no panel left to keep
  ///     responsive, and a budget would only add a worker that `Shutdown` then
  ///     waits on forever (`cmif` and `tipc` cannot cancel a request).
  ///   * **A deferred request** is the one that happens, and it is prevented
  ///     rather than timed. Atmosphere's sm does not refuse a name that is not
  ///     registered -- it parks the request until the name appears
  ///     (`sysmodule/source/boot_wait.hpp`, #195) -- and nx-ovlloader may ask
  ///     for any name, so with `sys-rommsync` switched off a bare
  ///     `smGetService` holds the draw thread, the panel and the user's input
  ///     forever. So sm is asked first whether `rommsync` is registered
  ///     (`AtmosphereHasService`, command 65100, which answers rather than
  ///     defers), and an unregistered name answers `LibnxError_NotFound`
  ///     without the request that would park. `overlay.version` holds this.
  ///
  /// What it leaves, stated: a sysmodule that unregisters between the two calls
  /// -- one exiting at that instant -- still parks the second; and an `sm` that
  /// does not answer 65100 is not Atmosphere's, so the acquisition goes ahead
  /// unasked exactly as before, the conservative direction `main.cpp`'s
  /// `MayProceedWith` takes for the same reason. Neither has been seen on a
  /// console: nothing here has run on one before the M8-1 gate.
  Result Open();

  /// Drop the session. **A no-op while a call is still out**, because the
  /// worker is dispatching on it -- `ScreenFrame` only reopens after a call that
  /// came back with a transport failure, which leaves nothing out, so this is
  /// the guard rather than a path anyone takes.
  void Close();

  /// Everything this client holds, given back: the call that is out waited for,
  /// the worker stopped and joined, the session closed. Idempotent, and the
  /// only teardown there is -- `Lifecycle::Stop` and the destructor both end
  /// here.
  ///
  /// **The one wait in this class with no budget.** `cmif` cannot cancel a
  /// request, and nx-ovlloader unloads the overlay's code the moment `main`
  /// returns, so a worker still inside `serviceDispatchOut` cannot be left
  /// behind: it would return into memory that is no longer ours. An exit with a
  /// call out therefore waits for the sysmodule to answer it. libtesla calls
  /// `requestForeground(false)` and tears the renderer down *before*
  /// `exitServices` (tesla.hpp's tail), so the wait holds neither the panel nor
  /// the user's input.
  void Shutdown();

  bool open() { return serviceIsActive(&service_); }

  /// A call that ran out of budget has not come back yet. Every call answers
  /// `NotResponding()` at once while this holds.
  bool busy();

  /// How many workers this process has running, across every client. What
  /// `overlay.cycles` balances: a worker left behind by a close is a thread
  /// returning into an overlay nx-ovlloader has already unloaded.
  static int Workers();

  /// The contract the *sysmodule* speaks. Compare it with `ipc::kVersion`: a
  /// mismatch means one half needs updating, and this is the only call that is
  /// safe to make before knowing (its encoding is frozen).
  Result GetInterfaceVersion(std::uint32_t* out);

  Result GetStatus(ipc::Status* out);
  Result GetConfig(ipc::ConfigView* out);

  /// The answer says what happened (`result->outcome`) and why
  /// (`result->diagnostics`) -- **both on a refusal**, where the diagnostics are
  /// the entire answer. A failing `Result` here means the call did not happen;
  /// a refused edit is a successful call with `kInvalid` in it. See
  /// `ipc::WriteOutcome` for why it has to be that way round.
  Result SetConfig(const ipc::ConfigEdit& edit, ipc::ConfigResult* result);

  /// Same shape. `result->enabled` is the state as it stands afterwards, which
  /// is what the switch should be drawn as -- not the state that was asked for,
  /// and not lost when the write failed.
  Result SetEnabled(bool enabled, ipc::EnabledResult* result);

  Result SyncNow(ipc::SyncOutcome* outcome);
  Result StartPair(auth::PairingStatus* status);
  Result GetPairState(auth::PairingStatus* status);
  Result Unpair();

  Result Enqueue(std::int64_t rom_id, std::int32_t* position);
  Result Dequeue(std::int64_t rom_id);

  Result ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor);
  Result ListNext(ipc::Cursor cursor, ipc::ListPage* page);
  Result ListEnd(ipc::Cursor cursor);

  /// M7-1 (#36). Neither touches the network: the history is what an earlier
  /// tick wrote, and a restore is one copy between two files on the card.
  Result ListConflicts(const ipc::ConflictQuery& query, ipc::ConflictPage* page);

  /// Ask for the restore, and where it stands the instant it was taken --
  /// usually `kRunning`, because the copy is the sysmodule worker's and is not
  /// waited for (M9-6, #209). `GetRestoreState` is the poll that follows.
  Result RestoreBackup(std::int64_t entry_id, ipc::RestoreState* state);
  Result GetRestoreState(ipc::RestoreState* state);

  /// Keep local for a save conflict (#249): `RestoreBackup`'s shape, polled
  /// with `GetRestoreState`, and the next upload of that save goes over the
  /// server's copy.
  Result KeepLocal(std::int64_t entry_id, ipc::RestoreState* state);

  /// The last `lines` lines of the sysmodule's log, from memory (M7-3, #38).
  /// The log screen's, since #209 wired it: until then it was the one command
  /// the sysmodule dispatched with nothing here to send it.
  Result GetLog(std::int32_t lines, ipc::LogTail* tail);

 private:
  /// Where the one call is. `kDone` with `abandoned_` set is a late answer
  /// nobody is waiting for, which the next `Call` drops.
  enum class Slot { kIdle, kQueued, kRunning, kDone };

  /// One command: a request payload in, `response_` out, handed to the worker
  /// and waited for for at most `budget_`.
  ///
  /// The buffers are members rather than locals, and are reused across calls:
  /// `kMaxPayloadBytes` is 8 KiB, an overlay's draw thread does not have that to
  /// spare on the stack, and the status screen polls every frame -- so an
  /// allocation per call would be an allocation per frame. They are also what
  /// the worker reads and writes, which is the other reason they cannot be the
  /// caller's: a call the caller gave up on is still using them.
  Result Call(ipc::Command command, std::string_view request);

  /// `serviceDispatchOut`, and the checks on what came back. Runs on the worker,
  /// or -- when the worker could not be started -- on the caller, unbounded, as
  /// every call did before #209.
  Result Dispatch(ipc::Command command);

  /// The worker's loop.
  void Run();
  static void* Enter(void* self);

  /// Start the worker if it is not running. False when it could not be.
  /// `mutex_` held.
  bool StartWorkerLocked();

  /// The shape every typed method above is: send, decode `response_`, and turn a
  /// payload this build cannot read into one named `Result` rather than into a
  /// half-filled struct. Written once because there are thirteen of them, and
  /// because a fix applied to twelve is the bug this whole header exists to
  /// prevent.
  template <typename T, typename Decode>
  Result CallAndDecode(ipc::Command command, std::string_view request, Decode decode, T* out);

  Service service_{};
  std::string request_;
  std::string response_;

  const std::chrono::milliseconds budget_;

  /// Guards everything below, and is never held across a dispatch.
  std::mutex mutex_;
  /// The worker waits on this for a call, or to stop.
  std::condition_variable work_;
  /// The caller waits on this for the answer.
  std::condition_variable done_;

  Slot slot_ = Slot::kIdle;
  ipc::Command command_ = ipc::Command::kGetInterfaceVersion;
  Result result_ = 0;
  /// The caller stopped waiting for the call in `slot_`.
  bool abandoned_ = false;
  bool stopping_ = false;

  ::pthread_t thread_{};
  bool started_ = false;
};

}  // namespace rommsync::overlay
