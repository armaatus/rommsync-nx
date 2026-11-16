#include "poller.hpp"

#include <cstdint>
#include <string_view>

#include "ipc_client.hpp"
#include "lifecycle.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

Poller::Poller(ScreenFrame& frame, IpcClient& client, ipc::Command read, int frames_between_reads)
    : frame_(frame), client_(client), read_(read), cadence_(frames_between_reads) {}

bool Poller::Send(ipc::Command command, std::string_view request) {
  if (sending()) {
    return false;
  }
  queued_ = true;
  queued_command_ = command;
  queued_request_.assign(request.data(), request.size());
  // Sent on the next frame rather than from the input handler: a press is no
  // place to find the line busy.
  return true;
}

Poller::Event Poller::Frame() {
  if (flight_ != Flight::kNone) {
    Result rc = 0;
    switch (client_.Collect(ticket_, &rc)) {
      case IpcClient::Arrival::kPending:
        return Pending();
      case IpcClient::Arrival::kLost: {
        // Another screen's call, or a reopen, took the line while this one was
        // out. A read is simply asked again; a press may or may not have
        // landed, and the read that follows is what says which.
        const Flight lost = flight_;
        flight_ = Flight::kNone;
        cadence_.Expire();
        if (lost == Flight::kSend) {
          Event event;
          event.kind = Event::Kind::kStillApplying;
          event.command = command_;
          return event;
        }
        return Event{};
      }
      case IpcClient::Arrival::kArrived: {
        const Flight arrived = flight_;
        flight_ = Flight::kNone;
        const Event event = Answer(arrived, command_, rc);
        if (event.kind != Event::Kind::kNone) {
          return event;
        }
        // A handshake that passed, or a stale read: the next command goes out
        // on this frame rather than the one after.
        break;
      }
    }
  }

  // Nothing is out. Ask only when something is owed: a press, a handshake on a
  // session that has none, or a read the cadence says is due. Opening the port
  // is in the last of those, so a sysmodule that is not running costs `sm` four
  // questions a second rather than sixty.
  const bool handshake_owed = client_.open() && !frame_.checked();
  if (!queued_ && !handshake_owed && !cadence_.Due()) {
    return Event{};
  }
  const Link link = frame_.Connect();
  if (link != Link::kOk) {
    // A press on a screen that has lost its sysmodule is not sent to the next
    // one that appears. Nor is a line that was held: there is no line.
    queued_ = false;
    held_ = false;
    return Unreachable(link);
  }
  if (!frame_.checked()) {
    return Post(Flight::kHandshake, ipc::Command::kGetInterfaceVersion, ipc::EncodeEmpty());
  }
  if (queued_) {
    return Post(Flight::kSend, queued_command_, queued_request_);
  }
  return Post(Flight::kRead, read_, ipc::EncodeEmpty());
}

Poller::Event Poller::Post(Flight flight, ipc::Command command, std::string_view request) {
  IpcClient::Ticket ticket = 0;
  const Result rc = client_.Post(command, request, &ticket);
  const Clock::time_point now = Clock::now();
  if (rc == NotResponding()) {
    // The line is held by a call this poller did not send: one a screen above
    // it gave up on, still out. Tried again every frame until it clears, and
    // reported as it would have been if it were this poller's own.
    if (!held_) {
      held_ = true;
      held_since_ = now;
    }
    cadence_.Expire();
    return now - held_since_ >= client_.budget() ? Unreachable(Link::kNotResponding) : Event{};
  }
  held_ = false;
  if (flight == Flight::kSend) {
    queued_ = false;
  }
  if (R_FAILED(rc)) {
    // Refused before it was sent -- no session -- which is an answer.
    return Answer(flight, command, rc);
  }
  flight_ = flight;
  command_ = command;
  ticket_ = ticket;
  since_ = now;
  showing_ = Showings();
  late_ = false;
  given_up_ = false;
  return Event{};
}

Poller::Event Poller::Answer(Flight flight, ipc::Command command, Result rc) {
  switch (flight) {
    case Flight::kNone:
      return Event{};
    case Flight::kHandshake: {
      std::uint32_t interface = 0;
      Result decoded = rc;
      if (R_SUCCEEDED(decoded)) {
        decoded = client_.Decode(ipc::DecodeInterfaceVersion, &interface);
      }
      const Link link = frame_.Admit(decoded, interface);
      if (link != Link::kOk) {
        return Unreachable(link);
      }
      // Read at once: the screen has nothing to draw until it has.
      cadence_.Expire();
      return Event{};
    }
    case Flight::kRead: {
      if (R_SUCCEEDED(rc) && showing_ != Showings()) {
        // Answered while the overlay was hidden, and so describes the console
        // as it was then. Asked again rather than drawn (`lifecycle.hpp`).
        cadence_.Expire();
        return Event{};
      }
      Event event;
      event.kind = Event::Kind::kRead;
      event.command = command;
      event.rc = rc;
      return event;
    }
    case Flight::kSend: {
      // Whatever a press answered, the next read says what it changed.
      cadence_.Expire();
      Event event;
      event.kind = Event::Kind::kSent;
      event.command = command;
      event.rc = rc;
      return event;
    }
  }
  return Event{};
}

Poller::Event Poller::Pending() {
  const Clock::duration out = Clock::now() - since_;
  if (flight_ == Flight::kSend && late_ && !given_up_ &&
      out >= client_.budget() * kBudgetsBeforeStuck) {
    // Busy does not last this long. The line is the write's until it answers,
    // so nothing can be re-read behind it, and "still applying" would stand
    // over a stuck sysmodule for as long as it stays stuck.
    given_up_ = true;
    return Unreachable(Link::kNotResponding);
  }
  if (late_ || out < client_.budget()) {
    return Event{};
  }
  late_ = true;
  if (flight_ == Flight::kSend) {
    Event event;
    event.kind = Event::Kind::kStillApplying;
    event.command = command_;
    return event;
  }
  return Unreachable(Link::kNotResponding);
}

}  // namespace rommsync::overlay
