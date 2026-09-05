// The Horizon side of the IPC contract: buffers in, buffers out, and a `Result`.
//
// **This file holds no logic.** Every decision a command makes is
// `ipc::ServiceCore`'s and runs on the host under `ctest`
// (core/src/ipc_service.cpp); what is left here is the part that cannot be
// tested off-console -- unpacking a `cmif` message, bounds-checking the two
// buffers it points at, and turning a portable `ipc::Error` into a Horizon
// `Result`. That split is deliberate: this is the code with no debugger attached
// and no test that can reach it before the M8-1 gate, so it is kept to a size a
// person can check by reading it.
//
// `core/` may not name a `Result` (hard rule 4), which is why the mapping lives
// on this side of the boundary rather than in the header both halves include.
//
// What is *not* here, on purpose: hosting the service. Registering the port
// (`smRegisterServiceCmif`), accepting sessions and `svcReplyAndReceive` belong
// with the sysmodule's service loop, which arrives with the first screen that
// talks to it (M4-1, #23). This is the function that loop calls.
#pragma once

#include <switch.h>

#include "rommsync/ipc.hpp"

namespace rommsync::sysmodule {

/// The Horizon result module these errors are reported under.
///
/// Arbitrary, and ours: nothing allocates module numbers to homebrew, so what
/// matters is only that it is not one a caller would confuse for somebody
/// else's. It is deliberately not `Module_Libnx` (345), which is what a libnx
/// call itself failing would report.
inline constexpr u32 kResultModule = 420;

/// `error` as a Horizon `Result`. `kOk` is `0`; everything else is
/// `MAKERESULT(kResultModule, <the enum's ordinal>)`, so a description of `N`
/// reads straight off `ipc::Error` and a new error cannot silently reuse one.
Result ToResult(ipc::Error error);

/// Answer one `cmif` request out of `message`, which is the calling thread's IPC
/// buffer (`armGetTls()`).
///
/// The wire is uniform (docs/DEVELOPMENT.md#ipc): one command id, an optional
/// **In** buffer holding the request payload, an optional **Out** buffer for the
/// response, and a `u64` in the reply saying how many bytes of it are valid. So
/// this is one function rather than fourteen stubs -- there is no per-command
/// marshalling to get wrong.
///
/// The message is overwritten with the reply, which is how `svcReplyAndReceive`
/// works. A request that is not a `cmif` `Request`, or whose header does not
/// carry the magic, is answered with a failing `Result` rather than parsed
/// further: the buffer is written by another process.
Result HandleRequest(ipc::ServiceCore& core, void* message);

}  // namespace rommsync::sysmodule
