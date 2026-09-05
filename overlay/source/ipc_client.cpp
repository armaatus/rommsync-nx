#include "ipc_client.hpp"

#include <switch.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"

namespace rommsync::overlay {
namespace {

/// A response the sysmodule answered but this build cannot read.
///
/// Distinct from the sysmodule's own errors on purpose: those say what the
/// *command* could not do, and this says the two halves disagree about the
/// payload -- which is a different sentence for the user ("update the overlay")
/// and is what `GetInterfaceVersion` exists to diagnose.
constexpr Result MalformedResponse() {
  return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
}

}  // namespace

IpcClient::IpcClient() { response_.resize(ipc::kMaxPayloadBytes); }

IpcClient::~IpcClient() { Close(); }

Result IpcClient::Open() {
  if (serviceIsActive(&service_)) {
    return 0;
  }
  return smGetService(&service_, ipc::kServiceName);
}

void IpcClient::Close() { serviceClose(&service_); }

Result IpcClient::Call(ipc::Command command, std::string_view request) {
  // Grown back to the full buffer and shrunk to the answer's length below, so
  // the allocation happens once in the constructor and never on a draw.
  response_.resize(ipc::kMaxPayloadBytes);
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

  u64 length = 0;
  const Result result = serviceDispatchOut(
      &service_, static_cast<u32>(command), length,
      .buffer_attrs =
          {
              SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
              SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
          },
      .buffers = {
          {request.data(), request.size()},
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

}  // namespace rommsync::overlay
