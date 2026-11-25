// What one answer from `nifmGetInternetConnectionStatus` means (M11-5, #289;
// #328).
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
  /// No network at all -- airplane mode, or none in range -- said the way libnx
  /// documents. **The one answer nothing is ever sent past**: a console in a bag
  /// spends nothing (#289).
  kOffline,
  /// A network, but nifm will not say `Connected` (#328). nifm's internet status
  /// is the verdict of Nintendo's own connection test, so this is also the
  /// steady state of a console that blocks Nintendo on purpose -- emuMMC's DNS
  /// block, 90DNS, a Pi-hole, a LAN with RomM and no internet -- whose RomM is a
  /// request away. Read as offline only until the engine has asked RomM itself
  /// (`SdEngine::RommAnswers`); still associating after a wake reads the same,
  /// which is why that ask waits out the network budget first.
  kNoInternet,
  /// A failure code nobody documented. The one worth a log line, so a firmware
  /// that answers something new shows up; otherwise read as `kNoInternet`,
  /// because the only answer known to mean "no network" is `kNifmNoNetwork`.
  kUnexpected,
};

/// `rc` is the call's result, zero for success; `connected` is whether the
/// status it filled in was `NifmInternetConnectionStatus_Connected`, and is not
/// read when the call failed.
constexpr NifmVerdict ClassifyNifm(std::uint32_t rc, bool connected) {
  if (rc == kNifmNoNetwork) return NifmVerdict::kOffline;
  if (rc != 0) return NifmVerdict::kUnexpected;
  return connected ? NifmVerdict::kOnline : NifmVerdict::kNoInternet;
}

}  // namespace rommsync::sysmodule
