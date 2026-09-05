// Hosting the `rommsync` service: the port, the session table, and the loop.
//
// `service.hpp` answers one request out of one IPC buffer. This is what calls
// it: `smRegisterService` claims the name, `svcAcceptSession` takes a client on,
// and `svcReplyAndReceive` is the single blocking point the process spends its
// life in. That split is the same one `service.hpp` explains -- this file also
// holds no logic, because it is also code with no debugger attached and no test
// that can reach it before the M8-1 gate (M4-1, #23).
//
// **Nothing here has ever run.** It is exercised in Ryujinx as a
// manually-launched build first, never as an auto-boot sysmodule and never on
// hardware before the gate (sysmodule/AGENTS.md).
//
// ## Why the port is registered before `smExit`
//
// A registered port outlives the `sm` session that registered it, so a resident
// process does not hold an `sm` handle for the life of the console just to keep
// its own service name. `main.cpp` registers inside `__appInit`, where `sm` is
// still up, and hands the handle here.
#pragma once

#include <switch.h>

#include <cstddef>

#include "rommsync/ipc.hpp"

namespace rommsync::sysmodule {

/// How many overlays may hold a session at once.
///
/// Three rather than one: Tesla tears an overlay's gui down and rebuilds it on
/// every hide, and a session whose close has not been processed yet would
/// otherwise refuse the next open. Each session costs a kernel handle and no
/// heap of ours -- the request buffer is the calling thread's own TLS page.
inline constexpr std::size_t kMaxSessions = 3;

/// Claim the service name. Call while `sm` is still initialised.
///
/// The handle it writes is the port, and it is what `ServiceServer` is built
/// on. Split from the server so the registration can happen in `__appInit`,
/// before the process has an engine to serve from.
Result RegisterPort(Handle* port);

/// The port, its sessions, and the loop over them.
class ServiceServer {
 public:
  /// Takes ownership of `port`.
  ServiceServer(ipc::ServiceCore& core, Handle port);
  ~ServiceServer();

  ServiceServer(const ServiceServer&) = delete;
  ServiceServer& operator=(const ServiceServer&) = delete;

  /// Serve until `svcReplyAndReceive` fails in a way that is not a closed
  /// session -- which, for a sysmodule, is never: this does not return in
  /// normal operation.
  void Run();

  /// One pass: reply to whoever was last answered, wait, and handle whatever
  /// arrives. Separated from `Run` because it is the unit a reader can check,
  /// and because a shutdown path that needs to interleave something else has
  /// somewhere to put it.
  ///
  /// `timeout_ns` bounds the wait; `UINT64_MAX` waits forever. A timeout is not
  /// an error and is not reported as one.
  Result Serve(u64 timeout_ns);

 private:
  /// Drop the session at `index` and close its handle. The table stays dense,
  /// so the last entry moves into the hole: nothing indexes a session by
  /// anything but its position in this array.
  void Drop(std::size_t index);

  ipc::ServiceCore& core_;

  /// `handles_[0]` is the port; the rest are live sessions. One array because
  /// that is what `svcReplyAndReceive` takes, and keeping the port at index 0
  /// is what makes "a new client" and "an existing one said something" a single
  /// comparison.
  Handle handles_[kMaxSessions + 1] = {};
  s32 handle_count_ = 0;

  /// The session whose reply is sitting in this thread's IPC buffer, waiting to
  /// be sent by the next `svcReplyAndReceive`. `INVALID_HANDLE` when there is
  /// nothing to send -- which is the case on the first pass, after a timeout,
  /// and after a session closed under us.
  Handle reply_target_ = INVALID_HANDLE;
};

}  // namespace rommsync::sysmodule
