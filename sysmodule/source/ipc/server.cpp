#include "server.hpp"

#include <switch.h>

#include <cstddef>

#include "rommsync/ipc.hpp"
#include "service.hpp"

namespace rommsync::sysmodule {
namespace {

/// A client closing its session: `serviceClose` sends this before dropping the
/// handle. It carries no payload and expects no reply -- answering one would
/// leave a reply queued for a handle that is about to go away.
///
/// A second parse of the same buffer, rather than a return value threaded out
/// of `HandleRequest`: that function is the reviewed, logic-free half of this
/// boundary (`service.hpp`) and it is worth more kept that way than the few
/// instructions `hipcParseRequest` costs on a read-only buffer.
bool IsCloseRequest(void* message) {
  return hipcParseRequest(message).meta.type == CmifCommandType_Close;
}

}  // namespace

Result RegisterPort(Handle* port) {
  // Cmif specifically, not `smRegisterService`'s auto-negotiation: the wire this
  // contract describes is cmif (`ipc.hpp`), and the overlay's client speaks it
  // through `serviceDispatchOut`. Letting the two ends negotiate would make the
  // encoding depend on the firmware rather than on the contract.
  return smRegisterServiceCmif(port, smEncodeName(ipc::kServiceName), /*is_light=*/false,
                               static_cast<s32>(kMaxSessions));
}

ServiceServer::ServiceServer(ipc::ServiceCore& core, Handle port) : core_(core) {
  handles_[0] = port;
  handle_count_ = 1;
}

ServiceServer::~ServiceServer() {
  for (s32 i = 0; i < handle_count_; ++i) {
    svcCloseHandle(handles_[i]);
  }
  handle_count_ = 0;
}

void ServiceServer::Drop(std::size_t index) {
  svcCloseHandle(handles_[index]);
  // The last live entry moves into the hole. Nothing outside this loop holds an
  // index into the table, and `svcReplyAndReceive` is handed the array afresh
  // every pass, so a dense table is simply one fewer thing to be wrong about.
  handles_[index] = handles_[handle_count_ - 1];
  --handle_count_;
}

Result ServiceServer::Serve() {
  s32 index = 0;
  // UINT64_MAX is libnx's "no timeout": this is the one place the process
  // blocks, and it blocks until somebody says something.
  const Result rc =
      svcReplyAndReceive(&index, handles_, handle_count_, reply_target_, UINT64_MAX);

  // Whatever happens below, the reply that was pending has been sent or has
  // failed to send. Leaving it armed would send the next pass's reply twice.
  const Handle replied_to = reply_target_;
  reply_target_ = INVALID_HANDLE;

  if (R_FAILED(rc)) {
    if (rc != KERNELRESULT(ConnectionClosed)) {
      // Not something this loop knows how to recover from: an invalid handle or
      // an exhausted resource is a bug rather than a client hanging up.
      return rc;
    }
    // A session went away, and which one depends on *when*. If there was a
    // reply pending, the send is what failed and the kernel never got as far as
    // waiting -- so `index` was not written and must not be read. Only when
    // there was no reply pending does `index` name a handle.
    if (replied_to != INVALID_HANDLE) {
      for (s32 i = 1; i < handle_count_; ++i) {
        if (handles_[i] == replied_to) {
          Drop(static_cast<std::size_t>(i));
          break;
        }
      }
      return 0;
    }
    // From 1: index 0 is the port, and a port does not close. Reading an
    // unwritten `index` as 0 and dropping it would take the service down and
    // leave the process running, which is the worst shape this failure has.
    if (index >= 1 && index < handle_count_) {
      Drop(static_cast<std::size_t>(index));
    }
    return 0;
  }

  if (index == 0) {
    Handle session = INVALID_HANDLE;
    const Result accepted = svcAcceptSession(&session, handles_[0]);
    if (R_FAILED(accepted)) {
      // Refusing a connection is survivable; giving up the service is not. A
      // client that could not be taken on retries, and the loop stays up.
      return 0;
    }
    if (handle_count_ > static_cast<s32>(kMaxSessions)) {
      // The port was registered with `kMaxSessions`, so the kernel should not
      // have signalled it. Closing the session it just handed us is the honest
      // answer either way -- overrunning the array is not.
      svcCloseHandle(session);
      return 0;
    }
    handles_[handle_count_] = session;
    ++handle_count_;
    return 0;
  }

  if (index < 1 || index >= handle_count_) {
    return 0;
  }

  void* message = armGetTls();
  if (IsCloseRequest(message)) {
    Drop(static_cast<std::size_t>(index));
    return 0;
  }

  // Everything a command decides happens in here, and it is all `core/`'s
  // (`service.hpp`). The message is overwritten with the reply, which is what
  // the next `svcReplyAndReceive` sends -- hence arming `reply_target_` rather
  // than sending anything now.
  const Result handled = HandleRequest(core_, message);
  if (R_FAILED(handled)) {
    // `HandleRequest` reports a refused *command* inside the reply and only
    // fails when it could not lay one out at all. There is nothing to send, so
    // the session is dropped rather than left waiting on a reply that will not
    // come.
    Drop(static_cast<std::size_t>(index));
    return 0;
  }
  reply_target_ = handles_[index];
  return 0;
}

void ServiceServer::Run() {
  while (R_SUCCEEDED(Serve())) {
  }
}

}  // namespace rommsync::sysmodule
