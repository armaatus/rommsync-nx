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

#include <charconv>
#include <cstddef>
#include <cstdint>
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
///
/// **Which session closed is read from `index`, not from `reply_pending`.**
/// Mesosphere's `ReplyAndReceiveImpl` sends the reply and *then* waits. A reply
/// that fails to send returns at once with `index` set to -1; a session that
/// closes during the wait returns ConnectionClosed with that session's index --
/// and the session just answered is healthy. Reading "a reply was pending" as
/// "the reply is what failed" dropped it and left the closed one in the table.
///
/// Index 0 is the port, and a port does not close: dropping it would take the
/// service down and leave the process running, the worst shape this has.
constexpr WaitOutcome DecideWait(std::uint32_t rc, std::int32_t index, bool reply_pending,
                                 std::int32_t handle_count) {
  if (rc != 0) {
    if (rc != kKernelConnectionClosed) return {WaitAction::kRecover, index};
    if (index == -1 && reply_pending) return {WaitAction::kDropReplyTarget, index};
    if (index >= 1 && index < handle_count) return {WaitAction::kDrop, index};
    return {WaitAction::kNothing, index};
  }
  if (index == 0) return {WaitAction::kAccept, index};
  if (index < 1 || index >= handle_count) return {WaitAction::kNothing, index};
  return {WaitAction::kHandle, index};
}

/// How many unexpected failures in a row the loop takes before it stops.
///
/// Not "never": a failure that recurs on every pass with nothing working in
/// between -- the port handle itself gone bad -- would otherwise be a process
/// spinning at full speed for the life of the console. Eight passes with the
/// waits below is about three and a half seconds, long enough for anything
/// transient to clear and short enough that the log line is near the cause.
inline constexpr int kMaxConsecutiveIpcErrors = 8;

/// The run of unexpected failures the loop is in. Any pass that is not one
/// clears it.
class IpcErrorRun {
 public:
  /// Count one more failure. True when that one is the last the loop takes.
  constexpr bool Fail() { return ++count_ >= kMaxConsecutiveIpcErrors; }
  constexpr void Clear() { count_ = 0; }
  constexpr int count() const { return count_; }

  /// How long to wait before the next pass, after `count()` failures.
  /// 50 ms, doubling, capped at a second.
  constexpr std::uint64_t BackoffNs() const {
    if (count_ <= 0) return 0;
    const int doublings = count_ - 1 < 5 ? count_ - 1 : 5;
    const std::uint64_t wait = 50'000'000ull << doublings;
    return wait < 1'000'000'000ull ? wait : 1'000'000'000ull;
  }

 private:
  int count_ = 0;
};

/// `rc` the way Atmosphère's crash reports and every Switch forum spell it --
/// `2001-0123` -- with the raw value beside it, for a line a user pastes.
///
/// `std::to_chars` rather than `snprintf`: the latter links newlib's
/// `_svfprintf_r` into the sysmodule, which `switch.builds` refuses.
inline std::string DescribeResult(std::uint32_t rc) {
  const auto padded = [](std::uint32_t value, std::size_t width) {
    char digits[16];
    const char* end = std::to_chars(digits, digits + sizeof digits, value).ptr;
    std::string text(static_cast<const char*>(digits), end);
    if (text.size() < width) text.insert(0, width - text.size(), '0');
    return text;
  };
  char hex[16];
  const char* hex_end = std::to_chars(hex, hex + sizeof hex, rc, 16).ptr;
  return padded(2000u + (rc & 0x1ffu), 4) + "-" + padded((rc >> 9) & 0x1fffu, 4) + " (0x" +
         std::string(static_cast<const char*>(hex), hex_end) + ")";
}

}  // namespace rommsync::sysmodule
