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

#include "result.hpp"
#include "rommsync/ipc.hpp"

namespace rommsync::sysmodule {

// `kResultModule` and `ToResult` are `result.hpp`'s. They are the half of this
// boundary that is arithmetic rather than `cmif`, so M9-7 (#198) moved them
// where a host compiler can reach them and the overlay's inverse can be tested
// against the real mapping instead of a copy of it.

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
