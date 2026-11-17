// What the service loop does with each return from `svcReplyAndReceive`
// (M11-7, #291).
//
// `server.cpp` is libnx and no test reaches it before the M8-1 gate; the
// decision it makes is `ipc/wait_outcome.hpp`, which names no libnx type, and
// this is that decision driven with the result/index pairs the kernel produces.
//
// The pairs are mesosphere's `ReplyAndReceiveImpl`: it sends the pending reply
// first and only then waits. A reply that cannot be sent returns its failure
// with the index set to -1; a session that closes *during the wait* returns
// ConnectionClosed with that session's index -- even when a reply had just gone
// out to a different, healthy session. The second is what the loop used to get
// wrong: it dropped the healthy one.
//
// Pure arithmetic: no rig, so it never skips.
#include <cstdint>
#include <iostream>
#include <string>

#include "checks.hpp"
#include "ipc/wait_outcome.hpp"

namespace sys = rommsync::sysmodule;
using sys::WaitAction;

namespace {

constexpr std::uint32_t kOk = 0;
constexpr std::uint32_t kClosed = sys::kKernelConnectionClosed;
/// `KERNELRESULT(InvalidHandle)` (module 1, description 114): the shape of
/// failure the loop has no case for.
constexpr std::uint32_t kInvalidHandle = 0xe401;
/// `KERNELRESULT(OutOfResource)` (module 1, description 103).
constexpr std::uint32_t kOutOfResource = 0xce01;

/// A table of the port plus three sessions.
constexpr std::int32_t kCount = 4;

const char* Name(WaitAction action) {
  switch (action) {
    case WaitAction::kAccept:
      return "accept";
    case WaitAction::kHandle:
      return "handle";
    case WaitAction::kDropReplyTarget:
      return "drop the reply target";
    case WaitAction::kDrop:
      return "drop the index";
    case WaitAction::kNothing:
      return "nothing";
    case WaitAction::kRecover:
      return "recover";
  }
  return "?";
}

void ExpectAction(checks::Checks& c, sys::WaitOutcome outcome, WaitAction expected,
                  const std::string& what) {
  if (outcome.action != expected) {
    c.Expect(false, what + " -- expected " + Name(expected) + ", got " + Name(outcome.action));
  }
}

int Closed() {
  checks::Checks c;

  // The reply could not be sent: the session it was for went away. Index -1 is
  // how the kernel says so, and it is the reply target that goes.
  ExpectAction(c, sys::DecideWait(kClosed, -1, /*reply_pending=*/true, kCount),
               WaitAction::kDropReplyTarget, "a reply that failed to send drops its target");

  // The reply went out, and then a *different* session closed during the wait.
  // That session is the one to drop; the one just answered is healthy.
  for (std::int32_t index = 1; index < kCount; ++index) {
    const sys::WaitOutcome outcome = sys::DecideWait(kClosed, index, /*reply_pending=*/true, kCount);
    ExpectAction(c, outcome, WaitAction::kDrop,
                 "a session closing after a reply went out is the one dropped (index " +
                     std::to_string(index) + ")");
    c.ExpectEq(outcome.index, index, "and it is that index that is dropped");
  }

  // No reply pending: the index names the session that closed, as before.
  for (std::int32_t index = 1; index < kCount; ++index) {
    const sys::WaitOutcome outcome =
        sys::DecideWait(kClosed, index, /*reply_pending=*/false, kCount);
    ExpectAction(c, outcome, WaitAction::kDrop,
                 "a session closing with no reply pending is dropped (index " +
                     std::to_string(index) + ")");
    c.ExpectEq(outcome.index, index, "and it is that index that is dropped");
  }

  // Index 0 is the port, and a port does not close: dropping it would take the
  // service down and leave the process running. An index past the table, or a
  // -1 with no reply to blame, names nothing.
  for (const bool pending : {false, true}) {
    const std::string suffix = pending ? " (reply pending)" : " (no reply pending)";
    ExpectAction(c, sys::DecideWait(kClosed, 0, pending, kCount), WaitAction::kNothing,
                 "a ConnectionClosed at the port's index drops nothing" + suffix);
    ExpectAction(c, sys::DecideWait(kClosed, kCount, pending, kCount), WaitAction::kNothing,
                 "an index past the table drops nothing" + suffix);
  }
  ExpectAction(c, sys::DecideWait(kClosed, -1, /*reply_pending=*/false, kCount),
               WaitAction::kNothing, "a -1 with no reply pending drops nothing");

  return c.failures() == 0 ? 0 : 1;
}

int Received() {
  checks::Checks c;
  for (const bool pending : {false, true}) {
    ExpectAction(c, sys::DecideWait(kOk, 0, pending, kCount), WaitAction::kAccept,
                 "the port signalling is a client connecting");
    for (std::int32_t index = 1; index < kCount; ++index) {
      ExpectAction(c, sys::DecideWait(kOk, index, pending, kCount), WaitAction::kHandle,
                   "a session signalling is a request to handle");
    }
    ExpectAction(c, sys::DecideWait(kOk, kCount, pending, kCount), WaitAction::kNothing,
                 "an index past the table is handled as nothing");
    ExpectAction(c, sys::DecideWait(kOk, -1, pending, kCount), WaitAction::kNothing,
                 "a negative index is handled as nothing");
  }
  return c.failures() == 0 ? 0 : 1;
}

int Error() {
  checks::Checks c;

  // Any other failure is survived, whatever the index and whether or not a
  // reply was pending: the loop logs it, drops its sessions and carries on.
  for (const std::uint32_t rc : {kInvalidHandle, kOutOfResource}) {
    for (const std::int32_t index : {-1, 0, 1, 3}) {
      for (const bool pending : {false, true}) {
        ExpectAction(c, sys::DecideWait(rc, index, pending, kCount), WaitAction::kRecover,
                     "an unexpected result is recovered from, not returned: " +
                         sys::DescribeResult(rc));
      }
    }
  }

  // ...but not forever. A run of them, with nothing succeeding in between, is a
  // loop that cannot work, and spinning on it is worse than stopping with a
  // line in the log. Survived several times first, though: one is not a run.
  c.Expect(sys::kMaxConsecutiveIpcErrors >= 3,
           "more than one or two unexpected failures in a row are survived");
  sys::IpcErrorRun run;
  for (int i = 1; i < sys::kMaxConsecutiveIpcErrors; ++i) {
    c.Expect(!run.Fail(), "failure " + std::to_string(i) + " of a run is survived");
    c.Expect(run.BackoffNs() > 0, "and the next pass waits a little first");
  }
  c.Expect(run.Fail(), "the last failure of a run is the one the loop stops on");

  // A pass that works in between starts the count again: two failures an hour
  // apart are not a run.
  run.Clear();
  c.ExpectEq(run.count(), 0, "a working pass clears the run");
  c.Expect(!run.Fail(), "and the next failure is the first of a new one");

  // The wait grows, and stays bounded: a loop that recovers is back within a
  // second or so, not minutes later.
  sys::IpcErrorRun growing;
  std::uint64_t previous = 0;
  std::uint64_t total = 0;
  for (int i = 1; i < sys::kMaxConsecutiveIpcErrors; ++i) {
    growing.Fail();
    c.Expect(growing.BackoffNs() >= previous, "the wait never shrinks within a run");
    previous = growing.BackoffNs();
    total += previous;
  }
  c.Expect(previous <= 1'000'000'000ull, "no single wait is longer than a second");
  c.Expect(total <= 10'000'000'000ull, "a whole run gives up within ten seconds");

  // The result in the log line is the spelling a user searches for.
  c.ExpectEq(sys::DescribeResult(kInvalidHandle), std::string("2001-0114 (0xe401)"),
             "InvalidHandle is described as 2001-0114");
  c.ExpectEq(sys::DescribeResult(kClosed), std::string("2001-0123 (0xf601)"),
             "ConnectionClosed is described as 2001-0123");

  return c.failures() == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "";
  if (scenario == "closed") return Closed();
  if (scenario == "received") return Received();
  if (scenario == "error") return Error();
  std::cerr << "unknown scenario: " << scenario << "\n";
  return 2;
}
