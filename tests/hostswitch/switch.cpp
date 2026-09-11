// The host side of `tests/hostswitch/switch.h`. See that header for what this
// stands in for and, more importantly, what it does not.
#include "switch.h"

#include <cstring>
#include <string>

namespace {

/// The one registered service. A table would be a table of one: this client
/// opens `rommsync` and nothing else (`ipc::kServiceName`).
std::string& RegisteredName() {
  static std::string name;
  return name;
}

hostswitch::Server*& RegisteredServer() {
  static hostswitch::Server* server = nullptr;
  return server;
}

/// Bumped on every registration and every unregistration, so a session minted
/// before one is not the session after it.
unsigned long& Generation() {
  static unsigned long generation = 1;
  return generation;
}

hostswitch::Dispatched& LastDispatchedMutable() {
  static hostswitch::Dispatched last;
  return last;
}

}  // namespace

namespace hostswitch {

void Register(const char* name, Server* server) {
  RegisteredName() = name;
  RegisteredServer() = server;
  ++Generation();
}

void Unregister(const char* name) {
  if (RegisteredName() == name) {
    RegisteredName().clear();
    RegisteredServer() = nullptr;
    ++Generation();
  }
}

const Dispatched& LastDispatched() { return LastDispatchedMutable(); }

Result DispatchOut(Service* service, u32 command_id, void* out, std::size_t out_size,
                   const SfDispatchParams& params) {
  if (!serviceIsActive(service)) {
    return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
  }
  if (service->server != RegisteredServer() || service->generation != Generation()) {
    // The sysmodule exited while this session was open. The handle is still a
    // handle -- which is why `serviceIsActive` above said yes -- and the kernel
    // is what says otherwise, on the first call that uses it.
    return MAKERESULT(Module_Kernel, KernelError_SessionClosed);
  }
  // The reply word is a `u64` on the wire (`ReplyPayload::response_length`), so
  // a client that asked for anything else would be reading a different reply.
  // Refused rather than truncated: on a console it would be silent.
  if (out_size != sizeof(u64)) {
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  }

  Dispatched& last = LastDispatchedMutable();
  last.command_id = command_id;
  last.in_attr = params.buffer_attrs[0];
  last.out_attr = params.buffer_attrs[1];
  last.request_size = params.buffers[0].size;
  last.response_capacity = params.buffers[1].size;

  // Pointed the right way, both of them. A request tagged Out is a buffer the
  // sysmodule may not read, and the failure on a console would be whatever the
  // kernel left in the map rather than an error.
  const u32 wanted_in = SfBufferAttr_HipcMapAlias | SfBufferAttr_In;
  const u32 wanted_out = SfBufferAttr_HipcMapAlias | SfBufferAttr_Out;
  if (last.in_attr != wanted_in || last.out_attr != wanted_out) {
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  }

  u64 length = 0;
  // `const_cast` because libnx's `SfBuffer` holds a `const void*` for both
  // directions and the attribute is what says which it is. The Out buffer is
  // `IpcClient::response_`, which is not const.
  const Result result = service->server->Handle(
      command_id, params.buffers[0].ptr, params.buffers[0].size,
      const_cast<void*>(params.buffers[1].ptr), params.buffers[1].size, &length);
  if (R_FAILED(result)) {
    // libnx's `cmifParseResponse` returns before it exposes the reply's data
    // words when the result failed, so a client never sees a length on a
    // failure. Modelled, because `IpcClient::Call` relies on it.
    return result;
  }
  std::memcpy(out, &length, sizeof(length));
  return 0;
}

}  // namespace hostswitch

Result smGetService(Service* out, const char* name) {
  if (RegisteredServer() == nullptr || RegisteredName() != name) {
    // What a console with no `sys-rommsync` answers, and the state the status
    // screen has a sentence for.
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
  }
  out->server = RegisteredServer();
  out->generation = Generation();
  return 0;
}
