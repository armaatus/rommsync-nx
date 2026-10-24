#include "conflicts_pump.hpp"

#include "ipc_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_conflicts_view.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {
namespace {

/// Send `command`, and report its answer to the model.
void Send(ScreenFrame& frame, IpcClient& client, ConflictsModel& model, Link& link,
          const ConflictsModel::Command& command) {
  Result rc = 0;
  ipc::ConflictPage page;
  ipc::RestoreState restore;
  ipc::CancelKeepLocalResult cancel;

  switch (command.kind) {
    case ConflictsModel::Command::Kind::kListConflicts:
      rc = client.ListConflicts(command.query, &page);
      break;
    case ConflictsModel::Command::Kind::kRestore:
      // Answered the moment the sysmodule has taken it; the copy is its
      // worker's, and the model polls for the outcome (#209).
      rc = client.RestoreBackup(command.entry_id, &restore);
      break;
    case ConflictsModel::Command::Kind::kKeepLocal:
      // A restore asked for as Keep local (#249), polled the same way.
      rc = client.KeepLocal(command.entry_id, &restore);
      break;
    case ConflictsModel::Command::Kind::kRestoreState:
      rc = client.GetRestoreState(&restore);
      break;
    case ConflictsModel::Command::Kind::kCancelKeepLocal:
      // A held Keep local withdrawn (#279), answered at once.
      rc = client.CancelKeepLocal(command.entry_id, &cancel);
      break;
    case ConflictsModel::Command::Kind::kNone:
      return;
  }

  if (R_FAILED(rc)) {
    // A refusal the sysmodule named, or the transport. Telling them apart is
    // the whole reason `DecodeError` exists (`ipc_client.hpp`): without it, a
    // sysmodule that is not running and a payload this build cannot read draw
    // the same sentence over an entry that is perfectly fine.
    ipc::Error error = ipc::Error::kOk;
    if (DecodeError(rc, &error)) {
      model.OnRefused(error);
      return;
    }
    const Link diagnosed = frame.Diagnose(rc);
    model.OnUnreachable(diagnosed, frame.sysmodule_interface());
    link = diagnosed;
    return;
  }

  switch (command.kind) {
    case ConflictsModel::Command::Kind::kListConflicts:
      model.OnPage(page);
      return;
    case ConflictsModel::Command::Kind::kRestore:
    case ConflictsModel::Command::Kind::kRestoreState:
    case ConflictsModel::Command::Kind::kKeepLocal:
      model.OnRestoreState(restore);
      return;
    case ConflictsModel::Command::Kind::kCancelKeepLocal:
      model.OnCancelKeepLocal(cancel);
      return;
    case ConflictsModel::Command::Kind::kNone:
      return;
  }
}

}  // namespace

void PumpConflicts(ScreenFrame& frame, IpcClient& client, ConflictsModel& model, Link& link) {
  // The port and the version handshake, both of which every screen needs and
  // none of which is this screen's own (`screen_frame.hpp`).
  const Link ready = frame.Ready();
  if (ready != Link::kOk) {
    model.OnUnreachable(ready, frame.sysmodule_interface());
    link = ready;
    return;
  }
  if (link != Link::kOk) {
    link = Link::kOk;
    model.OnLinkRestored();
  }

  const ConflictsModel::Command command = model.Next();
  if (command.kind != ConflictsModel::Command::Kind::kNone) {
    Send(frame, client, model, link, command);
  }
}

}  // namespace rommsync::overlay
