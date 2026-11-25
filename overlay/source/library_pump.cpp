#include "library_pump.hpp"

#include <cstdint>
#include <string>

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_library_model.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {
namespace {

/// The wire command a model command is.
ipc::Command WireCommand(LibraryBrowserModel::Command::Kind kind) {
  switch (kind) {
    case LibraryBrowserModel::Command::Kind::kListBegin:
      return ipc::Command::kListBegin;
    case LibraryBrowserModel::Command::Kind::kListNext:
      return ipc::Command::kListNext;
    case LibraryBrowserModel::Command::Kind::kListEnd:
      return ipc::Command::kListEnd;
    case LibraryBrowserModel::Command::Kind::kEnqueue:
    case LibraryBrowserModel::Command::Kind::kNone:
      break;
  }
  return ipc::Command::kEnqueue;
}

/// Its request payload.
std::string Request(const LibraryBrowserModel::Command& command) {
  switch (command.kind) {
    case LibraryBrowserModel::Command::Kind::kListBegin:
      return ipc::EncodeListRequest(command.request);
    case LibraryBrowserModel::Command::Kind::kListNext:
    case LibraryBrowserModel::Command::Kind::kListEnd:
      return ipc::EncodeCursor(command.cursor);
    case LibraryBrowserModel::Command::Kind::kEnqueue:
    case LibraryBrowserModel::Command::Kind::kNone:
      break;
  }
  return ipc::EncodeRomId(command.rom_id);
}

/// A failing `Result`, told to the model. A refusal the sysmodule named, or
/// the transport: telling them apart is the whole reason `DecodeError` exists
/// -- without it a rom that is already queued and a sysmodule that is not
/// running arrive here identically, and the screen would draw the second
/// sentence over the first (`ipc_client.hpp`). The link it leaves, or `kOk`
/// for a refusal.
Link Failed(ScreenFrame& frame, LibraryBrowserModel& model, Result rc) {
  ipc::Error error = ipc::Error::kOk;
  if (DecodeError(rc, &error)) {
    model.OnRefused(error);
    return Link::kOk;
  }
  const Link diagnosed = frame.Diagnose(rc);
  model.OnUnreachable(diagnosed, frame.sysmodule_interface());
  return diagnosed;
}

/// Send `command` and wait for it, within the budget. The teardown's, and
/// only the teardown's: a frame posts.
void SendWaiting(ScreenFrame& frame, IpcClient& client, LibraryBrowserModel& model,
                 const LibraryBrowserModel::Command& command) {
  Result rc = 0;
  switch (command.kind) {
    case LibraryBrowserModel::Command::Kind::kListEnd:
      rc = client.ListEnd(command.cursor);
      break;
    case LibraryBrowserModel::Command::Kind::kListBegin:
    case LibraryBrowserModel::Command::Kind::kListNext:
    case LibraryBrowserModel::Command::Kind::kEnqueue:
    case LibraryBrowserModel::Command::Kind::kNone:
      // A closed model hands out nothing else (`LibraryBrowserModel::Close`).
      return;
  }
  if (R_FAILED(rc)) {
    Failed(frame, model, rc);
    return;
  }
  model.OnEnded();
}

}  // namespace

LibraryPump::LibraryPump(IpcClient& client) : client_(client) {}

void LibraryPump::Frame() {
  Handle(poller_.Frame());
  if (SendNext()) {
    // Posted on this frame rather than the next, so an answer that is back by
    // the next frame is a page a frame sooner -- a held R outruns the pages
    // otherwise, and rests on "Loading..." for the difference.
    Handle(poller_.Frame());
  }
}

void LibraryPump::Handle(const Poller::Event& event) {
  switch (event.kind) {
    case Poller::Event::Kind::kNone:
    case Poller::Event::Kind::kRead:  // there is no read
      return;
    case Poller::Event::Kind::kUnreachable:
      Unreachable(event.link);
      return;
    case Poller::Event::Kind::kStillApplying:
      // Still out past the budget, or its answer lost to a call that waited
      // for the line. `sending()` is which: a lost one is no longer out.
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

bool LibraryPump::SendNext() {
  if (link_ != Link::kOk) {
    if (!poller_.linked() || !client_.open() || !frame_.checked()) {
      return false;
    }
    // The session came back. Everything loaded belonged to the one that went
    // away, so the browser starts again rather than asking the new session
    // about cursor numbers it never issued.
    link_ = Link::kOk;
    model_.OnLinkRestored(false);
  }
  // One command out at a time. The model answers `kNone` as soon as the loaded
  // pages cover what is on screen, so a browser sitting still costs nothing.
  if (poller_.sending()) {
    return false;
  }
  const LibraryBrowserModel::Command command = model_.Next();
  if (command.kind == LibraryBrowserModel::Command::Kind::kNone) {
    return false;
  }
  return poller_.Send(WireCommand(command.kind), Request(command));
}

void LibraryPump::Answered(ipc::Command command, Result rc) {
  if (R_FAILED(rc)) {
    const Link link = Failed(frame_, model_, rc);
    if (link != Link::kOk) {
      link_ = link;
    }
    return;
  }
  Result decoded = 0;
  switch (command) {
    case ipc::Command::kListBegin: {
      ipc::Cursor cursor = 0;
      decoded = client_.Decode(ipc::DecodeCursor, &cursor);
      if (R_SUCCEEDED(decoded)) {
        model_.OnCursor(cursor);
      }
      break;
    }
    case ipc::Command::kListNext: {
      ipc::ListPage page;
      decoded = client_.Decode(ipc::DecodeListPage, &page);
      if (R_SUCCEEDED(decoded)) {
        model_.OnPage(page);
      }
      break;
    }
    case ipc::Command::kListEnd:
      model_.OnEnded();
      break;
    case ipc::Command::kEnqueue: {
      std::int32_t position = 0;
      decoded = client_.Decode(ipc::DecodeQueuePosition, &position);
      if (R_SUCCEEDED(decoded)) {
        model_.OnEnqueued(position);
      }
      break;
    }
    default:
      return;
  }
  if (R_FAILED(decoded)) {
    // An answer this build cannot read: `Diagnose`'s `kUnreadable`.
    Unreachable(frame_.Diagnose(decoded));
  }
}

void LibraryPump::Unreachable(Link link) {
  model_.OnUnreachable(link, frame_.sysmodule_interface());
  link_ = link;
}

void LibraryPump::Close() {
  model_.Close();
  // Retired first: this runs from `~LibraryScreen`, which libtesla may run
  // after `exitServices` has stopped the client, and a session opened then is
  // one nothing closes (#285). Every cursor belongs to the session that is
  // open, so a session that is not open has none left to close anyway.
  frame_.Retire();
  // The call that is out, waited for within the budget: a `ListBegin` answered
  // now is a cursor the closed model queues for `ListEnd` (#329), and one left
  // out is a cursor only the sysmodule's TTL gets back.
  const Poller::Event event = poller_.Drain();
  if (event.kind == Poller::Event::Kind::kSent) {
    Answered(event.command, event.rc);
  }
  // The handshake, as before any other command (`overlay/AGENTS.md`). A
  // session that has gone has no cursors left to close and nothing to say
  // about it, so this is where the teardown stops.
  if (frame_.Ready() != Link::kOk) {
    return;
  }
  // This terminates, and it is worth saying why rather than capping it: after
  // `Close()` the only command the model will hand out is a `ListEnd` for a
  // cursor it holds, and every answer retires one -- `OnEnded` and a refused
  // `ListEnd` both drop it, and a transport failure puts the model behind a
  // link that is not `kOk`, which answers `kNone`. So each turn of this loop
  // either closes a cursor or ends the loop, and there are at most a few.
  // A count here would be worse than none: it is the kind that gets read in the
  // condition while each answer shrinks it, and stops one cursor short.
  for (LibraryBrowserModel::Command command = model_.Next();
       command.kind != LibraryBrowserModel::Command::Kind::kNone; command = model_.Next()) {
    SendWaiting(frame_, client_, model_, command);
  }
}

bool RenderLibrary(const LibraryBrowserModel& model, LibraryView* view, std::uint64_t* seen) {
  // A browser sitting still costs nothing: not a command, and not a copy of the
  // rows it already drew (#298).
  if (model.revision() == *seen) {
    return false;
  }
  *seen = model.revision();
  *view = model.Render(kViewRowsAbove, kViewRowsBelow);
  return true;
}

}  // namespace rommsync::overlay
