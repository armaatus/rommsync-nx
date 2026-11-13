#include "library_pump.hpp"

#include <cstdint>

#include "ipc_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_library_model.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {
namespace {

/// Send `command`, and report its answer to the model. Every arm ends the same
/// way -- a `Result` that failed is either a refusal the sysmodule named or the
/// transport, and only `DecodeError` can tell those apart.
void Send(ScreenFrame& frame, IpcClient& client, LibraryBrowserModel& model, Link& link,
          const LibraryBrowserModel::Command& command) {
  Result rc = 0;
  ipc::Cursor cursor = 0;
  ipc::ListPage page;
  std::int32_t position = 0;

  switch (command.kind) {
    case LibraryBrowserModel::Command::Kind::kListBegin:
      rc = client.ListBegin(command.request, &cursor);
      break;
    case LibraryBrowserModel::Command::Kind::kListNext:
      rc = client.ListNext(command.cursor, &page);
      break;
    case LibraryBrowserModel::Command::Kind::kListEnd:
      rc = client.ListEnd(command.cursor);
      break;
    case LibraryBrowserModel::Command::Kind::kEnqueue:
      rc = client.Enqueue(command.rom_id, &position);
      break;
    case LibraryBrowserModel::Command::Kind::kNone:
      return;
  }

  if (R_FAILED(rc)) {
    // A refusal the sysmodule named, or the transport. Telling them apart is
    // the whole reason `DecodeError` exists: without it a rom that is already
    // queued and a sysmodule that is not running arrive here identically, and
    // the screen would draw the second sentence over the first
    // (`ipc_client.hpp`).
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
    case LibraryBrowserModel::Command::Kind::kListBegin:
      model.OnCursor(cursor);
      return;
    case LibraryBrowserModel::Command::Kind::kListNext:
      model.OnPage(page);
      return;
    case LibraryBrowserModel::Command::Kind::kListEnd:
      model.OnEnded();
      return;
    case LibraryBrowserModel::Command::Kind::kEnqueue:
      model.OnEnqueued(position);
      return;
    case LibraryBrowserModel::Command::Kind::kNone:
      return;
  }
}

}  // namespace

void PumpLibrary(ScreenFrame& frame, IpcClient& client, LibraryBrowserModel& model, Link& link) {
  // The port and the version handshake, both of which every screen needs and
  // none of which is this screen's own (`screen_frame.hpp`).
  const Link ready = frame.Ready();
  if (ready != Link::kOk) {
    model.OnUnreachable(ready, frame.sysmodule_interface());
    link = ready;
    return;
  }
  if (link != Link::kOk) {
    // The session came back. Everything loaded belonged to the one that went
    // away, so the browser starts again rather than asking the new session
    // about cursor numbers it never issued.
    link = Link::kOk;
    model.OnLinkRestored();
  }

  // One command per frame. The model answers `kNone` as soon as the loaded
  // pages cover what is on screen, so a browser sitting still costs nothing.
  const LibraryBrowserModel::Command command = model.Next();
  if (command.kind != LibraryBrowserModel::Command::Kind::kNone) {
    Send(frame, client, model, link, command);
  }
}

void CloseLibrary(ScreenFrame& frame, IpcClient& client, LibraryBrowserModel& model) {
  model.Close();
  // The port and the handshake, as before any other command
  // (`overlay/AGENTS.md`). A session that has gone has no cursors left to close
  // and nothing to say about it, so this is where the teardown stops.
  if (frame.Ready() != Link::kOk) {
    return;
  }
  // This terminates, and it is worth saying why rather than capping it: after
  // `Close()` the only command the model will hand out is a `ListEnd` for a
  // cursor it holds, and every answer retires one -- `OnEnded` and a refused
  // `ListEnd` both drop it, and a transport failure puts the model behind a
  // link that is not `kOk`, which answers `kNone`. So each turn of this loop
  // either closes a cursor or ends the loop, and there are at most three.
  // A count here would be worse than none: it is the kind that gets read in the
  // condition while each answer shrinks it, and stops one cursor short.
  Link link = Link::kOk;
  for (LibraryBrowserModel::Command command = model.Next();
       command.kind != LibraryBrowserModel::Command::Kind::kNone; command = model.Next()) {
    Send(frame, client, model, link, command);
  }
}

}  // namespace rommsync::overlay
