#include "ipc_client.hpp"

#include <switch.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

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

IpcClient::IpcClient() : buffer_(ipc::kMaxPayloadBytes) {}

IpcClient::~IpcClient() { Close(); }

Result IpcClient::Open() {
  if (serviceIsActive(&service_)) {
    return 0;
  }
  return smGetService(&service_, ipc::kServiceName);
}

void IpcClient::Close() { serviceClose(&service_); }

Result IpcClient::Call(ipc::Command command, std::string_view request, std::string* response) {
  response->clear();
  if (!serviceIsActive(&service_)) {
    return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
  }
  if (!ipc::Fits(request)) {
    // Refused here rather than sent: the far side would refuse it anyway, and a
    // request this build cannot express is a bug on this side.
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
          {buffer_.data(), buffer_.size()},
      });
  // A failing command may still have answered -- `SetConfig`'s refusal is its
  // diagnostics -- but libnx reports the failure before the reply's data words
  // are readable, so there is no length to trust. The caller gets the `Result`.
  if (R_FAILED(result)) {
    return result;
  }
  if (length > buffer_.size()) {
    return MalformedResponse();
  }
  response->assign(buffer_.data(), static_cast<std::size_t>(length));
  return 0;
}

Result IpcClient::GetInterfaceVersion(std::uint32_t* out) {
  std::string response;
  const Result result = Call(ipc::Command::kGetInterfaceVersion, ipc::EncodeEmpty(), &response);
  if (R_FAILED(result)) {
    return result;
  }
  const ipc::Decoded<std::uint32_t> decoded = ipc::DecodeInterfaceVersion(response);
  if (!decoded.ok()) {
    return MalformedResponse();
  }
  *out = decoded.value;
  return 0;
}

Result IpcClient::GetStatus(ipc::Status* out) {
  std::string response;
  const Result result = Call(ipc::Command::kGetStatus, ipc::EncodeEmpty(), &response);
  if (R_FAILED(result)) {
    return result;
  }
  const ipc::Decoded<ipc::Status> decoded = ipc::DecodeStatus(response);
  if (!decoded.ok()) {
    return MalformedResponse();
  }
  *out = decoded.value;
  return 0;
}

Result IpcClient::GetConfig(ipc::ConfigView* out) {
  std::string response;
  const Result result = Call(ipc::Command::kGetConfig, ipc::EncodeEmpty(), &response);
  if (R_FAILED(result)) {
    return result;
  }
  const ipc::Decoded<ipc::ConfigView> decoded = ipc::DecodeConfigView(response);
  if (!decoded.ok()) {
    return MalformedResponse();
  }
  *out = decoded.value;
  return 0;
}

Result IpcClient::SetConfig(const ipc::ConfigEdit& edit,
                            std::vector<config::Diagnostic>* diagnostics) {
  diagnostics->clear();
  std::string response;
  const Result result = Call(ipc::Command::kSetConfig, ipc::EncodeConfigEdit(edit), &response);
  const ipc::Decoded<std::vector<config::Diagnostic>> decoded = ipc::DecodeDiagnostics(response);
  if (decoded.ok()) {
    *diagnostics = decoded.value;
  }
  return result;
}

Result IpcClient::SetEnabled(bool enabled, bool* effective) {
  *effective = enabled;
  std::string response;
  const Result result = Call(ipc::Command::kSetEnabled, ipc::EncodeEnabled(enabled), &response);
  const ipc::Decoded<bool> decoded = ipc::DecodeEnabled(response);
  if (decoded.ok()) {
    // The state that took, which is what the switch is drawn as -- see #24.
    *effective = decoded.value;
  }
  return result;
}

Result IpcClient::SyncNow(ipc::SyncOutcome* outcome) {
  std::string response;
  const Result result = Call(ipc::Command::kSyncNow, ipc::EncodeEmpty(), &response);
  if (R_FAILED(result)) {
    return result;
  }
  const ipc::Decoded<ipc::SyncOutcome> decoded = ipc::DecodeSyncOutcome(response);
  if (!decoded.ok()) {
    return MalformedResponse();
  }
  *outcome = decoded.value;
  return 0;
}

Result IpcClient::StartPair(auth::PairingStatus* status) {
  std::string response;
  const Result result = Call(ipc::Command::kStartPair, ipc::EncodeEmpty(), &response);
  if (R_FAILED(result)) {
    return result;
  }
  const auth::Parsed<auth::PairingStatus> decoded = auth::ParsePairingStatus(response);
  if (!decoded.ok()) {
    return MalformedResponse();
  }
  *status = decoded.value;
  return 0;
}

Result IpcClient::GetPairState(auth::PairingStatus* status) {
  std::string response;
  const Result result = Call(ipc::Command::kGetPairState, ipc::EncodeEmpty(), &response);
  if (R_FAILED(result)) {
    return result;
  }
  const auth::Parsed<auth::PairingStatus> decoded = auth::ParsePairingStatus(response);
  if (!decoded.ok()) {
    return MalformedResponse();
  }
  *status = decoded.value;
  return 0;
}

Result IpcClient::Unpair() {
  std::string response;
  return Call(ipc::Command::kUnpair, ipc::EncodeEmpty(), &response);
}

Result IpcClient::Enqueue(std::int64_t rom_id, std::int32_t* position) {
  std::string response;
  const Result result = Call(ipc::Command::kEnqueue, ipc::EncodeRomId(rom_id), &response);
  if (R_FAILED(result)) {
    return result;
  }
  const ipc::Decoded<std::int32_t> decoded = ipc::DecodeQueuePosition(response);
  if (!decoded.ok()) {
    return MalformedResponse();
  }
  *position = decoded.value;
  return 0;
}

Result IpcClient::Dequeue(std::int64_t rom_id) {
  std::string response;
  return Call(ipc::Command::kDequeue, ipc::EncodeRomId(rom_id), &response);
}

Result IpcClient::ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor) {
  std::string response;
  const Result result = Call(ipc::Command::kListBegin, ipc::EncodeListRequest(request), &response);
  if (R_FAILED(result)) {
    return result;
  }
  const ipc::Decoded<ipc::Cursor> decoded = ipc::DecodeCursor(response);
  if (!decoded.ok()) {
    return MalformedResponse();
  }
  *cursor = decoded.value;
  return 0;
}

Result IpcClient::ListNext(ipc::Cursor cursor, ipc::ListPage* page) {
  std::string response;
  const Result result = Call(ipc::Command::kListNext, ipc::EncodeCursor(cursor), &response);
  if (R_FAILED(result)) {
    return result;
  }
  const ipc::Decoded<ipc::ListPage> decoded = ipc::DecodeListPage(response);
  if (!decoded.ok()) {
    return MalformedResponse();
  }
  *page = decoded.value;
  return 0;
}

Result IpcClient::ListEnd(ipc::Cursor cursor) {
  std::string response;
  return Call(ipc::Command::kListEnd, ipc::EncodeCursor(cursor), &response);
}

}  // namespace rommsync::overlay
