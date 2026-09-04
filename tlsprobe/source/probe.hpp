// What the probe is asked to do, and what it found out.
//
// Deliberately allocation-free: every buffer here is fixed and lives in .bss or
// on the stack. The whole point of this target is to measure what the TLS path
// costs a sysmodule heap, so the measuring apparatus must not be on that heap
// (tlsprobe/README.md).
#pragma once

#include <switch.h>

#include <cstddef>
#include <cstdint>

namespace tlsprobe {

// Long enough for a hostname, an SNI name and an SD-card path, short enough to
// keep the whole config in .bss.
inline constexpr size_t kHostMax = 128;
inline constexpr size_t kPathMax = 256;

/// The bsd transfer memory sizing knobs, exposed because they *are* the heap
/// answer: libnx allocates that transfer memory out of this process's heap, so
/// what is set here is most of what a sysmodule would have to budget for
/// (docs/DEVELOPMENT.md#tls-in-a-sysmodule). Anyone re-measuring an alternative
/// budget should be able to do it by editing the ini, not the source.
struct SocketBudget {
  u32 tcp_tx_buf_size = 0x2000;
  u32 tcp_rx_buf_size = 0x4000;
  u32 tcp_tx_buf_max_size = 0x8000;
  u32 tcp_rx_buf_max_size = 0x10000;
  u32 udp_tx_buf_size = 0x2400;
  u32 udp_rx_buf_size = 0xA500;
  u32 sb_efficiency = 1;
  u32 num_bsd_sessions = 1;
};

/// libnx's own sizing rule, reproduced so the probe can print the number it is
/// about to spend before it spends it -- and so a run on hardware can be
/// compared against a prediction rather than against nothing.
/// Mirrors _bsdGetTransferMemSizeForConfig() in libnx's source/services/bsd.c.
constexpr size_t ExpectedBsdTransferMemory(const SocketBudget& b) {
  const u32 tx = b.tcp_tx_buf_max_size != 0 ? b.tcp_tx_buf_max_size : b.tcp_tx_buf_size;
  const u32 rx = b.tcp_rx_buf_max_size != 0 ? b.tcp_rx_buf_max_size : b.tcp_rx_buf_size;
  const u32 sum = (tx + rx + b.udp_tx_buf_size + b.udp_rx_buf_size + 0xFFF) & ~0xFFFu;
  return static_cast<size_t>(b.sb_efficiency) * sum;
}

struct Config {
  char host[kHostMax] = {};      ///< what to connect to; may be a name or an address
  char sni[kHostMax] = {};       ///< SNI + certificate hostname; defaults to `host`
  char path[kPathMax] = {};      ///< request target, e.g. /openapi.json
  char ca_path[kPathMax] = {};   ///< optional PEM to hand ssl via ImportServerPki
  u16 port = 443;

  /// Verify the server certificate. False is the deliberately-self-signed case
  /// and needs SkipDefaultVerify on [5.0.0+] (docs/SECURITY.md); it exists here
  /// because a fixture cert nothing trusts is exactly what the docker rig has.
  bool verify = true;

  /// One in-flight buffer, which is the sysmodule's download-streaming buffer
  /// under a different name. Reported so a measurement says which size it is of.
  u32 read_buf_size = 4096;

  u32 connect_timeout_ms = 10'000;
  u32 stall_timeout_ms = 20'000;

  SocketBudget socket;
};

/// One heap observation. Two numbers because they answer different questions:
/// `malloc_in_use` is what our allocations cost, `used_memory` is what the
/// kernel says the process holds -- and the bsd transfer memory shows up in
/// both, which is how a run confirms where it came from.
struct HeapSample {
  size_t malloc_in_use = 0;
  u64 used_memory = 0;
};

/// The stages worth a heap number of their own. Ordered as they happen.
enum class Stage {
  kBaseline,     ///< before any network service is touched
  kSocket,       ///< socketInitialize(): the bsd transfer memory lands here
  kSslContext,   ///< sslInitialize + sslCreateContext + optional ImportServerPki
  kConnected,    ///< TCP connect + SslConnection created
  kHandshake,    ///< after DoHandshake
  kPeak,         ///< high-water mark seen while reading the body
  kTeardown,     ///< everything closed again: baseline + leaks
  kCount,
};

inline constexpr size_t kStageCount = static_cast<size_t>(Stage::kCount);

const char* StageName(Stage stage);

struct Report {
  HeapSample stage[kStageCount];

  bool ok = false;
  Result rc = 0;               ///< the Horizon Result that ended a failed run
  const char* failed_at = "";  ///< the call that produced `rc`. Never null.

  /// Why the peer's certificate was refused, when the handshake is what failed.
  /// Separate from `rc` because "the handshake returned an error" and "the cert
  /// was rejected, for this reason" are two different findings, and the second
  /// one is the whole difference between a self-signed fixture and a broken
  /// TLS path.
  Result verify_cert_rc = 0;

  /// What ended the read loop. A server that closes after `Connection: close`
  /// is the expected ending, and some firmware reports that as a failed Read
  /// rather than a zero-length one -- so this is recorded, not treated as a
  /// failure on its own.
  Result read_end_rc = 0;

  int status = 0;              ///< HTTP status from the response line
  u64 header_bytes = 0;
  u64 body_bytes = 0;

  u64 connect_ms = 0;
  u64 handshake_ms = 0;
  u64 total_ms = 0;

  size_t expected_bsd_tmem = 0;

  char cipher[0x40] = {};            ///< from GetCipherInfo [4.0.0+], if it answers
  char protocol_version[0x8] = {};
  char ip[32] = {};                  ///< the console's own address, for the log
  bool ca_imported = false;
};

/// Sample the heap. Cheap enough to call in a read loop.
HeapSample SampleHeap();

/// Run the whole experiment once. Never throws (this target is built
/// -fno-exceptions) and never longer than the configured timeouts.
void RunProbe(const Config& config, Report& report);

}  // namespace tlsprobe
