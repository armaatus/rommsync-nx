// The overlay's half of the IPC contract.
//
// `ovl-rommsync` is a thin client (overlay/AGENTS.md): it renders what the
// sysmodule reports and asks it to do things. This is the only place it talks
// to `sys-rommsync`, and every payload it sends or reads goes through the *same*
// codecs the sysmodule uses (`rommsync/ipc.hpp`) -- one copy of the field names,
// so the two halves cannot disagree about one.
//
// That is why this target compiles `core/` at all. It links no engine: the
// screens call nothing but the methods below, and `--gc-sections` drops what is
// unreferenced. The rule that matters is unchanged -- no sync, download or auth
// logic lives here.
//
// Nothing here blocks on the network, because nothing on the other side does
// either (docs/DEVELOPMENT.md#ipc). `SyncNow` and `StartPair` return as soon as
// the sysmodule has taken the work; a screen polls `GetStatus` or
// `GetPairState` while it runs.
//
// Nothing here has been run: overlay UI and the Horizon IPC path are verified
// after the M8-1 gate (overlay/AGENTS.md). What is checked today is that it
// cross-compiles and that the payloads it speaks round-trip natively
// (`ctest -R ipc`).
#pragma once

#include <switch.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"

namespace rommsync::overlay {

/// The `Result` every typed method below answers when the sysmodule replied
/// with a payload this build cannot read.
///
/// Distinct from the sysmodule's own errors on purpose: those say what the
/// *command* could not do, and this says the two halves disagree about the
/// payload -- which is a different sentence for the user ("update the overlay")
/// and is what `GetInterfaceVersion` exists to diagnose. Named here rather than
/// kept private because a screen has to tell it from a transport failure to
/// pick between "not running" and "unreachable" (`status_screen.cpp`).
constexpr Result MalformedResponse() {
  return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
}

/// A session on `sys-rommsync`.
///
/// `Open` assumes `smInitialize()` has already been called -- Tesla does it
/// before an overlay draws, and an overlay that initialised sm itself would be
/// fighting its host for the session.
///
/// Not thread-safe: one session answers one request at a time, and an overlay
/// draws on one thread.
class IpcClient {
 public:
  IpcClient();
  ~IpcClient();

  IpcClient(const IpcClient&) = delete;
  IpcClient& operator=(const IpcClient&) = delete;

  /// `smGetService("rommsync")`. `MAKERESULT(Module_Libnx, LibnxError_NotFound)`
  /// when the sysmodule is not running, which is the case an overlay has to be
  /// able to say something about: it is what a user who forgot to enable it
  /// sees.
  Result Open();
  void Close();
  bool open() { return serviceIsActive(&service_); }

  /// The contract the *sysmodule* speaks. Compare it with `ipc::kVersion`: a
  /// mismatch means one half needs updating, and this is the only call that is
  /// safe to make before knowing (its encoding is frozen).
  Result GetInterfaceVersion(std::uint32_t* out);

  Result GetStatus(ipc::Status* out);
  Result GetConfig(ipc::ConfigView* out);

  /// The answer says what happened (`result->outcome`) and why
  /// (`result->diagnostics`) -- **both on a refusal**, where the diagnostics are
  /// the entire answer. A failing `Result` here means the call did not happen;
  /// a refused edit is a successful call with `kInvalid` in it. See
  /// `ipc::WriteOutcome` for why it has to be that way round.
  Result SetConfig(const ipc::ConfigEdit& edit, ipc::ConfigResult* result);

  /// Same shape. `result->enabled` is the state as it stands afterwards, which
  /// is what the switch should be drawn as -- not the state that was asked for,
  /// and not lost when the write failed.
  Result SetEnabled(bool enabled, ipc::EnabledResult* result);

  Result SyncNow(ipc::SyncOutcome* outcome);
  Result StartPair(auth::PairingStatus* status);
  Result GetPairState(auth::PairingStatus* status);
  Result Unpair();

  Result Enqueue(std::int64_t rom_id, std::int32_t* position);
  Result Dequeue(std::int64_t rom_id);

  Result ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor);
  Result ListNext(ipc::Cursor cursor, ipc::ListPage* page);
  Result ListEnd(ipc::Cursor cursor);

 private:
  /// One command: a request payload in, `response_` out.
  ///
  /// The buffer is a member rather than a local, and is reused across calls:
  /// `kMaxPayloadBytes` is 8 KiB, an overlay's draw thread does not have that to
  /// spare on the stack, and the status screen polls every frame -- so an
  /// allocation per call would be an allocation per frame.
  Result Call(ipc::Command command, std::string_view request);

  /// The shape every typed method above is: send, decode `response_`, and turn a
  /// payload this build cannot read into one named `Result` rather than into a
  /// half-filled struct. Written once because there are thirteen of them, and
  /// because a fix applied to twelve is the bug this whole header exists to
  /// prevent.
  template <typename T, typename Decode>
  Result CallAndDecode(ipc::Command command, std::string_view request, Decode decode, T* out);

  Service service_{};
  std::string response_;
};

}  // namespace rommsync::overlay
