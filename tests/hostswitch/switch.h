// The handful of libnx that `overlay/source/ipc_client.cpp` touches, so the
// client half of the IPC wire compiles and runs on a host (M9-7, #198).
//
// **This is not a libnx emulator and must not grow into one.** It exists for one
// translation unit: `ipc_client.cpp` names twelve libnx symbols and no more, and
// every one of them is below. Anything that needs a thirteenth is either a file
// that does not belong on this seam or a sign the seam has moved -- say so in
// `overlay/AGENTS.md` rather than adding a stub here. `tesla.hpp` in particular
// is deliberately absent: `tsl::gfx::Renderer` is `final` with non-virtual
// inline methods and cannot be faked, which is why the screens are split at
// `DrawList` instead (`overlay/source/draw_list.hpp`).
//
// It is on the include path of `test_overlay_native` alone, so no other target
// can pick it up by accident, and `core/` still names no libnx type: hard rule 4
// is about `core/`, and nothing here is in it.
//
// ## What the numbers are
//
// `Module_Libnx` and the `LibnxError_*` ordinals are libnx's own. Nothing in
// these tests depends on the exact values -- they are compared only against each
// other, inside one process, and never cross a wire -- but a shim that invented
// them would be a second set of constants for a reader to reconcile with the
// device build.
//
// ## What it does NOT stand in for
//
// The `cmif`/`hipc` message itself. `sysmodule/source/ipc/service.cpp`'s
// `HandleRequest` unpacks a real Horizon message, and reimplementing that here
// would be testing this file rather than that one. What the seam models is the
// contract either side of it: request bytes in, response bytes out, a `Result`,
// and the `u64` reply word saying how much of the Out buffer is valid. See
// `hostswitch::Server`, and docs/TESTING.md for the limit written down.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

using Result = std::uint32_t;

#define MAKERESULT(module, description) \
  ((((module) & 0x1FFu) | ((description) & 0x1FFFu) << 9))
#define R_MODULE(res) ((res) & 0x1FFu)
#define R_DESCRIPTION(res) (((res) >> 9) & 0x1FFFu)
#define R_SUCCEEDED(res) ((res) == 0u)
#define R_FAILED(res) ((res) != 0u)

/// libnx's own module number, and the three of its errors this client answers.
/// Spelled as libnx spells them for the reason in the header note.
enum { Module_Libnx = 345 };

/// The kernel's, and the one failure of its this seam produces: a session whose
/// far end is gone. It is what a call into a sysmodule that exited returns, and
/// it is the case `ScreenFrame::Diagnose` exists for -- so the shim answers the
/// real thing rather than inventing one in our own module, which the overlay
/// would read as a refusal.
enum { Module_Kernel = 1, KernelError_SessionClosed = 301 };
enum {
  LibnxError_NotInitialized = 8,
  LibnxError_NotFound = 9,
  LibnxError_BadInput = 11,
  LibnxError_InvalidCmifOutHeader = 47,
};

/// The buffer attributes `IpcClient::Call` tags its two buffers with. The values
/// are libnx's; what the shim checks is that both are present and pointed the
/// right way, because a client that sent its request as an Out buffer would be
/// handing the sysmodule a buffer it may not read.
enum {
  SfBufferAttr_In = (1u << 0),
  SfBufferAttr_Out = (1u << 1),
  SfBufferAttr_HipcMapAlias = (1u << 2),
};

namespace hostswitch {

/// The far side of one session: what `sysmodule::HandleRequest` does once the
/// Horizon message is unpacked.
///
/// `response_length` is the reply's data word -- the sysmodule's
/// `ReplyPayload::response_length` -- because the Out buffer a client supplies
/// is a capacity and the client cannot infer how much of it was filled.
class Server {
 public:
  virtual ~Server() = default;

  virtual Result Handle(u32 command_id, const void* request, std::size_t request_size,
                        void* response, std::size_t response_capacity,
                        u64* response_length) = 0;
};

/// The service table `smGetService` looks in. One name, because that is how many
/// this client opens; a second registration replaces the first.
///
/// `Unregister` is a sysmodule exiting: the port goes, **and so does every
/// session already open on it**. A client holding one does not find out until
/// its next call, which is exactly the case `ScreenFrame::Diagnose` is written
/// for -- a shim that let a live session keep working would make that path
/// unreachable.
void Register(const char* name, Server* server);

/// Takes the server it is giving up, so a fixture that went out of scope after a
/// later one registered cannot unregister the later one's port.
void Unregister(const char* name, Server* server);

/// Every dispatch this process has made, for the assertions that are about the
/// call rather than about the answer.
struct Dispatched {
  u32 command_id = 0;
  std::size_t request_size = 0;
  std::size_t response_capacity = 0;
  u32 in_attr = 0;
  u32 out_attr = 0;
};
const Dispatched& LastDispatched();

}  // namespace hostswitch

/// A session, as far as this client uses one. `serviceIsActive` is the whole of
/// what it reads off it.
///
/// `generation` is what makes a session outlive its port the way a real one
/// does: `serviceIsActive` still says yes on a session whose sysmodule exited,
/// because a handle stays a handle until somebody uses it.
struct Service {
  hostswitch::Server* server = nullptr;
  unsigned long generation = 0;
};

inline bool serviceIsActive(Service* s) { return s != nullptr && s->server != nullptr; }
inline void serviceClose(Service* s) {
  if (s != nullptr) {
    s->server = nullptr;
  }
}

Result smGetService(Service* out, const char* name);

/// libnx's `SfBufferAttr`/`SfBuffer` pair, in the shape
/// `serviceDispatchOut`'s designated-initialiser call site builds.
struct SfBuffer {
  const void* ptr = nullptr;
  std::size_t size = 0;
};

struct SfDispatchParams {
  u32 buffer_attrs[2]{};
  SfBuffer buffers[2]{};
};

namespace hostswitch {

/// One command, with the two buffers already unpacked. `out` is the reply's
/// data word; `out_size` is checked rather than assumed because a client that
/// declared a different out type would be reading a different reply.
Result DispatchOut(Service* service, u32 command_id, void* out, std::size_t out_size,
                   const SfDispatchParams& params);

}  // namespace hostswitch

/// libnx's variadic dispatch macro, in the one form this client uses: an Out
/// value plus the two mapped buffers, written with designated initialisers.
#define serviceDispatchOut(_s, _rid, _out, ...)                                 \
  ::hostswitch::DispatchOut((_s), (_rid), &(_out), sizeof(_out), SfDispatchParams{__VA_ARGS__})
