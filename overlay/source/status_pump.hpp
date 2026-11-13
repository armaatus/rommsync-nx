// The status screen's IPC: what `StatusScreen::update()` does, without the
// `tsl::Gui` around it (M11-15, #299).
//
// Split out of `status_screen.cpp` for `library_pump.cpp`'s reason: what is
// left in a `tsl::Gui` has never run, and "a busy sysmodule does not slow the
// overlay down" is a claim about exactly this code. `ctest -R overlay.frames`
// times its frames against a sysmodule that takes 300 ms to answer. It names no
// libultrahand type, which `overlay.portable` holds it to.
#pragma once

#include "card_probe.hpp"
#include "ipc_client.hpp"
#include "lifecycle.hpp"
#include "poller.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// The status screen's frame: the handshake, `GetStatus`, and the card when
/// nothing answered.
class StatusPump {
 public:
  /// `ProbeCard` on a console; a host test hands it a card of its own.
  using Probe = CardState (*)();

  explicit StatusPump(IpcClient& client, Probe probe = &ProbeCard);

  StatusPump(const StatusPump&) = delete;
  StatusPump& operator=(const StatusPump&) = delete;

  /// `update()`. Never waits on the sysmodule: the poll is `Poller`'s, about
  /// four times a second, and a frame only collects what came back.
  void Frame();

  const StatusView& view() const { return view_; }

 private:
  /// Draw `link` instead, with the card when it is the card that knows why.
  void Unreachable(Link link);

  /// What the card says about the two switches, re-reading it if this poll is
  /// the one due to.
  ///
  /// Not an accessor: it is the poll's own step, and it stats three files on the
  /// polls where `probe_cadence_` is due. Named for that -- `card()` beside
  /// `card_` would read as free.
  ///
  /// Only `Link::kNotRunning` uses the answer: `RenderUnreachable` discards the
  /// card for the other states, where a session that opened has already proved
  /// the module installed and running (`card_probe.hpp`).
  const CardState& CardThisPoll(Link link);

  IpcClient& client_;
  ScreenFrame frame_{client_};
  Poller poller_{frame_, client_, ipc::Command::kGetStatus};
  Probe probe_;

  /// What the last answer produced. Blank until the first: on a console that
  /// answers that is two frames, and on one that does not it is "not running"
  /// at once or "not responding" after the budget -- where a "not running"
  /// drawn ahead of the first answer would flash a wrong sentence on every
  /// open (#299).
  StatusView view_;

  /// The last look at the card, and when the next one is due. Due on the first
  /// poll that needs it, so that poll reads the card rather than drawing a
  /// default -- and on the first of every showing (`lifecycle.hpp`).
  CardState card_;
  Cadence probe_cadence_;
};

}  // namespace rommsync::overlay
