#include "service.hpp"

#include <switch.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "rommsync/ipc.hpp"

namespace rommsync::sysmodule {
namespace {

/// What the reply's data words carry beyond the `cmif` header: how many bytes of
/// the Out buffer the response actually filled. The client cannot infer it --
/// the buffer it supplied is a capacity, not a length.
struct ReplyPayload {
  u64 response_length;
};

/// Lay out a `cmif` reply in `message` and return where its payload goes.
///
/// Mirrors what libnx's own `cmifParseResponse` expects to read, which is what
/// the overlay's client uses: aligned data start, a `CmifOutHeader` carrying the
/// magic and the result, then the payload.
ReplyPayload* MakeReply(void* message, Result result) {
  const u32 size = 16 + static_cast<u32>(sizeof(CmifOutHeader) + sizeof(ReplyPayload));
  const HipcRequest hipc = hipcMakeRequestInline(message, .num_data_words = (size + 3) / 4);
  auto* header = static_cast<CmifOutHeader*>(cmifGetAlignedDataStart(hipc.data_words, message));
  *header = CmifOutHeader{CMIF_OUT_HEADER_MAGIC, 0, result, 0};
  auto* payload = reinterpret_cast<ReplyPayload*>(header + 1);
  *payload = ReplyPayload{0};
  return payload;
}

}  // namespace

Result ToResult(ipc::Error error) {
  if (error == ipc::Error::kOk) {
    return 0;
  }
  return MAKERESULT(kResultModule, static_cast<u32>(error));
}

Result HandleRequest(ipc::ServiceCore& core, void* message) {
  const HipcParsedRequest parsed = hipcParseRequest(message);
  if (parsed.meta.type != CmifCommandType_Request) {
    // Domains, control commands and the legacy encodings are not part of this
    // contract: it hosts one object with no sub-objects, so there is nothing for
    // them to address. Reported under this service's own module rather than
    // libnx's, so every failure a caller can get here reads off `ipc::Error`.
    MakeReply(message, ToResult(ipc::Error::kMalformedRequest));
    return 0;
  }

  const auto* in_header =
      static_cast<const CmifInHeader*>(cmifGetAlignedDataStart(parsed.data.data_words, message));
  if (in_header->magic != CMIF_IN_HEADER_MAGIC) {
    MakeReply(message, ToResult(ipc::Error::kMalformedRequest));
    return 0;
  }
  const u32 command_id = in_header->command_id;

  // Both descriptors are read before anything is written, because they live in
  // `message` and `MakeReply` overwrites it -- reading them afterwards would be
  // reading whatever the reply header happened to leave there.
  const void* in_address = nullptr;
  size_t in_size = 0;
  if (parsed.meta.num_send_buffers >= 1) {
    in_address = hipcGetBufferAddress(&parsed.data.send_buffers[0]);
    in_size = hipcGetBufferSize(&parsed.data.send_buffers[0]);
  }
  void* out_address = nullptr;
  size_t out_capacity = 0;
  if (parsed.meta.num_recv_buffers >= 1) {
    out_address = hipcGetBufferAddress(&parsed.data.recv_buffers[0]);
    out_capacity = hipcGetBufferSize(&parsed.data.recv_buffers[0]);
  }

  // A command that takes nothing sends no buffer, which is read as `{}` -- the
  // same thing an empty object means (docs/DEVELOPMENT.md#ipc).
  std::string_view request("{}");
  if (in_address != nullptr) {
    // Bounded before it is read: the length is written by another process. The
    // decoder would refuse anything past the cap anyway; refusing it here means
    // the refusal costs no copy.
    if (in_size > ipc::kMaxPayloadBytes) {
      MakeReply(message, ToResult(ipc::Error::kMalformedRequest));
      return 0;
    }
    request = std::string_view(static_cast<const char*>(in_address), in_size);
  }

  std::string response;
  ipc::Error error = ipc::Dispatch(core, command_id, request, &response);

  if (!response.empty()) {
    if (out_address == nullptr) {
      // A command that answers, called with nowhere to put the answer.
      error = ipc::Error::kMalformedRequest;
      response.clear();
    } else if (out_capacity < response.size()) {
      // Never a partial answer: half a JSON object is not a shorter answer, it
      // is a payload the other side cannot parse and might not notice was cut.
      error = ipc::Error::kTooLarge;
      response.clear();
    }
  }

  ReplyPayload* payload = MakeReply(message, ToResult(error));
  if (!response.empty()) {
    std::memcpy(out_address, response.data(), response.size());
    payload->response_length = response.size();
  }
  return 0;
}

}  // namespace rommsync::sysmodule
