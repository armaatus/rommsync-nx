// What one return from `svcReplyAndReceive` asks the service loop to do
// (M11-7, #291).
//
// Apart from `server.cpp`, which is the only caller, because that file is libnx
// through and through and this decision is the part worth a host test: a client
// hanging up during the wait used to drop the session that had just been
// answered, and any kernel error nobody expected ended the process without a
// log line. Names no libnx type -- the result is the plain integer libnx's
// `Result` is, and an index is the `s32` the kernel writes.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace rommsync::sysmodule {

/// `KERNELRESULT(ConnectionClosed)`: module 1, description 123.
inline constexpr std::uint32_t kKernelConnectionClosed = 0xf601;

enum class WaitAction {
  /// The port signalled: a client is connecting.
  kAccept,
  /// The session at `index` sent a request.
  kHandle,
  /// The pending reply could not be sent: its session went away first.
  kDropReplyTarget,
  /// The session at `index` closed while the loop waited.
  kDrop,
  /// Nothing to do, and nothing wrong: an index that names no session.
  kNothing,
  /// A failure this loop has no case for. Logged, the sessions dropped, and
  /// the loop carries on (`IpcErrorRun` says for how long).
  kRecover,
};

struct WaitOutcome {
  WaitAction action;
  std::int32_t index;
};

/// `rc` and `index` are what `svcReplyAndReceive` returned and wrote;
/// `reply_pending` is whether a reply target was passed in; `handle_count` is
/// the table's size, port included.
constexpr WaitOutcome DecideWait(std::uint32_t rc, std::int32_t index, bool reply_pending,
                                 std::int32_t handle_count) {
  if (rc != 0) {
    if (rc != kKernelConnectionClosed) return {WaitAction::kRecover, index};
    if (reply_pending) return {WaitAction::kDropReplyTarget, index};
    if (index >= 1 && index < handle_count) return {WaitAction::kDrop, index};
    return {WaitAction::kNothing, index};
  }
  if (index == 0) return {WaitAction::kAccept, index};
  if (index < 1 || index >= handle_count) return {WaitAction::kNothing, index};
  return {WaitAction::kHandle, index};
}

/// How many unexpected failures in a row the loop survives before it stops.
inline constexpr int kMaxConsecutiveIpcErrors = 1;

/// The run of unexpected failures the loop is in. Any pass that is not one
/// clears it.
class IpcErrorRun {
 public:
  /// Count one more failure. True when that one is the last the loop takes.
  constexpr bool Fail() { return ++count_ >= kMaxConsecutiveIpcErrors; }
  constexpr void Clear() { count_ = 0; }
  constexpr int count() const { return count_; }

  /// How long to wait before the next pass, after `count()` failures.
  constexpr std::uint64_t BackoffNs() const { return 0; }

 private:
  int count_ = 0;
};

/// `rc` the way Atmosphère's crash reports and every Switch forum spell it --
/// `2001-0123` -- with the raw value beside it, for a line a user pastes.
inline std::string DescribeResult(std::uint32_t rc) {
  char text[32];
  std::snprintf(text, sizeof text, "%04u-%04u (0x%x)", 2000u + (rc & 0x1ffu),
                (rc >> 9) & 0x1fffu, rc);
  return text;
}

}  // namespace rommsync::sysmodule
