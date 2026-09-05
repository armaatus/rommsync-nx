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
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"

namespace rommsync::overlay {

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

  /// `diagnostics` is filled whether the edit was applied or refused -- on a
  /// refusal it is the whole answer.
  Result SetConfig(const ipc::ConfigEdit& edit, std::vector<config::Diagnostic>* diagnostics);

  /// `effective` is the state as it stands afterwards, which is what the switch
  /// should be drawn as -- not the state that was asked for. Filled on a failed
  /// write too.
  Result SetEnabled(bool enabled, bool* effective);

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
  /// One command: a request payload in, a response payload out.
  ///
  /// The response buffer is a member rather than a local: `kMaxPayloadBytes` is
  /// 8 KiB and an overlay's draw thread does not have that to spare on the
  /// stack, and allocating one per call would be an allocation per frame on the
  /// status screen.
  Result Call(ipc::Command command, std::string_view request, std::string* response);

  Service service_{};
  std::vector<char> buffer_;
};

}  // namespace rommsync::overlay
