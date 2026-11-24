#include "conflicts_pump.hpp"

#include <string>

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_conflicts_view.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {
namespace {

using Kind = ConflictsModel::Command::Kind;

/// The wire command a model command is.
ipc::Command WireCommand(Kind kind) {
  switch (kind) {
    case Kind::kListConflicts:
      return ipc::Command::kListConflicts;
    case Kind::kRestore:
      // Answered the moment the sysmodule has taken it; the copy is its
      // worker's, and the model polls for the outcome (#209).
      return ipc::Command::kRestoreBackup;
    case Kind::kKeepLocal:
      // A restore asked for as Keep local (#249), polled the same way.
      return ipc::Command::kKeepLocal;
    case Kind::kRestoreState:
      return ipc::Command::kGetRestoreState;
    case Kind::kCancelKeepLocal:
      // A held Keep local withdrawn (#279), answered at once.
      return ipc::Command::kCancelKeepLocal;
    case Kind::kNone:
      break;
  }
  return ipc::Command::kGetRestoreState;
}

/// Its request payload.
std::string Request(const ConflictsModel::Command& command) {
  switch (command.kind) {
    case Kind::kListConflicts:
      return ipc::EncodeConflictQuery(command.query);
    case Kind::kRestore:
    case Kind::kKeepLocal:
    case Kind::kCancelKeepLocal:
      return ipc::EncodeEntryId(command.entry_id);
    case Kind::kRestoreState:
    case Kind::kNone:
      break;
  }
  return ipc::EncodeEmpty();
}

}  // namespace

ConflictsPump::ConflictsPump(IpcClient& client) : client_(client) {}

void ConflictsPump::Frame() {
  Handle(poller_.Frame());
  if (SendNext()) {
    // Posted on this frame rather than the next: `library_pump.cpp`'s reason.
    Handle(poller_.Frame());
  }
}

void ConflictsPump::Handle(const Poller::Event& event) {
  switch (event.kind) {
    case Poller::Event::Kind::kNone:
    case Poller::Event::Kind::kRead:  // there is no read
      return;
    case Poller::Event::Kind::kUnreachable:
      Unreachable(event.link);
      return;
    case Poller::Event::Kind::kStillApplying:
      // Out past the budget, or its answer lost to a call that waited for the
      // line; `sending()` is which.
      if (poller_.sending()) {
        model_.OnApplying();
      } else {
        model_.OnLost();
      }
      return;
    case Poller::Event::Kind::kSent:
      Answered(event.command, event.rc);
      return;
  }
}

bool ConflictsPump::SendNext() {
  if (link_ != Link::kOk) {
    if (!poller_.linked() || !client_.open() || !frame_.checked()) {
      return false;
    }
    link_ = Link::kOk;
    model_.OnLinkRestored();
  }
  if (poller_.sending()) {
    return false;
  }
  const ConflictsModel::Command command = model_.Next();
  if (command.kind == Kind::kNone) {
    return false;
  }
  return poller_.Send(WireCommand(command.kind), Request(command));
}

void ConflictsPump::Answered(ipc::Command command, Result rc) {
  if (R_FAILED(rc)) {
    // A refusal the sysmodule named, or the transport. Telling them apart is
    // the whole reason `DecodeError` exists (`ipc_client.hpp`): without it, a
    // sysmodule that is not running and a payload this build cannot read draw
    // the same sentence over an entry that is perfectly fine.
    ipc::Error error = ipc::Error::kOk;
    if (DecodeError(rc, &error)) {
      model_.OnRefused(error);
      return;
    }
    Unreachable(frame_.Diagnose(rc));
    return;
  }
  Result decoded = 0;
  switch (command) {
    case ipc::Command::kListConflicts: {
      ipc::ConflictPage page;
      decoded = client_.Decode(ipc::DecodeConflictPage, &page);
      if (R_SUCCEEDED(decoded)) {
        model_.OnPage(page);
      }
      break;
    }
    case ipc::Command::kRestoreBackup:
    case ipc::Command::kKeepLocal:
    case ipc::Command::kGetRestoreState: {
      ipc::RestoreState state;
      decoded = client_.Decode(ipc::DecodeRestoreState, &state);
      if (R_SUCCEEDED(decoded)) {
        model_.OnRestoreState(state);
      }
      break;
    }
    case ipc::Command::kCancelKeepLocal: {
      ipc::CancelKeepLocalResult cancel;
      decoded = client_.Decode(ipc::DecodeCancelKeepLocalResult, &cancel);
      if (R_SUCCEEDED(decoded)) {
        model_.OnCancelKeepLocal(cancel);
      }
      break;
    }
    default:
      return;
  }
  if (R_FAILED(decoded)) {
    Unreachable(frame_.Diagnose(decoded));
  }
}

void ConflictsPump::Unreachable(Link link) {
  model_.OnUnreachable(link, frame_.sysmodule_interface());
  link_ = link;
}

}  // namespace rommsync::overlay
