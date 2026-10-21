#include "ipc_client.hpp"

#include <pthread.h>
#include <switch.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"

namespace rommsync::overlay {

bool DecodeError(Result rc, ipc::Error* out) {
  if (R_SUCCEEDED(rc) || R_MODULE(rc) != ipc::kResultModule) {
    return false;
  }
  ipc::Error error = ipc::Error::kOk;
  if (!ipc::IsError(R_DESCRIPTION(rc), &error) || error == ipc::Error::kOk) {
    // Our module, and a description this build has no name for. That is a
    // sysmodule from a newer release rather than a refusal to invent, and
    // `ScreenFrame::Diagnose` is what has a sentence for it.
    return false;
  }
  *out = error;
  return true;
}

namespace {

/// The worker's stack.
///
/// Its whole call graph is `Run` -> `Dispatch` -> `serviceDispatchOut`, which
/// packs the request into the thread's TLS message buffer rather than onto the
/// stack, and none of it parses anything: the decode is the caller's, on the
/// draw thread. 16 KiB is several times what that needs, and it is a number
/// this file owns rather than the 128 KiB a null `pthread_attr_t` costs on
/// devkitA64 (`sysmodule/source/sized_thread.hpp` has the arithmetic) -- out of
/// an overlay heap that libultrahand sizes at 4 MiB on a console that has not
/// been given more.
constexpr std::size_t kWorkerStackBytes = 0x4000;
static_assert(kWorkerStackBytes % 0x1000 == 0,
              "a thread stack must be page-aligned or __syscall_thread_create returns EINVAL");

}  // namespace

IpcClient::IpcClient(std::chrono::milliseconds budget) : budget_(budget) {
  request_.reserve(ipc::kMaxPayloadBytes);
  response_.resize(ipc::kMaxPayloadBytes);
}

IpcClient::~IpcClient() { Shutdown(); }

Result IpcClient::Open() {
  if (serviceIsActive(&service_)) {
    return 0;
  }
  return smGetService(&service_, ipc::kServiceName);
}

void IpcClient::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (slot_ == Slot::kQueued || slot_ == Slot::kRunning) {
    return;
  }
  // A late answer nobody wanted goes with the session it arrived on.
  slot_ = Slot::kIdle;
  abandoned_ = false;
  serviceClose(&service_);
}

void IpcClient::Shutdown() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    // No budget, for the reason in the header: the worker cannot be left inside
    // a dispatch when the overlay is unloaded under it.
    done_.wait(lock, [this] { return slot_ != Slot::kQueued && slot_ != Slot::kRunning; });
    stopping_ = true;
  }
  work_.notify_all();
  if (started_) {
    ::pthread_join(thread_, nullptr);
    started_ = false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  // Usable again afterwards, which nothing on a console needs -- an overlay is
  // one `Shutdown` per process run -- and which costs three assignments.
  stopping_ = false;
  slot_ = Slot::kIdle;
  abandoned_ = false;
  serviceClose(&service_);
}

bool IpcClient::busy() {
  std::lock_guard<std::mutex> lock(mutex_);
  return abandoned_ && slot_ != Slot::kDone;
}

void* IpcClient::Enter(void* self) {
  static_cast<IpcClient*>(self)->Run();
  return nullptr;
}

bool IpcClient::StartWorkerLocked() {
  if (started_) {
    return true;
  }
  ::pthread_attr_t attributes;
  if (::pthread_attr_init(&attributes) != 0) {
    return false;
  }
  // A refused size falls back to the platform's default rather than to no
  // worker, which is `SizedThread::Start`'s rule and its reason: on Horizon the
  // `static_assert` above rules the refusal out, and a host -- glibc's floor is
  // 128 KiB on aarch64 -- is better served by a default stack than by a test
  // run with no bound in it.
  const bool sized = ::pthread_attr_setstacksize(&attributes, kWorkerStackBytes) == 0;
  bool ok = sized && ::pthread_create(&thread_, &attributes, &IpcClient::Enter, this) == 0;
  if (!sized) {
    ok = ::pthread_create(&thread_, nullptr, &IpcClient::Enter, this) == 0;
  }
  ::pthread_attr_destroy(&attributes);
  started_ = ok;
  return ok;
}

void IpcClient::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    work_.wait(lock, [this] { return stopping_ || slot_ == Slot::kQueued; });
    if (stopping_) {
      // `Shutdown` only sets this once nothing is queued or running, so there is
      // no call to abandon on the way out.
      return;
    }
    slot_ = Slot::kRunning;
    const ipc::Command command = command_;
    // Not held across the dispatch: that is the wait the caller must be able
    // to stop doing.
    lock.unlock();
    const Result result = Dispatch(command);
    lock.lock();
    result_ = result;
    slot_ = Slot::kDone;
    done_.notify_all();
  }
}

Result IpcClient::Call(ipc::Command command, std::string_view request) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (abandoned_) {
    if (slot_ != Slot::kDone) {
      // The last call is still out. Not sent: one session answers one request
      // at a time, and a second one queued behind a stuck first would only be
      // one more answer to drop.
      return NotResponding();
    }
    // It came back after its caller left. **Dropped**, never handed on: it is
    // an answer to a different command, and the decoder of this one might even
    // accept it.
    abandoned_ = false;
    slot_ = Slot::kIdle;
  }

  if (!serviceIsActive(&service_)) {
    response_.clear();
    return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
  }
  if (!ipc::Fits(request)) {
    // Refused here rather than sent: the far side would refuse it anyway, and a
    // request this build cannot express is a bug on this side.
    response_.clear();
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  }

  // Into the reserved buffer, so no allocation: `Fits` bounds it at the
  // capacity the constructor reserved.
  request_.assign(request.data(), request.size());
  command_ = command;

  if (!StartWorkerLocked()) {
    // No thread to carry it. The call still happens, unbounded, exactly as it
    // did before #209 -- an overlay that could not start a thread and so could
    // not talk to the sysmodule at all would be a worse failure than one that
    // can be held by a stuck one.
    lock.unlock();
    return Dispatch(command);
  }

  slot_ = Slot::kQueued;
  work_.notify_one();
  if (!done_.wait_for(lock, budget_, [this] { return slot_ == Slot::kDone; })) {
    // The bound. The call stays out and the worker stays on it; `response_` is
    // the worker's until it comes back, so nothing here reads it.
    abandoned_ = true;
    return NotResponding();
  }
  slot_ = Slot::kIdle;
  return result_;
}

Result IpcClient::Dispatch(ipc::Command command) {
  // Grown back to the full buffer and shrunk to the answer's length below, so
  // the allocation happens once in the constructor and never on a draw.
  response_.resize(ipc::kMaxPayloadBytes);

  u64 length = 0;
  const Result result = serviceDispatchOut(
      &service_, static_cast<u32>(command), length,
      .buffer_attrs =
          {
              SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
              SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
          },
      .buffers = {
          {request_.data(), request_.size()},
          {response_.data(), response_.size()},
      });
  if (R_FAILED(result)) {
    // Nothing readable came back, and that is a property of the wire rather
    // than a shortcut: libnx's `cmifParseResponse` returns on a failing result
    // before it exposes the reply's data words, so there is no length to trust
    // even when the far side wrote the buffer. It is exactly why a command
    // whose refusal has something to say answers it as an `ipc::WriteOutcome`
    // inside a successful reply -- see `ipc::WriteOutcome`.
    response_.clear();
    return result;
  }
  if (length > response_.size()) {
    response_.clear();
    return MalformedResponse();
  }
  response_.resize(static_cast<std::size_t>(length));
  return 0;
}

template <typename T, typename Decode>
Result IpcClient::CallAndDecode(ipc::Command command, std::string_view request, Decode decode,
                               T* out) {
  const Result result = Call(command, request);
  if (R_FAILED(result)) {
    return result;
  }
  const auto decoded = decode(std::string_view(response_));
  if (!decoded.ok()) {
    return MalformedResponse();
  }
  *out = decoded.value;
  return 0;
}

Result IpcClient::GetInterfaceVersion(std::uint32_t* out) {
  return CallAndDecode(ipc::Command::kGetInterfaceVersion, ipc::EncodeEmpty(),
                       ipc::DecodeInterfaceVersion, out);
}

Result IpcClient::GetStatus(ipc::Status* out) {
  return CallAndDecode(ipc::Command::kGetStatus, ipc::EncodeEmpty(), ipc::DecodeStatus, out);
}

Result IpcClient::GetConfig(ipc::ConfigView* out) {
  return CallAndDecode(ipc::Command::kGetConfig, ipc::EncodeEmpty(), ipc::DecodeConfigView, out);
}

Result IpcClient::SetConfig(const ipc::ConfigEdit& edit, ipc::ConfigResult* result) {
  return CallAndDecode(ipc::Command::kSetConfig, ipc::EncodeConfigEdit(edit),
                       ipc::DecodeConfigResult, result);
}

Result IpcClient::SetEnabled(bool enabled, ipc::EnabledResult* result) {
  return CallAndDecode(ipc::Command::kSetEnabled, ipc::EncodeEnabled(enabled),
                       ipc::DecodeEnabledResult, result);
}

Result IpcClient::SyncNow(ipc::SyncOutcome* outcome) {
  return CallAndDecode(ipc::Command::kSyncNow, ipc::EncodeEmpty(), ipc::DecodeSyncOutcome,
                       outcome);
}

Result IpcClient::StartPair(auth::PairingStatus* status) {
  return CallAndDecode(ipc::Command::kStartPair, ipc::EncodeEmpty(), auth::ParsePairingStatus,
                       status);
}

Result IpcClient::GetPairState(auth::PairingStatus* status) {
  return CallAndDecode(ipc::Command::kGetPairState, ipc::EncodeEmpty(), auth::ParsePairingStatus,
                       status);
}

Result IpcClient::Unpair() { return Call(ipc::Command::kUnpair, ipc::EncodeEmpty()); }

Result IpcClient::Enqueue(std::int64_t rom_id, std::int32_t* position) {
  return CallAndDecode(ipc::Command::kEnqueue, ipc::EncodeRomId(rom_id),
                       ipc::DecodeQueuePosition, position);
}

Result IpcClient::Dequeue(std::int64_t rom_id) {
  return Call(ipc::Command::kDequeue, ipc::EncodeRomId(rom_id));
}

Result IpcClient::ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor) {
  return CallAndDecode(ipc::Command::kListBegin, ipc::EncodeListRequest(request),
                       ipc::DecodeCursor, cursor);
}

Result IpcClient::ListNext(ipc::Cursor cursor, ipc::ListPage* page) {
  return CallAndDecode(ipc::Command::kListNext, ipc::EncodeCursor(cursor), ipc::DecodeListPage,
                       page);
}

Result IpcClient::ListEnd(ipc::Cursor cursor) {
  return Call(ipc::Command::kListEnd, ipc::EncodeCursor(cursor));
}

Result IpcClient::ListConflicts(const ipc::ConflictQuery& query, ipc::ConflictPage* page) {
  return CallAndDecode(ipc::Command::kListConflicts, ipc::EncodeConflictQuery(query),
                       ipc::DecodeConflictPage, page);
}

Result IpcClient::RestoreBackup(std::int64_t entry_id, conflicts::RestoreReport* report) {
  // Decoded rather than fire-and-forget: `RestoreBackup` never fails at the
  // transport, so the whole of what happened is in the answer (ipc.hpp).
  return CallAndDecode(ipc::Command::kRestoreBackup, ipc::EncodeEntryId(entry_id),
                       ipc::DecodeRestoreReport, report);
}

}  // namespace rommsync::overlay
