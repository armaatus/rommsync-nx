// The transport half of the console's `http::HttpClient`: bsd sockets and the
// Horizon `ssl` system service.
//
// This is the file that cannot be executed anywhere before the M8-1 gate (#43),
// and it is as small as it could be made for that reason -- everything that
// does not need a console lives in `http_wire.hpp` and is driven against the
// real RomM by `wire.*`. What is left here is opening a socket, handing it to
// `ssl`, and reading and writing through the connection that comes back
// (docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision).
//
// It includes `<switch.h>` and is devkitPro's alone (sysmodule/AGENTS.md).
#pragma once

#include <switch.h>

#include <cstddef>
#include <memory>

#include "rommsync/http.hpp"

namespace rommsync::sysmodule {

/// The bsd transfer-memory budget, which `socketInitialize` takes out of *this
/// process's* heap and which is the dominant term in it.
///
/// Not libnx's defaults: `socketInitializeDefault()` asks for 0x234000 --
/// 2.25 MiB -- which does not fit in `kInnerHeapSize` and never will. **That
/// single call is the difference between a working engine and one that dies at
/// `socketInitialize`**, which is why the config is spelled out here rather than
/// defaulted.
///
/// The TCP numbers are the M0-1 probe's (`tlsprobe/source/probe.hpp`); the UDP
/// ones are **not**, and the difference is deliberate rather than a typo. The
/// probe kept libnx's default 0xA500 receive buffer, which costs 39 KiB of a
/// 512 KiB heap for traffic this process never sends: name resolution on Horizon
/// goes through the `sfdnsres` *service*, not through a UDP socket of ours, so
/// what is left is the DNS a `getaddrinfo` fallback would do -- 9 KiB is several
/// times the largest answer either can produce.
///
/// That is also what makes the arithmetic come out at the 0x1D000 -- 116 KiB --
/// docs/DEVELOPMENT.md's heap budget is written against, and which
/// `state::kMaxStateBytes` is in turn sized against. The probe's own defaults
/// compute to 0x25000 (148 KiB); the doc quoted the narrower figure, and this
/// is the config that actually produces it (see the note in that section).
struct SocketBudget {
  u32 tcp_tx_buf_size = 0x2000;
  u32 tcp_rx_buf_size = 0x4000;
  u32 tcp_tx_buf_max_size = 0x8000;
  u32 tcp_rx_buf_max_size = 0x10000;
  u32 udp_tx_buf_size = 0x2400;
  u32 udp_rx_buf_size = 0x2400;
  u32 sb_efficiency = 1;

  /// Two, to match `kSslSessions`. libnx hands out a bsd session per socket so
  /// that blocking socket commands from different threads can overlap; with one,
  /// the download worker's and the sync engine's socket calls serialise on it,
  /// which is the opposite of what sharing one client is supposed to allow. It
  /// costs two service sessions and **nothing on this heap**: the transfer
  /// memory below does not depend on it.
  u32 num_bsd_sessions = 2;
};

/// libnx's own sizing rule (`_bsdGetTransferMemSizeForConfig`), reproduced so
/// the heap arithmetic in `main.cpp` is a number this build computes rather than
/// one a comment claims. With the budget above it is 0x1D000 -- 116 KiB.
constexpr std::size_t ExpectedBsdTransferMemory(const SocketBudget& budget) {
  const u32 tx =
      budget.tcp_tx_buf_max_size != 0 ? budget.tcp_tx_buf_max_size : budget.tcp_tx_buf_size;
  const u32 rx =
      budget.tcp_rx_buf_max_size != 0 ? budget.tcp_rx_buf_max_size : budget.tcp_rx_buf_size;
  const u32 sum = (tx + rx + budget.udp_tx_buf_size + budget.udp_rx_buf_size + 0xFFF) & ~0xFFFu;
  return static_cast<std::size_t>(budget.sb_efficiency) * sum;
}

/// How many `ssl` sessions the process opens. Two, so a download and an API call
/// can be in flight at once -- the download worker and the sync engine share one
/// client by design (`http.hpp`). Sessions are handles in the `ssl` sysmodule's
/// process, not buffers in ours: libnx's ssl client allocates nothing
/// (docs/DEVELOPMENT.md).
inline constexpr u32 kSslSessions = 2;

/// Bring up the socket driver and the `ssl` service.
///
/// **Call it from `__appInit`, while `sm` is still open.** Both are `sm`
/// lookups, and a sysmodule closes `sm` before it starts serving -- a later
/// attempt would fail with nothing able to explain why.
///
/// A failure is not fatal to the process and must not be treated as one: a
/// console with no network is a console the overlay still has to be able to
/// open, read its settings on, and see its queue. `NetworkReady()` is false and
/// every request answers `Error::kConnectFailed` (`sysmodule/README.md`).
Result NetworkInitialize(const SocketBudget& budget = {});

/// Undo `NetworkInitialize`. Safe when it failed or was never called.
void NetworkExit();

/// Whether `NetworkInitialize` succeeded, so `main` can say so in the log rather
/// than leaving a silently transportless console.
bool NetworkReady();

/// The console's `http::HttpClient`: `http_wire.cpp`'s framing over the `ssl`
/// transport. Never null -- a build whose network did not come up still answers,
/// with `Error::kConnectFailed`, which is the offline answer every caller
/// already handles rather than a null pointer every caller would have to check.
std::unique_ptr<http::HttpClient> MakeHorizonHttpClient(const http::ClientOptions& options = {});

}  // namespace rommsync::sysmodule
