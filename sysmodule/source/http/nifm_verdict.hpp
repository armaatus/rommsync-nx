// What one answer from `nifmGetInternetConnectionStatus` means (M11-5, #289).
//
// Apart from `ssl_http_client.cpp`, which is the only caller, because that file
// is libnx through and through and this decision is the part worth a host test:
// a console in airplane mode was read as online for as long as the call's
// failure meant "nifm was busy". Names no libnx type -- the result code is the
// plain integer libnx's `Result` is.
#pragma once

#include <cstdint>

namespace rommsync::sysmodule {

/// libnx `nifm.h`'s documented failure for airplane mode and for no known
/// network in range: the two offline consoles there are.
inline constexpr std::uint32_t kNifmNoNetwork = 0xd46ed;

enum class NifmVerdict {
  kOnline,
  /// The console is offline, and said so the way libnx documents.
  kOffline,
  /// Offline as well, but reached from a failure code nobody documented -- the
  /// one worth a log line, so a firmware that answers something new shows up
  /// instead of reading as a console that silently never syncs.
  kOfflineUnexpected,
};

/// `rc` is the call's result, zero for success; `connected` is whether the
/// status it filled in was `NifmInternetConnectionStatus_Connected`, and is not
/// read when the call failed.
///
/// **Any failure is offline.** There is no failure that means "ask anyway":
/// the transport would only find out the same thing a connect timeout later.
constexpr NifmVerdict ClassifyNifm(std::uint32_t rc, bool connected) {
  if (rc == kNifmNoNetwork) return NifmVerdict::kOffline;
  if (rc != 0) return NifmVerdict::kOfflineUnexpected;
  return connected ? NifmVerdict::kOnline : NifmVerdict::kOffline;
}

}  // namespace rommsync::sysmodule
