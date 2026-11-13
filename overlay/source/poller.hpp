// A screen's line to the sysmodule that never waits for it (M11-15, #299).
//
// The status, sync and pairing screens used to send a command from every
// `update()` and wait for the answer -- up to `IpcClient::kCallBudget`, every
// frame. A sysmodule answering in 300 ms, which is `SetConfig` under the card
// mutex while a download writes, was an overlay at three frames a second.
//
// A `Poller` posts instead (`IpcClient::Post`) and collects the answer on a
// later frame, so a frame costs a mutex and a decode whatever the far side is
// doing. It reads on a `Cadence` -- about four times a second -- and sends a
// screen's one-off command (a press) ahead of the next read. What came back is
// handed to the screen's pump as an `Event`; deciding what it means stays the
// pump's.
//
// Past the budget it says so, once: a read or the handshake is
// `Link::kNotResponding`, as before (#209), and a press is *still applying* --
// the write is out and will land, and a screen that called it a failure would
// drop an edit that took. A press still out after `kBudgetsBeforeStuck`
// budgets is *not responding* after all; its answer is still used if it comes.
//
// It names no libultrahand type, which `overlay.portable` holds it to.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "ipc_client.hpp"
#include "lifecycle.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

class Poller {
 public:
  /// Frames between two reads: four a second at 60 Hz. Faster than anything
  /// on these screens changes in a way a person reads, and slow enough that a
  /// busy sysmodule is asked a quarter as often as one per frame asked it.
  static constexpr int kFramesBetweenReads = 15;

  /// How many budgets a press may be out before *still applying* becomes
  /// *not responding*: ten seconds at the device's half-second budget. A card
  /// busy with a download answers in hundreds of milliseconds; a press out
  /// this long is a sysmodule that is stuck, and the screen cannot re-read
  /// anything behind it to find out otherwise.
  static constexpr int kBudgetsBeforeStuck = 20;

  /// What one frame produced.
  struct Event {
    enum class Kind {
      kNone,           ///< nothing new: draw what was drawn
      kUnreachable,    ///< draw `link` instead; `frame.sysmodule_interface()` is filled in
      kRead,           ///< the read answered `rc`; on success `client.Decode` reads it
      kSent,           ///< `command`, from `Send`, answered `rc`; the same
      kStillApplying,  ///< `command` is out past the budget, or its answer was lost
    };
    Kind kind = Kind::kNone;
    Link link = Link::kOk;
    ipc::Command command = ipc::Command::kGetInterfaceVersion;
    Result rc = 0;
  };

  /// `read` is the command asked on the cadence, with an empty request.
  Poller(ScreenFrame& frame, IpcClient& client, ipc::Command read,
         int frames_between_reads = kFramesBetweenReads);

  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  /// One frame: collect what came back, or send what is due. Never waits on
  /// the sysmodule. Opening the port goes to `sm`, as `ScreenFrame::Ready`
  /// always has, and only on the cadence.
  Event Frame();

  /// Send `command` once the line is free, ahead of the next read. False while
  /// another is waiting to go or still out: one press at a time.
  bool Send(ipc::Command command, std::string_view request);

  /// A `Send` is waiting to go or still out.
  bool sending() const { return queued_ || flight_ == Flight::kSend; }

 private:
  enum class Flight { kNone, kHandshake, kRead, kSend };

  using Clock = std::chrono::steady_clock;

  /// Hand `command` to the client. Its refusals are answers, at once.
  Event Post(Flight flight, ipc::Command command, std::string_view request);

  /// What an answer to `flight` means.
  Event Answer(Flight flight, ipc::Command command, Result rc);

  /// The call is still out: say so once it has been out for the budget.
  Event Pending();

  static Event Unreachable(Link link) {
    Event event;
    event.kind = Event::Kind::kUnreachable;
    event.link = link;
    return event;
  }

  ScreenFrame& frame_;
  IpcClient& client_;
  const ipc::Command read_;
  Cadence cadence_;

  /// What is out, under which ticket, since when, and in which showing.
  Flight flight_ = Flight::kNone;
  ipc::Command command_ = ipc::Command::kGetInterfaceVersion;
  IpcClient::Ticket ticket_ = 0;
  Clock::time_point since_{};
  std::uint64_t showing_ = 0;
  /// The budget has passed and that has been reported.
  bool late_ = false;
  /// A press out for `kBudgetsBeforeStuck` budgets, reported as not responding.
  bool given_up_ = false;

  /// The line is held by a call that is not this poller's -- one a screen above
  /// it gave up on -- and since when.
  bool held_ = false;
  Clock::time_point held_since_{};

  /// The `Send` waiting to go.
  bool queued_ = false;
  ipc::Command queued_command_ = ipc::Command::kGetInterfaceVersion;
  std::string queued_request_;
};

}  // namespace rommsync::overlay
